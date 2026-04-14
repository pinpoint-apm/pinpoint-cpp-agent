# Pinpoint C++ Agent - Build Guide

This document describes how to build the Pinpoint C++ Agent from source. Two build systems are supported: **Bazel** and **CMake**.

---

## Table of Contents

- [Requirements](#requirements)
- [Project Structure](#project-structure)
- [Build with Bazel](#build-with-bazel)
- [Build with CMake](#build-with-cmake)
  - [CMake + FetchContent](#cmake--fetchcontent)
  - [CMake + vcpkg](#cmake--vcpkg)
- [Build Options](#build-options)
- [Running Tests](#running-tests)
- [Integration Test](#integration-test)
- [Troubleshooting](#troubleshooting)

---

## Requirements

| Requirement | Version |
|---|---|
| C++ compiler | C++17 support (GCC 7+, Clang 5+, MSVC 2017+) |
| Bazel | 7.0+ |
| CMake | 3.20+ |
| OS | Linux, macOS, Windows |

---

## Project Structure

```
pinpoint-cpp-agent/
├── BUILD.bazel          # Bazel: main library target
├── MODULE.bazel         # Bazel: module dependencies (bzlmod)
├── CMakeLists.txt       # CMake: root build file
├── vcpkg.json           # vcpkg: dependency manifest
├── include/             # Public headers
│   └── pinpoint/
│       └── tracer.h
├── src/                 # Library source files
├── v1/                  # Protobuf/gRPC service definitions
├── 3rd_party/           # Vendored third-party code (httplib, MurmurHash3)
├── example/             # Example applications
├── test/                # Unit tests
│   └── it_test/         # Integration test (HTTP + gRPC + SQL tracing)
└── scripts/             # Valgrind helper scripts
```

---

## Build with Bazel

Bazel uses [bzlmod](https://bazel.build/external/module) (`MODULE.bazel`) for dependency management. All external dependencies are resolved automatically from the [Bazel Central Registry](https://registry.bazel.build/).

### Build all targets

```bash
bazel build //...
```

### Build the library only

```bash
bazel build //:pinpoint-cpp
```

### Build examples

```bash
bazel build //example/...
```

### Build integration test binaries

```bash
bazel build //test/it_test/...
```

### Bazel dependencies

Dependencies are declared in `MODULE.bazel`:

| Dependency | Version | Purpose |
|---|---|---|
| protobuf | 29.2 | Protocol Buffers serialization |
| grpc | 1.63.1 | gRPC communication with Pinpoint collector |
| yaml-cpp | 0.8.0 | YAML configuration parsing |
| abseil-cpp | 20240722.0 | Abseil C++ common libraries |
| fmt | 11.1.4 | String formatting |
| googletest | 1.17.0 | Unit testing framework |

---

## Build with CMake

CMake supports two dependency resolution strategies:

1. **FetchContent (default)**: Dependencies are downloaded and built from source automatically. No extra setup required.
2. **vcpkg**: Dependencies are pre-installed via vcpkg package manager.

### CMake + FetchContent

This is the simplest approach. CMake will download and build all dependencies from source.

```bash
mkdir build && cd build
cmake ..
cmake --build . -j$(nproc)
```

On macOS, replace `nproc` with `sysctl -n hw.ncpu`:

```bash
cmake --build . -j$(sysctl -n hw.ncpu)
```

### CMake + vcpkg

If you prefer faster builds by using pre-built packages:

1. **Install vcpkg** (if not already installed):

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg
```

2. **Install dependencies**:

```bash
vcpkg install grpc protobuf yaml-cpp fmt abseil
```

3. **Build**:

```bash
mkdir build && cd build
cmake ..
cmake --build . -j$(nproc)
```

CMake auto-detects vcpkg via the `VCPKG_ROOT` environment variable. To force FetchContent even when vcpkg is available:

```bash
cmake .. -DFORCE_FETCHCONTENT=ON
```

---

## Build Options

The following CMake options are available:

| Option | Default | Description |
|---|---|---|
| `BUILD_TESTING` | ON | Build unit tests |
| `BUILD_EXAMPLES` | ON | Build example applications |
| `BUILD_SHARED_LIBS` | OFF | Build as a shared library (.so / .dylib) |
| `BUILD_STATIC_LIBS` | ON | Build as a static library (.a) |
| `BUILD_C_WRAPPER` | ON | Build C wrapper library |
| `FORCE_FETCHCONTENT` | OFF | Force FetchContent even when vcpkg is available |
| `BUILD_COVERAGE` | OFF | Enable code coverage (GCC only) |

Example:

```bash
cmake .. -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF
```

---

## Running Tests

### Bazel

```bash
# Run all tests
bazel test //test/...

# Run a specific test
bazel test //test:test_sampling

# Run all tests with verbose output
bazel test //test/... --test_output=all
```

### CMake

```bash
cd build

# Run all tests
ctest

# Run with verbose output
ctest --verbose

# Run a specific test
ctest -R test_sampling
```

### Available unit tests

| Test | Description |
|---|---|
| `test_limiter` | Rate limiter logic |
| `test_sampling` | Sampling strategy |
| `test_cache` | Cache operations |
| `test_http` | HTTP header parsing |
| `test_annotation` | Annotation handling |
| `test_callstack` | Call stack tracking |
| `test_config` | YAML configuration loading |
| `test_sql` | SQL query normalization |
| `test_stat` | Statistics collection |
| `test_url_stat` | URL statistics |
| `test_span_event` | Span event lifecycle |
| `test_span` | Span lifecycle |
| `test_noop` | No-op mode behavior |
| `test_grpc` | gRPC transport |
| `test_grpc_with_mocks` | gRPC transport with mock services |

---

## Integration Test

The integration test (`test/it_test/`) runs a full HTTP + gRPC server stack with the agent enabled, then sends traffic to verify correctness and detect memory leaks.

### Build

```bash
# Bazel
bazel build //test/it_test/...

# CMake
cd build && cmake --build . --target grpc_server it_test_server
```

### Run

```bash
# 1. Start the gRPC backend server
./grpc_server &

# 2. Start the HTTP test server (connects to gRPC server)
./it_test_server &

# 3. Run the integration test script
./test/it_test/it_test.sh
```

The test script supports several modes and options:

```bash
# Run for 120 seconds with 20 concurrent workers in mixed mode
./it_test.sh -d 120 -c 20 -m mixed

# Stress test with 50 concurrent workers
./it_test.sh -m stress -d 300

# Test SQL tracing endpoints only
./it_test.sh -m db-all -d 60 -c 5

# Test gRPC endpoints only
./it_test.sh -m grpc-all -d 60 -c 10

# Full test (HTTP + gRPC + SQL)
./it_test.sh -m full -d 180 -c 15
```

---

## Troubleshooting

### Bazel: slow first build

The first Bazel build downloads and compiles all external dependencies (gRPC, protobuf, etc.), which can take several minutes. Subsequent builds use the cache and are much faster.

### CMake: FetchContent is slow

Similar to Bazel, FetchContent downloads and builds dependencies from source on the first run. Using vcpkg with pre-built packages avoids this overhead.

### macOS linker warnings

On macOS, you may see warnings like:

```
ld: warning: ignoring duplicate libraries: '-lm', '-lpthread'
```

These warnings are harmless and can be safely ignored.

### Empty table of contents warnings (Bazel)

Warnings such as:

```
warning: archive library: ... the table of contents is empty
```

These are expected for some gRPC/protobuf static libraries on macOS and do not affect the build.

### Valgrind

Helper scripts for running tests under Valgrind are available in `scripts/`:

```bash
# Run unit tests under Valgrind
./scripts/run_valgrind_tests.sh

# Run integration tests under Valgrind
./scripts/run_valgrind_it_test.sh
```

A suppression file for known gRPC/protobuf false positives is provided at `scripts/grpc_protobuf.supp`.
