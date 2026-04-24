# Pinpoint C++ Agent - Build Guide

This document describes how to build the Pinpoint C++ Agent from source. Two build systems are supported: **Bazel** and **CMake**.

---

## Table of Contents

- [Requirements](#requirements)
- [Cloning the Repository](#cloning-the-repository)
- [Project Structure](#project-structure)
- [Build with Bazel](#build-with-bazel)
- [Build with CMake](#build-with-cmake)
  - [CMake Presets](#cmake-presets)
  - [Dependency Versions](#dependency-versions)
  - [Package Managers](#package-managers)
  - [Compiler Cache (ccache)](#compiler-cache-ccache)
- [Build Options](#build-options)
- [Running Tests](#running-tests)
- [Integration Test](#integration-test)
- [Troubleshooting](#troubleshooting)

---

## Requirements

| Requirement | Version |
|---|---|
| C++ compiler | C++17 support (GCC 8+, Clang 6+) |
| Bazel | 7.0+ |
| CMake | 3.21+ |
| Ninja (recommended) | Used by all CMake presets |
| ccache (optional) | Auto-detected; speeds up incremental builds |
| OS | Linux, macOS, Windows |

---

## Cloning the Repository

The Protobuf/gRPC service definitions live in a git submodule
([pinpoint-apm/pinpoint-grpc-idl](https://github.com/pinpoint-apm/pinpoint-grpc-idl))
under `3rd_party/pinpoint-grpc-idl`. Fetch it when cloning:

```bash
git clone --recurse-submodules https://github.com/pinpoint-apm/pinpoint-cpp-agent.git
```

Or, if you already cloned without the flag:

```bash
git submodule update --init --recursive
```

---

## Project Structure

```
pinpoint-cpp-agent/
├── BUILD.bazel          # Bazel: main library target
├── MODULE.bazel         # Bazel: module dependencies (bzlmod)
├── CMakeLists.txt       # CMake: root build file (toolchain-agnostic)
├── CMakePresets.json    # CMake: presets (default / vcpkg / conan / fetchcontent / debug)
├── vcpkg.json           # vcpkg manifest (used by the `vcpkg` preset)
├── conanfile.txt        # Conan 2 requirements (used by the `conan` preset)
├── include/             # Public headers
│   └── pinpoint/
│       └── tracer.h
├── src/                 # Library source files
├── 3rd_party/           # Vendored third-party code (httplib, MurmurHash3)
│   └── pinpoint-grpc-idl/  # Protobuf/gRPC IDL (git submodule)
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

`CMakeLists.txt` is toolchain-agnostic: dependencies are located via `find_package(CONFIG)`, and any package manager that integrates with CMake (vcpkg, Conan, system packages, Nix, Spack, ...) can be plugged in at configure time via `CMAKE_TOOLCHAIN_FILE` or `CMAKE_PREFIX_PATH`. If a package is not found, CMake falls back to building it from source via `FetchContent`.

Common configurations are packaged as presets in `CMakePresets.json`.

### CMake Presets

List available presets:

```bash
cmake --list-presets
```

| Preset | Toolchain / source of dependencies |
|---|---|
| `default` | `find_package` against system / `CMAKE_PREFIX_PATH`, FetchContent fallback |
| `vcpkg` | vcpkg toolchain (requires `VCPKG_ROOT` env var) |
| `conan` | Conan 2 toolchain (requires `conan install` first) |
| `fetchcontent` | Force all deps to be built from source |
| `debug` | Same as `default` with `CMAKE_BUILD_TYPE=Debug` |

Configure + build + test using a preset:

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Each preset writes to its own build directory (`build/<preset-name>/`), so you can keep several configurations side by side.

### Dependency Versions

`vcpkg.json` and `conanfile.txt` pin dependency versions to match the ones used by `FetchContent` as closely as possible. Where an exact match is not in the registry, the closest available version is pinned and called out below.

| Package | FetchContent | vcpkg | Conan Center |
|---|---|---|---|
| gRPC      | `v1.76.0` | `1.76.0` | `1.78.1` ¹ |
| Protobuf  | bundled with gRPC | transitive via grpc | transitive via grpc (`>=5.27 <7`) |
| Abseil    | bundled with gRPC | transitive via grpc | transitive via grpc |
| yaml-cpp  | `0.8.0` | `0.8.0` | `0.8.0` |
| fmt       | `11.2.0` | `11.2.0` | `11.2.0` |
| GoogleTest | `v1.17.0` | `1.17.0` | `1.17.0` |

¹ Conan Center only publishes `1.54.3`, `1.69.0`, and `1.78.1` for gRPC, so the closest version to the FetchContent/vcpkg target (`1.76.0`) is pinned on the Conan side. The public API is source-compatible with what this project uses.

**Bumping vcpkg baseline:** `vcpkg.json` pins `builtin-baseline` to a specific vcpkg commit. If you want to follow vcpkg master, update it via:

```bash
cd $VCPKG_ROOT && git rev-parse HEAD
# paste into "builtin-baseline" in vcpkg.json
```

### Package Managers

#### System packages / `CMAKE_PREFIX_PATH`

Install the required packages through your package manager and use the `default` preset.

**macOS (Homebrew):**

```bash
brew install grpc protobuf abseil fmt yaml-cpp ninja
cmake --preset default
cmake --build --preset default
```

**Debian / Ubuntu (apt):**

```bash
sudo apt update
sudo apt install -y \
  cmake ninja-build pkg-config \
  libgrpc++-dev protobuf-compiler-grpc \
  libprotobuf-dev protobuf-compiler \
  libabsl-dev libfmt-dev libyaml-cpp-dev \
  libgtest-dev libgmock-dev
cmake --preset default
cmake --build --preset default
```

> Ubuntu 22.04 ships gRPC 1.30 and Protobuf 3.12, which satisfy the agent's build requirements. On older distros where `libabsl-dev` / `libgrpc++-dev` are missing or too old, use the `vcpkg` or `fetchcontent` preset instead.

**Fedora / RHEL (dnf):**

```bash
sudo dnf install -y \
  cmake ninja-build pkgconf-pkg-config \
  grpc-devel grpc-plugins \
  protobuf-devel protobuf-compiler \
  abseil-cpp-devel fmt-devel yaml-cpp-devel \
  gtest-devel gmock-devel
cmake --preset default
cmake --build --preset default
```

If the packages live in a non-standard prefix, set `CMAKE_PREFIX_PATH`:

```bash
cmake --preset default -DCMAKE_PREFIX_PATH=/opt/my-libs
```

#### vcpkg

Dependencies are declared in `vcpkg.json` (manifest mode). vcpkg installs them automatically during the first configure.

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg

cmake --preset vcpkg
cmake --build --preset vcpkg
```

The `vcpkg` preset picks `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake` as its toolchain file. Classic mode (`vcpkg install grpc protobuf ...`) still works if you prefer it — just delete `vcpkg.json` first.

#### Conan 2

Dependencies are declared in `conanfile.txt` (root of the repo).

```bash
pip install "conan>=2.0"
conan profile detect --force   # first time only

conan install . \
  --output-folder=build/conan \
  --build=missing \
  -s build_type=Release \
  -c tools.cmake.cmaketoolchain:generator=Ninja

cmake --preset conan
cmake --build --preset conan
```

The `conan` preset expects `build/conan/conan_toolchain.cmake` to exist, which is produced by the `conan install` step above.

#### FetchContent (no package manager)

```bash
cmake --preset fetchcontent
cmake --build --preset fetchcontent
```

This ignores any installed packages and compiles every dependency from source. Slow, but hermetic.

### Compiler Cache (ccache)

If `ccache` is available on `PATH`, `CMakeLists.txt` wires it up automatically as `CMAKE_C_COMPILER_LAUNCHER` / `CMAKE_CXX_COMPILER_LAUNCHER`. Verify:

```bash
cmake --preset default    # look for "ccache found: ..." in the output
ccache -s                 # after a build, check hit/miss counters
```

Disable with:

```bash
cmake --preset default -DUSE_CCACHE=OFF
```

Install ccache with `brew install ccache` (macOS) or `apt install ccache` (Debian/Ubuntu).

---

## Build Options

The following CMake options are available:

| Option | Default | Description |
|---|---|---|
| `BUILD_TESTING` | ON | Build unit tests |
| `BUILD_EXAMPLES` | ON | Build example applications |
| `BUILD_SHARED_LIBS` | OFF | Build as a shared library (.so / .dylib) |
| `BUILD_STATIC_LIBS` | ON | Build as a static library (.a) |
| `BUILD_COVERAGE` | OFF | Enable code coverage (GCC only) |
| `USE_CCACHE` | ON | Use ccache as compiler launcher if found on PATH |

Example:

```bash
cmake --preset default -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF
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
# Run all tests (uses the "default" test preset)
ctest --preset default

# Verbose output
ctest --preset default --verbose

# Run a specific test
ctest --preset default -R test_sampling
```

Substitute the preset name (`vcpkg`, `conan`, `fetchcontent`, `debug`) to run against a different build directory.

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
cmake --build --preset default --target grpc_server it_test_server
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

The `fetchcontent` preset (and the fallback path in the `default` preset when no system packages are found) downloads and builds every dependency from source on the first run. Use a package manager (vcpkg / Conan / Homebrew / apt / ...) for pre-built packages to avoid this overhead. After the first configure, `ccache` will also dramatically speed up subsequent rebuilds.

### Migrating from `FORCE_FETCHCONTENT=ON`

`FORCE_FETCHCONTENT` has been removed. Use the `fetchcontent` preset instead:

```bash
cmake --preset fetchcontent
cmake --build --preset fetchcontent
```

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
