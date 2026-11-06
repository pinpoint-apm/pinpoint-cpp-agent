#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
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

/* The phase-2 agent's log sink: called from several agent threads at once. */
static void count_log_line(void* userdata, const char* level, const char* message) {
    (void)level;
    (void)message;
    atomic_fetch_add((atomic_int*)userdata, 1);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char* env_or(const char* name, const char* fallback) {
    const char* value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
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
    char trace_id[PT_TRACE_ID_MAX];
    char short_trace_id[8];
    size_t trace_len;
    size_t short_len;
    pthread_t worker;
    async_work_t async_work;
    char default_agent_name[32];

    if (getenv("PINPOINT_CPP_COLLECTOR_HOST") == NULL ||
        getenv("PINPOINT_CPP_COLLECTOR_HOST")[0] == '\0') {
        fprintf(stderr, "C API scenario: PINPOINT_CPP_COLLECTOR_HOST must be set\n");
        return 2;
    }
    set_default_env("PINPOINT_CPP_APPLICATION_NAME", "cpp-it-c-api");
    snprintf(default_agent_name, sizeof(default_agent_name), "cpp-it-c-%ld",
             (long)getpid());
    set_default_env("PINPOINT_CPP_AGENT_NAME", default_agent_name);
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
    /* The file the environment already names, set through the builder too. */
    const char* config_file = getenv("PINPOINT_CPP_CONFIG_FILE");
    if (config_file != NULL && config_file[0] != '\0') {
        pt_agent_options_set_config_file(options, config_file);
    }
    const int started = pt_start_agent(options);
    pt_agent_options_free(options);
    if (!started) {
        fprintf(stderr, "C API scenario: agent start failed; check the agent log\n");
        return 1;
    }
    pt_agent_t agent = pt_global_agent();
    if (agent == NULL) {
        fprintf(stderr, "C API scenario: agent handle unavailable\n");
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
    pt_span_set_start_time_ms(span, now_ms() - 1);
    pt_span_record_header(span, PT_HTTP_REQUEST, &request_reader);
    pt_span_set_remote_address(span, "192.0.2.10");
    pt_span_set_end_point(span, "localhost:8092");
    pt_span_set_acceptor_host(span, "localhost:8092");
    pt_trace_http_server_request_with_cookie(
        span, "127.0.0.1", "localhost:8092", &request_reader, &cookie_reader);
    pt_span_set_logging(span, &logging_writer);

    pt_span_set_annotation_int(span, 9300, 42);
    pt_span_set_annotation_long(span, 9301, INT64_C(1234567890123));
    pt_span_set_annotation_string(span, 9302, "c-api-span");
    pt_span_set_annotation_string_string(span, 9303, "left", "right");

    pt_span_event_t event = pt_span_new_event_with_type(
        span, "c-api-event-initial", PT_SERVICE_TYPE_CPP_HTTP_CLIENT);
    if (pt_span_get_event(span) != event) {
        fprintf(stderr, "C API scenario: the open event is not the span's current one\n");
        return 8;
    }
    pt_span_event_set_start_time_ms(event, now_ms());
    pt_span_event_set_operation_name(event, "c-api-event");
    pt_span_event_set_destination(event, "localhost:8091");
    pt_span_event_set_end_point(event, "localhost:8091");
    pt_trace_http_client_request_with_cookie(
        event, "localhost:8091", "http://localhost:8091/trace",
        &request_reader, &cookie_reader);
    pt_span_event_inject_context(event, &context_writer);
    pt_trace_http_client_response(event, 200, &response_reader);
    pt_span_event_record_header(event, PT_HTTP_RESPONSE, &response_reader);

    pt_span_event_set_annotation_int(event, 9400, -1);
    pt_span_event_set_annotation_long(event, 9401, -2);
    pt_span_event_set_annotation_string(event, 9402, "c-api-event");
    pt_span_event_set_annotation_string_string(event, 9403, "key", "value");
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

    event = pt_span_new_event(span, "c-api-plain-error");
    pt_span_event_set_error(event, "plain C API event failure");
    pt_span_event_end(event);
    event = pt_span_new_event(span, "c-api-named-error");
    pt_span_event_set_error_named(event, "CNamedFailure", "named C API event failure");
    pt_span_event_end(event);

    /* An async span starts from the event that is open when it is created;
     * without one the agent hands back a span that records nothing. */
    pt_span_event_t async_spawn = pt_span_new_event(span, "c-api-async-spawn");
    async_work.span = pt_span_new_async_span(span, "c-api-async");
    async_work.complete = 0;
    if (pthread_create(&worker, NULL, run_async, &async_work) != 0) {
        fprintf(stderr, "C API scenario: could not start async worker\n");
        return 5;
    }
    pthread_join(worker, NULL);
    pt_span_destroy(async_work.span);
    pt_span_event_end(async_spawn);

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
    pt_span_set_annotation_int(NULL, 1, 1);
    pt_span_event_set_annotation_int(NULL, 1, 1);

    pt_agent_t global = pt_global_agent();
    if (global == NULL || !pt_agent_is_enabled(global)) {
        fprintf(stderr, "C API scenario: global agent lookup failed\n");
        return 7;
    }
    pt_agent_destroy(global);

    /* The plain overloads and span-level setters the request above skips. */
    pt_span_t plain = pt_agent_new_span(agent, "c-api-plain", "/c-api-plain");
    pt_trace_http_server_request(plain, "127.0.0.1", "localhost:8092", &request_reader);
    event = pt_span_new_event(plain, "c-api-plain-client");
    pt_trace_http_client_request(event, "localhost:8091",
                                 "http://localhost:8091/plain?x=1", &request_reader);
    pt_span_event_end(event);
    pt_span_set_error(plain, "plain C API span failure");
    pt_span_set_logging_flag(plain);
    pt_span_force_url_stat(plain, "/c-api-plain/{id}", "GET", 500);
    pt_span_end(plain);
    pt_span_destroy(plain);

    /* Continue the trace from the context the client event injected. */
    pt_context_reader_t injected_reader = {&context_headers, map_get};
    pt_span_t continued = pt_agent_new_span_with_reader(
        agent, "c-api-continued", "/c-api-continued", &injected_reader);
    char continued_trace_id[PT_TRACE_ID_MAX];
    const int continued_trace =
        continued != NULL &&
        pt_span_get_trace_id(continued, continued_trace_id, sizeof(continued_trace_id)) > 0 &&
        strcmp(continued_trace_id, trace_id) == 0;
    pt_span_set_error_named(continued, "CContinuedFailure", "continued span failure");
    pt_span_end(continued);
    pt_span_destroy(continued);
    if (!continued_trace) {
        fprintf(stderr, "C API scenario: the continued span did not join the trace\n");
        return 9;
    }

    /* A config file outranks YAML, so a YAML reload is refused here. */
    char restart_required[64];
    size_t restart_required_len = 0;
    if (config_file != NULL && config_file[0] != '\0' &&
        pt_reload_agent_config("Sampling:\n  CounterRate: 1\n", restart_required,
                               sizeof(restart_required), &restart_required_len) !=
            PT_RELOAD_CONFIG_FILE_ACTIVE) {
        fprintf(stderr, "C API scenario: a YAML reload was not refused under a config file\n");
        return 10;
    }

    pt_agent_shutdown(agent);
    pt_agent_shutdown(agent);
    pt_agent_destroy(agent);

    /* Phase 2: restart from inline YAML. The isolated prefix keeps the
     * suite's PINPOINT_CPP_CONFIG_FILE out, so the YAML is the source and a
     * reload can apply; the collector settings follow the environment. */
    static const char yaml_format[] =
        "Enable: true\n"
        "ApplicationName: %s\n"
        "AgentName: %s-yaml\n"
        "UidVersion: v3\n"
        "Log:\n"
        "  Level: info\n"
        "Collector:\n"
        "  Host: %s\n"
        "  AgentPort: %s\n"
        "  SpanPort: %s\n"
        "  StatPort: %s\n"
        "Sampling:\n"
        "  Type: COUNTER\n"
        "  CounterRate: 1\n"
        "Http:\n"
        "  Server:\n"
        "    RecordRequestParam: true\n";
    const char* app_name = env_or("PINPOINT_CPP_APPLICATION_NAME", "cpp-it-c-api");
    const char* agent_name = env_or("PINPOINT_CPP_AGENT_NAME", default_agent_name);
    const char* collector_host = getenv("PINPOINT_CPP_COLLECTOR_HOST");
    const char* agent_port = env_or("PINPOINT_CPP_COLLECTOR_AGENT_PORT", "9991");
    const char* span_port = env_or("PINPOINT_CPP_COLLECTOR_SPAN_PORT", "9993");
    const char* stat_port = env_or("PINPOINT_CPP_COLLECTOR_STAT_PORT", "9992");
    char yaml[1024];
    snprintf(yaml, sizeof(yaml), yaml_format, app_name, agent_name, collector_host,
             agent_port, span_port, stat_port);

    atomic_int log_lines = 0;
    options = pt_agent_options_new();
    if (options == NULL) {
        fprintf(stderr, "C API scenario: phase-2 options allocation failed\n");
        return 11;
    }
    pt_agent_options_set_env_prefix(options, "PINPOINT_CPP_C_API_YAML");
    pt_agent_options_set_config_yaml(options, yaml);
    pt_agent_options_set_agent_version(options, "c-api-scenario");
    pt_agent_options_set_app_type(options, PT_APP_TYPE_CPP);
    pt_agent_options_set_log_sink(options, count_log_line, &log_lines);
    const int restarted = pt_start_agent(options);
    pt_agent_options_free(options);
    pt_agent_t yaml_agent = pt_global_agent();
    if (!restarted || yaml_agent == NULL || !wait_until_enabled(yaml_agent)) {
        fprintf(stderr, "C API scenario: the YAML-configured agent did not come up\n");
        return 12;
    }

    /* The reloadable part applies; the renamed application waits for a restart. */
    char renamed_app[128];
    char renamed_yaml[1024];
    snprintf(renamed_app, sizeof(renamed_app), "%s-renamed", app_name);
    snprintf(renamed_yaml, sizeof(renamed_yaml), yaml_format, renamed_app, agent_name,
             collector_host, agent_port, span_port, stat_port);
    const int reload_applied =
        pt_reload_agent_config(renamed_yaml, restart_required, sizeof(restart_required),
                               &restart_required_len) == PT_RELOAD_APPLIED &&
        strstr(restart_required, "ApplicationName") != NULL;
    const int reload_refused =
        pt_reload_agent_config("- not a mapping", NULL, 0, NULL) == PT_RELOAD_INVALID_YAML;

    pt_span_t yaml_span = pt_agent_new_span(yaml_agent, "c-api-yaml", "/c-api-yaml");
    pt_trace_http_server_request_with_query(yaml_span, "127.0.0.1", "localhost:8092",
                                            &request_reader, &cookie_reader, "q=yaml&page=1");
    pt_trace_http_server_response(yaml_span, "/c-api-yaml", "GET", 200, &response_reader);
    pt_span_end(yaml_span);
    pt_span_destroy(yaml_span);

    const int lines_before_shutdown = atomic_load(&log_lines);
    pt_agent_shutdown(yaml_agent);
    /* Detached synchronously: the warning the refused reload logs must not
     * reach the sink once pt_agent_shutdown() has returned. */
    const int lines_at_shutdown = atomic_load(&log_lines);
    const int sink_detached =
        pt_reload_agent_config("Sampling: {}", NULL, 0, NULL) == PT_RELOAD_NO_AGENT &&
        atomic_load(&log_lines) == lines_at_shutdown;
    pt_agent_destroy(yaml_agent);
    if (!reload_applied || !reload_refused || lines_before_shutdown == 0 || !sink_detached) {
        fprintf(stderr,
                "C API scenario: phase 2 failed (applied=%d refused=%d lines=%d detached=%d)\n",
                reload_applied, reload_refused, lines_before_shutdown, sink_detached);
        return 13;
    }

    printf("{\"status\":\"ok\",\"agent_enabled\":true,"
           "\"sampled\":true,\"trace_id\":\"%s\","
           "\"trace_id_length\":%zu,\"short_buffer_truncated\":true,"
           "\"context_injected\":true,\"logging_context\":true,"
           "\"async_complete\":true,\"continued_trace\":true,"
           "\"yaml_reload_applied\":true,\"yaml_reload_refused\":true,"
           "\"log_sink_detached\":true}\n",
           trace_id, trace_len);
    return 0;
}
