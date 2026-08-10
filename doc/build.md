# Pinpoint C++ Agent - Build Guide

This document describes how to build the Pinpoint C++ Agent from source. Two build systems are supported: **Bazel** and **CMake**.

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
> include submodule contents. Prefer the release asset above; if you must use
> the auto-generated archive, see Option C.

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

### Option C — GitHub auto-generated source archive

The IDL directory will be empty. CMake detects this and, since there is no
`.git`, falls back to fetching the pinned IDL commit via
`FetchContent_Declare(... GIT_REPOSITORY ...)`. This requires **network access
and `git`** at configure time. For fully offline builds use Option A.

---

## Project Structure

```
pinpoint-cpp-agent/
├── BUILD.bazel          # Bazel: main library target
├── MODULE.bazel         # Bazel: module dependencies (bzlmod)
├── CMakeLists.txt       # CMake: root build file (toolchain-agnostic)
├── CMakePresets.json    # CMake: standard, debug, coverage, profiling, sanitizer presets
├── vcpkg.json           # vcpkg manifest (used by the `vcpkg` preset)
├── include/             # Public headers
│   └── pinpoint/
│       ├── tracer.h
│       └── tracer_c.h
├── src/                 # Library source files
├── 3rd_party/           # Vendored third-party code (httplib, MurmurHash3)
│   └── pinpoint-grpc-idl/  # Protobuf/gRPC IDL (git submodule)
├── example/             # Example applications (C++, C, nginx module)
├── benchmark/           # Overhead and version-comparison benchmarks
├── test/                # Unit tests
│   ├── it/              # Integration test against an in-process mock collector
│   └── e2e/             # Integration test (HTTP + gRPC + SQL tracing)
└── scripts/             # Sanitizer suppression files
```

---

## Build with Bazel

