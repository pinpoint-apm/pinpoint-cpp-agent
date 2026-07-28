#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "pinpoint/tracer_c.h"

#define MAX_HEADERS 32
#define MAX_KEY 96
#define MAX_VALUE 256

typedef struct {
    char key[MAX_KEY];
    char value[MAX_VALUE];
} header_entry_t;

typedef struct {
    header_entry_t entries[MAX_HEADERS];
    size_t count;
} header_map_t;

typedef struct {
    pt_span_t span;
    int complete;
} async_work_t;

static const char* map_get(void* userdata, const char* key) {
    header_map_t* map = (header_map_t*)userdata;
    size_t i;
    for (i = 0; i < map->count; ++i) {
        if (strcasecmp(map->entries[i].key, key) == 0) {
            return map->entries[i].value;
        }
    }
    return NULL;
}

static void map_set(void* userdata, const char* key, const char* value) {
    header_map_t* map = (header_map_t*)userdata;
    size_t i;
    for (i = 0; i < map->count; ++i) {
        if (strcasecmp(map->entries[i].key, key) == 0) {
            snprintf(map->entries[i].value, MAX_VALUE, "%s", value);
            return;
        }
    }
    if (map->count < MAX_HEADERS) {
        snprintf(map->entries[map->count].key, MAX_KEY, "%s", key);
        snprintf(map->entries[map->count].value, MAX_VALUE, "%s", value);
        ++map->count;
    }
}

static void map_for_each(void* userdata, pt_header_foreach_cb callback,
                         void* callback_userdata) {
    header_map_t* map = (header_map_t*)userdata;
    size_t i;
    for (i = 0; i < map->count; ++i) {
        if (callback(map->entries[i].key, map->entries[i].value,
                     callback_userdata) != 0) {
            break;
        }
    }
}

static void emit_callstack(void* userdata, pt_callstack_frame_cb callback,
                           void* callback_userdata) {
    (void)userdata;
    callback("c_api_scenario", "run_scenario", __FILE__, __LINE__,
             callback_userdata);
    callback("c_api_scenario", "synthetic_failure", __FILE__, __LINE__,
             callback_userdata);
}

static void* run_async(void* arg) {
    async_work_t* work = (async_work_t*)arg;
    pt_span_event_t event = pt_span_new_event(work->span, "c-api-async-work");
    pt_span_event_end(event);
    pt_span_event_destroy(event);
    pt_span_end(work->span);
    work->complete = 1;
    return NULL;
}

static void set_default_env(const char* name, const char* value) {
    if (getenv(name) == NULL) {
        setenv(name, value, 0);
    }
}

static int wait_until_enabled(pt_agent_t agent) {
    const char* timeout_value = getenv("PINPOINT_IT_AGENT_TIMEOUT");
    int timeout = timeout_value ? atoi(timeout_value) : 30;
    int elapsed_ms = 0;
    while (elapsed_ms < timeout * 1000) {
        if (pt_agent_is_enabled(agent)) return 1;
        usleep(100000);
        elapsed_ms += 100;
    }
    return pt_agent_is_enabled(agent);
}

