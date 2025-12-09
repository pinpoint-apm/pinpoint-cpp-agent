# Cpptrace Demo

This example demonstrates the Pinpoint C++ Agent with cpptrace integration for enhanced stack trace capabilities.

## Features

- HTTP server with Pinpoint tracing
- Stack trace capture using cpptrace library
- Distributed tracing with context propagation
- Configurable target URL for outgoing requests

## Prerequisites

- cpptrace library (v1.0.4 or later)
- C++17 compatible compiler
- CMake 3.14+
- Docker (for containerized deployment)

## Building Locally

### Install cpptrace

```bash
git clone https://github.com/jeremy-rifkin/cpptrace.git
cd cpptrace
git checkout v1.0.4
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
sudo cmake --install build
```

### Build the Demo

```bash
cd ../../..
mkdir -p build && cd build
cmake -DBUILD_EXAMPLES=ON -Dcpptrace_DIR=/usr/local/lib/cmake/cpptrace ..
make -j$(nproc)
```

The executable will be at `build/example/cpptrace_demo/cpptrace_demo`

## Running with Docker

The easiest way to run this demo is with Docker Compose:

```bash
cd example/cpptrace_demo
docker-compose up --build
```

This will:
- Start an httpbin service as a test target
- Build and run the cpptrace demo server on port 8088

## Usage

### Start the Server

```bash
# Local
./build/example/cpptrace_demo/cpptrace_demo

# Docker
docker-compose up
```

### Test Endpoints

**1. Users endpoint:**
```bash
curl "http://localhost:8088/users/123?name=alice"
```

Response:
```json
{
  "id": "123",
  "name": "alice",
  "client_ip": "127.0.0.1"
}
```

**2. Outgoing request (with default target):**
```bash
curl "http://localhost:8088/outgoing"
```

**3. Outgoing request (with custom target):**
```bash
curl "http://localhost:8088/outgoing?target=http://httpbin/get"
```

Response:
```json
{
  "target_url": "http://httpbin/get",
  "status_code": 200,
  "response_body": "...",
  "success": true
}
```

## Configuration

### Environment Variables

- `TARGET_URL`: Default target URL for outgoing requests (default: `http://localhost:9000/bar`)

### Pinpoint Configuration

Edit `pinpoint-config.yaml` to configure:
- Application name and type
- Collector host and ports
- Sampling strategy
- HTTP header recording
- Logging level

## How It Works

### Stack Trace Capture

When an error occurs in the `handle_outgoing` function, cpptrace automatically captures the call stack:

```cpp
CppTraceCallStackReader reader;
se->SetError("HandleOutgoingError", err_msg, reader);
```

The stack trace includes:
- Module names
- Function names
- File names and line numbers

### Distributed Tracing

The demo shows how to propagate trace context across service boundaries:

1. **Incoming request**: Extract trace context from headers
2. **Outgoing request**: Inject trace context into headers
3. **Pinpoint Collector**: Correlates traces across services

### Thread-Local Storage

Spans are stored in thread-local storage to avoid passing them as parameters:

```cpp
thread_local pinpoint::SpanPtr current_span;

void set_span_context(pinpoint::SpanPtr span) { current_span = span; }
pinpoint::SpanPtr get_span_context() { return current_span; }
```

## Troubleshooting

### cpptrace not found

```
cpptrace not found - skipping cpptrace_demo build
```

**Solution**: Install cpptrace or set `cpptrace_DIR`:
```bash
cmake -Dcpptrace_DIR=/usr/local/lib/cmake/cpptrace ..
```

### Connection refused to target

```
Outgoing call failed: Connection - Unable to connect to localhost:9000
```

**Solution**: 
- Use Docker Compose to start httpbin service
- Or set `TARGET_URL` environment variable to a reachable service
- Or pass `?target=http://your-service/path` in the request

### Port already in use

```
bind: Address already in use
```

**Solution**: Change the port in `web_demo.cpp`:
```cpp
server.listen("0.0.0.0", 8089);  // Use different port
```

## See Also

- [Pinpoint C++ Agent Documentation](../../doc/)
- [cpptrace GitHub](https://github.com/jeremy-rifkin/cpptrace)
- [cpp-httplib](https://github.com/yhirose/cpp-httplib)

