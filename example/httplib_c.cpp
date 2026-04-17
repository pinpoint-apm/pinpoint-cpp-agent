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
 * @file httplib_c.cpp
 * @brief C++ implementation of the pure-C httplib wrapper declared in
 *        httplib_c.h.
 *
 * Handles are thin heap-allocated wrappers around httplib objects.  Request
 * and response wrapper instances are built on the stack inside the handler
 * dispatch lambda — they live exactly as long as the handler call.
 */

#include "httplib_c.h"
#include "httplib.h"

#include <string>

// ============================================================================
// Opaque handle definitions
// ============================================================================

struct hlc_server_s {
    httplib::Server server;
};

struct hlc_request_s {
    const httplib::Request* req;
};

struct hlc_response_s {
    httplib::Response* res;
};

struct hlc_client_s {
    httplib::Client cli;
    explicit hlc_client_s(const std::string& host) : cli(host) {}
};

struct hlc_mutable_headers_s {
    httplib::Headers headers;
};

// Holds either a successful httplib::Result or a plain error message.
struct hlc_result_s {
    httplib::Result     result;
    std::string         error_msg;
    explicit hlc_result_s(httplib::Result r) : result(std::move(r)) {
        if (!result) {
            error_msg = httplib::to_string(result.error());
        }
    }
};

// ============================================================================
// Server lifecycle
// ============================================================================

extern "C" hlc_server_t hlc_server_create(void) {
    return new hlc_server_s();
}

extern "C" void hlc_server_destroy(hlc_server_t server) {
    delete server;
}

extern "C" void hlc_server_get(hlc_server_t   server,
                               const char*    pattern,
                               hlc_handler_fn handler,
                               void*          userdata) {
    if (!server || !pattern || !handler) return;

    // Capture the C handler pair by value; the lambda is invoked for each
    // matching request on one of httplib's worker threads.
    server->server.Get(pattern,
        [handler, userdata](const httplib::Request& req, httplib::Response& res) {
            hlc_request_s  c_req{&req};
            hlc_response_s c_res{&res};
            handler(&c_req, &c_res, userdata);
        });
}

extern "C" int hlc_server_listen(hlc_server_t server, const char* host, int port) {
    if (!server || !host) return 0;
    return server->server.listen(host, port) ? 1 : 0;
}

extern "C" void hlc_server_stop(hlc_server_t server) {
    if (server) server->server.stop();
}

// ============================================================================
// Request accessors
// ============================================================================

extern "C" const char* hlc_request_path(const hlc_request_t* req) {
    return (req && req->req) ? req->req->path.c_str() : "";
}

extern "C" const char* hlc_request_method(const hlc_request_t* req) {
    return (req && req->req) ? req->req->method.c_str() : "";
}

extern "C" const char* hlc_request_remote_addr(const hlc_request_t* req) {
    return (req && req->req) ? req->req->remote_addr.c_str() : "";
}

extern "C" const char* hlc_request_local_addr(const hlc_request_t* req) {
    return (req && req->req) ? req->req->local_addr.c_str() : "";
}

extern "C" int hlc_request_local_port(const hlc_request_t* req) {
    return (req && req->req) ? req->req->local_port : 0;
}

extern "C" const char* hlc_request_body(const hlc_request_t* req) {
    return (req && req->req) ? req->req->body.c_str() : "";
}

extern "C" const char* hlc_request_get_header(const hlc_request_t* req,
                                              const char*          key) {
    if (!req || !req->req || !key) return nullptr;
    auto it = req->req->headers.find(key);
    return it == req->req->headers.end() ? nullptr : it->second.c_str();
}

extern "C" void* hlc_request_headers_handle(const hlc_request_t* req) {
    if (!req || !req->req) return nullptr;
    // httplib::Headers is std::multimap<std::string, std::string, ...>
    // We return a non-owning pointer; lifetime is tied to the request.
    return const_cast<httplib::Headers*>(&req->req->headers);
}

