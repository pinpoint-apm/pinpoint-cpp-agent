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
 * @file httplib_c.h
 * @brief Pure-C wrapper around the cpp-httplib HTTP server.
 *
 * This header exposes a small subset of httplib::Server's functionality to C
 * callers using opaque handles and plain function pointers.  The header-only
 * httplib.h itself is C++-only, so this wrapper is the bridge that makes it
 * usable from C programs.
 *
 * The accessor functions that expose headers — hlc_headers_get() and
 * hlc_headers_for_each() — use exactly the same signatures expected by
 * pt_reader_get_fn and pt_header_for_each_fn in pinpoint/tracer_c.h, so a
 * pt_header_reader_t can be built directly from an hlc_request_t without
 * writing adapter functions:
 *
 * @code
 *   pt_header_reader_t reader = {
 *       hlc_request_headers_handle(req),
 *       hlc_headers_get,
 *       hlc_headers_for_each,
 *   };
 * @endcode
 */

#ifndef PINPOINT_EXAMPLE_HTTPLIB_C_H
#define PINPOINT_EXAMPLE_HTTPLIB_C_H

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Opaque handles                                                               */
/* ========================================================================== */

/** Opaque handle to a cpp-httplib server instance. */
typedef struct hlc_server_s*  hlc_server_t;

/** Opaque view of a single HTTP request.  Valid only during a handler call. */
typedef struct hlc_request_s  hlc_request_t;

/** Opaque view of a single HTTP response.  Valid only during a handler call. */
typedef struct hlc_response_s hlc_response_t;

/* ========================================================================== */
/* Handler callback                                                             */
/* ========================================================================== */

/**
 * @brief Per-request handler callback.
 *
 * The @p req and @p res pointers are only valid for the duration of this
 * call — do not store them.
 */
typedef void (*hlc_handler_fn)(const hlc_request_t* req,
                               hlc_response_t*       res,
                               void*                 userdata);

/* ========================================================================== */
/* Server lifecycle                                                             */
/* ========================================================================== */

/** Creates a new HTTP server.  Free with hlc_server_destroy(). */
hlc_server_t hlc_server_create(void);

/** Releases a server created by hlc_server_create(). */
void hlc_server_destroy(hlc_server_t server);

/**
 * @brief Registers a handler for the GET method on @p pattern.
 *
 * @param pattern  httplib path pattern (e.g. "/foo").
 * @param handler  Callback invoked on match.
 * @param userdata Opaque pointer forwarded to @p handler.
 */
void hlc_server_get(hlc_server_t   server,
                    const char*    pattern,
                    hlc_handler_fn handler,
                    void*          userdata);

/**
 * @brief Blocks until the server is stopped or listen fails.
 *
 * @return 1 if listen() succeeded, 0 on failure.
 */
int hlc_server_listen(hlc_server_t server, const char* host, int port);

/** Requests that the server stop accepting connections. */
void hlc_server_stop(hlc_server_t server);

/* ========================================================================== */
/* Request accessors                                                            */
/* ========================================================================== */

/** Returns the request path (e.g. "/foo"), NUL-terminated. */
const char* hlc_request_path(const hlc_request_t* req);

/** Returns the HTTP method (e.g. "GET"), NUL-terminated. */
const char* hlc_request_method(const hlc_request_t* req);

/** Returns the remote peer address. */
const char* hlc_request_remote_addr(const hlc_request_t* req);

/** Returns the local (server-side) address. */
const char* hlc_request_local_addr(const hlc_request_t* req);

/** Returns the local (server-side) port. */
int hlc_request_local_port(const hlc_request_t* req);

/**
 * @brief Looks up a request header by name.
 *
 * @return Pointer to the value, or NULL if absent.  The pointer is valid
 *         for the lifetime of the request (i.e. during the handler call).
 */
const char* hlc_request_get_header(const hlc_request_t* req, const char* key);

/**
 * @brief Returns an opaque handle suitable for hlc_headers_get() /
 *        hlc_headers_for_each(), or for use as the userdata field of a
 *        pt_header_reader_t.
 */
void* hlc_request_headers_handle(const hlc_request_t* req);

/* ========================================================================== */
/* Response setters                                                             */
/* ========================================================================== */

/** Sets the response body and its Content-Type. */
void hlc_response_set_content(hlc_response_t* res,
                              const char*     content,
                              const char*     mime_type);

/** Sets the HTTP response status code. */
void hlc_response_set_status(hlc_response_t* res, int status);

/** Adds (or replaces) a response header. */
void hlc_response_set_header(hlc_response_t* res,
                             const char*     key,
                             const char*     value);

/**
 * @brief Returns an opaque handle for the response header map.
 *
 * Usable with hlc_headers_get() / hlc_headers_for_each() or as the userdata
 * field of a pt_header_reader_t.
 */
void* hlc_response_headers_handle(const hlc_response_t* res);

/* ========================================================================== */
/* Header map access — signatures chosen to be directly compatible with        */
/* pt_reader_get_fn and pt_header_for_each_fn from pinpoint/tracer_c.h.        */
/* ========================================================================== */

/**
 * @brief Iteration callback for hlc_headers_for_each().
 *
 * @return 0 to continue iteration, non-zero to stop early.
 */
typedef int (*hlc_header_foreach_cb)(const char* key,
                                     const char* value,
                                     void*       userdata);

/**
 * @brief Looks up a value by key in a headers map.
 *
 * @param handle  Handle obtained from hlc_request_headers_handle() or
 *                hlc_response_headers_handle().
 * @return        Value pointer, or NULL if the key is absent.  The pointer
 *                remains valid as long as the underlying request/response
 *                object is alive.
 */
const char* hlc_headers_get(void* handle, const char* key);

/**
 * @brief Iterates all entries in a headers map.
 *
 * @param handle          Handle obtained from hlc_request_headers_handle() or
 *                        hlc_response_headers_handle().
 * @param callback        Per-entry callback.
 * @param callback_userdata  Opaque pointer forwarded to @p callback.
 */
void hlc_headers_for_each(void*                  handle,
                          hlc_header_foreach_cb  callback,
                          void*                  callback_userdata);

#ifdef __cplusplus
}
#endif

#endif /* PINPOINT_EXAMPLE_HTTPLIB_C_H */