int main(void) {
    const char* args[] = {"--scenario=c-api"};
    const char* libs[] = {"pinpoint-tracer-c"};
    header_map_t request_headers = {0};
    header_map_t response_headers = {0};
    header_map_t context_headers = {0};
    header_map_t logging_headers = {0};
    header_map_t cookie_headers = {0};
    unsigned char uid[16];
    char trace_id[PT_TRACE_ID_MAX];
    char short_trace_id[8];
    size_t trace_len;
    size_t short_len;
    size_t i;
    pthread_t worker;
    async_work_t async_work;
    char default_agent_id[25];

    if (getenv("PINPOINT_CPP_COLLECTOR_HOST") == NULL ||
        getenv("PINPOINT_CPP_COLLECTOR_HOST")[0] == '\0') {
        fprintf(stderr, "C API scenario: PINPOINT_CPP_COLLECTOR_HOST must be set\n");
        return 2;
    }
    set_default_env("PINPOINT_CPP_APPLICATION_NAME", "cpp-it-c-api");
    snprintf(default_agent_id, sizeof(default_agent_id), "cpp-it-c-%ld",
             (long)getpid());
    set_default_env("PINPOINT_CPP_AGENT_ID", default_agent_id);
    set_default_env("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true");
    set_default_env("PINPOINT_CPP_SQL_ENABLE_SQL_STATS", "true");
    set_default_env("PINPOINT_CPP_SQL_TRACE_BIND_VALUE", "true");
    set_default_env("PINPOINT_CPP_ENABLE_CALLSTACK_TRACE", "true");

    pt_agent_options_t options = pt_agent_options_new();
    if (options == NULL) {
        fprintf(stderr, "C API scenario: options allocation failed\n");
        return 1;
    }
    pt_agent_options_set_server_metadata(options, "cpp-it-c-api", args, 1, libs, 1);
    pt_agent_t agent = pt_start_agent(options);
    pt_agent_options_free(options);
    if (agent == NULL) {
        fprintf(stderr, "C API scenario: agent creation failed\n");
        return 1;
    }
    if (!wait_until_enabled(agent)) {
        fprintf(stderr, "C API scenario: collector registration timed out\n");
        pt_agent_shutdown(agent);
        pt_agent_destroy(agent);
        return 2;
    }

    map_set(&request_headers, "User-Agent", "pinpoint-cpp-c-api-it");
    map_set(&request_headers, "X-Request-ID", "c-api-request");
    map_set(&request_headers, "X-Forwarded-For", "192.0.2.10, 192.0.2.11");
    map_set(&request_headers, "Pinpoint-ProxyNginx", "t=1 D=2");
    map_set(&cookie_headers, "session_id", "c-api-session");
    map_set(&cookie_headers, "token", "c-api-token");
    map_set(&response_headers, "Content-Type", "application/json");
    map_set(&response_headers, "X-Response-Time", "1ms");

    pt_context_reader_t context_reader = {&request_headers, map_get};
    pt_header_reader_t request_reader = {
        &request_headers, map_get, map_for_each};
    pt_header_reader_t cookie_reader = {
        &cookie_headers, map_get, map_for_each};
    pt_header_reader_t response_reader = {
        &response_headers, map_get, map_for_each};
    pt_context_writer_t context_writer = {&context_headers, map_set};
    pt_context_writer_t logging_writer = {&logging_headers, map_set};
    pt_callstack_reader_t callstack = {NULL, emit_callstack};

    pt_span_t span = pt_agent_new_span_with_method(
        agent, "c-api-scenario", "/c-api", "GET", &context_reader);
    if (span == NULL || !pt_span_is_sampled(span)) {
        fprintf(stderr, "C API scenario: expected a sampled span\n");
        pt_span_destroy(span);
        pt_agent_shutdown(agent);
        pt_agent_destroy(agent);
        return 3;
    }

    trace_len = pt_span_get_trace_id(span, trace_id, sizeof(trace_id));
    short_len = pt_span_get_trace_id(span, short_trace_id, sizeof(short_trace_id));
    if (trace_len == 0 || short_len != trace_len ||
        short_trace_id[sizeof(short_trace_id) - 1] != '\0' ||
        pt_span_get_span_id(span) == 0) {
        fprintf(stderr, "C API scenario: trace identifier contract failed\n");
        return 4;
    }

    pt_span_set_service_type(span, PT_SERVICE_TYPE_CPP);
    pt_span_set_remote_address(span, "192.0.2.10");
    pt_span_set_end_point(span, "localhost:8092");
    pt_span_set_acceptor_host(span, "localhost:8092");
    pt_trace_http_server_request_with_cookie(
        span, "127.0.0.1", "localhost:8092", &request_reader, &cookie_reader);
    pt_span_set_logging(span, &logging_writer);

    pt_annotation_t span_annotation = pt_span_get_annotations(span);
    pt_annotation_append_int(span_annotation, 9300, 42);
    pt_annotation_append_long(span_annotation, 9301, INT64_C(1234567890123));
    pt_annotation_append_string(span_annotation, 9302, "c-api-span");
    pt_annotation_append_string_string(span_annotation, 9303, "left", "right");
    pt_annotation_append_int_string_string(span_annotation, 9304, 7, "a", "b");
    for (i = 0; i < sizeof(uid); ++i) uid[i] = (unsigned char)i;
    pt_annotation_append_sql_uid_string_string(
        span_annotation, 9305, uid, 16, "SELECT ?", "1");
    pt_annotation_append_long_int_int_byte_byte_string(
        span_annotation, 9306, 100, 10, 20, 1, 2, "network");
    pt_annotation_destroy(span_annotation);

    pt_span_event_t event = pt_span_new_event_with_type(
        span, "c-api-event-initial", PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    pt_span_event_set_operation_name(event, "c-api-event");
    pt_span_event_set_destination(event, "localhost:8091");
    pt_span_event_set_end_point(event, "localhost:8091");
    pt_trace_http_client_request_with_cookie(
        event, "localhost:8091", "http://localhost:8091/trace",
        &request_reader, &cookie_reader);
    pt_span_event_inject_context(event, &context_writer);
    pt_trace_http_client_response(event, 200, &response_reader);

    pt_annotation_t event_annotation = pt_span_event_get_annotations(event);
    pt_annotation_append_int(event_annotation, 9400, -1);
    pt_annotation_append_long(event_annotation, 9401, -2);
    pt_annotation_append_string(event_annotation, 9402, "c-api-event");
    pt_annotation_append_string_string(event_annotation, 9403, "key", "value");
    pt_annotation_append_int_string_string(event_annotation, 9404, 9, "x", "y");
    pt_annotation_append_sql_uid_string_string(
        event_annotation, 9405, uid, 16, "sql", "args");
    pt_annotation_append_long_int_int_byte_byte_string(
        event_annotation, 9406, 5, 6, 7, 8, 9, "details");
    pt_annotation_destroy(event_annotation);
    pt_span_event_end(event);
    pt_span_event_destroy(event);

    event = pt_span_new_event(span, "c-api-sql");
    pt_span_event_set_service_type(event, PT_SERVICE_TYPE_MYSQL_QUERY);
    const char* sql_args[] = {"17", "admin"};
    pt_span_event_set_sql_query(
        event, "SELECT name FROM users WHERE id = ? AND role = ?",
        sql_args, 2);
    pt_span_event_end(event);
    pt_span_event_destroy(event);

    event = pt_span_new_event(span, "c-api-callstack-error");
    pt_span_event_set_error_with_callstack(
        event, "CScenarioFailure", "synthetic C API failure", &callstack);
    pt_span_event_end(event);
    pt_span_event_destroy(event);

    async_work.span = pt_span_new_async_span(span, "c-api-async");
    async_work.complete = 0;
    if (pthread_create(&worker, NULL, run_async, &async_work) != 0) {
        fprintf(stderr, "C API scenario: could not start async worker\n");
        return 5;
    }
    pthread_join(worker, NULL);
    pt_span_destroy(async_work.span);

    pt_span_set_status_code(span, 200);
    pt_span_set_url_stat(span, "/c-api", "GET", 200);
    pt_trace_http_server_response(span, "/c-api", "GET", 200, &response_reader);
    pt_span_end(span);
    pt_span_destroy(span);

    if (map_get(&context_headers, PT_HEADER_TRACE_ID) == NULL ||
        map_get(&context_headers, PT_HEADER_SPAN_ID) == NULL ||
        map_get(&context_headers, PT_HEADER_PARENT_SPAN_ID) == NULL ||
        map_get(&logging_headers, "PtxId") == NULL ||
        map_get(&logging_headers, "PspanId") == NULL ||
        !async_work.complete) {
        fprintf(stderr, "C API scenario: propagation/logging/async invariant failed\n");
        return 6;
    }

    /* Exercise the public null-handle exception firewall. */
    pt_span_end(NULL);
    pt_span_event_end(NULL);
    pt_annotation_append_int(NULL, 1, 1);

    pt_agent_t global = pt_global_agent();
    if (global == NULL || !pt_agent_is_enabled(global)) {
        fprintf(stderr, "C API scenario: global agent lookup failed\n");
        return 7;
    }
    pt_agent_destroy(global);

    printf("{\"status\":\"ok\",\"agent_enabled\":true,"
           "\"sampled\":true,\"trace_id\":\"%s\","
           "\"trace_id_length\":%zu,\"short_buffer_truncated\":true,"
           "\"context_injected\":true,\"logging_context\":true,"
           "\"async_complete\":true}\n",
           trace_id, trace_len);

    pt_agent_shutdown(agent);
    pt_agent_shutdown(agent);
    pt_agent_destroy(agent);
    return 0;
}
