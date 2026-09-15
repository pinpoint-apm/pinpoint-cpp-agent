# Pinpoint C++ Agent - Build Guide

This document describes how to build the Pinpoint C++ Agent from source and link
it into your application. Two build systems are supported: **Bazel** and **CMake**.

To consume the agent without building it yourself first — a `FetchContent` block
or a `bazel_dep` in your own project — see the
[Quick Start Guide](quick_start.md#installation).

> Working **on** the agent rather than with it? The test suites, coverage,
> sanitizers, benchmarks and CI environments are in the
> [Development Guide](development.md).

---

## Requirements

| Requirement | Version |
|---|---|
| C++ compiler | C++17 support (GCC 8+, Clang 6+) |
| Bazel | 7.0+ |
| CMake | 3.21+ |
| Ninja (recommended) | Used by all CMake presets |
| OS | Linux, macOS |

---

## Getting the Source

The Protobuf/gRPC service definitions live in a git submodule
([pinpoint-apm/pinpoint-grpc-idl](https://github.com/pinpoint-apm/pinpoint-grpc-idl))
under `3rd_party/pinpoint-grpc-idl`. Depending on how you obtain the source, you
may need to populate it explicitly.

### Option A — Release tarball (recommended for packaging)

Download `pinpoint-cpp-agent-<version>.tar.gz` from the
[Releases](https://github.com/pinpoint-apm/pinpoint-cpp-agent/releases) page.
The IDL is already bundled inside, so the tarball builds offline with no extra
steps.

> GitHub's auto-generated "Source code (zip/tar.gz)" archives do **not**
> include submodule contents, and the build fails without them. Use the release
> asset above or a git clone.

### Option B — Git clone

```bash
git clone --recurse-submodules https://github.com/pinpoint-apm/pinpoint-cpp-agent.git
```

Or, if you already cloned without the flag:

```bash
git submodule update --init --recursive
```

CMake also auto-runs `git submodule update --init --recursive` on configure if
the submodule directory is empty, as long as `git` is on `PATH`.

---

## Build with Bazel

Bazel uses [bzlmod](https://bazel.build/external/module) (`MODULE.bazel`) for dependency management. All external dependencies — and their pinned versions — are declared in `MODULE.bazel` and resolved automatically from the [Bazel Central Registry](https://registry.bazel.build/).

```bash
bazel build //...                # everything
bazel build //:pinpoint-cpp      # the library only
bazel build //example/...        # examples
```

---

## Build with CMake

The CMake build supports exactly two dependency providers. When the vcpkg
toolchain is active, packages come from the repository's `vcpkg.json` manifest.
Without that toolchain, all library dependencies are built from pinned sources
via `FetchContent`. System packages, `CMAKE_PREFIX_PATH`, and other CMake package
managers are intentionally not used.

Common configurations are packaged as presets in `CMakePresets.json`.

### CMake Presets

List available presets:

```bash
cmake --list-presets
```

| Preset | Toolchain / source of dependencies |
|---|---|
| `default` | Build pinned dependency sources with FetchContent |
| `vcpkg` | vcpkg toolchain (requires `VCPKG_ROOT` env var) |
| `debug` | Same as `default` with `CMAKE_BUILD_TYPE=Debug` |

Configure + build + test using a preset:

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Each preset writes to its own build directory (`build/<preset-name>/`), so you can keep several configurations side by side.

> `CMakePresets.json` also carries `coverage`, `profiling`, `asan`, `tsan` and
> `ubsan` presets for work on the agent itself; they are documented in the
> [Development Guide](development.md#development-presets).

### Dependency Providers

#### vcpkg

Dependencies are declared in `vcpkg.json` (manifest mode). vcpkg installs them automatically during the first configure.

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg

cmake --preset vcpkg
cmake --build --preset vcpkg
```

The `vcpkg` preset picks `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake` as
its toolchain file. The manifest includes GoogleTest, so test dependencies use
the same provider as production dependencies.

#### FetchContent

Use any preset without the vcpkg toolchain (including `default`) to download and
build the pinned sources. GoogleTest is also built with FetchContent when tests
are enabled. Installed system libraries are ignored even when they are visible
through the default CMake search paths or `CMAKE_PREFIX_PATH`.

### Dependency Versions

Dependency versions are pinned in `vcpkg.json` and the
`FetchContent_Declare` calls in `cmake/PinpointDependencies.cmake`, and match as
closely as the registries allow. One caveat worth knowing: Protobuf and Abseil
are transitive dependencies of gRPC in every path; they are not pinned
separately.

---

## Build Options

The following CMake options are available:

| Option | Default | Description |
|---|---|---|
| `BUILD_TESTING` | ON | Build unit tests |
| `BUILD_EXAMPLES` | ON | Build example applications |
| `BUILD_SHARED_LIBS` | ON | Build as a shared library (.so / .dylib) |
| `BUILD_STATIC_LIBS` | ON | Build as a static library (.a) |
| `BUILD_COVERAGE` | OFF | Enable coverage instrumentation (Clang/LLVM or GCC) |
| `BUILD_PROFILING` | OFF | Preserve symbols and reliable call stacks for sampling profilers |
| `BUILD_BENCHMARKS` | OFF | Build the standalone microbenchmarks in `benchmark/` |
| `SANITIZE` | (empty) | Enable a sanitizer: `address`, `thread`, or `undefined`. Dependencies are rebuilt from source with the same instrumentation |

Embedding the agent in an application usually means turning the first two off:

```bash
cmake --preset default -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
```

The last four are development options; see the
[Development Guide](development.md) for coverage, profiling, sanitizers and
benchmarks.

---

## Running Tests

A quick check that the build is sound:

```bash
ctest --preset default     # CMake
bazel test //test/...      # Bazel
```

The full test story — the mock-collector suite, the live integration tests,
coverage and the sanitizer builds — is in the
[Development Guide](development.md#running-tests).

---

## Troubleshooting

For runtime problems — the agent starting but not reporting, collector
connectivity, missing spans — see the
[Troubleshooting Guide](trouble_shooting.md). This section covers build failures
only.

### Bazel: slow first build

The first Bazel build downloads and compiles all external dependencies (gRPC, protobuf, etc.), which can take several minutes. Subsequent builds use the cache and are much faster.

### CMake: the first FetchContent build is slow

The `default` preset downloads and builds all dependencies from source on the
first run. Use the `vcpkg` preset for managed packages, or set
`FETCHCONTENT_BASE_DIR` to reuse one dependency cache across presets and build
directories — see
[Sharing the FetchContent cache](development.md#sharing-the-fetchcontent-cache).

### macOS linker warnings

Warnings such as `ld: warning: ignoring duplicate libraries: '-lm', '-lpthread'`
(CMake) and `warning: archive library: ... the table of contents is empty`
(Bazel, for some gRPC/protobuf static libraries) are harmless and do not affect
the build.
