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
| OS | Linux, macOS |

---

## Docker build and test environment

Two images cover two different jobs. `Dockerfile` runs every preset against a
source tree baked into the image, which suits local verification of a change
across build modes. `ci/Dockerfile.vcpkg` is a source-free CI environment with the
`vcpkg` dependency closure already compiled; see
[CI image with prebuilt vcpkg dependencies](#ci-image-with-prebuilt-vcpkg-dependencies).

### All-preset test runner

`Dockerfile` contains GCC, Clang/LLVM (including the
sanitizer runtimes), CMake, Ninja, Bazel, and a pinned vcpkg checkout.
Build it once, then select the build mode with the first container argument:

```bash
docker build -t pinpoint-cpp-agent-test .
docker run --rm pinpoint-cpp-agent-test default
docker run --rm pinpoint-cpp-agent-test vcpkg
docker run --rm pinpoint-cpp-agent-test bazel
docker run --rm pinpoint-cpp-agent-test asan
docker run --rm pinpoint-cpp-agent-test tsan
docker run --rm pinpoint-cpp-agent-test ubsan
```

The remaining modes are `debug`, `coverage`, `profiling`,
`bazel-asan`, `bazel-tsan`, `bazel-ubsan`, and `bazel-profiling`. The source tree
is copied into the image; rebuild the image after changing source files.

### CI image with prebuilt vcpkg dependencies

`ci/Dockerfile.vcpkg` builds an Ubuntu 24.04 image carrying the build tools, a
vcpkg checkout pinned to `vcpkg.json`'s `builtin-baseline`, and the manifest's
entire dependency closure already compiled into a vcpkg binary cache at
`/opt/vcpkg-cache`. It contains **no agent source**, so one image serves every
commit, branch and pull request:

```bash
docker build -f ci/Dockerfile.vcpkg -t pinpoint-cpp-build-env .
```

A prebuilt copy is published, so most jobs do not need to build it at all:

```bash
docker pull pinpointdocker/pinpoint-cpp-build-env:1.0.0
```

A job then checks the source out and builds it. vcpkg restores gRPC, Protobuf,
Abseil, yaml-cpp, fmt and GoogleTest from the cache instead of compiling them,
which is the whole point of the image:

```bash
docker run --rm -v "$PWD:/workspace" \
    pinpointdocker/pinpoint-cpp-build-env:1.0.0 pinpoint-vcpkg-build
```

In GitHub Actions the image is a job container and the checkout is a step.
[.github/workflows/build.yml](../.github/workflows/build.yml) does exactly
that on every pull request and every push to `main`:

```yaml
jobs:
  vcpkg:
    runs-on: ubuntu-latest
    # pinpointdocker/pinpoint-cpp-build-env:1.0.0
    container: pinpointdocker/pinpoint-cpp-build-env@sha256:a477cd49ce...
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - run: pinpoint-vcpkg-build
```

The workflow pins the image by digest, not by tag. A tag can be moved onto new
contents at any time, so pinning to one would leave an old commit building
against whatever the tag points at today — the opposite of what the pin is
for. Read the digest of a published tag with
`docker buildx imagetools inspect pinpointdocker/pinpoint-cpp-build-env:<tag>`,
and update it in the workflow whenever `vcpkg.json` changes and the image is
republished. The `docker pull` and `docker run` lines above keep the tag: they
are for a human at a terminal, who wants the current release.

`push` is restricted to `main` there so a branch in this repository is built
once by `pull_request` rather than twice, and `concurrency` cancels a run that
a newer push has made obsolete.

Both `1.0.0` and `latest` point at the same image; prefer the version tag in a
pipeline so that republishing `latest` after a dependency bump cannot change
what an old commit builds against.

`pinpoint-vcpkg-build` ([ci/docker-vcpkg-build.sh](../ci/docker-vcpkg-build.sh))
runs `cmake --preset vcpkg`, `cmake --build --preset vcpkg` and
`ctest --preset vcpkg`, timing each phase so a slow log shows at a glance
whether the cost landed in the dependency restore (configure) or in the agent's
own code (build). `PINPOINT_BUILD_JOBS` overrides parallelism and
`PINPOINT_CTEST_ARGS` passes extra arguments to ctest. Run the three commands
directly instead when the CI system wants its own step boundaries.

Measured on a 20-core x86-64 host, from a bare container to green tests:

| Phase | Cold, no cache | With this image |
|---|---|---|
| Dependency closure (14 packages) | 11 min | **13 s** (restored) |
| Checkout | — | 1 s |
| Build (146 targets, `-j20`) | — | 33 s |
| `ctest` (24 tests) | — | 79 s |
| **Total** | | **~2 min** |

`ctest` is the largest remaining slice, and `agent_integration_test` is 56 s of
it. It talks to an in-process mock collector over real sockets, so it stays
serial; the other 23 tests finish in about 23 s together.

Building the image itself takes about 13 minutes and yields 1.95 GB (899 MB of
binary cache, 410 MB of vcpkg checkout, the rest base image and toolchain), so
push it to a registry once per dependency bump rather than rebuilding it per
job. gRPC alone is 7.6 minutes of that.

Two properties are worth knowing before wiring this into a pipeline:

- **Only the `vcpkg` preset benefits.** `asan`, `tsan`, `ubsan` and `coverage`
  set `PINPOINT_FORCE_FETCHCONTENT`, because a dependency must carry the same
  instrumentation as the agent and a prebuilt copy cannot; `default` never
  consults vcpkg at all. Those presets compile dependencies from source no
  matter which image they run in — use the all-preset runner above for them.
- **The image tracks `vcpkg.json`.** The vcpkg commit is read out of the
  manifest's `builtin-baseline` during the image build, so the two cannot drift,
  but bumping a dependency version or the baseline does require rebuilding the
  image. Until it is rebuilt the build still succeeds: vcpkg simply compiles
  whatever the cache does not cover, at full cost.

Two things in the image exist purely to survive how CI runners invoke it.
`git config --system --add safe.directory '*'`, because a checkout is owned by
the runner's uid rather than the container's, and without it Git refuses to read
the repository — failing the `git submodule update --init` that CMake runs on
configure against an otherwise valid checkout. And `buildtrees/`, `packages/`
and `downloads/` under `$VCPKG_ROOT` are left present and world-writable,
because vcpkg takes an exclusive lock at `buildtrees/vcpkg-running.lock` on
every invocation, restores included; a container running as a non-root uid
cannot create that path under root-owned `/opt` otherwise, and fails at
configure time.

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

## Project Structure

```
pinpoint-cpp-agent/
├── BUILD.bazel          # Bazel: main library target
├── MODULE.bazel         # Bazel: module dependencies (bzlmod)
├── CMakeLists.txt       # CMake: root build file (toolchain-agnostic)
├── CMakePresets.json    # CMake: standard, debug, coverage, profiling, sanitizer presets
├── vcpkg.json           # vcpkg manifest (used by the `vcpkg` preset)
├── ci/                  # CI build image (Dockerfile.vcpkg) and its build script
├── include/             # Public headers
│   └── pinpoint/
│       ├── tracer.h
│       └── tracer_c.h
├── src/                 # Library source files
├── 3rd_party/           # Vendored third-party code (httplib, MurmurHash3)
│   └── pinpoint-grpc-idl/  # Protobuf/gRPC IDL (git submodule)
├── example/             # Example applications (C++, C)
├── benchmark/           # Microbenchmarks (span queue, caches, active spans)
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
bazel build -c opt //benchmark/...   # microbenchmarks
```

Benchmarks are always compiled `-O3`, so they are meaningful at any compilation
mode; `-c opt` additionally optimizes the library, which the two targets that
link it measure through. See [benchmark/README.md](../benchmark/README.md).
Unlike CMake, where benchmarks sit behind `BUILD_BENCHMARKS=ON` (off by
default), `bazel build //...` compiles them — so a change that breaks one is
caught by the `bazel` Docker mode.

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
| `coverage` | FetchContent deps; Clang `Debug` build with source-based coverage; the build step also runs the tests and writes the reports (see [LLVM Coverage](#llvm-coverage-cmake)) |
| `profiling` | Optimized `RelWithDebInfo` build with symbols and frame pointers for xctrace/perf |
| `asan` | FetchContent deps; `Debug` build instrumented with AddressSanitizer (`-fsanitize=address`) |
| `tsan` | FetchContent deps; `Debug` build instrumented with ThreadSanitizer (`-fsanitize=thread`) |
| `ubsan` | FetchContent deps; Clang `Debug` build instrumented with UndefinedBehaviorSanitizer (`-fsanitize=undefined`) |

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

### Sharing the FetchContent cache

By default every FetchContent preset downloads and builds its dependencies into
its own `build/<preset>/_deps/`, so switching presets re-fetches everything.
Point `FETCHCONTENT_BASE_DIR` at a directory outside the build tree to share one
download/build cache across presets:

```bash
cmake --preset debug -D FETCHCONTENT_BASE_DIR="$HOME/.cache/cmake-fetchcontent"
cmake --build --preset debug
```

`FETCHCONTENT_BASE_DIR` is a CMake **cache** variable, not an environment
variable: it must be passed with `-D` on the configure command (exporting it in
the shell has no effect). It is stored in the build directory's cache, so later
`cmake --preset`/`--build` runs on the same directory keep using it.

Use an absolute path: a relative one is resolved against the directory `cmake`
was invoked from, so the same argument run from elsewhere points at a different
cache. To share the cache in an editor, set it as a configure
setting rather than an environment variable; the checked-in
[`.vscode/settings.json`](../.vscode/settings.json) does this via
`cmake.configureSettings`.

> The directory holds full dependency *build* trees (`<dep>-build/`), not just
> downloaded sources (`<dep>-src/`). Two presets that compile with different
> flags — `Debug` vs `Release`, sanitized vs not — write the dependency object
> files to the same paths, so each switch between them rebuilds those
> dependencies. Sharing pays off across presets with matching flags; give the
> sanitizer and `Release` presets their own directory (for example
> `…/cmake-fetchcontent-asan`) when you alternate.

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
| `SANITIZE` | (empty) | Enable a sanitizer: `address`, `thread`, or `undefined`. Dependencies are rebuilt from source with the same instrumentation |

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
`RelWithDebInfo` build: debug symbols and frame pointers are retained and
sibling-call optimization is disabled. It cannot be combined with
`BUILD_COVERAGE`. The Bazel equivalent is:

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

Substitute the preset name (`vcpkg`, `debug`) to run against a different build directory.

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
Both build systems expose the same three; enable one at a time (ASan and TSan
are mutually exclusive).

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
directory, builds dependencies from source with FetchContent, and is a `Debug`
build with examples off. Notable details:

- ASan uses static linkage to prevent ODR reports on gRPC's generated UPB
  globals; TSan and UBSan retain shared linkage.
- Links are pinned to a two-job Ninja pool (`CMAKE_JOB_POOLS`): each instrumented
  test executable links the whole static gRPC/Abseil/BoringSSL closure, and a
  machine-wide `-j` at the link phase peaks past 17 GiB and gets `ld` OOM-killed.
- The `ubsan` preset selects Clang because GCC 13 cannot compile the vendored
  Abseil hash policy constexpr expressions under UB instrumentation. Install
  `libclang-rt-dev` when using Clang outside the provided image.
- On ARM64 Linux, sanitizer links add `--rtlib=compiler-rt --unwindlib=libgcc`,
  because ARM64 `libgcc` lacks some builtins UBSan emits. The two flags travel
  together: selecting compiler-rt leaves Clang with no unwinder by default, and
  the sanitizer runtimes need one — see
  [Sanitizers: a sanitized link cannot find `_Unwind_GetIP`](#sanitizers-a-sanitized-link-cannot-find-_unwind_getip).
- The instrumented integration test uses a fifteen-minute timeout.

The equivalent manual configuration uses the `SANITIZE` cache variable, which
accepts `address`, `thread`, or `undefined` and cannot be combined with
`BUILD_COVERAGE` or `BUILD_PROFILING`:

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
> run rebuilds the external dependencies (gRPC, Abseil, ...) with instrumentation
> — intentional, since it avoids false positives at library boundaries, but slow.
> Bazel ASan uses static linking to avoid duplicate generated UPB globals; Bazel
> UBSan selects Clang and compiler-rt for the same constraints as the CMake
> preset. Sanitizer tests use a fifteen-minute timeout; the TSan suppression file
> is included in the test runfiles.

### Suppressing third-party false positives

Point the relevant `*_OPTIONS` variable at a suppression file when a dependency
trips a sanitizer, e.g. `ASAN_OPTIONS=suppressions=my.supp ctest --preset asan`.

Suppressions are rarely needed because sanitized builds are whole-program
instrumented: gRPC, Protobuf, Abseil, yaml-cpp and fmt are all rebuilt with the
same instrumentation, and the full suite passes under both build systems. Mixing
instrumented and prebuilt libraries is not supported — the weak instrumented
copies of inline C++ symbols get interposed into the uninstrumented libraries,
producing unsuppressible `use-after-poison` false positives inside gRPC.

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

`run_e2e.sh` also takes `--load-mode`, `--load-duration`, `--load-concurrency`
and `--load-rps` to append a load phase after the correctness checks.
`load_test.py` drives load alone, selecting the endpoint set with `-m`
(`stress`, `db-all`, `grpc-all`, `full`), the duration with `-d` and the
concurrency with `-c`:

```bash
python3 ./test/e2e/load_test.py -m full -d 180 -c 15
```

---

## Troubleshooting

### Bazel: slow first build

The first Bazel build downloads and compiles all external dependencies (gRPC, protobuf, etc.), which can take several minutes. Subsequent builds use the cache and are much faster.

### CMake: the first FetchContent build is slow

The `default` preset downloads and builds all dependencies from source on the
first run. Use the `vcpkg` preset for managed packages, or set
`FETCHCONTENT_BASE_DIR` to reuse one dependency cache across presets and build
directories — see [Sharing the FetchContent cache](#sharing-the-fetchcontent-cache).

### macOS linker warnings

Warnings such as `ld: warning: ignoring duplicate libraries: '-lm', '-lpthread'`
(CMake) and `warning: archive library: ... the table of contents is empty`
(Bazel, for some gRPC/protobuf static libraries) are harmless and do not affect
the build.

### Sanitizers: ThreadSanitizer aborts with "unexpected memory mapping"

A sanitized build also *runs* part of what it just built — the instrumented
`protoc` and `grpc_cpp_plugin` generate the agent's sources — so this surfaces as
a failed codegen step rather than a failing test:

```
[2165/3342] Running cpp protocol buffer compiler on .../v1/Annotation.proto
FAILED: [code=66] v1/Annotation.pb.h v1/Annotation.pb.cc
FATAL: ThreadSanitizer: unexpected memory mapping 0x6530eab0b000-0x6530eab0f000
```

Where the dependencies are already built, the same message aborts every `tsan`
test binary instead, before `main()` runs.

The cause is the host's ASLR entropy, not the code or the toolchain. Kernels
configured with `vm.mmap_rnd_bits = 32` place mappings outside the address ranges
the ThreadSanitizer runtime expects, and it refuses to start; a hello-world built
with `-fsanitize=thread` reproduces it in one line. Containers inherit the setting
from the host kernel, so `docker run ... tsan` fails the same way. ASan and UBSan
are unaffected.

Either lower the entropy globally, which needs root —

```bash
sudo sysctl -w vm.mmap_rnd_bits=28
```

— or run the build and the tests with ASLR disabled, which needs no privileges.
The personality is inherited by child processes, so wrapping the top-level
command also covers the `protoc` invocations and the test executables:

```bash
setarch $(uname -m) -R cmake --build --preset tsan --parallel "$(nproc)"
```

```bash
setarch $(uname -m) -R ctest --preset tsan
```

Wrap `bazel test --config=tsan` the same way, after a `bazel shutdown`: the test
actions are spawned by the Bazel server, so a server already running with ASLR on
keeps failing until it is restarted under `setarch`.

### Sanitizers: a sanitized link cannot find `_Unwind_GetIP`

A sanitized Clang build fails at the first executable it links — usually the
fetched `protoc`, long before any agent target:

```
libclang_rt.ubsan_standalone.a(sanitizer_unwind_linux_libcdep.cpp.o): undefined reference to symbol '_Unwind_GetIP@@GCC_3.0'
/lib/x86_64-linux-gnu/libgcc_s.so.1: error adding symbols: DSO missing from command line
```

Through `lld`, which the Bazel configs pick up, the same failure reads
`ld.lld: error: undefined symbol: _Unwind_GetIP`.

The trigger is a sanitized link that passes `--rtlib=compiler-rt` without naming
an unwinder. Clang's default `--unwindlib=platform` resolves to *no* unwinder on
Linux once the runtime library is compiler-rt, yet the sanitizer runtimes call
`_Unwind_*` to walk the stack for their reports. `libgcc_s` still enters the link
as an indirect dependency of `libstdc++`, which is exactly what `ld` refuses to
resolve against. Distribution Clang packages may patch that default, so the same
tree can link with the distribution compiler and fail with an upstream LLVM
release.

The build already pairs the two flags wherever it asks for compiler-rt — see
[Sanitizers with CMake](#sanitizers-with-cmake) — so this should not appear on a
clean checkout. If it does, because of a toolchain of your own or extra sanitizer
flags, name the unwinder next to `--rtlib`:

```bash
cmake --preset ubsan -DCMAKE_EXE_LINKER_FLAGS=--unwindlib=libgcc
```

```bash
bazel test --config=ubsan --linkopt=--unwindlib=libgcc //test:all
```
