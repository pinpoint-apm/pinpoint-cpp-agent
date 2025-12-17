#include <string>
#include <sstream>
#include <iostream>
#include <cstdlib>

#include <cpptrace/utils.hpp>
#include <cpptrace/cpptrace.hpp>
#include <cpptrace/formatting.hpp>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

// Thread local storage for span
thread_local pinpoint::SpanPtr current_span;

bool startsWith(const std::string& str, const std::string& prefix) {
    if (str.length() < prefix.length()) {
        return false;
    }
    return str.compare(0, prefix.length(), prefix) == 0;
}

class CppTraceCallStackReader : public pinpoint::CallStackReader {
public:
    CppTraceCallStackReader() = default;
    ~CppTraceCallStackReader() override = default;

    void ForEach(std::function<void(std::string_view module, std::string_view function, std::string_view file, int line)> callback) const override {
        auto stack_trace = cpptrace::generate_trace(2, 32);
        for (const auto& frame : stack_trace.frames) {
            auto symbol = cpptrace::prune_symbol(frame.symbol);
            if (symbol.empty() || startsWith(symbol, "std::")) {
                continue;
            }

            auto module = std::string("unknown");
            auto function = symbol;
            auto file = cpptrace::basename(frame.filename);
            auto line = frame.line.value_or(0);
            if (file.empty()) {
                file = "unknown";
                line = 0;
            }

            auto pos = symbol.rfind("::");
            if (pos != std::string::npos) {
                module = symbol.substr(0, pos);
                function = symbol.substr(pos + 2);
            } else {
                pos = file.find(".");
                if (pos != std::string::npos) {
                    module = file.substr(0, pos);
                }
            }

            callback(module, function, file, line);
        }
    }
};

// Helper functions for thread local span management
void set_span_context(pinpoint::SpanPtr span) { current_span = span; }
pinpoint::SpanPtr get_span_context() { return current_span; }

void handle_users(const httplib::Request& req, httplib::Response& res);
void handle_outgoing(const httplib::Request& req, httplib::Response& res);
pinpoint::SpanPtr trace_request(const httplib::Request& req);
void trace_response(const httplib::Request& req, httplib::Response& res, pinpoint::SpanPtr span);
httplib::Server::Handler wrap_handler(httplib::Server::Handler handler);

int main() {
    // Pinpoint configuration
    setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-cpptrace-demo", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);

    auto agent = pinpoint::CreateAgent();
    httplib::Server server;
    
    // Register /users/:id endpoint
    server.Get(R"(/users/(\d+))", wrap_handler(handle_users));
    // Register /outgoing endpoint
    server.Get("/outgoing", wrap_handler(handle_outgoing));
   
    std::cout << "Web demo server starting on http://localhost:8088" << std::endl;
    std::cout << "Try: http://localhost:8088/users/123?name=foo" << std::endl;
    std::cout << "Try: http://localhost:8088/outgoing?host=localhost:9000&path=/bar" << std::endl;
    
    server.listen("0.0.0.0", 8088);
    agent->Shutdown();
    
    return 0;
}

pinpoint::SpanPtr trace_request(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();

    HttpHeaderReader request_reader(req.headers);
    auto span = agent->NewSpan("C++ Cpptrace Demo", req.path, request_reader);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    pinpoint::helper::TraceHttpServerRequest(span, req.remote_addr, end_point, request_reader);

    return span;
}

void trace_response(const httplib::Request& req, httplib::Response& res, pinpoint::SpanPtr span) {
    HttpHeaderReader response_reader(res.headers);
    pinpoint::helper::TraceHttpServerResponse(span, req.matched_route, req.method, res.status, response_reader);
    span->EndSpan();
}

httplib::Server::Handler 
wrap_handler(httplib::Server::Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        auto span = trace_request(req);
        set_span_context(span);  // Store span in thread local storage
        
        handler(req, res);

        trace_response(req, res, span);        
        set_span_context(nullptr);  // Clear span from thread local storage
    };
}

