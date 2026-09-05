/*
* Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <cassert>
#include <iterator>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "cache.h"
#include "callstack.h"
#include "logging.h"
#include "noop.h"
#include "span.h"
#include "span_event.h"
#include "utility.h"

namespace pinpoint {

    namespace {
        // Numeric values are formatted into the caller's reused scratch
        // buffer (fmt::memory_buffer keeps ~500 bytes inline, far above any
        // int64/double rendering), replacing the previous fresh std::string
        // per numeric argument — this runs per bind value per traced SQL
        // statement when trace_bind_value is on.
        std::string_view sqlBindValueView(
            const SqlBindValue& value,
            fmt::memory_buffer& scratch) {
            return std::visit([&scratch](const auto& bind_value) -> std::string_view {
                using Value = std::decay_t<decltype(bind_value)>;
                if constexpr (std::is_same_v<Value, std::nullptr_t>) {
                    return "null";
                } else if constexpr (std::is_same_v<Value, bool>) {
                    return bind_value ? "true" : "false";
                } else if constexpr (std::is_same_v<Value, std::string_view>) {
                    return bind_value;
                } else {
                    scratch.clear();
                    fmt::format_to(std::back_inserter(scratch), "{}", bind_value);
                    return {scratch.data(), scratch.size()};
                }
            }, value);
        }

        // Injection policy for an event whose span is already gone: the
        // trace and span ids live in the span, so no valid context can be
        // built any more. Tell the downstream not to start a trace of its own
        // instead of writing a half context — the same header an unsampled
        // span writes (see UnsampledSpanEvent::InjectContext).
        void injectDeadSpanContext(TraceContextWriter& writer) {
            writer.Set(HEADER_SAMPLED, "s0");
        }

        std::string joinSqlBindValues(
            const std::vector<SqlBindValue>& bind_args,
            int max_bind_args_size) {
            std::string joined_bind_args;
            if (max_bind_args_size <= 0) {
                return joined_bind_args;
            }

            const auto max_size = static_cast<std::size_t>(max_bind_args_size);
            // Separator and truncation marker both follow Java's
            // BindValueUtils.bindValueToString: values are joined with ", "
            // and a dropped tail is reported as "...(<number of bind values>)"
            // — the count, not the byte limit, so the reader can tell how many
            // values the statement had. This string is display-only (the
            // SQL_BINDVALUE annotation); the placeholders the web substitutes
            // come from the normalizer's parameters, which keep their own
            // separator.
            constexpr std::string_view kSeparator = ", ";
            // Room for the "...(<bind arg count>)" marker: 5 punctuation
            // chars plus at most 20 digits of a size_t.
            constexpr std::size_t kTruncationSuffixMax = 25;

            // The output never exceeds max_size plus a separator and the
            // marker. Reserve that up front, capped so a large configured
            // limit does not allocate more than a typical bind list needs;
            // the rest is left to the string's geometric growth.
            constexpr std::size_t kMaxInitialReserve = 256;
            joined_bind_args.reserve(std::min(max_size, kMaxInitialReserve) +
                                     kSeparator.size() + kTruncationSuffixMax);

            fmt::memory_buffer scratch;
            for (std::size_t i = 0; i < bind_args.size(); ++i) {
                const auto arg = sqlBindValueView(bind_args[i], scratch);
                // Java appends the separator after every value but the last,
                // so it precedes whatever comes next: the value, or the marker
                // that stands in for the values left out.
                if (i != 0) {
                    joined_bind_args.append(kSeparator);
                }
                if (joined_bind_args.size() >= max_size ||
                    arg.size() > max_size - joined_bind_args.size()) {
                    fmt::format_to(std::back_inserter(joined_bind_args),
                                   "...({})", bind_args.size());
                    break;
                }
                joined_bind_args.append(arg);
            }
            return joined_bind_args;
        }
    }

    std::atomic<int64_t> Exception::exception_id_gen{1};

    SpanEventImpl::SpanEventImpl(SpanImpl* span, std::string_view operation) :
        // The span data, not the span: it owns this event and outlives the
        // SpanImpl (see spanIfAlive).
        data_(span->data_.get()),
        agent_(span->agent_),
        service_type_{defaults::SPAN_EVENT_SERVICE_TYPE},
        start_time_{to_milli_seconds(std::chrono::system_clock::now())} {
        assert(data_ != nullptr);
        assert(agent_ != nullptr);

        if (!operation.empty()) {
            api_id_ = agent_->cacheApi(operation, API_TYPE_DEFAULT);
        }
        // Same trade as SpanData: the name is the fallback for an event with
        // no api id, and build_span_event reads it only on that branch. A
        // span event is created per traced call, so this is the copy that
        // repeats most within a single span.
        if (api_id_ <= 0) {
            operation_ = operation;
        }
    }

    SpanImpl* SpanEventImpl::spanIfAlive() const {
        // Nulled by ~SpanImpl. A SpanData still pinned by an in-flight chunk
        // keeps this event (and any raw SpanEventPtr user code holds) alive
        // past its span, so every span access degrades to a logged no-op here
        // instead of a use-after-free.
        auto* span = data_->getOwner();
        if (span == nullptr) {
            LOG_WARN("span event outlived its span");
        }
        return span;
    }

    bool SpanEventImpl::warnIfFinished() const {
        // A finished event may already sit in a chunk being serialized on the
        // gRPC worker thread; handing out the live annotation container, or
        // mutating any field the worker reads, would race that iteration. So
        // every recording accessor and mutator degrades to a safe no-op after
        // the event is finished, mirroring EndEvent()/EndSpan(). This guard
        // is also what makes releaseRetiredPayload() sound: once the chunk is
        // destroyed the payload heap is gone entirely, so no post-finish path
        // may touch those fields — the guard is load-bearing for memory
        // safety, not just race avoidance.
        if (finished_) {
            LOG_WARN("span event is already finished");
            return true;
        }
        return false;
    }

    void SpanEventImpl::SetAnnotation(int32_t key, int32_t value) try {
        if (warnIfFinished()) return;
        annotations_.AppendData(key, AnnotationData(value));
    } CATCH_AND_LOG("set annotation")

    void SpanEventImpl::SetAnnotation(int32_t key, int64_t value) try {
        if (warnIfFinished()) return;
        annotations_.AppendData(key, AnnotationData(value));
    } CATCH_AND_LOG("set annotation")

    void SpanEventImpl::SetAnnotation(int32_t key, std::string_view value) try {
        if (warnIfFinished()) return;
        annotations_.AppendData(key, AnnotationData(value));
    } CATCH_AND_LOG("set annotation")

    void SpanEventImpl::SetAnnotation(int32_t key,
                                      std::string_view value1,
                                      std::string_view value2) try {
        if (warnIfFinished()) return;
        annotations_.AppendData(key, AnnotationData(value1, value2));
    } CATCH_AND_LOG("set annotation")

    void SpanEventImpl::EndEvent() try {
        // Atomic exchange so only the first end proceeds: ending an event
        // twice would pop a DIFFERENT (still-active) event from the span's
        // stack and desync the whole call tree.
        if (finished_.exchange(true)) {
            LOG_WARN("span event is already finished");
            return;
        }
        // Pass the identity so the span can detect (and unwind) out-of-order
        // ends instead of finishing whatever event happens to be on top.
        // A dead span has nothing left to unwind: the event never reaches a
        // chunk, which is what dropping a span without EndSpan means anyway.
        if (auto* span = spanIfAlive()) {
            span->endSpanEvent(this);
        }
    } catch (const std::exception& e) {
        // Like EndSpan: EndEvent runs in host destructors — including this
        // library's own helper::ScopedSpanEvent — where an escaping
        // exception would std::terminate the host, so nothing may escape.
        // `this` is still alive here: storeFinishedEvent never lets an event
        // die on its failure paths (see its catch handler).
        LOG_ERROR("end span event exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("end span event unknown exception");
    }

    void SpanEventImpl::finish() {
        // Ended through an internal path (event-stack pop): mark it so a
        // later user-level EndEvent on this event is rejected by the guard.
        finished_.store(true);
        // Seal the annotation list too: an internal append path that bypasses
        // the finished_ guard above must become a no-op, since the list may
        // be under serialization on the gRPC worker once this event reaches
        // a chunk.
        annotations_.seal();
        data_->decrEventDepth();
        // Batch-replayed events (Span::RecordSpanEvent) carry their real end
        // time, recorded by the wrapper when the event actually ended; only
        // live events fall back to the wall clock.
        const auto end_time = end_time_ != 0
            ? end_time_
            : to_milli_seconds(std::chrono::system_clock::now());
        // system_clock can step backwards (NTP); never report a negative
        // elapsed time. Only the low side is clamped: the wire field is
        // int32 ms, so a delta beyond INT32_MAX ms (~24.8 days — e.g. a
        // user-supplied start time in seconds instead of ms) wraps.
        elapsed_ = static_cast<int32_t>(
            std::max<int64_t>(end_time - start_time_, 0));
    }

    void SpanEventImpl::releaseRetiredPayload() noexcept {
        // Swap-with-temporary, not clear(): clear() keeps the capacity
        // allocated, and releasing that heap is this function's whole point.
        std::string{}.swap(operation_);
        std::string{}.swap(endpoint_);
        std::string{}.swap(destination_id_);
        std::string{}.swap(error_string_);
        // Also drops the aliasing shared_ptr pins that SQL annotations hold
        // on PreparedSql cache entries (see SetSqlQuery), instead of keeping
        // those cache strings alive until the span data dies.
        annotations_.releaseStorage();
    }

    int64_t SpanEventImpl::generateNextSpanId(const int64_t span_id, const int64_t parent_span_id) {
        next_span_id_ = generate_next_span_id(span_id, parent_span_id);
        return next_span_id_;
    }

    void SpanEventImpl::SetOperationName(std::string_view operation) try {
        if (warnIfFinished()) return;
        operation_ = operation;
    } CATCH_AND_LOG("set operation name")

    void SpanEventImpl::SetDestination(std::string_view dest) try {
        if (warnIfFinished()) return;
        destination_id_ = dest;
    } CATCH_AND_LOG("set destination")

    void SpanEventImpl::SetEndPoint(std::string_view endpoint) try {
        if (warnIfFinished()) return;
        endpoint_ = endpoint;
    } CATCH_AND_LOG("set end point")

    void SpanEventImpl::SetError(std::string_view error_message) {
        SetError("Error", error_message);
    }

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message) try {
        if (warnIfFinished()) return;
        error_func_id_ = agent_->cacheError(error_name);
        error_string_ = abbreviateErrorString(error_message);
        // Propagate to the owning span (the async child span for async
        // events) so PSpan.err and the URL stat failure flag see it.
        if (auto* span = spanIfAlive()) {
            span->markSpanError(error_name, error_message);
        }
    } CATCH_AND_LOG("set error")

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) {
        if (warnIfFinished()) return;
        SetError(error_name, error_message);

        auto* span = spanIfAlive();
        if (span == nullptr || !span->config_->enable_callstack_trace) {
            return;
        }

        try {
            auto callstack = std::make_unique<CallStack>(error_message, error_name, start_time_);
            reader.ForEach([&](std::string_view module, std::string_view function, std::string_view file, int line) {
                callstack->push(module, function, file, line);
                return;
            });
            recordException(*span, std::move(callstack));
        } catch (const std::exception& e) {
            LOG_ERROR("call stack trace exception = {}", e.what());
        }
    }

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message,
                                 const std::vector<CallStackFrame>& frames) {
        if (warnIfFinished()) return;
        SetError(error_name, error_message);

        auto* span = spanIfAlive();
        if (span == nullptr || !span->config_->enable_callstack_trace) {
            return;
        }

        try {
            auto callstack = std::make_unique<CallStack>(error_message, error_name, start_time_);
            for (const auto& frame : frames) {
                callstack->push(frame.module, frame.function, frame.file, frame.line);
            }
            recordException(*span, std::move(callstack));
        } catch (const std::exception& e) {
            LOG_ERROR("call stack trace exception = {}", e.what());
        }
    }

    void SpanEventImpl::recordException(SpanImpl& span, std::unique_ptr<CallStack> callstack) {
        // Repeated call-stack SetError calls on one event are the cause chain
        // of a single exception (Java records them under one exceptionId with
        // an increasing depth), so they reuse the id of the first link and are
        // annotated only once — and only the first link is rate limited.
        // A rejected chain keeps the plain error (SetError already ran and
        // marked the span); only the call stack is dropped, which is what
        // Java's DISABLED sampling state does.
        //
        // That verdict is latched for the rest of the chain, because Java's
        // DISABLED state lives in the trace context and every later link
        // reads it back. Asking per link instead would charge one logical
        // chain several times over, and once a token refilled mid-chain it
        // would record a cause link as a brand new chain with its head
        // missing - the half-recorded chain the reuse above exists to avoid.
        if (exception_chain_disabled_) {
            return;
        }
        if (exception_id_ == 0 && !span.allowNewExceptionChain()) {
            exception_chain_disabled_ = true;
            return;
        }
        auto exception = std::make_unique<Exception>(std::move(callstack), exception_id_);
        const auto exception_id = exception->getId();
        if (!span.addException(std::move(exception))) {
            // Buffer full. It does not shrink before EndSpan, so every later
            // link would be dropped too — and latching here is what keeps a
            // dropped first link from letting the next one charge the limiter
            // for a chain that cannot be stored either.
            exception_chain_disabled_ = true;
            return;
        }
        if (exception_id_ == 0) {
            annotations_.AppendLong(ANNOTATION_EXCEPTION_ID, exception_id);
            exception_id_ = exception_id;
        }
    }

    void SpanEventImpl::SetSqlQuery(
        std::string_view sql_query,
        const std::vector<SqlBindValue>& bind_args) try {
        if (warnIfFinished()) return;
        auto* span = spanIfAlive();
        if (span == nullptr) return;
        const auto& config = span->config_;
        const auto mode = config->sql.enable_sql_stats
            ? SqlMetaMode::Uid
            : SqlMetaMode::Id;
        auto prepared = agent_->prepareSql(sql_query, mode);
        if (!prepared || !*prepared) {
            return;
        }
        // One recorded SQL execution for this transaction. Charged where Java
        // charges it (SqlCountService.recordSqlCount, once the SQL annotation
        // is built), so a statement the normalizer rejected is not counted.
        span->countSqlExecution();

        std::string joined_bind_args;
        if (config->sql.trace_bind_value) {
            joined_bind_args = joinSqlBindValues(
                bind_args, config->sql.max_bind_args_size);
        }

        const auto& value = **prepared;
        // Aliasing shares PreparedSql's existing control block; no allocation
        // or parameter-string copy occurs on a raw-cache hit. It also protects
        // the bytes until asynchronous span serialization completes.
        auto parameters = std::shared_ptr<const std::string>(
            *prepared, &value.parameters);

        if (mode == SqlMetaMode::Uid) {
            if (const auto* uid = std::get_if<SqlUid>(&value.identity)) {
                annotations_.AppendData(
                    ANNOTATION_SQL_UID,
                    AnnotationData(*uid, std::move(parameters), std::move(joined_bind_args)));
            }
            return;
        }

        if (const auto* sql_id = std::get_if<int32_t>(&value.identity)) {
            annotations_.AppendData(
                ANNOTATION_SQL_ID,
                AnnotationData(*sql_id, std::move(parameters), std::move(joined_bind_args)));
        }
    } CATCH_AND_LOG("set sql query")

    void SpanEventImpl::RecordHeader(HeaderType which, HeaderReader& reader) try {
        if (warnIfFinished()) return;
        agent_->recordClientHeader(which, reader, &annotations_);
    } CATCH_AND_LOG("record header")

    void SpanEventImpl::InjectContext(TraceContextWriter& writer) try {
        // Guarded like every other mutator: generateNextSpanId() writes a
        // field the gRPC worker reads once this event sits in a chunk. An
        // event can also outlive the SpanImpl itself (SpanData keeps it alive
        // for an in-flight chunk), hence the second guard.
        if (warnIfFinished()) return;
        auto* span = spanIfAlive();
        if (span == nullptr) {
            injectDeadSpanContext(writer);
            return;
        }
        // The ids come from the SpanData (always alive here) rather than the
        // span: injectContext writes this span's id as the callee's parent, so
        // the child id has to clear both of them.
        span->injectContext(writer,
                            generateNextSpanId(data_->getSpanId(), data_->getParentSpanId()),
                            destination_id_);
    } CATCH_AND_LOG("inject context")

    void SpanEventImpl::SetNextSpanId(int64_t next_span_id) try {
        // Same guard as InjectContext: next_span_id_ is read by the gRPC
        // worker once this event sits in a chunk.
        if (warnIfFinished()) return;
        next_span_id_ = next_span_id;
    } CATCH_AND_LOG("set next span id")

    void DisabledSpanEvent::EndEvent() {
        // The shared per-span instance stands in for every overflowed event,
        // so it cannot carry a per-instance finished flag; the span data's
        // overflow counter provides the duplicate-end guard instead (warns and
        // refuses to touch the real event stack once no overflow is pending).
        // The counter lives in the span data precisely so this stays safe
        // after the SpanImpl is gone — an overflowed event is ended through a
        // pointer that outlives the span just like a real event's does.
        data_->endDisabledSpanEvent();
    }

    void DisabledSpanEvent::SetError(std::string_view error_name, std::string_view error_message) try {
        // Same reach as SpanEventImpl::SetError, minus the recording: overflow
        // is a profiling depth limit, not a verdict on the transaction. The
        // owner check is InjectContext's (this event has no spanIfAlive of its
        // own) — a span already gone has no error flag left to mark.
        if (auto* span = data_->getOwner()) {
            span->markSpanError(error_name, error_message);
        }
    } CATCH_AND_LOG("set error")

    void DisabledSpanEvent::SetDestination(std::string_view dest) try {
        destination_id_ = dest;
    } CATCH_AND_LOG("set destination")

    void DisabledSpanEvent::InjectContext(TraceContextWriter& writer) try {
        // The overflowed event is never recorded, so the generated child span
        // id is not stored anywhere either — the same shape as the Java
        // agent, where recordNextSpanId on the DisableSpanEventRecorder is a
        // no-op but the full header set is still written.
        auto* span = data_->getOwner();
        if (span == nullptr) {
            injectDeadSpanContext(writer);
            return;
        }
        span->injectContext(writer,
                            generate_next_span_id(data_->getSpanId(), data_->getParentSpanId()),
                            destination_id_);
    } CATCH_AND_LOG("inject context")

} // namespace pinpoint
