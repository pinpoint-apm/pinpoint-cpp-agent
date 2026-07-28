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

/*
 * Example nginx module showing the Pinpoint C++ agent's pre-fork lifecycle
 * (see doc/prefork.md). The contract in one line: the master process makes NO
 * agent API calls — each worker starts its own agent after fork().
 *
 *   init_module   (master)      -> nothing agent-related
 *   init_process  (each worker) -> pt_start_agent()
 *   exit_process  (each worker) -> pt_agent_shutdown() + pt_agent_destroy()
 *
 * Request tracing is deliberately minimal: one span per completed request,
 * recorded retroactively from the LOG phase (nginx knows the request's start
 * time, so the span still carries the real timing). Incoming Pinpoint-*
 * headers are honored, so this nginx joins distributed traces started by its
 * callers. Propagating context onward to upstreams is out of scope here — see
 * README.md.
 *
 * nginx.conf:
 *
 *   load_module modules/ngx_http_pinpoint_module.so;
 *   http {
 *       pinpoint_enable       on;
 *       pinpoint_config_file  /etc/pinpoint/nginx-agent.yaml;
 *       ...
 *   }
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <pinpoint/tracer_c.h>

typedef struct {
    ngx_flag_t enable;
    ngx_str_t  config_file;
} ngx_http_pinpoint_main_conf_t;

/* One agent per worker process (set in init_process, cleared in exit_process). */
static pt_agent_t g_agent = NULL;

static ngx_int_t ngx_http_pinpoint_postconfiguration(ngx_conf_t *cf);
static void *ngx_http_pinpoint_create_main_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_pinpoint_init_process(ngx_cycle_t *cycle);
static void ngx_http_pinpoint_exit_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_pinpoint_log_handler(ngx_http_request_t *r);

static ngx_command_t ngx_http_pinpoint_commands[] = {

    { ngx_string("pinpoint_enable"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_pinpoint_main_conf_t, enable),
      NULL },

    { ngx_string("pinpoint_config_file"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_pinpoint_main_conf_t, config_file),
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_http_pinpoint_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_pinpoint_postconfiguration,   /* postconfiguration */

    ngx_http_pinpoint_create_main_conf,    /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

ngx_module_t ngx_http_pinpoint_module = {
    NGX_MODULE_V1,
    &ngx_http_pinpoint_module_ctx,         /* module context */
    ngx_http_pinpoint_commands,            /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module (master: NO agent calls) */
    ngx_http_pinpoint_init_process,        /* init process (worker) */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_http_pinpoint_exit_process,        /* exit process (worker) */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static void *
ngx_http_pinpoint_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_pinpoint_main_conf_t *pmcf;

    pmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_pinpoint_main_conf_t));
    if (pmcf == NULL) {
        return NULL;
    }
    pmcf->enable = NGX_CONF_UNSET;
    /* config_file: ngx_pcalloc left it as the empty string */
    return pmcf;
}

static ngx_int_t
ngx_http_pinpoint_postconfiguration(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    /* LOG phase: the request is complete, status and timings are final. */
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_pinpoint_log_handler;

    return NGX_OK;
}

/* ==========================================================================
 * Worker lifecycle
 * ========================================================================== */

static ngx_int_t
ngx_http_pinpoint_init_process(ngx_cycle_t *cycle)
{
    ngx_http_pinpoint_main_conf_t *pmcf;
    pt_agent_options_t             opts;
    char                           config_file[1024];
    char                           suffix[32];
    char                           server_info[64];

    /* Only request-serving processes trace. The cache manager/loader also run
     * init_process; starting an agent there would register useless
     * instances. */
    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;
    }

    pmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_pinpoint_module);
    if (pmcf == NULL || pmcf->enable != 1) {
        return NGX_OK;
    }

    opts = pt_agent_options_new();
    if (opts == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "pinpoint: agent options allocation failed");
        return NGX_OK;  /* never fail worker startup over tracing */
    }

    if (pmcf->config_file.len > 0
        && pmcf->config_file.len < sizeof(config_file)) {
        ngx_memcpy(config_file, pmcf->config_file.data, pmcf->config_file.len);
        config_file[pmcf->config_file.len] = '\0';
        pt_agent_options_set_config_file(opts, config_file);
    }

    /* ngx_worker is this worker's slot number (0..N-1). It is stable across
     * worker respawns and config reloads, so the per-worker agent id
     * ("<AgentId>-w0", "<AgentId>-w1", ...) survives restarts. In single
     * process mode there is only one slot. */
    ngx_snprintf((u_char *) suffix, sizeof(suffix) - 1, "w%ui%Z",
                 (ngx_process == NGX_PROCESS_SINGLE) ? 0 : ngx_worker);
    pt_agent_options_set_instance_suffix(opts, suffix);

    ngx_snprintf((u_char *) server_info, sizeof(server_info) - 1,
                 "nginx/%s%Z", NGINX_VERSION);
    pt_agent_options_set_server_metadata(opts, server_info, NULL, 0, NULL, 0);

    /* Starts the agent in THIS worker: first gRPC use in this process, own
     * threads, own collector registration. Returns immediately; registration
     * completes asynchronously (pt_agent_is_enabled() flips to non-zero). */
    g_agent = pt_start_agent(opts);
    pt_agent_options_free(opts);

    if (g_agent == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "pinpoint: agent start failed; worker runs untraced");
    }

    return NGX_OK;
}

