# Pinpoint C++ Agent

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

The C++ Agent for [Pinpoint APM](https://github.com/pinpoint-apm/pinpoint), an open-source Application Performance Management tool for large-scale distributed systems.

Pinpoint C++ Agent enables you to monitor C++ applications using Pinpoint. Developers can instrument C++ applications to collect traces, analyze distributed call chains, and visualize service maps in the Pinpoint Web UI.

## Requirements

| Requirement | Version |
|---|---|
| Pinpoint Collector | 3.1.0+ |
| C++ Compiler | C++17 (GCC 8+, Clang 6+) |
| Build System | Bazel 7.0+ or CMake 3.21+ |
| OS | Linux, macOS |

## Building from Source

```bash
# CMake
cmake --preset default
cmake --build --preset default
ctest --preset default

# Bazel
bazel build //...
bazel test //test/...
```

See the [Build Guide](doc/build.md) for submodule setup, vcpkg and FetchContent
dependency builds, build options, coverage, sanitizers, and the integration
tests.

## Examples

Start the agent, then trace a request as a span and each unit of work within
it as a span event (trimmed from [proxy.cpp](example/proxy.cpp)):

```cpp
#include <cstdlib>
#include <iostream>

#include "pinpoint/tracer.h"

int main() {
    // Set the application name shown in the Pinpoint Web UI.
    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-proxy", 0);

    // Start the agent. Configuration comes from a config file or
    // PINPOINT_CPP_* environment variables (see doc/config.md).
    if (!pinpoint::StartAgent()) {
        std::cerr << "failed to start the pinpoint agent: check the agent log" << std::endl;
    }

    // Create a span for an incoming request.
    auto span = pinpoint::GlobalAgent()->NewSpan("C++ Proxy", "/api/members");

    // Trace a unit of work within the request as a span event.
    auto event = span->NewSpanEvent("proxy.forward");
    // ... do the work ...
    event->EndEvent();

    // Finish the span; it is delivered to the collector asynchronously.
    span->EndSpan();

    pinpoint::GlobalAgent()->Shutdown();
}
```

The `example/` directory contains complete working examples:

- **[proxy.cpp](example/proxy.cpp)** — HTTP proxy: client-side tracing and cross-process context propagation
- **[server.cpp](example/server.cpp)** — HTTP backend: server-side tracing, a MySQL span event, and an async span

More examples are in the
[pinpoint-cpp-examples](https://github.com/pinpoint-apm/pinpoint-cpp-examples)
repository.

## Documentation

| Document | Description |
|---|---|
| [Quick Start Guide](doc/quick_start.md) | Step-by-step setup with full examples |
| [Configuration Guide](doc/config.md) | All configuration options, environment variables, and best practices |
| [Instrumentation Guide](doc/instrument.md) | C++ API reference: spans, span events, annotations, distributed tracing |
| [C API Instrumentation Guide](doc/instrument_c.md) | The same, for plain C via `tracer_c.h` (`pt_*` functions) |
| [API Contracts](doc/api_contracts.md) | Threading, end-exactly-once, overflow and noop-span rules the agent enforces on spans, events and annotations |
| [Pre-fork Integration Guide](doc/prefork.md) | Running the agent inside pre-fork servers (nginx, Apache prefork, uWSGI) |
| [Build Guide](doc/build.md) | Building from source with Bazel and CMake |
| [Troubleshooting](doc/trouble_shooting.md) | Startup contract, logging, common issues and solutions |
| [Measured Complexity Decisions](doc/complexity_decisions.md) | Benchmark-adjudicated verdicts on the perf/semantics machinery kept after the code audit |

## Contributing

We are looking forward to your contributions via pull requests.

For tips on contributing code fixes or enhancements, please see the [Contributing Guide](CONTRIBUTING.md).

To report bugs or request features, please create an [Issue](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues).

## Community

- [Pinpoint APM](https://github.com/pinpoint-apm/pinpoint) - Main Pinpoint project
- [Pinpoint Documentation](https://pinpoint-apm.github.io/pinpoint/) - Official documentation

## License

Pinpoint C++ Agent is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for full license text.
