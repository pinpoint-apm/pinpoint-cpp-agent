# Pinpoint C Wrapper

This directory contains the C wrapper for the Pinpoint C++ Agent, allowing C applications to use Pinpoint tracing functionality.

## Files

- `pinpoint_c.h` - C header file with public API declarations
- `pinpoint_c.cpp` - C++ implementation of the C wrapper
- `example.c` - Example usage demonstrating the C API
- `CMakeLists.txt` - Build configuration

## New Features: HTTP Trace Helper Functions

The C wrapper now includes helper functions for tracing HTTP server and client requests/responses, corresponding to the `pinpoint::helper` namespace in the C++ API.

### HTTP Server Tracing

#### `pinpoint_trace_http_server_request`

Traces an HTTP server request with headers.

```c
void pinpoint_trace_http_server_request(
    pinpoint_span_handle span,
    const char* remote_addr,
    const char* endpoint,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data
);
```

#### `pinpoint_trace_http_server_request_with_cookies`

Traces an HTTP server request with headers and cookies.

```c
void pinpoint_trace_http_server_request_with_cookies(
    pinpoint_span_handle span,
    const char* remote_addr,
    const char* endpoint,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data,
    pinpoint_header_iterator_fn cookie_iterator_fn,
    void* cookie_user_data
);
```

#### `pinpoint_trace_http_server_response`

Traces an HTTP server response with headers.

```c
void pinpoint_trace_http_server_response(
    pinpoint_span_handle span,
    const char* url_pattern,
    const char* method,
    int status_code,
    pinpoint_header_iterator_fn response_iterator_fn,
    void* response_user_data
);
```

### HTTP Client Tracing

#### `pinpoint_trace_http_client_request`

Traces an HTTP client request with headers.

```c
void pinpoint_trace_http_client_request(
    pinpoint_span_event_handle span_event,
    const char* host,
    const char* url,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data
);
```

#### `pinpoint_trace_http_client_request_with_cookies`

Traces an HTTP client request with headers and cookies.

```c
void pinpoint_trace_http_client_request_with_cookies(
    pinpoint_span_event_handle span_event,
    const char* host,
    const char* url,
    pinpoint_header_iterator_fn request_iterator_fn,
    void* request_user_data,
    pinpoint_header_iterator_fn cookie_iterator_fn,
    void* cookie_user_data
);
```

#### `pinpoint_trace_http_client_response`

Traces an HTTP client response with headers.

```c
void pinpoint_trace_http_client_response(
    pinpoint_span_event_handle span_event,
    int status_code,
    pinpoint_header_iterator_fn response_iterator_fn,
    void* response_user_data
);
```

## Header Iterator Pattern

The HTTP trace helper functions use a callback-based pattern for providing headers. You need to implement a `pinpoint_header_iterator_fn` function that:

1. Receives your user data (header collection) as the first parameter
2. Receives the reader context as the second parameter
3. Iterates through your headers collection
4. Calls `pinpoint_header_iterator_callback(key, value, reader_context)` for each header
5. Returns 1 on success

### Example Implementation

```c
typedef struct {
    const char* key;
    const char* value;
} header_entry;

typedef struct {
    header_entry* headers;
    int count;
} header_collection;

// Iterator function implementation
int my_header_iterator(void* user_data, void* reader_context) {
    // user_data is your header collection
    header_collection* headers = (header_collection*)user_data;
    
    // Iterate through all headers
    for (int i = 0; i < headers->count; i++) {
        // Call Pinpoint's callback for each header
        // Pass the reader_context so Pinpoint can invoke the internal C++ callback
        pinpoint_header_iterator_callback(
            headers->headers[i].key,
            headers->headers[i].value,
            reader_context  // Pass the reader context
        );
    }
    
    return 1;
}
```

### Usage Example

```c
// Prepare headers
header_entry request_headers[] = {
    {"Content-Type", "application/json"},
    {"User-Agent", "MyClient/1.0"}
};
header_collection request_coll = {request_headers, 2};

// Create span
pinpoint_span_handle span = pinpoint_new_span(agent, "POST /api/users", "/api/users");

// Trace HTTP server request
pinpoint_trace_http_server_request(
    span,
    "192.168.1.100:54321",  // remote address
    "/api/users",            // endpoint
    my_header_iterator,      // iterator function
    &request_coll           // user data (headers)
);

// ... process request ...

// Trace HTTP server response
header_entry response_headers[] = {
    {"Content-Type", "application/json"}
};
header_collection response_coll = {response_headers, 1};

pinpoint_trace_http_server_response(
    span,
    "/api/users",           // URL pattern
    "POST",                 // method
    201,                    // status code
    my_header_iterator,
    &response_coll
);

pinpoint_span_end(span);
pinpoint_span_destroy(span);
```

## Building

The C wrapper is built as part of the main Pinpoint C++ Agent build:

```bash
mkdir build && cd build
cmake ..
make
```

## Integration with C Projects

To use the C wrapper in your C project:

1. Include the header: `#include "pinpoint_c.h"`
2. Link against the Pinpoint C library: `-lpinpoint_c`
3. Ensure the Pinpoint C++ agent library is also linked: `-lpinpoint_cpp_agent`

## Full Example

See `example.c` for a complete working example demonstrating:
- Agent initialization
- Span creation
- HTTP server request/response tracing
- HTTP client request/response tracing
- Span events for database calls
- Proper cleanup

## Thread Safety

The header iterator mechanism uses class member variables to store callbacks, making it inherently thread-safe. Each `CHeaderReader` instance maintains its own callback state, so multiple threads can safely trace HTTP requests concurrently without any global state conflicts.

### Design Benefits

1. **No Global State**: Callbacks are stored in class members, not global variables
2. **Thread-Safe**: Each reader instance is independent
3. **Clean Architecture**: Context is passed explicitly through function parameters
4. **Type-Safe**: Strong typing through the context structure

## Compatibility

This C wrapper is compatible with C99 and later. The underlying C++ implementation requires C++17.

