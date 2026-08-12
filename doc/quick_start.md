# Pinpoint C++ Agent - Quick Start Guide

This guide helps you get started with the Pinpoint C++ Agent (`pinpoint-cpp-agent`) for monitoring your C++ applications.

---

## Prerequisites

Before you begin, ensure you have:

- **Pinpoint Collector**: Version 2.4.0 or higher
- **C++ Compiler**: Supporting C++17 or higher
- **Build System**: CMake 3.21+ or Bazel 7.0+
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

Add to your `MODULE.bazel` file:

```python
bazel_dep(name = "pinpoint-cpp", version = "2.0.0")

# Not published to the Bazel Central Registry yet — point at the repository.
git_override(
    module_name = "pinpoint-cpp",
    remote = "https://github.com/pinpoint-apm/pinpoint-cpp-agent.git",
    branch = "main",
)
```

Then in your `BUILD` file:

```python
cc_binary(
    name = "your_app",
    srcs = ["main.cpp"],
    deps = ["@pinpoint-cpp//:pinpoint-cpp"],
)
```

To build the agent itself from source, see the [Build Guide](build.md).

---

## Configuration

You can configure the agent using a YAML file, environment variables, or an inline configuration string. Environment variables take the highest priority, followed by the YAML file, then built-in defaults.

### Option 1: Configuration File

Create a `pinpoint-config.yaml` file:

```yaml
ApplicationName: "MyApplication"
AgentName: "my-agent-name"  # Optional label; the agent id is always auto-generated

Collector:
  Host: "localhost"          # Your Pinpoint collector host
  AgentPort: 9991            # gRPC agent port
  SpanPort: 9993             # gRPC span port
  StatPort: 9992             # gRPC stat port

Sampling:
  Type: "COUNTER"
  CounterRate: 1             # Sample all requests

Log:
  Level: "info"              # trace, debug, info, warn, error
```

Set the configuration file path in your application:

```cpp
#include "httplib.h"
#include "pinpoint/tracer.h"

int main() {
    pinpoint::AgentOptions options;
    options.config_file_path = "/path/to/pinpoint-config.yaml";
    if (!pinpoint::StartAgent(options)) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    httplib::Server server;
    server.Get("/users", on_users);   // your traced handler
    server.listen("0.0.0.0", 8090);   // returns when the server stops

    agent->Shutdown();
    return 0;
}
```

### Option 2: Environment Variables

```bash
export PINPOINT_CPP_APPLICATION_NAME="MyApplication"
export PINPOINT_CPP_AGENT_NAME="my-agent-name"
export PINPOINT_CPP_COLLECTOR_HOST="localhost"
export PINPOINT_CPP_COLLECTOR_AGENT_PORT="9991"
export PINPOINT_CPP_COLLECTOR_SPAN_PORT="9993"
export PINPOINT_CPP_COLLECTOR_STAT_PORT="9992"
```

You can also point to a config file via environment variable:

```bash
export PINPOINT_CPP_CONFIG_FILE="/path/to/pinpoint-config.yaml"
```

### Option 3: Configuration String

Pass configuration directly as a YAML string:

```cpp
pinpoint::AgentOptions options;
options.config_yaml = R"(
    ApplicationName: "MyApplication"
    Collector:
      Host: "localhost"
)";
if (!pinpoint::StartAgent(options)) {
    std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
}
```

For a complete list of configuration options, see the [Configuration Guide](config.md).

---

## Basic Usage

