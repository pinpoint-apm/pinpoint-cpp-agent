/*
 * Copyright 2025 NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file civetweb_example.c
 * @brief Example demonstrating Pinpoint C wrapper integration with CivetWeb
 * 
 * This example shows how to integrate Pinpoint distributed tracing with a
 * CivetWeb HTTP server using the C wrapper API.
 */

#include "civetweb.h"
#include "../pinpoint_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global Pinpoint agent */
static pinpoint_agent_handle g_agent = NULL;

/* Header collection structure for CivetWeb integration */
typedef struct {
    const char* key;
    const char* value;
} header_pair;

typedef struct {
    header_pair* headers;
    int count;
    int capacity;
} header_collection;

/* Context reader for extracting Pinpoint trace context from request headers */
static int trace_context_reader(const char* key, char* value_out, size_t value_size, void* user_data) {
    struct mg_connection* conn = (struct mg_connection*)user_data;
    const char* value = mg_get_header(conn, key);
    
    if (value != NULL) {
        size_t len = strlen(value);
        if (len < value_size) {
            strcpy(value_out, value);
            return (int)len;
        }
    }
    return 0;
}

/* Header iterator for providing headers to Pinpoint */
static int header_iterator(void* user_data, void* reader_context) {
    header_collection* headers = (header_collection*)user_data;
    
    for (int i = 0; i < headers->count; i++) {
        pinpoint_header_iterator_callback(
            headers->headers[i].key,
            headers->headers[i].value,
            reader_context
        );
    }
    
    return 1;
}

/* Helper function to collect all request headers */
static void collect_request_headers(struct mg_connection* conn, header_collection* headers) {
    const struct mg_request_info* req_info = mg_get_request_info(conn);
    
    headers->count = 0;
    headers->capacity = req_info->num_headers;
    headers->headers = (header_pair*)malloc(sizeof(header_pair) * headers->capacity);
    
    for (int i = 0; i < req_info->num_headers; i++) {
        headers->headers[i].key = req_info->http_headers[i].name;
        headers->headers[i].value = req_info->http_headers[i].value;
        headers->count++;
    }
}

/* Helper function to collect response headers */
static void collect_response_headers(header_collection* headers, const char** header_pairs, int count) {
    headers->count = 0;
    headers->capacity = count;
    headers->headers = (header_pair*)malloc(sizeof(header_pair) * headers->capacity);
    
    for (int i = 0; i < count; i += 2) {
        headers->headers[headers->count].key = header_pairs[i];
        headers->headers[headers->count].value = header_pairs[i + 1];
        headers->count++;
    }
}

/* Free header collection */
static void free_headers(header_collection* headers) {
    if (headers->headers != NULL) {
        free(headers->headers);
        headers->headers = NULL;
    }
    headers->count = 0;
    headers->capacity = 0;
}

/* Handler for /api/users endpoint */
static int handle_users(struct mg_connection* conn, void* cbdata) {
    (void)cbdata; // Unused
    
    const struct mg_request_info* req_info = mg_get_request_info(conn);
    
    /* Create span with trace context propagation */
    pinpoint_span_handle span = pinpoint_new_span_with_context(
        g_agent,
        req_info->request_method,
        req_info->request_uri,
        trace_context_reader,
        conn
    );
    
    if (span == NULL) {
        mg_printf(conn, "HTTP/1.1 500 Internal Server Error\r\n"
                       "Content-Type: text/plain\r\n\r\n"
                       "Tracing initialization failed\n");
        return 500;
    }
    
    /* Collect request headers */
    header_collection request_headers;
    collect_request_headers(conn, &request_headers);
    
    /* Get remote address */
    char remote_addr[64];
    snprintf(remote_addr, sizeof(remote_addr), "%s:%d",
             req_info->remote_addr, req_info->remote_port);
    
    /* Trace HTTP server request */
    pinpoint_trace_http_server_request(
        span,
        remote_addr,
        req_info->request_uri,
        header_iterator,
        &request_headers
    );
    
    free_headers(&request_headers);
    
    /* Simulate some business logic */
    const char* json_response = 
        "[\n"
        "  {\"id\": 1, \"name\": \"Alice\", \"email\": \"alice@example.com\"},\n"
        "  {\"id\": 2, \"name\": \"Bob\", \"email\": \"bob@example.com\"},\n"
        "  {\"id\": 3, \"name\": \"Charlie\", \"email\": \"charlie@example.com\"}\n"
        "]\n";
    
    /* Send response */
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %d\r\n"
              "X-Powered-By: Pinpoint-CivetWeb\r\n"
              "\r\n"
              "%s",
              (int)strlen(json_response),
              json_response);
    
    /* Collect response headers */
    const char* response_header_pairs[] = {
        "Content-Type", "application/json",
        "X-Powered-By", "Pinpoint-CivetWeb"
    };
    header_collection response_headers;
    collect_response_headers(&response_headers, response_header_pairs, 4);
    
    /* Trace HTTP server response */
    pinpoint_trace_http_server_response(
        span,
        req_info->request_uri,
        req_info->request_method,
        200,
        header_iterator,
        &response_headers
    );
    
    free_headers(&response_headers);
    
    /* End span */
    pinpoint_span_end(span);
    pinpoint_span_destroy(span);
    
    return 200;
}