static void
ngx_http_pinpoint_exit_process(ngx_cycle_t *cycle)
{
    if (g_agent != NULL) {
        /* Drains queued spans (bounded) and joins the agent's threads. */
        pt_agent_shutdown(g_agent);
        pt_agent_destroy(g_agent);
        g_agent = NULL;
    }
}

/* ==========================================================================
 * Request tracing (LOG phase)
 * ========================================================================== */

/* pt_context_reader_t callback: look an incoming header up by name.
 *
 * Returns a pointer into the request's header storage. nginx NUL-terminates
 * HTTP/1 header values in place; for HTTP/2 and HTTP/3 that is not guaranteed,
 * so a production module should copy the value into a NUL-terminated buffer
 * allocated from r->pool instead of returning h[i].value.data directly. */
static const char *
ngx_http_pinpoint_header_get(void *userdata, const char *key)
{
    ngx_http_request_t *r = userdata;
    size_t              key_len = ngx_strlen(key);
    ngx_list_part_t    *part = &r->headers_in.headers.part;
    ngx_table_elt_t    *h = part->elts;
    ngx_uint_t          i;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }
        if (h[i].key.len == key_len
            && ngx_strncasecmp(h[i].key.data, (u_char *) key, key_len) == 0) {
            return (const char *) h[i].value.data;
        }
    }
    return NULL;
}

/* Copies an ngx_str_t into a NUL-terminated stack buffer (truncating). */
static void
ngx_http_pinpoint_to_cstr(const ngx_str_t *src, char *dst, size_t dst_size)
{
    size_t n = src->len < dst_size - 1 ? src->len : dst_size - 1;
    ngx_memcpy(dst, src->data, n);
    dst[n] = '\0';
}

static ngx_int_t
ngx_http_pinpoint_log_handler(ngx_http_request_t *r)
{
    pt_context_reader_t reader;
    pt_span_t           span;
    char                uri[1024];
    char                method[16];
    char                remote_addr[128];
    char                endpoint[256];
    int64_t             start_ms;
    int                 status;

    if (g_agent == NULL || !pt_agent_is_enabled(g_agent)) {
        return NGX_OK;
    }
    if (r != r->main) {
        return NGX_OK;  /* trace the main request only, not subrequests */
    }

    ngx_http_pinpoint_to_cstr(&r->uri, uri, sizeof(uri));
    ngx_http_pinpoint_to_cstr(&r->method_name, method, sizeof(method));

    /* Continue a distributed trace when the caller sent Pinpoint-* headers;
     * start a fresh (sampled-by-config) trace otherwise. */
    reader.userdata = r;
    reader.get = ngx_http_pinpoint_header_get;

    span = pt_agent_new_span_with_method(g_agent, "NGINX", uri, method, &reader);
    if (span == NULL) {
        return NGX_OK;
    }

    /* Retroactive span: the request already ran, so hand the span nginx's
     * request start time and the final status. */
    start_ms = (int64_t) r->start_sec * 1000 + (int64_t) r->start_msec;
    pt_span_set_start_time_ms(span, start_ms);

    ngx_http_pinpoint_to_cstr(&r->connection->addr_text, remote_addr,
                              sizeof(remote_addr));
    pt_span_set_remote_address(span, remote_addr);

    if (r->headers_in.server.len > 0) {
        ngx_http_pinpoint_to_cstr(&r->headers_in.server, endpoint,
                                  sizeof(endpoint));
        pt_span_set_end_point(span, endpoint);
        pt_span_set_acceptor_host(span, endpoint);
    }

    status = r->err_status ? (int) r->err_status
                           : (int) r->headers_out.status;
    pt_span_set_status_code(span, status);
    pt_span_set_url_stat(span, uri, method, status);

    pt_span_end(span);
    pt_span_destroy(span);

    return NGX_OK;
}