void handle_users(const httplib::Request& req, httplib::Response& res) {
    auto span = get_span_context();  // Get span from thread local storage
    pinpoint::helper::ScopedSpanEvent scoped(span, "handle_users");

    // Extract ID from path parameter
    std::string user_id;
    if (req.matches.size() > 1) {
        user_id = req.matches[1];
    }
    
    // Extract name from query parameter
    std::string name = req.get_param_value("name");
    if (name.empty()) {
        name = "unknown";
    }
    
    // Extract client IP
    std::string client_ip = req.remote_addr;
    
    // Generate JSON response
    std::stringstream json_response;
    json_response << "{\n";
    json_response << "  \"id\": \"" << user_id << "\",\n";
    json_response << "  \"name\": \"" << name << "\",\n";
    json_response << "  \"client_ip\": \"" << client_ip << "\"\n";
    json_response << "}";
    
    // Set response
    res.set_content(json_response.str(), "application/json");
    res.status = 200;
    
    std::cout << "Request handled: " << req.matched_route 
              << " -> ID: " << user_id 
              << ", Name: " << name 
              << ", IP: " << client_ip << std::endl;
}

void handle_outgoing(const httplib::Request& req, httplib::Response& res) {
    auto span = get_span_context();  // Get span from thread local storage
    auto handler_scoped_span_event = pinpoint::helper::ScopedSpanEvent(span, "handle_outgoing");
    
    // Get target URL from query parameter or use default
    std::string host = req.get_param_value("host");
    if (host.empty()) {
        host = "localhost:9000";
    }
    std::string path = req.get_param_value("path");
    if (path.empty()) {
        path = "/bar";
    }
    std::string target_url = "http://" + host + path;
    std::cout << "Making outgoing request to: " << target_url << std::endl;

    httplib::Result external_res;
    {
        auto outgoing_span_event = pinpoint::helper::ScopedSpanEvent(span, "httplib::Client.Get");
        httplib::Headers headers;
        headers.emplace("User-Agent", "cpptrace-demo");
        headers.emplace("Accept", "application/json");  
        headers.emplace("Accept-Language", "en-US,en;q=0.5");

        // Create a writer adapter for your HTTP client headers
        HttpHeaderReaderWriter header_reader_writer(headers);
        pinpoint::helper::TraceHttpClientRequest(outgoing_span_event.value(), host, target_url, header_reader_writer);
        // Inject trace context into headers
        span->InjectContext(header_reader_writer);

        // Call external URL with HTTP client
        httplib::Client cli(host);
        cli.set_connection_timeout(5, 0);  // 5 seconds
        cli.set_read_timeout(5, 0);        // 5 seconds
        external_res = cli.Get(path, headers);

        // Create a reader adapter for your HTTP client response headers
        HttpHeaderReader response_reader(external_res->headers);
        pinpoint::helper::TraceHttpClientResponse(outgoing_span_event.value(), external_res->status, response_reader);
    }

    std::stringstream json_response;
    json_response << "{\n";
    json_response << "  \"target_url\": \"" << target_url << "\",\n";
    json_response << "  \"status_code\": " << external_res->status << ",\n";
    json_response << "  \"response_body\": \"" << external_res->body << "\"\n";
    json_response << "}";
    
    auto error = external_res.error();
    if (error != httplib::Error::Success) {
        std::string error_msg = httplib::to_string(error);
        
        std::string err_msg = "Outgoing call failed: " + error_msg + " - Unable to connect to " + host;
        std::cout << err_msg << std::endl;

        CppTraceCallStackReader reader;
        handler_scoped_span_event->SetError("HandleOutgoingError", err_msg, reader);
    }
        
    // Set response
    res.set_content(json_response.str(), "application/json");
    res.status = 200;
    
    std::cout << "Request handled: " << req.path 
              << " -> Called " << target_url << std::endl;
}
