#include <string>
#include <sstream>
#include <iostream>

#include <cpptrace/utils.hpp>
#include <cpptrace/cpptrace.hpp>
#include <cpptrace/formatting.hpp>

#include "pinpoint/tracer.h"
#include "3rd_party/httplib.h"
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
            if (startsWith(symbol, "std::")) {
                continue;
            }

            auto module = std::string("unknown");
            auto function = symbol;
            auto file = cpptrace::basename(frame.filename);

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

            callback(module, function, file, frame.line.value_or(0));
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
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-web-demo", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);

    auto agent = pinpoint::CreateAgent();
    httplib::Server server;
    
    // Register /users/:id endpoint
    server.Get(R"(/users/(\d+))", wrap_handler(handle_users));
    // Register /outgoing endpoint
    server.Get("/outgoing", wrap_handler(handle_outgoing));
   
    std::cout << "Web demo server starting on http://localhost:8088" << std::endl;
    std::cout << "Try: http://localhost:8088/users/123?name=foo" << std::endl;
    std::cout << "Try: http://localhost:8088/outgoing" << std::endl;
    
    server.listen("localhost", 8088);
    agent->Shutdown();
    
    return 0;
}

pinpoint::SpanPtr trace_request(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();

    HttpTraceContextReader trace_context_reader(req.headers);
    auto span = agent->NewSpan("C++ Web Demo", req.path, trace_context_reader);

    span->SetRemoteAddress(req.remote_addr);
    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    span->SetEndPoint(end_point);

    HttpHeaderReader http_reader(req.headers);
    span->RecordHeader(pinpoint::HTTP_REQUEST, http_reader);

    return span;
}

void trace_response(const httplib::Request& req, httplib::Response& res, pinpoint::SpanPtr span) {
    HttpHeaderReader http_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, http_reader);

    span->SetStatusCode(res.status);
    span->SetUrlStat(req.matched_route, req.method, res.status);
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
    span->NewSpanEvent("handle_users");

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
    span->EndSpanEvent();
}

void handle_outgoing(const httplib::Request& req, httplib::Response& res) {
    auto span = get_span_context();  // Get span from thread local storage
    std::string host = "localhost:9000", path = "/bar";
    std::string url = "http://" + host + path;

    auto se = span->NewSpanEvent("handle_outgoing");
    se->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
    se->SetEndPoint(host);
    se->SetDestination(host);

    auto anno = se->GetAnnotations();
    anno->AppendString(pinpoint::ANNOTATION_HTTP_URL, url);

    // Call external URL with HTTP client
    httplib::Client cli(host);
    std::stringstream json_response;
    json_response << "{\n";
    json_response << "  \"target_url\": \"" << url << "\",\n";

    // Inject trace context into headers
    httplib::Headers headers;
    HttpTraceContextWriter trace_context_writer(headers);
    span->InjectContext(trace_context_writer);

    // Call external URL with HTTP client
    auto external_res = cli.Get(path, headers);
    
    if (external_res) {
        json_response << "  \"status_code\": " << external_res->status << ",\n";
        json_response << "  \"response_body\": \"" << external_res->body << "\",\n";
        json_response << "  \"success\": true\n";
        
        std::cout << "Outgoing call successful: " 
                  << "Status=" << external_res->status 
                  << ", Body=" << external_res->body << std::endl;
        anno->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, external_res->status);
    } else {
        json_response << "  \"status_code\": 0,\n";
        json_response << "  \"response_body\": \"Failed to connect\",\n";
        json_response << "  \"success\": false\n";
        
        std::string err_msg = "Outgoing call failed: Unable to connect to localhost:9000";
        std::cout << err_msg << std::endl;

        CppTraceCallStackReader reader;
        se->SetError("HandleOutgoingError", err_msg, reader);
    }
    
    json_response << "}";
    
    // Set response
    res.set_content(json_response.str(), "application/json");
    res.status = 200;
    
    std::cout << "Request handled: " << req.path 
              << " -> Called " << url << std::endl;
    span->EndSpanEvent();
}
