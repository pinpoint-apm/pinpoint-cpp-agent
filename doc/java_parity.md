# Java Agent Feature Parity Decisions

The Java agent (`agent-module/profiler`) is the reference implementation, but
this agent does not follow it feature for feature: there is no bytecode
instrumentation and no plugin/Guice graph here — the host application calls the
API explicitly — so some Java features have nowhere to attach, and others cost
far more than they are worth. This file records which Java behaviours were
reviewed, what was decided, and what would make us revisit.

Add an entry when a Java behaviour is deliberately *not* matched. A feature that
is simply not written yet does not belong here.

**Cross-agent facts live here and nowhere else.** Statements of the form "Java
does X, Go does Y, this agent does Z" go stale the moment one of the three
changes, and a stale one in a code comment is invisible until it misleads a
reviewer. Code comments state what *this* agent does and why; this file is the
one place that compares.

The Go agent keeps the same file at `doc/java_parity.md`, so the two read side
by side.

---

## Summary

| Behaviour | Java reference | Decision |
|---|---|---|
| Tracing before agent registration | `AgentInfoSender`, `DefaultApplicationContext.start()` | **Declined** — see [below](#tracing-before-agent-registration--declined) |
| Per-URL sampler | `UrlTraceSampler`, `UrlSamplerConfig`, `TraceSamplerProvider` | **Declined** — see [below](#per-url-sampler--declined) |
| Retrying a rejected metadata send | `RetryResponseStreamObserver.onNext` | **Declined** — see [below](#retrying-a-rejected-metadata-send--declined) |
| Error on an unsampled span event | `DisableSpanEventRecorder.recordException` | **Exceeds Java** — see [below](#error-on-an-unsampled-span-event--exceeds-java) |
| Dropping the oldest item when a send queue is full | `SpanBatchGrpcDataSender` | **Same as Java** — see [below](#full-send-queue-drops-the-oldest-item--same-as-java) |

---

## Tracing before agent registration — declined

**Java.** `DefaultApplicationContext.start()` calls `AgentInfoSender.start()`,
which only *schedules* the AgentInfo send (`Integer.MAX_VALUE` retries, spaced
`profiler.agentInfo.send.retry.interval`) and returns. Nothing gates the trace
path on the result: the `TraceContext` the interceptors use is already live, so
a span created before the collector ever accepted the AgentInfo is sampled,
recorded and sent, and the collector reconciles it when the metadata arrives.

**This agent.** `AgentImpl::init_grpc_workers` (`src/agent.cpp`) blocks on
`grpc_agent_->registerAgentWithRetry()` and spawns the ping, meta, span, stat
and command workers only after the collector has accepted the first AgentInfo.
Until then `Enable()` is `false`, `NewSpan()` hands out noop spans, and no
agent, URL or system statistics are collected. None of it is buffered, so those
transactions are simply lost.

**What was tried.** `c09f69a` ("start tracing without waiting for agent
registration") spawned the workers and set `enabled_` first, leaving
registration to retry on the init thread behind them. It was reverted whole by
`aca6611`, whose message says only "This reverts commit …" — this entry is the
reasoning that commit did not carry.

**Decision: keep the gate.** Two reasons:

1. **`Enable()` means "the collector accepted this agent", and the test suite
   is built on that.** The revert restored
   `CollectorUnavailableAtStartupIntegrationTest.EnablesAndStartsAllGrpcWorkersAfterCollectorRecovery`
   ("must not expose a half-started agent or start any downstream worker before
   AgentInfo is accepted") and two siblings, plus `StartStack()`'s own wait
   (`test/it/test_agent_integration.cpp`). Without the gate, a wrong
   `ApplicationName`, an unsupported agent id or a TLS mismatch leaves the agent
   reporting itself enabled and shipping spans the collector cannot attribute —
   a misconfiguration becomes a silently useless process instead of a logged
   one. Registration is also the only RPC that runs before the workers, so it is
   where such a failure surfaces at all: `15b2926` made a rejected registration
   count as a failure precisely so that this could be relied on.
2. **The divergence costs less than it looks.** Registration retries for as long
   as the process runs, so a collector that comes up later is picked up without
   a restart; only the spans created during the outage are lost, and Java loses
   those too whenever the collector is fully down. The behaviours differ only
   when the **agent port (9991) alone** is unreachable while the span port is
   fine — a firewall or mesh rule that misses one of three ports.

**What it costs, and the mitigation.** That narrow case is a real observability
hole: zero spans with 9992/9993 wide open, and nothing in the span pipeline to
show for it. So the wait is loud rather than silent —
`GrpcAgent::registerAgentWithRetry` logs, every
`registration_wait_log_interval` (30s, `src/grpc.h`):

```
still waiting for agent registration after <n>ms (<reason>): tracing stays
disabled (NewSpan is a noop and no stats are collected) until the collector
accepts AgentInfo
```

`<reason>` distinguishes "collector unreachable or the send failed" from
"collector rejected the registration, likely permanent". The Go agent logs the
same line for the same reason, so one troubleshooting page covers both. See
[Troubleshooting](trouble_shooting.md#verifying-agent-startup) and
[API Contracts §11](api_contracts.md#11-noop-and-unsampled-spans-are-deliberately-silent).

**Revisit if** a lazy-dial failure can be surfaced without registering — a
blocking dial or a health probe before `enabled_` is set. The integration tests
would then still see a disabled agent on a bad certificate, which is the only
thing that made `c09f69a` unacceptable.

---

## Per-URL sampler — declined

**Java.** `UrlSamplerConfig` discovers indexed properties by regex over the
whole property map (`profiler.sampling.url.<n>.path`,
`.counting.sampling-rate`, `.percent.sampling-rate`, `.new.throughput`,
`.continue.throughput`) and `TraceSamplerProvider` builds one `TraceSampler` per
entry. `UrlTraceSampler.isNewSampled(urlPath)` walks the entries, takes the
first whose Ant-style pattern matches, and falls back to the default sampler.
Continued traces are never affected — `isContinueSampled()` always delegates to
the default sampler.

**This agent.** One `TraceSampler` per agent, rebuilt on config reload
(`AgentImpl::build_runtime`), consulted by `AgentImpl::newSpan` in `src/agent.cpp`.

**Decision: not ported.** Three reasons:

1. **The common case is already covered, earlier and more cheaply.**
   Per-URL sampling is used overwhelmingly to keep health checks, metrics
   endpoints and static assets out of traces. `Http.Server.ExcludeUrl` does
   exactly that — the same Ant matcher (`HttpUrlFilter`, `src/http.cpp`), run in
   `newSpan()` *before* the sampler, and reloadable. What per-URL sampling adds
   beyond it is a different *rate* per path, not exclusion.
2. **The sampling decision only has the raw path.** `newSpan()` takes
   `rpc_point` — for the HTTP integrations, the request path. The URL template
   arrives later, through `SetUrlStat()`, well after the sampler ran. Entries
   would therefore match raw paths only: `/user/123` needs a hand-written
   wildcard, and an entry can never key on the route template the UI groups by.
3. **Every entry is stateful, and that state is now something we keep.**
   A `TraceSampler` is a base sampler plus up to two `RateLimiter`s
   (`src/sampling.h`), and `fc4fe7e` made the reload path *preserve* that state
   when the sampling config is unchanged — otherwise a config edit resets the
   counter and the very next request is sampled regardless of `CounterRate`. N
   per-URL entries means N such objects to identify across a reload and carry
   over individually. That machinery is the bulk of the work, not the matching.

The config surface is the smaller obstacle but worth stating: `kConfigFields`
(`src/config.cpp`) resolves every option through a fixed table with one YAML
name and one environment variable each, so there is no analogue to Java's regex
scan for `profiler.sampling.url.<n>.*`. An indexed family would have to collapse
into a single list-valued key the way `Span.IgnoreErrors` does, and the
comma-separated environment-variable form documented in
[Configuration](config.md#method-2-environment-variables) cannot express five
sub-fields per entry.

**Revisit if** the sampling decision gains access to the URL template, or if
operators ask for per-path *rates* rather than per-path exclusion. The matching
half is nearly free — `HttpUrlFilter` already compiles and matches these
patterns for `ExcludeUrl` — so what would need designing is reload-safe
per-entry sampler state and a config shape to carry it.

---

## Retrying a rejected metadata send — declined

**Java.** `RetryResponseStreamObserver.onNext` treats
`PResult.getSuccess() == false` exactly like a transport failure and calls
`retryScheduler.scheduleNextRetry(message, nextRetryCount())`.

**This agent.** `GrpcMetadata::process_completed` (`src/grpc.cpp`) drops the
item instead, the way it drops a non-retryable status, and releases its cache
entry so the next span re-registers the id and sends a *new* request.
`PResult.success == false` is a verdict on the request's content (bad id,
unsupported field, rejected payload), and a retry would replay the same bytes
for the same verdict.

**Decision: intentional divergence.** The retry cannot change the answer, and
the release is what preserves a recovery path at all. Note that agent
*registration* does the opposite — a rejected AgentInfo is retried forever
(`GrpcAgent::registerAgentWithRetry`) — because registration is the precondition
for tracing and a rejection there can be transient on the collector's side.

**Revisit if** the collector ever distinguishes a transient rejection from a
permanent one in `PResult`, which would make the retry meaningful.

---

## Error on an unsampled span event — exceeds Java

**Java.** An unsampled trace is a `DisableTrace`. Its span recorder,
`DisableSpanRecorder.recordException`, masks the shared error code so the URL
stat entry counts as failed. Its span *event* recorder does not:
both `DisableSpanEventRecorder.recordException` overloads are empty.

**This agent.** `UnsampledSpanEvent::SetError` (`src/noop.cpp`) routes to the
owning span's `markError`, so a step error on an unsampled request fails the URL
stat entry exactly as a span-level error does. Nothing else about the error is
kept — an unsampled span has nowhere to keep it.

**Decision: intentional, stronger than Java.** With sampling on, unsampled
requests are the majority. A host that reports failures only on the step that
failed — the common case for an outbound call — would have those failures
counted on sampled requests and ignored on unsampled ones, biasing the URL stat
failure rate toward zero. `Span.IgnoreErrors` still applies on this path.

**Revisit if** Java's `DisableSpanEventRecorder` starts masking the error code,
at which point this is parity rather than a divergence.

---

## Full send queue drops the oldest item — same as Java

Recorded because it has been flagged as a divergence in review. It is not one.

**Java.** The default span sender is BATCH
(`profiler.transport.grpc.span.sender.type=BATCH` in `pinpoint-root.config`),
and `SpanBatchGrpcDataSender` makes room for a new item on a full queue with
`queue.poll()` — the oldest item is discarded. Rejecting the *new* item is the
STREAM sender's policy only.

**This agent.** The span and stat send queues drop the oldest item when full.
Both agents therefore leave the same gap in a sequence under back-pressure.

**Decision: no divergence.** Head-drop is the Java default sender's policy.
