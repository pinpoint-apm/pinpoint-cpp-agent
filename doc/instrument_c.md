# Pinpoint C++ Agent — C API Instrumentation Guide

This document covers instrumenting **plain C** applications with the Pinpoint C++ agent using the pure-C API declared in `include/pinpoint/tracer_c.h`.

If you are writing C++, prefer the richer C++ API documented in [instrument.md](instrument.md).

**Target readers**: C service owners and library authors who want to add Pinpoint distributed tracing without taking a C++ dependency.

---

## 1. Core Concepts

Pinpoint models each transaction as a tree of **spans**.

| Concept | Description |
|---|---|
| **Span** (`pt_span_t`) | Top-level trace segment for an incoming request, job, or logical unit of work. Carries a trace ID and span ID. |
| **SpanEvent** (`pt_span_event_t`) | A child operation inside a span (DB query, HTTP client call, function block). Multiple events form the call-stack view in the Pinpoint UI. |
| **Annotation** | Key/value metadata recorded on a span or span event via the typed `pt_span_set_annotation_*()` / `pt_span_event_set_annotation_*()` functions (URL, status code, etc.). |
| **Agent** (`pt_agent_t`) | Entry point that manages configuration, sampling, and span lifecycle. |

---

## 2. Key Differences from the C++ API

| Topic | C++ API (`tracer.h`) | C API (`tracer_c.h`) |
|---|---|---|
| Handle types | Smart pointers (`SpanPtr`) | Opaque pointers (`pt_span_t`) |
| Memory management | RAII / destructors | Owning handles use `_destroy()`; view handles are non-owning |
| Propagation carriers | Virtual base classes | Callback structs (`pt_context_reader_t`, etc.) |
| Span event lifecycle | `ScopedSpanEvent` helper available | Manual `pt_span_event_end(se)`; `pt_span_event_destroy(se)` is an optional no-op |
| Error handling | C++ exceptions | Return values / `pt_span_set_error()` |
| Header file | `include/pinpoint/tracer.h` | `include/pinpoint/tracer_c.h` |

### Handle lifetime rules

Agent and span handles own C wrapper storage and must be destroyed when you are done with them:

```c
pt_agent_t      → pt_agent_destroy()
pt_span_t       → pt_span_destroy()        (call pt_span_end() first)
```

`pt_agent_destroy()` releases the C handle wrapper. It does not shut down tracing by itself; call `pt_agent_shutdown()` only for an agent you intend to stop.

Span-event handles are non-owning views. They allocate no resources, and `pt_span_event_destroy()` is a safe no-op kept for API symmetry and compatibility. Use these handles only while their parent span is alive and active; do not read, mutate, or store them for work that can run after `pt_span_event_end()`, `pt_span_end()`, or `pt_span_destroy()`.

---

## 3. Bootstrapping the Agent

### Configuration

Configuration sources are collected in an options object (`pt_agent_options_t`) passed to `pt_start_agent()`. Environment variables override individual settings from either source.

```c
#include "pinpoint/tracer_c.h"

pt_agent_options_t opts = pt_agent_options_new();

/* Option 1: config file */
pt_agent_options_set_config_file(opts, "/etc/pinpoint/agent.yaml");

/* Option 2: inline YAML string (used when no config file is set) */
pt_agent_options_set_config_yaml(opts, "ApplicationName: my-c-service\n"
                                       "Collector:\n"
                                       "  Host: localhost\n");

/* Option 3: environment variables (set before calling pt_start_agent) */
setenv("PINPOINT_CPP_CONFIG_FILE",           "/tmp/pinpoint-config.yaml", 0);
setenv("PINPOINT_CPP_APPLICATION_NAME",      "my-c-service",              0);
setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true",                      0);
```

### Starting and destroying an agent