// ============================================================================
// Response setters
// ============================================================================

extern "C" void hlc_response_set_content(hlc_response_t* res,
                                         const char*     content,
                                         const char*     mime_type) {
    if (!res || !res->res || !content) return;
    res->res->set_content(content, mime_type ? mime_type : "text/plain");
}

extern "C" void hlc_response_set_status(hlc_response_t* res, int status) {
    if (res && res->res) res->res->status = status;
}

extern "C" void hlc_response_set_header(hlc_response_t* res,
                                        const char*     key,
                                        const char*     value) {
    if (!res || !res->res || !key || !value) return;
    res->res->set_header(key, value);
}

extern "C" void* hlc_response_headers_handle(const hlc_response_t* res) {
    if (!res || !res->res) return nullptr;
    return &res->res->headers;
}

// ============================================================================
// Header map access
// ============================================================================

extern "C" const char* hlc_headers_get(void* handle, const char* key) {
    if (!handle || !key) return nullptr;
    const auto* headers = static_cast<const httplib::Headers*>(handle);
    auto it = headers->find(key);
    return it == headers->end() ? nullptr : it->second.c_str();
}

extern "C" void hlc_headers_for_each(void*                 handle,
                                     hlc_header_foreach_cb callback,
                                     void*                 callback_userdata) {
    if (!handle || !callback) return;
    const auto* headers = static_cast<const httplib::Headers*>(handle);
    for (const auto& kv : *headers) {
        if (callback(kv.first.c_str(), kv.second.c_str(), callback_userdata) != 0) break;
    }
}

extern "C" void hlc_headers_set(void* handle, const char* key, const char* value) {
    if (!handle || !key || !value) return;
    auto* headers = static_cast<httplib::Headers*>(handle);
    headers->emplace(key, value);
}

// ============================================================================
// Mutable headers
// ============================================================================

extern "C" hlc_mutable_headers_t hlc_mutable_headers_create(void) {
    return new hlc_mutable_headers_s();
}

extern "C" void hlc_mutable_headers_destroy(hlc_mutable_headers_t headers) {
    delete headers;
}

extern "C" void* hlc_mutable_headers_handle(hlc_mutable_headers_t headers) {
    return headers ? &headers->headers : nullptr;
}

// ============================================================================
// HTTP client
// ============================================================================

extern "C" hlc_client_t hlc_client_create(const char* host) {
    if (!host) return nullptr;
    return new hlc_client_s(host);
}

extern "C" void hlc_client_destroy(hlc_client_t client) {
    delete client;
}

extern "C" hlc_result_t hlc_client_get(hlc_client_t client,
                                       const char*  path,
                                       void*        headers) {
    if (!client || !path) return nullptr;
    httplib::Result result = headers
        ? client->cli.Get(path, *static_cast<httplib::Headers*>(headers))
        : client->cli.Get(path);
    return new hlc_result_s(std::move(result));
}

extern "C" int hlc_result_ok(const hlc_result_t result) {
    return (result && static_cast<bool>(result->result)) ? 1 : 0;
}

extern "C" int hlc_result_status(const hlc_result_t result) {
    return (result && result->result) ? result->result->status : 0;
}

extern "C" const char* hlc_result_body(const hlc_result_t result) {
    return (result && result->result) ? result->result->body.c_str() : "";
}

extern "C" void* hlc_result_headers_handle(const hlc_result_t result) {
    if (!result || !result->result) return nullptr;
    // Cast away the logical const — the pointer is treated as read-only by
    // the C header accessors (hlc_headers_get / hlc_headers_for_each).
    return const_cast<httplib::Headers*>(&result->result->headers);
}

extern "C" const char* hlc_result_error_message(const hlc_result_t result) {
    return result ? result->error_msg.c_str() : "";
}

extern "C" void hlc_result_destroy(hlc_result_t result) {
    delete result;
}
