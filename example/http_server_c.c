/*
 * Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file http_server_c.c
 * @brief Pure-C port of example/http_server.cpp.
 *
 * Demonstrates how to combine the Pinpoint C API (pinpoint/tracer_c.h) with
 * a thin C wrapper around cpp-httplib (httplib_c.h) to trace an HTTP server
 * from plain C code.  The behaviour mirrors the C++ example exactly:
 *
 *   1. Create an agent and start the HTTP server listening on 0.0.0.0:8090.
 *   2. For each GET /foo request, open a root span, extract the inbound
 *      trace context from the request headers, record the request, emit
 *      some nested span events with randomised sleeps, then finalize.
 *
 * Build: compiled as C (-x c).  Links against pinpoint_cpp-static and
 * the httplib_c wrapper object.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#  define sleep_ms(ms) Sleep((DWORD)(ms))
#else
#  include <unistd.h>
#  define sleep_ms(ms) usleep((useconds_t)((ms) * 1000))
#endif

#include "pinpoint/tracer_c.h"
#include "httplib_c.h"

/* ========================================================================== */
/* Randomised sample data — matches the C++ example                            */
/* ========================================================================== */

static const int kHttpStatus[5] = {200, 303, 404, 500, 501};

static const char* const kUrls[5] = {
    "/path/to?resource=here",
    "/example/to?resource=here",
    "/pinpoint",
    "/pinpoint-apm/pinpoint",
    "/pinpoint-envoy/to?resource=here",
};

static const char* const kMethods[5] = { "GET", "PUT", "DELETE", "GET", "PUT" };

static const int kSleepMillis[5] = {5, 10, 50, 80, 100};

static int random_number(void) {
    return rand() % 5;
}

/* ========================================================================== */
/* Helpers                                                                      */
/* ========================================================================== */

/**
 * Opens a root span for an inbound HTTP request.  Extracts any incoming
 * Pinpoint trace-context headers from the request so that cross-process
 * traces are linked.
 */
static pt_span_t make_span(const hlc_request_t* req) {
    pt_agent_t agent = pt_global_agent();

    /* Build a pt_header_reader_t directly from the httplib header map — the
     * hlc_headers_get/hlc_headers_for_each signatures match pt_reader_get_fn
     * and pt_header_for_each_fn exactly, so no adapter code is needed. */
    pt_header_reader_t reader;
    reader.userdata = hlc_request_headers_handle(req);
    reader.get      = hlc_headers_get;
    reader.for_each = hlc_headers_for_each;

    /* NewSpan expects a pt_context_reader_t (smaller, read-only subset of
     * pt_header_reader_t).  Build one from the same backing data. */
    pt_context_reader_t ctx_reader;
    ctx_reader.userdata = reader.userdata;
    ctx_reader.get      = reader.get;

    pt_span_t span = pt_agent_new_span_with_reader(
        agent, "C Http Server", hlc_request_path(req), &ctx_reader);

    /* Compute endpoint: prefer the "Host" header, else fall back to local_addr:port. */
    char endpoint[512];
    const char* host = hlc_request_get_header(req, "Host");
    if (host && host[0] != '\0') {
        snprintf(endpoint, sizeof(endpoint), "%s", host);
    } else {
        snprintf(endpoint, sizeof(endpoint), "%s:%d",
                 hlc_request_local_addr(req), hlc_request_local_port(req));
    }

    pt_trace_http_server_request(span,
                                 hlc_request_remote_addr(req),
                                 endpoint,
                                 &reader);

    /* Release the non-owning handle returned by pt_global_agent(). */
    pt_agent_destroy(agent);

    return span;
}

/**
 * Creates a nested cascade of span events exactly like the C++ example.
 * Each "New" pushes an event, and the matching "End" pops it.
 */
static void record_nested_events(pt_span_t span) {
    const int rand_url    = random_number();
    const int rand_method = random_number();
    const int rand_status = random_number();

    pt_span_event_t e;

    e = pt_span_new_event(span, "func_example");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_event_destroy(e);

    e = pt_span_new_event(span, "func_1");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_event_destroy(e);

    e = pt_span_new_event(span, "func_2");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_end_event(span);
    pt_span_event_destroy(e);

    e = pt_span_new_event(span, "func_3");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_event_destroy(e);

    e = pt_span_new_event(span, "func_4");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_event_destroy(e);

    e = pt_span_new_event(span, "func_5");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_end_event(span);
    pt_span_event_destroy(e);

    pt_span_end_event(span);  /* closes func_4 */
    pt_span_end_event(span);  /* closes func_3 */

    e = pt_span_new_event(span, "foo");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_event_destroy(e);

    e = pt_span_new_event(span, "bar");
    sleep_ms(kSleepMillis[random_number()]);
    pt_span_end_event(span);
    pt_span_event_destroy(e);

    pt_span_end_event(span);  /* closes foo  */

    pt_span_end_event(span);  /* closes func_1 */
    pt_span_end_event(span);  /* closes func_example */

    const int status_code = kHttpStatus[rand_status];
    pt_span_set_status_code(span, status_code);
    pt_span_set_url_stat(span, kUrls[rand_url], kMethods[rand_method], status_code);
}

/* ========================================================================== */
/* Handler: GET /foo                                                            */
/* ========================================================================== */

static void on_foo(const hlc_request_t* req, hlc_response_t* res, void* ud) {
    (void)ud;

    pt_span_t span = make_span(req);

    pt_span_event_t foo_event = pt_span_new_event(span, "foo");

    /* Simulate some work (10–500 ms). */
    sleep_ms(10 + (rand() % 491));

    hlc_response_set_content(res, "hello, foo!!", "text/plain");

    pt_span_end_event(span);
    pt_span_event_destroy(foo_event);

    /* Record response headers on the span. */
    pt_header_reader_t resp_reader;
    resp_reader.userdata = hlc_response_headers_handle(res);
    resp_reader.get      = hlc_headers_get;
    resp_reader.for_each = hlc_headers_for_each;
    pt_span_record_header(span, PT_HTTP_RESPONSE, &resp_reader);

    record_nested_events(span);

    pt_span_end(span);
    pt_span_destroy(span);
}

/* ========================================================================== */
/* Entry point                                                                  */
/* ========================================================================== */

int main(void) {
    /* Seed RNG for deterministic-enough demo data. */
    srand((unsigned)time(NULL));

    /* setenv() with overwrite=0 matches the C++ example: only set if unset. */
    setenv("PINPOINT_CPP_CONFIG_FILE",        "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME",   "c-http-server",             0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true",                   0);
    /* setenv("PINPOINT_CPP_LOG_LEVEL", "debug", 0); */

    pt_agent_t agent = pt_create_agent();
    if (!agent) {
        fprintf(stderr, "failed to create pinpoint agent\n");
        return 1;
    }

    hlc_server_t server = hlc_server_create();
    if (!server) {
        fprintf(stderr, "failed to create http server\n");
        pt_agent_shutdown(agent);
        pt_agent_destroy(agent);
        return 1;
    }

    hlc_server_get(server, "/foo", on_foo, NULL);

    printf("c-http-server listening on 0.0.0.0:8090\n");
    (void)hlc_server_listen(server, "0.0.0.0", 8090);

    hlc_server_destroy(server);
    pt_agent_shutdown(agent);
    pt_agent_destroy(agent);

    return 0;
}