/* Handler for /api/health endpoint */
static int handle_health(struct mg_connection* conn, void* cbdata) {
    (void)cbdata; // Unused
    
    const struct mg_request_info* req_info = mg_get_request_info(conn);
    
    /* Create span */
    pinpoint_span_handle span = pinpoint_new_span_with_context(
        g_agent,
        req_info->request_method,
        req_info->request_uri,
        trace_context_reader,
        conn
    );
    
    if (span != NULL) {
        header_collection request_headers;
        collect_request_headers(conn, &request_headers);
        
        char remote_addr[64];
        snprintf(remote_addr, sizeof(remote_addr), "%s:%d",
                 req_info->remote_addr, req_info->remote_port);
        
        pinpoint_trace_http_server_request(
            span,
            remote_addr,
            req_info->request_uri,
            header_iterator,
            &request_headers
        );
        
        free_headers(&request_headers);
    }
    
    /* Send simple health response */
    const char* response = "{\"status\": \"healthy\", \"service\": \"civetweb-example\"}\n";
    
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %d\r\n"
              "\r\n"
              "%s",
              (int)strlen(response),
              response);
    
    if (span != NULL) {
        const char* response_header_pairs[] = {
            "Content-Type", "application/json"
        };
        header_collection response_headers;
        collect_response_headers(&response_headers, response_header_pairs, 2);
        
        pinpoint_trace_http_server_response(
            span,
            req_info->request_uri,
            req_info->request_method,
            200,
            header_iterator,
            &response_headers
        );
        
        free_headers(&response_headers);
        
        pinpoint_span_end(span);
        pinpoint_span_destroy(span);
    }
    
    return 200;
}

/* Handler for root endpoint */
static int handle_root(struct mg_connection* conn, void* cbdata) {
    (void)cbdata; // Unused
    
    const char* html = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>Pinpoint CivetWeb Example</title></head>\n"
        "<body>\n"
        "<h1>Pinpoint CivetWeb Example</h1>\n"
        "<p>This is a demo server showing Pinpoint C wrapper integration with CivetWeb.</p>\n"
        "<h2>Available Endpoints:</h2>\n"
        "<ul>\n"
        "<li><a href=\"/api/users\">/api/users</a> - Get user list</li>\n"
        "<li><a href=\"/api/health\">/api/health</a> - Health check</li>\n"
        "</ul>\n"
        "</body>\n"
        "</html>\n";
    
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html\r\n"
              "Content-Length: %d\r\n"
              "\r\n"
              "%s",
              (int)strlen(html),
              html);
    
    return 200;
}

int main(int argc, char* argv[]) {
    struct mg_context* ctx;
    struct mg_callbacks callbacks;
    
    printf("Pinpoint CivetWeb Example Server\n");
    printf("=================================\n\n");
    
    /* Initialize Pinpoint agent */
    const char* config_file = (argc > 1) ? argv[1] : "pinpoint-config.yaml";
    pinpoint_set_config_file_path(config_file);
    
    g_agent = pinpoint_create_agent();
    if (g_agent == NULL || !pinpoint_agent_enable(g_agent)) {
        fprintf(stderr, "Warning: Failed to initialize Pinpoint agent. Tracing will be disabled.\n");
        fprintf(stderr, "         Server will continue without tracing.\n\n");
    } else {
        printf("Pinpoint agent initialized successfully\n");
        printf("Config file: %s\n\n", config_file);
    }
    
    /* Initialize CivetWeb callbacks */
    memset(&callbacks, 0, sizeof(callbacks));
    
    /* CivetWeb server options */
    const char* options[] = {
        "listening_ports", "8080",
        "num_threads", "4",
        NULL
    };
    
    /* Start CivetWeb server */
    ctx = mg_start(&callbacks, NULL, options);
    if (ctx == NULL) {
        fprintf(stderr, "Error: Failed to start CivetWeb server\n");
        if (g_agent != NULL) {
            pinpoint_agent_shutdown(g_agent);
            pinpoint_agent_destroy(g_agent);
        }
        return 1;
    }
    
    /* Register handlers */
    mg_set_request_handler(ctx, "/", handle_root, NULL);
    mg_set_request_handler(ctx, "/api/users", handle_users, NULL);
    mg_set_request_handler(ctx, "/api/health", handle_health, NULL);
    
    printf("Server started on port 8080\n");
    printf("Press Enter to stop the server...\n");
    getchar();
    
    /* Cleanup */
    printf("\nStopping server...\n");
    mg_stop(ctx);
    
    if (g_agent != NULL) {
        pinpoint_agent_shutdown(g_agent);
        pinpoint_agent_destroy(g_agent);
        printf("Pinpoint agent shut down\n");
    }
    
    printf("Server stopped\n");
    return 0;
}

