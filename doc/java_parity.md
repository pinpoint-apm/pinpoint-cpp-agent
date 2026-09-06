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
| Exception chain on an overflowed span event | `AbstractRecorder.recordException`, `DefaultExceptionRecorder` | **Declined** — see [below](#exception-chain-on-an-overflowed-span-event--declined) |
| Unusable inbound trace context | `DefaultTraceHeaderReader.read`, `DefaultTraceContext.createTraceId` | **Exceeds Java** — see [below](#unusable-inbound-trace-context--exceeds-java) |
| Unparseable `Pinpoint-SpanID` | `DefaultTraceHeaderReader.read`, `SpanId.NULL` | **Exceeds Java** — see [below](#unparseable-pinpoint-spanid--exceeds-java) |
| `Pinpoint-Sampled: s0` checked first | `DefaultTraceHeaderReader.samplingEnable` | **Same as Java** — see [below](#pinpoint-sampled-s0-is-checked-first--same-as-java) |
| SQL statement count scope | `DefaultSqlCountService`, `DefaultShared.incrementAndGetSqlCount` | **Same as Java** — see [below](#sqlerrorcount-is-a-per-transaction-budget--same-as-java) |

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

**This agent.** The *span* send queue drops the oldest item when full.
Both agents therefore leave the same gap in a sequence under back-pressure.

The stat send queue is not comparable and drops nothing: it holds one token per
stats type with no payload, so a duplicate token is simply not enqueued and the
producers keep their data until the stream drains it (`src/grpc.cpp:2941-2954`).

**Decision: no divergence.** Head-drop is the Java default sender's policy.

---

## Exception chain on an overflowed span event — declined

**Java.** `AbstractRecorder.recordException` (`AbstractRecorder.java:62-64`)
calls `recordDetailedException` before anything else;
`WrappedSpanEventRecorder.recordDetailedException`
(`WrappedSpanEventRecorder.java:169-171`) forwards the throwable to
`DefaultExceptionRecorder.recordException`
(`DefaultExceptionRecorder.java:73-83`), which pushes it onto the recorder's
`ExceptionContext` and flushes the finished chain as its own exception
metadata. That path runs unchanged while the call stack is overflowed:
`DefaultCallStack.newInstance` hands out the shared dummy `SpanEvent` once
`isOverflow()` holds, and `DefaultTrace.traceBlockEnd` drops that dummy instead
of appending it — so what overflow discards is only what was written *onto the
event*, the `EXCEPTION_CHAIN_ID` annotation and the `exceptionInfo` class id
and message. The chain itself leaves through the `ExceptionContext`, which
belongs to the recorder and not to the event, and the failure reaches the trace
root separately through `recordError(ErrorCategory.EXCEPTION)`.

**Both ports.** Neither keeps anything but the failure flag.
`DisabledSpanEvent::SetError` (`src/span_event.cpp:483-491`) routes to the
owning span's `markSpanError`; the Go agent's `overflowSpanEvent.SetError`
(`span.go:69-81`) sets `span.root().err`. No exception info, no annotation, no
chain link. `Span.IgnoreErrors` (`Error.IgnoreErrors` in Go) filters this path
exactly as it filters a recorded event's, so an ignored error fails the
transaction on neither.

**Decision: intentional simplification.** Overflow is a profiling *depth*
limit, not a verdict on the transaction, so the two halves are treated
differently on purpose: the failure flag is what the transaction is judged by
and it survives, while a detailed record of a call made past the depth limit is
precisely what the limit exists to drop. It is also the expensive half — every
chain link carries a full string callstack, and overflow is by definition the
state in which events arrive faster than the configured depth allows.

**Revisit if** this is reported to have blocked a real investigation — an
exception that occurred only past the depth limit, leaving nothing but a failed
transaction to go on.

---

## Unusable inbound trace context — exceeds Java

Both ports require all three headers before continuing a trace — a trace id
that parses **plus** the presence of `Pinpoint-SpanID` and `Pinpoint-pSpanID`
— which is Java's rule (`DefaultTraceHeaderReader.java:54-70`). What the two
ports do with a trace id that is *present but unusable* is not.

**Java.** `read()` tests `transactionId == null`
(`DefaultTraceHeaderReader.java:55`) and nothing else, so a **blank** value is
a continued trace: it reaches `DefaultTraceContext.createTraceId`
(`DefaultTraceContext.java:227-231`) →
`TransactionIdUtils.parseTransactionId("")`, where `nextIndex("", 0)` returns
`-1` and the method **throws** `IllegalArgumentException("agentIndex not
found:")` (`TransactionIdUtils.java:84-90`). Any other malformed value that
gets past the reader ends the same way.

**Both ports.** A blank `Pinpoint-TraceID` is read as no header at all, and a
value that fails to parse is not a continued trace either. Either way the
request starts its own transaction: a locally generated trace id, a generated
span id, no parent span id, and the *new*-trace sampler deciding it. The
malformed value is logged once per throttle window, since it is peer-controlled
input that can recur on every request (C++ `TraceId::parseTraceId`,
`src/agent.cpp`; Go `splitTransactionId`, `span.go`).

**Decision: intentional, stronger than Java.** An exception is the host
application's problem, not the agent's, and the request is a real request
whatever its headers say — recording it as the root of its own trace loses the
link to the caller and nothing else. Both ports also make this decision
*before* the sampler is chosen, so an unusable context cannot spend a
continue-sampler slot on a transaction that is then recorded as new.

**Revisit if** Java starts validating the header rather than throwing, at which
point the blank case is parity.

---

## Unparseable `Pinpoint-SpanID` — exceeds Java

**Java.** The reader checks presence only, so a present-but-unparseable id is
still a continued trace; `NumberUtils.parseLong(spanIdStr, SpanId.NULL)` then
yields `SpanId.NULL` (`-1`, `SpanId.java:27`) and the span is recorded with it.

**Both ports.** The trace is continued exactly as in Java — a broken value on
a hop that exists is not the same as a hop that was never described — but the
span id is **generated** rather than left at the sentinel (C++
`SpanImpl::extractContext`, `src/span.cpp`; Go `Span.Extract`, `span.go`), with
a throttled warning.

**Decision: intentional, stronger than Java.** A sentinel id the collector
cannot tell from a real one collapses every such request onto a single node in
the call tree. A generated id keeps each request distinct; the one thing lost
is the (already broken) link to the caller's span.

**Revisit if** the collector gains a way to render `SpanId.NULL` as "unknown
parent" that is more useful than a distinct node.

---

## `Pinpoint-Sampled: s0` is checked first — same as Java

Recorded because the ordering looks arbitrary and is not.

**Java.** `read()` calls `samplingEnable(request)` before it reads any other
header and returns `DisableTraceHeader.INSTANCE` immediately when the value is
`"s0"` (`DefaultTraceHeaderReader.java:47-51`), so no sampler is consulted.

**Both ports.** Same order: `s0` short-circuits ahead of the three-header
check and the sampler (C++ `AgentImpl::NewSpan`, `src/agent.cpp`; Go
`Agent.NewSpanTracerWithReader`, `agent.go`), yielding an unsampled span.

**Decision: no divergence.** An upstream that has already decided not to trace
the request decides for the whole call chain; asking a local sampler first
would let a partly-traced request through and waste a sampler slot on it.

---

## `Sql.ErrorCount` is a per-transaction budget — same as Java

Recorded because it was a real divergence until it was fixed, and the fix is a
behaviour change operators can see.

**Java.** `DefaultSqlCountService.recordSqlCount` takes the trace root's
`Shared` and increments the counter living there
(`DefaultSqlCountService.java:15-25`); `DefaultShared` holds it in an
`AtomicIntegerFieldUpdater`-driven field (`DefaultShared.java:185-187`), so
async work on other threads adds to the one counter. The threshold compares
the post-increment value with `>=`, and an already-failed transaction
(`shared.getErrorCode() != 0`) is skipped before the increment.

**Both ports.** The count lives on the trace root's shared data and is atomic
for the same reason — C++ `SpanData::sql_count_` reached through
`SpanImpl::traceRootData()` (`src/span.h`), Go `span.sqlCount` reached through
`root()` (`span.go`). A trace made of N async spans therefore gets one budget,
not N.

**Decision: no divergence.** Counting per span made the effective limit scale
with a trace's async fan-out, which is exactly the shape `Sql.ErrorCount` is
meant to catch: an N+1 pattern spread over async work would never reach the
threshold.

### Upgrade note

C++ counted per span up to and including v2.0.0. A service that uses async
spans and runs `Sql.ErrorCount` or more statements across a whole transaction
now has those transactions **marked failed** where they previously passed —
visible as failed points in the scatter chart, in the failed histogram of the
URL statistics, and as `PSpan.err`. Raise the threshold, or set
`Sql.ErrorCount: 0` to turn counting off.
