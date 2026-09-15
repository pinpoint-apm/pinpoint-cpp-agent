# Pinpoint C++ Agent - Development Guide

This guide is for people working **on** the agent: the repository layout, the
Docker and CI environments, how to run the test suites, coverage, sanitizers,
benchmarks, and the design decisions the code rests on.

If you only want to *use* the agent in your application, you do not need this
document. Start with the [Quick Start Guide](quick_start.md), and see the
[Build Guide](build.md) for building and linking the library.

For the pull-request and CLA process, see [CONTRIBUTING.md](../CONTRIBUTING.md).

> **Where documentation goes.** Everything else under `doc/` is written for
> application developers using the agent. Internal rationale — why an
> implementation is shaped the way it is, what was measured, what a sibling
> agent does in its own source — belongs here instead, so the user-facing
> guides stay about observable behaviour.

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
├── 3rd_party/           # Vendored third-party code (MurmurHash3)
│   └── pinpoint-grpc-idl/  # Protobuf/gRPC IDL (git submodule)
├── example/             # Example applications (C++; C examples live in pinpoint-cpp-examples)
├── benchmark/           # Microbenchmarks (span queue, caches, active spans)
├── test/                # Unit tests
│   ├── it/              # Integration test against an in-process mock collector
│   └── e2e/             # Integration test (HTTP + gRPC + SQL tracing)
└── scripts/             # Sanitizer suppression files
```

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

> The `tsan` and `bazel-tsan` modes need one host-side prerequisite. ASLR entropy
> is a host kernel setting that containers inherit and cannot override, so on an
> affected host these two modes fail inside the container exactly as they do
> outside it — see
> [Sanitizers: ThreadSanitizer aborts with "unexpected memory mapping"](#sanitizers-threadsanitizer-aborts-with-unexpected-memory-mapping).

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
| `ctest` (25 tests) | — | 79 s |
| **Total** | | **~2 min** |

`ctest` is the largest remaining slice, and `agent_integration_test` is 56 s of
it. It talks to an in-process mock collector over real sockets, so it stays
serial; the other 24 tests finish in about 23 s together. Ask the build system
for the current list rather than trusting this count:
`ctest --preset default -N`.

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

## Development Presets

Beyond the `default`, `vcpkg` and `debug` presets described in the
[Build Guide](build.md#cmake-presets), `CMakePresets.json` carries four presets
that exist for working on the agent:

| Preset | Toolchain / source of dependencies |
|---|---|
| `coverage` | FetchContent deps; Clang `Debug` build with source-based coverage; the build step also runs the tests and writes the reports (see [LLVM Coverage](#llvm-coverage-cmake)) |
| `profiling` | Optimized `RelWithDebInfo` build with symbols and frame pointers for xctrace/perf |
| `asan` | FetchContent deps; `Debug` build instrumented with AddressSanitizer (`-fsanitize=address`) |
| `tsan` | FetchContent deps; `Debug` build instrumented with ThreadSanitizer (`-fsanitize=thread`) |
| `ubsan` | FetchContent deps; Clang `Debug` build instrumented with UndefinedBehaviorSanitizer (`-fsanitize=undefined`) |

> `coverage` and `profiling` have no matching **test** preset: `coverage` runs the
> tests as part of its build step, and `profiling` is meant to be driven by a
> profiler. Every other preset has one, so `ctest --preset <name>` works.

For a production-like optimized build suitable for macOS `xctrace` or Linux
`perf`:

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

### Bumping the vcpkg baseline

Dependency versions are pinned in `vcpkg.json` and the `FetchContent_Declare`
calls in `cmake/PinpointDependencies.cmake`, and match as closely as the
registries allow. One caveat worth knowing: Protobuf and Abseil are transitive
dependencies of gRPC in every path; they are not pinned separately.

`vcpkg.json` pins `builtin-baseline` to a specific vcpkg commit. To follow
vcpkg master, update it via:

```bash
cd $VCPKG_ROOT && git rev-parse HEAD
# paste into "builtin-baseline" in vcpkg.json
```

Rebuild and republish the CI image after either change — see
[CI image with prebuilt vcpkg dependencies](#ci-image-with-prebuilt-vcpkg-dependencies).

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

### Live integration test

The integration test (`test/e2e/`) runs a multi-process HTTP + gRPC stack
against the configured collector. A deterministic smoke suite verifies
distributed context, the public C++/C APIs, lifecycle, sampling, SQL metadata,
async work and limits; the existing load/RSS suite remains available for longer
stress runs. See [`test/e2e/README.md`](../test/e2e/README.md) for the
coverage matrix and troubleshooting details.

#### Build

```bash
# Bazel
bazel build //test/e2e/...

