# Pinpoint C++ Agent live integration tests

This directory contains three complementary suites:

- `smoke_test.sh` is a deterministic correctness suite. It checks agent
  registration, the public C++ and C APIs, HTTP/gRPC propagation, all four gRPC
  RPC shapes, annotations, SQL metadata, call-stack errors, async spans,
  sampling reload, limits, and lifecycle restart.
- `e2e.sh` is the longer-running traffic/RSS suite. It is useful for stress,
  ASan, and Valgrind runs after the correctness suite passes.
- `fixed_rps_test.py` is a constant-arrival-rate load test. It schedules request
  starts at monotonic-clock deadlines, reports latency and scheduling lag, and
  fails when errors or dropped arrivals exceed the configured thresholds.

The correctness stack uses separate processes because a Pinpoint agent is a
process-global singleton:

```text
curl -> it_test_server (HTTP upstream)
          |-> http_downstream_server (real HTTP trace headers)
          `-> grpc_server (unary + three streaming shapes)

c_api_scenario  (standalone pure-C API coverage)
fork_scenario   (cold CreateAgent -> fork -> Start per child)
```

## Collector configuration

Set the collector explicitly before running the suite:

```bash
export PINPOINT_CPP_COLLECTOR_HOST="your-collector-host"
```

`pinpoint-config.yaml` intentionally has no `Collector.Host`. Each process also
gets a unique `PINPOINT_CPP_AGENT_ID`, so concurrent test runs are distinguishable
under the stable applications `cpp-it-http-upstream`,
`cpp-it-http-downstream`, `cpp-it-grpc-downstream`, `cpp-it-c-api`, and
`cpp-it-fork`.

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
- all annotation payload types, logging context, SQL/SQL-UID metadata,
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
requests. Without it, the existing concurrency-driven `e2e.sh` pass is used.

The SQL endpoints intentionally exercise `SetSqlQuery` and collector metadata;
they do not require a database. `init.sql` remains only as optional seed data
for developers who attach a real MySQL instrumentation sample.