`pt_start_agent()` creates, configures and starts the agent in the **current
process** and installs it as the global agent. Call it in the process that
records spans — for pre-fork servers (nginx, Apache prefork) that means each
worker calls it after `fork()`, and the master makes no agent API calls at all;
see the [Pre-fork Integration Guide](prefork.md).

```c
int main(void) {
    pt_agent_options_t opts = pt_agent_options_new();
    pt_agent_options_set_config_file(opts, "/etc/pinpoint/agent.yaml");

    int started = pt_start_agent(opts);  /* NULL options = all defaults */
    pt_agent_options_free(opts);         /* only read during the call   */
    if (!started) {
        /* configuration/setup failure — the app still runs, just untraced */
        fprintf(stderr, "failed to start the pinpoint agent: check the agent log\n");
    }
    pt_agent_t agent = pt_global_agent();

    /* application logic ... */

    pt_agent_shutdown(agent);  /* flush pending spans */
    pt_agent_destroy(agent);   /* release the handle  */
    return 0;
}
```

> **The startup contract is documented once in
> [Verifying Agent Startup](trouble_shooting.md#verifying-agent-startup)**, and it
> applies here with C spellings: `pt_start_agent()` returns `0` for a
> *synchronous* configuration or setup failure (and a deliberate `Enable: false`
> takes that same path, writing nothing to the agent log), registration happens
> asynchronously, and `pt_agent_is_enabled()` returns `1` only after it succeeds.
> Verify startup through the agent log, never through the return value.

`pt_agent_shutdown()` is terminal for that agent: the same handle can never come
back online. To stop and later resume tracing, destroy the handle and run
`pt_start_agent()` again for each cycle — the full contract is in
[Stopping and Resuming the Agent](trouble_shooting.md#stopping-and-resuming-the-agent):

```c
pt_agent_shutdown(agent);
pt_agent_destroy(agent);          /* drop the dead handle */

/* ... later: a NEW agent; the old one cannot restart */
if (!pt_start_agent(NULL)) {
    fprintf(stderr, "failed to restart the pinpoint agent: check the agent log\n");
}
agent = pt_global_agent();
```

`pt_agent_options_set_server_metadata()` attaches AgentInfo server metadata
(runtime description, args, libs):

```c
const char* args[] = {"--port=8080"};
const char* libs[] = {"my-http-framework/1.2.3"};

pt_agent_options_t opts = pt_agent_options_new();
pt_agent_options_set_server_metadata(opts, "my-service-runtime", args, 1, libs, 1);
if (!pt_start_agent(opts)) {
    fprintf(stderr, "failed to start the pinpoint agent: check the agent log\n");
}
pt_agent_options_free(opts);
```

### Using the global agent

`pt_global_agent()` returns a C handle wrapper around the singleton agent that was installed by the most recent `pt_start_agent()` call. If no global agent exists, it returns a disabled no-op agent handle. Release the returned wrapper with `pt_agent_destroy()` when done. Do not call `pt_agent_shutdown()` from request-handling code unless you intentionally want to stop the process-wide agent.

```c
/* In a request handler, far from main() */
pt_agent_t agent = pt_global_agent();  /* wrapper around the global agent */

pt_span_t span = pt_agent_new_span(agent, "MyService", "/api/v1");
/* ... */
pt_span_end(span);
pt_span_destroy(span);

pt_agent_destroy(agent);  /* releases this C handle wrapper */
```

### Checking agent status

`pt_agent_is_enabled()` is **not a startup success check**. Use it for one purpose
only: as a **fast-fail guard before creating a span**, to skip instrumentation
work while tracing is off. The request is served normally either way:

```c
void handle_request(request_t* req) {
    pt_agent_t agent = pt_global_agent();
    if (!pt_agent_is_enabled(agent)) {
        pt_agent_destroy(agent);
        process(req);              /* tracing is off — skip instrumentation */
        return;
    }

    pt_span_t span = pt_agent_new_span(agent, "MyService", "/api/v1");
    process(req);
    pt_span_end(span);
    pt_span_destroy(span);
    pt_agent_destroy(agent);
}
```

---

## 4. Propagation Carriers

The C API represents header maps as **callback structs** rather than virtual C++ classes. You fill in function pointers that match your HTTP library's header access API.

### `pt_context_reader_t` — inbound context extraction

Used to extract an upstream trace context from incoming request headers.

```c
typedef struct {
    void*            userdata;  /* your header map pointer   */
    pt_reader_get_fn get;       /* fn(userdata, key) → value */
} pt_context_reader_t;
```

Example with a hypothetical `my_headers_t`:

```c
static const char* my_hdr_get(void* ud, const char* key) {
    return my_headers_get((my_headers_t*)ud, key);  /* NULL if absent */
}

pt_context_reader_t reader = {
    .userdata = &req->headers,
    .get      = my_hdr_get,
};
```

> **Header lookup must be case-insensitive.** The agent asks for the canonical
> spellings (`Pinpoint-TraceID`, ...), but proxies and HTTP/2 clients re-case
> header names; a case-sensitive lookup silently finds nothing and every request
> looks like a new transaction.

### `pt_context_writer_t` — outbound context injection

Used to inject trace context into outgoing request headers.

```c
typedef struct {
    void*            userdata;
    pt_writer_set_fn set;  /* fn(userdata, key, value) */
} pt_context_writer_t;
```

```c
static void my_hdr_set(void* ud, const char* key, const char* value) {
    my_headers_set((my_headers_t*)ud, key, value);
}

pt_context_writer_t writer = {
    .userdata = &out_headers,
    .set      = my_hdr_set,
};
pt_span_event_inject_context(se, &writer);
```

### `pt_header_reader_t` — full header access with iteration

Required wherever the agent needs to both look up and iterate headers (e.g., `pt_span_record_header()`, `pt_trace_http_server_request()`).

```c
typedef struct {
    void*                 userdata;
    pt_reader_get_fn      get;
    pt_header_for_each_fn for_each;  /* iterates all headers */
} pt_header_reader_t;
```

```c
static void my_hdr_for_each(void* ud, pt_header_foreach_cb cb, void* cb_ud) {
    my_headers_t* h = (my_headers_t*)ud;
    for (size_t i = 0; i < h->count; i++) {
        if (cb(h->keys[i], h->values[i], cb_ud) != 0)
            break;
    }
}

pt_header_reader_t hdr_reader = {
    .userdata = &req->headers,
    .get      = my_hdr_get,
    .for_each = my_hdr_for_each,
};
```

> **Tip**: If your HTTP library's `get` and `for_each` function signatures already match `pt_reader_get_fn` and `pt_header_for_each_fn`, assign them directly without writing adapter functions — see [`example/http_server_c.c`](../example/http_server_c.c) for a concrete example.

---

## 5. Creating Spans for Incoming Requests

Create a span at the entry point of each transaction: HTTP handler, message consumer, scheduled job, etc.

### New span (no upstream context)

```c
pt_span_t span = pt_agent_new_span(agent, "MyService", "/api/endpoint");
```

### Span continuing an upstream trace

```c
pt_context_reader_t reader = { &req->headers, my_hdr_get };

pt_span_t span = pt_agent_new_span_with_reader(
    agent, "MyService", req->path, &reader);
```

Pass `NULL` for `reader` if there are no inbound headers — the agent will start a new trace.

### Span with an HTTP method

```c
pt_span_t span = pt_agent_new_span_with_method(
    agent, "MyService", req->path, "GET", &reader);
```

### Setting span properties

```c
pt_span_set_remote_address(span, "192.168.1.100");  /* client IP        */
pt_span_set_end_point(span, "api.example.com:8080"); /* server endpoint  */
pt_span_set_acceptor_host(span, "api.example.com");  /* accepted Host    */
pt_span_set_service_type(span, PT_SERVICE_TYPE_CPP); /* service type     */
pt_span_set_status_code(span, 200);                  /* HTTP status code */
pt_span_set_start_time_ms(span, start_ms);           /* override start   */
```

### Ending and destroying a span

```c
pt_span_end(span);      /* flush all data to the collector */
pt_span_destroy(span);  /* release the handle              */
```

`pt_span_end()` must be called on **every** code path, including error paths.

### Inspecting span state

```c
pt_trace_id_t tid = pt_span_get_trace_id(span);
printf("agent_id=%s seq=%" PRId64 "\n", tid.agent_id, tid.sequence);

int64_t span_id = pt_span_get_span_id(span);

if (!pt_span_is_sampled(span)) {
    /* skip expensive annotation collection */
}
```

---

## 6. Recording Span Events

Span events represent operations inside a span. They form the call-stack view in the Pinpoint UI.

### Basic usage

```c
pt_span_event_t se = pt_span_new_event(span, "db_query");

/* ... perform the operation ... */

pt_span_event_end(se);        /* finalize the event — called on the EVENT handle */
pt_span_event_destroy(se);    /* optional compatibility no-op */
```

> `pt_span_event_end()` is called on the **event handle**, not on the parent span. After it returns, do not read or mutate the event handle. `pt_span_event_destroy()` is optional and does not extend the event lifetime.

### Creating an event with an explicit service type

```c
pt_span_event_t se = pt_span_new_event_with_type(
    span, "redis_get", PT_SERVICE_TYPE_REDIS);
```

### Setting span event properties

```c
pt_span_event_set_service_type(se, PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
pt_span_event_set_operation_name(se, "GET /downstream");
pt_span_event_set_end_point(se, "downstream-host:8090");
pt_span_event_set_destination(se, "downstream-host:8090");
pt_span_event_set_start_time_ms(se, start_ms);
```

### Nested span events

`pt_span_new_event()` pushes onto an internal stack; `pt_span_event_end(se)` finalizes and pops that event. End events in innermost-first order so pops stay balanced with pushes. Ending the same event twice is a warning no-op — it does not pop another event.

```c
pt_span_event_t e1 = pt_span_new_event(span, "outer");

    pt_span_event_t e2 = pt_span_new_event(span, "inner");
    pt_span_event_end(e2);     /* ends inner */
    pt_span_event_destroy(e2);

pt_span_event_end(e1);         /* ends outer */
pt_span_event_destroy(e1);
```

See `example/http_server_c.c` (`record_nested_events`) for an extended nesting example.

---

## 7. Annotations

Annotations attach structured key/value metadata to spans and span events. Each annotation is recorded directly on the span or span-event handle with a typed setter; the supported value shapes are int, long, string, and string+string.

### Predefined annotation key constants

```c
PT_ANNOTATION_HTTP_URL              /* 40  — HTTP URL string            */
PT_ANNOTATION_HTTP_STATUS_CODE      /* 46  — HTTP status (integer)      */
PT_ANNOTATION_HTTP_COOKIE           /* 45  — Cookie header string       */
PT_ANNOTATION_HTTP_REQUEST_HEADER   /* 47  — Recorded request headers   */
PT_ANNOTATION_HTTP_RESPONSE_HEADER  /* 55  — Recorded response headers  */
PT_ANNOTATION_API                   /* 12  — API/method name            */
PT_ANNOTATION_SQL_ID                /* 20  — SQL statement ID           */
PT_ANNOTATION_SQL_UID               /* 25  — SQL UID (binary)           */
PT_ANNOTATION_EXCEPTION_ID          /* -52 — Exception info             */
```

### Annotation value types

```c
pt_span_set_annotation_int(span, key, (int32_t)value);
pt_span_set_annotation_long(span, key, (int64_t)value);
pt_span_set_annotation_string(span, key, "string value");
pt_span_set_annotation_string_string(span, key, "s1", "s2");

/* same shapes on a span event: */
pt_span_event_set_annotation_int(se, key, (int32_t)value);
pt_span_event_set_annotation_long(se, key, (int64_t)value);
pt_span_event_set_annotation_string(se, key, "string value");
pt_span_event_set_annotation_string_string(se, key, "s1", "s2");
```

Richer collector-side formats (SQL ids/UIDs, proxy-header metadata, ...) are recorded internally by the agent itself — e.g. via `pt_span_event_set_sql_query()` — and are not part of the public API.

### Custom annotation keys

Use large positive integers to avoid collisions with predefined keys:

```c
#define MY_ANNOTATION_USER_ID    10000
#define MY_ANNOTATION_SESSION_ID 10001

pt_span_event_set_annotation_string(se, MY_ANNOTATION_USER_ID, user_id);
```

Never record passwords, secrets, or PII in annotations.

---

## 8. Usage Cautions: Span, SpanEvent, and Annotation Contracts

The C API wraps the same span, span event, and annotation implementations as the
C++ API, so **the contracts are identical** and are documented once in
[Instrumentation Guide §6](instrument.md#6-usage-cautions-span-spanevent-and-annotation-contracts).
Read that section — the rules there are not restated here.

Translate the C++ names in it as follows:

| §6 topic | C++ | C |
|---|---|---|
| [§6.1](instrument.md#61-a-span-is-single-threaded) single-threaded span | `NewSpanEvent()`, `NewAsyncSpan()` | `pt_span_new_event()`, `pt_span_new_async_span()` |
| [§6.2](instrument.md#62-end-exactly-once-and-record-before-ending) end exactly once | `EndSpan()`, `EndEvent()` | `pt_span_end()`, `pt_span_event_end()` |
| [§6.3](instrument.md#63-end-span-events-in-nesting-lifo-order) LIFO order | `EndEvent()` | `pt_span_event_end()` |
| [§6.4](instrument.md#64-spaneventptr-is-non-owning) non-owning event handles | `SpanEventPtr` | `pt_span_event_t` |
| [§6.5](instrument.md#65-event-depth-and-count-limits-overflow) overflow | `NewSpanEvent()` | `pt_span_new_event()` |
| [§6.6](instrument.md#66-getspanevent-returns-the-innermost-active-event) innermost event | `GetSpanEvent()` | `pt_span_get_event()` |
| [§6.7](instrument.md#67-annotation-rules) annotations | `SetAnnotation()` | `pt_span_*_set_annotation_*()` |
| [§6.8](instrument.md#68-keep-operation-and-error-names-low-cardinality) low cardinality | `SetError()` | `pt_span_set_error_named()` and friends |
| [§6.9](instrument.md#69-error-recording-and-exception-buffering) errors | `SetError(..., CallStackReader&)` | `pt_span_event_set_error_with_callstack()` |
| [§6.10](instrument.md#610-clock-and-setstarttime-caveats) clock | `SetStartTime(time_point)` | `pt_span_set_start_time_ms()` |
| [§6.11](instrument.md#611-noop-and-unsampled-spans-are-deliberately-silent) noop spans | `IsSampled()` | `pt_span_is_sampled()` |

Three rules are specific to the C handle model:

- **Destroy last, and only after every derived handle is out of use.** A span
  destroyed without `pt_span_end()` is never sent; `pt_span_destroy()` only
  releases the handle. Calling into an event handle *after the span is destroyed*
  is a use-after-free.
- **`pt_span_event_destroy()` does not end the event.** It is a compatibility
  no-op; forgetting `pt_span_event_end()` is not fixed by calling it.
- **`NULL` is tolerated, never a signal.** Passing `NULL` for an event handle is
  silently ignored, and creation functions return `NULL` only for a `NULL`/destroyed
  span or an internal failure — `pt_span_get_event()` returns a shared no-op event
  rather than `NULL` when no event is active, so it cannot be used to test for one.
  A crash on a `NULL` handle points at memory corruption, not at the agent.
- **`pt_span_end()`/`pt_span_destroy()` are safe and cheap on noop and unsampled
  handles** — keep the normal end/destroy flow unconditionally.

The `start_time_ms` arguments take **milliseconds** since the Unix epoch. Passing
seconds (e.g. `time(NULL)`) is not validated: the computed deltas overflow int32
and silently corrupt the trace timeline.

---

## 9. Distributed Tracing and Context Propagation

### Trace propagation headers

```c
PT_HEADER_TRACE_ID              /* "Pinpoint-TraceID"        */
PT_HEADER_SPAN_ID               /* "Pinpoint-SpanID"         */
PT_HEADER_PARENT_SPAN_ID        /* "Pinpoint-pSpanID"        */
PT_HEADER_SAMPLED               /* "Pinpoint-Sampled"        */
PT_HEADER_FLAG                  /* "Pinpoint-Flags"          */
PT_HEADER_PARENT_APP_NAME       /* "Pinpoint-pAppName"       */
PT_HEADER_PARENT_APP_TYPE       /* "Pinpoint-pAppType"       */
PT_HEADER_PARENT_APP_NAMESPACE  /* "Pinpoint-pAppNamespace"  */
PT_HEADER_PARENT_SERVICE_NAME   /* "Pinpoint-pServiceName"   */
PT_HEADER_HOST                  /* "Pinpoint-Host"           */
```

### Server side — extracting incoming context

Pass a `pt_context_reader_t` to `pt_agent_new_span_with_reader()`. If the inbound headers contain a trace ID, the span continues that trace; otherwise a new trace is started.

```c
pt_context_reader_t reader = { &req->headers, my_hdr_get };
pt_span_t span = pt_agent_new_span_with_reader(
    agent, "C Web Server", req->path, &reader);
```

### Client side — injecting outgoing context

Context is injected through the span event that represents the outbound call:

```c
/* Span event representing the outbound call */
pt_span_event_t se = pt_span_new_event(span, "HTTP_CLIENT");

/* Build outbound headers, then inject trace context */
my_headers_t out = my_headers_create();

pt_context_writer_t writer = { &out, my_hdr_set };
pt_span_event_inject_context(se, &writer);

/* Issue the outgoing request with out headers */
my_http_get(client, "/downstream", &out);
my_headers_destroy(&out);

pt_span_event_end(se);
pt_span_event_destroy(se);
```

See [`example/tutorial_c.c`](../example/tutorial_c.c) for a complete client-side injection example using `hlc_mutable_headers_t`.

---

## 10. HTTP Request Tracing

### HTTP helper functions

The C API provides convenience functions that bundle common HTTP tracing steps:

```c
/* Server: records remote address, endpoint, and request headers on the span */
pt_trace_http_server_request(span, remote_addr, endpoint, &req_hdr_reader);

/* Server with cookies */
pt_trace_http_server_request_with_cookie(span, remote_addr, endpoint,
                                         &req_hdr_reader, &cookie_reader);

/* Server: records response status, URL stat, and response headers */
pt_trace_http_server_response(span, url_pattern, method,
                               status_code, &resp_hdr_reader);

/* Client: records host, URL, and request headers on the span event */
pt_trace_http_client_request(se, host, url, &req_hdr_reader);

/* Client: records status code and response headers on the span event */
pt_trace_http_client_response(se, status_code, &resp_hdr_reader);
```

### Complete HTTP server handler

```c
static void handle_request(const my_request_t* req, my_response_t* res) {
    pt_agent_t agent = pt_global_agent();

    /* Build carriers */
    pt_context_reader_t ctx_reader = {
        my_request_headers(req), my_hdr_get
    };
    pt_header_reader_t req_reader = {
        my_request_headers(req), my_hdr_get, my_hdr_for_each
    };

    /* Create span, extracting upstream context */
    pt_span_t span = pt_agent_new_span_with_reader(
        agent, "C HTTP Server", my_request_path(req), &ctx_reader);

    /* Record the full server request */
    pt_trace_http_server_request(span,
                                 my_request_remote_addr(req),
                                 my_request_host(req),
                                 &req_reader);

    /* -- business logic -- */
    pt_span_event_t se = pt_span_new_event(span, "process");
    /* ... */
    pt_span_event_end(se);
    pt_span_event_destroy(se);

    /* Record response */
    pt_header_reader_t resp_reader = {
        my_response_headers(res), my_hdr_get, my_hdr_for_each
    };
    pt_trace_http_server_response(span, my_request_path(req),
                                  "GET", 200, &resp_reader);

    pt_span_end(span);
    pt_span_destroy(span);
    pt_agent_destroy(agent);  /* releases the pt_global_agent() handle wrapper */
}
```

The full compiling version is [`example/http_server_c.c`](../example/http_server_c.c).
For an end-to-end two-hop trace (client + server), run `example/tutorial_c`
alongside `example/http_server_c` — `tutorial_c` sends requests to port 8090 and
demonstrates inbound context extraction, outbound injection, and async spans.

### Outbound HTTP client call

```c
static void call_downstream(pt_span_t span) {
    const char* host = "downstream-host:8090";

    pt_span_event_t se = pt_span_new_event(span, "HTTP_CLIENT");
    pt_span_event_set_service_type(se, PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    pt_span_event_set_end_point(se, host);
    pt_span_event_set_destination(se, host);

    /* Inject trace context into outbound headers */
    my_headers_t out = my_headers_create();
    pt_context_writer_t writer = { &out, my_hdr_set };
    pt_span_event_inject_context(se, &writer);

    /* Annotate the outbound URL */
    pt_span_event_set_annotation_string(se, PT_ANNOTATION_HTTP_URL,
                                        "http://downstream-host:8090/api");

    /* Issue the request */
    int status = my_http_get(host, "/api", &out);
    my_headers_destroy(&out);

    pt_span_event_set_annotation_int(se, PT_ANNOTATION_HTTP_STATUS_CODE, status);

    pt_span_event_end(se);
    pt_span_event_destroy(se);
}
```

### URL statistics

```c
pt_span_set_url_stat(span, "/api/users", "GET", 200);
```

---

## 11. Asynchronous Spans

Use `pt_span_new_async_span()` to trace background tasks that continue after the parent span has ended.

```c
static void* background_worker(void* arg) {
    pt_span_t async_span = (pt_span_t)arg;

    pt_span_event_t e = pt_span_new_event(async_span, "background_job");
    /* ... work ... */
    pt_span_event_end(e);
    pt_span_event_destroy(e);

    pt_span_end(async_span);
    pt_span_destroy(async_span);
    return NULL;
}

static void handle_request(pt_span_t span) {
    /* Create async child before the parent ends */
    pt_span_t async_span = pt_span_new_async_span(span, "BackgroundTask");

    pthread_t tid;
    pthread_create(&tid, NULL, background_worker, async_span);
    pthread_detach(tid);

    /* Parent can end independently */
    pt_span_end(span);
    pt_span_destroy(span);
}
```

Rules:
- Each async span must be ended with `pt_span_end()` **exactly once**.
- Create the async span before the parent span ends.
- The async span's lifetime is independent of the parent span after creation.

See `example/tutorial_c.c` (step 3) for a complete async span example.

---

## 12. Error Reporting

### Simple and named errors

```c
pt_span_set_error(span, "something went wrong");
pt_span_set_error_named(span, "DatabaseError", "connection timeout after 30s");
pt_span_event_set_error(se, "QueryError");
pt_span_event_set_error_named(se, "SQL_ERROR", "invalid syntax near 'FROM'");
```

### Error with call stack

Provide a `pt_callstack_reader_t` for stack-enriched errors on span events. Frames are recorded only when `EnableCallstackTrace: true`:

```c
static void my_frame_iter(void* ud,
                           pt_callstack_frame_cb cb, void* cb_ud) {
    my_stack_t* s = (my_stack_t*)ud;
    for (int i = 0; i < s->depth; i++) {
        cb(s->frames[i].module,
           s->frames[i].function,
           s->frames[i].file,
           s->frames[i].line,
           cb_ud);
    }
}

my_stack_t stack = my_capture_stack();
pt_callstack_reader_t reader = { &stack, my_frame_iter };

pt_span_event_set_error_with_callstack(se, "OperationFailed",
                                       error_message, &reader);
```

### SQL error pattern

```c
pt_span_event_t se = pt_span_new_event(span, "SQL_SELECT");
pt_span_event_set_service_type(se, PT_SERVICE_TYPE_MYSQL_QUERY);
pt_span_event_set_end_point(se, "mysql-host:3306");
pt_span_event_set_destination(se, "app_db");

int rc = db_execute(sql);
if (rc != 0) {
    pt_span_event_set_error_named(se, "SQL_ERROR", db_last_error());
    pt_span_set_error(span, "database error");
} else {
    /* sanitize or omit sensitive bind arguments — never pass a password */
    pt_span_event_set_sql_query(se, sql, NULL, 0);
}

pt_span_event_end(se);
pt_span_event_destroy(se);
```

---

## 13. Service Type and Annotation Constants

### Application / service type constants

```c
/* Server-side */
PT_APP_TYPE_CPP             /* 1300 — C/C++ application          */
PT_SERVICE_TYPE_CPP         /* 1300 — alias for PT_APP_TYPE_CPP  */
PT_SERVICE_TYPE_CPP_FUNC    /* 1301 — C/C++ function block       */
PT_SERVICE_TYPE_GRPC_SERVER /* 1130 — gRPC server                */

/* HTTP client */
PT_SERVICE_TYPE_CPP_HTTP_CLIENT /* 9800 */

/* Databases */
PT_SERVICE_TYPE_MYSQL_QUERY     /* 2101 */
PT_SERVICE_TYPE_MSSQL_QUERY     /* 2201 */
PT_SERVICE_TYPE_ORACLE_QUERY    /* 2301 */
PT_SERVICE_TYPE_PGSQL_QUERY     /* 2501 */
PT_SERVICE_TYPE_CASSANDRA_QUERY /* 2601 */
PT_SERVICE_TYPE_MONGODB_QUERY   /* 2651 */

/* Caches / messaging */
PT_SERVICE_TYPE_MEMCACHED /* 8050 */
PT_SERVICE_TYPE_REDIS     /* 8203 */
PT_SERVICE_TYPE_KAFKA     /* 8660 */
PT_SERVICE_TYPE_HBASE     /* 8800 */

/* gRPC client */
PT_SERVICE_TYPE_GRPC_CLIENT /* 9160 */

/* Async */
PT_SERVICE_TYPE_ASYNC /* 100 */
```

### API type constants

```c
PT_API_TYPE_DEFAULT     /* 0   */
PT_API_TYPE_WEB_REQUEST /* 100 */
PT_API_TYPE_INVOCATION  /* 200 */
```

---

## Related Documentation

- [instrument.md](instrument.md) — C++ API guide; owns the shared API contracts (§6)
- [config.md](config.md) — full configuration reference
- [trouble_shooting.md](trouble_shooting.md) — startup contract and diagnostics
- API header: [`include/pinpoint/tracer_c.h`](../include/pinpoint/tracer_c.h)
- Examples: [`example/http_server_c.c`](../example/http_server_c.c), [`example/tutorial_c.c`](../example/tutorial_c.c), [`example/nginx/`](../example/nginx/)
