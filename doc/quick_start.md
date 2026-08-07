# Pinpoint C++ Agent - Quick Start Guide

This guide helps you get started with the Pinpoint C++ Agent (`pinpoint-cpp-agent`) for monitoring your C++ applications.

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Configuration](#configuration)
- [Basic Usage](#basic-usage)
- [Stopping and Resuming the Agent](#stopping-and-resuming-the-agent)
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
- **Build System**: CMake 3.21+ or Bazel
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
    deps = ["@pinpoint_cpp//:pinpoint-cpp"],
)
```

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
#include "pinpoint/tracer.h"

int main() {
    pinpoint::AgentOptions options;
    options.config_file_path = "/path/to/pinpoint-config.yaml";
    if (!pinpoint::StartAgent(options)) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    // Your application code

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
#include "pinpoint/tracer.h"

int main() {
    std::string config = R"(
        ApplicationName: "MyApplication"
        Collector:
          Host: "localhost"
    )";

    pinpoint::AgentOptions options;
    options.config_yaml = config;
    if (!pinpoint::StartAgent(options)) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    // Your application code

    agent->Shutdown();
    return 0;
}
```

For a complete list of configuration options, see the [Configuration Guide](config.md).

---

## Basic Usage

The typical workflow follows five steps:

1. **Initialize** — call `StartAgent()` with your configuration at application startup, in the process that records spans (for pre-fork servers: in each worker, see the [Pre-fork Integration Guide](prefork.md)).
2. **Trace** — use `Agent::NewSpan` to start tracing a transaction.
3. **Record work** — create span events and annotations for sub-operations.
4. **End** — call `EndSpan()` when the transaction completes.
5. **Shutdown** — call `agent->Shutdown()` before the application exits.

> **`Shutdown()` is terminal for an agent instance.** The same handle can never be
> brought back online: `Enable()` stays `false`, and every span it returns is a
> noop span. The
> application itself keeps running normally — it just stops being traced. To stop
> and later resume tracing in a long-running process, see
> [Stopping and Resuming the Agent](#stopping-and-resuming-the-agent).

### Initialize the Agent

```cpp
#include "pinpoint/tracer.h"

int main() {
    pinpoint::AgentOptions options;
    options.config_file_path = "pinpoint-config.yaml";
    if (!pinpoint::StartAgent(options)) {
        // StartAgent() returns false on a configuration or setup failure —
        // the application keeps running untraced.
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    // StartAgent() returns immediately; the agent registers with the
    // collector on a background thread. Verify startup via the agent log —
    // do NOT gate application startup on Enable() here (see the note below).

    // Your application code here

    agent->Shutdown();
    return 0;
}
```

> **`StartAgent()` is asynchronous.** It returns right away, and a background
> thread connects to the collector, registers the agent (retrying until it
> succeeds) and starts the workers. A `false` return reports a *synchronous*
> configuration or setup failure — print a message pointing at the agent log,
> as in the example above. (One `false` is not a failure: a deliberate
> `Enable: false` also returns `false`, and it is the only one that leaves no
> trace in the agent log. See the
> [Configuration Guide](config.md#agent-configuration).) A `true` return means
> initialization was launched,
> NOT that registration already succeeded: `Enable()` flips to `true` only
> after that registration completes, so it is normally still `false`
> immediately after `StartAgent()` returns — checking it there is meaningless.
> **Verify agent start through the agent log**: `AgentInfo sent` on success,
> error entries such as `agent start failed: ...` on failure. Even if the
> agent fails to start, the application is unaffected — every tracing call is
> a safe noop and only the traces are lost. Use `Enable()` solely as a cheap
> fast-fail guard before creating a span (skip instrumentation while tracing
> is off), never as a startup success check.

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

    dbEvent->EndEvent();
    span->EndSpan();
}
```

### Add Annotations

Annotations provide additional metadata:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyOperation", "/api/endpoint");

    span->SetAnnotation(pinpoint::ANNOTATION_API, "getUserInfo");
    span->SetAnnotation(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

    // Your business logic here

    span->EndSpan();
}
```

---

## Stopping and Resuming the Agent

Most applications start the agent once and shut it down at exit. If yours needs to
stop and later resume tracing while it keeps running, use a **fresh agent for each
cycle** — an agent instance cannot be restarted.

```cpp
// Stop tracing. The application keeps working; it is simply no longer traced.
agent->Shutdown();
agent.reset();          // drop the dead handle so nothing keeps using it

// ... later: resume tracing with a NEW agent ...
if (!pinpoint::StartAgent(options)) {
    std::cerr << "failed to restart the pinpoint agent: check the agent log" << std::endl;
}
agent = pinpoint::GlobalAgent();
```

What each call guarantees:

| Call | Behavior after `Shutdown()` |
|---|---|
| `agent->Enable()` | Always `false`. |
| `agent->NewSpan(...)` | Returns the shared noop span. Safe to call, records nothing. |
| `pinpoint::StartAgent(...)` | Builds a fresh agent, installs it as the global agent and returns `true`; returns `false` when the rebuild fails. |
| `pinpoint::GlobalAgent()` | Returns the noop agent until `StartAgent()` installs a new one. |

Points to keep in mind:

- **Never reuse the shut-down handle.** It stays permanently disabled; get the
  replacement handle from `StartAgent()`. The new agent registers with the
  collector in the background — watch the agent log (`AgentInfo sent`) if you
  need confirmation that it came online.
- **Drop cached agent pointers.** Any `AgentPtr` the application cached — in a
  thread-local, a service object, a C `pt_agent_t` handle — still points at the
  dead agent and keeps producing noop spans. Re-fetch with `GlobalAgent()` or the
  new handle.
- **Spans that outlive the shutdown are safe.** A span created before `Shutdown()`
  can still be ended afterwards without crashing; its data is simply dropped.
- **Each cycle registers as a new agent instance.** Every `StartAgent()`
  re-resolves the agent identity with a freshly auto-generated agent id. Set
  `AgentName` for a stable label in the Pinpoint UI across cycles.
- **Shutdown is synchronous but bounded.** It joins the worker threads while
  queued spans drain, and returns within a 3-second deadline even if a worker
  is stuck in an unresponsive RPC (stragglers finish draining on a background
  thread). Still, do not call it from a latency-sensitive path.

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
    spanEvent->EndEvent();

    // Add result annotations
    span->SetAnnotation(pinpoint::ANNOTATION_API, "doWork");
    span->SetAnnotation(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);

    span->EndSpan();
}

int main() {
    // Configure via environment or file
    setenv("PINPOINT_CPP_APPLICATION_NAME", "my-first-app", 0);
    setenv("PINPOINT_CPP_COLLECTOR_HOST", "localhost", 0);

    // Create and start agent. StartAgent() returns immediately while the
    // agent registers with the collector in the background — check the agent
    // log ("AgentInfo sent") to confirm it came online. If it fails, the app
    // still runs normally; it is just not traced.
    if (!pinpoint::StartAgent()) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

    std::cout << "Pinpoint agent starting" << std::endl;

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
    se->EndEvent();
    span->SetStatusCode(res.status);
    span->EndSpan();
}

int main() {
    pinpoint::AgentOptions options;
    options.config_file_path = "pinpoint-config.yaml";
    if (!pinpoint::StartAgent(options)) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }
    auto agent = pinpoint::GlobalAgent();

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
    se->SetSqlQuery(query, {});  // Record the query string (sanitize in production)

    try {
        func();  // Execute actual DB operation
    } catch (const std::exception& e) {
        se->SetError(e.what());  // Record error if any
        throw;
    }

    se->EndEvent();
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

**Start with the agent log.** Almost every first-run problem is answered there,
and the log is the *only* authoritative signal — `StartAgent()` returns before
registration completes, so `Enable()` right after it is normally still `false`
even on a healthy start:

| In the agent log | Meaning |
|---|---|
| `AgentInfo sent` | The agent registered with the collector. Startup succeeded. |
| `agent start failed: ...` | Configuration or setup error; the line names the cause. |
| `failed to send AgentInfo` | The collector is not reachable yet. Retried indefinitely. |
| *(nothing at all)* | Check `Enable` — a deliberate `Enable: false` returns `false` from `StartAgent()` and logs nothing. |

Set `Log.Level: "debug"` for the full picture, including the resolved
configuration. Whatever the log says, **your application keeps working**: a
failed agent start costs you traces, nothing else.

Nothing in the UI despite `AgentInfo sent`? The three usual causes are sampling
(`CounterRate: 1` samples everything — use it while testing), a span that is
never ended (`EndSpan()` must run on every code path), and the collection
interval (wait a few seconds).

For everything else — connection failures, memory or CPU concerns, missing
distributed traces, and the diagnostic commands to run — see the
[Troubleshooting Guide](trouble_shooting.md).

---

## Support

- **GitHub Issues**: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
- **Pinpoint Documentation**: [Pinpoint APM](https://pinpoint-apm.github.io/pinpoint/)
- **Community**: Use the main Pinpoint project and issue tracker for discussions

---

## License

Pinpoint C++ Agent is licensed under the Apache License 2.0. See [LICENSE](../LICENSE) for details.
