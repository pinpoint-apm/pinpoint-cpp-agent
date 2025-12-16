#include <iostream>
#include <string>
#include <cstdlib>
#include <curl/curl.h>

#include "pinpoint/tracer.h"
#include "httplib.h"
#include "http_trace_context.h"

// Global variables
pinpoint::AgentPtr g_agent = nullptr;
std::string g_target_url = "http://google.com";

// Trace Context Writer adapter for libcurl
class CurlTraceContextWriter : public pinpoint::TraceContextWriter {
public:
    explicit CurlTraceContextWriter(struct curl_slist** headers) : headers_(headers) {}

    void Set(std::string_view key, std::string_view value) override {
        std::string header = std::string(key) + ": " + std::string(value);
        *headers_ = curl_slist_append(*headers_, header.c_str());
    }

private:
    struct curl_slist** headers_;
};

// Write callback for cURL to capture response body (optional)
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

bool perform_curl_request(const std::string& url, pinpoint::SpanPtr span, std::string& out_response) {
    CURL* curl;
    CURLcode res;
    struct curl_slist* headers = NULL;

    // Create SpanEvent for the outgoing HTTP call
    auto se = span->NewSpanEvent("execute_curl");
    se->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
    se->SetDestination(url);
    
    // Record URL annotation
    se->GetAnnotations()->AppendString(pinpoint::ANNOTATION_HTTP_URL, url);

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_response);

        // Inject Trace Context headers
        CurlTraceContextWriter writer(&headers);
        span->InjectContext(writer);
        
        // Add custom headers to cURL
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Perform the request
        res = curl_easy_perform(curl);
        
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

        if(res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
            se->SetError(curl_easy_strerror(res));
            span->EndSpanEvent();
            
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            return false;
        }
        
        // Record status code
        se->GetAnnotations()->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, (int)response_code);

        // Clean up
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }

    span->EndSpanEvent();
    return true;
}

void handle_run_request(const httplib::Request& req, httplib::Response& res) {
    // Extract trace context from HTTP request headers
    HttpHeaderReader reader(req.headers);
    auto span = g_agent->NewSpan("CurlWebExample", "/run_request", reader);
    
    span->SetRemoteAddress(req.remote_addr);
    span->SetEndPoint(req.get_header_value("Host"));
    HttpHeaderReader http_reader(req.headers);
    span->RecordHeader(pinpoint::HTTP_REQUEST, http_reader);

    // Determine target URL
    std::string target = g_target_url;
    if (req.has_param("url")) {
        target = req.get_param_value("url");
    }

    std::string response_body;
    bool success = perform_curl_request(target, span, response_body);

    if (success) {
        res.status = 200;
        res.set_content("cURL request successful\nResponse body preview: " + response_body.substr(0, 200), "text/plain");
    } else {
        res.status = 500;
        res.set_content("cURL request failed", "text/plain");
        span->SetError("cURL request failed");
    }

    HttpHeaderReader response_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, response_reader);
    span->SetStatusCode(res.status);
    span->EndSpan();
}

int main(int argc, char* argv[]) {
    if (const char* env_url = std::getenv("TARGET_URL")) {
        g_target_url = env_url;
    }

    // Pinpoint Agent Init
    pinpoint::SetConfigString(
        "AgentId=curl-web-agent\n"
        "ApplicationName=CurlWebExampleApp\n"
        "Collector.Ip=127.0.0.1\n"
        "Collector.GrpcPort=9991\n"
        "Collector.StatPort=9992\n"
        "Collector.SpanPort=9993"
    );
    g_agent = pinpoint::CreateAgent();

    httplib::Server server;
    server.Get("/run_request", handle_run_request);

    std::cout << "Curl Web Example starting on http://localhost:8091" << std::endl;
    std::cout << "Default target URL: " << g_target_url << std::endl;
    
    server.listen("0.0.0.0", 8091);
    
    g_agent->Shutdown();
    return 0;
}
