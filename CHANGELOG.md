# Changelog

## Unreleased

### Breaking

- **Proxy request headers: every header is recorded, and one without a usable
  `t=` is discarded.**

  Four changes to `setProxyHeader` ([src/http.cpp](src/http.cpp)), all bringing
  the agent to what the Java agent already does:

  1. **nginx `D=` is parsed as `seconds.milliseconds`.** nginx writes
     `$request_time` (and `$msec`) as `sec.mmm`, so the integer parse the agent
     used always failed and **every** nginx proxy annotation reported a
     duration of `0`. Only that exact shape is accepted, as in
     `NginxRequestParser.toDurationTimeMicros`; anything else records no
     duration. apache's `D=` is unaffected — it really is plain microseconds.
  2. **A header with no positive `t=` records nothing.** Java's parsers call
     `setValid(false)` and `DefaultProxyRequestRecorder` skips the header
     entirely. The agent used to record the annotation anyway, with a received
     time of `0` — which the web UI charts as a proxy-to-agent gap of five
     decades.
  3. **All headers are recorded, not just the first match.** A request that
     crossed two proxies now records one annotation per hop, like
     `DefaultProxyRequestRecorder.record`, instead of only the hop closest to
     the agent.
  4. **`app=` is validated, not truncated.** `AppRequestParser` runs it through
     `IdValidateUtils.validateId(app, 30)` and discards the header on failure;
     the agent used to cut the value at 32 bytes, inventing a server-map node
     for any oversized or html-bearing name.

  **New: the user proxy type (code 4).** `Pinpoint-ProxyUser` — or any header
  name listed in the new `Http.Server.ProxyUserHeaderNames`, Java's
  `profiler.proxy.http.headers` — is now read, one annotation per configured
  name that is present, labelled with the header name. Empty by default, as in
  Java. See [doc/config.md](doc/config.md#server-side-tracing).

  **Symptom if you do not migrate:** nothing to migrate, but the annotations
  change. Requests behind nginx gain a real proxy duration where they reported
  `0`; requests behind two proxies gain a second annotation; annotations whose
  received time was `0` disappear; and an `app=` outside `[a-zA-Z0-9._-]` or
  over 30 characters drops its header instead of being truncated.

  **The Go agent still has all four.** `setProxyHeader`
  (`plugin/http/server.go`) is unchanged there, so until the matching change
  lands a C++ service and a Go service behind the same nginx report different
  proxy annotations to the same collector. See
  [doc/java_parity.md](doc/java_parity.md#proxy-request-headers--same-as-java-shared-with-go).

- **The acceptor host falls back to the request endpoint without
  `Pinpoint-Host`.**

  Java's `ServerRequestRecorder.recordParentInfo` reads `Pinpoint-Host` and
  falls back to `requestAdaptor.getAcceptorHost(request)` when the peer sent
  none. The agent recorded the header or nothing, so a continued trace from a
  peer that omits it sent a **blank** acceptor host — and, because
  `getEndPoint()` and `getRemoteAddr()` default through that field, a caller
  who recorded neither sent `UNKNOWN` for both. The endpoint passed to
  `helper::TraceHttpServerRequest` now fills the gap
  ([src/http.cpp](src/http.cpp), [src/span.cpp](src/span.cpp)); a header that
  is present still wins. The Go agent's `Extract` (`span.go`) still leaves it
  blank. See
  [doc/java_parity.md](doc/java_parity.md#acceptor-host-without-pinpoint-host--same-as-java-shared-with-go).

- **`PSpan.err` now carries the error cause, not a flat `1`.**

  Up to and including v2.0.0 every failure set `err = 1`. It is now a bitmask
  of error causes, matching the Java agent's `ErrorCategory`
  (`commons/.../trace/ErrorCategory.java`): `2` for an exception, `4` for a
  status code matching `Http.Server.StatusCodeErrors`, `8` for a
  `Sql.ErrorCount` overflow. Causes accumulate, so a request that threw *and*
  returned 503 reports `err = 6` ([src/span.h](src/span.h),
  `markSpanError`). The same transactions are marked failed as before — only
  the value naming the cause is new.

  This is Java's **default** behaviour: `profiler.error.enable` defaults to
  `true`, which loads `ConfigurableErrorRecorder` and ORs
  `errorCategory.getBitMask()` into the shared error code. The flat `1` the
  agent used to send is Java's `SimpleErrorRecorder`, reached only with
  `profiler.error.enable=false`.

  **Symptom if you do not migrate:** anything that compares `err` against `1`
  — a dashboard query, a log filter, a test — stops matching failures whose
  cause is not `UNKNOWN`. Test `err != 0` instead.

  **New configuration.** `Span.ErrorMark` and `Span.ErrorMarkExclude` (Java's
  `profiler.error.mark` / `.mark.exclude`) select which causes may fail a
  transaction, so a policy like "a 5xx is not by itself a failed transaction"
  is now expressible: `Span.ErrorMarkExclude: [http-status]`. See
  [doc/config.md](doc/config.md#error-causes-spanerrormark-spanerrormarkexclude).

  **The Go agent still sends `1`.** Until the matching change lands there, a
  C++ service and a Go service that failed the same way report different `err`
  values to the same collector. See
  [doc/java_parity.md](doc/java_parity.md#pspanerr-carries-the-error-cause-mask--same-as-java-shared-with-go).

- **Continuing an inbound trace now requires all three trace headers.**

  Up to and including v2.0.0, a request carrying `Pinpoint-TraceID` continued
  the inbound trace whether or not `Pinpoint-SpanID` and `Pinpoint-pSpanID`
  came with it. `NewSpan()` now continues a trace only when the trace id
  parses **and** both id headers are present
  ([src/span.cpp](src/span.cpp), `readInboundTrace`); anything else starts a
  new transaction. This is the Java agent's decision, which runs the same
  checks in order and starts a new trace as soon as one fails
  (`DefaultTraceHeaderReader.java:54-70`).

  A trace id without the two id headers describes a hop the agent cannot
  place. Adopting it recorded a non-root span whose parent was in no trace —
  or left the parent span id at its `-1` default under a trace id that claimed
  a caller — and spent a continue-sampler slot (and `ContinueThroughput`
  budget) doing it.

  **Symptom if you do not migrate:** calls from a peer that sends only
  `Pinpoint-TraceID` — a proxy or gateway that forwards a subset of headers, a
  hand-written client — appear as **two traces instead of one**. No data is
  lost; the link between the two halves is. Continue-sampler slots are no
  longer consumed by such requests, so `ContinueThroughput` counters change
  accordingly.

  **Migration.** Make the peer send `Pinpoint-SpanID` and `Pinpoint-pSpanID`
  as well. Both ports' `InjectContext()` (Go: `InjectContext`) already write
  all three, so a call chain made only of Pinpoint agents needs no change; the
  header set is documented in
  [API Contracts §12](doc/api_contracts.md#12-continuing-an-inbound-trace-requires-three-headers).

  **Two related changes come with it.** An inbound trace id that does not
  parse — including a present-but-blank one — no longer produces a silent noop
  span: since it is not a continued trace, the request starts its own
  transaction and is recorded (with the malformed value logged, throttled).
  Previously the parse ran *after* the sampling decision, so such a request
  spent a continue-sampler slot and then vanished from Pinpoint entirely. The
  decision is now taken once, before the sampler, so the sampler chosen and
  the context extracted can no longer disagree. `Pinpoint-Sampled: s0` still
  short-circuits ahead of all of it, a present-but-unparseable
  `Pinpoint-SpanID` still continues the trace with a generated span id, and an
  absent `Pinpoint-Flags` still means `0` — see
  [doc/java_parity.md](doc/java_parity.md).

- **`Sql.ErrorCount` now applies per transaction, not per span.**

  Up to and including v2.0.0, each span carried its own SQL statement counter,
  so a transaction built from async spans got a fresh `Sql.ErrorCount` budget
  for every one of them — three async spans effectively allowed three times the
  configured number of statements. The counter now lives on the trace root's
  shared data ([src/span.h](src/span.h), `SpanData::sql_count_`, reached
  through `SpanImpl::traceRootData()`), so one transaction has one budget. This
  is the Java agent's behaviour: `DefaultSqlCountService.recordSqlCount`
  increments the trace root's `Shared` (`DefaultSqlCountService.java:15-25`,
  `DefaultShared.java:185-187`), and the Go agent does the same.

  **Who is affected:** services that use async spans *and* run
  `Sql.ErrorCount` or more SQL statements across a whole transaction. Those
  transactions are now **marked failed** where they previously passed. This is
  the point of the setting — an N+1 query pattern spread across async work
  never reached the old per-span threshold — but it is a visible change.

  **Where you will see it:** failed points in the scatter chart, the failed
  histogram in URL statistics, and `PSpan.err`.

  **Migration.** Nothing to change if the new marking is what you want. To keep
  the old volume of failures, raise `Sql.ErrorCount`; to stop SQL-count marking
  altogether, set `Sql.ErrorCount: 0`. Traces without async spans are
  unaffected — a single span was already the whole transaction. See
  [doc/java_parity.md](doc/java_parity.md#sqlerrorcount-is-a-per-transaction-budget--same-as-java).

- **A negative `Sql.ErrorCount` now turns SQL-count error marking off.**

  Up to and including v2.0.0, `make_config()` replaced any negative
  `Sql.ErrorCount` with the default `100`, so a deployment that configured
  `-1` to switch the feature off kept counting and kept marking transactions
  failed at 100 statements — the exact opposite of what it asked for. A
  negative value now warns and is published as `0`
  ([src/config.cpp:866-875](src/config.cpp#L866-L875)), and the runtime's
  existing `limit <= 0` guard ([src/span.h:661-664](src/span.h#L661-L664))
  disables marking.

  `Sql.ErrorCount` merges the Java agent's two keys, where `0` means
  `profiler.sql.error.enable=false` (`SqlCountServiceProvider.java:21-27`).
  Once `0` carries "off", "off" is the only reading a negative threshold can
  consistently carry. Java's literal arithmetic — `enable=true` with a count
  of `0` or less marks the first statement failed, because
  `DefaultSqlCountService` validates nothing and compares with `>=` — is not
  expressible through one key, and is a gap in Java's validation rather than a
  feature to port.

  **Symptom if you do not migrate:** a deployment with a negative
  `Sql.ErrorCount` stops marking transactions failed on SQL count, so N+1
  query patterns no longer surface in the UI through this signal. Nothing else
  changes: `0` still disables, positive thresholds are untouched, and the
  default is still `100`.

  **Migration.** A deployment that relied on a negative value meaning "count
  at 100" must say so: set `Sql.ErrorCount: 100` (YAML) or
  `PINPOINT_CPP_SQL_ERROR_COUNT=100`. Deployments that meant it as "off" need
  no change — they now get what they asked for.

- **`Sampling.PercentRate: 0` now samples nothing.**

  Up to and including v2.0.0, `make_config()` raised any non-negative
  `PercentRate` below `0.01` — exactly `0` included — to `0.01`, so a
  deployment that configured `0` kept collecting traces at 0.01%. That floor is
  gone ([src/config.cpp:795-824](src/config.cpp#L795-L824)): `0` and below now
  disable percent sampling outright, and a positive rate below `0.01` (e.g.
  `0.005`) truncates to `0` and disables it too, with a warning.

  This matches the Java agent, where `parseSamplingRate` truncates the
  configured rate and `createSampler` hands a non-positive result to
  `FalseSampler` (`PercentSamplerFactory.java:40-48,56-58`). The other two
  outcomes were already in place: `>= 100` is always-sample (Java's
  `TrueSampler`) and everything between runs the percent sampler.

  **Symptom if you do not migrate:** with `Sampling.Type: PERCENT` and
  `PercentRate` at `0` (or below `0.01`), the agent stops sampling new
  transactions — no new traces appear in the UI. Continued traces and
  throughput limiting are unaffected, as is `Sampling.Type: COUNTER`.

  **Migration.** A deployment that relied on `0` meaning 0.01% must say so:
  set `PercentRate: 0.01` (YAML) or `PINPOINT_CPP_SAMPLING_PERCENT_RATE=0.01`.
  Deployments that meant `0` as "off" need no change — they now get what they
  asked for.

  See [Sampling Configuration](doc/config.md#sampling-configuration).

- **`ServiceName` is now required for `UidVersion: v4`.**

  Up to and including v2.0.0, a v4 agent started with no `ServiceName`
  silently registered under the fallback service name `DEFAULT` (mirroring
  Java's `ServiceUid.DEFAULT_SERVICE_UID_NAME`). That fallback is gone:
  `resolve_object_name()` now returns `std::nullopt` for a missing or invalid
  `ServiceName`, and the caller aborts agent startup
  ([src/object_name.cpp:211-218](src/object_name.cpp#L211-L218),
  [src/object_name.h:126-132](src/object_name.h#L126-L132)). This matches Java's
  `ObjectNameResolverV4` ("ServiceName not provided") and the Go agent.

  **Symptom if you do not migrate:** the process does **not** start — this is a
  startup failure, not a silent degradation to reduced tracing. The log shows:

  ```
  Failed to resolve ServiceName (required for uid.version=v4, max length 254)
  ```

  **Migration.** Any deployment running `UidVersion: v4` without a
  `ServiceName` must do one of:

  - Set the service name explicitly — YAML key `ServiceName`, or environment
    variable `PINPOINT_CPP_SERVICE_NAME` (max 254 bytes, `[a-zA-Z0-9._-]`). Use
    `DEFAULT` to keep registering under exactly the service the v2.0.0 fallback
    produced.
  - Or stay on the v1/v3 identity, which does not use `ServiceName` at all —
    YAML key `UidVersion: v3`, or `PINPOINT_CPP_UID_VERSION=v3` (`v3` is the
    default).

  See [Identity Versions](doc/config.md#identity-versions).

- **`Http.UrlStatEnableTrimPath` now defaults to `false`.**

  With trimming on at depth 3, a four-segment URI template such as
  `/api/v1/users/{id}` was recorded as `/api/v1/users/*`, so a C++ service
  and a Java service behind the same collector showed different URI lists.
  Java and Go never trim. A caller that passes raw request URLs must now opt
  in with `Http.UrlStatEnableTrimPath: true` to keep its previous keys; a
  caller that passes templates gets its routes back. See
  [doc/config.md](doc/config.md#turn-trimming-on-only-when-you-pass-a-raw-url).

- **URL stat capacities follow Java.** Five completed ticks are retained
  while the stats stream is down (was four: Java compares `size() > 4`
  before offering) and `Http.UrlStatLimit` defaults to `1000` (was `1024`,
  Java's `completed.data.limit.size`). The sharded input queue keeps its
  shape; see
  [doc/java_parity.md](doc/java_parity.md#url-stat-capacities--completed-ticks-and-uri-limit-as-java-input-queue-sharded).
  **The Go agent still has 4 and 1024.**

- **`Stat.BatchInterval` is capped at `10000` ms, Java's
  `DefaultAgentStatMonitor` maximum** (was `60000`); a larger value falls back
  to the default as before. **The Go agent still accepts `60000`.**

### Fixed

- **The metadata worker no longer stalls inside `readyChannel()` for a whole
  collector outage.**

  The metadata pipeline is one thread. It dequeued an item, took a permit and
  then called `GrpcClient::readyChannel()`, which waits — holding the channel
  mutex — until the channel is READY again. Through the outage nothing left
  the queue, `enqueueMeta` overflowed at `Collector.Grpc.SenderQueueSize`,
  every overflow released the item's cache entry, and the next span
  re-registered and re-enqueued the same metadata: drops feeding their own
  inflow, the loop the separate retry-queue budget was meant to prevent. The
  worker now uses `GrpcClient::channelReadyNow()` ([src/grpc.cpp](src/grpc.cpp)):
  READY sends, a connect in progress is waited for only as long as the RPC's
  own deadline, and TRANSIENT_FAILURE fails the attempt at once so the item
  takes the retry schedule and the worker keeps draining. Java and Go fire
  the RPC without waiting and let the failure schedule the retry; this is the
  same shape.

  The outage tests in [test/test_grpc_with_mocks.cpp](test/test_grpc_with_mocks.cpp)
  mocked `readyChannel()` as an immediate `false`, a value production returns
  only when the client is stopping, and so pinned a span drop that never
  happens. The mocks now wait like production and the span test asserts what
  the worker really does: the batch held through the outage and the queued
  spans are delivered on recovery.

- **A queued SQL metadata item no longer holds a second, untruncated copy of
  the statement.**

  `StringMeta` kept the whole normalized SQL as the id cache's eviction key,
  next to the abbreviated copy it sends. The SQL id cache has no length limit
  (a bypassed statement would burn a fresh id per use), so one queue slot
  could pin the normalizer's 1 MiB cap — about 2 GB across a full new queue
  and retry schedule during an outage. The item now keeps the cache's hash of
  the key and evicts with `IdCache::removeByHash()` ([src/cache.h](src/cache.h)),
  so a slot is bounded by the wire cap whatever the statement's length.
  **The Go agent still keeps the key** (`sqlMeta.key`, `agent.go`).

- **URL statistics queued when shutdown begins are aggregated, not lost.**

  `UrlStats::runAddUrlStatsWorker` ([src/url_stat.cpp](src/url_stat.cpp))
  left as soon as the agent was exiting, abandoning up to a tick of entries in
  its shard queues; the stats worker's shutdown flush, which runs after that
  thread is joined, never saw them. The worker now closes its gate and drains
  the shards once on the way out, like Java's `AsyncQueueingExecutor.stop()`
  falling through to `flushQueue()`. **The Go agent's `collectUrlStatWorker`
  still returns on the stop signal without draining `urlStatChan`.**

- **`doc/quick_start.md` step 5 now names `SIGTERM`.** The default
  disposition kills the process before `install_atexit_shutdown` or anything
  else can run, so a host under Kubernetes or systemd must route the signal
  to a normal exit. The rationale for shipping no signal helper is unchanged
  and recorded in [doc/java_parity.md](doc/java_parity.md#automatic-shutdown-at-process-exit--opt-in-default-off).

- **An overflowed span event no longer injects the previous overflow's host.**

  The one `DisabledSpanEvent` a span shares among its overflowed events kept
  the last `SetDestination()` forever. After the stack came back under the
  limit, a later overflow that recorded no destination injected that stale
  host into `Pinpoint-Host`, drawing a server-map edge to a node the request
  never called. The destination is cleared when the last outstanding overflow
  ends ([src/span.cpp](src/span.cpp)), as the Go agent already does.

- **Exception chain links dropped by the per-span cap are counted and
  reported.** `SpanData::addException` refused silently past 100 links;
  `EndSpan` now logs (throttled) how many links the cap cut and on which
  operation, as the Go agent does. The `seal()` comment in
  [src/annotation.h](src/annotation.h) no longer promises a concurrency
  guarantee the flag does not provide; it is a misuse guard under the
  span's single-thread contract.

- **The 1 MiB SQL cap is re-measured on the normalized text.** Literals become
  indexed placeholders (`1` -> `0#`), so a statement dense with short literals
  grows past the cap it passed on input; `AgentImpl::prepareSql` now drops it
  whole instead of caching and queueing an over-cap key, as the Go agent does.

- **The metadata id sequence no longer wraps into negative ids.** Past
  `INT32_MAX` the id caches latch, log once, and hand out id 0 as a hit so no
  metadata is enqueued and no span points at another entry's text
  ([src/cache.h](src/cache.h)). Entries minted before the wrap keep their ids.

- **Every call-stack `SetError()` is now its own exception chain.** The
  span-wide chain sent every link under one `exceptionId` at depth 0, which
  the collector cannot order (the Go agent's `errors.go` states the invariant:
  no two entries of a chain share a depth). Each recorded exception now gets
  its own id and its own `ANNOTATION_EXCEPTION_ID`, and asks the
  `CallstackTraceNewThroughput` limiter on its own; a refusal is no longer
  latched for the rest of the span. See
  [doc/java_parity.md](doc/java_parity.md#exception-chain-scope-and-depth--one-entry-per-exception).

- **Unknown configuration keys are warned about.** The loader only ever
  looked up known paths, so a misspelled key (`Sampling.CounterRte`) was
  silently ignored and, being unknown, never appeared in the `config:` dump
  the troubleshooting guide points at. `make_config` now walks the file
  (profiles included) and logs `unknown config key '<path>' is ignored` for
  each key the table does not know.

- **A log file that cannot be opened is reported on stderr**, naming the path
  and the OS reason, before the logger falls back to stdout. The operator
  used to see only an absent file and agent lines mixed into stdout.

- **Every lost span is counted, and one report carries the total.** The
  drop reporter read only the queue's overwritten-oldest count; a batch
  dropped for want of a permit, a launch that threw, a failed `SendSpanBatch`,
  spans the collector rejected, and batches cleared while the channel was
  down or on shutdown were logged but never counted. They now feed one
  cumulative `span drops: N in total (...)` line that says how many came
  from the queue and how many were never sent, as the Go agent's `spanDrops`
  does. `GrpcSpan::droppedSpans()` exposes the same total.

- **The metadata rejection policy is locked as a port consensus.** Both ports
  drop a `PResult.success=false` reply and release the cache entry after one
  retry delay where Java retries it; the retry budget (3 attempts, 1 s, a
  1000-entry queue) is Java's. Group 17 of the parity invariants
  (`test/test_java_parity_lock.cpp`) pins the numbers so a change in either
  port is a deliberate joint change. **The Go lock suite needs the same
  group.**

- **`Http.Server.ProxyHeaderEnable` switches proxy header recording off.**
  Java's `profiler.proxy.http.header.enable` and the Go agent's key of the
  same name had no counterpart, so the three built-in parsers ran on every
  request even with `ProxyUserHeaderNames` empty. Default `true`, reloadable.

- **An unparseable `Pinpoint-pSpanID` is warned about like an unparseable
  `Pinpoint-SpanID`** (throttled). It used to be silent here and logged in
  the Go agent, so the two agents each reported a different half of the
  same broken hop.
