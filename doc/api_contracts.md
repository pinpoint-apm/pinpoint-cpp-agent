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
- `RecordSpanEvent()` (batch replay, for wrappers that time their events themselves) is the one exception: the event it returns is **already finished**, because the call supplies both timestamps. Record everything through its arguments — setters on the returned handle are warning no-ops — and do not call `EndEvent()` on it, which would log `span event is already finished`.

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

Per span, event nesting depth is capped by `Span.MaxEventDepth` (default 64) and the total event count by `Span.MaxEventSequence` (default 5000). Depth is 1-based and the cap is **inclusive** — with `MaxEventDepth: 3` the events at depth 1, 2 and 3 are recorded and the fourth nesting level overflows. This is **one level shallower than the Java agent**: `DefaultCallStack.isOverflow()` compares `maxDepth < index` against the element count taken *before* the push, so `maxDepth = 3` there still admits the fourth push and marks it at depth 4 (`CallStackTest.overflow()` pins that: the fourth `push()` returns `maxDepth + 1` and the event it stored pops back non-null). Java's effective allowance is therefore `MaxEventDepth + 1`. The **sequence** cap has no such offset — both agents record exactly `MaxEventSequence` events. When either cap is exceeded, `NewSpanEvent()` logs `span event maximum depth/sequence exceeded` and returns a shared **disabled event** instead:

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

- `SetError()` on the **span** marks the whole transaction as failed. `SetError()` on a **span event** records the error on that step **and also marks the transaction as failed** (`PSpan.err = 1`, URL stat failed histogram) — the same as Java, where every `recordException` is OR-ed into the shared error code. An error on an **async child span** fails the **trace root**, not just that child: only the root's `PSpan` carries `err` on the wire, so the flag is shared down the whole async chain (Java's `TraceRoot.getShared().maskErrorCode()`). The one case it cannot reach is a child that ends *after* the root already sent its final chunk — the `PSpan` is serialized once, at root end, in Java too. There is no way to record a step error without failing the transaction; use an annotation for informational failures.
- Every path into `SetError()` fails the transaction, including the ones that record nothing else: an **overflowed** span event (§5) marks the trace root without keeping the error, and an **unsampled** span (§11) marks its URL stat entry as failed. Java behaves the same way in both cases (`DisableSpanEventRecorder`, `DisableSpanRecorder.recordException`).
- `Span.IgnoreErrors` is applied on all of them, so an excluded error never fails a transaction regardless of which path recorded it.
- The call-stack overload `SetError(name, message, CallStackReader&)` exists only on `SpanEvent`, and records frames only when `EnableCallstackTrace: true` is set in the configuration (default `false`).
- The error **message** is abbreviated to 256 bytes, matching the Java agent's `StringUtils.abbreviate(message, 256)`: a longer message is cut at the last whole UTF-8 character within 256 bytes and gains a `...(<original length>)` suffix. Put anything longer (a full SQL statement, a response body) in its own annotation, not in the error message.
- At most **100 exceptions with call stacks are buffered per span**; further ones are dropped. The buffer does not shrink before `EndSpan()`, so once a link of one event's chain is dropped the rest of that chain is dropped with it, rather than being sent without the head it belongs under. Buffered exceptions are transmitted only at `EndSpan()` — a span kept open for a very long time delays them and grows memory.
- Every call-stack `SetError()` on the **same span event** is treated as one cause chain, matching the Java agent: the recorded exceptions share a single `exceptionId`, the annotation carrying that id is written once, and `PException.exceptionDepth` counts up **from 0** in record order (a single exception is depth 0). Call stacks recorded on *different* span events are independent exceptions, each with its own id and depth 0.
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
