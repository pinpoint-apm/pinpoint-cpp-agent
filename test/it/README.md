# Mock-collector integration tests

This suite starts a real in-process gRPC collector on three OS-assigned
ephemeral ports. It does not need a running Pinpoint Collector or external
network access.

The mock uses the generated `pinpoint-grpc-idl` service types and exposes the
same topology as production:

- Agent, Metadata, and ProfilerCommandService on the agent port
- Span on the span port
- Stat on the stat port

Every received protobuf and its client metadata are copied into a thread-safe
`CollectorSnapshot`. The tests cover AgentInfo and ping, all metadata request
types, batched root/chunk/async spans, propagation and annotations, agent/URL
statistics, and profiler echo/active-thread commands.

## Agent feature coverage

The suite also exercises the SDK-facing features through a running agent and
asserts their collector wire representation:

- span lifecycle, scoped and implicitly finalized events, duplicate completion,
  out-of-order completion, post-finish mutation guards, abandoned-span active
  request cleanup, async spans, propagation, SQL/errors, annotations, and event
  chunking
- v1/v3 and v4 agent identity metadata across the Agent, Metadata, Span, Stat,
  ping, and command channels, including v4 service-name propagation and API-key
  payload redaction
- exception metadata for errors captured on async spans, including the literal
  `NULL` URI-template fallback
- periodic agent statistics, including sampling decisions, response time, CPU,
  memory, thread count, and active-request histograms, plus the
  `Stat.Enable: false` gate that must suppress all agent-stat batches
- URL-stat normalization, method prefixes, aggregation, latency histograms, and
  failed-request histograms
- counter, percent, and zero-rate sampling, upstream sampling decisions,
  unsampled `s0` downstream propagation, and new/continuation throughput
  limits, including their transaction-stat counters
- API, error, SQL, and SQL-UID cache hits, type separation, invalidation,
  metadata re-publication, and cache release with a fresh id after metadata
  retry exhaustion
- HTTP server/client helpers, proxy address/header handling for the Apache,
  Nginx, and App proxy headers (priority order, out-of-range timestamp
  rejection), configured header and cookie recording, HTTP status handling,
  and client endpoint serialization
- every typed SQL bind-value representation, the `Sql.TraceBindValue` privacy
  gate, and bind-value join truncation at `Sql.MaxBindArgsSize`
- the active-thread-count stream limit and ping-stream socket-id increments on
  reconnect
- the noop agent produced by `Enable: false`, and C API double-destroy safety

## Failure injection

`MockCollector` also provides deterministic transport failure controls:

- `FailNext()` returns a selected gRPC status, immediately or after N stream
  messages.
- `TimeoutNext()` withholds a response until the client's deadline or
  cancellation.
- `RejectNext()` returns `grpc::Status::OK` with `PResult.success=false`.
- `StopEndpoint()` and `StartEndpoint()` close and rebind the Agent, Span, or
  Stat listener on its original ephemeral port, exercising a real connection
  outage rather than only an RPC-level error.

Each handler completion is appended to `CollectorSnapshot::rpc_results`, so a
test can assert both the received protobuf and the injected result. The failure
suite verifies metadata retries, command deadlines, failed span batches,
ping/command/stat stream reconnection, endpoint recovery, and bounded shutdown
while a span request is stalled.

Run with CMake:

```bash
cmake --build --preset default --target agent_integration_test
ctest --test-dir build/default -R agent_integration_test --output-on-failure
```

Run with Bazel:

```bash
bazel test //test/it:agent_integration_test --test_output=errors
```
