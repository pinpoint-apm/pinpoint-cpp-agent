# Pinpoint C++ Agent - Quick Start Guide

This guide helps you get started with the Pinpoint C++ Agent (`pinpoint-cpp-agent`) for monitoring your C++ applications.

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Configuration](#configuration)
- [Basic Usage](#basic-usage)
- [Running Your First Traced Application](#running-your-first-traced-application)
- [Example: HTTP Server](#example-http-server)
- [Example: Database Query](#example-database-query)
- [Next Steps](#next-steps)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

Before you begin, ensure you have:

- **Pinpoint Collector**: Version 2.4.0 or higher
- **C++ Compiler**: Supporting C++17 or higher
- **Build System**: CMake 3.20+ or Bazel
- **Operating System**: Linux, macOS, or Windows

---

## Installation

### Using CMake

Add the `pinpoint-cpp-agent` to your `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
  pinpoint_cpp
  GIT_REPOSITORY https://github.com/pinpoint-apm/pinpoint-cpp-agent.git
  GIT_TAG main
)
FetchContent_MakeAvailable(pinpoint_cpp)

# Link against your target
target_link_libraries(your_target PRIVATE pinpoint_cpp)
```

### Using Bazel

Add to your `WORKSPACE` file:

```python
http_archive(
    name = "pinpoint_cpp",
    urls = ["https://github.com/pinpoint-apm/pinpoint-cpp-agent/archive/main.zip"],
    strip_prefix = "pinpoint-cpp-agent-main",
)
```

Then in your `BUILD` file:

```python
cc_binary(
    name = "your_app",
    srcs = ["main.cpp"],
    deps = ["@pinpoint_cpp//:pinpoint_cpp"],
)
```

---

## Configuration

You can configure the agent using a YAML file, environment variables, or an inline configuration string. Environment variables take the highest priority, followed by the YAML file, then built-in defaults.

### Option 1: Configuration File

Create a `pinpoint-config.yaml` file:

```yaml
ApplicationName: "MyApplication"
AgentId: "my-agent-id"  # Optional: auto-generated if not specified

Collector:
  GrpcHost: "localhost"      # Your Pinpoint collector host
  GrpcAgentPort: 9991        # gRPC agent port
  GrpcSpanPort: 9993         # gRPC span port
  GrpcStatPort: 9992         # gRPC stat port

Sampling:
  Type: "COUNTER"
  CounterRate: 1             # Sample all requests

Log:
  Level: "info"              # trace, debug, info, warn, error
```

Set the configuration file path in your application:

```cpp
#include "pinpoint/tracer.h"

int main() {
    pinpoint::SetConfigFilePath("/path/to/pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();

    // Your application code

    agent->Shutdown();
    return 0;
}
```

### Option 2: Environment Variables

```bash
export PINPOINT_CPP_APPLICATION_NAME="MyApplication"
export PINPOINT_CPP_AGENT_ID="my-agent-id"
export PINPOINT_CPP_GRPC_HOST="localhost"
export PINPOINT_CPP_GRPC_AGENT_PORT="9991"
export PINPOINT_CPP_GRPC_SPAN_PORT="9993"
export PINPOINT_CPP_GRPC_STAT_PORT="9992"
```

You can also point to a config file via environment variable:

```bash
export PINPOINT_CPP_CONFIG_FILE="/path/to/pinpoint-config.yaml"
```

### Option 3: Configuration String

Pass configuration directly as a YAML string:

```cpp
#include "pinpoint/tracer.h"

int main() {
    std::string config = R"(
        ApplicationName: "MyApplication"
        Collector:
          GrpcHost: "localhost"
    )";

    pinpoint::SetConfigString(config);
    auto agent = pinpoint::CreateAgent();

    // Your application code

    agent->Shutdown();
    return 0;
}
```

For a complete list of configuration options, see the [Configuration Guide](config.md).

---

## Basic Usage

The typical workflow follows five steps:

1. **Initialize** — set configuration and create an agent at application startup.
2. **Trace** — use `Agent::NewSpan` to start tracing a transaction.
3. **Record work** — create span events and annotations for sub-operations.
4. **End** — call `EndSpan()` when the transaction completes.
5. **Shutdown** — call `agent->Shutdown()` before the application exits.

### Initialize the Agent

```cpp
#include "pinpoint/tracer.h"

int main() {
    pinpoint::SetConfigFilePath("pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();

    // Check if agent is enabled
    if (!agent->Enable()) {
        std::cerr << "Failed to enable Pinpoint agent" << std::endl;
        return 1;
    }

    // Your application code here

    agent->Shutdown();
    return 0;
}
```

### Create a Span

A **Span** represents a single operation or request:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();

    auto span = agent->NewSpan("MyOperation", "/api/endpoint");

    span->SetRemoteAddress("192.168.1.100");
    span->SetEndPoint("localhost:8080");

    // Your business logic here

    span->EndSpan();
}
```

### Create Span Events

**SpanEvents** represent sub-operations within a span:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyOperation", "/api/endpoint");

    // Create a span event for a database operation
    auto dbEvent = span->NewSpanEvent("queryDatabase");
    dbEvent->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    dbEvent->SetDestination("mysql-db");
    dbEvent->SetEndPoint("localhost:3306");

    // Execute database query ...

    span->EndSpanEvent();
    span->EndSpan();
}
```

### Add Annotations

Annotations provide additional metadata:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyOperation", "/api/endpoint");

    auto annotations = span->GetAnnotations();
    annotations->AppendString(pinpoint::ANNOTATION_API, "getUserInfo");
    annotations->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

    // Your business logic here

    span->EndSpan();
}
```

---

## Running Your First Traced Application

Here is a complete minimal example:

```cpp
#include <iostream>
#include <thread>
#include <chrono>
#include "pinpoint/tracer.h"

void doWork() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyService", "/work");

    span->SetRemoteAddress("client-address");
    span->SetEndPoint("localhost:8080");

    // Simulate some work
    auto spanEvent = span->NewSpanEvent("processData");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    span->EndSpanEvent();

    // Add result annotations
    auto annotations = span->GetAnnotations();
    annotations->AppendString(pinpoint::ANNOTATION_API, "doWork");
    annotations->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

    span->EndSpan();
}

int main() {
    // Configure via environment or file
    setenv("PINPOINT_CPP_APPLICATION_NAME", "my-first-app", 0);
    setenv("PINPOINT_CPP_GRPC_HOST", "localhost", 0);

    // Create agent
    auto agent = pinpoint::CreateAgent();

    if (!agent->Enable()) {
        std::cerr << "Failed to enable agent" << std::endl;
        return 1;
    }

    std::cout << "Pinpoint agent started" << std::endl;

    // Simulate multiple requests
    for (int i = 0; i < 5; i++) {
        std::cout << "Processing request " << (i + 1) << std::endl;
        doWork();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "Shutting down agent..." << std::endl;
    agent->Shutdown();

    return 0;
}
```

### Build and Run

```bash
# Compile your application
g++ -std=c++17 -o my_app my_app.cpp -lpinpoint_cpp

# Run your application
./my_app
```

---

## Example: HTTP Server

This example shows how to instrument an HTTP server to trace incoming requests and outgoing calls. It uses `httplib` and demonstrates context propagation.

See full example: `example/http_server.cpp`

```cpp
#include "pinpoint/tracer.h"
// ... includes ...

void handle_users(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();

    // Extract trace context from incoming request headers
    HttpTraceContextReader reader(req.headers);
    auto span = agent->NewSpan("HTTP Server", req.path, reader);

    // Set span properties
    span->SetEndPoint(req.get_header_value("Host"));
    span->SetRemoteAddress(req.remote_addr);

    // Record request headers (optional)
    HttpHeaderReader header_reader(req.headers);
    span->RecordHeader(pinpoint::HTTP_REQUEST, header_reader);

    // Start a sub-operation (SpanEvent)
    auto se = span->NewSpanEvent("process_logic");

    // ... business logic ...

    // End SpanEvent and Span
    span->EndSpanEvent();
    span->SetStatusCode(res.status);
    span->EndSpan();
}

int main() {
    pinpoint::SetConfigFilePath("pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();

    httplib::Server server;
    server.Get("/users", handle_users);

    server.listen("localhost", 8080);

    agent->Shutdown();
    return 0;
}
```

---

## Example: Database Query

This example demonstrates tracing database operations (e.g., MySQL). It shows how to create `SpanEvent`s for SQL queries.

See full example: `example/tutorial.cpp`

```cpp
// Helper to trace DB operations
void trace_db_op(pinpoint::SpanPtr span,
                 const std::string& query,
                 std::function<void()> func) {
    auto se = span->NewSpanEvent("mysql_query");
    se->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    se->SetEndPoint("localhost:33060");
    se->SetDestination("test_db");
    se->SetSqlQuery(query, "");  // Record the query string (sanitize in production)

    try {
        func();  // Execute actual DB operation
    } catch (const std::exception& e) {
        se->SetError(e.what());  // Record error if any
        throw;
    }

    span->EndSpanEvent();
}

void db_logic(pinpoint::SpanPtr span) {
    // Insert example
    trace_db_op(span, "INSERT INTO users ...", [&]() {
        // ... execute insert ...
    });

    // Select example
    trace_db_op(span, "SELECT * FROM users ...", [&]() {
        // ... execute select ...
    });
}
```

---

## Next Steps

Now that you have a basic understanding of the Pinpoint C++ Agent, you can:

1. **Learn Advanced Instrumentation**: Read the [Instrumentation Guide](instrument.md) for detailed information on HTTP request/response tracing, database query tracing, distributed tracing with context propagation, error handling and exception tracking, and asynchronous operation tracing.

2. **Explore Examples**: Check the `example/` directory for complete examples including `http_server.cpp` (HTTP server instrumentation) and `tutorial.cpp` (database query and tracing tutorial).

3. **Configure Advanced Options**: See the [Configuration Guide](config.md) for sampling strategies, URL statistics collection, SQL parameter binding, logging, and stat collection.

4. **Monitor Your Application**: Access the Pinpoint Web UI to view service maps, analyze transaction traces, monitor performance metrics, and identify bottlenecks.

---

## Troubleshooting

### Agent Not Starting

If the agent fails to start:

1. Check that the collector host and ports are correct.
2. Verify network connectivity to the Pinpoint collector.
3. Check application logs for error messages (set `Log.Level: "debug"` for verbose output).
4. Ensure `ApplicationName` is set correctly.
5. Confirm `agent->Enable()` returns `true`.

### No Data in Pinpoint UI

If you don't see data in Pinpoint:

1. Verify the agent is enabled: `agent->Enable()` returns `true`.
2. Check sampling configuration — use `CounterRate: 1` (sample all) for initial testing.
3. Ensure spans are properly ended with `EndSpan()`.
4. Wait a few seconds for data to appear (there is a collection interval).
5. Check Pinpoint collector logs for errors.

### Performance Impact

To minimize performance impact:

1. Use appropriate sampling rates (not 100% in production).
2. Avoid excessive annotations.
3. Only trace critical paths.
4. Monitor agent overhead and adjust configuration (queue sizes, URL stats, SQL stats).

For more detailed troubleshooting, see the [Instrumentation Guide](instrument.md#14-troubleshooting).

---

## Support

- **GitHub Issues**: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
- **Pinpoint Documentation**: [Pinpoint APM](https://pinpoint-apm.github.io/pinpoint/)
- **Community**: Use the main Pinpoint project and issue tracker for discussions

---

## License

Pinpoint C++ Agent is licensed under the Apache License 2.0. See [LICENSE](../LICENSE) for details.