# CMake
cmake --build --preset default --target \
  grpc_server http_downstream_server it_test_server \
  c_api_scenario fork_scenario
```

#### Run

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

### Benchmarks

Microbenchmarks live in `benchmark/` and are always compiled `-O3`, so they are
meaningful at any surrounding compilation mode. Under CMake they sit behind
`BUILD_BENCHMARKS=ON` (off by default); `bazel build //...` compiles them
unconditionally, so a change that breaks one is caught by the `bazel` Docker
mode.

```bash
cmake --preset default -DBUILD_BENCHMARKS=ON
bazel build -c opt //benchmark/...
```

`-c opt` additionally optimizes the library, which the two targets that link it
measure through. See [benchmark/README.md](../benchmark/README.md).

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

> **On Linux, TSan may need a `setarch` wrapper.** On a host configured with
> `vm.mmap_rnd_bits = 32`, a `-fsanitize=thread` binary usually aborts with
> `FATAL: ThreadSanitizer: unexpected memory mapping` — a hello-world reproduces
> it in one line. *Usually*, not always: a run that happens to get an acceptable
> layout passes, so one green run without the wrapper does not mean the host
> does not need it. A sanitized build also runs what it just built — the
> instrumented `protoc` generates the agent's sources — so this shows up as a
> failed **build** step, not a failed test. Disabling ASLR for the command fixes
> it and needs no privileges; the personality is inherited by child processes, so
> wrapping the top-level command covers the `protoc` runs and the test binaries:
>
> ```bash
> setarch $(uname -m) -R cmake --build --preset tsan --parallel "$(nproc)"
> setarch $(uname -m) -R ctest --preset tsan
> ```
>
> ASan and UBSan are unaffected. For the root-privileged alternative, the Bazel
> equivalent, and the full explanation, see
> [Sanitizers: ThreadSanitizer aborts with "unexpected memory mapping"](#sanitizers-threadsanitizer-aborts-with-unexpected-memory-mapping).

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
- The `tsan` preset's build *and* `ctest` commands both need the `setarch`
  wrapper described above on an affected Linux host. Wrapping only one of the two
  still fails: the build runs the instrumented `protoc`, and the tests are
  instrumented binaries themselves.

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
> is included in the test runfiles. On a host that needs the `setarch` wrapper
> above, run `bazel shutdown` first and wrap the whole `bazel test` command: the
> test actions are spawned by the Bazel server, so a server already running with
> ASLR on keeps failing until it is restarted under `setarch`.

### Suppressing third-party false positives

Point the relevant `*_OPTIONS` variable at a suppression file when a dependency
trips a sanitizer, e.g. `ASAN_OPTIONS=suppressions=my.supp ctest --preset asan`.

Suppressions are rarely needed because sanitized builds are whole-program
instrumented: gRPC, Protobuf, Abseil, yaml-cpp and fmt are all rebuilt with the
same instrumentation, and the full suite passes under both build systems. Mixing
instrumented and prebuilt libraries is not supported — the weak instrumented
copies of inline C++ symbols get interposed into the uninstrumented libraries,
producing unsuppressible `use-after-poison` false positives inside gRPC.

### Sanitizers: ThreadSanitizer aborts with "unexpected memory mapping"

A sanitized build also *runs* part of what it just built — the instrumented
`protoc` and `grpc_cpp_plugin` generate the agent's sources — so this surfaces as
a failed codegen step rather than a failing test:

```
[2165/3342] Running cpp protocol buffer compiler on .../v1/Annotation.proto
FAILED: [code=66] v1/Annotation.pb.h v1/Annotation.pb.cc
FATAL: ThreadSanitizer: unexpected memory mapping 0x6530eab0b000-0x6530eab0f000
```

Where the dependencies are already built, the same message aborts the `tsan` test
binaries instead, before `main()` runs.

The cause is the host's ASLR entropy, not the code or the toolchain. Kernels
configured with `vm.mmap_rnd_bits = 32` place mappings outside the address ranges
the ThreadSanitizer runtime expects, and it refuses to start; a hello-world built
with `-fsanitize=thread` reproduces it in one line. Containers inherit the setting
from the host kernel, so `docker run ... tsan` fails the same way. ASan and UBSan
are unaffected.

The abort is probabilistic, not deterministic. The layout is redrawn for every
process, so a run occasionally lands inside the range the runtime accepts and
passes: one Bazel test measured 1 pass in 12 without the wrapper, and 12 in 12
with it. That makes this worse than a hard failure — a single green run without
`setarch` is luck rather than evidence, and a one-shot check can talk you out of
a workaround the host does need.

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

---

## Measured Complexity Decisions

Seven mechanisms an over-engineering audit flagged as *possibly* removable, each
adjudicated — six by benchmark A/B, one by correctness review. **All seven stay.**
The numbers are recorded here so a future change to the workload can revisit them
against a baseline.

Environment: Apple silicon (arm64, Darwin 24), `-O3`, median of repeated runs.
Absolute numbers are machine-specific; the *ratios* are the finding. Reproduction
recipes are at the end.

| Mechanism | Simplification considered | Verdict | Decisive number |
|---|---|---|---|
| `AtomicSharedPtr` `SnapshotCache::ThreadCached` | Delete; keep only `Uncached` | **Keep** | 62x slower loads at 14 threads |
| `ShardedBoundedQueue` quota borrowing | Fixed per-shard quota | **Keep** | 97% retention loss with 1 producer |
| `cache.h` hash-carrying key twins | Hash again per lookup | **Keep** | +58% on the raw-SQL hit (190 B), worse as SQL grows |
| LRU aged promotion | Promote every full-cache hit | **Keep** | 17.7x slower hits at 16 threads |
| `HttpUrlFilter` Exact/Prefix/SegmentPrefix fast paths | Always run the Ant DP | **Keep** | 3.6x slower per request |
| Deferred-destroy shutdown half | Rely on the keep-alive path alone | **Keep** | Cannot work: `weak_from_this()` is expired in the deleter |
| `OwnedHandleRegistry` 32-way sharding | One `shared_mutex` over one map | **Keep** | 38x slower per C-API span at 8 threads |

### 1. `AtomicSharedPtr` ThreadCached mode (`src/atomic_shared_ptr.h`)

Every span admission loads the `runtime_` snapshot. `Uncached` (a plain atomic
`shared_ptr` load) bumps one shared control-block refcount per load, so it
degrades as threads are added; `ThreadCached` stays flat.

| threads | Uncached load ns/op | ThreadCached load ns/op |
|---|---|---|
| 1 | 12.7 | 4.5 |
| 4 | 200.5 | 4.2 |
| 8 | 340.6 | 4.2 |
| 14 | 428.1 | 6.9 |

`atomic_shared_ptr_benchmark` carries an `uncached load` column permanently, so
this comparison re-runs with the benchmark.

### 2. `ShardedBoundedQueue` quota borrowing (`src/sharded_bounded_queue.h`)

Not a performance feature — raw throughput is mixed, and a fixed per-shard quota
is even *faster* in some drain scenarios (32 producers: 606 vs 364 Mops/s)
because the borrow branch disappears. The point is the retention contract:
`QueueSize` is the *global* logical bound while producers stick to their thread's
home shard. With a paused consumer (collector stall):

| producers | retained (borrowing) | retained (fixed quota) |
|---|---|---|
| 1 | 1,024 (= QueueSize) | 32 (= QueueSize / shards) |
| 4 | 1,024 | 128 |

A server with fewer busy threads than shards — the common case — would lose up
to 97% of its buffered spans during any collector stall.

### 3. Hash-carrying cache key twins (`src/cache.h`)

The `*CacheKey`/`StoredKey`/`KeyTraits` structs carry the key's hash so the
shard's map never re-hashes it. Hashing again inside the map costs one extra
`std::hash` over the raw key per lookup, against a raw-SQL-cache hit of **25 ns**
(single-threaded):

| key size | extra hash cost | overhead on a 25 ns hit |
|---|---|---|
| 190 B (benchmark query) | 14.6 ns | +58% |
| 1 KB | 41.3 ns | +164% |
| 8 KB | 301 ns | 12x |
| 64 KB (raw SQL cache limit) | 2.4 µs | 96x |

The raw SQL cache exists to make hits ~76x cheaper than re-normalizing; giving a
growing fraction of that back per lookup defeats it exactly where SQL is large.

### 4. LRU aged promotion (`src/cache.h`)

Only relevant when a cache is full (e.g. more than 1,024 distinct
non-parameterized SQL statements — the classic raw-SQL blowup). Aged promotion
keeps a stable hot set on the pure shared-lock path; promoting under the
exclusive lock on every full-cache hit serializes all hits on the splice.
Hot-set hits, cache full, production shard count:

| threads | aged (current) ns/op | promote-every-hit ns/op |
|---|---|---|
| 1 | 31.4 | 35.7 |
| 4 | 32.2 | 158.7 |
| 8 | 67.1 | 644.4 |
| 16 | 75.2 | 1,332.7 |

### 5. `HttpUrlFilter` literal fast paths (`src/http.cpp`)

The Ant token DP matches everything the Exact/Prefix/SegmentPrefix
specializations match (A/B produced identical match results), so they are pure
speed — but the filter runs once per request. With a representative 5-pattern
exclude list over a 6-URL mix: **722 ns/call with fast paths, 2,585 ns/call
all-Ant (3.6x, +1.9 µs per request)**, scaling with pattern-list length.

### 6. Deferred-destroy shutdown half (`src/agent.cpp`, `SharedDeleter`)

The `weak_from_this().lock()` keep-alive cannot cover the bounded-shutdown
contract alone: by the time `SharedDeleter` runs the strong count is zero and
`weak_from_this().lock()` is guaranteed to return nullptr. The
final-release-without-`Shutdown()` case — promised bounded in `tracer.h` — is
covered *only* by the `delete_when_done` ownership transfer. Without it,
`agent.reset()` (or `pt_agent_destroy` without a prior shutdown) blocks
unboundedly on a wedged worker and
`AgentShutdownDeadlineTest.ReleaseWithoutShutdownDefersDestructionWhenWedged`
fails immediately.

A correctness review of the handoff races, the no-member-access-after-handover
discipline, double-shutdown, the three non-`createShared` fallbacks and fork
inheritance found no defect. A leak-forever alternative (~20 more lines saved)
was rejected: stragglers that finish late would permanently leak caches that can
reach tens of MB.

### 7. `OwnedHandleRegistry` sharding (`src/tracer_c.cpp`)

The C API keeps every live `pt_agent_t`/`pt_span_t` in a registry behind a
`shared_mutex`. Lookups take a shared lock, so a single mutex looks sufficient —
concurrent readers never block each other. But the registry's real traffic is
not read-only: a span handle is **inserted once and erased once** around its
5-15 lookups, and those two take the *exclusive* lock, which blocks every
reader on that mutex. One cycle below is that whole unit (insert + 10 finds +
erase), i.e. one traced request's worth of registry traffic.

| threads | shards=1 ns/cycle | shards=4 | shards=8 | shards=32 (current) |
|---|---|---|---|---|
| 1 | 176 | 175 | 177 | 177 |
| 4 | 1,509 | 718 | 122 | 128 |
| 8 | 9,084 | 4,071 | 2,369 | 237 |
| 14 | 31,407 | 10,123 | 5,165 | 1,185 |

Un-sharded costs **38x at 8 threads** and 27x at 14. The count is not
over-provisioned either: 8 shards is still 10x worse than 32 at 8 threads,
because an exclusive lock on a colliding shard stalls that shard's readers too,
and collisions stay common until the shard count is well above the thread count.

The sharding is not free, and the benchmark keeps the cost visible in its
second table. With a **pure-lookup** workload — the population pre-inserted, no
insert/erase traffic at all — sharding is *slower*, 111 vs 45 ns/find at 14
threads (2.5x), because a thread's handles scatter across many shard cache
lines instead of reusing one resident map. That workload does not exist in
production (a handle that is never inserted or erased cannot be looked up), but
if the C API ever grows long-lived handles that are looked up far more than they
are created, this is the number that would change the decision.

One caveat the numbers do not show: the **agent** registry normally holds a
single handle, so every lookup lands on one shard whatever the shard count.
Sharding neither helps nor hurts there; the span registry is what it buys.

### Reproduction

Build with `-DBUILD_BENCHMARKS=ON`; binaries land in `<build>/benchmark/`.

- **1**: `atomic_shared_ptr_benchmark` (the `uncached load` column).
- **2**: `span_queue_benchmark --mode all --producers 1,4,32`; for the fixed-
  quota variant change `borrowable_shards_(initial_bitmap(shard_count_))` to
  `borrowable_shards_(0)` in `sharded_bounded_queue.h` and compare the
  `drop-old` column (paused rows) and Mops/s.
- **3**: `raw_sql_cache_benchmark` for the hit cost; time
  `std::hash<std::string>` at the key sizes above for the extra-hash cost.
- **4**: full-cache hot-set micro-bench over `ApiIdCache` (warm past capacity,
  then hit 64 hot keys from N threads); for the variant change
  `if (age < promote_age_threshold_)` to `if (false)` in `cache.h`.
- **5**: micro-bench `HttpUrlFilter::isFiltered` over the pattern/URL mix
  above; for the variant disable the three early-return classifications in
  `HttpUrlFilter::compilePattern` so everything compiles to `PatternKind::Ant`.
- **7**: `owned_handle_registry_benchmark`. It carries the shard-count sweep
  permanently (the registry is copied into it verbatim, with `kShardCount`
  lifted to a template parameter, so `shards=1` is the un-sharded candidate
  running otherwise identical code), so this comparison re-runs with the
  benchmark.

---

## Java Agent Parity Notes

The user-facing guides state what this agent does. This section records **where
the matching behaviour is pinned in the Java agent's source**, so a contributor
changing one of these paths can check parity without re-deriving it. Line
numbers are indicative — verify against the Java agent revision you are
comparing with.

### Sampling

| Behaviour | Java source |
|---|---|
| `PercentRate` truncation — `(long) (rate * 100)`; a non-positive rate goes to `FalseSampler` | `PercentSamplerFactory.java:40-48,56-58` (`0` → `FalseSampler`: `:40-42`) |
| `PercentRate` phase — admits on a remainder in `(0, rate]` | `PercentRateSampler`; `PercentRate: 100` is Java's separate `TrueSampler` |
| Counter phase — tested before increment, so transaction 1 is always sampled | `CountingSampler` (`counter.getAndIncrement()`) |

`PercentRate` is stored internally as hundredths of a percent and the conversion
truncates, matching Java and the Go agent's `uint64(percent * 100)`. Truncation
is an artifact of IEEE-754, not a policy: `0.29 * 100` is `28.999999999999996`,
so the cast drops a hundredth. This agent rounded to nearest for a while, on the
argument that it hands the operator back the rate they typed. That was reverted
— one configuration file deployed across a polyglot fleet should produce one
sampling rate, and a hundredth of a percentage point is not worth being the one
agent that disagrees. Releases up to and including v2.0.0 also raised a
configured `0` to `0.01`; see the [changelog](../CHANGELOG.md).

### Spans, events and errors

| Behaviour | Java source |
|---|---|
| Event-depth off-by-one: the push landing at `maxDepth + 1` is still admitted | `DefaultCallStack.isOverflow()` compares `maxDepth < index`; `CallStackTest.overflow()` pins it — the fourth `push()` at `maxDepth = 3` returns `4` |
| An overflowed span event still fails the transaction | `DefaultTrace.traceBlockBegin0` hands a `WrappedSpanEventRecorder` over a `DisableSpanEvent`; `recordException` reaches `AbstractRecorder` → `SimpleErrorRecorder` |
| An unsampled **span**'s error still fails the transaction | `DisableSpanRecorder.recordException` |
| Every `recordException` is OR-ed into the shared error code | `TraceRoot.getShared().maskErrorCode()` |
| The `PSpan` is serialized once, at root end | `DefaultTrace.close()` → `logSpan()` stores it (`DefaultTrace.java:181-199`), and that is the trace every normal entry point builds (`DefaultBaseTraceFactory.java:191`) |
| Error message abbreviation at 256 bytes | `StringUtils.abbreviate(message, 256)` |
| Inbound-trace continuation: four ordered checks | `DefaultTraceHeaderReader.java:54-70` |
| Conditional injected-header set | `DefaultRequestTraceWriter` |

Two places where this agent **deliberately differs**, and why:

- **An unsampled span *event*'s error fails the transaction.** Java's
  `DisableSpanEventRecorder.recordException` is a no-op. With sampling on,
  unsampled requests are the majority, so ignoring their step errors would bias
  the URL stat failure rate toward zero.
- **Deferring the `PSpan` store until the last async child ends** exists only on
  Java's `AsyncDefaultTrace` path (`AsyncDefaultTrace.java:24-31` awaits;
  `SpanAsyncStateListener.java:59` stores), whose entry points are the vert.x-only
  ones marked `@InterfaceAudience.LimitedPrivate("vert.x")`
  (`DefaultBaseTraceFactory.java:148,161`). An async child that ends after the
  root sent its final chunk therefore cannot carry its error onto the wire here —
  the same design as Java's ordinary trace, not a gap against it.
