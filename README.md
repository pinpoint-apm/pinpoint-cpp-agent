# Pinpoint C++ Agent

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

The C++ Agent for [Pinpoint APM](https://github.com/pinpoint-apm/pinpoint), an open-source Application Performance Management tool for large-scale distributed systems.

Pinpoint C++ Agent enables you to monitor C++ applications using Pinpoint. Developers can instrument C++ applications to collect traces, analyze distributed call chains, and visualize service maps in the Pinpoint Web UI.

## Features

- Distributed tracing with automatic context propagation
- HTTP server/client request tracing
- Database query tracing (MySQL, etc.)
- Customizable sampling strategies (Counter, Percent, Throughput)
- Annotations for rich metadata
- Low-overhead, production-ready design
- Dynamic agent control (enable/disable at runtime)

## Requirements

| Requirement | Version |
|---|---|
| Pinpoint Collector | 2.4.0+ |
| C++ Compiler | C++17 (GCC 8+, Clang 6+) |
| Build System | Bazel 7.0+ or CMake 3.20+ |
| OS | Linux, macOS, Windows |

## Quick Start

### 1. Install

**CMake (FetchContent)**

```cmake
include(FetchContent)
FetchContent_Declare(
  pinpoint_cpp
  GIT_REPOSITORY https://github.com/pinpoint-apm/pinpoint-cpp-agent.git
  GIT_TAG main
)
FetchContent_MakeAvailable(pinpoint_cpp)

target_link_libraries(your_target PRIVATE pinpoint_cpp)
```

**Bazel**

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

### 2. Configure

Create a `pinpoint-config.yaml`:

```yaml
ApplicationName: "MyAppName"
Collector:
  GrpcHost: "my.collector.host"
Sampling:
  Type: "COUNTING"
  CounterRate: 1
Log:
  Level: "info"
```

Or use environment variables:

```bash
export PINPOINT_CPP_APPLICATION_NAME="MyAppName"
export PINPOINT_CPP_GRPC_HOST="my.collector.host"
```

### 3. Instrument

```cpp
#include "pinpoint/tracer.h"

int main() {
    // Initialize agent
    pinpoint::SetConfigFilePath("pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();

    // Create a span for an incoming request
    auto span = agent->NewSpan("C++ Server", "/api/endpoint");
    span->SetRemoteAddress("remote_addr");
    span->SetEndPoint("localhost:8080");

    // Record a sub-operation
    auto se = span->NewSpanEvent("processData");
    // ... your business logic ...
    span->EndSpanEvent();

    // End the span (sends trace to collector)
    span->EndSpan();

    agent->Shutdown();
    return 0;
}
```

## Building from Source

```bash
# CMake
mkdir build && cd build
cmake ..
cmake --build . -j$(nproc)

# Bazel
bazel build //...
```

### Running Tests

```bash
# CMake
cd build && ctest --verbose

# Bazel
bazel test //test/...
```

See the [Build Guide](doc/build.md) for detailed instructions including vcpkg integration, build options, and integration tests.

## Documentation

| Document | Description |
|---|---|
| [Quick Start Guide](doc/quick_start.md) | Step-by-step setup with full examples |
| [Configuration Guide](doc/config.md) | All configuration options, environment variables, and best practices |
| [Instrumentation Guide](doc/instrument.md) | API reference: spans, span events, annotations, distributed tracing |
| [Build Guide](doc/build.md) | Building from source with Bazel and CMake |
| [Troubleshooting](doc/trouble_shooting.md) | Debugging, logging, common issues and solutions |

## Examples

The `example/` directory contains complete working examples:

- **[tutorial.cpp](example/tutorial.cpp)** - Basic agent usage and span creation
- **[http_server.cpp](example/http_server.cpp)** - HTTP server instrumentation with context propagation

## Project Structure

```
pinpoint-cpp-agent/
├── include/pinpoint/    # Public headers (tracer.h)
├── src/                 # Library source files
├── v1/                  # Protobuf/gRPC service definitions
├── example/             # Example applications
├── test/                # Unit and integration tests
├── doc/                 # Documentation
├── 3rd_party/           # Vendored dependencies (httplib, MurmurHash3)
└── scripts/             # Valgrind helper scripts
```

## Dependencies

| Library | Purpose |
|---|---|
| [gRPC](https://grpc.io/) | Communication with Pinpoint Collector |
| [Protocol Buffers](https://protobuf.dev/) | Serialization |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | YAML configuration parsing |
| [Abseil](https://abseil.io/) | C++ common libraries |
| [fmt](https://fmt.dev/) | String formatting |

## Contributing

We are looking forward to your contributions via pull requests.

For tips on contributing code fixes or enhancements, please see the [Contributing Guide](CONTRIBUTING.md).

To report bugs or request features, please create an [Issue](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues).

## Community

- [Pinpoint APM](https://github.com/pinpoint-apm/pinpoint) - Main Pinpoint project
- [Pinpoint Documentation](https://pinpoint-apm.github.io/pinpoint/) - Official documentation

## License

Pinpoint C++ Agent is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for full license text.
