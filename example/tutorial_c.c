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
 * @file tutorial_c.c
 * @brief Pure-C port of example/tutorial.cpp.
 *
 * Exercises the whole Pinpoint C API surface in a single request flow:
 *
 *   Inbound  → new span with extracted trace context
 *            → record request headers on the span
 *            → open a child span event for an outbound HTTP call
 *               · set service type / endpoint / destination
 *               · inject trace context into the outbound headers
 *               · annotate URL and status code
 *               · record errors if the call fails
 *            → open another child span event
 *               · create an async span for a background thread
 *               · emit a nested span event on the async span
 *               · end the async span
 *            → set status + end span
 *
 * The downstream HTTP endpoint this tutorial points at is localhost:8090,
 * which is exactly what example/http_server_c (or http_server) listens on —
 * run that alongside this program to see a full two-hop trace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  define sleep_sec(s) Sleep((DWORD)((s) * 1000))
#else
#  include <unistd.h>
#  define sleep_sec(s) sleep((unsigned)(s))
#endif

#include "pinpoint/tracer_c.h"
#include "httplib_c.h"

/* ========================================================================== */
/* Per-request handler state — forwarded into on_foo() via the userdata ptr    */
/* ========================================================================== */

typedef struct {
    pt_agent_t agent;
} handler_ctx_t;

/* ========================================================================== */
/* GET /foo — builds a full trace for this request                              */
/* ========================================================================== */

static void on_foo(const hlc_request_t* req, hlc_response_t* res, void* userdata) {
    handler_ctx_t* ctx = (handler_ctx_t*)userdata;

    /* ---- 1. Open root span from inbound context ---------------------------- */

    pt_context_reader_t req_ctx_reader = {
        hlc_request_headers_handle(req),
        hlc_headers_get,
    };
    pt_span_t span = pt_agent_new_span_with_reader(
        ctx->agent, "C Web Server", "/foo", &req_ctx_reader);

    pt_span_set_remote_address(span, hlc_request_remote_addr(req));

    char endpoint[512];
    const char* host_hdr = hlc_request_get_header(req, "Host");
    if (host_hdr && host_hdr[0] != '\0') {
        snprintf(endpoint, sizeof(endpoint), "%s", host_hdr);
    } else {
        snprintf(endpoint, sizeof(endpoint), "%s:%d",
                 hlc_request_local_addr(req), hlc_request_local_port(req));
    }
    pt_span_set_end_point(span, endpoint);

    /* Record inbound request headers on the span. */
    pt_header_reader_t req_hdr_reader = {
        hlc_request_headers_handle(req),
        hlc_headers_get,
        hlc_headers_for_each,
    };
    pt_span_record_header(span, PT_HTTP_REQUEST, &req_hdr_reader);

    /* ---- 2. Outbound HTTP call traced as a child span event ---------------- */

    const char* downstream_host = "localhost:8090";

    pt_span_event_t se = pt_span_new_event(span, "TestSpanEvent");
    pt_span_event_set_service_type(se, PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    pt_span_event_set_end_point(se, downstream_host);
    pt_span_event_set_destination(se, downstream_host);

    /* Inject our trace context into the outbound headers.  The headers map
     * is created here and destroyed at the end of the event. */
    hlc_mutable_headers_t out_headers = hlc_mutable_headers_create();
    pt_context_writer_t   ctx_writer  = {
        hlc_mutable_headers_handle(out_headers),
        hlc_headers_set,
    };
    pt_span_inject_context(span, &ctx_writer);

    /* Annotate the outbound URL. */
    pt_annotation_t anno = pt_span_event_get_annotations(se);
    pt_annotation_append_string(anno, PT_ANNOTATION_HTTP_URL, "localhost:8090/foo");

    /* Actually issue the GET. */
    hlc_client_t cli = hlc_client_create(downstream_host);
    hlc_result_t result = hlc_client_get(cli, "/foo",
                                         hlc_mutable_headers_handle(out_headers));

    if (hlc_result_ok(result)) {
        int status = hlc_result_status(result);
        if (status == 200) {
            printf("%s\n", hlc_result_body(result));
        }
        pt_annotation_append_int(anno, PT_ANNOTATION_HTTP_STATUS_CODE, status);
    } else {
        const char* err_msg = hlc_result_error_message(result);
        printf("HTTP error: %s\n", err_msg);
        pt_span_event_set_error(se, err_msg);
        pt_span_set_error(span, "http client error");
    }

    pt_annotation_destroy(anno);
    hlc_result_destroy(result);
    hlc_client_destroy(cli);
    hlc_mutable_headers_destroy(out_headers);

    pt_span_end_event(span);
    pt_span_event_destroy(se);

    /* ---- 3. Second event with a nested async span -------------------------- */

    pt_span_event_t se2 = pt_span_new_event(span, "TestSpanEvent2");

    pt_span_t async_span = pt_span_new_async_span(span, "New Thread");
    pt_span_event_t async_event = pt_span_new_event(async_span, "ThreadSpanEvent");
    pt_span_end_event(async_span);
    pt_span_event_destroy(async_event);
    pt_span_end(async_span);
    pt_span_destroy(async_span);

    pt_span_end_event(span);
    pt_span_event_destroy(se2);

    /* ---- 4. Finalise ------------------------------------------------------- */

    pt_span_set_status_code(span, 200);
    pt_span_end(span);
    pt_span_destroy(span);

    /* Echo the request body back, like the original tutorial. */
    hlc_response_set_content(res, hlc_request_body(req), "text/plain");
}

/* ========================================================================== */
/* Entry point                                                                  */
/* ========================================================================== */

int main(void) {
    setenv("PINPOINT_CPP_CONFIG_FILE",      "/tmp/pinpoint-config.yaml", 0);
    setenv("PINPOINT_CPP_APPLICATION_NAME", "c-tutorial",                0);

    pt_agent_t agent = pt_create_agent();
    if (!agent) {
        fprintf(stderr, "failed to create pinpoint agent\n");
        return 1;
    }

    /* Give the agent a moment to come up — matches the C++ tutorial. */
    sleep_sec(5);

    handler_ctx_t ctx = { agent };

    hlc_server_t server = hlc_server_create();
    hlc_server_get(server, "/foo", on_foo, &ctx);

    printf("c-tutorial listening on 0.0.0.0:8080\n");
    (void)hlc_server_listen(server, "0.0.0.0", 8080);

    hlc_server_destroy(server);
    pt_agent_shutdown(agent);
    pt_agent_destroy(agent);
    return 0;
}