- **The disabled span event is one shared object per span**, where
  `SpanEventFactory.disableInstance()` hands back a new `DisableSpanEvent` per
  call. Only `Pinpoint-Host` on calls made past the depth limit is affected.
- **An unbalanced `EndSpan()` keeps the span**, auto-closing open events at the
  end timestamp. `DefaultTrace.close()` dumps the call stack and discards the
  span entirely.

### SQL, URL statistics and configuration

| Behaviour | Java source / key |
|---|---|
| SQL cache size, length limit, UID expiry | `profiler.jdbc.sqlcachesize`, `profiler.jdbc.sqlcachelengthlimit`, `profiler.jdbc.sqlcacheexpirehours` |
| Transmitted SQL abbreviated at 65536 bytes | `profiler.jdbc.maxsqllength`, applied by `SqlCacheService` |
| SQL UID hash — MurmurHash3-128, little-endian | `UidGenerator.Murmur` |
| Comment stripping on by default | `DefaultJdbcOption.removeComments` initializes to `true`, and `profiler.jdbc.removecomments` is absent from the shipped `pinpoint.config`, so the initializer stands |
| `Sql.ErrorCount` merges two Java keys | `SqlCountServiceProvider.java:21-27` picks `DisableSqlCountService` or `DefaultSqlCountService` on `profiler.sql.error.enable`; `DefaultSqlCountService.java:15-25` compares with `>=` |
| Bind-value join and truncation marker | `BindValueUtils.bindValueToString` |
| URL stat "no URI recorded" bucket — `/NULL` | `URITemplate.NULL_URI` (the Go agent uses its own `UNKNOWN_URL`) |
| Completed-tick-only sending, on the stat scheduler | `UriStatCollectingJob` |
| Ignore-error handler leaf matchers | `profiler.ignore-error-handler.<id>.class-name\|exception-message@contains` (Java's `nested` / `parent` matchers are not implemented) |
| Error-cause bitmask values | `common/trace/ErrorCategory` |
| Error mark allow/deny lists | `profiler.error.mark`, `profiler.error.mark.exclude` |
| Real-IP header resolution | `profiler.server.realipheader`, `profiler.server.realipemptyvalue` (`RealIpHeaderResolver`) |
| Request-parameter recording format | `profiler.server.tracerequestparam` (`HttpServletParameterExtractor`) |
| Proxy header keys | `profiler.proxy.http.header.enable`, `profiler.proxy.http.headers` (`UserRequestParser`) |
| Client URL query recording | per-plugin `profiler.<plugin>.param` (`ClientRequestRecorder`, `InterceptorUtils.getHttpUrl`) |
| Identity version | `pinpoint.modules.uid.version` |
| Profiles | `profiles/{release,local}/pinpoint.config` selected by `pinpoint.profiler.profiles.active` |
| Exception-chain throughput limiter, sticky verdict | `profiler.exceptiontrace.new.throughput`, Java's sticky `DISABLED` sampling state |
| Stat: uncollected JVM values travel as `-1` | `MemoryMetric.UNCOLLECTED_VALUE` |
| Stat: `collectInterval` is the measured period | `CollectJob` |
| Channel / stream renewal | `profiler.transport.grpc.loadbalancer.renew.period.millis`, `profiler.transport.grpc.span.sender.rpc.age.max.millis`, `ClientOption.idleTimeoutMillis` |

Deliberate divergences here:

- **Oversize SQL is dropped whole, not cut.** A statement over the 1 MiB
  normalizer cap gets no SQL annotation rather than a truncated one: a cut
  landing inside a literal would give the statement an id/UID no other agent
  computes for it.
- **`Http.UrlStatQueueSize` is per shard.** Java buffers on one queue of `5192`;
  here the bound is per shard (16 of them), so one thread can buffer the
  configured size and the process up to 16 times it.
- **`Http.Server.RealIpHeader` defaults to `X-Forwarded-For`, `X-Real-Ip`**,
  where Java trusts no header by default, so existing deployments keep recording
  the address they did before.
- **`Http.Server.RecordRequestParam` defaults to off**, where Java defaults it
  on, because query strings routinely carry tokens, session ids and user ids.
- **`Http.UrlStatEnableTrimPath` defaults to off**, matching Java and Go, which
  aggregate a recorded URI template verbatim and have no equivalent option.
- **Tracing does not start before agent registration.** Java sends spans while
  registration retries in the background; here `NewSpan()` returns noop spans and
  no statistics are collected until registration succeeds.
- **`Sampling.PercentRate: 0` never samples**, matching Java's `FalseSampler`
  rather than the pre-v2.0.0 behaviour of raising it to `0.01`.

### Internal reload details

`Enable` is read once, when the agent is constructed (`make_agent()` in
`src/agent.cpp`), which is why a reload can neither start nor stop tracing.
`IsContainer` is carried by the next periodic AgentInfo re-registration because
`GrpcAgent::build_agent_info()` (`src/grpc.cpp`) reads the published config, not
the pinned boot snapshot. A bypassed SQL statement carries no cache key
(`SqlUidMeta` in `src/grpc.h`), so its re-sent metadata is abbreviated up front
at `kMaxSqlMetaLength` (64 KiB) rather than carrying the whole statement.

---

## Related Documentation

- [CONTRIBUTING.md](../CONTRIBUTING.md) — pull requests and the CLA
- [Build Guide](build.md) — building and linking the library
- [benchmark/README.md](../benchmark/README.md) — microbenchmark targets
- [`test/it/README.md`](../test/it/README.md) — mock-collector integration tests
- [`test/e2e/README.md`](../test/e2e/README.md) — live integration tests
- [CHANGELOG.md](../CHANGELOG.md) — release history
