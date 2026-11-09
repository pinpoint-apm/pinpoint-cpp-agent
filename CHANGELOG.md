# Changelog

## Unreleased

### Breaking

- **The next release is 2.3.0, with shared-library ABI version 2.3.0.** Adding
  `cluster_namespace` to the public, by-value `SpanConfigSnapshot` changes
  its layout. A caller compiled against 2.2.0 allocates a smaller return
  buffer, so loading the new implementation into that caller corrupts memory.
  CMake now produces `libpinpoint_cpp.so.2.3.0` (macOS:
  `libpinpoint_cpp.2.3.0.dylib`), with a distinct loader identity.
  Rebuild applications, plugins and language bindings against the new headers
  and library together; static consumers also need a full rebuild. Keep the
  original ABI-2 library for binaries that have not migrated. Never alias the
  new library under its old ABI-2 name.

### Added

- **An unset `AgentName` is auto-generated as `<hostname>-<pid>`.** It used to
  fall back to the agent id, a random Base64 UUID that told an operator
  nothing; now the Pinpoint UI shows which host and process each agent is.
  Hostname characters outside `[a-zA-Z0-9._-]` become `_`, and the hostname is
  cut to fit the 254-character limit. An explicit `AgentName` is unchanged.
  Documented in [Configuration Guide § Agent Configuration](doc/config.md#agent-configuration).

- **`ClusterNamespace`: keep traces apart from another Pinpoint cluster, as
  the Java agent's `profiler.cluster.namespace` does.** When set (env
  `PINPOINT_CPP_CLUSTER_NAMESPACE`), outbound calls send it as
  `Pinpoint-pAppNamespace`, and an inbound request carrying a different one
  (an empty one included) starts a new trace instead of continuing the
  caller's; a request without the header still continues. Unset, nothing
  changes: no header is sent and none is checked. Not reloadable. Documented
  in [Configuration Guide § Agent Configuration](doc/config.md#agent-configuration).
  Appended to `SpanConfigSnapshot` as `cluster_namespace`, for binding layers
  that build outbound headers themselves.

- **`Collector.Grpc.Compression`: gzip span batches and the stats stream, on
  by default.** Each `SendSpanBatch` request and each `SendAgentStat` stream is
  gzip-compressed, as the Java agent does
  (`profiler.transport.grpc.span.batch-sender.compression`,
  `profiler.transport.grpc.stat.sender.compression`); gRPC sends a message
  uncompressed when gzip does not shrink it. `none` turns it off (env
  `PINPOINT_CPP_GRPC_COMPRESSION`). A collector that cannot decompress gzip
  answers `UNIMPLEMENTED`; the agent then warns once and sends that data
  uncompressed for the rest of its life, losing only what was already sent
  compressed. Documented in
  [Configuration Guide § gRPC Transport](doc/config.md#grpc-transport-configuration).

### Fixed

- **The stdout log is flushed after each line, as the file log already was.**
  With no `Log.FilePath`, lines went to `std::cout` unflushed, and C stdio
  buffers stdout fully when it is a pipe, as it is under docker and
  kubernetes. Lines waited for 4 KiB of others, so a registered agent showed
  no `success to register the agent` until the container stopped. They also
  went out in 4096-byte writes cut mid-line, which pre-fork workers sharing
  the pipe landed inside, and a worker leaving through `_exit()` lost whatever
  was still buffered. Each line now leaves in one write as it is logged. Hosts
  that set `unitbuf` on `std::cout` to work around this no longer need it.

- **Concurrent C API agent lookups no longer contend on a single handle
  registry shard and reference count.** A bounded set of reader caches spreads
  lookups of the shared agent token across threads. `pt_global_agent()` returns
  its cached token through plain loads, with no lock and no agent reference
  count, until the global agent changes. This holds when no agent is running
  too. Shutdown and replacement invalidate these caches, including those used
  by idle threads, so old agents are released and retained handles still
  behave as disabled agents. A retained handle is remembered as disabled
  beside the live one, so a thread that mixes the two neither allocates nor
  refills the cache on each call.

- **Restarting an init-failed agent cannot deadlock on a repeated shutdown
  from its teardown log sink.** `Shutdown()` now returns before taking the
  global agent lock when shutdown is already in progress, so the teardown
  callback can return while `StartAgent()` holds that lock during replacement.

- **An async span keeps only its trace's error mask and SQL count alive, not
  the whole root span.** A child pointed at its root's span data, so every
  async span still queued or left open kept the ended root's event husks (up
  to `Span.MaxEventSequence` of them, about 1.3 MB at the default), span
  annotations and strings alive. The two fields a child writes now move into
  a small block of their own when the first child is spawned.

- **Operation names, error names and URL-stat patterns are capped in the
  agent's caches.** An operation name over 1024 bytes and an error name over
  256 bytes are now cached, keyed and sent abbreviated (the kept prefix plus
  `...(<length>)`), as SQL already was, so names built from URIs no longer
  make every API cache slot and each thread's lookup cache keep kilobytes. A
  URL-stat pattern over 1024 bytes is kept abbreviated in the queue and the
  tick tables, and `Http.UrlStatLimit` accepts at most `10000` keys per tick
  (an out-of-range value falls back to the default, as a negative one did).
  Documented in [Configuration Guide § URL Statistics](doc/config.md#url-statistics).

- **`RecordSpanEvent()` also stops at the span's own event count and depth.**
  It checked only the sequence and depth the caller passed, so a binding that
  repeated or reset its sequence numbers, or never ended what it recorded,
  could grow a span past `Span.MaxEventSequence` and `Span.MaxEventDepth`.

- **A span batch split over `Collector.Grpc.MaxSendMessageSize` no longer
  allocates an unused 64 KiB block per request.** Protobuf allocates an
  arena's first block when it is constructed with non-default options, and
  every further request of a split batch built one it never used.

- **A span batch split over `Collector.Grpc.MaxSendMessageSize` waits for
  in-flight permits once, not once per request.** Each further request of a
  split batch waited the whole permit timeout again, so while the collector
  held every permit a batch split into N requests blocked N + 1 times as
  long: the shutdown drain ran that far past its budget (a process exit
  behind `install_atexit_shutdown` waited it out), and the span worker
  stalled for N + 1 flush intervals. The requests of a batch now share one
  deadline.

- **A per-thread cache rebuilt from a `thread_local` destructor during thread
  exit is freed with the thread.** Such a call (a host recording a final span
  at thread exit) got a replacement object that stayed allocated until the
  process ended, one per exiting thread; a thread-specific key now reclaims
  it after the thread's other destructors.

- **`pt_global_agent()` hands out one library-owned handle per agent, so a
  missed `pt_agent_destroy()` no longer leaks.** Each call used to register a
  fresh owning handle, and the documented handlers call it once per request,
  so a handler that skipped `pt_agent_destroy()` on one return path leaked a
  wrapper per request (and pinned the agent past a restart). Every call made
  while the same agent is global now returns the same handle, and
  `pt_agent_destroy()` is a no-op kept for compatibility. Once the agent is
  shut down or replaced the library releases it; a handle a caller kept
  behaves as the disabled agent, as a handle to a shut-down agent always did.
  Documented in [C API § Handle lifetime rules](doc/instrument_c.md#handle-lifetime-rules).

- **A shut-down agent no longer keeps its collector connections open.** The
  ping and stats streams kept their last finished `ClientContext`, and the
  metadata sender kept the requests that completed while it was stopping;
  each pins its gRPC channel, so an agent still referenced after `Shutdown()`
  (a live span, a host `AgentPtr`) kept two or three TCP connections open
  with keepalive pings, plus unsent metadata, until it was destroyed. They
  are released when each worker stops, and so is a URL-stat message whose
  shutdown flush timed out.

- **Restarting the agent on a long-lived thread pool no longer pins one old
  runtime per thread per restart.** Each reader thread caches the agent's
  config snapshot by holder address, and a replacement agent lands at a new
  address, so every `Shutdown()` + `StartAgent()` cycle left the old
  `AgentRuntime` (its config, stats and URL-stat tables, tens of KB) pinned by
  every thread that had traced. A thread now frees entries of destroyed agents
  the first time it loads the new one.

- **Replacing an agent whose asynchronous initialization failed shuts it down
  first.** `StartAgent()` only dropped its own reference, so when the host
  still held an `AgentPtr` the failed agent's channels, AgentInfo sender,
  already-started workers and config watcher kept running beside the new
  agent, and an enabled watcher kept reconfiguring the shared logger.

- **A span's or span event's annotation list is capped at 1024 entries.** A
  span's own annotations are sent only with its final chunk, and an event's
  only when it ends, so a long-lived span annotating per message, or an event
  left open around a `SetSqlQuery()` loop, grew until it ended — tens of
  megabytes per million calls, for a PSpan the collector limit then dropped
  anyway. Later appends are dropped with a throttled warning. Documented in
  [API contracts § Annotation Rules](doc/api_contracts.md#7-annotation-rules).

- **A span batch over `Collector.Grpc.MaxSendMessageSize` is sent in several
  requests, not lost.** gRPC refuses a request over the limit with
  `RESOURCE_EXHAUSTED` before sending it, and the collector refuses one over
  its own inbound limit, so an oversize batch lost every span in it, up to
  `Collector.SpanBatch.Size` of them, with only a throttled
  `SendSpanBatch failed` warning. The sender now measures each batch and
  sends one over the limit in as many requests as it takes; only a span chunk
  too large for any request is dropped, counted, and named in a warning. The
  SQL parameters a span event carries, the one span field with no bound of its
  own, are dropped whole past 64 KiB (the statement keeps its id, its count
  and its bind values and is shown with its placeholders unfilled), so a chunk
  of bulk-insert events stays sendable. Documented in
  [Configuration Guide § gRPC Transport](doc/config.md#grpc-transport-configuration)
  and [§ SQL Configuration](doc/config.md#sql-configuration).

- **`SpanEvent::SetOperationName()` (and `pt_span_event_set_operation_name()`)
  renames the event on the wire.** An event created with a name sent the api
  id of that first name, so a later rename never reached the collector and
  Pinpoint Web kept showing the original operation. The new name now replaces
  the api id, as the Java agent's `recordApi()` does; the last call wins. An
  async span's event keeps its `API_TYPE_INVOCATION` when renamed, an empty
  name is ignored, and the new name is interned like the one passed to
  `NewSpanEvent()`, so it must be low-cardinality too
  ([API Contracts § 8](doc/api_contracts.md#8-keep-operation-and-error-names-low-cardinality)).
  An async span's event whose operation could not be interned now falls back
  to sending the name, as every other event does, instead of sending none.

- **`Log.Level` and `Sampling.Type` ignore surrounding whitespace.** A value
  carrying a stray space or tab from a shell or a template, such as
  `PINPOINT_CPP_SAMPLING_TYPE="PERCENT "` or a quoted YAML `" debug"`, read as
  unknown: sampling fell back to `COUNTER` and the log level stayed where it
  was, each with a warning. Both are now trimmed before they are matched, as
  `UidVersion` already was and `Collector.Grpc.Compression` is. Documented in
  [Configuration Guide § Logging Configuration](doc/config.md#logging-configuration)
  and [§ Sampling Configuration](doc/config.md#sampling-configuration).

## 2.2.0 (2026-10-23)

### Breaking

- **`UidVersion` defaults to `v4`, as in the Java agent.** An unset
  `UidVersion` used to resolve to `v3`; it now resolves to `v4`, the value the
  Java agent's `NameVersion` falls back to and ships as its default, and an
  unknown value (`V5`, `v2`) also falls back to `v4` instead of `v3`, still
  with its warning. Because `v4` requires a `ServiceName`, **a deployment that
  sets neither `UidVersion` nor `ServiceName` no longer starts**:
  `StartAgent()` logs
  `Failed to resolve ServiceName (required for uid.version=v4, max length 254)`
  and installs a noop agent, exactly as a misconfigured explicit `v4` did.

  **Migration.** Do one of:

  - Set `ServiceName` (YAML key `ServiceName`, or `PINPOINT_CPP_SERVICE_NAME`)
    to register under that service with the v4 identity
    (`protocol.version=400`).
  - Or pin the previous identity with `UidVersion: v3` (or
    `PINPOINT_CPP_UID_VERSION=v3`); nothing else about a v3 agent changes.

  The YAML handed to `ReloadAgentConfig()` is resolved as a complete
  configuration, so it needs the same key: a reload YAML that names neither
  `UidVersion` nor `ServiceName` is now refused like a startup configuration
  would be. A config-file watcher reload is unaffected (an omitted key keeps
  its running value).

  The sample configurations in [Getting Started](doc/getting_started.md) and
  the examples now set a `ServiceName`. See
  [Identity Versions](doc/config.md#identity-versions).

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

- **`AgentOptions::agent_version`: override the agent version reported in
  AgentInfo.** Empty (the default) reports this library's own version. C
  counterpart: `pt_agent_options_set_agent_version()`.

### Changed

- **`ApiKey` is optional for `UidVersion: v4`.** A v4 agent without an
  `ApiKey` previously refused to start. It now resolves its identity with an
  empty key and omits the gRPC `apikey` header, as the Java agent does; the
  header is sent only when a key is configured. `ServiceName` is still
  required. See [Identity Versions](doc/config.md#identity-versions).

- **Identity values are trimmed before validation.** `ApplicationName`,
  `AgentName`, `ServiceName` and `ApiKey` lose their leading and trailing
  whitespace before the charset and length checks, as the Java agent does
  (`AgentProperties.getProperty`). A value such as `ServiceName: "svc "`,
  which used to fail the `[a-zA-Z0-9._-]` check and abort a v4 startup, now
  resolves to `svc`; a whitespace-only `ApiKey` counts as unset and the
  `apikey` header is omitted.

- **An unknown `UidVersion` logs a warning.** The value still falls back to
  the default, `v4`, but a typo (`V5`, `v 4`) is now named in the log instead
  of silently registering the agent under the default. An empty value stays
  the documented default and does not warn. Surrounding whitespace in the
  value is ignored.

- **The non-reloadable-change warning names the fields.** A reload that tries
  to change a non-reloadable option now logs `non-reloadable config fields
  changed at runtime: ApplicationName, Collector.Host; ...` instead of a fixed
  list of categories, as [the reload docs](doc/config.md#configuration-hot-reload)
  already said it did.

- **The SQL id cache keys a statement by its 128-bit UID, not by its text.**

  The id cache kept the whole normalized statement as its key and is exempt
  from `Sql.CacheLengthLimit` (a bypassed statement would burn a fresh id per
  use), so `Sql.CacheSize` statements of up to the normalizer's 1 MiB cap could
  pin up to a gigabyte for the life of the process. The key is now the
  statement's SQL UID (MurmurHash3-128 of the normalized text, the same bytes
  the UID metadata carries): 16 bytes per entry whatever the statement's
  length, so 1024 statements of 64 KiB each retain about 120 KiB instead of
  about 96 MiB. The id and UID a statement resolves to are unchanged. A queued
  SQL metadata item carries that key instead of a hash of the text, and a
  failed send evicts with `remove(key, id)` like every other metadata kind;
  `IdCache::removeByHash()` is gone.

### Fixed

- **A v4 `ServiceName` shorter than 3 characters is rejected at startup.**
  Only the shared `[a-zA-Z0-9._-]` id validation applied, so a two-character
  name resolved and the agent registered under a service the Collector would
  refuse to create. The lower bound the Collector enforces is now checked in
  `resolve_object_name()`, and the startup failure logs
  `Failed to resolve ServiceName (required for uid.version=v4, min length 3)`.
  v1/v3 carry no `ServiceName` and are unaffected.
- **An async span of an unsampled span now propagates the unsampled
  decision.** `NewAsyncSpan()` on an unsampled span returned the no-op span,
  from both overloads and from `pt_span_new_async_span()`. The no-op span's
  events inject nothing, so an outbound call made through that async child
  carried no Pinpoint headers, and the downstream agent started a trace of its
  own and sampled it by itself. An unsampled span now returns an unsampled
  async child whose events write `Pinpoint-Sampled: s0`, as do the events of
  its own async children. The child is not a transaction: it registers no
  active request, records no URL stat or response time, and an error on it
  does not fail the parent's URL stat. Like the no-op span it is one shared,
  stateless instance reporting span id 0, so creating it allocates nothing and
  it may outlive the parent and end on another thread. `RecordSpanEvent()` on an
  unsampled span likewise returns an event that writes `s0`. Sampled spans and
  no-op spans (agent disabled, URL excluded) are unchanged; a no-op span's
  async child still writes nothing. Documented in
  [API Contracts §11](doc/api_contracts.md#11-noop-and-unsampled-spans-are-deliberately-silent).
- **s0-only unsampled propagation no longer logs "Pinpoint headers present
  but unrecognized".** The header-map `NewSpan()` overload warned whenever the
  map carried no `Pinpoint-TraceID`, but an unsampled upstream legitimately
  sends only `Pinpoint-Sampled: s0`, so at 1% sampling 99% of inbound requests
  hit the throttled warning (556 occurrences in a 60-second e2e run). The
  warning now requires that neither `Pinpoint-TraceID` nor `Pinpoint-Sampled`
  is present under any case; the span is handed out as unsampled exactly as
  before.

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

- **`ServiceName` is now required for `UidVersion: v4`.**

  Up to and including v2.0.0, a v4 agent started with no `ServiceName`
  silently registered under the fallback service name `DEFAULT`. That
  fallback is gone: a missing or invalid `ServiceName` aborts agent startup
  (`resolve_object_name()` in [src/object_name.cpp](src/object_name.cpp)).

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
    YAML key `UidVersion: v3`, or `PINPOINT_CPP_UID_VERSION=v3`.

  See [Identity Versions](doc/config.md#identity-versions).

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
  (Superseded: the id cache now keys by the statement's UID and the queued
  item carries that 16-byte key; `removeByHash()` no longer exists.)

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
