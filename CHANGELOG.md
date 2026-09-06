# Changelog

## Unreleased

### Breaking

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
