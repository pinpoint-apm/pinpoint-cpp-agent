#include "pinpoint_c.h"

// Example header storage structure
typedef struct {
    const char* key;
    const char* value;
} header_entry;

typedef struct {
    header_entry* headers;
    int count;
} header_collection;

// Context writer callback for injecting trace context
void my_context_writer(const char* key, const char* value, void* user_data) {
    // Set HTTP header - implementation depends on your HTTP library
    // set_http_header(user_data, key, value);
    (void)key;
    (void)value;
    (void)user_data;
}

// Header iterator callback for providing headers to Pinpoint
// This function is called by Pinpoint when it needs to iterate through headers
int my_header_iterator(void* user_data, void* reader_context) {
    // user_data contains the header collection
    header_collection* headers = (header_collection*)user_data;
    
    // Iterate through all headers in the collection
    for (int i = 0; i < headers->count; i++) {
        // Call the Pinpoint callback for each header
        // Pass the reader_context so Pinpoint can invoke the C++ callback
        pinpoint_header_iterator_callback(
            headers->headers[i].key,
            headers->headers[i].value,
            reader_context  // Pass the reader context
        );
    }
    
    return 1;
}

int main() {
    // Configure agent
    pinpoint_set_config_file_path("/etc/pinpoint/config.yaml");
    
    // Create agent
    pinpoint_agent_handle agent = pinpoint_create_agent();
    
    /* Example 1: Basic span usage */
    {
        pinpoint_span_handle span = pinpoint_new_span(agent, "my_operation", "/api/users");
        pinpoint_span_set_service_type(span, PINPOINT_SERVICE_TYPE_CPP);
        
        // Create span event for database call
        pinpoint_span_event_handle event = pinpoint_new_span_event(span, "mysql_query");
        pinpoint_span_event_set_service_type(event, PINPOINT_SERVICE_TYPE_MYSQL_QUERY);
        pinpoint_span_event_set_sql_query(event, "SELECT * FROM users", "");
        pinpoint_span_event_end(span);
        
        // Inject context for outbound call
        // pinpoint_span_inject_context(span, my_context_writer, http_request);
        
        // End span
        pinpoint_span_set_status_code(span, 200);
        pinpoint_span_end(span);
        pinpoint_span_destroy(span);
    }
    
    /* Example 2: HTTP Server Request/Response Tracing */
    {
        // Prepare request headers
        header_entry request_headers[] = {
            {"Content-Type", "application/json"},
            {"User-Agent", "MyClient/1.0"},
            {"Accept", "application/json"}
        };
        header_collection request_header_coll = {request_headers, 3};
        
        // Create span with trace context
        pinpoint_span_handle span = pinpoint_new_span(agent, "POST /api/users", "/api/users");
        
        // Trace HTTP server request
        pinpoint_trace_http_server_request(
            span,
            "192.168.1.100:54321",  // remote address
            "/api/users",            // endpoint
            my_header_iterator,      // request header iterator
            &request_header_coll     // user data (header collection)
        );
        
        // Process request...
        
        // Prepare response headers
        header_entry response_headers[] = {
            {"Content-Type", "application/json"},
            {"Cache-Control", "no-cache"}
        };
        header_collection response_header_coll = {response_headers, 2};
        
        // Trace HTTP server response
        pinpoint_trace_http_server_response(
            span,
            "/api/users",           // URL pattern
            "POST",                 // method
            201,                    // status code
            my_header_iterator,     // response header iterator
            &response_header_coll   // user data
        );
        
        pinpoint_span_end(span);
        pinpoint_span_destroy(span);
    }
    
    /* Example 3: HTTP Client Request/Response Tracing */
    {
        pinpoint_span_handle span = pinpoint_new_span(agent, "external_api_call", "/internal/service");
        
        // Create span event for HTTP client call
        pinpoint_span_event_handle event = pinpoint_new_span_event_with_type(
            span, 
            "GET http://api.example.com/data",
            PINPOINT_SERVICE_TYPE_CPP_HTTP_CLIENT
        );
        
        // Prepare request headers
        header_entry request_headers[] = {
            {"Accept", "application/json"},
            {"Authorization", "Bearer token123"}
        };
        header_collection request_header_coll = {request_headers, 2};
        
        // Trace HTTP client request
        pinpoint_trace_http_client_request(
            event,
            "api.example.com",      // host
            "http://api.example.com/data",  // URL
            my_header_iterator,     // request header iterator
            &request_header_coll    // user data
        );
        
        // Make HTTP request...
        
        // Prepare response headers
        header_entry response_headers[] = {
            {"Content-Type", "application/json"},
            {"X-RateLimit-Remaining", "99"}
        };
        header_collection response_header_coll = {response_headers, 2};
        
        // Trace HTTP client response
        pinpoint_trace_http_client_response(
            event,
            200,                    // status code
            my_header_iterator,     // response header iterator
            &response_header_coll   // user data
        );
        
        pinpoint_span_event_end(span);
        
        pinpoint_span_end(span);
        pinpoint_span_destroy(span);
    }
    
    // Cleanup
    pinpoint_agent_shutdown(agent);
    pinpoint_agent_destroy(agent);
    
    return 0;
}