The examples below instrument an HTTP server built with
[cpp-httplib](https://github.com/yhirose/cpp-httplib), the same library the
programs in `example/` use. The typical workflow follows five steps:

1. **Initialize** — call `StartAgent()` with your configuration at application startup, in the process that records spans (for pre-fork servers: in each worker, see the [Pre-fork Integration Guide](prefork.md)).
2. **Trace** — use `Agent::NewSpan` at the top of each request handler to start tracing a transaction.
3. **Record work** — create span events and annotations for sub-operations.
4. **End** — call `EndSpan()` before the handler returns, on every code path.
5. **Shutdown** — call `agent->Shutdown()` after the server stops listening.

> `StartAgent()` returns before the agent has registered with the collector, and
> `Shutdown()` is terminal for an agent instance. Both contracts — including how
> to tell a real failure from a deliberate `Enable: false`, and how to resume
> tracing after a shutdown — are described in
> [Verifying Agent Startup](trouble_shooting.md#verifying-agent-startup) and
> [Stopping and Resuming the Agent](trouble_shooting.md#stopping-and-resuming-the-agent).

### Initialize the Agent

```cpp
#include "httplib.h"
#include "pinpoint/tracer.h"

int main() {
    pinpoint::AgentOptions options;
    options.config_file_path = "pinpoint-config.yaml";
    if (!pinpoint::StartAgent(options)) {
        // A configuration or setup failure — the application keeps running
        // untraced. The cause is in the agent log.
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    httplib::Server server;
    server.Get("/users", on_users);
    server.listen("0.0.0.0", 8090);

    agent->Shutdown();
    return 0;
}
```

### Create a Span

A **Span** represents a single operation or request — for an HTTP server, one
inbound request. `HttpHeaderReader` comes from
[`example/http_trace_context.h`](../example/http_trace_context.h); it implements
`pinpoint::HeaderReader`, which also serves as the `TraceContextReader` that
joins this span to the caller's trace:

```cpp
#include "http_trace_context.h"

void on_users(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();

    // Extract the trace context from the inbound request headers
    HttpHeaderReader reader(req.headers);
    auto span = agent->NewSpan("HTTP Server", req.path, req.method, reader);

    span->SetRemoteAddress(req.remote_addr);
    span->SetEndPoint(req.get_header_value("Host"));

    // Your business logic here
    res.set_content("hello, users!", "text/plain");
    // httplib fills res.status only after the handler returns (it is still -1
    // here), so set it explicitly before recording it on the span.
    res.status = 200;

    span->SetStatusCode(res.status);
    span->EndSpan();
}
```

### Create Span Events

**SpanEvents** represent sub-operations within a span:

```cpp
void on_users(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    HttpHeaderReader reader(req.headers);
    auto span = agent->NewSpan("HTTP Server", req.path, req.method, reader);

    // Create a span event for a database operation
    auto dbEvent = span->NewSpanEvent("queryDatabase");
    dbEvent->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    dbEvent->SetDestination("mysql-db");
    dbEvent->SetEndPoint("localhost:3306");

    // Execute database query ...

    dbEvent->EndEvent();

    res.set_content(R"({"users":[]})", "application/json");
    res.status = 200;  // see note above: res.status is -1 until the handler returns
    span->SetStatusCode(res.status);
    span->EndSpan();
}
```

### Add Annotations

Annotations provide additional metadata:

```cpp
span->SetAnnotation(pinpoint::ANNOTATION_API, "getUserInfo");
span->SetAnnotation(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
```

---

## Running Your First Traced Application

Here is a complete minimal HTTP server, traced end to end:

```cpp
#include <cstdlib>   // setenv
#include <iostream>

#include "httplib.h"
#include "pinpoint/tracer.h"
#include "http_trace_context.h"   // HttpHeaderReader, from example/

void on_users(const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();

    // Join the caller's trace using the inbound headers
    HttpHeaderReader req_reader(req.headers);
    auto span = agent->NewSpan("MyService", req.path, req.method, req_reader);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }
    // Records the remote address, endpoint and request headers in one call
    pinpoint::helper::TraceHttpServerRequest(span, req.remote_addr, end_point, req_reader);

    // Sub-operation: a database query
    auto se = span->NewSpanEvent("queryDatabase");
    se->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    se->SetDestination("test_db");
    se->SetEndPoint("localhost:3306");
    se->SetSqlQuery("SELECT * FROM users", {});
    // ... execute the query ...
    se->EndEvent();

    res.set_content(R"({"users":[]})", "application/json");
    // httplib fills res.status only after the handler returns; set it
    // explicitly so the span records the real status instead of -1.
    res.status = 200;

    HttpHeaderReader res_reader(res.headers);
    span->RecordHeader(pinpoint::HTTP_RESPONSE, res_reader);

    span->SetAnnotation(pinpoint::ANNOTATION_API, "getUserInfo");
    span->SetStatusCode(res.status);
    span->SetUrlStat(req.path, req.method, res.status);
    span->EndSpan();   // must run on every path, including error returns
}

int main() {
    // Configure via environment or file
    setenv("PINPOINT_CPP_APPLICATION_NAME", "my-first-app", 0);
    setenv("PINPOINT_CPP_COLLECTOR_HOST", "localhost", 0);

    if (!pinpoint::StartAgent()) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    httplib::Server server;
    server.Get("/users", on_users);

    std::cout << "listening on 0.0.0.0:8090" << std::endl;
    server.listen("0.0.0.0", 8090);   // blocks until the server stops

    std::cout << "Shutting down agent..." << std::endl;
    agent->Shutdown();

    return 0;
}
```

See the full working program — several handlers, nested span events, URL
statistics — in [`example/http_server.cpp`](../example/http_server.cpp).

### Build and Run

cpp-httplib is header-only and is not installed with the agent, so point the
compiler at it and at `http_trace_context.h`:

```bash
g++ -std=c++17 -o my_app my_app.cpp -lpinpoint_cpp -pthread \
    -I/path/to/pinpoint-cpp-agent/3rd_party \
    -I/path/to/pinpoint-cpp-agent/example
```

```bash
./my_app & curl http://localhost:8090/users
```

The request appears in the Pinpoint Web UI as a transaction for
`my-first-app`.

---

## Example: Database Query

This example demonstrates tracing database operations (e.g., MySQL). It shows how to create `SpanEvent`s for SQL queries.

See full example: [`example/tutorial.cpp`](../example/tutorial.cpp)

```cpp
// Helper to trace DB operations
void trace_db_op(pinpoint::SpanPtr span,
                 const std::string& query,
                 std::function<void()> func) {
    auto se = span->NewSpanEvent("mysql_query");
    se->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    se->SetEndPoint("localhost:33060");
    se->SetDestination("test_db");
    se->SetSqlQuery(query, {});  // Record the query string (sanitize in production)

    try {
        func();  // Execute actual DB operation
    } catch (const std::exception& e) {
        se->SetError(e.what());  // Record error if any
        throw;
    }

    se->EndEvent();
}
```

---

## Next Steps

1. **Learn Advanced Instrumentation**: the [Instrumentation Guide](instrument.md)
   covers HTTP request/response tracing, database query tracing, distributed
   tracing, error handling, asynchronous work, and the API contracts you must
   respect. For plain C, see the [C API Guide](instrument_c.md).
2. **Explore Examples**: the `example/` directory has complete working programs
   for C++ (`http_server.cpp`, `tutorial.cpp`) and C (`http_server_c.c`,
   `tutorial_c.c`).
3. **Configure Advanced Options**: see the [Configuration Guide](config.md) for
   sampling strategies, URL statistics, SQL bind values, logging, and stats.
4. **Monitor Your Application**: use the Pinpoint Web UI to view service maps,
   analyze transaction traces, and identify bottlenecks.

---

## Troubleshooting

**Start with the agent log** — it answers almost every first-run problem, and it
is the only authoritative startup signal. See
[Verifying Agent Startup](trouble_shooting.md#verifying-agent-startup).

Nothing in the UI despite `AgentInfo sent`? The three usual causes are sampling
(`CounterRate: 1` samples everything — use it while testing), a span that is
never ended (`EndSpan()` must run on every code path), and the collection
interval (wait a few seconds).

For everything else — connection failures, memory or CPU concerns, missing
distributed traces — see the [Troubleshooting Guide](trouble_shooting.md).
