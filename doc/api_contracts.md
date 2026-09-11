# Pinpoint C++ Agent — Span, SpanEvent, and Annotation Contracts

The API contracts enforced by the span, span event, and annotation
implementations, shared by the C++ API ([instrument.md](instrument.md)) and the C
API ([instrument_c.md](instrument_c.md)) — see the
[name mapping](instrument_c.md#8-usage-cautions-span-spanevent-and-annotation-contracts)
for the C spellings.

Most violations are detected at runtime and degrade to a **logged no-op** rather
than a crash — but they distort traces, and the threading and lifetime rules
below are hard requirements that can crash the process if broken. Treat the
warning messages quoted here as instrumentation bugs when they appear in the
agent log.

---

## 1. A Span Is Single-Threaded

A `Span` instance — including every `SpanEvent` it hands out — must be used by **one thread only** for its entire lifetime. Nothing inside a span is locked (the event stack, string fields, annotation lists), so concurrent calls on the same span are undefined behavior and can corrupt memory or crash.

- The agent binds a span to the first thread that calls `NewSpanEvent()` and logs an error (plus an `assert` in debug builds) when another thread touches it afterwards: `span accessed from another thread`. `GetSpanEvent()` and `RecordSpanEvent()` are checked and bind the same way — they read the same event stack.
- Because binding is lazy, a **complete handoff** is allowed — but only *before* the span records anything: the owning thread is fixed by the **first `NewSpanEvent()`/`RecordSpanEvent()` call**, so create the span on thread A, pass the `SpanPtr` to thread B before any event is created, and never touch it from A again (the thread examples in [instrument.md §10](instrument.md#10-asynchronous-and-background-work) rely on this). Once the first event exists, the span belongs to that thread for good; handing it on afterwards logs `span accessed from another thread`.
- To trace work that runs **concurrently** with the parent, do not share the span. Call `NewAsyncSpan()` *on the span's owning thread* and hand the returned child span to the worker; the child follows the same single-thread rule on its own thread.

## 2. End Exactly Once, and Record Before Ending

`EndSpan()` and `EndEvent()` are terminal:

- A duplicate `EndSpan()`/`EndEvent()` logs `span (event) is already finished` and does nothing.
- After the end call, **every recording method** on that object becomes a warning no-op: property setters, `SetError`, `RecordHeader`, `SetSqlQuery`, `InjectContext`, and `SetAnnotation()`. The data may already be in flight on the agent's gRPC worker thread, so nothing can be added afterwards. Record status codes, errors, and annotations **before** calling `EndSpan()`/`EndEvent()`.
- A span released without `EndSpan()` is **never sent** — its data is lost. The destructor only cleans up internal bookkeeping; it does not submit the span.
- `RecordSpanEvent()` (batch replay, for wrappers that time their events themselves) returns an **open** event even though the call already supplies both timestamps: annotations, SQL, errors, `SetDestination()`, `SetEndPoint()` and `SetNextSpanId()` are applied to the returned handle, so it must be closed with `EndEvent()` before the next `RecordSpanEvent()` or `EndSpan()`. Skipping that end holds one depth slot until `EndSpan()` auto-closes it (§3), which walks a long-lived span toward `MaxEventDepth` overflow.

This is why RAII guards are the recommended pattern:
`helper::ScopedSpanEvent` for events (see
[instrument.md §4](instrument.md#4-recording-span-events)), and for spans a guard
of your own:

```cpp
class SpanGuard {
public:
    explicit SpanGuard(pinpoint::SpanPtr span) : span_(std::move(span)) {}
    ~SpanGuard() { if (span_) span_->EndSpan(); }
private:
    pinpoint::SpanPtr span_;
};

void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("Service", "/endpoint");
    SpanGuard guard(span);

    // Even if an exception is thrown, the span is ended
    processRequest();
}
```

## 3. End Span Events in Nesting (LIFO) Order

Span events form a stack. Calling `EndEvent()` on an outer event while an inner event is still open implicitly finishes every event nested above it and logs `span event ended out of order`. Likewise, `EndSpan()` force-finishes all still-open events and logs `N span event(s) not ended by user code`. The trace survives, but implicitly finished events get the wrong end time — their duration silently stretches to the enclosing end call.

**Unbalanced end policy (differs from the Java agent).** An `EndSpan()` that arrives with events still on the stack is not treated as a fatal inconsistency: every open event is **auto-closed at the `EndSpan()` timestamp, kept in the final chunk, and the span is sent as usual**. The Java agent does the opposite — `DefaultTrace.close()` dumps the call stack and **discards the span entirely** when the stack is not empty — so a C++ transaction that a Java agent would drop still shows up in Pinpoint, with the unbalanced events' durations stretched to the span end. Treat the `N span event(s) not ended by user code` warning as an instrumentation bug to fix, not as a lost span. An async child span is the one legitimate case: its root event stays open until `EndSpan()` by design and is excluded from that count.

## 4. `SpanEventPtr` Is Non-Owning

`SpanEventPtr` is a raw pointer whose object is owned by the parent span's internal data:

- It stays valid only while you hold the parent `SpanPtr`. Calling into an already-ended event while the span is alive is a safe warning no-op.
- Do not cache these pointers in long-lived structures. Obtain them, use them, and let them go within the span's scope. **Once the last `SpanPtr` is released, every `SpanEventPtr` it handed out is dangling** — including the shared disabled event of §5 and an unsampled span's event (§11), both of which have exactly the same lifetime as a real event's.
- A call that does slip past the span's release is handled defensively rather than crashing *when the event object itself is still alive* (the agent keeps ended events alive while a chunk is in flight): it logs `span event outlived its span` and records nothing, and `InjectContext()` falls back to writing only `Pinpoint-Sampled: s0` — no valid trace context can be built without the span, so the downstream service is told not to trace instead of continuing a broken one. This is a backstop for a contract violation, not a supported pattern: the release order is not yours to observe, so treat a dangling `SpanEventPtr` as a use-after-free.

## 5. Event Depth and Count Limits (Overflow)

Per span, event nesting depth is capped by `Span.MaxEventDepth` (default 64) and the total event count by `Span.MaxEventSequence` (default 5000). Depth is 1-based, and the allowance is `MaxEventDepth + 1` nesting levels — with `MaxEventDepth: 3` the events at depth 1, 2, 3 and 4 are recorded and the fifth nesting level overflows. The off-by-one is Java's, matched deliberately: `DefaultCallStack.isOverflow()` compares `maxDepth < index` against the element count taken *before* the push, so the push landing at depth `maxDepth + 1` is still admitted (`CallStackTest.overflow()` pins it — the fourth `push()` at `maxDepth = 3` returns `4` and the event it stored pops back non-null). The **sequence** cap has no such offset on either agent — exactly `MaxEventSequence` events are recorded. When either cap is exceeded, `NewSpanEvent()` logs `span event maximum depth/sequence exceeded` and returns a shared **disabled event** instead:

- It records nothing — operation name, timings, SQL, error strings, exception call stacks and annotations are all discarded.
- `SetError()` on it is the one exception: nothing about the error is recorded, but it still **fails the transaction** (§9) — the depth limit bounds what is recorded, not whether the transaction failed.
- `InjectContext()` **still writes the full trace context**, so downstream services continue the distributed trace. Overflow limits profiling detail; it is not a sampling decision.
- You must still call `EndEvent()` exactly once for each overflowed `NewSpanEvent()` call — the span balances an internal overflow counter with it.
- The disabled event is a single shared object per span, so `SetDestination()` values from interleaved overflowed calls can bleed into each other's `Pinpoint-Host` header. **This differs from the Java agent**, where `SpanEventFactory.disableInstance()` hands back a new `DisableSpanEvent` per call and each one therefore carries its own destination. Concretely: with two nested overflowed events, the inner one's `SetDestination("db:3306")` is what the outer one's later `InjectContext()` writes as `Pinpoint-Host`. It affects only that header on calls made past the depth limit — nothing is recorded at that depth either way — so if it matters, raise `Span.MaxEventDepth` rather than working around it.
- Its lifetime and its post-span behavior match a real span event's exactly (§4): it is owned by the same span data, not by the span object, and after the span is gone `EndEvent()` is a no-op and `InjectContext()` writes only `Pinpoint-Sampled: s0`.
- `NewAsyncSpan()` called while the span is overflowed returns a no-op span.

If the overflow warning appears regularly, create fewer, coarser span events per transaction or raise the limits in the configuration.

## 6. `GetSpanEvent()` Returns the Innermost Active Event

`GetSpanEvent()` returns the top of the event stack: the most recently created event that has not ended. It never returns null — a span with no active event returns a shared no-op event and logs `abnormal span - has no event`; a finished span returns the same no-op event but logs `span is already finished`; an overflowed span returns the disabled event without logging. Do not assume it refers to a specific event you created earlier; in helper functions, prefer passing the `SpanEventPtr` returned by `NewSpanEvent()` explicitly.

## 7. Annotation Rules

- The annotation list is **sealed** when its owner ends (`EndEvent()`/`EndSpan()`). A later `SetAnnotation()` logs a warning and does nothing.
- String views are **consumed and copied during the call** and do not need to outlive it. No annotation payload string is materialized for a no-op, unsampled, or already-ended span/event.
- `SetAnnotation()` never throws; on allocation failure the annotation is dropped with an error log.
- There is no key de-duplication: recording the same key twice records two annotations.
- Every annotation byte is copied into the span and shipped to the collector — keep annotations small and sanitized (see [instrument.md §5](instrument.md#5-annotations)).

## 8. Keep Operation and Error Names Low-Cardinality

The `operation` passed to `NewSpan()`/`NewSpanEvent()`/`NewAsyncSpan()` and the `error_name` passed to `SetError()` are interned in bounded LRU caches, and **every new unique string enqueues a metadata message to the collector**. Per-request unique names churn the cache and flood the collector with metadata:

```cpp
// DON'T: unique operation name per request
auto se = span->NewSpanEvent("getUser-" + user_id);

// DO: fixed operation name, variable data as an annotation
auto se = span->NewSpanEvent("getUser");
se->SetAnnotation(CUSTOM_USER_ID, user_id);
```

The `rpc_point` argument of `NewSpan()` is not interned — it may safely carry the actual request path.

## 9. Error Recording and Exception Buffering

- `MarkError(name, message)` is the verdict-only counterpart to `SetError()`
  for language bindings that detect event overflow before native event
  creation. It applies `Span.IgnoreErrors` and the exception category mask to
  the trace root (or to an unsampled span's URL-stat failure flag) but creates
  no error string, `exceptionInfo`, exception metadata, call stack or event.
  Application instrumentation should normally use `SetError()`; `MarkError`
  exists to preserve `DisabledSpanEvent::SetError` semantics across a batched
  binding.
- `SetError()` on the **span** marks the whole transaction as failed. `SetError()` on a **span event** records the error on that step **and also marks the transaction as failed** (`PSpan.err`, URL stat failed histogram) — the same as Java, where every `recordException` is OR-ed into the shared error code. `PSpan.err` is a mask of error *causes*, not a boolean: an exception sets bit `2`, a failing status code bit `4`, a `Sql.ErrorCount` overflow bit `8`, and they accumulate. `Span.ErrorMark` / `Span.ErrorMarkExclude` decide which causes are allowed to fail the transaction (see [config.md](config.md)). An error on an **async child span** fails the **trace root**, not just that child: only the root's `PSpan` carries `err` on the wire, so the flag is shared down the whole async chain (Java's `TraceRoot.getShared().maskErrorCode()`). The one case it cannot reach is a child that ends *after* the root already sent its final chunk — the `PSpan` is serialized once, at root end. Java's ordinary `DefaultTrace` does the same: `close()` calls `logSpan()`, which stores the `PSpan` right there (`DefaultTrace.java:181-199`), and that is the trace every normal entry point builds (`DefaultBaseTraceFactory.java:191`). Deferring the store until the last async child ends exists only on the `AsyncDefaultTrace` path (`AsyncDefaultTrace.java:24-31` awaits; `SpanAsyncStateListener.java:59` stores), whose entry points are the vert.x-only ones marked `@InterfaceAudience.LimitedPrivate("vert.x")` (`DefaultBaseTraceFactory.java:148,161`) — so this is the same design as Java, not a gap against it. There is no way to record a step error without failing the transaction; use an annotation for informational failures.
- Every path into `SetError()` fails the transaction, including the ones that record nothing else: an **overflowed** span event (§5) marks the trace root without keeping the error, and an **unsampled** span (§11) marks its URL stat entry as failed. The overflow case matches Java: `DefaultTrace.traceBlockBegin0` hands out a regular `WrappedSpanEventRecorder` over a `DisableSpanEvent`, and its `recordException` reaches `AbstractRecorder` → `SimpleErrorRecorder`, which masks the shared error code. The unsampled **span** matches too (`DisableSpanRecorder.recordException`). The unsampled span **event** is a deliberate step beyond Java, whose `DisableSpanEventRecorder.recordException` is a no-op: with sampling on, unsampled requests are the majority, and ignoring their step errors would bias the URL stat failure rate toward zero (see [Java parity](java_parity.md#error-on-an-unsampled-span-event--exceeds-java)).
- `Span.IgnoreErrors` is applied on all of them, so an excluded error never fails a transaction regardless of which path recorded it.
- The call-stack overload `SetError(name, message, CallStackReader&)` exists only on `SpanEvent`, and records frames only when `EnableCallstackTrace: true` is set in the configuration (default `false`).
- The error **message** is abbreviated to 256 bytes, matching the Java agent's `StringUtils.abbreviate(message, 256)`: a longer message is cut at the last whole UTF-8 character within 256 bytes and gains a `...(<original length>)` suffix. Put anything longer (a full SQL statement, a response body) in its own annotation, not in the error message.
- At most **100 exceptions with call stacks are buffered per span**; further ones are dropped, counted, and reported once at `EndSpan()` (a throttled warning naming the operation and the number of dropped entries). The buffer does not shrink before `EndSpan()`, so once it is full every later exception on the span is dropped. Buffered exceptions are transmitted only at `EndSpan()` — a span kept open for a very long time delays them and grows memory. There is no per-chain depth cap (Java's `profiler.exceptiontrace.max.depth`, Go's `Error.MaxChainDepth`): every chain is one entry, so the span cap is the only bound (see [Java parity](java_parity.md#per-span-exception-buffer-cap--declined)).
- **Every call-stack `SetError()` is its own exception chain of one entry.** Each call buffers one exception with a fresh `exceptionId` at `PException.exceptionDepth` 0 and stamps `ANNOTATION_EXCEPTION_ID` on the event that recorded it. Java and Go tell a cause from an unrelated exception by throwable identity and number the causes 0, 1, 2 …; this agent has no throwable identity, so it neither joins calls into one chain nor numbers them — two entries under one id at the same depth would leave the collector no way to order them. The cost: the same exception recorded on a nested event and again on the event that catches it is two chains, each asking the `CallstackTraceNewThroughput` limiter (Java charges once for a `CONTINUED` chain, and once per unrelated exception exactly as here). A call stack the limiter refused is a plain error (`exceptionInfo`, failed transaction) without its frames; the next call asks again. See [Java parity](java_parity.md#exception-chain-scope-and-depth--one-entry-per-exception).
- On the wire, `PException.exceptionClassName` is the `name` argument of `SetError(name, message, ...)`; only a call stack recorded without a name falls back to the top frame's module. `PException.startTime` is the **span event's start time** (including one overridden with `SetStartTime()`), not the moment the frames were collected.

## 10. Clock and `SetStartTime()` Caveats

Elapsed times travel as **int32 milliseconds** on the wire. If you override timestamps with `SetStartTime()`:

- Only pass values derived from `std::chrono::system_clock::now()` taken at the actual start of the operation.
- A start time more than ~24.8 days in the past overflows the elapsed field; a start time in the future is clamped to an elapsed of 0 at end time, but inter-event offsets within a chunk can still wrap.
- A fabricated `time_point` (e.g. built from epoch **seconds** interpreted as milliseconds) produces wrapped, meaningless timings.

The C API's `start_time_ms` arguments take **milliseconds** since the Unix epoch. Passing seconds (e.g. `time(NULL)`) is not validated: the computed deltas overflow int32 and silently corrupt the trace timeline.

## 11. Noop and Unsampled Spans Are Deliberately Silent

`NewSpan()` never returns null. When the agent is disabled, not started or not yet registered with the collector (registration is retried indefinitely and tracing stays off — with no statistics collected either — until it succeeds; see [Verifying Agent Startup](trouble_shooting.md#verifying-agent-startup)), the URL/method is excluded by filters, or sampling rejects the transaction, you receive a no-op or unsampled span on which every call succeeds and records nothing:

- `IsSampled()` returns `false`, `GetTraceId()` returns an empty string, and `GetSpanId()` returns 0 for no-op spans (unsampled spans do carry a real span id).
- An unsampled span is never sent, but it still feeds the response-time and **URL statistics**. `SetError()` on it, or on its span event, marks that URL stat entry as failed — otherwise the failure rate would be biased toward zero, since unsampled requests are the majority once sampling is on. Nothing else about the error is kept.
- **The injected header set is conditional.** `InjectContext()` writes a header only when it has a value, mirroring the Java agent's `DefaultRequestTraceWriter`; a `TraceContextWriter` must tolerate any subset. `Pinpoint-pServiceName` appears only for `uid.version=v4`, `Pinpoint-Host` only when the event has a destination, `Pinpoint-pAppNamespace` never (cluster namespaces are unsupported — sending an empty one makes a Java receiver with `profiler.cluster.namespace` set restart the trace instead of continuing it), and a dead-span or unsampled event writes only `Pinpoint-Sampled: s0`.
- Use `IsSampled()` to skip *expensive data collection only* — do **not** skip creating span events and calling `InjectContext()` on outbound calls. An unsampled span's event still writes `Pinpoint-Sampled: s0`, which tells downstream services not to trace the request. Skipping the injection makes downstream agents treat the call as a brand-new transaction and sample it, producing broken partial traces.

## 12. Continuing an Inbound Trace Requires Three Headers

`NewSpan()` continues an inbound distributed trace **only when all three of
these are true**:

1. `Pinpoint-TraceID` is present and parses as `agentId^startTime^sequence`.
2. `Pinpoint-SpanID` is present.
3. `Pinpoint-pSpanID` is present.

Anything else — a missing or blank trace id, a trace id that does not parse,
or either id header absent — starts a **new transaction**: a locally generated
trace id, a generated span id, no parent span id, and the *new*-trace sampler
(plus `Sampling.NewThroughput`) deciding it. Nothing is dropped and nothing is
guessed; the request simply becomes the root of its own trace. This matches the
Java agent's four ordered checks in `DefaultTraceHeaderReader.java:54-70`.

Only the **presence** of the two id headers is checked, not their contents: a
present-but-unparseable `Pinpoint-SpanID` still describes a hop, so the trace
is continued and only that one id is regenerated (a warning is logged,
throttled). A *missing* header is a different thing — a hop that was never
described at all.

Two consequences for a `TraceContextReader` implementation:

- **A carrier that forwards only `Pinpoint-TraceID` breaks the trace.** Some
  proxies, gateways and hand-written clients strip the rest. Such a request is
  recorded as its own transaction, so the call appears as two traces in the UI
  rather than one — nothing is lost, but the link is. `InjectContext()` always
  writes all three, so an all-Pinpoint call chain is unaffected.
- `Pinpoint-Sampled: s0` is checked **before** any of this and short-circuits
  everything (unsampled span, no sampler consulted). `Pinpoint-Flags` takes
  part in no decision and defaults to `0` when absent.

### Key rules for the header-map `NewSpan()` overload

`NewSpan(operation, rpc_point, method, const std::map<std::string, std::string>&)`
wraps the map in the agent's own `TraceContextReader`, and that adapter keeps
the contract `tracer.h` places on HTTP-backed readers:

- **Keys are matched case-insensitively.** The canonical names
  (`Pinpoint-TraceID`, `HEADER_TRACE_ID`, ...) are the fast path — an exact map
  lookup. Any other spelling, including the all-lowercase form HTTP/2 and
  HTTP/3 deliver (`pinpoint-traceid`), falls back to a linear scan of the map
  compared case-insensitively. A binding layer may therefore dump the request
  headers as received; it does not have to re-case them.
- **Values are taken verbatim.** Only the key comparison ignores case.
- **A non-empty map with no trace id under any spelling** starts a fresh
  transaction exactly as an empty map does, and additionally logs a throttled
  warning, `Pinpoint headers present but unrecognized`, so a binding bug that
  turns every request into its own trace is visible in the agent log.
- The three-header rule above applies unchanged; case-insensitive matching
  changes how a header is *found*, not which headers are *required*.
