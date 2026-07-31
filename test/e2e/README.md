# Pinpoint C++ Agent live integration tests

This directory contains four complementary suites:

- `smoke_test.sh` is a deterministic correctness suite. It checks agent
  registration, the public C++ and C APIs, HTTP/gRPC propagation, all four gRPC
  RPC shapes, annotations, SQL metadata, call-stack errors, async spans,
  sampling reload, limits, and lifecycle restart.
- `e2e.sh` is the longer-running traffic/RSS suite. It is useful for stress,
  ASan, and Valgrind runs after the correctness suite passes.
- `fixed_rps_test.py` is a constant-arrival-rate load test. It schedules request
  starts at monotonic-clock deadlines, reports latency and scheduling lag, and
  fails when errors or dropped arrivals exceed the configured thresholds.
- `max_throughput_test.py` is an unthrottled saturation test. Its workers reuse
  HTTP connections and issue the next request immediately after each response.

The correctness stack uses separate processes because a Pinpoint agent is a
process-global singleton:

```text
curl -> it_test_server (HTTP upstream)
          |-> http_downstream_server (real HTTP trace headers)
          `-> grpc_server (unary + three streaming shapes)

c_api_scenario  (standalone pure-C API coverage)
fork_scenario   (master makes no agent calls -> fork -> StartAgent per child)
```

## Collector configuration

Set the collector explicitly before running the suite:

```bash
export PINPOINT_CPP_COLLECTOR_HOST="your-collector-host"
```

`pinpoint-config.yaml` intentionally has no `Collector.Host`. Each process
gets an auto-generated agent id plus a unique `PINPOINT_CPP_AGENT_NAME`, so
concurrent test runs are distinguishable under the stable applications
`cpp-it-http-upstream`, `cpp-it-http-downstream`, `cpp-it-grpc-downstream`,
`cpp-it-c-api`, and `cpp-it-fork`.

## Build and run with CMake

```bash
cmake --preset default
cmake --build --preset default --target \
  it_test_server http_downstream_server grpc_server \
  c_api_scenario fork_scenario

./test/e2e/run_e2e.sh \
  --build-dir ./build/default/test/e2e
```

Or use the custom build target:

```bash
cmake --build --preset default --target run_it_test
```

The live suite is not added to ordinary `ctest` runs because it needs the dev
collector and network access. To register it explicitly:

```bash
cmake --preset default -DPINPOINT_REGISTER_LIVE_IT_TEST=ON
ctest --test-dir build/default -L live --output-on-failure
```

## Build and run with Bazel

```bash
bazel build //test/e2e/...
bazel run //test/e2e:run_it_test
```

`//test/e2e:live_integration_test` is tagged `manual`,
`requires-network`, and `exclusive`.

## Correctness assertions

The suite fails unless all of the following are observed:

- every long-running app reports `Agent::Enable() == true`, which occurs only
  after AgentInfo registration succeeds;
- upstream and downstream HTTP trace IDs match, the injected child span ID is
  accepted by the downstream span, and the downstream parent span ID equals
  the upstream span ID;
- gRPC unary, server-streaming, client-streaming, and bidirectional-streaming
  calls retain the distributed trace ID;
- inbound `Pinpoint-Sampled: s0` creates an unsampled downstream span;
- all public annotation value types, logging context, SQL/SQL-UID metadata,
  call-stack exception metadata, and a joined async span are exercised;
- reduced depth/sequence/chunk limits are crossed without breaking requests;
- config reload changes counter sampling and both sampled/unsampled decisions
  are observed;
- shutdown disables the global agent and a subsequent cold agent registers and
  records a trace;
- the pure-C facade and fork-safe startup scenarios pass;
- process logs contain successful AgentInfo registration for every server and
  a successful upstream `SendSpanBatch`.

The local response assertions prove propagation and public API behavior. The
transport-log assertions prove that data reached the configured collector. If
a Pinpoint Web/API endpoint becomes available, a future test can additionally
query the unique agent IDs and validate the stored payload fields.

## Optional load passes

Append a load mode to the orchestrated run:

```bash
./test/e2e/run_e2e.sh \
  --build-dir ./build/default/test/e2e \
  --load-mode full --load-duration 120 --load-concurrency 20
```

Or run the load generator against an already-started stack:

```bash
HOST=127.0.0.1 PORT=8090 ./test/e2e/e2e.sh \
  --mode grpc-all --duration 60 --concurrency 10
```

For maximum throughput without an RPS limit, run the connection-reusing
generator. `--concurrency` is the number of workers continuously kept busy:

```bash
python3 ./test/e2e/max_throughput_test.py \
  --base-url http://127.0.0.1:8090 \
  --mode mixed --duration 60 --concurrency 100
```

The default two-second warm-up is excluded from throughput and latency results.
Use `--warmup 0` to disable it, `--max-error-rate` to permit expected errors, or
`--min-rps` to enforce a performance-regression threshold. The agent must be
ready unless `--no-require-agent` is supplied. The orchestrated stack can run
this generator after smoke checks with:

```bash
./test/e2e/run_e2e.sh \
  --build-dir ./build/default/test/e2e \
  --load-mode mixed --load-max-throughput \
  --load-duration 60 --load-concurrency 100
```

For a fixed request rate, run the dedicated generator directly:

```bash
python3 ./test/e2e/fixed_rps_test.py \
  --base-url http://127.0.0.1:8090 \
  --mode mixed --rps 50 --duration 60 --max-in-flight 100
```

It rotates deterministically through the endpoints in the selected mode. The
`/error` endpoint's intentional HTTP 500 is treated as success. Arrivals are
dropped rather than queued or emitted as catch-up bursts when the client falls
behind or reaches `--max-in-flight`; the default pass criteria allow up to 5%
dropped arrivals and no unexpected response errors. Use `--rps-tolerance` and
`--max-error-rate` to change those thresholds. The agent must be ready unless
`--no-require-agent` is supplied.

The orchestrated stack can run the same fixed-RPS pass after smoke checks:

```bash
./test/e2e/run_e2e.sh \
  --build-dir ./build/default/test/e2e \
  --load-mode full --load-rps 25 --load-duration 120 \
  --load-concurrency 100
```

With `--load-rps`, `--load-concurrency` is the maximum number of in-flight
requests; with `--load-max-throughput`, it is the continuously busy worker
count. With neither option, the existing concurrency-driven `e2e.sh` pass is
used.

## Performance profiling

Build the integration servers with optimized code, debug symbols, and frame
pointers preserved:

```bash
cmake --preset profiling
cmake --build --preset profiling
```

Add `--profile` to either Python load mode. `run_e2e.sh` attaches to the
`it_test_server` PID and selects the platform profiler automatically: xctrace's
Time Profiler on macOS, or `perf record -F 99 --call-graph dwarf` on Linux.

```bash
# Fixed 50 RPS, profiled
./test/e2e/run_e2e.sh \
  --build-dir ./build/profiling/test/e2e \
  --load-mode mixed --load-rps 50 --load-duration 60 \
  --load-concurrency 100 --profile --keep-logs

# Unthrottled maximum throughput, profiled
./test/e2e/run_e2e.sh \
  --build-dir ./build/profiling/test/e2e \
  --load-mode mixed --load-max-throughput --load-duration 60 \
  --load-concurrency 100 --profile --keep-logs
```

By default the profile is stored under the run's log directory, which is kept
automatically. Use `--profile-output PATH` to select another location and
`--profile-frequency N` to change the Linux sampling frequency. macOS produces
a `.trace` bundle for Instruments; Linux produces a `perf.data`-compatible file.

To profile either generator against an already-running server, use the common
wrapper directly:

```bash
./test/e2e/profile_load.sh --pid SERVER_PID --output ./profile -- \
  python3 ./test/e2e/fixed_rps_test.py \
    --base-url http://127.0.0.1:8090 --mode mixed --rps 50 --duration 60
```

The SQL endpoints intentionally exercise `SetSqlQuery` and collector metadata;
they do not require a database. `init.sql` remains only as optional seed data
for developers who attach a real MySQL instrumentation sample.
