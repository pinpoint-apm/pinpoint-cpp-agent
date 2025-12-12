#include "pinpoint_c.h"

// Context writer callback
void my_context_writer(const char* key, const char* value, void* user_data) {
    // Set HTTP header
    set_http_header(user_data, key, value);
}

int main() {
    // Configure agent
    pinpoint_set_config_file_path("/etc/pinpoint/config.yaml");
    
    // Create agent
    pinpoint_agent_handle agent = pinpoint_create_agent();
    
    // Create span
    pinpoint_span_handle span = pinpoint_new_span(agent, "my_operation", "/api/users");
    pinpoint_span_set_service_type(span, PINPOINT_SERVICE_TYPE_CPP);
    
    // Create span event for database call
    pinpoint_span_event_handle event = pinpoint_new_span_event(span, "mysql_query");
    pinpoint_span_event_set_service_type(event, PINPOINT_SERVICE_TYPE_MYSQL_QUERY);
    pinpoint_span_event_set_sql_query(event, "SELECT * FROM users", "");
    pinpoint_span_event_end(span);
    
    // Inject context for outbound call
    pinpoint_span_inject_context(span, my_context_writer, http_request);
    
    // End span
    pinpoint_span_set_status_code(span, 200);
    pinpoint_span_end(span);
    
    // Cleanup
    pinpoint_span_destroy(span);
    pinpoint_agent_shutdown(agent);
    pinpoint_agent_destroy(agent);
    
    return 0;
}
