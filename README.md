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
- C++ and plain-C APIs, and a pre-fork (nginx, Apache, uWSGI) integration path

## Requirements

| Requirement | Version |
|---|---|
| Pinpoint Collector | 2.4.0+ |
| C++ Compiler | C++17 (GCC 8+, Clang 6+) |
| Build System | Bazel 7.0+ or CMake 3.21+ |
| OS | Linux, macOS, Windows |

## Quick Start

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

**Bazel** — add to your `MODULE.bazel`:

```python
bazel_dep(name = "pinpoint-cpp", version = "2.0.0")

# Not published to the Bazel Central Registry yet — point at the repository.
git_override(
    module_name = "pinpoint-cpp",
    remote = "https://github.com/pinpoint-apm/pinpoint-cpp-agent.git",
    branch = "main",
)
```

then depend on `@pinpoint-cpp//:pinpoint-cpp` from your target.

Then configure the agent, start it, and create your first span — the
[Quick Start Guide](doc/quick_start.md) walks through all three steps with
complete, runnable code.

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

## Examples

The `example/` directory contains complete working examples:

- **[proxy.cpp](example/proxy.cpp)** — HTTP proxy: client-side tracing and cross-process context propagation
- **[server.cpp](example/server.cpp)** — HTTP backend: server-side tracing, a MySQL span event, and an async span
- **[README.md](example/README.md)** — how to run the full distributed-trace demo: proxy → server → MySQL

More examples are in the
[pinpoint-cpp-examples](https://github.com/pinpoint-apm/pinpoint-cpp-examples)
repository. For C applications, see the
[civetweb example](https://github.com/pinpoint-apm/pinpoint-cpp-examples/tree/main/civetweb)
and the
[nginx module](https://github.com/pinpoint-apm/pinpoint-cpp-examples/blob/main/nginx/ngx_http_pinpoint_module.c).

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
