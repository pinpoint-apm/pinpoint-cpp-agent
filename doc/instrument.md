# Pinpoint C++ Agent - Instrumentation Guide

This document is the consolidated reference for instrumenting C++ applications with the Pinpoint C++ agent (`pinpoint-cpp-agent`). It covers the full tracer API declared in `include/pinpoint/tracer.h`, from bootstrapping through production best practices.

The C++ tracer API mirrors the design of the [Pinpoint Go agent](https://github.com/pinpoint-apm/pinpoint-go-agent). For conceptual background, see the Go agent's [quick start](https://github.com/pinpoint-apm/pinpoint-go-agent/blob/main/doc/quick_start.md) and [instrumentation guide](https://github.com/pinpoint-apm/pinpoint-go-agent/blob/main/doc/instrument.md).

**Target readers**: C++ service owners and library authors who want to add Pinpoint tracing.

---

## 1. Core Concepts

Pinpoint models each transaction as a collection of **spans**.

| Type | Description |
|---|---|
| `Agent` | Entry point that manages configuration, span creation, and shutdown lifecycle. |
| `Span` | A top-level operation — HTTP request, gRPC method, scheduled job, worker iteration. Records timing, status and metadata, carries a trace ID and span ID, and holds a stack of span events. |
| `SpanEvent` | An operation *inside* a span (DB query, HTTP client call, function block). Multiple events form the call-stack view in the Pinpoint UI. |
| Annotation | Key/value metadata on a span or span event: URLs, status codes, customer IDs, sanitized SQL parameters. |
| `SqlBindValue` | Variant scalar accepted as a SQL bind argument. |
| `TraceContextReader` / `TraceContextWriter` | Context propagation adapters. Trace context (trace ID, span ID, sampling decision) travels between services in transport-specific headers. |
| `HeaderReader` | Structured access to HTTP headers for recording request/response metadata. Derives from `TraceContextReader`, so one implementation covers both roles. |
| `HeaderReaderWriter` | `HeaderReader` plus `TraceContextWriter`, for carriers you both read and write. |
| `CallStackReader` | Optional stack trace provider for enriched error reporting. |

Free helpers (`StartAgent`, `GlobalAgent`) and the `AgentOptions` struct make configuration and agent bootstrapping convenient.

---

## 2. Bootstrapping the Agent

Before you can create spans, you must start an `Agent` instance with `StartAgent()`. Configuration sources and identity inputs are collected in an `AgentOptions` struct: a config file path, an inline YAML string, and environment variables (which override individual settings from either source).

`StartAgent()` creates, configures and starts the agent in the **current process** and installs it as the global agent. Call it in the process that records spans — for pre-fork servers (nginx, Apache prefork, uWSGI) that means each worker calls it after `fork()`, and the master makes no agent API calls at all; see the [Pre-fork Integration Guide](prefork.md).

> **The startup contract — asynchronous registration, what a `false` return means,
> why `Enable()` is not a startup check, and how `Enable: false` takes the same
> return path — is documented once in
> [Verifying Agent Startup](trouble_shooting.md#verifying-agent-startup).** Read it
> before wiring `StartAgent()` into your application's error handling.

### Starting an Agent

```cpp
#include "pinpoint/tracer.h"

int main() {
    pinpoint::AgentOptions options;
    options.config_file_path = "/path/to/pinpoint-config.yaml";
    // Or supply inline YAML via options.config_yaml, or rely on
    // environment variables alone (StartAgent() with no arguments).

    // options.app_type defaults to APP_TYPE_CPP.
    if (!pinpoint::StartAgent(options)) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    // Your application logic ...

    agent->Shutdown();  // flush pending data and stop worker threads
    return 0;
}
```

`Shutdown()` is terminal for that agent instance: the same handle can never come
back online and only produces noop spans from then on. To stop and later resume
tracing in a long-running process, build a new agent with `StartAgent()` for each
cycle — see
[Stopping and Resuming the Agent](trouble_shooting.md#stopping-and-resuming-the-agent).

### Sending AgentInfo Metadata

`AgentOptions` can include server runtime metadata that is sent with AgentInfo.
All fields are optional: `app_type` defaults to `APP_TYPE_CPP` and `server_info`
to `"C/C++ Application"`.

```cpp
pinpoint::AgentOptions options;
options.server_info = "my-service-runtime";
options.args = {"--port=8080"};
options.libs = {"my-http-framework/1.2.3"};

pinpoint::StartAgent(options);
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

`Enable()` is **not** a startup success check — see
[Verifying Agent Startup](trouble_shooting.md#verifying-agent-startup). Use it for
one purpose only: as a **fast-fail guard before creating a span**, to skip
instrumentation work (span creation, header capture, context extraction) while
tracing is off. The request itself is served normally either way:

```cpp
void handleRequest(const Request& req) {
    auto agent = pinpoint::GlobalAgent();
    if (!agent->Enable()) {
        process(req);   // tracing is off — skip the instrumentation, serve as usual
        return;
    }

    auto span = agent->NewSpan("MyOperation", "/api/endpoint");
    process(req);
    span->EndSpan();
}
```

### Environment Variable Configuration

You can also configure via environment variables before calling `StartAgent()`:

```cpp
setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-web-demo", 0);
setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);

pinpoint::StartAgent();
auto agent = pinpoint::GlobalAgent();
```

---

## 3. Creating Spans for Incoming Work

Create a span at the **entry point** of each transaction: HTTP/gRPC server handler, message consumer callback, scheduled job, or worker loop iteration.

### Span Creation Methods

There are three overloads:

- `Agent::NewSpan(operation, rpc_point)` — starts a **new** transaction.
- `Agent::NewSpan(operation, rpc_point, TraceContextReader& reader)` — continues a transaction when upstream trace headers are present. Falls back to a new transaction if headers are missing.
- `Agent::NewSpan(operation, rpc_point, method, TraceContextReader& reader)` — same, plus the HTTP method, so the `Http.Server.ExcludeMethod` filter can apply. Prefer this one in HTTP servers.

A `HeaderReader` is also a `TraceContextReader`, so one object serves both the
context extraction and the header recording. See [§8](#8-http-request-tracing)
for a complete server handler.

### Setting Span Properties

```cpp
span->SetRemoteAddress("192.168.1.100");                 // client IP
span->SetEndPoint("localhost:8080");                     // logical endpoint
span->SetAcceptorHost("api.example.com");                // Host header of the accepted request
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
- Prefer RAII or `try`/`catch` to guarantee `EndSpan()` is executed — see the guard in [§6.2](#62-end-exactly-once-and-record-before-ending).
- Use descriptive `operation` and `rpc_point` names (e.g., `"C++ Web Demo"`, `"/users/:id"`).

### Thread-Local Storage for Span Context

A `thread_local pinpoint::SpanPtr` lets nested helpers reach the span without
threading it through every signature. A span is single-threaded
([§6.1](#61-a-span-is-single-threaded)), so a thread-local is a natural fit —
but clear it when the request ends (see the wrapper in [§8](#8-http-request-tracing)).

---

## 4. Recording Span Events

Span events describe important operations inside a span: HTTP/RPC client calls, database queries, cache operations, and function blocks. They form the call-stack view in the Pinpoint UI.

### Basic Usage

```cpp
auto span = agent->NewSpan("MyService", "/api/endpoint", reader);

// Start a span event
auto se = span->NewSpanEvent("process_logic");

// ... execute logic ...

// End the span event on its own handle
se->EndEvent();

span->EndSpan();
```

### Nested Span Events

Nest events to reflect the call hierarchy:

```cpp
auto processRequest = span->NewSpanEvent("processRequest");   // level 1
{
    auto businessLogic = span->NewSpanEvent("businessLogic"); // level 2
    {
        auto query = span->NewSpanEvent("queryDatabase");     // level 3
        queryDatabase();
        query->EndEvent();
    }
    businessLogic->EndEvent();
}
processRequest->EndEvent();
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

span_event->EndEvent();
```

`span->GetSpanEvent()` returns the innermost active event when you need it
without threading the handle through — see [§6.6](#66-getspanevent-returns-the-innermost-active-event).

### RAII Helper: `helper::ScopedSpanEvent`

`helper::ScopedSpanEvent` calls `EndEvent()` when it goes out of scope — including
on an exception — so events can never dangle. Access the underlying `SpanEvent`
through `operator->` or `value()`:

```cpp
void queryDatabase(pinpoint::SpanPtr span) {
    pinpoint::helper::ScopedSpanEvent guard(span, "SQL_SELECT", pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    guard->SetEndPoint("mysql-server:3306");
    guard->SetDestination("user_database");

    database->execute("SELECT * FROM users");   // no EndEvent() needed
}
```

### Recommendations

- Create **one span event per major logical step**; avoid an event per trivial
  function — it costs overhead and clutters the UI.
- Always pair `NewSpanEvent()` with `EndEvent()` on the returned event — prefer `helper::ScopedSpanEvent` for automatic cleanup.
- Use appropriate `SERVICE_TYPE_*` constants for downstream services: correct
  values improve UI rendering and filtering.
- Call `EndEvent()` in the same scope or via RAII wrappers to avoid dangling events. Ending an already-ended event is a warning no-op.

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

### Annotation Values

`Span::SetAnnotation()` and `SpanEvent::SetAnnotation()` provide typed overloads for the four supported payload shapes:

| C++ value | Collector payload |
|---|---|
| `int32_t` | int |
| `int64_t` | long |
| `std::string_view` | string |
| two `std::string_view` values | string + string |

Richer collector-side formats (SQL ids, proxy-header metadata, ...) are recorded internally by the agent itself and are not part of the public API.

### Adding Annotations

```cpp
// On a span
span->SetAnnotation(pinpoint::ANNOTATION_API, "getUserById");
span->SetAnnotation(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
span->SetAnnotation(12345, int64_t{1234567890});   // long
span->SetAnnotation(100, "key", "value");          // string pair

// On a span event
auto se = span->NewSpanEvent("external_call");
se->SetServiceType(pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
se->SetEndPoint("localhost:9000");
se->SetAnnotation(pinpoint::ANNOTATION_HTTP_URL, url);
se->SetAnnotation(pinpoint::ANNOTATION_HTTP_STATUS_CODE, status_code);
se->EndEvent();
```

### Custom Annotations

Define your own keys using high integer values to avoid conflicts with predefined keys:

```cpp
constexpr int32_t CUSTOM_USER_ID    = 10000;
constexpr int32_t CUSTOM_SESSION_ID = 10001;
constexpr int32_t CUSTOM_CACHE_HIT  = 10002;

span->SetAnnotation(CUSTOM_USER_ID, "user-123");
span->SetAnnotation(CUSTOM_SESSION_ID, "session-456");
span->SetAnnotation(CUSTOM_CACHE_HIT, 1);  // 1 = hit, 0 = miss
```

**Guideline**: Carefully sanitize annotations so that sensitive data (passwords, secrets, PII) is never recorded. The same applies to SQL bind values — pass
`"[REDACTED]"` rather than the real value when it may be sensitive.

---

## 6. Usage Cautions: Span, SpanEvent, and Annotation Contracts

This section collects the API contracts enforced by the span, span event, and annotation implementations. Most violations are detected at runtime and degrade to a **logged no-op** rather than a crash — but they distort traces, and the threading and lifetime rules below are hard requirements that can crash the process if broken. Treat the warning messages quoted here as instrumentation bugs when they appear in the agent log.

### 6.1 A Span Is Single-Threaded

A `Span` instance — including every `SpanEvent` it hands out — must be used by **one thread only** for its entire lifetime. Nothing inside a span is locked (the event stack, string fields, annotation lists), so concurrent calls on the same span are undefined behavior and can corrupt memory or crash.

- The agent binds a span to the first thread that calls `NewSpanEvent()` and logs an error (plus an `assert` in debug builds) when another thread touches it afterwards: `span accessed from another thread`.
- Because binding is lazy, a **complete handoff** is allowed: create the span on thread A, pass the `SpanPtr` to thread B, and never touch it from A again (the thread examples in [§11](#11-asynchronous-and-background-work) rely on this).
- To trace work that runs **concurrently** with the parent, do not share the span. Call `NewAsyncSpan()` *on the span's owning thread* and hand the returned child span to the worker; the child follows the same single-thread rule on its own thread.

### 6.2 End Exactly Once, and Record Before Ending

`EndSpan()` and `EndEvent()` are terminal:

- A duplicate `EndSpan()`/`EndEvent()` logs `span (event) is already finished` and does nothing.
- After the end call, **every recording method** on that object becomes a warning no-op: property setters, `SetError`, `RecordHeader`, `SetSqlQuery`, `InjectContext`, and `SetAnnotation()`. The data may already be in flight on the agent's gRPC worker thread, so nothing can be added afterwards. Record status codes, errors, and annotations **before** calling `EndSpan()`/`EndEvent()`.
- A span released without `EndSpan()` is **never sent** — its data is lost. The destructor only cleans up internal bookkeeping; it does not submit the span.

This is why RAII guards are the recommended pattern:
`helper::ScopedSpanEvent` for events (see [§4](#4-recording-span-events)), and for
spans a guard of your own:

```cpp
class SpanGuard {
public:
    explicit SpanGuard(pinpoint::SpanPtr span) : span_(std::move(span)) {}
    ~SpanGuard() { if (span_) span_->EndSpan(); }
private:
    pinpoint::SpanPtr span_;
};

void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("Service", "/endpoint");
    SpanGuard guard(span);

    // Even if an exception is thrown, the span is ended
    processRequest();
}
```

### 6.3 End Span Events in Nesting (LIFO) Order

Span events form a stack. Calling `EndEvent()` on an outer event while an inner event is still open implicitly finishes every event nested above it and logs `span event ended out of order`. Likewise, `EndSpan()` force-finishes all still-open events and logs `N span event(s) not ended by user code`. The trace survives, but implicitly finished events get the wrong end time — their duration silently stretches to the enclosing end call.

### 6.4 `SpanEventPtr` Is Non-Owning

`SpanEventPtr` is a raw pointer whose object is owned by the parent span:

- It stays valid only while you hold the parent `SpanPtr`. Calling into an already-ended event while the span is alive is a safe warning no-op; calling through a pointer that **outlives the span** is a use-after-free.
- Do not cache these pointers in long-lived structures. Obtain them, use them, and let them go within the span's scope.

### 6.5 Event Depth and Count Limits (Overflow)

Per span, event nesting depth is capped by `Span.MaxEventDepth` (default 64) and the total event count by `Span.MaxEventSequence` (default 5000). When either cap is reached, `NewSpanEvent()` logs `span event maximum depth/sequence exceeded` and returns a shared **disabled event** instead:

- It records nothing — operation name, timings, SQL, errors, and annotations are discarded.
- `InjectContext()` **still writes the full trace context**, so downstream services continue the distributed trace. Overflow limits profiling detail; it is not a sampling decision.
- You must still call `EndEvent()` exactly once for each overflowed `NewSpanEvent()` call — the span balances an internal overflow counter with it.
- The disabled event is a single shared object per span, so `SetDestination()` values from interleaved overflowed calls can bleed into each other's `Pinpoint-Host` header.
- `NewAsyncSpan()` called while the span is overflowed returns a no-op span.

If the overflow warning appears regularly, create fewer, coarser span events per transaction or raise the limits in the configuration.

### 6.6 `GetSpanEvent()` Returns the Innermost Active Event

`GetSpanEvent()` returns the top of the event stack: the most recently created event that has not ended. It never returns null — when the span is finished or has no active event, it returns a shared no-op event and logs `abnormal span - has no event`. Do not assume it refers to a specific event you created earlier; in helper functions, prefer passing the `SpanEventPtr` returned by `NewSpanEvent()` explicitly.

### 6.7 Annotation Rules

- The annotation list is **sealed** when its owner ends (`EndEvent()`/`EndSpan()`). A later `SetAnnotation()` logs a warning and does nothing.
- String views are **consumed and copied during the call** and do not need to outlive it. No annotation payload string is materialized for a no-op, unsampled, or already-ended span/event.
- `SetAnnotation()` never throws; on allocation failure the annotation is dropped with an error log.
- There is no key de-duplication: recording the same key twice records two annotations.
- Every annotation byte is copied into the span and shipped to the collector — keep annotations small and sanitized (see [§5](#5-annotations)).

### 6.8 Keep Operation and Error Names Low-Cardinality

The `operation` passed to `NewSpan()`/`NewSpanEvent()`/`NewAsyncSpan()` and the `error_name` passed to `SetError()` are interned in bounded LRU caches, and **every new unique string enqueues a metadata message to the collector**. Per-request unique names churn the cache and flood the collector with metadata:

```cpp
// DON'T: unique operation name per request
auto se = span->NewSpanEvent("getUser-" + user_id);

// DO: fixed operation name, variable data as an annotation
auto se = span->NewSpanEvent("getUser");
se->SetAnnotation(CUSTOM_USER_ID, user_id);
```

The `rpc_point` argument of `NewSpan()` is not interned — it may safely carry the actual request path.

### 6.9 Error Recording and Exception Buffering

- `SetError()` on the **span** marks the whole transaction as failed; `SetError()` on a **span event** marks only that step. Record at the granularity of the failure.
- The call-stack overload `SetError(name, message, CallStackReader&)` exists only on `SpanEvent`, and records frames only when `EnableCallstackTrace: true` is set in the configuration (default `false`).
- At most **100 exceptions with call stacks are buffered per span**; further ones are dropped. Buffered exceptions are transmitted only at `EndSpan()` — a span kept open for a very long time delays them and grows memory.

### 6.10 Clock and `SetStartTime()` Caveats

Elapsed times travel as **int32 milliseconds** on the wire. If you override timestamps with `SetStartTime()`:

- Only pass values derived from `std::chrono::system_clock::now()` taken at the actual start of the operation.
- A start time more than ~24.8 days in the past overflows the elapsed field; a start time in the future is clamped to an elapsed of 0 at end time, but inter-event offsets within a chunk can still wrap.
- A fabricated `time_point` (e.g. built from epoch **seconds** interpreted as milliseconds) produces wrapped, meaningless timings.

### 6.11 Noop and Unsampled Spans Are Deliberately Silent

`NewSpan()` never returns null. When the agent is disabled or not started, the URL/method is excluded by filters, or sampling rejects the transaction, you receive a no-op or unsampled span on which every call succeeds and records nothing:

- `IsSampled()` returns `false`, `GetTraceId()` returns an empty string, and `GetSpanId()` returns 0 for no-op spans (unsampled spans do carry a real span id).
- Use `IsSampled()` to skip *expensive data collection only* — do **not** skip creating span events and calling `InjectContext()` on outbound calls. An unsampled span's event still writes `Pinpoint-Sampled: s0`, which tells downstream services not to trace the request. Skipping the injection makes downstream agents treat the call as a brand-new transaction and sample it, producing broken partial traces.

---

## 7. Distributed Tracing and Context Propagation

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
pinpoint::HEADER_PARENT_APP_NAMESPACE // "Pinpoint-pAppNamespace"
pinpoint::HEADER_PARENT_SERVICE_NAME  // "Pinpoint-pServiceName"
pinpoint::HEADER_HOST              // "Pinpoint-Host"
```

### Server Side: Extracting Context

When receiving a request, pass a `TraceContextReader` to `NewSpan()`; it extracts
the upstream context if present and starts a new trace otherwise:

```cpp
HttpHeaderReader reader(req.headers);
auto span = agent->NewSpan("Server", req.path, req.method, reader);
```

### Client Side: Injecting Context

When making an outgoing call, create a span event for the call and inject the
trace context into the request headers through that event:

```cpp
void sendRequest(pinpoint::SpanPtr span) {
    httplib::Client cli("localhost", 8080);
    httplib::Headers headers;

    auto se = span->NewSpanEvent("outgoing-call",
                                 pinpoint::SERVICE_TYPE_CPP_HTTP_CLIENT);
    HttpHeaderReaderWriter writer(headers);
    se->InjectContext(writer);  // adds Pinpoint-* headers

    auto res = cli.Get("/target", headers);
    se->EndEvent();
}
```

### Implementing Custom Adapters

The carriers above are your own adapters over your HTTP library's header map.
Working implementations for `cpp-httplib` ship with the repository — read and copy
[`example/http_trace_context.h`](../example/http_trace_context.h):

| Class | Implements | Used for |
|---|---|---|
| `HttpHeaderReader` | `pinpoint::HeaderReader` (⊃ `TraceContextReader`) | `NewSpan(...)` context extraction and `RecordHeader()` |
| `HttpHeaderReaderWriter` | `pinpoint::HeaderReaderWriter` | the above plus `InjectContext()` on outbound calls |

To support a different framework or protocol, implement the same three methods
over your own header type:

- `std::optional<std::string_view> Get(std::string_view key) const` — the returned
  view must stay valid until the next call on this reader, so point into storage
  you own, never into a temporary.
- `void ForEach(std::function<bool(std::string_view, std::string_view)>) const` —
  iterate all headers; stop when the callback returns `false`.
- `void Set(std::string_view key, std::string_view value)` — writers only.

The same pattern applies to message queues, custom RPC frameworks, and binary
protocols by mapping trace keys to your own metadata format.

> **Header lookup must be case-insensitive.** The agent asks for the canonical
> spellings (`Pinpoint-TraceID`, ...), but proxies and HTTP/2 clients re-case
> header names. A reader backed by a case-sensitive `std::map` silently finds
> nothing and every request looks like a new transaction.

---

## 8. HTTP Request Tracing

### Tracing a Server Handler

The complete, compiling version of this pattern is
[`example/http_server.cpp`](../example/http_server.cpp); wrapping every handler
once keeps the tracing out of the business logic:

```cpp
#include "pinpoint/tracer.h"
#include "http_trace_context.h"

thread_local pinpoint::SpanPtr current_span;

httplib::Server::Handler wrap_handler(httplib::Server::Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        auto agent = pinpoint::GlobalAgent();
        HttpHeaderReader req_reader(req.headers);
        auto span = agent->NewSpan("C++ Web Server", req.path, req.method, req_reader);

        auto end_point = req.get_header_value("Host");
        if (end_point.empty()) {
            end_point = req.local_addr + ":" + std::to_string(req.local_port);
        }
        span->SetRemoteAddress(req.remote_addr);
        span->SetEndPoint(end_point);
        span->RecordHeader(pinpoint::HTTP_REQUEST, req_reader);

        current_span = span;   // handlers reach the span without a parameter

        try {
            handler(req, res);
        } catch (const std::exception& e) {
            span->SetError("HandlerError", e.what());
            res.status = 500;
        }

        HttpHeaderReader res_reader(res.headers);
        span->RecordHeader(pinpoint::HTTP_RESPONSE, res_reader);
        span->SetStatusCode(res.status);
        span->SetUrlStat(req.matched_route, req.method, res.status);
        span->EndSpan();
        current_span = nullptr;
    };
}
```

### HTTP Tracing Helpers

The `helper` namespace bundles the recording steps above into single calls:

```cpp
// Server request: remote address, endpoint, and request headers in one call
pinpoint::helper::TraceHttpServerRequest(span, req.remote_addr,
                                        req.get_header_value("Host"), req_reader);

// Server response: status code, URL stat, and response headers
pinpoint::helper::TraceHttpServerResponse(span, req.matched_route, req.method,
                                          res.status, res_reader);

// Client side, on the span event representing the outbound call
pinpoint::helper::TraceHttpClientRequest(se, "api.example.com", "/users", req_reader);
pinpoint::helper::TraceHttpClientResponse(se, res.status, res_reader);
```

| Function | Description |
|---|---|
| `TraceHttpServerRequest(span, remote_addr, endpoint, header_reader)` | Sets remote address, endpoint, and records request headers |
| `TraceHttpServerRequest(span, remote_addr, endpoint, header_reader, cookie_reader)` | Same as above, plus records cookies |
| `TraceHttpServerResponse(span, url_pattern, method, status_code, response_reader)` | Sets status code, URL stat, and records response headers |
| `TraceHttpClientRequest(span_event, host, url, header_reader)` | Sets endpoint, destination, and records request headers |
| `TraceHttpClientRequest(span_event, host, url, header_reader, cookie_reader)` | Same as above, plus records cookies |
| `TraceHttpClientResponse(span_event, status_code, response_reader)` | Records status code and response headers |

### URL Statistics

```cpp
span->SetUrlStat("/users/:id", "GET", 200);
```

This collects statistics normalized by URL pattern, HTTP method, and response status code. Enable it with `Http.CollectUrlStat`.

---

## 9. Database and Backend Instrumentation

Database, cache, and other backend calls are represented as span events with appropriate service types and annotations.

### Tracing SQL Queries

```cpp
void executeQuery(pinpoint::SpanPtr span,
                  const std::string& sql,
                  const std::vector<std::string>& params) {
    auto db_event = span->NewSpanEvent("SQL_SELECT");
    db_event->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    db_event->SetEndPoint("mysql-server:3306");
    db_event->SetDestination("user_database");

    try {
        auto stmt = session->sql(sql);
        for (const auto& param : params) {
            stmt.bind(param);
        }
        stmt.execute();

        // Pass {} to record the SQL text alone. Sanitize bind values first:
        // with Sql.TraceBindValue on, SetSqlQuery formats and joins them.
        std::vector<pinpoint::SqlBindValue> bind_values(params.begin(), params.end());
        db_event->SetSqlQuery(sql, bind_values);

    } catch (const std::exception& e) {
        db_event->SetError("SQL_ERROR", e.what());
    }

    db_event->EndEvent();
}
```

`SqlBindValue` accepts null, boolean, signed and unsigned integer, floating-point,
and `std::string_view` values. Values are converted to text and joined with commas
before the configured bind-value size limit is applied.

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
- See [`example/tutorial.cpp`](../example/tutorial.cpp) for a full working example.

---

## 10. Error Reporting and Stack Traces

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

Implement `CallStackReader` to capture and record stack traces. The call-stack overload is available on `SpanEvent`; for a span-level failure, set the span error and record the stack on the span event that represents the failed operation.

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

Frames are recorded only when `EnableCallstackTrace: true`.

### Exception Handling Pattern

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyService", "/api/endpoint");

    try {
        processRequest(span);
        span->SetStatusCode(200);

    } catch (const ValidationException& e) {
        span->SetError("ValidationError", e.what());
        span->SetStatusCode(400);

    } catch (const std::exception& e) {
        // Record the stack on a span event; SetError on the span marks the
        // transaction as failed.
        CppTraceCallStackReader stack_reader;
        span->SetError("UnexpectedError", e.what());
        auto error_event = span->NewSpanEvent("unexpected_error");
        error_event->SetError("UnexpectedError", e.what(), stack_reader);
        error_event->EndEvent();
        span->SetStatusCode(500);
    }

    span->EndSpan();   // on every path
}
```

Record errors at the granularity that is useful — often on the specific span
event representing the failed operation — and end that event and the span so the
failure lands at a precise point in the timeline.

---

## 11. Asynchronous and Background Work

Tracing asynchronous or background tasks requires careful span lifecycle management.

### Using Existing Spans with Threads

Since `SpanPtr` is a shared pointer, you can hand it off to a worker thread — as
long as the original thread never touches it again ([§6.1](#61-a-span-is-single-threaded)):

```cpp
void asyncWithThread(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("AsyncHandler", req.path);

    std::thread t([span]() {          // thread B owns the span from here on
        auto se = span->NewSpanEvent("outgoingRequest_thread");
        se->SetServiceType(pinpoint::SERVICE_TYPE_CPP_FUNC);
        outgoingRequest();
        se->EndEvent();
        span->EndSpan();
    });
    t.detach();                       // thread A never touches the span again
}
```

### Async Spans (Fire-and-Forget Work)

Use `Span::NewAsyncSpan()` to trace background tasks that run **concurrently
with** or continue after the original request:

```cpp
void safeAsyncOperation() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("Service", "/endpoint");

    // Create the async child on the parent's own thread, before the parent ends
    auto async_span = span->NewAsyncSpan("async_work");

    std::thread([async_span]() {
        try {
            auto event = async_span->NewSpanEvent("work");
            performWork();
            event->EndEvent();
        } catch (const std::exception& e) {
            async_span->SetError("AsyncError", e.what());
        }
        async_span->EndSpan();
    }).detach();

    span->EndSpan();  // main request finishes immediately
}
```

### Guidelines

- Each async span must be **ended exactly once**.
- Ensure the original span outlives any span events created on it.
- For long-running worker loops, create a **new span per iteration** rather than keeping a single span open indefinitely.

---

## 12. Sampling Policy

Tracing every request of every service costs overhead, bandwidth and storage.
Sampling collects a representative subset instead.

The agent uses **head-based sampling**: the decision is made when the **root
span** is created and propagated to all downstream services in the trace context
headers, so a distributed trace is collected or discarded as a whole — never
partially. A **new transaction** has no parent context; a **continue
transaction** arrives with a `Pinpoint-TraceID` header and follows the parent's
decision.

### Sampling Decision Flow

`NewSpan()` decides in this order:

| Condition | Transaction Type | Result |
|---|---|---|
| Agent disabled | — | NoopSpan (no tracing) |
| URL excluded | — | NoopSpan (no tracing) |
| Method excluded | — | NoopSpan (no tracing) |
| Parent says `Pinpoint-Sampled: s0` | Continue | UnsampledSpan — the child's own configuration is ignored |
| No trace ID in headers | New | Apply the configured sampler |
| Trace ID in headers | Continue | Sampled by default, subject to `ContinueThroughput` |

### Sampler Types

All three are configured under `Sampling.*` — see the
[Configuration Guide](config.md#sampling-configuration) for keys and ranges.

- **CounterSampler** (`Type: COUNTER`) — samples 1 out of every N transactions
  with an atomic counter. `CounterRate: 1` samples everything (development),
  `10` samples 10%, `0` samples nothing.
- **PercentSampler** (`Type: PERCENT`) — samples a configured percentage,
  deterministic for a given call sequence. `PercentRate` is clamped to
  `[0.01, 100]`.
- **Throughput limiting** — wraps whichever base sampler is configured with
  per-second caps, enabled automatically when `NewThroughput` or
  `ContinueThroughput` is greater than `0` (`0` = unlimited). New transactions
  pass the base sampler first and then the limiter; continue transactions face
  only the limiter. This is the production-friendly option: it caps trace volume
  regardless of traffic spikes.

### Sampling Best Practices

- **Let the root service control sampling** — continue transactions follow the
  parent, and a `s0` decision suppresses the entire downstream call chain.
- **Use throughput limiting in production** — it protects the collector and
  storage from traffic spikes.
- **Check `span->IsSampled()` before expensive work** — skip heavy data
  collection (large payloads, detailed annotations) on unsampled traces, but
  never skip `InjectContext()` ([§6.11](#611-noop-and-unsampled-spans-are-deliberately-silent)).

---

## 13. HTTP Filtering and Header Recording

### URL Filtering

Exclude URL patterns (health checks, static files, etc.) from tracing. Patterns support Ant-style wildcards: `?` matches one character, `*` matches any run of characters, and `**` matches across path segments. Neither `?` nor `*` crosses a `/`:

```yaml
Http:
  Server:
    ExcludeUrl:
      - "/health"
      - "/static/**"
      - "/**/*.css"
```

> A pattern must match the **whole** path, so it has to account for the leading
> `/`. A bare suffix pattern like `"*.css"` never matches `/static/main.css` —
> `*` cannot consume the `/` separators. Use `"/**/*.css"` for "any `.css` at
> any depth".

### HTTP Method Filtering

Exclude specific HTTP methods from being traced. This applies only when the span is created with the `method` overload of `NewSpan()`:

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

Use `"HEADERS-ALL"` to record all headers (debug only — may produce large payloads). Never record sensitive headers (`Authorization`, `Cookie`) in production.

In code, use `RecordHeader` with a `HeaderReader` implementation:

```cpp
HttpHeaderReader request_headers(req.headers);
span->RecordHeader(pinpoint::HTTP_REQUEST, request_headers);

HttpHeaderReader response_headers(res.headers);
span->RecordHeader(pinpoint::HTTP_RESPONSE, response_headers);
```

---

## 14. Troubleshooting

The [Troubleshooting Guide](trouble_shooting.md) owns the full diagnostic
procedure — agent startup, collector connectivity, memory and CPU, and the
commands to run. This section covers only the failure modes whose cause is in
**your instrumentation code**, and where to look for each.

| Symptom | Instrumentation causes to rule out first |
|---|---|
| Spans missing entirely | `EndSpan()` not reached on some path (an early `return`, an exception — use the RAII guard in [§6.2](#62-end-exactly-once-and-record-before-ending)). A span released without it is never sent. |
| Span appears, data missing | Recorded *after* `EndSpan()`/`EndEvent()`; every setter is a no-op past that point ([§6.2](#62-end-exactly-once-and-record-before-ending)). Check the agent log for `span (event) is already finished`. |
| Wrong durations, odd nesting | Events ended out of LIFO order, or not at all — the log says `span event ended out of order` / `N span event(s) not ended by user code` ([§6.3](#63-end-span-events-in-nesting-lifo-order)). |
| Deep call trees truncated | Depth/count overflow: `span event maximum depth/sequence exceeded` ([§6.5](#65-event-depth-and-count-limits-overflow)). Create coarser events or raise the limits. |
| Traces break at a service boundary | `InjectContext()` skipped on the outbound call — including on unsampled spans, where skipping it makes downstream agents start a brand-new trace ([§6.11](#611-noop-and-unsampled-spans-are-deliberately-silent)). Both sides must be wired: inject on the client, extract on the server ([§7](#7-distributed-tracing-and-context-propagation)). |
| Collector flooded with metadata | High-cardinality `operation` / `error_name` strings; put the variable part in an annotation instead ([§6.8](#68-keep-operation-and-error-names-low-cardinality)). |
| Crashes or corrupt traces under load | A span used from more than one thread — the log says `span accessed from another thread`. Hand off completely, or use `NewAsyncSpan()` ([§6.1](#61-a-span-is-single-threaded)). |

Set `Log.Level: "debug"`: the contract violations above are all logged, so the
agent log usually names the bug before you have to reason about it.

---

## Related Documentation

- [Configuration Guide](config.md) — every configuration option
- [Troubleshooting Guide](trouble_shooting.md) — startup contract and diagnostics
- [C API Guide](instrument_c.md) — the same API for plain C
- API header: [`include/pinpoint/tracer.h`](../include/pinpoint/tracer.h)
- Examples: [`example/`](../example/) (`http_server.cpp`, `tutorial.cpp`)