Bazel uses [bzlmod](https://bazel.build/external/module) (`MODULE.bazel`) for dependency management. All external dependencies — and their pinned versions — are declared in `MODULE.bazel` and resolved automatically from the [Bazel Central Registry](https://registry.bazel.build/).

```bash
bazel build //...                # everything
bazel build //:pinpoint-cpp      # the library only
bazel build //example/...        # examples
bazel build //test/e2e/...       # integration test binaries
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
| `debug-cached` | Debug FetchContent build with a shared cache under `$HOME/.cache/cmake-fetchcontent` |
| `coverage` | FetchContent deps; Clang `Debug` build with source-based coverage; the build step also runs the tests and writes the reports (see [LLVM Coverage](#llvm-coverage-cmake)) |
| `profiling` | Optimized `RelWithDebInfo` build with symbols and frame pointers for xctrace/perf |
| `asan` | FetchContent deps; `Debug` build instrumented with AddressSanitizer (`-fsanitize=address`) |
| `tsan` | FetchContent deps; `Debug` build instrumented with ThreadSanitizer (`-fsanitize=thread`) |
| `ubsan` | FetchContent deps; `Debug` build instrumented with UndefinedBehaviorSanitizer (`-fsanitize=undefined`) |

Configure + build + test using a preset:

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Each preset writes to its own build directory (`build/<preset-name>/`), so you can keep several configurations side by side.

> `coverage` and `profiling` have no matching **test** preset: `coverage` runs the
> tests as part of its build step, and `profiling` is meant to be driven by a
> profiler. Every other preset above has one, so `ctest --preset <name>` works.

### Dependency Versions

Dependency versions are pinned in `vcpkg.json` and the
`FetchContent_Declare` calls in `cmake/PinpointDependencies.cmake`, and match as
closely as the registries allow. One caveat worth knowing: Protobuf and Abseil
are transitive dependencies of gRPC in every path; they are not pinned
separately.

**Bumping the vcpkg baseline:** `vcpkg.json` pins `builtin-baseline` to a specific vcpkg commit. To follow vcpkg master, update it via:

```bash
cd $VCPKG_ROOT && git rev-parse HEAD
# paste into "builtin-baseline" in vcpkg.json
```

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
| `BUILD_SHARED_LIBS` | ON | Build as a shared library (.so / .dylib) |
| `BUILD_STATIC_LIBS` | ON | Build as a static library (.a) |
| `BUILD_COVERAGE` | OFF | Enable coverage instrumentation (Clang/LLVM or GCC) |
| `BUILD_PROFILING` | OFF | Preserve symbols and reliable call stacks for sampling profilers |
| `SANITIZE` | (empty) | Enable a sanitizer: `address`, `thread`, `undefined`, or `address+undefined` |
| `SANITIZE_DEPS` | ON | With `SANITIZE` set, build gRPC/Protobuf/absl/yaml-cpp/fmt from source so the sanitizer covers them too |
| `USE_CCACHE` | ON | Use ccache as compiler launcher if found on PATH |

Example:

```bash
cmake --preset default -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF
```

For a production-like optimized build suitable for macOS `xctrace` or Linux
`perf`, use the profiling preset:

```bash
cmake --preset profiling
cmake --build --preset profiling
```

The equivalent manual option is `-DBUILD_PROFILING=ON` with a `Release` or
`RelWithDebInfo` build. It retains debug symbols and frame pointers and disables
sibling-call optimization. Coverage and profiling instrumentation cannot be
enabled together. Bazel users can build the same configuration with:

```bash
bazel build --config=profiling //test/e2e/...
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

Substitute the preset name (`vcpkg`, `debug`, `debug-cached`) to run against a different build directory.

For the current list of test targets, ask the build system rather than a document:
`ctest --preset default -N` or `bazel query //test:all`. The agent integration
test that runs against an in-process mock collector lives in `test/it/` — see
[`test/it/README.md`](../test/it/README.md).

### LLVM Coverage (CMake)

The `coverage` preset selects Clang, enables source-based coverage
instrumentation, builds all CTest binaries, runs the tests, merges every raw
profile with `llvm-profdata`, and generates text and HTML reports with
`llvm-cov`:

```bash
cmake --preset coverage
cmake --build --preset coverage
```

The second command runs the `coverage` target, so a separate `ctest` command is
not needed. Reports are recreated on every run:

| Output | Path |
|---|---|
| Text summary | `build/coverage/coverage/coverage.txt` |
| HTML report | `build/coverage/coverage/html/index.html` |
| Merged profile | `build/coverage/coverage/coverage.profdata` |

Clang, `llvm-profdata`, and `llvm-cov` must be installed. CMake first looks for
the LLVM tools beside the selected compiler and then searches `PATH`. Override
`LLVM_COV_EXECUTABLE` or `LLVM_PROFDATA_EXECUTABLE` at configure time when the
tools are installed elsewhere.

Without presets, the equivalent workflow is:

```bash
cmake -S . -B build/coverage -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_COVERAGE=ON
cmake --build build/coverage --target coverage
```

---

## Sanitizers (ASan, TSan, UBSan)

Runtime sanitizers instrument the binaries to catch bugs while the tests run.
They are especially valuable for this agent, which is concurrency-heavy
(background gRPC workers, lock-free queues and caches), crosses a C FFI boundary,
and parses untrusted input (HTTP headers, SQL). Both build systems expose the same
three sanitizers; enable one at a time (ASan and TSan are mutually exclusive).

| Sanitizer | Catches | CMake preset | Bazel config |
|---|---|---|---|
| AddressSanitizer (ASan) | use-after-free, heap/stack overflow, leaks | `asan` | `--config=asan` |
| ThreadSanitizer (TSan) | data races, lock-order inversions | `tsan` | `--config=tsan` |
| UndefinedBehaviorSanitizer (UBSan) | out-of-range casts, signed overflow, null/misaligned derefs | `ubsan` | `--config=ubsan` |

UBSan halts on the first error (`-fno-sanitize-recover`), so CI fails loudly and
points at the exact `file:line` of the violation.

### Sanitizers with CMake

```bash
cmake --preset ubsan
cmake --build --preset ubsan
ctest --preset ubsan          # the matching UBSAN_OPTIONS is applied automatically
```

Swap `ubsan` for `asan` or `tsan`. Each preset writes to its own `build/<preset>/`
directory. These are `Debug` builds with examples off that build only the shared
library, mirroring the `coverage` preset. Like `coverage` and `profiling`, they
build dependencies from source with FetchContent. To keep vcpkg packages
uninstrumented instead, pass its toolchain and disable `SANITIZE_DEPS`:

```bash
cmake --preset ubsan --toolchain "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DSANITIZE_DEPS=OFF
cmake --build --preset ubsan
ctest --preset ubsan
```

The equivalent manual configuration uses the `SANITIZE` cache variable, which
accepts `address`, `thread`, `undefined`, or `address+undefined` (ASan and UBSan
can be combined) and cannot be combined with `BUILD_COVERAGE` or `BUILD_PROFILING`:

```bash
cmake -S . -B build/ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=undefined
cmake --build build/ubsan
```

### Sanitizers with Bazel

The sanitizers are `--config` groups in `.bazelrc`, so they apply to any build or
test target; the matching `*SAN_OPTIONS` are wired in with `--test_env`:

```bash
bazel test --config=asan  //test:all
bazel test --config=tsan  //test:all
bazel test --config=ubsan //test:all
```

> Bazel applies `--copt` to every C++ target in the graph, so the first sanitized
> run rebuilds the external dependencies (gRPC, Abseil, ...) with instrumentation.
> This is intentional — it gives TSan a fully instrumented view and avoids false
> positives at library boundaries — but makes the first build slow.

### Suppressing third-party false positives

Point the relevant `*_OPTIONS` variable at a suppression file when a dependency
trips a sanitizer, e.g. `ASAN_OPTIONS=suppressions=my.supp ctest --preset asan`.

Prefer a whole-program instrumented build over suppressions. `SANITIZE_DEPS` is
ON by default, so the sanitizer presets rebuild gRPC, Protobuf, Abseil, yaml-cpp
and fmt from source with the same instrumentation, and both `ctest --preset asan`
and `bazel test --config=asan` pass the full suite. The cost is the first build:
it compiles the whole dependency graph.

Turning it off (`-DSANITIZE_DEPS=OFF`) keeps vcpkg-provided packages when the
vcpkg toolchain is active, but reintroduces a mixed-instrumentation process,
which produces false positives that look alarming and are hard to read. The
instrumented copies of inline C++ symbols (`std::string_view`,
`std::char_traits`, ...) are weak, so the
dynamic linker interposes them into the uninstrumented gRPC libraries; they then
execute on stack frames ASan has no descriptor for and it misattributes leftover
shadow poison, reporting `use-after-poison` inside gRPC internals. The reports are
recognisable by that misattribution — an access at "offset 544" inside a frame
whose objects span only 104 bytes — plus ASan's own "this may be a false positive"
hint.

`scripts/asan.supp` is applied automatically by the `asan` test preset and covers
part of that noise, but only part: ASan understands `interceptor_via_fun`,
`interceptor_via_lib` and `odr_violation`, so it can silence errors raised inside
an interceptor (`memcmp`, `strlen`, ...) and nothing else. Reports coming from a
*direct* instrumented load check are not suppressible by any suppression entry or
`ASAN_OPTIONS` setting. That is why `SANITIZE_DEPS=ON` is the default rather than
a larger suppression list.

---

## Integration Test

The integration test (`test/e2e/`) runs a multi-process HTTP + gRPC stack
against the configured collector. A deterministic smoke suite verifies
distributed context, the public C++/C APIs, lifecycle, sampling, SQL metadata,
async work and limits; the existing load/RSS suite remains available for longer
stress runs. See [`test/e2e/README.md`](../test/e2e/README.md) for the
coverage matrix and troubleshooting details.

### Build

```bash
# Bazel
bazel build //test/e2e/...

# CMake
cmake --build --preset default --target \
  grpc_server http_downstream_server it_test_server \
  c_api_scenario fork_scenario
```

### Run

```bash
# The runner starts and stops every app, assigns unique agent ids, points all
# agents at the dev collector, executes assertions, and checks transport logs.
./test/e2e/run_e2e.sh \
  --build-dir ./build/default/test/e2e

# Override only when a different collector is intentional.
PINPOINT_CPP_COLLECTOR_HOST=collector.example.com \
  ./test/e2e/run_e2e.sh \
  --build-dir ./build/default/test/e2e
```

The test script supports several modes and options:

```bash
# Run correctness checks followed by 120 seconds of mixed load
./test/e2e/run_e2e.sh \
  --build-dir ./build/default/test/e2e \
  --load-mode mixed --load-duration 120 --load-concurrency 20

# Stress test with 50 concurrent workers
./test/e2e/e2e.sh -m stress -d 300

# Test SQL tracing endpoints only
./test/e2e/e2e.sh -m db-all -d 60 -c 5

# Test gRPC endpoints only
./test/e2e/e2e.sh -m grpc-all -d 60 -c 10

# Full test (HTTP + gRPC + SQL)
./test/e2e/e2e.sh -m full -d 180 -c 15
```

---

## Troubleshooting

### Bazel: slow first build

The first Bazel build downloads and compiles all external dependencies (gRPC, protobuf, etc.), which can take several minutes. Subsequent builds use the cache and are much faster.

### CMake: the first FetchContent build is slow

The `default` preset downloads and builds all dependencies from source on the
first run. Use the `vcpkg` preset for managed packages, or `debug-cached` to
reuse a shared FetchContent cache. After the first configure, `ccache` also
speeds up subsequent rebuilds.

### Migrating from `FORCE_FETCHCONTENT=ON`

`FORCE_FETCHCONTENT` has been removed. FetchContent is now automatic whenever
the vcpkg toolchain is not active; no clean environment or search-path changes
are needed.

### macOS linker warnings

Warnings such as `ld: warning: ignoring duplicate libraries: '-lm', '-lpthread'`
(CMake) and `warning: archive library: ... the table of contents is empty`
(Bazel, for some gRPC/protobuf static libraries) are harmless and do not affect
the build.
