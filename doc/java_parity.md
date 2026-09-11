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

**A cross-repository reference names a symbol or a file, never a line.** A line
number in the Go tree is stale the next time that tree is edited, and nothing
in this repository's build or review catches it; `mergeUrlStat` (`span.go`) is
still findable a year later. Line numbers are kept only for the Java reference
tree and for this repository's own sources.

---

## Summary

| Behaviour | Java reference | Decision |
|---|---|---|
| Tracing before agent registration | `AgentInfoSender`, `DefaultApplicationContext.start()` | **Declined** — see [below](#tracing-before-agent-registration--declined) |
| Per-URL sampler | `UrlTraceSampler`, `UrlSamplerConfig`, `TraceSamplerProvider` | **Declined** — see [below](#per-url-sampler--declined) |
| Retrying a rejected metadata send | `RetryResponseStreamObserver.onNext` | **Declined, shared with Go** — see [below](#retrying-a-rejected-metadata-send--declined-shared-with-go) |
| Error on an unsampled span event | `DisableSpanEventRecorder.recordException` | **Exceeds Java** — see [below](#error-on-an-unsampled-span-event--exceeds-java) |
| URL statistics tick in progress at shutdown | `AsyncQueueingExecutor.stop`, `UriStatCollectingJob` | **Exceeds Java** — see [below](#url-statistics-tick-in-progress-at-shutdown--exceeds-java) |
| Oversize SQL statement | `DefaultSqlNormalizer` (no cap) | **Exceeds Java, shared with Go** — see [below](#oversize-sql-is-dropped-not-cut--exceeds-java-shared-with-go) |
| Queued SQL / error metadata text | `SqlCacheService`, `profiler.jdbc.maxsqllength` | **Same as Java** — see [below](#queued-metadata-text-is-abbreviated-at-cache-time--same-as-java) |
| Raw-SQL normalization cache | none (`DefaultSqlNormalizer` re-normalizes every call) | **Exceeds Java, shared with Go** — see [below](#raw-sql-normalization-cache--exceeds-java-shared-with-go) |
| Per-environment configuration profiles | `ProfileConfigLoader`, `pinpoint.profiler.profiles.active`, `profiles/{release,local}/pinpoint.config` | **Same idea, Go's layout** — see [below](#configuration-profiles--same-idea-gos-layout) |
| Dropping the oldest item when a send queue is full | `SpanBatchGrpcDataSender` | **Same as Java** — see [below](#full-send-queue-drops-the-oldest-item--same-as-java) |
| Exception chain on an overflowed span event | `AbstractRecorder.recordException`, `DefaultExceptionRecorder` | **Declined** — see [below](#exception-chain-on-an-overflowed-span-event--declined) |
| Exception chain scope and depth | `ExceptionContext`, `ExceptionRecordingState.isChaining`, `ExceptionWrapperFactory` | **One entry per exception** — see [below](#exception-chain-scope-and-depth--one-entry-per-exception) |
| Per-span exception buffer cap | `ExceptionWrapperFactory.maxDepth`, `profiler.exceptiontrace.io.buffering.buffersize` | **Declined** — see [below](#per-span-exception-buffer-cap--declined) |
| Unusable inbound trace context | `DefaultTraceHeaderReader.read`, `DefaultTraceContext.createTraceId` | **Exceeds Java** — see [below](#unusable-inbound-trace-context--exceeds-java) |
| Unparseable `Pinpoint-SpanID` | `DefaultTraceHeaderReader.read`, `SpanId.NULL` | **Exceeds Java** — see [below](#unparseable-pinpoint-spanid--exceeds-java) |
| `Pinpoint-Sampled: s0` checked first | `DefaultTraceHeaderReader.samplingEnable` | **Same as Java** — see [below](#pinpoint-sampled-s0-is-checked-first--same-as-java) |
| SQL statement count scope | `DefaultSqlCountService`, `DefaultShared.incrementAndGetSqlCount` | **Same as Java** — see [below](#sqlerrorcount-is-a-per-transaction-budget--same-as-java) |
| `PSpan.err` error cause mask | `ConfigurableErrorRecorder`, `ErrorCategory`, `DefaultShared.maskErrorCode` | **Same as Java, shared with Go** — see [below](#pspanerr-carries-the-error-cause-mask--same-as-java-shared-with-go) |
| Proxy request headers | `DefaultProxyRequestRecorder`, `NginxRequestParser`, `ApacheRequestParser`, `AppRequestParser`, `UserRequestParser` | **Same as Java, shared with Go** — see [below](#proxy-request-headers--same-as-java-shared-with-go) |
| Acceptor host without `Pinpoint-Host` | `ServerRequestRecorder.recordParentInfo` | **Same as Java, shared with Go** — see [below](#acceptor-host-without-pinpoint-host--same-as-java-shared-with-go) |
| Agent stat collection failure | `CollectJob.run()`, `StatMonitorJob.run()` | **Same as Java for the sample, exceeds Java for the scheduler** — see [below](#agent-stat-collection-failure-loses-one-sample--same-as-java) |
| Active trace registry cap | `DefaultActiveTraceRepository`, `DEFAULT_MAX_ACTIVE_TRACE_SIZE` (Caffeine `maximumSize`) | **Declined, replaced by a warning** — see [below](#active-span-registry-cap--declined-replaced-by-a-warning) |
| Automatic shutdown at process exit | `ShutdownHookRegister`, `DefaultAgent.close()` | **Opt-in, default off** — see [below](#automatic-shutdown-at-process-exit--opt-in-default-off) |
| URL stat capacities | `AsyncQueueingUriStatStorage.SNAPSHOT_LIMIT`, `DefaultMonitorConfig.completedUriStatDataLimitSize`, `UriStatStorageProvider` (5192) | **Completed ticks and URI limit as Java, input queue sharded** — see [below](#url-stat-capacities--completed-ticks-and-uri-limit-as-java-input-queue-sharded) |
| Tracing while a shutdown is in progress | *(no Java counterpart)* | **Off from the first line of `Shutdown()`, Go keeps tracing until drain** — see [below](#tracing-while-a-shutdown-is-in-progress--off-at-once-go-keeps-tracing-until-drain) |
| gRPC channel arguments (flow control, header list, write buffer, connect timeout, idle timeout) | `ClientOption`, `DefaultChannelFactory.setupClientOption` | **Idle timeout disabled as in Java and Go; the rest left at C-core defaults** — see [below](#grpc-channel-arguments--idle-timeout-disabled-as-in-java-and-go-the-rest-left-at-c-core-defaults) |
| URI template recorded twice on one span | `DefaultShared.setUriTemplate`, `DefaultSpanRecorder.recordUriTemplate` | **Same as Java** — see [below](#uri-template-is-first-wins--same-as-java) |
| URL stat entry without an end time | `AgentUriStatData.add` | **Same as Java** — see [below](#url-stat-entry-without-an-end-time-is-skipped--same-as-java) |
| Worker thread lifecycle | `GrpcModuleLifeCycle`, `DefaultApplicationContext`, each `DataSender.close()` | **Same goal, declared as a table** — see [below](#worker-thread-lifecycle--same-goal-declared-as-a-table) |
| Locked parity invariants (17 groups) | `ParserContext`, `DefaultCallStack`, `GrpcSpanProcessorV2`, `Header`, `CountingSampler`, `UriStatHistogramBucket`, `BaseHistogramSchema`, `DefaultTransactionCounter`, `StringUtils`, `ClientOption`, `ErrorCategory`, `SpanBatchGrpcDataSender`, `DefaultProxyRequestRecorder` | **Verified identical** (groups 15, 16 and the policy half of 17 are a port consensus) — see [below](#locked-parity-invariants--verified-identical) |

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

## Retrying a rejected metadata send — declined, shared with Go

**Java.** `RetryResponseStreamObserver.onNext` treats
`PResult.getSuccess() == false` exactly like a transport failure and calls
`retryScheduler.scheduleNextRetry(message, nextRetryCount())`.

**This agent.** `GrpcMetadata::process_completed` (`src/grpc.cpp`) drops the
item instead, the way it drops a non-retryable status, and releases its cache
entry so the next span re-registers the id and sends a *new* request.
`PResult.success == false` is a verdict on the request's content (bad id,
unsupported field, rejected payload), and a retry would replay the same bytes
for the same verdict.

**Go.** Same policy: `metaVerdictOf` (`agent.go`) classifies
`PResult.success=false` as `metaRejected`, no retry, and parks the item in
`agent.metaRetry` as a release-only entry so the cache slot is freed one
`metaRetryDelay` later — the counterpart of `schedule_cache_release`. The Go
agent's `doc/java_parity.md` records the same decision from its side.

**Decision: intentional divergence, fixed as a port consensus.** The retry
cannot change the answer, and the release is what preserves a recovery path at
all. Because the policy is a three-way split (Java retries, both ports drop),
it is locked as group 17 of the invariants below so a change in either port
cannot silently leave the two disagreeing; a change of policy is a change in
both ports, made together. Note that agent
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
failure rate toward zero. `Span.IgnoreErrors` and `Span.ErrorMark` /
`Span.ErrorMarkExclude` still apply on this path — the only cause reachable
here is `kException`, since an unsampled span records neither a status code
nor SQL.

**Revisit if** Java's `DisableSpanEventRecorder` starts masking the error code,
at which point this is parity rather than a divergence.

---

## URL statistics tick in progress at shutdown — exceeds Java

**Java.** `AsyncQueueingUriStatStorage` keeps an input queue and a queue of at
most four *completed* 30s snapshots. Shutdown runs `AsyncQueueingExecutor.stop`,
which drains the input queue into the current snapshot and stops; the snapshot
itself is only ever published by `UriStatCollectingJob`, which polls the
completed queue on the stat scheduler and is already closed by then. So Java
loses the tick that was being collected, and any completed snapshot the poller
had not yet taken.

**Go.** `shutdownAgent` calls `flushUrlStat(true)` — the `true` is exactly this
agent's `include_in_progress` — and sends the open tick along with whatever the
completed queue still holds.

**This agent.** Follows Go. `GrpcStats::flush_url_stats_on_shutdown`
(`src/grpc.cpp`) is the stats worker's last act before it closes its stream: it
calls `UrlStats::takeSnapshot(true)`, which drains `completed_` *and* takes the
tick in progress, and writes the result as one `PAgentUriStat`. It is the only
caller that passes `true`; the periodic send never splits an open tick.

The flush sends nothing when the channel is not `GRPC_CHANNEL_READY`, or when no
stats stream is live — the same probe and the same policy as
`GrpcSpan::flush_remaining`, because a write on a dead channel cannot complete
inside `kDefaultShutdownDeadline` (3s). Those entries are counted
(`shutdown_dropped_url_stats`) and logged rather than attempted. The write's own
wait is bounded by `stats_shutdown_flush_timeout` (500 ms), after which it
closes and cancels so the only remaining wait is `finish_stats_stream()`'s.

**Decision: intentional, stronger than Java.** A 30s tick is a large unit to
lose on every clean shutdown, and a rolling deploy makes that loss periodic
rather than rare. The cost is a tick that may cover less than its full window,
which the server aggregates by `(uri, tick)` anyway; the alternative is not
"a whole tick later" but "no tick at all".

**Why it is recorded here.** This behaviour was described by the code comments
long before it worked. The flush used to be attempted inside
`GrpcStats::next_write` as `takeSnapshot(agent_->isExiting())`, but
`isExiting()` turns true the moment `do_shutdown()` runs and `stopping()` is
`stop_requested_ || isExiting()`, so both the worker loop and `next_write`
returned before that line: the argument could only ever be `false`. Comments
asserting a divergence that the code cannot reach survive review; the
regression tests `GrpcStatsShutdownFlushSendsTickStillInProgress`,
`GrpcStatsShutdownFlushAlsoSendsRetainedCompletedTicks` and
`GrpcStatsShutdownFlushDropsAndCountsWhenChannelNotReady`
(`test/test_grpc_with_mocks.cpp`) are what makes this one checkable.

**Revisit if** Java starts flushing the open tick, at which point this is
parity rather than a divergence.

---

## URL stat capacities — completed ticks and URI limit as Java, input queue sharded

Three capacities decide where URL statistics start dropping. Two are now the
Java values; the third differs by construction and is recorded here.

**Java.** `AsyncQueueingUriStatStorage.addCompletedData` tests
`snapshotQueue.size() > SNAPSHOT_LIMIT` (4) *before* offering the closed tick,
so the queue holds **five** completed ticks. Each tick tracks at most
`profiler.uri.stat.completed.data.limit.size` distinct URIs, **1000** by
default (`DefaultMonitorConfig`). Requests wait for the consumer on one
`AsyncQueueingExecutor` queue of **5192** entries (`UriStatStorageProvider`).

**This agent.** `UrlStats::kMaxCompletedSnapshots` is 5 and
`Http.UrlStatLimit` defaults to 1000 — both were 4 and 1024, the first
because both ports read Java's constant rather than its comparison (the Go
lock test still attributes 4 to "Java snapshotQueue capacity"; see the Go
agent's file). The input queue is not one queue: request threads enqueue into
16 shards of `Http.UrlStatQueueSize` (1024) each, drained every 10 ms, so a
single thread can buffer 1024 entries and the process up to 16 × 1024. A
5192-entry single queue has no faithful sharded equivalent (5192 / 16 is not
whole, and a per-shard bound is a per-thread bound); the drain cadence makes
the bound academic outside a stalled worker, where head-of-queue loss is
counted and logged either way.

**Decision: two values adopted, the queue shape kept.** Locked in
`UrlStatWindow` of the parity invariants.

---

## URL statistics send cadence — same as Java (structure)

**Java.** `UriStatCollectingJob` has no timer of its own: it is a job on the
agent stat scheduler, so it polls the completed-tick queue every
`profiler.jvm.stat.collect.interval` (5000 ms in code, 10000 ms in the release
profile). The queue only ever holds *completed* 30s ticks. In addition,
`AsyncQueueingUriStatStorage`'s consumer wakes on a 2s queue-poll timeout and
runs `checkAndFlushOldData`, so the last tick of a burst is closed within about
2s of its window ending and sent on the next scheduler run.

**Before.** `UrlStats::runSendUrlStatsWorker` waited on a fixed 30s
`URL_STAT_SEND_INTERVAL` (`std::make_shared<UrlStats>(this)` took the default,
so production could not change it) and the timer was the only thing that
issued a send token. A tick cut by traffic at its boundary waited up to 30s for
the timer; a trailing tick, closed by the same timer, up to 30s to close and
then the send on top. The Go agent's `sendUrlStatWorker` had the same fixed
30s ticker.

**Now.** Two changes, the same in both ports.

1. **A completed tick wakes the send worker.** `cutInProgressLocked` — the one
   place a tick moves onto `completed_`, reached from both the arrival cut in
   `addLocked` and the clock cut in `closeElapsedTick` — sets `tick_completed_`
   and notifies `send_cond_var_`. The worker's wait predicate reads that flag
   next to `isExiting()`, so the shutdown wakeup on the same condition variable
   is told apart from a ready tick, and a wake with nothing completed issues a
   token whose consumer finds `completed_` empty and builds no message (the
   pre-existing behavior). Under traffic the send interval no longer matters:
   the tick leaves within milliseconds of its boundary, ahead of Java's 5–10s.
   Go signals a capacity-1 channel from `completeLocked` and selects on it
   beside the ticker.
2. **The timed wait follows the stat collect interval** — `Stat.BatchInterval`
   here (`config.stat.collect_interval`, default 5000 ms, range 1–60 s), passed
   by `AgentImpl` into the `UrlStats` constructor; `Stat.CollectInterval` in
   Go. There is no separate key. Its only remaining job is the trailing tick of
   an agent whose traffic stopped, which nothing arrives to cut:
   `closeElapsedTick` runs on every timed wakeup, so that tick is closed within
   one stat interval of its window ending and, through (1), sent at once.

**Why no separate key (option B over option A).** A `Http.UrlStat.SendInterval`
key defaulting to the stat interval was considered and rejected. With (1) the
interval is not a send cadence any more — a tick is sent when it completes —
but only the bound on the idle-agent close of the last tick, and there is no
case for tuning that bound apart from the agent stat cadence: both answer "how
promptly does this agent report a quiet period", and a deployment that
tightens or relaxes one wants the other to follow. That is also Java's
structure verbatim: one scheduler, one interval, URL stats riding on it. The
cost is that URL stats cannot be paced independently of agent stats, which is
exactly Java's constraint too.

**Java's 2s close is not matched.** Java closes the trailing tick within its
2s poll timeout but still sends it only on the next 5–10s scheduler run, so
its end-to-end latency for the last tick is 5–10s. Here the close and the send
are one event, bounded by `Stat.BatchInterval` — 5s by default, so equal or
better at the defaults; a deployment running `Stat.BatchInterval: 60000`
accepts a 60s close of its last tick along with 60s agent stats. A dedicated
2s poll would buy nothing at the defaults and would be a second timer with its
own constant, which is what this change removes.

**Test injection.** The constructor keeps `send_interval` as a parameter so
`test/test_url_stat.cpp` can run the worker in milliseconds; the default is
`defaults::STAT_INTERVAL_MS`, which `JavaParityLockTest.UrlStatWindow` locks
in place of the old `URL_STAT_SEND_INTERVAL == 30s` assertion. The tick width
(`URL_STAT_TICK_INTERVAL`, 30s), the bucket layout, `bucketVersion` and the
completed cap of 4 are unchanged and still locked.

---

## Oversize SQL is dropped, not cut — exceeds Java, shared with Go

**Java.** `DefaultSqlNormalizer` has no length limit: the whole statement is
normalized, however long, and the result is hashed for the SQL UID. Only the
copy in `PSqlMetaData.sql` is abbreviated afterwards (`SqlCacheService`).

**Go.** Before gap C1 there was no limit either, so one pathological statement
could make a span allocate without bound. C1 introduces the same cap as here.

**This agent.** `kMaxNormalizedSqlLength` (1 MiB, `src/sql.h`) bounds the text
the normalizer processes. It used to *cut* the statement at the cap and
normalize the prefix (gap N1). A cut landing inside a string literal left an
unterminated literal, which the state machine — like Java's — emits with no
placeholder, so the normalized text, the SQL id and the SQL UID of a statement
over 1 MiB all differed from what Java computes for the same statement. The cap
now **drops** the statement whole: `SpanEventImpl::SetSqlQuery` returns before
normalization, records no annotation and does not count it toward
`Sql.ErrorCount`, and `SqlNormalizer::normalize` returns an empty result for
any text over its limit so a direct caller cannot get a cut key either.

**Decision, shared with Go.** Both ports had two choices: (A) drop the
statement over the cap, or (B) cut it and normalize the prefix. (B) keeps some
data but yields a key no other agent has, so a mixed Java/C++/Go service splits
one statement across UIDs the moment it crosses the cap. (A) loses the
statement from the trace but every agent that records it agrees on its
identity, and the memory bound — the cap's only purpose — holds. Both agents
take **(A) with the same 1 MiB value**; the Go change is its gap C1 and points
back here, so the two caps move together. A statement of exactly 1 MiB is kept
in both.

**Upgrade note.** A statement over 1 MiB used to appear with a truncated key;
it now does not appear at all, and a throttled warning names its size.

## Raw-SQL normalization cache — exceeds Java, shared with Go

**Java.** There is no cache in front of the normalizer: `DefaultSqlNormalizer`
re-parses every statement, and the only caches are the id/uid ones keyed by
the *normalized* text (`SqlCacheService`).

**Go.** One `rawSqlCache` (`agent.go`) maps raw text to the normalization
result (`normalizedSql{sql, param}`), shared by the id and uid paths.

**This agent.** One `raw_sql_cache_` (`AgentImpl`, `RawSqlCache` in
`src/cache.h`), likewise shared by both `SqlMetaMode`s. It briefly held two
caches, one per mode (gap C6): the cached `PreparedSql` is only the parameters
and the normalized text, which the same normalizer with the same
`Sql.CacheLengthLimit` produces regardless of mode, and the id/uid is resolved
per use from the id/uid caches. The split therefore stored identical entries
twice and doubled the raw cache's worst-case memory for nothing; flipping
`Sql.EnableSqlStats` at runtime also lost every warm entry. The single cache
is toggled by `Sql.EnableRawSqlCache` (reloadable) and matches Go.

**Cache size (gap C7).** Java sizes its SQL id and uid caches by
`profiler.jdbc.sqlcachesize` (1024) through `SimpleCacheFactory.newSqlCache()`
/ `newSqlUidCache()`, while `newSimpleCache()` — the api and string caches —
keeps `SimpleCache`'s own default. This agent follows that split exactly:
`Sql.CacheSize` (default 1024, startup-only) sizes `sql_cache_`,
`sql_uid_cache_` and `raw_sql_cache_`, and the api/error caches stay at
`AgentImpl::kDefaultCacheSize`. The raw cache has no Java counterpart, so it
takes the SQL size because it is keyed per statement like the other two. The
Go agent exposes the same setting, with the same scope and range, as
`SQL.CacheSize` in that port's naming.

## Queued metadata text is abbreviated at cache time — same as Java

**Java.** `SqlCacheService` abbreviates the SQL text to
`profiler.jdbc.maxsqllength` (65536) when the statement first enters the
cache, and that abbreviated copy is what the metadata sender queues. No
queued item carries more than 64 KiB of SQL, however long the normalizer's
output was.

**This agent.** `SqlUidMeta` already did the same: `sql_` is abbreviated to
`kMaxSqlMetaLength` on construction and only the uid cache key stays whole.
`StringMeta` used to queue the whole normalized SQL (up to
`kMaxNormalizedSqlLength`, 1 MiB) and abbreviate only when building
`PSqlMetaData`, so `Grpc.SenderQueueSize` items could pin ~1 GiB of SQL
after a collector outage. It now has the same shape as `SqlUidMeta`:
`str_val_` is the transmitted copy, abbreviated on construction to the cap
of its type — `kMaxSqlMetaLength` (64 KiB) for SQL, `kMaxErrorStringLength`
(256) for an error name — and `cache_key_` is the whole string the id cache
stored under, which `removeCacheSql()` / `removeCacheError()` evict by. The
send path no longer abbreviates, so the `...(<original length>)` marker is
appended exactly once. The caps themselves are unchanged and stay locked by
`JavaParityLockTest.MessageLimits`.

## Configuration profiles — same idea, Go's layout

**Java.** `pinpoint.profiler.profiles.active` (default `release`) selects a
directory, `profiles/<name>/pinpoint.config`, whose properties are layered
over `pinpoint-root.config`; the shipped agent carries `release` and `local`.

**Go.** One file: an `ActiveProfile` key (also `--pinpoint-activeprofile` and
`PINPOINT_GO_ACTIVEPROFILE`) selects the `profile.<name>` subtree of the config
file, which `loadConfig` ranks between the file's top level and the
environment (`cfgSrcFile < cfgSrcProfile < cfgSrcEnv`). An unknown name logs
`config file doesn't have the profile` and applies nothing; `reloadConfig`
re-reads the profile from the file.

**This agent.** Follows Go, key for key: `ActiveProfile` /
`PINPOINT_CPP_ACTIVE_PROFILE` selects `Profile.<name>`, applied after the top
level and before the environment, with the same warning for a missing profile
and the same re-selection on reload. A profile directory in Java's shape has
no place here because this agent has no `profiler` install tree; one file with
subtrees is what an embedding application ships. See
[Configuration](config.md#profiles).

## Full send queue drops the oldest item — same as Java

Recorded because it has been flagged as a divergence in review. It is not one.

**Java.** The default span sender is BATCH
(`profiler.transport.grpc.span.sender.type=BATCH`, `pinpoint-root.config:135`,
repeated in the release and local profiles), and `SpanBatchGrpcDataSender.send`
(`:95-111`) makes room for a new item on a full queue with `queue.poll()` —
the oldest item is discarded, under a "discard oldest message" log line — then
re-offers the new one. Rejecting the *new* item is `GrpcDataSender.send`
(`:56-68`, "reject message"), the base class of the non-default STREAM sender,
reached only when `span.sender.type` is set to STREAM.

**Both ports.** The *span* send queue drops the oldest item when full and
counts the drop, in this agent and in Go alike, so all three leave the same
gap in a sequence under back-pressure. Locked as group 13 below, where the
repeated mis-citation of `GrpcDataSender.send` is recorded too.

The stat send queue is a different mechanism in each port and is compared in
its own right. Here it neither blocks nor discards: `GrpcStats::enqueueStats`
(`src/grpc.cpp`) holds one token per stats type with no payload, so a duplicate
token is simply not enqueued and the producers keep their data until the
stream drains it. This agent's stat loss point is elsewhere —
`AgentStats::runAgentStatsWorker` (`src/stat.cpp`) overwrites a completed batch
that the sender has not taken yet, and reports that through
`stat_batch_drop_reporter_`. Go's `enqueueStat` head-drops its `statChan`
instead; the Go agent records that comparison in its own file.

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

**Both ports.** Neither keeps anything but the failure verdict.
`DisabledSpanEvent::SetError` (`src/span_event.cpp:531-539`) routes to the
owning span's `markSpanError` with `ErrorCategory::kException`; the Go agent's
`overflowSpanEvent.SetError` (`span.go:69-81`) sets `span.root().err`. No
exception info, no annotation, no chain link. `Span.IgnoreErrors`
(`Error.IgnoreErrors` in Go) filters this path exactly as it filters a recorded
event's, so an ignored error fails the transaction on neither.

**Decision: intentional simplification.** Overflow is a profiling *depth*
limit, not a verdict on the transaction, so the two halves are treated
differently on purpose: the failure verdict is what the transaction is judged
by and it survives, while a detailed record of a call made past the depth limit is
precisely what the limit exists to drop. It is also the expensive half — every
chain link carries a full string callstack, and overflow is by definition the
state in which events arrive faster than the configured depth allows.

**Revisit if** this is reported to have blocked a real investigation — an
exception that occurred only past the depth limit, leaving nothing but a failed
transaction to go on.

---

## Exception chain scope and depth — one entry per exception

**Java.** The chain state is the `ExceptionContext` attached to the trace's
recorder, so it spans every span event of a trace. `ExceptionRecordingState`
(`stateOf(previous, current)`) compares the throwable being recorded with the
one recorded before it: if the previous one is in the new one's cause chain the
state is `CONTINUED` and the same `exceptionId` is kept, otherwise `NEW` flushes
the finished chain and asks `ExceptionChainSampler` for a new id (or `DISABLED`).
At flush, `ExceptionWrapperFactory` walks the outermost throwable's causes and
numbers them `exceptionDepth` 0, 1, 2 … — a **cause depth**, independent of the
order the events recorded them in.

**Go.** `span.errorChains` keeps the chain state **per span** (`errors.go`
`getExceptionChainId` / `findError`); an error that is already recorded, or that
wraps a recorded chain head, reuses that chain's id, and the causes are walked
with `depth` 1..n (`addCauserCallStack`). Identity is the error value. The Go
code states the invariant the collector needs: no two entries of one chain
share a depth — "a second depth 0 leaves the collector no way to order the
chain".

**This agent.** Every call-stack `SetError()` on any span event buffers **one
`Exception` with its own `exceptionId`, at depth 0** — a chain of one entry
(`SpanEventImpl::recordException`, `src/span_event.cpp`). Each recorded
exception stamps `ANNOTATION_EXCEPTION_ID` on its event. The rate limiter
(`CallstackTraceNewThroughput`) is asked once per exception, and a refusal
drops that call stack only; the one span-wide latch left is the buffer cap
(`SpanImpl::exception_buffer_full_`). Contract in
[api_contracts.md §9](api_contracts.md#9-error-recording-and-exception-buffering).

**Decision: single-entry chains, no cause API.** Java and Go derive both the
chain boundary and the depth from throwable identity (`getCause()`,
`Unwrap()`). A C++ exception carries no such link the agent could follow, so it
cannot tell a cause from an unrelated exception nor which link caused which.
An earlier revision kept one span-wide chain instead — every link under the
first link's id, all at depth 0 — which is exactly the state Go's comment
rules out: entries sharing an id and a depth cannot be ordered by the
collector. With one entry per exception every chain is trivially well
formed. The consequences are accepted: an exception recorded on a nested
event and again on the event that catches it is two chains, each charged to
the limiter (Java would join them as `CONTINUED`), and a genuine cause chain
recorded link by link arrives as unrelated chains. Java behaves the same way
for exceptions that are *not* in each other's cause chain (`NEW` per
exception), so the id budget is spent the way Java spends it on unrelated
exceptions.

**Revisit if** the API grows a way to name a cause (e.g. `SetError` taking the
id of the exception it wraps) — then a real depth could be numbered.

---

## Per-span exception buffer cap — declined

**Java.** No per-span cap. `ExceptionWrapperFactory.maxDepth`
(`profiler.exceptiontrace.max.depth`, default `5`) caps how many cause links one
chain keeps; `profiler.exceptiontrace.io.buffering.buffersize` (default `20`)
is only the flush threshold of the metadata sender.

**Go.** `Error.MaxChainDepth` (`config.go`, default `maxCauserDepth` = 64 in the
current tree) caps the cause walk of one error, and `canAddErrorChain`
(`span.go`) caps the entries a span keeps at `max(minErrorChainEntry = 10,
Error.MaxChainDepth)` — so 64 by default, 10 only when the depth knob is
lowered below it.

**This agent.** `SpanImpl::kMaxBufferedExceptions` = 100 per span, no per-chain
depth cap, and no `Error.MaxChainDepth` counterpart (gaps S6 and E4 of the
cross-agent review).

**Decision: keep 100, no depth key, for now.** The cap exists to bound the
memory of a long-lived span that keeps failing (every link carries a full
string call stack), and 100 sits between Java's unbounded span and Go's 64
default. A per-chain depth cap is a *walk* limit in both reference agents —
how far the agent follows `getCause()` / `Unwrap()` on its own. This agent does
not walk anything and its chain is flat (see above): every link is one explicit
`SetError` call, so a depth cap here would only discard links the caller
deliberately recorded. Changing the number or adding the key is a config
surface change with its own compatibility note, kept out of the chain-scope
change above.

**Revisit if** a binding gains a cause walk of its own (e.g. over
`std::nested_exception`), at which point `Error.MaxChainDepth` becomes the
natural bound for it, or if 100 links per span is shown to be too much memory
under a real error storm.

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
URL statistics, and as `PSpan.err` (the `SQL` cause, bit `8`). Raise the
threshold, set `Sql.ErrorCount: 0` to turn counting off, or keep the count
and drop just the verdict with `Span.ErrorMarkExclude: [sql]`.

---

## `PSpan.err` carries the error cause mask — same as Java, shared with Go

Recorded because it was a real divergence until it was fixed, and because the
fix is a behaviour change operators can see. The Go port has since landed the
same change, so the three agents now agree; the bit values and the mask rules
are locked as group 12 of the invariants below.

**Java.** `profiler.error.enable` defaults to `true`
(`ErrorRecorderConfig.java`), so `ApplicationContextModuleFactory.newErrorRecorderModule`
loads `ConfigurableErrorRecorderModule`. Its recorder ORs the *category's* bit
into the shared error code — `traceRoot.getShared().maskErrorCode(errorCategory.getBitMask())`
(`ConfigurableErrorRecorder.java:20-24`), and `DefaultShared.maskErrorCode`
is a `getAndUpdate(x -> x | mask)` (`DefaultShared.java:69-72`). The categories
are `UNKNOWN=1<<0`, `EXCEPTION=1<<1`, `HTTP_STATUS=1<<2`, `SQL=1<<3`
(`commons/.../trace/ErrorCategory.java`), recorded respectively by
`AbstractRecorder.recordException`, `HttpStatusCodeRecorder` and
`DefaultSqlCountService`. `ConfigurableErrorRecorderFactory.getEnabledTypes`
turns `profiler.error.mark` / `.mark.exclude` into the enabled set: an unset
mark string means `EnumSet.allOf(ErrorCategory.class)`, the exclusions are
removed, and `UNKNOWN` is added back unconditionally. A category outside that
set masks nothing at all. The flat `1` comes only from `SimpleErrorRecorder`,
which is loaded when `profiler.error.enable=false`.

**This agent.** `SpanImpl::markSpanError(ErrorCategory)` (`src/span.h`) checks
`Config::span::error_mark_mask` and, when the category is enabled, ORs its bit
into the trace root through `SpanData::maskErr` (a relaxed `fetch_or`, which is
what Java's `getAndUpdate` amounts to). `kException` comes from the two
`SetError` paths and from `DisabledSpanEvent::SetError`, `kHttpStatus` from
`SetStatusCode` via `isStatusFail`, `kSql` from `countSqlExecution`.
`Span.ErrorMark` / `Span.ErrorMarkExclude` are the two config keys, parsed by
`error_mark_mask()` (`src/config.cpp`) with Java's rules — comma separated,
`exception` / `http-status` / `sql`, unknown names warned about and ignored,
`kUnknown` always enabled. `kUnknown` is never *recorded* here: every failure
this agent knows about has a cause, so nothing needs the catch-all bit.

**The Go agent does the same.** It exports `ErrorCategory` with Java's four
bit values (`tracer.go`) and funnels every cause through one
`span.markSpanError(category)` (`span.go`), which applies the mask and ORs the
bit into the trace root with `atomic.Int32.Or` — the counterpart of
`SpanData::maskErr` here. `Span.ErrorMark` and `Span.ErrorMarkExclude` are the
same two keys, resolved by `parseErrorMarkMask` (`config.go`) under Java's
rules. The one difference in reach: Go records `kUnknown` for a bare
`SetFailure()` with no category, where this agent never records the catch-all
bit because every failure it knows about has a cause. A C++ service and a Go
service that failed the same way now report the same `err` to the same
collector.

**Decision: no divergence from Java.** The agent previously implemented only
Java's *non-default* path (`SimpleErrorRecorder`, i.e. `profiler.error.enable=false`),
which threw away the cause and made `profiler.error.mark`-style policy —
"a 5xx is not by itself a failed transaction" — inexpressible.

### Upgrade note

C++ sent `err = 1` for every failure up to and including v2.0.0. The same
transactions are still marked failed, but the value on the wire now names the
cause: `2` for an exception, `4` for a status code in
`Http.Server.StatusCodeErrors`, `8` for a `Sql.ErrorCount` overflow, OR-ed
together when more than one applies. Anything that compared `err` against `1`
must test `err != 0` instead.

---

## Proxy request headers — same as Java, shared with Go

Recorded because four separate divergences were fixed at once, and because each
fix changes what operators see on the proxy-header annotation. The Go port has
since landed the same four; the pipeline is locked as group 14 of the
invariants below.

**Java.** `DefaultProxyRequestRecorder.record` walks **every** configured
parser and calls `parseHeaderAndRecord` on each
(`DefaultProxyRequestRecorder.java:52-53`), so one request records one
annotation per header it carries. A parser marks its result valid only from
the `t=` branch: with no `t=`, or one that is not positive, it calls
`setValid(false)` and `parseHeaderAndRecord` records nothing at all
(`NginxRequestParser`, `ApacheRequestParser`, `AppRequestParser`,
`UserRequestParser`, all in `agent-module/agent-plugins/proxy-*`). The four
types and their codes are `APP=1`, `NGINX=2`, `APACHE=3`, `USER=4`
(`*RequestType.getCode()`); the user type reads header names from
`profiler.proxy.http.headers` (`UserRequestConstants`) and puts the matched
header *name* in the annotation's app field. Numeric formats differ per type:
apache's `t`/`D` are microseconds, app's `t` is milliseconds, and nginx's
`t` (`$msec`) and `D` (`$request_time`) are `seconds.milliseconds` —
`NginxRequestParser.toReceivedTimeMillis` / `toDurationTimeMicros` require
exactly three digits after the last `.` and convert by *deleting* the `.`,
returning 0 for any other shape. `AppRequestParser` runs its `app=` value
through `IdValidateUtils.validateId(app, 30)` and discards the whole header
when the charset (`[a-zA-Z0-9._-]+`) or the 30-character limit fails.

**This agent.** `HttpTracerUtil::setProxyHeader` (`src/http.cpp`) is four
independent blocks, one per type, each appending its own annotation, each
gating on `received_time > 0`. The nginx conversions are integer arithmetic on
the dot-stripped digits, like Java's, not a `double` scaled by 1e6: `0.123`
has no exact binary representation, so the floating-point route truncates
123000 µs to 122999. `app=` is validated with the shared `isIdChars`
(`src/utility.h`, the same check the inbound transaction id uses) plus the
30-byte limit, and a failure discards the header. The user type's header names
are `Http.Server.ProxyUserHeaderNames`, empty by default as in Java.

**Two deliberate small deviations.** Java's `validateId` counts UTF-16 code
units where this agent counts bytes, so a non-ASCII name at the boundary can
be judged differently — moot in practice, since the charset check rejects every
non-ASCII byte first. And when a configured user header name is present but
empty, Java's `parseHeaderAndRecord` `return`s, skipping the *remaining* names
for that parser; this agent continues to the next name, which is what the loop
plainly means.

**The Go agent has the same four now.** `setProxyHeader`
(`plugin/http/server.go`) is four independent `if`s — apache, nginx, app and
the configured user headers — and `appendProxyHeader` is the single `t=` gate
in front of the annotation. `nginxMillis` reads `sec.mmm` as dot-stripped
digits, so the nginx duration is real rather than always 0, and `app=` runs
through `IsValidId(app, 30)`. The user header names are
`Http.Server.ProxyUserHeaderNames`, the same key. A C++ service and a Go
service behind the same nginx now report the same proxy annotations to the
same collector.

**One Go divergence remains, found while locking this: the nginx `D=` is not
gated on being positive.** Java applies the duration only inside
`if (durationTimeMicroseconds > 0)`
(`NginxRequestParser.parseHeader`), so a negative `$request_time` leaves the
duration unset. This agent reaches the same outcome by a different route:
`parseProxyDigits` (`src/http.cpp`) accepts `[0-9]` only, so `D=-0.123` fails
the format check and `parseProxyNginxDurationMicros` returns 0. Go's
`nginxMillis` (`plugin/http/server.go`) parses the value with
`strconv.ParseInt`, which takes the sign, so `D=-0.123` is recorded as
`-123000` µs. Java and this agent agree; Go is the odd one out, and the
negative duration reaches the annotation. Not locked in group 14 for that
reason — the received-time gate is locked on all three, the duration gate is
not.

**Decision: no divergence from Java.** The `t=` gate is the load-bearing one:
an annotation whose received time is 0 is not a harmless empty field. The web
UI charts the proxy-to-agent gap from it, so a 0 renders as a gap of five
decades — worse than recording nothing.

### Upgrade note

Up to and including v2.0.0 this agent recorded the first matching header only,
never recorded a user proxy header, always reported an nginx proxy duration of
0, and recorded an annotation even with no usable `t=`. After this change:
requests behind two proxies gain a second proxy annotation; nginx duration
values become real; annotations that carried a received time of 0 disappear;
and an `app=` value over 30 characters or outside `[a-zA-Z0-9._-]` now drops
its header instead of being truncated to 32 characters.

**Optional fields, later.** An absent or refused `D=`, `i=` or `b=` goes on
the wire as -1 (`kProxyUnset` in `src/http.cpp`), the
`ProxyRequestHeaderBuilder` default the web UI reads as "not reported", rather
than as a 0 it cannot tell from a measured zero; `D=` is applied only when
positive, as every Java parser's `durationTimeMicroseconds > 0` guard does,
and apache `i=`/`b=` only inside `[0, 100]` (`ApacheRequestParser`). The Go
agent made the same change at the same time (`proxyUnset` in
`plugin/http/server.go`); locked in group 14 (`ProxyDurationAndPercentAreGated`).

**Parent application type.** `SpanData::parent_app_type_` starts at -1
(`ServiceType.UNDEFINED`), the value Java's `ServerRequestRecorder` records
through `NumberUtils.parseShort(type, UNDEFINED)` when `Pinpoint-pAppType` is
absent or unparseable; both ports used to default to 1 (UNKNOWN), a real
service type. Sent only next to a parent application name, as before. Locked
in group 5 (`ParentAppTypeDefaultsToUndefined`).

---

## Acceptor host without `Pinpoint-Host` — same as Java, shared with Go

**Java.** `ServerRequestRecorder.recordParentInfo` reads
`Header.HTTP_HOST` and, when the peer sent none, falls back to
`requestAdaptor.getAcceptorHost(request)` before recording it
(`ServerRequestRecorder.java:81-88`). The whole block is gated on a present
`Pinpoint-pAppName`, which is also the condition under which the field reaches
the wire.

**This agent.** `SpanImpl::extractContext` (`src/span.cpp`) records the header
when there is one. It cannot apply the fallback itself — it runs while the span
is created, before any request detail is known — so `helper::traceServerRequest`
(`src/http.cpp`) passes the endpoint it already receives to
`SpanData::setAcceptorHostIfAbsent`, which never overwrites a recorded header.
That endpoint is this agent's equivalent of `getAcceptorHost()`: the host
integration supplies it per request.

**The Go agent fills it in too.** `Extract` (`span.go`) still records the
header when there is one, and the fallback lives in `SetEndPoint` for the same
reason it lives outside `extractContext` here: the server plugins know the
request host only after the span exists. An explicit `SetAcceptorHost` or a
`Pinpoint-Host` header still wins in both.

**Decision: no divergence from Java.** A blank acceptor host is not only a
blank field: `SpanData::getEndPoint` and `getRemoteAddr` default through it, so
a caller that recorded neither used to send `UNKNOWN` for both.

What keeps the fallback from over-reaching is the parent-app-name guard: a span
that has an acceptor host but no `Pinpoint-pAppName` emits no `PParentInfo` at
all, so the fallback cannot invent an unnamed caller node on the server map.
Both ports lock that guard — group 14 of the invariants below.

---

## Agent stat collection failure loses one sample — same as Java

**Java.** `CollectJob.run()` wraps the whole collection of one agent-stat
snapshot in `try/catch (Exception)`: a failure logs at WARN, skips that one
snapshot, and leaves the batch and the scheduler untouched. `StatMonitorJob.run()`
then runs its sub-jobs unprotected, so an exception escaping *there* cancels the
`scheduleAtFixedRate` task for the life of the process.

**Go.** The same policy. `collectAgentStatWorker` (`stats.go`) samples through
`agentStats.collect`, whose deferred `recover` is the counterpart of
`CollectJob`'s `catch`: the panic costs that tick's snapshot, the batch cursor
is left where it was, and the failure is reported through a throttled WARN
(`collectFailures`) rather than swallowed. `superviseWorker` is the backstop
for a panic outside that call, and a restart keeps the partial batch there too
— `collected` and `batch` live on `agentStats`, and a restarted worker re-takes
only the CPU/time baseline (`resetBaseline`, guarded by `workerStarted`) while
the first run cold-initializes.

**This agent.** `AgentStats::runAgentStatsWorker` (`src/stat.cpp`) wraps
`collectAgentStat()` in `try/catch`, as `CollectJob` does: a collection
exception costs exactly that cycle's snapshot, the batch cursor is not advanced,
and the next cycle fills the same slot. Failures are counted and reported through
a rate-limited WARN (`stat_collect_failure_reporter_`, the `QueueDropReporter`
pattern the send queues use) rather than swallowed. `superviseWorker` stays as
the backstop for anything thrown outside that call; that restart keeps the
partial batch too — `runAgentStatsWorker` re-takes only the CPU/time baseline
(`resetCollectionBaseline()`) on a restart, and cold-initializes
(`initAgentStats()`) only on its first run — which is the liveness edge this
agent keeps over Java's `StatMonitorJob`.

---

## Active span registry cap — declined, replaced by a warning

**Java.** `DefaultActiveTraceRepository` keeps in-flight traces in a Caffeine
cache built with `maximumSize(DEFAULT_MAX_ACTIVE_TRACE_SIZE)` (`1024 * 10`).
The map holds copies (`ActiveTraceHandle`), so when instrumentation forgets to
end a trace the cache silently evicts arbitrary entries and memory stays
bounded — at the cost of an active-trace histogram that under-reports.

**Go.** `activeSpanRegistry` is a set of per-shard maps whose entries are real
map values, so the same missing-end bug accumulates memory there rather than in
the spans. It therefore adopts Java's cap: `activeSpanMaxSize` (10240) applied
per shard, a full shard evicting an arbitrary existing entry for the new span,
with the same rate-limited WARN.

**This agent.** `ActiveSpanRegistry` (`src/active_span.h`) has no cap and does
not evict, deliberately. Registrations are intrusive nodes owned by the span
(`ActiveSpanNode` is embedded in `SpanImpl` / `UnsampledSpan`) and merely
linked into a shard list. The registry unlinking one on its own would race the
owner's `drop` and break the `linked_` handshake (the release in `add` paired
with the acquire in `drop`); refusing an `add` past a limit would leave `drop`
unlinking a node that was never linked. Either needs a redesign of the
ownership contract, not a size check. What a leaked node costs is also
different: the span already owns the memory, the registry adds no allocation,
so the leak is the span's, and eviction would only hide it from the histogram.

Instead the registry counts its nodes (a per-shard atomic, adjusted by `add`,
`drop` and the fork-only `abandon`), and `AgentStats::addActiveSpan`
(`src/stat.cpp`) logs a rate-limited WARN (the `LOG_WARN_THROTTLED` /
`QueueDropReporter` pattern) when the total exceeds
`AgentStats::kActiveSpanWarnThreshold`, Java's 10240, naming the count and the
threshold so the operator can suspect a missing `EndSpan`. The threshold is not
configurable: Java's is a constant too, and a count above it is never
legitimate load. The count is not added to `PAgentStat` — the active-request
histogram already sums to it.

---

## Automatic shutdown at process exit — opt-in, default off

**Java.** The agent registers a JVM shutdown hook (`ShutdownHookRegister`,
one of the Java 7 / Java 9 / `Runtime` variants) that calls
`DefaultAgent.close()`, so the last queued spans are flushed and the collector
learns the agent stopped without the application doing anything. The JVM owns
the process, and a shutdown hook runs on normal exit and on `SIGTERM`/`SIGINT`
alike.

**Go.** Same policy — off by default, opt-in — reached by a different
mechanism, and covering the other half of the problem.
`ShutdownOnSignal(agent, sigs...)` (`shutdown_signal.go`) installs a
`signal.Notify` watcher that calls `Shutdown()`, restores the default
disposition with `signal.Stop` and re-raises the signal so the process still
exits with `128+signum`; with no signals named it watches `SIGTERM` and
`SIGINT` (`defaultShutdownSignals`). It is opt-in because `signal.Notify` is
process-wide state a library must not take over silently. What it cannot cover
is `os.Exit`: Go has no `atexit` and no runtime exit hook.

**This agent.** `Shutdown()` (`Agent::Shutdown()`, `include/pinpoint/tracer.h`)
is the only path that flushes the span queue and closes the ping stream, and
by default nothing calls it at process exit — the queued spans are dropped and
the collector is not told the agent stopped. That is deliberate, not an
omission: this is a library embedded in a host application, and the global
agent is heap-allocated and never destroyed precisely so that no teardown
(thread joins, gRPC channel teardown, logging through singletons that may
already be gone) runs during `__cxa_atexit` unless the host has decided it is
safe (see `global_agent()` in `src/agent.cpp`). A signal handler is ruled out
outright: `Shutdown()` is not async-signal-safe, and a library must not take
over the host's signal dispositions.

The host therefore chooses: `helper::ScopedAgent` binds `Shutdown()` to a
scope (the recommended form), an explicit `Shutdown()` call works anywhere,
and `AgentOptions::install_atexit_shutdown`
(`include/pinpoint/tracer.h`, installed by `maybe_install_atexit_shutdown_hook`
in `src/agent.cpp`) registers a `std::atexit` hook for a plain executable that
owns its process. The flag is off by default and is documented with its caveats
(reverse-order destruction of host objects created after `StartAgent()`, no
coverage of fatal signals). The documentation (`Agent::Shutdown()`,
`doc/quick_start.md`, `doc/trouble_shooting.md`) states the loss plainly
instead of leaving it implied.

**The two ports cover opposite halves.** Go's helper handles signals but not
`os.Exit`; this agent's hook handles normal `exit()` but not signals, so under
the default disposition a `SIGTERM` — a Kubernetes rollout, an init-system stop
— still loses the queued spans here even with `install_atexit_shutdown` on. The
gap is not an oversight in either port: neither mechanism is available to the
other, and the remedies differ for the same reason. In Go a handler is
ordinary code, so a host with its own handler calls `Shutdown()` from it. Here
it must not: `Shutdown()` joins threads and tears down gRPC, so a host that has
to survive `SIGTERM` makes its handler take the process down the *normal* exit
path — stop the server loop, let `main()` return — and leaves the guard or the
hook to do the rest. `doc/trouble_shooting.md` ("Signals are yours to handle")
spells that out. Everything the two ports *do* agree on about shutdown — the
3 s deadline, idempotence, naming the stragglers — is group 16 of the
invariants below.

**Revisit if** the agent stops being embeddable (a standalone process of its
own) — then a Java-style unconditional hook would be the right default.

---

## Tracing while a shutdown is in progress — off at once, Go keeps tracing until drain

Recorded because the two ports chose opposite policies and Java offers no
tie-breaker: `DefaultAgent.close()` stops the senders and the JVM is on its
way out, so there is no request path left to decide about.

**Go.** During `phaseStopping` the agent still accepts `NewSpanTracer` and
records spans, which the drain then sends. A request that arrives while the
process is being taken out of the load balancer is traced.

**This agent.** `AgentImpl::do_shutdown` (`src/agent.cpp`) clears `enabled_`
before anything else, so from that instant `NewSpan()` returns the noop span
and the in-flight requests of a graceful shutdown window are not traced. The
workers then drain what was recorded up to that point within the 3 s bound.

**Decision: keep it.** Spans recorded during the shutdown window would race
the drain itself: a span that ends after its worker's final flush is dropped
anyway, and one that ends after teardown would touch a torn-down pipeline.
Turning the request path off first makes the last flush a complete picture
of everything that will ever be sent, at the cost of the shutdown window's
requests. Go accepts the opposite trade — the Go agent records that in its own
file.

**Revisit if** hosts report that the shutdown window carries traffic they
need traced; the change is confined to where `enabled_` is cleared.

---

## gRPC channel arguments — idle timeout disabled as in Java and Go, the rest left at C-core defaults

Keepalive and the message-size limits are locked (group 11 below). The other
HTTP/2 tuning that `ClientOption` carries is compared here, knob by knob. The
C-core mechanics behind each decision (BDP probing, the metadata soft/hard
limits, the write-buffer hint) are documented above `make_channel_arguments` in
`src/grpc.cpp`; this section holds only what the other two agents do.

**Java.** `DefaultChannelFactory.setupClientOption` applies `ClientOption` to a
`NettyChannelBuilder`: `flowControlWindow(1 MiB)` (which turns Netty's
auto-tuning off), `maxInboundMetadataSize(8 KB)`, `WRITE_BUFFER_WATER_MARK` low
/ high, `CONNECT_TIMEOUT_MILLIS`, and `idleTimeout(idleTimeoutMillis)` where
`ClientOption.IDLE_TIMEOUT_MILLIS_DISABLE` is 30 days — the constant is named
as a disable sentinel, and nothing in the agent overrides it.

**Go.** `dialOptions` (`grpc.go`) mirrors Java's fixed values:
`WithInitialWindowSize` / `WithInitialConnWindowSize(1 MiB)` (which sets
`StaticWindowSize` so no BDP estimator is created), `WithWriteBufferSize`,
`WithMaxHeaderListSize(8 KB)`. It also passes
`grpc.WithIdleTimeout(o.idleTimeout)` with `Collector.Grpc.IdleTimeout`
defaulting to `0`, which is what `WithIdleTimeout` documents as "disabled";
grpc-go's unset default would be 30 minutes (`dialoptions.go`
`defaultDialOptions`, v1.82.1).

**This agent.**

| Knob | Java | Go | C++ (gRPC C-core 1.81.1) |
|---|---|---|---|
| Flow control window | fixed 1 MiB, auto-tuning off | fixed 1 MiB, no BDP estimator | **unset**: BDP probing on, window sized from measured bandwidth-delay product |
| Max header list size | 8 KB inbound | 8 KB inbound | **unset**: default is already 8 KB soft / 16 KB hard |
| Write buffer | Netty writability watermarks | socket write batching | **unset**: the only C-core knob is a no-op without `GRPC_WRITE_BUFFER_HINT` |
| Connect timeout | `CONNECT_TIMEOUT_MILLIS` | grpc-go dialer default | **unset**: no C-core equivalent; `readyChannel()` + backoff own reconnects |
| Idle timeout | 30 days (disabled) | `WithIdleTimeout(0)` (disabled); `Collector.Grpc.IdleTimeout` re-enables it | **disabled** by default: `GRPC_ARG_CLIENT_IDLE_TIMEOUT_MS=INT_MAX`; `Collector.Grpc.IdleTimeoutMs` re-enables it |

The first four are left alone because the C-core default is equal or better
than the pinned value, and a pinned value would either disable auto-tuning or
be a no-op. The idle timeout is the one knob whose C-core default (30 minutes,
`legacy_channel_idle_filter.cc kDefaultIdleTimeout`) is worse than Java's
setting: an idle channel closes its transport and stops its keepalive pings,
so an application with no traffic pays a reconnect and a `readyChannel()`
backoff on its next send, and a firewall or L4 balancer can drop the silent
connection unnoticed in between. The decision follows Java; the value cannot
(30 days does not fit a 32-bit millisecond argument), so `0` in this agent's
config is the disable sentinel and maps to the `INT_MAX` that C-core documents
as unlimited. Only the decision is locked
(`JavaParityLockTest.GrpcChannelDefaults`), not the sentinel value.

**All three disable idling, by three different sentinels.** Java's 30 days,
this agent's `INT_MAX` and Go's `0` all mean "never idle" in their own runtime,
and none of the three can spell the others' value: 30 days does not fit a
32-bit millisecond channel argument, and `0` is C-core's "use the default", not
its disable. So the *decision* is the shared thing and the value is not, which
is why this row stays out of group 11 — a lock on the number would be a lock on
an accident of three runtimes. Each port locks its own sentinel against its own
disable semantics instead (`JavaParityLockTest.GrpcChannelDefaults` here,
`Test_javaParityLock_GrpcChannelDefaults` in Go), and the shared decision is
recorded in this row and in the Go agent's matching table. This resolves the
earlier "revisit if the Go agent starts setting `WithIdleTimeout`" trigger: it
did, with `0`, and the answer is that the row does not move.

**Revisit if** Java stops disabling the idle timeout, or if a runtime changes
what its sentinel means — then the row needs a divergence entry of its own
rather than this one.

---

## URI template is first-wins — same as Java

**Java.** `DefaultShared.setUriTemplate(uriTemplate)` is an atomic
`null -> value` compare-and-set: the first recorder to name the URI template
owns it, and later plain calls are ignored. `setUriTemplate(uriTemplate, true)`
(the `force` overload `DefaultSpanRecorder.recordUriTemplate` exposes) is a
plain set for the host that has to replace an early guess with the route it
eventually matched. The status code travels separately
(`DefaultShared.setStatusCode:128-131`, `HttpStatusCodeRecorder`) and is a
plain last-wins setter.

**Java's HTTP method is *not* a plain setter, and both ports diverge from it.**
`DefaultShared.setHttpMethods` (`DefaultShared.java:168-177`) is
`HTTP_METHODS_UPDATER.compareAndSet(this, null, httpMethod)` — the same
`null -> value` CAS as `setUriTemplate`, so Java is first-wins on the method
too, and only `setStatusCode` is the plain setter. Both ports are last-wins on
the method. That half is therefore a two-port consensus, not Java parity: one
C++ call carries `(url_pattern, method, status_code)` together and the status
code has to be last-wins (see below), so the method rides with it. The
practical cost is nil — a request has one method, and the frameworks record it
once — but it is a divergence and is recorded as one rather than presented as
parity. Two in-tree comments still say otherwise and are **not** corrected by
this entry: the comment above `SpanTest.SetUrlStatKeepsTheFirstPatternTest`
(`test/test_span.cpp`) calls Java's method setter "plain", and the comment on
`mergeUrlStat` (`span.go`) in the Go agent says the same. A reader meets the
wrong claim in the code first; this file is where it is settled.

**Go.** The same policy, adopted after this agent. `span.collectUrlStat` and
`noopSpan.collectUrlStat` both go through `mergeUrlStat` (`span.go`,
`noop.go`): once the span holds a real `Url`, a later call keeps it and
refreshes only `Method` and `Status`. The `urlStatUnknown` stand-in is treated
as Java's `null`, so a later real template still fills it in, and
`MetricURLStatForce` is the `force = true` overload. Gap **U6** is closed.

**This agent.** `SpanImpl::SetUrlStat` and `UnsampledSpan::SetUrlStat` used to
`emplace` over the existing entry, i.e. last-wins like Go. They now follow Java:
once `url_stat_` holds a non-empty pattern, a later call keeps the pattern and
refreshes only the method and status code. An empty pattern is treated as Java's
`null` and does not claim the slot. `ForceUrlStat()` (C API
`pt_span_force_url_stat`) is the `force = true` overload. The entry-creation
gate (URL stats off and callstack tracing off) is unchanged.

The scope is the pattern only, on purpose. One C++ call carries
`(url_pattern, method, status_code)` in a single optional, so the alternative
was making the whole entry first-wins. That would have frozen the status code at
whatever the first caller passed — typically `0`, because the framework records
the route before the response exists — and departed from Java, where the status
code is the last recorder's. Keeping the pattern first-wins and the rest
last-wins reproduces Java's per-field semantics for the template and the status
code inside the existing structure, and accepts the method divergence above as
the price.

This also fixes the exception tagging path: `recordException` reads
`getUrlTemplate()` off the same entry, so the `url_template` of a reported
exception is now the first recorded pattern, as in Java.

---

## URL stat entry without an end time is skipped — same as Java

**Java.** `AgentUriStatData.add` checks `uriStatInfo.getEndTime() == 0L` and,
if so, logs at INFO and returns `true` — the entry is *skipped*, which is a
different outcome from the capacity check just above it that returns `false`
(a *drop*).

**Go.** `urlStats.add` and `urlStatSnapshot.add` in `url_stat.go` both return
early on `us.endTime.IsZero()`.

**This agent.** `UrlStatSnapshot::add` (`src/url_stat.cpp`) only guarded
against a null entry and called `tick_clock.tick(us->end_time_)` directly. An
entry whose `end_time_` was never set would have keyed under tick 0 (1970),
and, since `UrlStats::addLocked` compares that tick to the snapshot watermark,
could have disturbed the cut of legitimate ticks. Both producers today
(`SpanImpl::sendUrlStat`, `UnsampledSpan::EndSpan`) set the end time, so this
was latent — but Java and Go both guard it, and a new producer that forgot
would otherwise fail silently. (gap **U7**)

The guard now sits right after the null check and mirrors Java's return
value: it returns `true`, because `addLocked` reads `false` as "the snapshot
refused this entry for capacity" and feeds the limit-drop reporter. A skipped
entry is a producer bug, not a capacity event, so it must not pollute that
counter nor advance the watermark. Rather than Java's INFO, the skip is
reported through a rate-limited WARN (the `QueueDropReporter` pattern) naming
the URL, since its only purpose is to surface a new producer that does not set
the end time.

---

## Worker thread lifecycle — same goal, declared as a table

**Java.** Module lifetime is a first-class concept. `ModuleLifeCycle`
implementations (`GrpcModuleLifeCycle` for the gRPC senders,
`DefaultApplicationContext` for the rest) name each module once in `start()`
and once in `shutdown()`; the threads themselves belong to the module — a
`DataSender.close()` or `ExecutorService` shutdown joins its own executor with
a 3-second bound (`MoreExecutors.shutdownAndAwaitTermination`). Nothing
outside the module knows its thread, so adding a sender is one provider and
two lines, and a forgotten line is a leak, never a crash.

**Go.** Each goroutine is started by the client that owns it and stopped
through its own context / channel close; there is no central list either.

**This agent.** The `AgentImpl` owns every worker thread directly (the
workers dereference the agent, so they must be joined before it dies —
`std::thread` cannot be left joinable without `std::terminate`). Until this
change, the same nine workers were listed in six places: the `Worker` enum,
the name array behind `running_worker_names()`, the spawn sites, the stop
list in `request_stop_workers()`, the join order in `wait_grpc_workers()` and
the abandon list used on the fork path. Missing one place meant a missed join
(a `std::terminate` at destruction) or a leaked signal.

Module lifetime is now declared as a table: `AgentImpl::worker_specs()`
(`src/agent.cpp`) holds one `WorkerSpec` row per `Worker` enumerator — log
name, spawn phase (boot vs after registration), start condition (the command
worker exists only when a command client was built), thread body and
non-blocking stop signal — and the threads live in
`std::array<std::thread, kWorkerCount> worker_threads_`. Spawn, stop, join
and abandon all iterate that table. The order constraints that Java expresses
by writing the calls out are kept explicit in `kTeardownOrder`: the init
thread is joined first because it assigns the other thread slots, and the
config watcher (owned by `ConfigFileWatcher`, not a table row) is signaled
and joined before everything because its reload callback dereferences the
agent. `static_assert`s make a row/enum/order mismatch a build error, which
is the property Java gets from having no list to keep in sync at all.

Not in the table, deliberately: the AgentInfo re-send scheduler (owned by
`GrpcAgent`, joined by `stopAgentInfo()`) and the config watcher, both of
which stay owned by their module the way Java's senders own their executors.
Folding them into the table would have meant spawning them through
`spawn_worker()` from an owner that is not the agent, moving thread ownership
for the sake of one diagnostic. Instead the shutdown-deadline report
(`running_worker_names()`) asks each owner through a lock-free flag —
`GrpcAgent::agentInfoRunning()`, `ConfigFileWatcher::running()`,
`GrpcClient::closingChannel()` — set before the thread (or the
`closeChannel()` call) starts and cleared as it returns. When the 3-second
deadline is exceeded the WARN therefore names the straggler ("still running:
agent-info scheduler") rather than listing three candidates, and an empty
result says what was checked. Java gets the equivalent for free from
`shutdownAndAwaitTermination`'s per-executor "shutdown failed" warning; this
agent has one deadline for the whole teardown, so the report has to name the
thread itself.

**Revisit if** a worker ever needs to be owned by its gRPC client rather than
the agent — then the Java shape (client-owned thread, `close()` joins) fits
and the table row would shrink to a start/stop pair.

---

## Locked parity invariants — verified identical

Everything else in this file records a place where the three agents deliberately
differ. This section is the opposite list: values and algorithms that a
cross-agent review verified to be **identical** in the Java agent, the C++ agent
and the Go agent, and that are now pinned by an assertion suite in each port so
they cannot drift back apart unnoticed.

The suites are `test/test_java_parity_lock.cpp` (C++) and
`java_parity_lock_test.go` (Go), plus
`plugin/http/java_parity_lock_test.go` for the half of group 14 that lives in
the Go agent's `plugin/http` package. They are organised into the same sixteen
groups, in the same order, as the table below. Where an older suite already
covered a group, the lock file cross-references it instead of duplicating it —
the table's "locked by" column names whichever file holds the assertions.

Groups 15 and 16 are the exception to the section's own rule: they lock a
**port consensus** rather than Java parity, because Java has no counterpart to
either. They are kept here because they are still two-agent contracts that must
not drift apart, and each row says so.

**Changing a locked value is a three-agent change.** If one of these assertions
fails, either the change is wrong, or all three implementations, this table and
both suites move in the same pull request. A locked value that has to differ
stops being locked: delete its row here, delete the assertion, and add a
divergence entry above saying why.

| # | Group | Java reference | What is locked | Locked by (C++) | Locked by (Go) |
|---|---|---|---|---|---|
| 1 | SQL normalization state machine | `commons-profiler` `sql/ParserContext.parse`, `DefaultSqlNormalizer` | `<n>#` / `<n>$` substitution drawing from **one shared index counter**; `,,` escaping of a comma inside a literal; `''` consuming no index; an unterminated literal emitting no placeholder; `#` not being a comment; `/*/`; `$`+digit staying an identifier; whitespace preserved; normalization not idempotent; a statement over the 1 MiB input cap **dropped whole**, never cut — the same constant and the same drop policy in both ports, a deliberate shared divergence from Java, which has no input cap at all | `test_sql.cpp` (`SqlTest.JavaParityGoldenCases`, `OversizeSqlIsDroppedNotCut`, `DropsAtHardCap`, ported `JavaDefault*`) · `test_java_parity_lock.cpp` (`SqlNormalizer*`) | `sql_util_test.go` · `java_parity_lock_test.go` (`…SqlNormalizerGoldenCases`, `…SqlNormalizerSharedIndexCounter`, `…SqlNormalizerIsNotIdempotent`, `…SqlNormalizerWhitespaceIsNotNormalized`, `…SqlNormalizerRemoveComments`, `…SqlNormalizerInputCapDropsTheWholeStatement`) |
| 2 | span event depth / sequence numbering | `DefaultCallStack.isOverflow`, `DefaultCallStack.push`, `DefaultInstrumentConfig`, `pinpoint-root.config` | depth 64 / sequence 5000 / event chunk 20; deepest recorded level is `maxDepth + 1`; exactly `maxSequence` events recorded; `-1` means unlimited; the `(sequence, depth)` pair is **reserved atomically**, so two threads of one span can never be handed the same sequence — a two-port addition, since Java numbers under a single-thread call-stack contract (`sequence++` inside `push`) that neither port can rely on | `…SpanEventLimitDefaults`, `…SpanEventOverflowBoundaries`, `…SpanEventPositionsAreReservedAtomically` | `…SpanEventLimitDefaults`, `…SpanEventLimitFloors`, `…SpanEventOverflowDecision`, `…SpanEventPositionIsReservedAtomically` |
| 3 | span chunk serialization | `context/compress/GrpcSpanProcessorV2` | `keyTime` — final chunk keys off the span's start time, a non-final chunk off its first event; `startElapsed` is the delta to the previous event (to `keyTime` for the first); the chunk is sorted by sequence before serialization; a non-final chunk carries the `endPoint` it was cut with | `test_span.cpp` (`SpanChunkOptimizeMultipleEventsTest`, `SpanChunkOptimizeNonFinalKeyTimeTest`, `SpanChunkEndPointSnapshotTest`) | `…ChunkKeyTimeAndStartElapsed`, `…ChunkSortsBySequence`, `…ChunkSnapshotsEndPoint` |
| 4 | async id / span id sentinels | `DefaultAsyncIdGenerator`, `bootstrap/context/SpanId.NULL` | async id `0` and span id `-1` are reserved for "absent"; a drawn id is redrawn until it is not the sentinel | `…AsyncIdSentinel` | `…Sentinels`, `…GeneratedSpanIdIsNeverTheSentinel` |
| 5 | propagation headers and transaction id | `Header`, `TransactionIdUtils`, `sampler/SamplingFlagUtils`, `AnnotationKey` | all ten `Pinpoint-*` header names; `agentId^startTime^sequence`; the agent-id character class; the parser stopping at the third delimiter; only the exact string `"s0"` disabling sampling; the annotation keys the agent emits (12 / 20 / 25 / 40 / 46 / 300 / −52) | `…PropagationHeaderNames`, `…AnnotationKeys`, `…TransactionIdFormat`, `…TransactionIdParsing`, `…SampledHeaderEncoding` | `…PropagationHeaderNames`, `…AnnotationKeys`, `…TransactionIdFormat`, `…TransactionIdParsing`, `…SampledHeaderEncoding` |
| 6 | sampling formulas and the throughput limiter | `sampler/CountingSampler`, `PercentRateSampler`, `PercentSamplerFactory`, `RateLimiter.create` → Guava `SmoothBursty` (via `RateLimitTraceSampler`, `ExceptionChainSampler`) | counting tests the **pre-increment** value, so the first request of the process is sampled and every rate-th one after it; the percent admission window is `(0, rate]`; the percentage is multiplied by 100 and truncated; rate 0 / 1 / 100 are the False- and TrueSampler cases; a negative rate is clamped, never promoted to unsigned; the throughput bucket behind the per-second limits **starts empty** (Guava's `storedPermits = 0`), so a fresh limiter admits exactly one caller and paces the rest at tps, a rebuild on reload starts empty again, and steady-state capacity is exactly one second of permits however long the idle (`maxBurstSeconds = 1`) | `…CountingSamplerPhase`, `…CountingSamplerEdgeRates`, `…PercentSamplerWindow`, `…PercentSamplerEdgeRates`, `…ThroughputLimiterInitialState` · `test_limiter.cpp` (`FirstCallPassesThenPacesAtTps`, `IdleBurstIsCappedAtTps`, `LongIdleDoesNotAccumulate`) | `…CountingSamplerPhase`, `…CountingSamplerEdgeRates`, `…PercentSamplerWindow`, `…PercentSamplerRateTruncation`, `…ThroughputLimiterInitialState`, `…ThroughputLimiterCapacity` |
| 7 | URI histogram layout and URL stat entry rules | `common/trace/UriStatHistogramBucket.Layout`, `AsyncQueueingUriStatStorage`, `URITemplate.NULL_URI`, `DefaultShared.setUriTemplate`, `AgentUriStatData.add` | the eight bucket bounds (100 / 300 / 500 / 1000 / 3000 / 5000 / 8000 / ∞); `bucketVersion = 0`; a 30s tick aligned to the epoch boundary; five completed snapshots retained (`SNAPSHOT_LIMIT` 4 compared before the offer); the per-tick URI limit of 1000; an all-zero histogram travels as an empty message while a single 0 ms sample does not; the no-URI stand-in key `/NULL`; the URI template is **first-write-wins** with an explicit force override, while the status code is last-write-wins (the HTTP method is last-write-wins in both ports and diverges from Java — see [URI template is first-wins](#uri-template-is-first-wins--same-as-java)); an entry whose end time was never set is **skipped**, not keyed under tick 0, and is not counted as a capacity drop | `…UrlStatHistogramBuckets`, `…UrlStatWindow`, `…UrlStatUnknownKey`, `…UrlStatEmptyHistogram`, `…UrlStatEntryWithoutAnEndTimeIsSkipped` · `test_span.cpp` (`SetUrlStatKeepsTheFirstPatternTest`, `SetUrlStatEmptyPatternDoesNotClaimTheSlotTest`, `ForceUrlStatReplacesTheRecordedPatternTest`) | `…UrlStatHistogramBuckets`, `…UrlStatWindow`, `…UrlStatEmptyHistogram`, `…UrlStatUnknownKey`, `…UrlStatTemplateIsFirstWriteWins`, `…UrlStatWithoutAnEndTimeIsSkipped` |
| 8 | active trace histogram layout | `common/trace/BaseHistogramSchema` NORMAL schema | the four slots at 1000 / 3000 / 5000 ms with an **inclusive** upper bound, so a span at exactly 1000 ms is still "fast" | `…ActiveTraceHistogram` | `…ActiveTraceHistogram` |
| 9 | transaction counters | `context/id/DefaultTransactionCounter` | all six counters (sampled/unsampled/skipped × new/continuation) exist and drain independently, and a drain resets them | `test_stat.cpp` (`SamplingCountersTest`, `AllCountersMixedIncrementTest`, `CollectResetsCountersBetweenCallsTest`) | `…TransactionCounters` |
| 10 | message truncation format | `StringUtils.abbreviate`, `AbstractRecorder.recordException` | a value within the cap is returned verbatim; a longer one keeps its first *n* bytes and gains a `...(original length)` suffix; the caps 256 (span / span event error) and 65536 (SQL metadata text); the cut lands on a UTF-8 boundary so the result stays valid for protobuf | `…TruncationFormat`, `…TruncationCutsOnAUtf8Boundary`, `…MessageLimits` | `…TruncationFormat`, `…TruncationCutsOnARuneBoundary`, `…MessageLimits` |
| 11 | gRPC channel constants | `grpc/.../client/config/ClientOption`, `GrpcTransportConfig`, `AgentInfoSender`, `pinpoint-root.config` | collector ports 9991 / 9992 / 9993; keepalive 30s / 60s without permit-without-stream; 4 MiB max message; connection and stream renewal off; AgentInfo refresh 24h with 3 tries per attempt; span batch 20 / 1000 ms / 500 ms / 10 concurrent; stat 5000 ms × 6; SQL cache size 1024, limit 2048, expiry 168h, bind value 1024, error count 100; the SQL length limit gating the **UID cache only** and never the id cache, as Java's `UidCache.put` bypass and limit-free `newSqlCache()` do (raised as a defect by two consecutive cross-agent reviews; it is the Java behaviour, so it is locked as behaviour rather than as a constant) | `…CollectorPortDefaults`, `…GrpcChannelDefaults`, `…AgentInfoSchedule`, `…SpanBatchDefaults`, `…StatCollectionDefaults`, `…SqlCacheDefaults`, `…SqlCacheLengthLimitAppliesToTheUidCacheOnly` | `…CollectorPortDefaults`, `…GrpcChannelDefaults`, `…ReconnectBackoff`, `…AgentInfoSchedule`, `…SqlCacheLengthLimitAppliesToTheUidCacheOnly` |
| 12 | error cause categories | `commons/.../trace/ErrorCategory`, `ConfigurableErrorRecorder.recordError`, `ConfigurableErrorRecorderFactory.getEnabledTypes` | the four bits `UNKNOWN = 1`, `EXCEPTION = 2`, `HTTP_STATUS = 4`, `SQL = 8` as a **wire contract** the collector reads out of `PSpan.err`; mask resolution — an unset mark enables every category, the exclude list is subtracted from it, and `UNKNOWN` is re-added last, so it can be neither selected nor excluded; tokens are trimmed, lower-cased and comma-separable, matching `exception` / `http-status` / `sql`, and an unrecognized one is warned about and ignored rather than failing the parse; an **excluded category records nothing at all** — not its bit, not an `UNKNOWN` fallback | `…ErrorCategoryBitValues`, `…ErrorMarkMaskResolution` · `test_span.cpp` (`ErrorMarkExclude*`) | `…ErrorCategoryBits`, `…ErrorMarkMaskResolution`, `…ExcludedCategoryRecordsNothing` |
| 13 | queue overflow policy | `pinpoint-root.config:135` (`profiler.transport.grpc.span.sender.type=BATCH`), `SpanBatchGrpcDataSender.send` | the span queue **head-drops** — the oldest entry is discarded, the newest is always taken, and every drop is counted — which is Java's *default* sender: `SpanBatchGrpcDataSender.send` offers, and on a full queue `queue.poll()`s the head away before re-offering. The tail-drop of `GrpcDataSender.send` ("reject message") belongs to the non-default STREAM sender's base class and has been **mis-cited as the reference in five successive reviews**; the config default above is where to check it before raising it a sixth time. | `…SpanQueueHeadDropsTheOldest` · `test_sharded_bounded_queue.cpp` | `…SpanQueueHeadDrops` · `span_queue_test.go` |
| 14 | proxy request header pipeline | `DefaultProxyRequestRecorder.record`, `NginxRequestParser`, `ApacheRequestParser`, `AppRequestParser`, `UserRequestParser`, `ServerRequestRecorder.recordParentInfo` | all four parsers run **independently**, so a request behind two proxies records two annotations rather than only the hop nearest the agent; each is gated on a **positive received time**, so no `t=`, a `t=0` or one that does not parse records nothing at all; nginx's `t=` (`$msec`) and `D=` (`$request_time`) accept only `sec.mmm` — exactly three decimals — and are converted with integer arithmetic, never a float multiply (`0.123` is 123000 µs, not 122999); `PParentInfo` is emitted **only when `parentAppName` is non-empty**, which is the invariant that keeps the acceptor-host fallback from shipping a parent node with no application name. The nginx **duration** gate is deliberately *not* in this group — see [Proxy request headers](#proxy-request-headers--same-as-java-shared-with-go). | `…ProxyParsersRunIndependently`, `…ProxyHeaderNeedsAPositiveReceivedTime`, `…ProxyNginxTimestampsAreExactThreeDecimals`, `…ParentInfoOnlyWhenParentAppNameIsPresent` · `test_http.cpp` | `plugin/http/java_parity_lock_test.go` (`…ProxyParsersRunIndependently`, `…ProxyHeaderNeedsAPositiveReceivedTime`, `…ProxyNginxTimestampsAreExactThreeDecimals`) · `…ParentInfoRequiresAParentAppName` |
| 15 | logging level policy | **none** — the Java agent's own level comes from its log4j2 configuration, which fails or falls back on its own terms | an **unsupported level string leaves the level in effect unchanged** and logs that it did, rather than resetting to a default: silently ignoring a typo looks like a successful change, and on a config reload it would leave an operator debugging at the old level with no line explaining why; `warn` and `warning` are both accepted; `MaxBackups` defaults to 1 and a value below 1 is restored to it rather than honoured, since `0` reads as "keep none" to one reader and "keep all" to another; the maximum file size defaults to 10 MB. A **two-port consensus**, not Java parity — Java has no counterpart to the first rule. | `…UnsupportedLogLevelKeepsTheCurrentLevel`, `…LogRotationDefaults` | `…UnsupportedLogLevelKeepsTheCurrentLevel`, `…ConfigRejectsAnUnsupportedLogLevel`, `…LogRotationDefaults` |
| 16 | shutdown contract | **none** — shutdown in Java is per-component (each `DataSender.close()` / `GrpcDataSender.release` awaits its own executor for 3 s), with no wall-clock bound on the teardown as a whole and no report of what was still running | a **3 s deadline** bounds the blocking phase of shutdown, after which the teardown is abandoned and `Shutdown()` returns, because neither a gRPC cancellation nor a filesystem-bound watcher join can be bounded on its own; `Shutdown()` is **idempotent** and safe under concurrent callers; a deadline overrun names the **straggler workers by name**, not a count, since "shutdown timed out" alone is not actionable in a host process; the **worker table is the single source of truth** for spawn, stop, join and that report, so the goroutine/thread set and the drain cannot disagree. A **port consensus**, not Java parity. | group 16 of `test_java_parity_lock.cpp` (narrative) · `test_agent_with_mocks.cpp` (`AgentShutdownDeadlineTest.*`, `AgentImplTest.ShutdownIsIdempotent`), and the `static_assert`s on `worker_specs()` / `kTeardownOrder` in `src/agent.cpp` | `…ShutdownDeadline`, `…ShutdownIsIdempotent`, `…ShutdownNamesStragglers`, `…WorkerTableIsTheSingleSourceOfTruth` · `agent_test.go` |

### Deliberately not locked

These sit next to locked values and look like they belong in the table. They do
not, because the agents knowingly differ; each has its own entry above or in
`doc/config.md`.

- **AgentInfo send retry interval** — 3000 ms in both ports against Java's
  effective 300000 ms (`profiler.agentInfo.send.retry.interval`). Registration
  gates tracing in both ports, so it has to retry far more often. The 24h
  refresh and the 3 tries per attempt *are* locked; the retry interval is
  asserted at its port value with a comment pointing here.
- **Span batch size** — 20 in Java's shipped config and in the C++ agent, 50 in
  the Go agent.
- **Flow-control window, write buffer, max header list size, idle timeout** —
  Java pins the first three (`ClientOption`) and the Go agent follows; the C++
  agent leaves them at the gRPC C-core defaults so the BDP estimator can tune
  the window. The idle timeout is disabled in all three, but by three different
  sentinels (30 days / `INT_MAX` / `0`), so the decision is shared and the value
  is not. See
  [gRPC channel arguments](#grpc-channel-arguments--idle-timeout-disabled-as-in-java-and-go-the-rest-left-at-c-core-defaults).
- **The nginx proxy `D=` positivity gate** — group 14 locks the *received
  time* gate on all three agents, but not the duration one. Java applies the
  duration only when `durationTimeMicroseconds > 0` and this agent reaches the
  same outcome through a digits-only parser; the Go agent records a negative
  duration. See
  [Proxy request headers](#proxy-request-headers--same-as-java-shared-with-go).
- **Stat collect interval** — the locked 5000 ms is Java's *code* default
  (`DefaultMonitorConfig`); Java's release profile ships 10000 ms.
- **URL statistics send cadence** — not a constant of its own in any of the
  three. Java polls on the stat scheduler (5–10s); both ports now send a
  completed tick the moment it is closed and time their trailing-tick close by
  the stat collect interval (`Stat.BatchInterval` here, `Stat.CollectInterval`
  in Go). `UrlStatWindow` locks that the default of that ceiling is the stat
  interval's default, not a second 30s timer — see
  [URL statistics send cadence](#url-statistics-send-cadence--same-as-java-structure).

### Skipped assertions

None. The two that used to be here are both resolved and now run:

- Gap **S4**, chunk depth compression — `optimizeSpanEvents` (`span.go`) seeds
  the compression baseline with the first event's own depth, so the Go agent
  compresses from the second event of a chunk as Java's `GrpcSpanProcessorV2`
  and this agent do. `Test_javaParityLock_ChunkDepthCompression` asserts it
  without a skip.
- Gap **U2**, the no-URI stand-in key — `urlStatUnknown` (`url_stat.go`) is
  `/NULL`, Java's `URITemplate.NULL_URI`, not the old `UNKNOWN_URL`, so a mixed
  deployment no longer splits its "no URI recorded" traffic across two
  server-side keys. `Test_javaParityLock_UrlStatUnknownKey` asserts it without
  a skip, on both the sampled and the unsampled path.

The group 3 note in `test/test_java_parity_lock.cpp` still describes S4 as open
and its Go test as skipped; that comment is stale, not this table.
