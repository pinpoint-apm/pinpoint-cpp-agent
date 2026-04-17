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
- [8. Distributed Tracing and Context Propagation](#8-distributed-tracing-and-context-propagation)
- [9. HTTP Request Tracing](#9-http-request-tracing)
- [10. Asynchronous Spans](#10-asynchronous-spans)
- [11. Error Reporting](#11-error-reporting)
- [12. Service Type and Annotation Constants](#12-service-type-and-annotation-constants)
- [13. Best Practices](#13-best-practices)
- [14. Complete Examples](#14-complete-examples)

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
| Memory management | RAII / destructors | Manual `_destroy()` calls |
| Propagation carriers | Virtual base classes | Callback structs (`pt_context_reader_t`, etc.) |
| Span event lifecycle | `ScopedSpanEvent` helper available | Manual `pt_span_end_event(span)` + `pt_span_event_destroy(se)` |
| Error handling | C++ exceptions | Return values / `pt_span_set_error()` |
| Header file | `include/pinpoint/tracer.h` | `include/pinpoint/tracer_c.h` |

### Handle lifetime rules

Every handle obtained from a `pt_*_new_*` or `pt_*_get_*` function **must** be released with its matching `_destroy()` function:

```c
pt_agent_t      → pt_agent_destroy()       (except pt_global_agent() — see §3)
pt_span_t       → pt_span_destroy()        (call pt_span_end() first)
pt_span_event_t → pt_span_event_destroy()  (call pt_span_end_event() first)
pt_annotation_t → pt_annotation_destroy()
```

---

## 3. Bootstrapping the Agent

### Configuration

Configuration can be supplied via a YAML file, an inline YAML string, or environment variables. All configuration must be applied **before** calling `pt_create_agent()`.

```c
#include "pinpoint/tracer_c.h"

/* Option 1: config file */
pt_set_config_file_path("/etc/pinpoint/agent.yaml");

/* Option 2: inline YAML string */
pt_set_config_string("ApplicationName: my-c-service\n"
                     "Collector:\n"
                     "  GrpcHost: localhost\n");

/* Option 3: environment variables (set before calling pt_create_agent) */
setenv("PINPOINT_CPP_CONFIG_FILE",           "/tmp/pinpoint-config.yaml", 0);
setenv("PINPOINT_CPP_APPLICATION_NAME",      "my-c-service",              0);
setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true",                      0);
```

### Creating and destroying an agent

```c
int main(void) {
    pt_agent_t agent = pt_create_agent();
    if (!agent) {
        fprintf(stderr, "failed to create pinpoint agent\n");
        return 1;
    }

    /* application logic ... */

    pt_agent_shutdown(agent);  /* flush pending spans */
    pt_agent_destroy(agent);   /* release the handle  */
    return 0;
}
```

`pt_create_agent_with_type()` lets you supply an explicit application type constant:

```c
pt_agent_t agent = pt_create_agent_with_type(PT_APP_TYPE_CPP);
```

### Using the global agent

`pt_global_agent()` returns a non-owning handle to the singleton agent that was created by the most recent `pt_create_agent()` call. **Do not call `pt_agent_destroy()` on a handle returned by `pt_global_agent()`.**

```c
/* In a request handler, far from main() */
pt_agent_t agent = pt_global_agent();  /* non-owning */

pt_span_t span = pt_agent_new_span(agent, "MyService", "/api/v1");
/* ... */
pt_span_end(span);
pt_span_destroy(span);

pt_agent_destroy(agent);  /* releases the non-owning handle wrapper */
```

### Checking agent status

```c
if (!pt_agent_is_enabled(agent)) {
    fprintf(stderr, "agent is disabled — check configuration\n");
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
pt_span_inject_context(span, &writer);
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

pt_span_end_event(span);      /* pop and finalize the event — called on SPAN */
pt_span_event_destroy(se);    /* release the handle */
```

> `pt_span_end_event()` is called on the **parent span**, not on the event handle. The event handle may still be read after `pt_span_end_event()` but must be released with `pt_span_event_destroy()`.

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

`pt_span_new_event()` pushes onto an internal stack. Each `pt_span_end_event()` pops the top of the stack. You are responsible for ensuring pops are balanced with pushes.

```c
pt_span_event_t e1 = pt_span_new_event(span, "outer");

    pt_span_event_t e2 = pt_span_new_event(span, "inner");
    pt_span_end_event(span);   /* ends inner */
    pt_span_event_destroy(e2);

pt_span_end_event(span);       /* ends outer */
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

pt_annotation_destroy(anno);  /* always destroy when done */
```

### Adding annotations to a span event

```c
pt_annotation_t anno = pt_span_event_get_annotations(se);

pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, "http://downstream/foo");
pt_annotation_append_int(anno, PT_ANNOTATION_HTTP_STATUS_CODE, status);

pt_annotation_destroy(anno);
```

### Annotation value types

```c
pt_annotation_append_int(anno, key, (int32_t)value);
pt_annotation_append_long(anno, key, (int64_t)value);
pt_annotation_append_string(anno, key, "string value");
pt_annotation_append_string_string(anno, key, "s1", "s2");
pt_annotation_append_int_string_string(anno, key, 42, "s1", "s2");
/* SQL with binary UID: */
pt_annotation_append_bytes_string_string(anno, PT_ANNOTATION_SQL_UID,
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

## 8. Distributed Tracing and Context Propagation

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

You can also extract context after creating a span:

```c
pt_span_t span = pt_agent_new_span(agent, "MyService", "/api");
pt_context_reader_t reader = { &req->headers, my_hdr_get };
pt_span_extract_context(span, &reader);
```

### Client side — injecting outgoing context

```c
/* Build outbound headers, then inject trace context */
my_headers_t out = my_headers_create();

pt_context_writer_t writer = { &out, my_hdr_set };
pt_span_inject_context(span, &writer);

/* Issue the outgoing request with out headers */
my_http_get(client, "/downstream", &out);
my_headers_destroy(&out);
```

See `example/tutorial_c.c` for a complete client-side injection example using `hlc_mutable_headers_t`.

---

## 9. HTTP Request Tracing

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
    pt_span_end_event(span);
    pt_span_event_destroy(se);

    /* Record response */
    pt_header_reader_t resp_reader = {
        my_response_headers(res), my_hdr_get, my_hdr_for_each
    };
    pt_trace_http_server_response(span, my_request_path(req),
                                  "GET", 200, &resp_reader);

    pt_span_end(span);
    pt_span_destroy(span);
    pt_agent_destroy(agent);  /* non-owning handle from pt_global_agent() */
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
    pt_span_inject_context(span, &writer);

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

    pt_span_end_event(span);
    pt_span_event_destroy(se);
}
```

### URL statistics

```c
pt_span_set_url_stat(span, "/api/users", "GET", 200);
```

---

## 10. Asynchronous Spans

Use `pt_span_new_async_span()` to trace background tasks that continue after the parent span has ended.

```c
static void* background_worker(void* arg) {
    pt_span_t async_span = (pt_span_t)arg;

    pt_span_event_t e = pt_span_new_event(async_span, "background_job");
    /* ... work ... */
    pt_span_end_event(async_span);
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

## 11. Error Reporting

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
    pt_span_event_set_sql_query(se, sql, "");  /* sanitize args */
}

pt_span_end_event(span);
pt_span_event_destroy(se);
```

---

## 12. Service Type and Annotation Constants

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

## 13. Best Practices

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

Each `pt_span_new_event()` pushes an event; each `pt_span_end_event()` pops one. Unbalanced calls corrupt the event stack and produce incorrect call-graph data in the Pinpoint UI.

```c
pt_span_event_t e = pt_span_new_event(span, "op");
/* ... */
pt_span_end_event(span);    /* pop   */
pt_span_event_destroy(e);   /* free  */
```

### Sanitize sensitive data

```c
/* WRONG — records the password */
/* pt_span_event_set_sql_query(se, sql, password); */

/* RIGHT — sanitize or omit sensitive parameters */
pt_span_event_set_sql_query(se, sql, "[REDACTED]");
```

### Release annotation handles promptly

`pt_annotation_t` handles are lightweight wrappers but must be destroyed to avoid leaks:

```c
pt_annotation_t anno = pt_span_event_get_annotations(se);
pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, url);
pt_annotation_destroy(anno);   /* always destroy */
```

### Shut down cleanly

```c
/* At process exit: flush pending spans before terminating */
pt_agent_shutdown(agent);
pt_agent_destroy(agent);
```

---

## 14. Complete Examples

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
    pt_span_end_event(span);
    pt_span_event_destroy(se);

    pt_span_set_status_code(span, 200);
    pt_span_set_url_stat(span, my_request_path(req), "GET", 200);

    pt_span_end(span);
    pt_span_destroy(span);
    pt_agent_destroy(agent);
}

int main(void) {
    setenv("PINPOINT_CPP_CONFIG_FILE",      "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "c-http-server",             0);

    pt_agent_t agent = pt_create_agent();
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
