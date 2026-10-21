# Changelog

## Unreleased

### Added

- **`ReloadAgentConfig()`: apply a new YAML configuration to the running
  agent.** `StartAgent()` stays one-shot — a repeated call still never reloads
  — so a host that owns the configuration (a control plane, Envoy over xDS)
  had no way to change it without the config-file watcher. The YAML is
  resolved as `StartAgent()` would resolve it from the running agent's own
  `AgentOptions` (environment variables still outrank it; a key it omits takes
  its default), the reloadable options are applied, and the non-reloadable
  ones are kept and returned in `ConfigReloadResult::restart_required` by YAML
  path (`"ApplicationName"`, `"Collector.Host"`). YAML that does not parse, a
  value of the wrong type, or a config `StartAgent()` would refuse is refused
  with nothing changed, as is a call with no running agent or while a config
  file is the source. Thread-safe, and serialized with the config-file
  watcher. C counterpart: `pt_reload_agent_config()`. Documented in
  [Configuration Guide § Reloading from YAML](doc/config.md#reloading-from-yaml-reloadagentconfig).

### Changed

- **The non-reloadable-change warning names the fields.** A reload that tries
  to change a non-reloadable option now logs `non-reloadable config fields
  changed at runtime: ApplicationName, Collector.Host; ...` instead of a fixed
  list of categories, as [the reload docs](doc/config.md#configuration-hot-reload)
  already said it did.

## 2.1.0 (2026-09-17)

### Breaking

- **`SetError()` on an unsampled span's *event* no longer fails its URL stat
  entry.** It now records nothing. `SetError()` on the unsampled **span** is
  unchanged and still marks the entry failed. **A host that records step
  failures on the event and needs them in the URL statistics must record them
  on the span instead** — the failed-request count on unsampled traffic will
  otherwise drop after the upgrade.
- **`helper::TraceHttpClientRequest` strips the URL query by default.** The
  `ANNOTATION_HTTP_URL` value is cut at its first `?` unless the new
  `Http.Client.RecordUrlQuery` (default `false`) is on. Query strings carry
  tokens and ids. Endpoint and destination are unchanged.

### Added

- **`Span::SetLogging()`, a flag-only overload.** Marks the span as one whose
  transaction and span ids reached an application log
  (`PSpan.loggingTransactionInfo`), without writing them anywhere. For hosts
  that inject the context into their logger themselves — a language binding, or
  a logging framework with its own context carrier — instead of through a
  `TraceContextWriter`. The existing `SetLogging(writer)` overload is
  unchanged. Documented in
  [Instrumentation Guide §3](doc/instrument.md#linking-application-logs-to-the-trace).
- **`pt_span_set_logging_flag()`, the C counterpart of the above.** The C API
  could previously only mark a span through `pt_span_set_logging(span, writer)`,
  which also writes `PtxId`/`PspanId` into the carrier; a `NULL` writer there
  records nothing rather than marking. A C host that writes those two keys
  itself now has a way to mark the span without one. Documented in
  [C API Guide §5](doc/instrument_c.md#linking-application-logs-to-the-trace).
- **`SetIgnoredError()` on `Span` and `SpanEvent`.** Records an error exactly
  as `SetError()` does but never marks the transaction failed, for a caller
  that matched an ignore rule `Span.IgnoreErrors` cannot express. Documented in
  [Instrumentation Guide §9](doc/instrument.md#recording-an-error-without-failing-the-transaction).
- **Configurable real-IP headers.** `Http.Server.RealIpHeader` (ordered list,
  default `["X-Forwarded-For", "X-Real-Ip"]` = today's behaviour, `[]` trusts
  none) and `Http.Server.RealIpEmptyValue`: a `Forwarded` header is parsed for
  its `for=` token, other headers give their first hop, a value equal to the
  empty value is skipped. Both reloadable and appended to
  `SpanConfigSnapshot`.
- **Request parameter recording, opt-in.** New `TraceHttpServerRequest`
  overloads taking a `query_string` (C: `pt_trace_http_server_request_with_query`)
  record it as annotation 41 (`ANNOTATION_HTTP_PARAM`) when
  `Http.Server.RecordRequestParam` is on (default `false`).
  `helper::FormatRequestParams` exposes the formatter. Both toggles are
  reloadable and appended to `SpanConfigSnapshot`.

- **Proxy request headers: every header is recorded, and one without a usable
  `t=` is discarded.**

  Four changes to `setProxyHeader` ([src/http.cpp](src/http.cpp)):

  1. **nginx `D=` is parsed as `seconds.milliseconds`.** nginx writes
     `$request_time` (and `$msec`) as `sec.mmm`, so the integer parse the agent
     used always failed and **every** nginx proxy annotation reported a
     duration of `0`. Only that exact shape is accepted; anything else records
     no duration. apache's `D=` is unaffected — it really is plain
     microseconds.
  2. **A header with no positive `t=` records nothing.** The agent used to
     record the annotation anyway, with a received time of `0` — which the web
     UI charts as a proxy-to-agent gap of five decades.
  3. **All headers are recorded, not just the first match.** A request that
     crossed two proxies now records one annotation per hop, instead of only
     the hop closest to the agent.
  4. **`app=` is validated, not truncated.** The value must be at most 30
     characters of `[a-zA-Z0-9._-]` or the header is discarded; the agent used
     to cut the value at 32 bytes, inventing a server-map node for any
     oversized or html-bearing name.

  **New: the user proxy type (code 4).** `Pinpoint-ProxyUser` — or any header
  name listed in the new `Http.Server.ProxyUserHeaderNames` — is now read, one
  annotation per configured name that is present, labelled with the header
  name. Empty by default. See
  [doc/config.md](doc/config.md#server-side-tracing).

  **Symptom if you do not migrate:** nothing to migrate, but the annotations
  change. Requests behind nginx gain a real proxy duration where they reported
  `0`; requests behind two proxies gain a second annotation; annotations whose
  received time was `0` disappear; and an `app=` outside `[a-zA-Z0-9._-]` or
  over 30 characters drops its header instead of being truncated.

- **The acceptor host falls back to the request endpoint without
  `Pinpoint-Host`.**

  The agent recorded the header or nothing, so a continued trace from a peer
  that omits it sent a **blank** acceptor host — and, because `getEndPoint()`
  and `getRemoteAddr()` default through that field, a caller who recorded
  neither sent `UNKNOWN` for both. The endpoint passed to
  `helper::TraceHttpServerRequest` now fills the gap
  ([src/http.cpp](src/http.cpp), [src/span.cpp](src/span.cpp)); a header that
  is present still wins.

- **`PSpan.err` now carries the error cause, not a flat `1`.**

  Up to and including v2.0.0 every failure set `err = 1`. It is now a bitmask
  of error causes: `2` for an exception, `4` for a status code matching
  `Http.Server.StatusCodeErrors`, `8` for a `Sql.ErrorCount` overflow. Causes
  accumulate, so a request that threw *and* returned 503 reports `err = 6`
  ([src/span.h](src/span.h), `markSpanError`). The same transactions are marked
  failed as before — only the value naming the cause is new.

  **Symptom if you do not migrate:** anything that compares `err` against `1`
  — a dashboard query, a log filter, a test — stops matching failures whose
  cause is not `UNKNOWN`. Test `err != 0` instead.

  **New configuration.** `Span.ErrorMark` and `Span.ErrorMarkExclude` select
  which causes may fail a transaction, so a policy like "a 5xx is not by itself
  a failed transaction" is now expressible: `Span.ErrorMarkExclude:
  [http-status]`. See
  [doc/config.md](doc/config.md#error-causes-spanerrormark-spanerrormarkexclude).

- **Continuing an inbound trace now requires all three trace headers.**

  Up to and including v2.0.0, a request carrying `Pinpoint-TraceID` continued
  the inbound trace whether or not `Pinpoint-SpanID` and `Pinpoint-pSpanID`
  came with it. `NewSpan()` now continues a trace only when the trace id
  parses **and** both id headers are present
  ([src/span.cpp](src/span.cpp), `readInboundTrace`); anything else starts a
  new transaction.

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
  as well. `InjectContext()` already writes all three, so a call chain made
  only of Pinpoint agents needs no change; the header set is documented in
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
  absent `Pinpoint-Flags` still means `0`.

- **`Sql.ErrorCount` now applies per transaction, not per span.**

  Up to and including v2.0.0, each span carried its own SQL statement counter,
  so a transaction built from async spans got a fresh `Sql.ErrorCount` budget
  for every one of them — three async spans effectively allowed three times the
  configured number of statements. The counter now lives on the trace root's
  shared data, so one transaction has one budget.

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
  unaffected — a single span was already the whole transaction.

- **A negative `Sql.ErrorCount` now turns SQL-count error marking off.**

  Up to and including v2.0.0, `make_config()` replaced any negative
  `Sql.ErrorCount` with the default `100`, so a deployment that configured
  `-1` to switch the feature off kept counting and kept marking transactions
  failed at 100 statements — the exact opposite of what it asked for. A
  negative value now warns and is published as `0`
  (`make_config()` in [src/config.cpp](src/config.cpp)), which disables
  marking.

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
  gone (`make_config()` in [src/config.cpp](src/config.cpp)): `0` and below now
  disable percent sampling outright, and a positive rate below `0.01` (e.g.
  `0.005`) truncates to `0` and disables it too, with a warning. `>= 100` is
  always-sample and everything between runs the percent sampler, as before.

  **Symptom if you do not migrate:** with `Sampling.Type: PERCENT` and
  `PercentRate` at `0` (or below `0.01`), the agent stops sampling new
  transactions — no new traces appear in the UI. Continued traces and
  throughput limiting are unaffected, as is `Sampling.Type: COUNTER`.

  **Migration.** A deployment that relied on `0` meaning 0.01% must say so:
  set `PercentRate: 0.01` (YAML) or `PINPOINT_CPP_SAMPLING_PERCENT_RATE=0.01`.
  Deployments that meant `0` as "off" need no change — they now get what they
  asked for.

  See [Sampling Configuration](doc/config.md#sampling-configuration).

- **`Http.UrlStatEnableTrimPath` now defaults to `false`.**

  With trimming on at depth 3, a four-segment URI template such as
  `/api/v1/users/{id}` was recorded as `/api/v1/users/*`. A caller that passes
  raw request URLs must now opt in with `Http.UrlStatEnableTrimPath: true` to
  keep its previous keys; a caller that passes templates gets its routes back.
  See
  [doc/config.md](doc/config.md#turn-trimming-on-only-when-you-pass-a-raw-url).

- **URL stat capacities changed.** Five completed ticks are retained while the
  stats stream is down (was four) and `Http.UrlStatLimit` defaults to `1000`
  (was `1024`).

- **`Stat.BatchInterval` is capped at `10000` ms** (was `60000`); a larger
  value falls back to the default as before.

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
  takes the retry schedule and the worker keeps draining.

- **A queued SQL metadata item no longer holds a second, untruncated copy of
  the statement.**

  `StringMeta` kept the whole normalized SQL as the id cache's eviction key,
  next to the abbreviated copy it sends. The SQL id cache has no length limit
  (a bypassed statement would burn a fresh id per use), so one queue slot
  could pin the normalizer's 1 MiB cap — about 2 GB across a full new queue
  and retry schedule during an outage. The item now keeps the cache's hash of
  the key and evicts with `IdCache::removeByHash()` ([src/cache.h](src/cache.h)),
  so a slot is bounded by the wire cap whatever the statement's length.

- **URL statistics queued when shutdown begins are aggregated, not lost.**

  `UrlStats::runAddUrlStatsWorker` ([src/url_stat.cpp](src/url_stat.cpp))
  left as soon as the agent was exiting, abandoning up to a tick of entries in
  its shard queues; the stats worker's shutdown flush, which runs after that
  thread is joined, never saw them. The worker now closes its gate and drains
  the shards once on the way out.

- **`doc/getting_started.md` step 5 now names `SIGTERM`.** The default
  disposition kills the process before `install_atexit_shutdown` or anything
  else can run, so a host under Kubernetes or systemd must route the signal
  to a normal exit. The rationale for shipping no signal helper is unchanged.

- **An overflowed span event no longer injects the previous overflow's host.**

  The one `DisabledSpanEvent` a span shares among its overflowed events kept
  the last `SetDestination()` forever. After the stack came back under the
  limit, a later overflow that recorded no destination injected that stale
  host into `Pinpoint-Host`, drawing a server-map edge to a node the request
  never called. The destination is cleared when the last outstanding overflow
  ends ([src/span.cpp](src/span.cpp)).

- **Exception chain links dropped by the per-span cap are counted and
  reported.** `SpanData::addException` refused silently past 100 links;
  `EndSpan` now logs (throttled) how many links the cap cut and on which
  operation.

- **The 1 MiB SQL cap is re-measured on the normalized text.** Literals become
  indexed placeholders (`1` -> `0#`), so a statement dense with short literals
  grows past the cap it passed on input; `AgentImpl::prepareSql` now drops it
  whole instead of caching and queueing an over-cap key.

- **The metadata id sequence no longer wraps into negative ids.** Past
  `INT32_MAX` the id caches latch, log once, and hand out id 0 as a hit so no
  metadata is enqueued and no span points at another entry's text
  ([src/cache.h](src/cache.h)). Entries minted before the wrap keep their ids.

- **Every call-stack `SetError()` is now its own exception chain.** The
  span-wide chain sent every link under one `exceptionId` at depth 0, which
  the collector cannot order. Each recorded exception now gets its own id and
  its own `ANNOTATION_EXCEPTION_ID`, and asks the
  `CallstackTraceNewThroughput` limiter on its own; a refusal is no longer
  latched for the rest of the span.

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
  from the queue and how many were never sent. `GrpcSpan::droppedSpans()`
  exposes the same total.

- **`Http.Server.ProxyHeaderEnable` switches proxy header recording off.**
  The three built-in parsers used to run on every request even with
  `ProxyUserHeaderNames` empty. Default `true`, reloadable.

- **An unparseable `Pinpoint-pSpanID` is warned about like an unparseable
  `Pinpoint-SpanID`** (throttled). It used to be silent.
