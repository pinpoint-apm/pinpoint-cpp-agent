# Pinpoint C++ Agent — C API Instrumentation Guide

This document covers instrumenting **plain C** applications with the Pinpoint C++ agent using the pure-C API declared in `include/pinpoint/tracer_c.h`.

If you are writing C++, prefer the richer C++ API documented in [instrument.md](instrument.md).

**Target readers**: C service owners and library authors who want to add Pinpoint distributed tracing without taking a C++ dependency.

---

## Table of Contents

- [1. Core Concepts](#1-core-concepts)
- [2. Key Differences from the C++ API](#2-key-differences-from-the-c-api)
- [3. Bootstrapping the Agent](#3-bootstrapping-the-agent)
- [4. Propagation Carriers](#4-propagation-carriers)
- [5. Creating Spans for Incoming Requests](#5-creating-spans-for-incoming-requests)
- [6. Recording Span Events](#6-recording-span-events)
- [7. Annotations](#7-annotations)
- [8. Usage Cautions: Span, SpanEvent, and Annotation Contracts](#8-usage-cautions-span-spanevent-and-annotation-contracts)
- [9. Distributed Tracing and Context Propagation](#9-distributed-tracing-and-context-propagation)
- [10. HTTP Request Tracing](#10-http-request-tracing)
- [11. Asynchronous Spans](#11-asynchronous-spans)
- [12. Error Reporting](#12-error-reporting)
- [13. Service Type and Annotation Constants](#13-service-type-and-annotation-constants)
- [14. Best Practices](#14-best-practices)
- [15. Complete Examples](#15-complete-examples)

---

## 1. Core Concepts

Pinpoint models each transaction as a tree of **spans**.

| Concept | Description |
|---|---|
| **Span** (`pt_span_t`) | Top-level trace segment for an incoming request, job, or logical unit of work. Carries a trace ID and span ID. |
| **SpanEvent** (`pt_span_event_t`) | A child operation inside a span (DB query, HTTP client call, function block). Multiple events form the call-stack view in the Pinpoint UI. |
| **Annotation** (`pt_annotation_t`) | Key/value metadata attached to a span or span event (URL, status code, SQL text, etc.). |
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

Span-event and annotation handles are non-owning views. They allocate no resources, and `pt_span_event_destroy()` / `pt_annotation_destroy()` are safe no-ops kept for API symmetry and compatibility. Use these handles only while their parent span/span event is alive and active; do not read, mutate, or store them for work that can run after `pt_span_event_end()`, `pt_span_end()`, or `pt_span_destroy()`.

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

    pt_agent_t agent = pt_start_agent(opts);  /* NULL options = all defaults */
    pt_agent_options_free(opts);              /* only read during the call   */
    if (!agent) {
        /* handle allocation failed — the app still runs, just untraced */
        fprintf(stderr, "failed to create pinpoint agent handle\n");
    }

    /* application logic ... */

    pt_agent_shutdown(agent);  /* flush pending spans */
    pt_agent_destroy(agent);   /* release the handle  */
    return 0;
}
```

**`pt_start_agent()` is asynchronous.** It returns as soon as initialization is
launched: a background thread opens the gRPC channels, registers the agent with
the collector (retrying until the collector accepts it) and starts the workers.
`pt_agent_is_enabled()` returns `1` only after that registration succeeds, so it
is normally still `0` right after `pt_start_agent()` returns — that is not an
error, and a non-NULL return value does not mean the start succeeded either (on
a configuration or setup failure a no-op agent handle is returned).

**Verify agent start through the agent log**, not through the API: on success
the log shows `AgentInfo sent`; failures appear as error entries such as
`agent start failed: ...` or `failed to init grpc workers: ...` (set
`Log.Level: "debug"` for more detail). Even when the agent fails to start, the
application is unaffected — every call on a no-op agent is safe and does
nothing, so the app runs normally and only the traces are lost.

`pt_agent_shutdown()` is terminal for that agent: the same handle can never come
back online, `pt_agent_is_enabled()` stays `0`, and every span it hands out is a
no-op span. The application keeps running normally — it is just no longer
traced. To stop and later resume tracing in a long-running process, destroy the
handle and run `pt_start_agent()` again for each cycle:

```c
pt_agent_shutdown(agent);
pt_agent_destroy(agent);          /* drop the dead handle */

/* ... later ... */
agent = pt_start_agent(NULL);     /* a NEW agent; the old one cannot restart */
```

Each cycle re-resolves the identity with a freshly auto-generated agent id and
registers as a new agent instance in the Pinpoint UI. Set `AgentName` in the
configuration for a stable label across cycles.

`pt_agent_options_set_server_metadata()` attaches AgentInfo server metadata
(runtime description, args, libs):

```c
const char* args[] = {"--port=8080"};
const char* libs[] = {"my-http-framework/1.2.3"};

pt_agent_options_t opts = pt_agent_options_new();
pt_agent_options_set_server_metadata(opts, "my-service-runtime", args, 1, libs, 1);
pt_agent_t agent = pt_start_agent(opts);
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

`pt_agent_is_enabled()` is **not a startup success check**: it returns `1` only
after the background registration with the collector completes, so it is
normally still `0` right after `pt_start_agent()` returns. Verify startup
through the agent log instead (see above).

Use it for one purpose only: as a **fast-fail guard before creating a span**,
to skip instrumentation work while tracing is off. The request is served
normally either way:

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

> **Tip**: If your HTTP library's `get` and `for_each` function signatures already match `pt_reader_get_fn` and `pt_header_for_each_fn`, assign them directly without writing adapter functions — see `example/http_server_c.c` for a concrete example.

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

> `pt_span_event_end()` is called on the **event handle**, not on the parent span. After it returns, do not read or mutate the event handle or annotation handles derived from it. `pt_span_event_destroy()` is optional and does not extend the event lifetime.

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

Annotations attach structured key/value metadata to spans and span events.

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

### Adding annotations to a span

```c
pt_annotation_t anno = pt_span_get_annotations(span);

pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, "/api/users/123");
pt_annotation_append_int(anno, PT_ANNOTATION_HTTP_STATUS_CODE, 200);

pt_annotation_destroy(anno);  /* optional compatibility no-op */
```

### Adding annotations to a span event

```c
pt_annotation_t anno = pt_span_event_get_annotations(se);

pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, "http://downstream/foo");
pt_annotation_append_int(anno, PT_ANNOTATION_HTTP_STATUS_CODE, status);

pt_annotation_destroy(anno);  /* optional compatibility no-op */
```

### Annotation value types

```c
pt_annotation_append_int(anno, key, (int32_t)value);
pt_annotation_append_long(anno, key, (int64_t)value);
pt_annotation_append_string(anno, key, "string value");
pt_annotation_append_string_string(anno, key, "s1", "s2");
pt_annotation_append_int_string_string(anno, key, 42, "s1", "s2");
/* SQL with binary UID: */
pt_annotation_append_sql_uid_string_string(anno, PT_ANNOTATION_SQL_UID,
                                         uid_bytes, uid_len, sql, args);
```

### Custom annotation keys

Use large positive integers to avoid collisions with predefined keys:

```c
#define MY_ANNOTATION_USER_ID    10000
#define MY_ANNOTATION_SESSION_ID 10001

pt_annotation_append_string(anno, MY_ANNOTATION_USER_ID, user_id);
```

Never record passwords, secrets, or PII in annotations.

---

## 8. Usage Cautions: Span, SpanEvent, and Annotation Contracts

The C API wraps the same span, span event, and annotation implementations as the C++ API, so the same contracts apply — this section restates them in C-API terms. Most violations are detected at runtime and degrade to a **logged no-op** rather than a crash, but they distort traces, and the threading and lifetime rules below are hard requirements that can crash the process if broken. Treat the warning messages quoted here as instrumentation bugs when they appear in the agent log.

### 8.1 A Span Is Single-Threaded

A span — including every span event and annotation handle derived from it — must be used by **one thread only** for its entire lifetime. Nothing inside a span is locked, so concurrent calls on the same span are undefined behavior and can corrupt memory or crash.

- The agent binds a span to the first thread that calls `pt_span_new_event()` and logs an error (plus an `assert` in debug builds) when another thread touches it afterwards: `span accessed from another thread`.
- Because binding is lazy, a **complete handoff** is allowed: create the span on thread A, pass the handle to thread B, and never touch it from A again (the async example in [§11](#11-asynchronous-spans) hands the async span to a `pthread` this way).
- To trace work that runs **concurrently** with the parent, do not share the span. Call `pt_span_new_async_span()` *on the span's owning thread* and give the returned child span to the worker; the child follows the same single-thread rule on its own thread.

### 8.2 End Exactly Once, Record Before Ending, Destroy Last

`pt_span_end()` and `pt_span_event_end()` are terminal:

- A duplicate end call logs `span (event) is already finished` and does nothing.
- After the end call, **every recording function** on that object becomes a warning no-op: property setters, error setters, `pt_span_event_set_sql_query()`, `pt_span_event_inject_context()`, header recording, and the `_get_annotations()` functions (which then return a no-op container). The data may already be in flight on the agent's gRPC worker thread. Record status codes, errors, and annotations **before** ending.
- A span destroyed without `pt_span_end()` is **never sent** — its data is lost. `pt_span_destroy()` only releases the handle; it does not submit the span.
- `pt_span_event_destroy()` and `pt_annotation_destroy()` are compatibility no-ops — they do **not** end the event. Forgetting `pt_span_event_end()` is not fixed by calling destroy.

### 8.3 End Span Events in Nesting (LIFO) Order

Span events form a stack. Calling `pt_span_event_end()` on an outer event while an inner event is still open implicitly finishes every event nested above it and logs `span event ended out of order`. Likewise, `pt_span_end()` force-finishes all still-open events and logs `N span event(s) not ended by user code`. The trace survives, but implicitly finished events get the wrong end time — their duration silently stretches to the enclosing end call.

### 8.4 Event and Annotation Handles Are Non-Owning Views

`pt_span_event_t` and `pt_annotation_t` are raw pointers into storage owned by the parent span:

- They stay valid only until `pt_span_destroy()` releases the parent span. Calling into an already-ended event or a sealed annotation **while the span handle is alive** is a safe warning no-op; calling through a handle **after the span is destroyed** is a use-after-free.
- Destroy the span only after every derived event/annotation handle is out of use. Do not cache these handles in long-lived structures.
- Passing `NULL` for an event or annotation handle is silently ignored, and event-creation functions return `NULL` when the span handle is `NULL` or already destroyed — so a crash on a NULL handle points at memory corruption, not at the agent.

### 8.5 Event Depth and Count Limits (Overflow)

Per span, event nesting depth is capped by `Span.MaxEventDepth` (default 64) and the total event count by `Span.MaxEventSequence` (default 5000). When either cap is reached, `pt_span_new_event()` logs `span event maximum depth/sequence exceeded` and returns a shared **disabled event** handle instead:

- It records nothing — operation name, timings, SQL, errors, and annotations are discarded.
- `pt_span_event_inject_context()` **still writes the full trace context**, so downstream services continue the distributed trace. Overflow limits profiling detail; it is not a sampling decision.
- You must still call `pt_span_event_end()` exactly once for each overflowed `pt_span_new_event()` call — the span balances an internal overflow counter with it.
- The disabled event is a single shared object per span, so `pt_span_event_set_destination()` values from interleaved overflowed calls can bleed into each other's `Pinpoint-Host` header.
- `pt_span_new_async_span()` called while the span is overflowed returns a no-op span.

If the overflow warning appears regularly, create fewer, coarser span events per transaction or raise the limits in the configuration.

### 8.6 `pt_span_get_event()` Returns the Innermost Active Event

`pt_span_get_event()` returns the top of the event stack: the most recently created event that has not ended. When the span is finished or has no active event, it returns a valid handle to a shared no-op event (and logs `abnormal span - has no event`), so the return value cannot be used to detect whether an event is active; `NULL` is returned only for a `NULL`/destroyed span handle or an internal failure. Do not assume it refers to a specific event you created earlier; in helper functions, prefer passing the `pt_span_event_t` returned by `pt_span_new_event()` explicitly.

### 8.7 Annotation Container Rules

- The annotation container is **sealed** when its owner ends (`pt_span_event_end()`/`pt_span_end()`). Appends through a previously obtained `pt_annotation_t` then log `annotation is already finished` and do nothing.
- Values are **copied at append time**: string arguments only need to remain valid for the duration of the call.
- Append calls never fail visibly; on allocation failure the annotation is dropped with an error log.
- There is no key de-duplication: appending the same key twice records two annotations.
- Every annotation byte is copied into the span and shipped to the collector — keep annotations small and sanitized (see [§7](#7-annotations)).

### 8.8 Keep Operation and Error Names Low-Cardinality

The `operation` passed to `pt_agent_new_span*()`/`pt_span_new_event*()`/`pt_span_new_async_span()` and the `error_name` passed to the named-error functions are interned in bounded LRU caches, and **every new unique string enqueues a metadata message to the collector**. Per-request unique names churn the cache and flood the collector with metadata:

```c
/* DON'T: unique operation name per request */
char op[64];
snprintf(op, sizeof(op), "getUser-%s", user_id);
pt_span_event_t se = pt_span_new_event(span, op);

/* DO: fixed operation name, variable data as an annotation */
pt_span_event_t se = pt_span_new_event(span, "getUser");
pt_annotation_t anno = pt_span_event_get_annotations(se);
pt_annotation_append_string(anno, MY_ANNOTATION_USER_ID, user_id);
```

The `rpc_point` argument of `pt_agent_new_span*()` is not interned — it may safely carry the actual request path.

### 8.9 Error Recording and Exception Buffering

- `pt_span_set_error*()` marks the whole transaction as failed; `pt_span_event_set_error*()` marks only that step. Record at the granularity of the failure.
- The call-stack variant `pt_span_event_set_error_with_callstack()` exists only for span events, and records frames only when `EnableCallstackTrace: true` is set in the configuration (default `false`).
- At most **100 exceptions with call stacks are buffered per span**; further ones are dropped. Buffered exceptions are transmitted only at `pt_span_end()` — a span kept open for a very long time delays them and grows memory.

### 8.10 Clock and Start-Time Caveats

Elapsed times travel as **int32 milliseconds** on the wire. If you override timestamps with `pt_span_set_start_time_ms()` / `pt_span_event_set_start_time_ms()`:

- The argument is **milliseconds** since the Unix epoch. Passing seconds (e.g. `time(NULL)`) makes the computed deltas overflow int32 and silently corrupts the trace timeline — the value is not validated.
- A start time more than ~24.8 days in the past overflows the elapsed field; a start time in the future is clamped to an elapsed of 0 at end time, but inter-event offsets within a chunk can still wrap.
- Only pass wall-clock values taken at the actual start of the operation.

### 8.11 Noop and Unsampled Spans Are Deliberately Silent

When the agent is disabled or shut down, the URL/method is excluded by filters, or sampling rejects the transaction, the span-creation functions return a no-op or unsampled span handle on which every call succeeds and records nothing (`NULL` is returned only when handle setup itself fails):

- `pt_span_is_sampled()` returns `0`, `pt_span_get_trace_id()` returns length 0 with an empty string, and `pt_span_get_span_id()` returns 0 for no-op spans (unsampled spans do carry a real span id).
- Use `pt_span_is_sampled()` to skip *expensive data collection only* — do **not** skip creating span events and calling `pt_span_event_inject_context()` on outbound calls. An unsampled span's event still writes `Pinpoint-Sampled: s0`, which tells downstream services not to trace the request. Skipping the injection makes downstream agents treat the call as a brand-new transaction and sample it, producing broken partial traces.
- `pt_span_end()`/`pt_span_destroy()` are safe (and cheap) on these handles — keep the normal end/destroy flow unconditionally.

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

See `example/tutorial_c.c` for a complete client-side injection example using `hlc_mutable_headers_t`.

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
    pt_annotation_t anno = pt_span_event_get_annotations(se);
    pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL,
                                "http://downstream-host:8090/api");
    pt_annotation_destroy(anno);

    /* Issue the request */
    int status = my_http_get(host, "/api", &out);
    my_headers_destroy(&out);

    pt_annotation_t anno2 = pt_span_event_get_annotations(se);
    pt_annotation_append_int(anno2, PT_ANNOTATION_HTTP_STATUS_CODE, status);
    pt_annotation_destroy(anno2);

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

### Simple error message

```c
pt_span_set_error(span, "something went wrong");
```

### Named error

```c
pt_span_set_error_named(span, "DatabaseError", "connection timeout after 30s");
pt_span_event_set_error(se, "QueryError");
pt_span_event_set_error_named(se, "SQL_ERROR", "invalid syntax near 'FROM'");
```

### Error with call stack

Provide a `pt_callstack_reader_t` for stack-enriched errors on span events:

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
    pt_span_event_set_sql_query(se, sql, NULL, 0);  /* omit args */
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

## 14. Best Practices

### Always end spans and events on every code path

```c
pt_span_t span = pt_agent_new_span(agent, "MyService", "/api");

int rc = do_work();
if (rc != 0) {
    pt_span_set_error(span, "work failed");
    pt_span_set_status_code(span, 500);
} else {
    pt_span_set_status_code(span, 200);
}

/* Always reached */
pt_span_end(span);
pt_span_destroy(span);
```

### Check sampling before expensive operations

```c
if (pt_span_is_sampled(span)) {
    /* collect detailed payload or build large annotation */
}
```

### Balance every push with a pop

Each `pt_span_new_event()` pushes an event; each `pt_span_event_end(se)` pops the event it ends. Unbalanced calls corrupt the event stack and produce incorrect call-graph data in the Pinpoint UI. Ending the same event twice is a warning no-op — it does not pop another event.

```c
pt_span_event_t e = pt_span_new_event(span, "op");
/* ... */
pt_span_event_end(e);       /* pop   */
pt_span_event_destroy(e);   /* optional no-op */
```

### Sanitize sensitive data

```c
/* WRONG — records the password */
/* const char* unsafe_args[] = {password}; */
/* pt_span_event_set_sql_query(se, sql, unsafe_args, 1); */

/* RIGHT — sanitize or omit sensitive parameters */
const char* safe_args[] = {"[REDACTED]"};
pt_span_event_set_sql_query(se, sql, safe_args, 1);
```

### Keep annotation handles short-lived

`pt_annotation_t` handles are non-owning views. They do not leak if `pt_annotation_destroy()` is omitted, but keeping their scope short avoids accidental use after the parent span or span event is finished:

```c
pt_annotation_t anno = pt_span_event_get_annotations(se);
pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, url);
pt_annotation_destroy(anno);   /* optional no-op */
```

### Shut down cleanly

```c
/* At process exit: flush pending spans before terminating */
pt_agent_shutdown(agent);
pt_agent_destroy(agent);
```

---

## 15. Complete Examples

### Minimal HTTP server handler

The following is a condensed version of `example/http_server_c.c`:

```c
#include "pinpoint/tracer_c.h"

static void on_request(const my_request_t* req, my_response_t* res) {
    pt_agent_t agent = pt_global_agent();

    pt_header_reader_t reader = {
        my_request_headers(req),
        my_headers_get,
        my_headers_for_each,
    };
    pt_context_reader_t ctx = { reader.userdata, reader.get };

    pt_span_t span = pt_agent_new_span_with_reader(
        agent, "C HTTP Server", my_request_path(req), &ctx);

    pt_trace_http_server_request(span,
                                 my_request_remote_addr(req),
                                 my_request_host(req),
                                 &reader);

    pt_span_event_t se = pt_span_new_event(span, "handle");
    my_response_set_body(res, "OK");
    pt_span_event_end(se);
    pt_span_event_destroy(se);

    pt_span_set_status_code(span, 200);
    pt_span_set_url_stat(span, my_request_path(req), "GET", 200);

    pt_span_end(span);
    pt_span_destroy(span);
    pt_agent_destroy(agent);  /* releases the pt_global_agent() handle wrapper */
}

int main(void) {
    setenv("PINPOINT_CPP_CONFIG_FILE",      "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "c-http-server",             0);

    pt_agent_t agent = pt_start_agent(NULL);
    if (!agent) return 1;

    my_server_t* srv = my_server_create();
    my_server_get(srv, "/api", on_request, NULL);
    my_server_listen(srv, "0.0.0.0", 8090);

    my_server_destroy(srv);
    pt_agent_shutdown(agent);
    pt_agent_destroy(agent);
    return 0;
}
```

For a full end-to-end two-hop trace (client + server), run `example/tutorial_c` alongside `example/http_server_c` — `tutorial_c` sends requests to port 8090 and demonstrates inbound context extraction, outbound injection, and async span creation.

---

## Related Documentation

- [instrument.md](instrument.md) — C++ API instrumentation guide
- [config.md](config.md) — full configuration reference
- [quick_start.md](quick_start.md) — getting started in five minutes
- [trouble_shooting.md](trouble_shooting.md) — diagnostics and common issues
- API header: [`include/pinpoint/tracer_c.h`](../include/pinpoint/tracer_c.h)
- Example: [`example/http_server_c.c`](../example/http_server_c.c)
- Example: [`example/tutorial_c.c`](../example/tutorial_c.c)

---

*Apache License 2.0 — See [LICENSE](../LICENSE) for details.*
