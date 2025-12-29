# Pinpoint C Wrapper - CivetWeb Example

This example demonstrates how to integrate Pinpoint distributed tracing with a [CivetWeb](https://github.com/civetweb/civetweb) HTTP server using the Pinpoint C wrapper API.

## Overview

CivetWeb is a lightweight, embeddable C/C++ web server with MIT license. This example shows how to:

- Create an HTTP server using CivetWeb
- Integrate Pinpoint tracing using the C wrapper
- Trace incoming HTTP requests with full header propagation
- Trace outgoing HTTP responses
- Extract and inject trace context for distributed tracing

## Features

- **HTTP Server**: Simple web server with multiple endpoints
- **Distributed Tracing**: Full request/response tracing with Pinpoint
- **Header Propagation**: Automatic trace context propagation
- **Multiple Endpoints**: 
  - `/` - Welcome page
  - `/api/users` - REST API endpoint (returns JSON)
  - `/api/health` - Health check endpoint

## Prerequisites

### Install CivetWeb

#### Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install libcivetweb-dev
```

#### macOS:
```bash
brew install civetweb
```

#### From Source:
```bash
git clone https://github.com/civetweb/civetweb.git
cd civetweb
mkdir build && cd build
cmake ..
make
sudo make install
```

### Pinpoint Agent

Make sure the Pinpoint C wrapper library is built:

```bash
cd /path/to/pinpoint-cpp-agent-cursor
mkdir -p build && cd build
cmake ..
make pinpoint_c-static
```

## Building

From the project root:

```bash
cd build
cmake ..
make civetweb_example
```

Or build only the example:

```bash
cd export/civetweb_example
mkdir -p build && cd build
cmake ..
make
```

## Configuration

Create a `pinpoint-config.yaml` file in the same directory as the executable:

```yaml
# Pinpoint Collector Configuration
collector:
  agent_id: civetweb-example
  application_name: civetweb-demo
  application_type: 1300  # CPP application type
  
  # Collector server settings
  grpc:
    collector_host: localhost
    collector_port: 9991
    
# Sampling Configuration
sampling:
  type: COUNTING
  rate: 1  # 1 = 100% sampling for testing
  
# Logging
log:
  level: INFO
  file: pinpoint-civetweb.log
```

## Running

```bash
./civetweb_example [config_file]
```

If no config file is specified, it defaults to `pinpoint-config.yaml`.

Example:
```bash
./civetweb_example pinpoint-config.yaml
```

The server will start on port 8080.

## Testing

### Basic Request
```bash
curl http://localhost:8080/
```

### Users API
```bash
curl http://localhost:8080/api/users
```

### Health Check
```bash
curl http://localhost:8080/api/health
```

### With Trace Context
Test distributed tracing by injecting trace context:

```bash
curl -H "Pinpoint-TraceID: test-agent^1234567890^1" \
     -H "Pinpoint-SpanID: 9876543210" \
     -H "Pinpoint-pSpanID: -1" \
     -H "Pinpoint-Sampled: 1" \
     http://localhost:8080/api/users
```

## Code Structure

### Main Components

1. **Header Collection**: Custom implementation to collect HTTP headers from CivetWeb

```c
typedef struct {
    header_pair* headers;
    int count;
    int capacity;
} header_collection;
```

2. **Trace Context Reader**: Extracts Pinpoint trace context from incoming requests

```c
static int trace_context_reader(const char* key, char* value_out, 
                                 size_t value_size, void* user_data)
```

3. **Header Iterator**: Provides headers to Pinpoint tracing system

```c
static int header_iterator(void* user_data, void* reader_context)
```

4. **Request Handlers**: Handle different endpoints with full tracing

```c
static int handle_users(struct mg_connection* conn, void* cbdata)
static int handle_health(struct mg_connection* conn, void* cbdata)
```

### Tracing Flow

1. **Create Span**: Extract trace context from incoming request
```c
pinpoint_span_handle span = pinpoint_new_span_with_context(
    g_agent,
    req_info->request_method,
    req_info->request_uri,
    trace_context_reader,
    conn
);
```

2. **Trace Request**: Record incoming request with headers
```c
pinpoint_trace_http_server_request(
    span,
    remote_addr,
    req_info->request_uri,
    header_iterator,
    &request_headers
);
```

3. **Process Request**: Handle the business logic

4. **Trace Response**: Record outgoing response with headers
```c
pinpoint_trace_http_server_response(
    span,
    req_info->request_uri,
    req_info->request_method,
    200,
    header_iterator,
    &response_headers
);
```

5. **End Span**: Finalize and send trace data
```c
pinpoint_span_end(span);
pinpoint_span_destroy(span);
```

## Viewing Traces

1. Start Pinpoint Collector and Web UI (see main Pinpoint documentation)

2. Access the Pinpoint Web UI (typically at http://localhost:8080)

3. Select your application ("civetweb-demo")

4. View the service map and trace details

## Integration with Your Application

To integrate this pattern into your own CivetWeb application:

1. **Initialize Pinpoint agent at startup**:
```c
pinpoint_set_config_file_path("pinpoint-config.yaml");
g_agent = pinpoint_create_agent();
```

2. **Wrap your request handlers**:
   - Create span with trace context
   - Collect request headers
   - Call `pinpoint_trace_http_server_request()`
   - Process your business logic
   - Collect response headers
   - Call `pinpoint_trace_http_server_response()`
   - End span

3. **Shutdown cleanly**:
```c
pinpoint_agent_shutdown(g_agent);
pinpoint_agent_destroy(g_agent);
```

## Performance Considerations

- Header collection uses dynamic allocation - consider using a static buffer pool for high-traffic scenarios
- Tracing overhead is minimal when sampling rate is low
- The example uses synchronous tracing - Pinpoint agent handles async transmission

## Troubleshooting

### Server fails to start
- Check if port 8080 is already in use
- Verify CivetWeb is properly installed

### Tracing not working
- Check Pinpoint collector is running and accessible
- Verify configuration file exists and is valid
- Check log file for error messages
- Ensure agent is enabled: `pinpoint_agent_enable()` returns true

### Headers not captured
- Verify header iterator is called correctly
- Check header collection is properly populated
- Enable debug logging in Pinpoint configuration

## References

- [CivetWeb Documentation](https://github.com/civetweb/civetweb/blob/master/docs/UserManual.md)
- [Pinpoint C Wrapper Documentation](../README.md)
- [Pinpoint Documentation](https://pinpoint-apm.github.io/pinpoint/)

## License

This example code is licensed under the Apache License 2.0, same as the Pinpoint project.

CivetWeb is licensed under the MIT License.

