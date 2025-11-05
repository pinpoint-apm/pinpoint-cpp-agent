# Pinpoint C++ Agent - Instrumentation Guide

This guide provides detailed information on how to instrument your C++ applications using the Pinpoint C++ Agent API.

## Table of Contents
- [Core Concepts](#core-concepts)
- [Agent Initialization](#agent-initialization)
- [Span Management](#span-management)
- [Span Event Management](#span-event-management)
- [Distributed Tracing](#distributed-tracing)
- [HTTP Request Tracing](#http-request-tracing)
- [Database Query Tracing](#database-query-tracing)
- [Error Handling](#error-handling)
- [Annotations](#annotations)
- [Asynchronous Operations](#asynchronous-operations)
- [Best Practices](#best-practices)

## Core Concepts

### Agent

The `Agent` is the main entry point for the Pinpoint C++ library. It manages the connection to the Pinpoint collector and provides methods to create spans.

### Span

A `Span` represents a single transaction or request in your application. It has:
- A unique span ID and trace ID
- Start and end times
- Service type information
- Metadata (annotations)
- Zero or more span events

### Span Event

A `SpanEvent` represents a sub-operation within a span, such as:
- A database query
- An external HTTP call
- A function call
- Any other logical operation

### Trace Context

Trace context contains distributed tracing information that propagates between services:
- Trace ID
- Span ID
- Parent Span ID
- Sampling decision
- Application metadata

## Agent Initialization

### Creating an Agent

```cpp
#include "pinpoint/tracer.h"

int main() {
    // Option 1: Set config file path
    pinpoint::SetConfigFilePath("/path/to/pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();
    
    // Option 2: Set config string
    std::string config = R"(
        ApplicationName: "MyApp"
        Collector:
          GrpcHost: "localhost"
    )";
    pinpoint::SetConfigString(config);
    auto agent = pinpoint::CreateAgent();
    
    // Option 3: Create with custom app type
    auto agent = pinpoint::CreateAgent(pinpoint::APP_TYPE_CPP);
    
    // Your application logic
    
    agent->Shutdown();
    return 0;
}
```

### Using Global Agent

For convenience, you can use the global agent instance:

```cpp
void someFunction() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("operation", "/endpoint");
    // ...
}
```

### Checking Agent Status

```cpp
auto agent = pinpoint::CreateAgent();
if (!agent->Enable()) {
    std::cerr << "Agent failed to start - check configuration" << std::endl;
    // Continue without tracing or exit
}
```

## Span Management

### Creating a Span

A span represents the root transaction. Create one for each incoming request:

```cpp
// Simple span creation
auto span = agent->NewSpan("ServiceName", "/api/endpoint");

// With trace context (for distributed tracing)
HttpTraceContextReader reader(request_headers);
auto span = agent->NewSpan("ServiceName", "/api/endpoint", reader);

// With HTTP method
auto span = agent->NewSpan("ServiceName", "/api/endpoint", "GET", reader);
```

### Setting Span Properties

```cpp
// Set remote client address
span->SetRemoteAddress("192.168.1.100");

// Set service endpoint
span->SetEndPoint("localhost:8080");

// Set service type
span->SetServiceType(pinpoint::SERVICE_TYPE_CPP);

// Set custom start time
auto start_time = std::chrono::system_clock::now();
span->SetStartTime(start_time);

// Set HTTP status code
span->SetStatusCode(200);
```

### Getting Span Information

```cpp
// Get trace ID
auto& trace_id = span->GetTraceId();
std::cout << "Trace: " << trace_id.ToString() << std::endl;

// Get span ID
int64_t span_id = span->GetSpanId();

// Check if sampled
bool sampled = span->IsSampled();
if (!sampled) {
    // This span won't be sent to the collector
    // Skip expensive operations
}
```

### Ending a Span

Always end spans when the operation completes:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyService", "/api/users");
    
    try {
        // Process request
        processRequest();
        span->SetStatusCode(200);
    } catch (const std::exception& e) {
        span->SetError("RequestError", e.what());
        span->SetStatusCode(500);
    }
    
    // Always end the span
    span->EndSpan();
}
```

## Span Event Management

### Creating Span Events

Span events track sub-operations within a span:

```cpp
auto span = agent->NewSpan("MyService", "/api/endpoint");

// Create a span event
auto event = span->NewSpanEvent("databaseQuery");

// With service type
auto event = span->NewSpanEvent("httpCall", pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);

// Process the operation
performOperation();

// End the span event
span->EndSpanEvent();
```

### Nested Span Events

Span events can be nested to represent call hierarchies:

```cpp
auto span = agent->NewSpan("MyService", "/api/endpoint");

// Level 1
span->NewSpanEvent("processRequest");
{
    // Level 2
    span->NewSpanEvent("validateInput");
    validateInput();
    span->EndSpanEvent();
    
    // Level 2
    span->NewSpanEvent("businessLogic");
    {
        // Level 3
        span->NewSpanEvent("queryDatabase");
        queryDatabase();
        span->EndSpanEvent();
    }
    span->EndSpanEvent();
}
span->EndSpanEvent();

span->EndSpan();
```

### Getting Current Span Event

```cpp
// Get the current active span event to set properties
auto span_event = span->GetSpanEvent();
if (span_event) {
    span_event->SetOperationName("customOperation");
    span_event->SetServiceType(pinpoint::SERVICE_TYPE_REDIS);
}
```

### Setting Span Event Properties

```cpp
auto span_event = span->NewSpanEvent("operation");

// Set service type
span_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);

// Set operation name
span_event->SetOperationName("SELECT * FROM users");

// Set start time
auto start_time = std::chrono::system_clock::now();
span_event->SetStartTime(start_time);

// Set destination (database name, cache key, etc.)
span_event->SetDestination("user_database");

// Set endpoint (host:port)
span_event->SetEndPoint("mysql-server:3306");

span->EndSpanEvent();
```

## Distributed Tracing

Distributed tracing allows you to track requests across multiple services.

### Server Side: Extracting Context

When receiving a request, extract the trace context:

```cpp
// Define a TraceContextReader implementation
class HttpTraceContextReader : public pinpoint::TraceContextReader {
public:
    HttpTraceContextReader(const httplib::Headers& headers) : headers_(headers) {}
    
    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(std::string(key));
        if (it != headers_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
private:
    const httplib::Headers& headers_;
};

// Use it when creating a span
void handleHttpRequest(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    
    // Extract trace context from incoming headers
    HttpTraceContextReader trace_reader(req.headers);
    auto span = agent->NewSpan("MyService", req.path, "GET", trace_reader);
    
    // Process request
    // ...
    
    span->EndSpan();
}
```

You can also extract context after span creation:

```cpp
auto span = agent->NewSpan("MyService", "/api/endpoint");

HttpTraceContextReader trace_reader(req.headers);
span->ExtractContext(trace_reader);

// Continue processing
```

### Client Side: Injecting Context

When making outgoing requests, inject the trace context:

```cpp
// Define a TraceContextWriter implementation
class HttpTraceContextWriter : public pinpoint::TraceContextWriter {
public:
    HttpTraceContextWriter(httplib::Headers& headers) : headers_(headers) {}
    
    void Set(std::string_view key, std::string_view value) override {
        headers_.insert({std::string(key), std::string(value)});
    }
    
private:
    httplib::Headers& headers_;
};

// Use it when making outgoing calls
void makeHttpCall(pinpoint::SpanPtr span, const std::string& url) {
    auto span_event = span->NewSpanEvent("httpCall");
    span_event->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
    span_event->SetEndPoint("external-service:8080");
    
    // Prepare outgoing headers
    httplib::Headers headers;
    HttpTraceContextWriter trace_writer(headers);
    
    // Inject trace context
    span->InjectContext(trace_writer);
    
    // Make HTTP call with headers
    httplib::Client cli("external-service", 8080);
    auto res = cli.Get(url, headers);
    
    if (res) {
        auto anno = span_event->GetAnnotations();
        anno->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, res->status);
    } else {
        span_event->SetError("HTTP call failed");
    }
    
    span->EndSpanEvent();
}
```

### Trace Context Headers

Pinpoint uses the following headers for trace propagation:

```cpp
pinpoint::HEADER_TRACE_ID          // "Pinpoint-TraceID"
pinpoint::HEADER_SPAN_ID           // "Pinpoint-SpanID"
pinpoint::HEADER_PARENT_SPAN_ID    // "Pinpoint-pSpanID"
pinpoint::HEADER_SAMPLED           // "Pinpoint-Sampled"
pinpoint::HEADER_FLAG              // "Pinpoint-Flags"
pinpoint::HEADER_PARENT_APP_NAME   // "Pinpoint-pAppName"
pinpoint::HEADER_PARENT_APP_TYPE   // "Pinpoint-pAppType"
pinpoint::HEADER_HOST              // "Pinpoint-Host"
```

## HTTP Request Tracing

### Complete HTTP Server Example

```cpp
#include "pinpoint/tracer.h"
#include "3rd_party/httplib.h"

// Thread-local storage for current span
thread_local pinpoint::SpanPtr current_span;

void set_span_context(pinpoint::SpanPtr span) { 
    current_span = span; 
}

pinpoint::SpanPtr get_span_context() { 
    return current_span; 
}

pinpoint::SpanPtr trace_request(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();
    
    // Extract trace context
    HttpTraceContextReader trace_reader(req.headers);
    auto span = agent->NewSpan("C++ Web Server", req.path, req.method, trace_reader);
    
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

// Wrapper to add tracing to any handler
httplib::Server::Handler wrap_handler(httplib::Server::Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        auto span = trace_request(req);
        set_span_context(span);
        
        try {
            handler(req, res);
        } catch (const std::exception& e) {
            span->SetError("HandlerError", e.what());
            res.status = 500;
        }
        
        trace_response(req, res, span);
        set_span_context(nullptr);
    };
}

void handle_users(const httplib::Request& req, httplib::Response& res) {
    auto span = get_span_context();
    
    // Trace the handler function
    span->NewSpanEvent("handle_users");
    
    // Business logic
    res.set_content("{\"users\": []}", "application/json");
    res.status = 200;
    
    span->EndSpanEvent();
}

int main() {
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-web-server", 0);
    auto agent = pinpoint::CreateAgent();
    
    httplib::Server server;
    server.Get("/users", wrap_handler(handle_users));
    
    server.listen("0.0.0.0", 8080);
    agent->Shutdown();
}
```

### Recording HTTP Headers

Implement `HeaderReader` to record HTTP headers:

```cpp
class HttpHeaderReader : public pinpoint::HeaderReader {
public:
    HttpHeaderReader(const httplib::Headers& headers) : headers_(headers) {}
    
    std::optional<std::string> Get(std::string_view key) const override {
        auto it = headers_.find(std::string(key));
        if (it != headers_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    void ForEach(std::function<bool(std::string_view key, 
                                    std::string_view val)> callback) const override {
        for (const auto& [key, val] : headers_) {
            if (!callback(key, val)) {
                break;
            }
        }
    }
    
private:
    const httplib::Headers& headers_;
};

// Record headers
HttpHeaderReader header_reader(request.headers);
span->RecordHeader(pinpoint::HTTP_REQUEST, header_reader);
span->RecordHeader(pinpoint::HTTP_RESPONSE, response_header_reader);
```

### URL Statistics

Collect URL statistics for monitoring:

```cpp
// Set URL pattern, method, and status code
span->SetUrlStat("/users/:id", "GET", 200);

// This collects statistics for:
// - URL pattern (with path parameters normalized)
// - HTTP method
// - Response status code
```

## Database Query Tracing

### Tracing SQL Queries

```cpp
void executeQuery(pinpoint::SpanPtr span, const std::string& sql) {
    // Create span event for database operation
    auto db_event = span->NewSpanEvent("SQL_SELECT");
    
    // Set database service type
    db_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    
    // Set database information
    db_event->SetEndPoint("mysql-server:3306");
    db_event->SetDestination("user_database");
    
    try {
        // Execute the query
        auto result = database->execute(sql);
        
        // Record SQL query (without sensitive data)
        db_event->SetSqlQuery(sql, "");
        
    } catch (const std::exception& e) {
        db_event->SetError("SQL_ERROR", e.what());
    }
    
    span->EndSpanEvent();
}
```

### Tracing Parameterized Queries

```cpp
void executeParameterizedQuery(pinpoint::SpanPtr span,
                               const std::string& sql,
                               const std::vector<std::string>& params) {
    auto db_event = span->NewSpanEvent("SQL_INSERT");
    db_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    db_event->SetEndPoint("localhost:3306");
    db_event->SetDestination("test_db");
    
    try {
        // Format parameters (avoid logging sensitive data)
        std::stringstream param_str;
        for (size_t i = 0; i < params.size(); i++) {
            if (i > 0) param_str << ", ";
            param_str << params[i];
        }
        
        // Execute query with parameters
        auto stmt = session->sql(sql);
        for (const auto& param : params) {
            stmt.bind(param);
        }
        auto result = stmt.execute();
        
        // Record query with parameters
        db_event->SetSqlQuery(sql, param_str.str());
        
        // Add result information
        auto anno = db_event->GetAnnotations();
        anno->AppendInt(pinpoint::ANNOTATION_SQL_UID, 1);
        
    } catch (const std::exception& e) {
        db_event->SetError("DB_ERROR", e.what());
    }
    
    span->EndSpanEvent();
}
```

### Supported Database Types

Use the appropriate service type constant:

```cpp
pinpoint::SERVICE_TYPE_MYSQL_QUERY      // MySQL
pinpoint::SERVICE_TYPE_PGSQL_QUERY      // PostgreSQL
pinpoint::SERVICE_TYPE_ORACLE_QUERY     // Oracle
pinpoint::SERVICE_TYPE_MSSQL_QUERY      // SQL Server
pinpoint::SERVICE_TYPE_MONGODB_QUERY    // MongoDB
pinpoint::SERVICE_TYPE_CASSANDRA_QUERY  // Cassandra
```

### Complete Database Example

See `example/db_demo.cpp` for a full working example with:
- Connection management
- Query execution with tracing
- Error handling
- Parameter binding

## Error Handling

### Setting Error Messages

```cpp
// Simple error message
span->SetError("Something went wrong");

// Error with name and message
span->SetError("DatabaseError", "Connection timeout after 30s");

// For span events
span_event->SetError("QueryError", "Invalid SQL syntax");
```

### Recording Stack Traces

Implement `CallStackReader` to capture and record stack traces:

```cpp
class CppTraceCallStackReader : public pinpoint::CallStackReader {
public:
    void ForEach(std::function<void(std::string_view module,
                                   std::string_view function,
                                   std::string_view file,
                                   int line)> callback) const override {
        // Use your preferred stack trace library (cpptrace, backward-cpp, etc.)
        auto stack_trace = cpptrace::generate_trace();
        for (const auto& frame : stack_trace.frames) {
            callback(
                frame.module.c_str(),
                frame.function.c_str(),
                frame.filename.c_str(),
                frame.line
            );
        }
    }
};

// Record error with stack trace
try {
    dangerousOperation();
} catch (const std::exception& e) {
    CppTraceCallStackReader stack_reader;
    span_event->SetError("OperationFailed", e.what(), stack_reader);
}
```

### Exception Handling Pattern

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyService", "/api/endpoint");
    
    try {
        processRequest(span);
        span->SetStatusCode(200);
        
    } catch (const DatabaseException& e) {
        span->SetError("DatabaseError", e.what());
        span->SetStatusCode(500);
        
    } catch (const ValidationException& e) {
        span->SetError("ValidationError", e.what());
        span->SetStatusCode(400);
        
    } catch (const std::exception& e) {
        CppTraceCallStackReader stack_reader;
        span->SetError("UnexpectedError", e.what());
        span->SetStatusCode(500);
        
    } catch (...) {
        span->SetError("UnknownError", "An unknown error occurred");
        span->SetStatusCode(500);
    }
    
    span->EndSpan();
}
```

## Annotations

Annotations provide additional metadata about operations.

### Predefined Annotation Keys

```cpp
pinpoint::ANNOTATION_API                    // API/method name
pinpoint::ANNOTATION_SQL_ID                 // SQL statement ID
pinpoint::ANNOTATION_SQL_UID                // SQL UID
pinpoint::ANNOTATION_EXCEPTION_ID           // Exception information
pinpoint::ANNOTATION_HTTP_URL               // HTTP URL
pinpoint::ANNOTATION_HTTP_STATUS_CODE       // HTTP status code
pinpoint::ANNOTATION_HTTP_COOKIE            // HTTP cookies
pinpoint::ANNOTATION_HTTP_REQUEST_HEADER    // HTTP request headers
pinpoint::ANNOTATION_HTTP_RESPONSE_HEADER   // HTTP response headers
```

### Adding Annotations

```cpp
auto annotations = span->GetAnnotations();

// String annotation
annotations->AppendString(pinpoint::ANNOTATION_API, "getUserById");
annotations->AppendString(pinpoint::ANNOTATION_HTTP_URL, "http://api.example.com/users/123");

// Integer annotation
annotations->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

// Long annotation
annotations->AppendLong(12345, 1234567890L);

// String-String pair
annotations->AppendStringString(100, "key", "value");

// Int-String-String
annotations->AppendIntStringString(200, 42, "description", "value");
```

### Span Event Annotations

```cpp
auto span_event = span->NewSpanEvent("operation");
auto annotations = span_event->GetAnnotations();

annotations->AppendString(pinpoint::ANNOTATION_API, "processData");
annotations->AppendInt(1000, 42);  // Custom annotation

span->EndSpanEvent();
```

### Custom Annotations

You can use custom annotation keys (avoid conflicts with predefined keys):

```cpp
// Use high numbers for custom annotations
constexpr int32_t CUSTOM_USER_ID = 10000;
constexpr int32_t CUSTOM_SESSION_ID = 10001;
constexpr int32_t CUSTOM_CACHE_HIT = 10002;

auto annotations = span->GetAnnotations();
annotations->AppendString(CUSTOM_USER_ID, "user-123");
annotations->AppendString(CUSTOM_SESSION_ID, "session-456");
annotations->AppendInt(CUSTOM_CACHE_HIT, 1);  // 1 = hit, 0 = miss
```

## Asynchronous Operations

### Creating Async Spans

For asynchronous operations that continue after the parent span ends:

```cpp
void handleAsyncRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyService", "/async-endpoint");
    
    // Create async span for background processing
    auto async_span = span->NewAsyncSpan("backgroundProcessing");
    
    // Start async operation
    std::thread([async_span]() {
        // This runs asynchronously
        auto event = async_span->NewSpanEvent("processInBackground");
        
        // Do async work
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        async_span->EndSpanEvent();
        async_span->EndSpan();
    }).detach();
    
    // End parent span immediately
    span->SetStatusCode(202);  // Accepted
    span->EndSpan();
}
```

### Async Span Best Practices

1. **Always end async spans**: Make sure to call `EndSpan()` even in error cases
2. **Thread safety**: Span objects can be passed between threads
3. **Lifecycle management**: Use `shared_ptr` to manage span lifetime
4. **Resource cleanup**: Ensure async spans don't leak memory

```cpp
void safeAsyncOperation() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("Service", "/endpoint");
    
    auto async_span = span->NewAsyncSpan("async_work");
    
    // Capture span in shared_ptr for thread safety
    std::thread([async_span]() {
        try {
            auto event = async_span->NewSpanEvent("work");
            performWork();
            async_span->EndSpanEvent();
            async_span->EndSpan();
            
        } catch (const std::exception& e) {
            async_span->SetError("AsyncError", e.what());
            async_span->EndSpan();
        }
    }).detach();
    
    span->EndSpan();
}
```

## Best Practices

### 1. Always End Spans and Events

Use RAII or ensure proper cleanup:

```cpp
class SpanGuard {
public:
    SpanGuard(pinpoint::SpanPtr span) : span_(span) {}
    ~SpanGuard() { if (span_) span_->EndSpan(); }
    
private:
    pinpoint::SpanPtr span_;
};

void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("Service", "/endpoint");
    SpanGuard guard(span);
    
    // Even if exceptions occur, span will be ended
    processRequest();
}
```

### 2. Check Sampling Before Expensive Operations

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("Service", "/endpoint");
    
    // Check if this span is sampled
    if (span->IsSampled()) {
        // Only collect expensive data if sampled
        collectDetailedMetrics();
        addExtensiveAnnotations();
    }
    
    // Always do the actual work
    processRequest();
    
    span->EndSpan();
}
```

### 3. Use Thread-Local Storage for Context

```cpp
// Thread-local storage for current span
thread_local pinpoint::SpanPtr current_span;

void setSpan(pinpoint::SpanPtr span) { current_span = span; }
pinpoint::SpanPtr getSpan() { return current_span; }

void handleRequest() {
    auto span = agent->NewSpan("Service", "/endpoint");
    setSpan(span);
    
    // Call other functions without passing span
    processData();
    
    span->EndSpan();
    setSpan(nullptr);
}

void processData() {
    auto span = getSpan();
    if (span) {
        span->NewSpanEvent("processData");
        // ...
        span->EndSpanEvent();
    }
}
```

### 4. Sanitize Sensitive Data

Never log sensitive information:

```cpp
void executeQuery(const std::string& sql, const std::string& password) {
    auto span_event = span->NewSpanEvent("SQL_QUERY");
    
    // DON'T: Record sensitive data
    // span_event->SetSqlQuery(sql, password);
    
    // DO: Sanitize or omit sensitive data
    span_event->SetSqlQuery(sql, "[REDACTED]");
    
    span->EndSpanEvent();
}
```

### 5. Use Appropriate Service Types

Always set the correct service type:

```cpp
// For database operations
span_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);

// For HTTP client calls
span_event->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);

// For Redis operations
span_event->SetServiceType(pinpoint::SERVICE_TYPE_REDIS);

// For custom operations
span_event->SetServiceType(pinpoint::SERVICE_TYPE_CPP_FUNC);
```

### 6. Minimize Performance Impact

- Use sampling in production (not 100%)
- Avoid creating too many span events
- Don't add excessive annotations
- Check `IsSampled()` before expensive operations
- Consider agent overhead in performance testing

### 7. Handle Errors Gracefully

```cpp
void robustOperation() {
    pinpoint::SpanPtr span;
    try {
        auto agent = pinpoint::GlobalAgent();
        span = agent->NewSpan("Service", "/endpoint");
        
        processRequest();
        
    } catch (const std::exception& e) {
        if (span) {
            span->SetError("OperationError", e.what());
        }
        // Handle error appropriately
        
    } catch (...) {
        if (span) {
            span->SetError("UnknownError", "Unknown error occurred");
        }
    }
    
    if (span) {
        span->EndSpan();
    }
}
```

## Complete Examples

For complete, working examples, see the `example/` directory:

- **`http_server.cpp`**: HTTP server with request tracing
- **`web_demo.cpp`**: Web application with outgoing HTTP calls and distributed tracing
- **`db_demo.cpp`**: Database query tracing with MySQL

## API Reference

For detailed API documentation, see:
- `include/pinpoint/tracer.h` - Main API header
- Service type constants
- Annotation key constants
- Header constants for distributed tracing

## Troubleshooting

### Spans Not Appearing

- Check sampling rate (must be > 0)
- Verify `EndSpan()` is called
- Ensure agent is enabled
- Check collector connectivity

### Performance Issues

- Reduce sampling rate
- Minimize span events
- Reduce annotation count
- Check for resource leaks

### Distributed Tracing Not Working

- Verify trace context headers are propagated
- Check `InjectContext()` and `ExtractContext()` implementation
- Ensure TraceContextReader/Writer are implemented correctly
- Verify header names match Pinpoint's expectations

## Next Steps

- Review the [Quick Start Guide](quick_start.md) for basic setup
- Explore complete examples in the `example/` directory
- Configure advanced options in `pinpoint-config.yaml`
- Monitor your application in Pinpoint Web UI

## Support

- **GitHub**: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent)
- **Documentation**: [Pinpoint APM](https://pinpoint-apm.github.io/pinpoint/)
- **Issues**: Report bugs and request features on GitHub

## License

Apache License 2.0 - See [LICENSE](../LICENSE) for details.

