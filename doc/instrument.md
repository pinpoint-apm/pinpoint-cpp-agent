# Pinpoint C++ Agent - Instrumentation Guide

This document is the consolidated reference for instrumenting C++ applications with the Pinpoint C++ agent (`pinpoint-cpp-agent`). It covers the full tracer API declared in `include/pinpoint/tracer.h`, from bootstrapping through production best practices.

The C++ tracer API mirrors the design of the [Pinpoint Go agent](https://github.com/pinpoint-apm/pinpoint-go-agent). For conceptual background, see the Go agent's [quick start](https://github.com/pinpoint-apm/pinpoint-go-agent/blob/main/doc/quick_start.md) and [instrumentation guide](https://github.com/pinpoint-apm/pinpoint-go-agent/blob/main/doc/instrument.md).

**Target readers**: C++ service owners and library authors who want to add Pinpoint tracing.

---

## Table of Contents

- [1. Core Concepts](#1-core-concepts)
- [2. Bootstrapping the Agent](#2-bootstrapping-the-agent)
- [3. Creating Spans for Incoming Work](#3-creating-spans-for-incoming-work)
- [4. Recording Span Events](#4-recording-span-events)
- [5. Annotations](#5-annotations)
- [6. Distributed Tracing and Context Propagation](#6-distributed-tracing-and-context-propagation)
- [7. HTTP Request Tracing](#7-http-request-tracing)
- [8. Database and Backend Instrumentation](#8-database-and-backend-instrumentation)
- [9. Error Reporting and Stack Traces](#9-error-reporting-and-stack-traces)
- [10. Asynchronous and Background Work](#10-asynchronous-and-background-work)
- [11. Sampling Policy](#11-sampling-policy)
- [12. HTTP Filtering and Header Recording](#12-http-filtering-and-header-recording)
- [13. Best Practices](#13-best-practices)
- [14. Troubleshooting](#14-troubleshooting)
- [15. Checklist for New Instrumentation](#15-checklist-for-new-instrumentation)

---

## 1. Core Concepts

Pinpoint models each transaction as a collection of **spans**.

### Span

Represents a top-level operation in your application, such as an HTTP request, gRPC method, scheduled job, or long-running worker iteration. A span records timing, status, key metadata, and contains a stack of **SpanEvents** describing internal operations. Each span has a unique span ID and trace ID.

### SpanEvent

Represents an operation *inside* a span: database query, HTTP client call, cache access, function block, etc. Multiple span events form a call-stack view in the Pinpoint UI.

### Annotations

Key/value metadata attached to spans or span events. Use them to record HTTP URLs, status codes, customer IDs, SQL parameters (sanitized), and other context.

### Distributed Tracing

Trace context (trace ID, span ID, sampling decision, etc.) is propagated across services using transport-specific headers (e.g., HTTP). The C++ agent uses `TraceContextReader` / `TraceContextWriter` interfaces to abstract header access.

### Supporting Types

| Type | Description |
|---|---|
| `Agent` | Entry point that manages configuration, span creation, and shutdown lifecycle. |
| `Span` | Top-level trace segment for an incoming request or logical operation. |
| `SpanEvent` | Fine-grained operations inside a span. |
| `Annotation` | Helper interface to append structured key/value metadata. |
| `TraceContextReader` / `TraceContextWriter` | Context propagation adapters for distributed tracing headers. |
| `HeaderReader` | Structured access to HTTP headers for recording request/response metadata. |
| `CallStackReader` | Optional stack trace provider for enriched error reporting. |

Free helpers (`SetConfigFilePath`, `SetConfigString`, `CreateAgent`, `GlobalAgent`) make configuration and agent bootstrapping convenient.

---

## 2. Bootstrapping the Agent

Before you can create spans, you must configure and create an `Agent` instance. Configuration can be supplied via a file path, inline config string, or environment variables.

### Creating an Agent

```cpp
#include "pinpoint/tracer.h"

int main() {
    // Option 1: Set config file path
    pinpoint::SetConfigFilePath("/path/to/pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();

    // Option 2: Set config string (YAML)
    std::string config = R"(
        ApplicationName: "MyApp"
        Collector:
          GrpcHost: "localhost"
    )";
    pinpoint::SetConfigString(config);
    auto agent = pinpoint::CreateAgent();

    // Option 3: Create with custom app type
    auto agent = pinpoint::CreateAgent(pinpoint::APP_TYPE_CPP);

    // Your application logic ...

    agent->Shutdown();  // flush pending data and stop worker threads
    return 0;
}
```

### Using the Global Agent

For convenience, you can access a global agent instance:

```cpp
void someFunction() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("operation", "/endpoint");
    // ...
    span->EndSpan();
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

### Environment Variable Configuration

You can also configure via environment variables before calling `CreateAgent()`:

```cpp
setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-web-demo", 0);
setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);

auto agent = pinpoint::CreateAgent();
```

### Startup Checklist

1. Set configuration (file or string) *before* calling `CreateAgent()`.
2. Configure additional features using environment variables (e.g., HTTP/SQL stats).
3. Hold the `AgentPtr` for the lifetime of the process.
4. Call `Shutdown()` on application exit (see [§10](#10-asynchronous-and-background-work)).

---

## 3. Creating Spans for Incoming Work

Create a span at the **entry point** of each transaction: HTTP/gRPC server handler, message consumer callback, scheduled job, or worker loop iteration.

### Span Creation Methods

There are two main overloads:

- `Agent::NewSpan(operation, rpc_point)` — starts a **new** transaction.
- `Agent::NewSpan(operation, rpc_point, TraceContextReader& reader)` — continues a transaction when upstream trace headers are present. Falls back to a new transaction if headers are missing.

### Example: HTTP Server Handler

```cpp
void handleRequest(const httplib::Request& req, httplib::Response& res) {
    // 1. Extract upstream context (if any)
    HttpTraceContextReader reader(req.headers);
    auto agent = pinpoint::GlobalAgent();

    // 2. Create span (continues trace if headers exist, otherwise starts new)
    auto span = agent->NewSpan("HTTP Server", req.path, reader);

    // 3. Record request details
    span->SetRemoteAddress(req.remote_addr);
    span->SetEndPoint(req.get_header_value("Host"));

    // Record request headers (optional)
    HttpHeaderReader header_reader(req.headers);
    span->RecordHeader(pinpoint::HTTP_REQUEST, header_reader);

    // ... process request ...

    // 4. Record response details
    span->SetStatusCode(res.status);
    span->SetUrlStat(req.matched_route, req.method, res.status);

    // 5. End span — MUST be called on all code paths
    span->EndSpan();
}
```

### Setting Span Properties

```cpp
span->SetRemoteAddress("192.168.1.100");                 // client IP
span->SetEndPoint("localhost:8080");                     // logical endpoint
span->SetServiceType(pinpoint::SERVICE_TYPE_CPP);        // service type
span->SetStatusCode(200);                                // HTTP status code

auto start_time = std::chrono::system_clock::now();
span->SetStartTime(start_time);                          // override start time
```

### Inspecting Span State

```cpp
auto& trace_id = span->GetTraceId();
std::cout << "Trace: " << trace_id.ToString() << std::endl;

int64_t span_id = span->GetSpanId();

bool sampled = span->IsSampled();
if (!sampled) {
    // This span will not be sent — skip expensive data collection
}
```

### Key Rules

- Always call `EndSpan()` on **all** code paths (success, error, exception).
- Prefer RAII or `try`/`catch` to guarantee `EndSpan()` is executed.
- Use descriptive `operation` and `rpc_point` names (e.g., `"C++ Web Demo"`, `"/users/:id"`).

### Thread-Local Storage for Span Context

Use TLS when span access is needed by nested helpers:

```cpp
thread_local pinpoint::SpanPtr current_span;

void set_span_context(pinpoint::SpanPtr span) { current_span = span; }
pinpoint::SpanPtr get_span_context() { return current_span; }
```

---

## 4. Recording Span Events

Span events describe important operations inside a span: HTTP/RPC client calls, database queries, cache operations, and function blocks. They form the call-stack view in the Pinpoint UI.

### Basic Usage

```cpp
auto span = agent->NewSpan("MyService", "/api/endpoint", reader);

// Start a span event
auto se = span->NewSpanEvent("process_logic");

// ... execute logic ...

// End the span event
span->EndSpanEvent();

span->EndSpan();
```

### Nested Span Events

Nest events to reflect the call hierarchy:

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

### SpanEvent Properties

```cpp
auto span_event = span->NewSpanEvent("operation");

span_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
span_event->SetOperationName("SELECT * FROM users");
span_event->SetDestination("user_database");
span_event->SetEndPoint("mysql-server:3306");

auto start_time = std::chrono::system_clock::now();
span_event->SetStartTime(start_time);

span->EndSpanEvent();
```

### Getting the Current Span Event

```cpp
auto span_event = span->GetSpanEvent();
if (span_event) {
    span_event->SetOperationName("customOperation");
    span_event->SetServiceType(pinpoint::SERVICE_TYPE_REDIS);
}
```

### Recommendations

- Create **one span event per major logical step**.
- Always pair `NewSpanEvent()` with `EndSpanEvent()` — prefer RAII where possible.
- Use appropriate `SERVICE_TYPE_*` constants for downstream services.
- Call `EndSpanEvent()` in the same scope or via RAII wrappers to avoid dangling events.

---

## 5. Annotations

Annotations enrich spans and span events with contextual metadata such as URLs, status codes, query parameters, and custom identifiers.

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

### Adding Annotations to Spans

```cpp
auto annotations = span->GetAnnotations();

// String annotations
annotations->AppendString(pinpoint::ANNOTATION_API, "getUserById");
annotations->AppendString(pinpoint::ANNOTATION_HTTP_URL, "http://api.example.com/users/123");

// Integer annotation
annotations->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

// Long annotation
annotations->AppendLong(12345, 1234567890L);

// Compound forms
annotations->AppendStringString(100, "key", "value");
annotations->AppendIntStringString(200, 42, "description", "value");
```

### Adding Annotations to Span Events

```cpp
auto se = span->NewSpanEvent("external_call");
se->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
se->SetEndPoint("localhost:9000");

auto anno = se->GetAnnotations();
anno->AppendString(pinpoint::ANNOTATION_HTTP_URL, url);
anno->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, status_code);

span->EndSpanEvent();
```

### Custom Annotations

Define your own keys using high integer values to avoid conflicts with predefined keys:

```cpp
constexpr int32_t CUSTOM_USER_ID    = 10000;
constexpr int32_t CUSTOM_SESSION_ID = 10001;
constexpr int32_t CUSTOM_CACHE_HIT  = 10002;

auto annotations = span->GetAnnotations();
annotations->AppendString(CUSTOM_USER_ID, "user-123");
annotations->AppendString(CUSTOM_SESSION_ID, "session-456");
annotations->AppendInt(CUSTOM_CACHE_HIT, 1);  // 1 = hit, 0 = miss
```

**Guideline**: Carefully sanitize annotations so that sensitive data (passwords, secrets, PII) is never recorded.

---

## 6. Distributed Tracing and Context Propagation

To connect traces across services, Pinpoint propagates context using transport-specific headers.

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

### Server Side: Extracting Context

When receiving a request, extract the trace context to continue the trace:

```cpp
void handleRequest(const httplib::Request& req) {
    HttpTraceContextReader reader(req.headers);
    auto agent = pinpoint::GlobalAgent();

    // NewSpan automatically extracts context if present
    auto span = agent->NewSpan("Server", req.path, reader);
    // ...
    span->EndSpan();
}
```

You can also extract context after span creation:

```cpp
auto span = agent->NewSpan("MyService", "/api/endpoint");
HttpTraceContextReader trace_reader(req.headers);
span->ExtractContext(trace_reader);
```

### Client Side: Injecting Context

When making an outgoing call, inject the trace context into the request headers:

```cpp
void sendRequest(pinpoint::SpanPtr span) {
    httplib::Client cli("localhost", 8080);
    httplib::Headers headers;

    HttpTraceContextWriter writer(headers);
    span->InjectContext(writer);  // adds Pinpoint-* headers

    auto res = cli.Get("/target", headers);
}
```

### Implementing Custom Adapters

To integrate with different frameworks or protocols, implement `TraceContextReader` and `TraceContextWriter`:

```cpp
class HttpTraceContextReader : public pinpoint::TraceContextReader {
public:
    explicit HttpTraceContextReader(const httplib::Headers& headers)
        : headers_(headers) {}

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

class HttpTraceContextWriter : public pinpoint::TraceContextWriter {
public:
    explicit HttpTraceContextWriter(httplib::Headers& headers)
        : headers_(headers) {}

    void Set(std::string_view key, std::string_view value) override {
        headers_.emplace(std::string(key), std::string(value));
    }

private:
    httplib::Headers& headers_;
};
```

You can apply the same pattern for message queues, custom RPC frameworks, or binary protocols by mapping trace keys to your own metadata format.

---

## 7. HTTP Request Tracing

### Complete HTTP Server Example

```cpp
#include "pinpoint/tracer.h"
#include "3rd_party/httplib.h"

// Thread-local storage for current span
thread_local pinpoint::SpanPtr current_span;

void set_span_context(pinpoint::SpanPtr span) { current_span = span; }
pinpoint::SpanPtr get_span_context() { return current_span; }

// Trace incoming request
pinpoint::SpanPtr trace_request(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();
    HttpTraceContextReader trace_reader(req.headers);
    auto span = agent->NewSpan("C++ Web Server", req.path, req.method, trace_reader);

    span->SetRemoteAddress(req.remote_addr);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    span->SetEndPoint(end_point);

    HttpHeaderReader header_reader(req.headers);
    span->RecordHeader(pinpoint::HTTP_REQUEST, header_reader);

    return span;
}

// Trace outgoing response
void trace_response(const httplib::Request& req,
                    httplib::Response& res,
                    pinpoint::SpanPtr span) {
    HttpHeaderReader header_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, header_reader);

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

// Example handler
void handle_users(const httplib::Request& req, httplib::Response& res) {
    auto span = get_span_context();
    span->NewSpanEvent("handle_users");

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

### Implementing HeaderReader

Implement `HeaderReader` to record HTTP request/response headers:

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

// Usage
HttpHeaderReader header_reader(request.headers);
span->RecordHeader(pinpoint::HTTP_REQUEST, header_reader);
span->RecordHeader(pinpoint::HTTP_RESPONSE, response_header_reader);
```

### URL Statistics

Collect URL statistics for monitoring:

```cpp
// Set URL pattern, method, and status code
span->SetUrlStat("/users/:id", "GET", 200);
```

This collects statistics normalized by URL pattern, HTTP method, and response status code.

---

## 8. Database and Backend Instrumentation

Database, cache, and other backend calls are represented as span events with appropriate service types and annotations.

### Tracing SQL Queries

```cpp
void executeQuery(pinpoint::SpanPtr span, const std::string& sql) {
    auto db_event = span->NewSpanEvent("SQL_SELECT");

    db_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    db_event->SetEndPoint("mysql-server:3306");
    db_event->SetDestination("user_database");

    try {
        auto result = database->execute(sql);
        db_event->SetSqlQuery(sql, "");  // record SQL without sensitive data

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

        auto stmt = session->sql(sql);
        for (const auto& param : params) {
            stmt.bind(param);
        }
        auto result = stmt.execute();

        db_event->SetSqlQuery(sql, param_str.str());

    } catch (const std::exception& e) {
        db_event->SetError("DB_ERROR", e.what());
    }

    span->EndSpanEvent();
}
```

### Supported Database Service Types

```cpp
pinpoint::SERVICE_TYPE_MYSQL_QUERY      // MySQL
pinpoint::SERVICE_TYPE_PGSQL_QUERY      // PostgreSQL
pinpoint::SERVICE_TYPE_ORACLE_QUERY     // Oracle
pinpoint::SERVICE_TYPE_MSSQL_QUERY      // SQL Server
pinpoint::SERVICE_TYPE_MONGODB_QUERY    // MongoDB
pinpoint::SERVICE_TYPE_CASSANDRA_QUERY  // Cassandra
```

### Recommendations

- Wrap DB access in helper methods so every query is traced consistently.
- Use the correct `SERVICE_TYPE_*` constant for the backend.
- Sanitize SQL text and parameters before recording — never log passwords or secrets.
- See `example/db_demo.cpp` for a full working example.

---

## 9. Error Reporting and Stack Traces

### Setting Error Messages

Call `SetError` on either `Span` or `SpanEvent` to capture failures:

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

// Usage
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
        span->SetError("UnexpectedError", e.what(), stack_reader);
        span->SetStatusCode(500);

    } catch (...) {
        span->SetError("UnknownError", "An unknown error occurred");
        span->SetStatusCode(500);
    }

    // Always end the span
    span->EndSpan();
}
```

**Tips**:
- Record errors at the granularity that is useful — often on the specific span event representing the failed operation.
- Always end the span event and span so the failure appears at a precise time in the timeline.

---

## 10. Asynchronous and Background Work

Tracing asynchronous or background tasks requires careful span lifecycle management.

### Using Existing Spans with Threads

Since `SpanPtr` is typically a shared pointer, you can pass it to worker threads:

```cpp
void outgoingRequest(pinpoint::SpanPtr span) {
    auto se = span->NewSpanEvent("outgoingRequest_thread");
    se->SetServiceType(pinpoint::SERVICE_TYPE_CPP_FUNC);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    span->EndSpanEvent();
}

void asyncWithThread(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("AsyncHandler", req.path);

    std::thread t([span]() {
        outgoingRequest(span);
        span->EndSpan();
    });
    t.detach();
}
```

### Async Spans (Fire-and-Forget Work)

Use `Span::NewAsyncSpan()` to trace background tasks that continue after the original request completes:

```cpp
void backgroundTask(pinpoint::SpanPtr asyncSpan) {
    auto se = asyncSpan->NewSpanEvent("background_job");
    // ... do work ...
    asyncSpan->EndSpanEvent();
    asyncSpan->EndSpan();
}

void handleRequest(const httplib::Request& req) {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("RequestHandler", req.path);

    // Create an async span linked to the current span
    auto asyncSpan = span->NewAsyncSpan("BackgroundTask");

    std::thread t(backgroundTask, asyncSpan);
    t.detach();

    span->EndSpan();  // main request finishes immediately
}
```

### Safe Async Pattern with Error Handling

```cpp
void safeAsyncOperation() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("Service", "/endpoint");

    auto async_span = span->NewAsyncSpan("async_work");

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

### Guidelines

- Each async span must be **ended exactly once**.
- Ensure the original span outlives any span events created on it.
- For long-running worker loops, create a **new span per iteration** rather than keeping a single span open indefinitely.
- Use `shared_ptr` semantics to manage span lifetime across threads.

---

## 11. Sampling Policy

### Why Sampling?

In microservice architectures, a single request traverses multiple services, generating numerous spans. Tracing every request can create significant system overhead, generate massive data volumes, and burden collector and storage systems. Transaction sampling selectively collects trace data from only a subset of transactions, minimizing overhead while still providing enough data to understand system health, identify bottlenecks, and detect anomalies.

### Key Concepts

- **New Transaction**: A transaction without a parent trace context — the root span of a trace.
- **Continue Transaction**: A transaction that continues from a parent span via distributed tracing headers.
- **Sampled**: The transaction will be fully traced and sent to the collector.
- **Unsampled**: The transaction will not be traced (minimal overhead, no data sent).

### Head-based Sampling

The Pinpoint C++ Agent uses **head-based sampling**, where the sampling decision is made when the **root span** (first span of a trace) is created. This decision is then propagated to all downstream services via trace context headers, ensuring the entire distributed trace is either collected or discarded as a whole — no partial traces.

### Sampling Decision Flow

When `NewSpan()` is called, the agent follows these steps in order:

1. **Agent enabled check** — if disabled, a no-op span is returned.
2. **URL filtering** — if the URL matches an exclusion pattern, a no-op span is returned.
3. **Method filtering** — if the HTTP method is excluded, a no-op span is returned.
4. **Parent sampling** — if the parent explicitly said "don't sample" (header `Pinpoint-Sampled: s0`), an unsampled span is returned. The child's own sampling configuration is ignored.
5. **Trace context check**:
   - If `Pinpoint-TraceID` header exists → **continue transaction** — apply continue-transaction sampling logic.
   - If `Pinpoint-TraceID` header does not exist → **new transaction** — apply the configured sampling rate.

### Sampler Types

The agent supports three sampling strategies. The choice depends on your traffic volume and monitoring objectives.

#### 1. CounterSampler (Constant Rate)

Samples 1 out of every N transactions using an atomic counter. Simple and predictable.

- `CounterRate: 1` — sample **every** transaction (for development/debugging).
- `CounterRate: 10` — sample **1 in 10** transactions (10%).
- `CounterRate: 0` — sample **no** transactions (effectively disables tracing).

```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 10  # Sample 1 out of every 10 new transactions
```

#### 2. PercentSampler (Probabilistic)

Samples a configured percentage of transactions. Provides a more uniform distribution than counter sampling, especially under varying traffic patterns.

- `PercentRate: 100` — sample all transactions.
- `PercentRate: 10.0` — sample approximately 10% of transactions.
- Range: clamped to `[0.01, 100]`.

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 10.0  # Sample 10% of transactions
```

#### 3. ThroughputLimitTraceSampler (Adaptive / Throughput-based)

Wraps a base sampler (Counter or Percent) with per-second rate limiters. This is the most production-friendly option for high-traffic environments, as it dynamically caps the volume of collected traces regardless of traffic spikes.

The sampler applies two stages of filtering for **new transactions**: first the base sampler decides, then the rate limiter caps the throughput. For **continue transactions**, only the rate limiter applies (continue transactions follow the parent's decision by default).

- `NewThroughput` — maximum new transactions sampled per second. `0` = unlimited.
- `ContinueThroughput` — maximum continue transactions sampled per second. `0` = unlimited.

The throughput limiter is automatically enabled when either `NewThroughput` or `ContinueThroughput` is set to a value greater than 0.

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 100.0       # Base: accept all
  NewThroughput: 100       # Then cap new transactions at 100/sec
  ContinueThroughput: 200  # Cap continue transactions at 200/sec
```

### Sampling Decision Matrix

| Condition | Transaction Type | Result |
|---|---|---|
| Agent disabled | — | NoopSpan (no tracing) |
| URL excluded | — | NoopSpan (no tracing) |
| Method excluded | — | NoopSpan (no tracing) |
| Parent says `s0` | Continue | UnsampledSpan (follows parent) |
| No trace ID in headers | New | Apply `isNewSampled()` logic |
| Trace ID exists in headers | Continue | Apply `isContinueSampled()` logic |

### Distributed Tracing Behavior

In a distributed system, sampling decisions flow from the root service to all downstream services:

- The **root service** (no parent trace context) makes the initial sampling decision based on its configured sampler.
- **Downstream services** receiving a request with a trace ID will **continue** the transaction. By default, continue transactions are always sampled (BasicTraceSampler) or subject to the continue throughput limiter (ThroughputLimitTraceSampler).
- If the parent explicitly marks the trace as unsampled (`Pinpoint-Sampled: s0`), all downstream services will skip sampling regardless of their own configuration.

### Configuration Examples

**Development** — sample all transactions for full visibility:

```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 1
```

**Production (low traffic)** — percentage-based sampling:

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 20.0  # 20% sampling
```

**Production (high traffic)** — throughput-limited sampling to control data volume:

```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 100.0
  NewThroughput: 100
  ContinueThroughput: 200
```

**Troubleshooting** — temporarily sample everything to diagnose issues:

```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 1  # Sample all, revert after debugging
```

### Sampling Best Practices

- **Balance is key**: too low a rate risks missing critical issues; too high a rate incurs performance overhead and storage costs.
- **Let the root service control sampling**: continue transactions automatically follow the parent's decision.
- **Use throughput limiting in production**: it protects the collector and storage from traffic spikes.
- **Check `span->IsSampled()` before expensive work**: skip heavy data collection (e.g., large payloads, detailed annotations) for unsampled traces to minimize overhead.
- **Don't set parent sampling to `s0` unless intentional**: this suppresses tracing for the entire downstream call chain.

For the full configuration reference, see [Configuration Guide — Sampling](config.md).

---

## 12. HTTP Filtering and Header Recording

### URL Filtering

Exclude URL patterns (health checks, static files, etc.) from tracing. Patterns support Ant-style wildcards (`*` for single path segment, `**` for multiple segments):

```yaml
Http:
  Server:
    ExcludeUrl:
      - "/health"
      - "/static/**"
      - "*.css"
```

### HTTP Method Filtering

Exclude specific HTTP methods from being traced:

```yaml
Http:
  Server:
    ExcludeMethod:
      - "OPTIONS"
      - "HEAD"
```

### Header Recording

Configure which headers to capture for server-side and client-side separately:

```yaml
Http:
  Server:
    RecordRequestHeader:
      - "User-Agent"
      - "Referer"
    RecordResponseHeader:
      - "Content-Type"
  Client:
    RecordRequestHeader:
      - "User-Agent"
```

Use `"HEADERS-ALL"` to record all headers (debug only — may produce large payloads).

In code, use `RecordHeader` with a `HeaderReader` implementation:

```cpp
HttpHeaderReader request_headers(req);
span->RecordHeader(pinpoint::HTTP_REQUEST, request_headers);

HttpHeaderReader response_headers(res);
span->RecordHeader(pinpoint::HTTP_RESPONSE, response_headers);
```

---

## 13. Best Practices

### Always End Spans and Events

Use RAII or structured `try`/`catch` to guarantee cleanup:

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

### Check Sampling Before Expensive Operations

```cpp
if (span->IsSampled()) {
    collectDetailedMetrics();
    addExtensiveAnnotations();
}
```

### Use Thread-Local Storage for Context

Pass span context implicitly using TLS to avoid polluting function signatures:

```cpp
thread_local pinpoint::SpanPtr current_span;

void setSpan(pinpoint::SpanPtr span) { current_span = span; }
pinpoint::SpanPtr getSpan() { return current_span; }

void processData() {
    auto span = getSpan();
    if (span) {
        span->NewSpanEvent("processData");
        // ...
        span->EndSpanEvent();
    }
}
```

### Sanitize Sensitive Data

Never log sensitive information in SQL, annotations, or headers:

```cpp
// DON'T: Record sensitive data
// span_event->SetSqlQuery(sql, password);

// DO: Sanitize or omit sensitive data
span_event->SetSqlQuery(sql, "[REDACTED]");
```

### Use Appropriate Service Types

Correct `SERVICE_TYPE_*` values improve UI rendering and filtering:

```cpp
pinpoint::SERVICE_TYPE_MYSQL_QUERY       // database
pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT   // HTTP client
pinpoint::SERVICE_TYPE_REDIS             // Redis cache
pinpoint::SERVICE_TYPE_CPP_FUNC          // custom function
```

### Minimize Performance Impact

- Use sampling in production (not 100%).
- Avoid creating too many span events for trivial operations.
- Don't add excessive annotations.
- Check `IsSampled()` before expensive data collection.

### Shutdown Cleanly

Always call `agent->Shutdown()` on application exit to flush pending spans:

```cpp
server.listen("localhost", 8089);
if (agent) {
    agent->Shutdown();
}
```

---

## 14. Troubleshooting

### Spans Not Appearing

- Confirm the agent is enabled and configuration is loaded.
- Check that `EndSpan()` is called for every created span.
- Verify sampling rate is greater than zero.
- Ensure collector host/port and network connectivity are correct.
- Set `Log.Level: "debug"` and review startup logs.

### High Memory Usage

- Reduce `Span.QueueSize` (e.g., `512`).
- Lower `Span.MaxEventSequence` (e.g., `1000`).
- Reduce `Http.UrlStatLimit` (e.g., `512`).

### Performance Issues

- Lower the sampling rate or use throughput-limited sampling.
- Avoid creating extremely fine-grained span events for trivial work.
- Reduce the volume and size of annotations and recorded headers.
- Disable `CollectUrlStat` and `EnableSqlStats` if not needed.

For more detailed troubleshooting, see the [Troubleshooting Guide](trouble_shooting.md).

### Distributed Tracing Not Working

- Verify trace headers are **both** injected on the client and extracted on the server.
- Confirm `TraceContextReader`/`Writer` implementations use the exact header names expected by the agent.
- Check for gateways or proxies that might strip or rewrite Pinpoint headers.
- Ensure header name matching is case-sensitive as required.

---

## 15. Checklist for New Instrumentation

1. **Initialize the agent** — configure via YAML and/or environment variables, then call `CreateAgent()`.
2. **Create a span** — on each incoming request or logical unit of work, call `NewSpan(...)`.
3. **Record metadata** — set remote address, endpoint, service type, and critical attributes.
4. **Add span events** — wrap key internal operations (DB, HTTP client, cache, business logic).
5. **Attach annotations** — record useful, non-sensitive details (URLs, status codes, IDs).
6. **Propagate context** — implement and use `TraceContextReader`/`Writer` for all outgoing/incoming calls.
7. **Handle errors** — use `SetError` (optionally with a `CallStackReader`) on spans or span events.
8. **Support async work** — use shared `SpanPtr` carefully or `NewAsyncSpan()` for background tasks.
9. **Respect sampling and filters** — configure sampling, URL/method filters, and header recording.
10. **Shutdown cleanly** — call `agent->Shutdown()` to flush spans on application exit.

---

## Related Documentation

- [Configuration Guide](config.md)
- [Quick Start Guide](quick_start.md)
- [Troubleshooting Guide](trouble_shooting.md)
- Complete examples: see the `example/` directory (`http_server.cpp`, `web_demo.cpp`, `db_demo.cpp`)
- API header: `include/pinpoint/tracer.h`
- GitHub: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent)
- Pinpoint APM Docs: [https://pinpoint-apm.github.io/pinpoint/](https://pinpoint-apm.github.io/pinpoint/)

---

*Apache License 2.0 — See [LICENSE](../LICENSE) for details.*
