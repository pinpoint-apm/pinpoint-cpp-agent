# Pinpoint C++ Agent - Code Examples

This document provides reusable code examples and helper classes for instrumenting your C++ applications with Pinpoint.

## Table of Contents
- [Helper Implementations](#helper-implementations)
- [HTTP Server Examples](#http-server-examples)
- [HTTP Client Examples](#http-client-examples)
- [Database Examples](#database-examples)
- [Error Handling Examples](#error-handling-examples)

## Helper Implementations

### TraceContextReader for HTTP Headers

Implementation for reading trace context from HTTP headers:

```cpp
#include "pinpoint/tracer.h"
#include <optional>
#include <string>
#include <string_view>

// For cpp-httplib
#include "httplib.h"

class HttpTraceContextReader : public pinpoint::TraceContextReader {
public:
    explicit HttpTraceContextReader(const httplib::Headers& headers) 
        : headers_(headers) {}
    
    ~HttpTraceContextReader() override = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(key.data());
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

private:
    const httplib::Headers& headers_;
};
```

### TraceContextWriter for HTTP Headers

Implementation for injecting trace context into HTTP headers:

```cpp
class HttpTraceContextWriter : public pinpoint::TraceContextWriter {
public:
    explicit HttpTraceContextWriter(httplib::Headers& headers) 
        : headers_(headers) {}
    
    ~HttpTraceContextWriter() override = default;

    void Set(std::string_view key, std::string_view value) override {
        headers_.emplace(key, value);
    }

private:
    httplib::Headers& headers_;
};
```

### HeaderReader for HTTP Headers

Implementation for reading and recording HTTP headers:

```cpp
#include <algorithm>
#include <functional>

class HttpHeaderReader : public pinpoint::HeaderReader {
public:
    explicit HttpHeaderReader(const httplib::Headers& headers) 
        : headers_(headers) {}
    
    ~HttpHeaderReader() override = default;

    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(key.data());
        if (it == headers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void ForEach(std::function<bool(std::string_view key, 
                                    std::string_view val)> callback) const override {
        for (const auto& [key, val] : headers_) {
            if (!callback(key, val)) {
                break;  // Stop if callback returns false
            }
        }
    }

private:
    const httplib::Headers& headers_;
};
```

### CallStackReader for Stack Traces

Implementation using cpptrace library:

```cpp
#include <cpptrace/cpptrace.hpp>
#include <cpptrace/utils.hpp>

class CppTraceCallStackReader : public pinpoint::CallStackReader {
public:
    CppTraceCallStackReader() = default;
    ~CppTraceCallStackReader() override = default;

    void ForEach(std::function<void(std::string_view module,
                                   std::string_view function,
                                   std::string_view file,
                                   int line)> callback) const override {
        // Skip first 2 frames (this function and the error handler)
        auto stack_trace = cpptrace::generate_trace(2, 32);
        
        for (const auto& frame : stack_trace.frames) {
            auto symbol = cpptrace::prune_symbol(frame.symbol);
            
            // Skip std library and empty symbols
            if (symbol.empty() || symbol.starts_with("std::")) {
                continue;
            }

            std::string module = "unknown";
            std::string function = symbol;
            std::string file = cpptrace::basename(frame.filename);
            int line = frame.line.value_or(0);
            
            if (file.empty()) {
                file = "unknown";
                line = 0;
            }

            // Try to extract module from namespace
            auto pos = symbol.rfind("::");
            if (pos != std::string::npos) {
                module = symbol.substr(0, pos);
                function = symbol.substr(pos + 2);
            } else {
                // Use file name as module if no namespace
                pos = file.find(".");
                if (pos != std::string::npos) {
                    module = file.substr(0, pos);
                }
            }

            callback(module, function, file, line);
        }
    }
};
```

## HTTP Server Examples

### Complete HTTP Server with Tracing

```cpp
#include "pinpoint/tracer.h"
#include "httplib.h"
#include <thread>
#include <iostream>

// Thread-local storage for current span
thread_local pinpoint::SpanPtr current_span;

void set_span_context(pinpoint::SpanPtr span) { 
    current_span = span; 
}

pinpoint::SpanPtr get_span_context() { 
    return current_span; 
}

// Create span for incoming request
pinpoint::SpanPtr trace_request(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();
    
    // Extract trace context from incoming headers
    HttpTraceContextReader trace_reader(req.headers);
    auto span = agent->NewSpan("MyWebService", req.path, req.method, trace_reader);
    
    // Set request information
    span->SetRemoteAddress(req.remote_addr);
    
    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    span->SetEndPoint(end_point);
    
    // Record request headers
    HttpHeaderReader header_reader(req.headers);
    span->RecordHeader(pinpoint::HTTP_REQUEST, header_reader);
    
    return span;
}

// End span and record response
void trace_response(const httplib::Request& req, 
                   httplib::Response& res, 
                   pinpoint::SpanPtr span) {
    // Record response headers
    HttpHeaderReader header_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, header_reader);
    
    // Set status code and URL statistics
    span->SetStatusCode(res.status);
    span->SetUrlStat(req.matched_route, req.method, res.status);
    
    span->EndSpan();
}

// Handler wrapper that adds tracing
httplib::Server::Handler wrap_handler(httplib::Server::Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        auto span = trace_request(req);
        set_span_context(span);
        
        try {
            handler(req, res);
        } catch (const std::exception& e) {
            span->SetError("HandlerError", e.what());
            res.status = 500;
            res.set_content("{\"error\": \"Internal Server Error\"}", 
                          "application/json");
        }
        
        trace_response(req, res, span);
        set_span_context(nullptr);
    };
}

// Example handler
void handle_users(const httplib::Request& req, httplib::Response& res) {
    auto span = get_span_context();
    
    // Trace the handler function
    span->NewSpanEvent("handle_users");
    
    // Simulate some processing
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Business logic
    res.set_content("{\"users\": []}", "application/json");
    res.status = 200;
    
    span->EndSpanEvent();
}

int main() {
    // Configure Pinpoint
    setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-web-server", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);
    
    // Create agent
    auto agent = pinpoint::CreateAgent();
    
    if (!agent->Enable()) {
        std::cerr << "Failed to enable Pinpoint agent" << std::endl;
        return 1;
    }
    
    // Create HTTP server
    httplib::Server server;
    
    // Register handlers with tracing
    server.Get("/users", wrap_handler(handle_users));
    server.Get(R"(/users/(\d+))", wrap_handler([](const httplib::Request& req, 
                                                    httplib::Response& res) {
        auto span = get_span_context();
        span->NewSpanEvent("get_user_by_id");
        
        // Extract user ID from path
        std::string user_id = req.matches[1];
        
        // Business logic
        res.set_content("{\"id\": " + user_id + "}", "application/json");
        res.status = 200;
        
        span->EndSpanEvent();
    }));
    
    std::cout << "Server starting on http://localhost:8080" << std::endl;
    server.listen("0.0.0.0", 8080);
    
    agent->Shutdown();
    return 0;
}
```

## HTTP Client Examples

### Making Traced HTTP Calls

```cpp
void make_http_call(pinpoint::SpanPtr span, 
                   const std::string& host,
                   const std::string& path) {
    // Create span event for HTTP call
    auto span_event = span->NewSpanEvent("httpCall");
    span_event->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
    span_event->SetEndPoint(host);
    span_event->SetDestination(host);
    
    // Add URL annotation
    auto annotations = span_event->GetAnnotations();
    std::string full_url = "http://" + host + path;
    annotations->AppendString(pinpoint::ANNOTATION_HTTP_URL, full_url);
    
    try {
        // Prepare headers and inject trace context
        httplib::Headers headers;
        HttpTraceContextWriter trace_writer(headers);
        span->InjectContext(trace_writer);
        
        // Make HTTP call
        httplib::Client client(host);
        auto res = client.Get(path, headers);
        
        if (res) {
            // Record response status
            annotations->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 
                                 res->status);
            
            std::cout << "HTTP call successful: " << res->status << std::endl;
        } else {
            // Record error
            span_event->SetError("HTTP call failed: connection error");
            std::cerr << "HTTP call failed" << std::endl;
        }
        
    } catch (const std::exception& e) {
        span_event->SetError("HTTPError", e.what());
    }
    
    span->EndSpanEvent();
}

// Example usage
void process_request(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    HttpTraceContextReader trace_reader(req.headers);
    auto span = agent->NewSpan("Service", req.path, trace_reader);
    
    // Make external HTTP call with distributed tracing
    make_http_call(span, "api.example.com:80", "/data");
    
    res.set_content("{\"status\": \"ok\"}", "application/json");
    res.status = 200;
    
    span->SetStatusCode(res.status);
    span->EndSpan();
}
```

## Database Examples

### MySQL Query Tracing

```cpp
#include <mysqlx/xdevapi.h>

class DatabaseClient {
public:
    DatabaseClient(const std::string& connection_string)
        : session_(connection_string) {}
    
    void execute_query(pinpoint::SpanPtr span, 
                      const std::string& sql,
                      const std::vector<std::string>& params = {}) {
        // Create span event for database operation
        auto db_event = span->NewSpanEvent("SQL_QUERY");
        db_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
        db_event->SetEndPoint("mysql-server:3306");
        db_event->SetDestination("my_database");
        
        try {
            // Execute query
            auto stmt = session_.sql(sql);
            for (const auto& param : params) {
                stmt.bind(param);
            }
            auto result = stmt.execute();
            
            // Format parameters for logging
            std::stringstream param_str;
            for (size_t i = 0; i < params.size(); i++) {
                if (i > 0) param_str << ", ";
                param_str << params[i];
            }
            
            // Record query
            db_event->SetSqlQuery(sql, param_str.str());
            
            // Log result
            std::cout << "Query executed successfully" << std::endl;
            if (result.hasData()) {
                std::cout << "Rows: " << result.count() << std::endl;
            }
            
        } catch (const mysqlx::Error& e) {
            db_event->SetError("MySQLError", e.what());
            throw;
        }
        
        span->EndSpanEvent();
    }
    
private:
    mysqlx::Session session_;
};

// Example usage
void handle_user_query(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    HttpTraceContextReader trace_reader(req.headers);
    auto span = agent->NewSpan("UserService", req.path, trace_reader);
    
    try {
        DatabaseClient db("mysqlx://user:pass@localhost:33060/mydb");
        
        // Execute SELECT query
        db.execute_query(span, "SELECT * FROM users WHERE id = ?", {"123"});
        
        res.set_content("{\"status\": \"ok\"}", "application/json");
        res.status = 200;
        
    } catch (const std::exception& e) {
        span->SetError("DatabaseError", e.what());
        res.status = 500;
        res.set_content("{\"error\": \"Database error\"}", "application/json");
    }
    
    span->SetStatusCode(res.status);
    span->EndSpan();
}
```

### Redis Operations

```cpp
#include <hiredis/hiredis.h>

class RedisClient {
public:
    RedisClient(const std::string& host, int port) {
        context_ = redisConnect(host.c_str(), port);
        if (context_ == nullptr || context_->err) {
            throw std::runtime_error("Redis connection failed");
        }
    }
    
    ~RedisClient() {
        if (context_) {
            redisFree(context_);
        }
    }
    
    std::string get(pinpoint::SpanPtr span, const std::string& key) {
        auto redis_event = span->NewSpanEvent("REDIS_GET");
        redis_event->SetServiceType(pinpoint::SERVICE_TYPE_REDIS);
        redis_event->SetEndPoint("redis-server:6379");
        redis_event->SetDestination("cache");
        
        std::string result;
        try {
            redisReply* reply = (redisReply*)redisCommand(context_, 
                                                         "GET %s", 
                                                         key.c_str());
            if (reply == nullptr) {
                throw std::runtime_error("Redis command failed");
            }
            
            if (reply->type == REDIS_REPLY_STRING) {
                result = reply->str;
            }
            
            freeReplyObject(reply);
            
            // Record operation
            auto annotations = redis_event->GetAnnotations();
            annotations->AppendString(pinpoint::ANNOTATION_API, 
                                    "GET " + key);
            
        } catch (const std::exception& e) {
            redis_event->SetError("RedisError", e.what());
            throw;
        }
        
        span->EndSpanEvent();
        return result;
    }
    
private:
    redisContext* context_;
};
```

## Error Handling Examples

### Exception Handling with Stack Traces

```cpp
void risky_operation(pinpoint::SpanPtr span) {
    auto span_event = span->NewSpanEvent("risky_operation");
    
    try {
        // Some operation that might throw
        perform_dangerous_task();
        
    } catch (const DatabaseException& e) {
        CppTraceCallStackReader stack_reader;
        span_event->SetError("DatabaseException", e.what(), stack_reader);
        throw;
        
    } catch (const NetworkException& e) {
        CppTraceCallStackReader stack_reader;
        span_event->SetError("NetworkException", e.what(), stack_reader);
        throw;
        
    } catch (const std::exception& e) {
        CppTraceCallStackReader stack_reader;
        span_event->SetError("UnexpectedException", e.what(), stack_reader);
        throw;
    }
    
    span->EndSpanEvent();
}
```

### RAII Span Guard

Automatically end spans even if exceptions occur:

```cpp
class SpanGuard {
public:
    explicit SpanGuard(pinpoint::SpanPtr span) : span_(span) {}
    
    ~SpanGuard() {
        if (span_) {
            span_->EndSpan();
        }
    }
    
    // Prevent copying
    SpanGuard(const SpanGuard&) = delete;
    SpanGuard& operator=(const SpanGuard&) = delete;
    
    // Allow moving
    SpanGuard(SpanGuard&& other) noexcept : span_(std::move(other.span_)) {
        other.span_ = nullptr;
    }
    
    pinpoint::SpanPtr get() const { return span_; }
    
private:
    pinpoint::SpanPtr span_;
};

// Usage
void handle_request(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    HttpTraceContextReader trace_reader(req.headers);
    auto span = agent->NewSpan("Service", req.path, trace_reader);
    
    SpanGuard guard(span);  // Automatically ends span on scope exit
    
    // Even if this throws, span will be ended
    process_request(span);
    
    span->SetStatusCode(200);
}
```

### Retry Logic with Tracing

```cpp
template<typename Func>
auto retry_with_tracing(pinpoint::SpanPtr span, 
                       const std::string& operation_name,
                       Func&& func,
                       int max_retries = 3) {
    for (int attempt = 1; attempt <= max_retries; attempt++) {
        auto span_event = span->NewSpanEvent(operation_name);
        
        // Add attempt annotation
        auto annotations = span_event->GetAnnotations();
        annotations->AppendInt(10000, attempt);  // Custom annotation
        
        try {
            auto result = func();
            span->EndSpanEvent();
            return result;
            
        } catch (const std::exception& e) {
            span_event->SetError("AttemptFailed", 
                               "Attempt " + std::to_string(attempt) + ": " + 
                               e.what());
            span->EndSpanEvent();
            
            if (attempt == max_retries) {
                throw;  // Re-throw on last attempt
            }
            
            // Wait before retry
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100 * attempt));
        }
    }
}

// Usage
void make_reliable_call(pinpoint::SpanPtr span) {
    retry_with_tracing(span, "external_api_call", []() {
        return call_external_api();
    }, 3);
}
```

## Utility Functions

### URL Pattern Normalization

```cpp
std::string normalize_url_pattern(const std::string& path) {
    // Replace numeric IDs with :id
    std::regex id_pattern(R"(/\d+)");
    std::string result = std::regex_replace(path, id_pattern, "/:id");
    
    // Replace UUIDs with :uuid
    std::regex uuid_pattern(
        R"(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})",
        std::regex::icase);
    result = std::regex_replace(result, uuid_pattern, "/:uuid");
    
    return result;
}

// Usage
span->SetUrlStat(normalize_url_pattern(req.path), req.method, res.status);
```

### Configuration Helper

```cpp
class PinpointConfig {
public:
    static void configure_from_env() {
        setenv("PINPOINT_CPP_CONFIG_FILE", 
              get_env_or_default("PINPOINT_CONFIG", "/tmp/pinpoint-config.yaml").c_str(), 
              0);
        
        setenv("PINPOINT_CPP_APPLICATION_NAME",
              get_env_or_default("APP_NAME", "cpp-app").c_str(),
              0);
        
        setenv("PINPOINT_CPP_COLLECTOR_HOST",
              get_env_or_default("PINPOINT_COLLECTOR", "localhost").c_str(),
              0);
    }
    
private:
    static std::string get_env_or_default(const char* key, 
                                          const std::string& default_val) {
        const char* val = std::getenv(key);
        return val ? val : default_val;
    }
};

// Usage
int main() {
    PinpointConfig::configure_from_env();
    auto agent = pinpoint::CreateAgent();
    // ...
}
```

## Complete Working Examples

For complete, production-ready examples, see the `example/` directory:

- **[http_server.cpp](../example/http_server.cpp)** - HTTP server with request tracing
- **[web_demo.cpp](../example/web_demo.cpp)** - Web application with distributed tracing
- **[db_demo.cpp](../example/db_demo.cpp)** - Database query instrumentation

## Related Documentation

- [Quick Start Guide](quick_start.md)
- [Instrumentation Guide](instrument.md)
- [API Reference](../include/pinpoint/tracer.h)

## License

Apache License 2.0 - See [LICENSE](../LICENSE) for details.

