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

        std::string joinSqlBindValues(
            const std::vector<SqlBindValue>& bind_args,
            int max_bind_args_size) {
            std::string joined_bind_args;
            if (max_bind_args_size <= 0) {
                return joined_bind_args;
            }

            const auto max_size = static_cast<std::size_t>(max_bind_args_size);
            // Room for the "...(<max_bind_args_size>)" truncation suffix:
            // 5 punctuation chars plus at most 10 digits of a positive int.
            constexpr std::size_t kTruncationSuffixMax = 16;

            // The output never exceeds max_size plus the suffix. Reserve that
            // up front, capped so a large configured limit does not allocate
            // more than a typical bind list needs; the rest is left to the
            // string's geometric growth.
            constexpr std::size_t kMaxInitialReserve = 256;
            joined_bind_args.reserve(std::min(max_size, kMaxInitialReserve) + kTruncationSuffixMax);

            fmt::memory_buffer scratch;
            for (std::size_t i = 0; i < bind_args.size(); ++i) {
                const auto arg = sqlBindValueView(bind_args[i], scratch);
                const std::size_t separator_size = i == 0 ? 0 : 1;
                const std::size_t remaining_size = max_size - joined_bind_args.size();
                if (separator_size > remaining_size ||
                    arg.size() > remaining_size - separator_size) {
                    fmt::format_to(std::back_inserter(joined_bind_args),
                                   "...({})", max_bind_args_size);
                    break;
                }

                if (i != 0) {
                    joined_bind_args.push_back(',');
                }
                joined_bind_args.append(arg);
            }
            return joined_bind_args;
        }
    }

    std::atomic<int64_t> Exception::exception_id_gen{1};

    SpanEventImpl::SpanEventImpl(SpanImpl* span, std::string_view operation) :
        span_(span),
        agent_(span->agent_),
        service_type_{defaults::SPAN_EVENT_SERVICE_TYPE},
        start_time_{to_milli_seconds(std::chrono::system_clock::now())} {
        assert(span_ != nullptr);
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
        span_->endSpanEvent(this);
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
        span_->data_->decrEventDepth();
        // system_clock can step backwards (NTP); never report a negative
        // elapsed time. Only the low side is clamped: the wire field is
        // int32 ms, so a delta beyond INT32_MAX ms (~24.8 days — e.g. a
        // user-supplied start time in seconds instead of ms) wraps.
        elapsed_ = static_cast<int32_t>(
            std::max<int64_t>(to_milli_seconds(std::chrono::system_clock::now()) - start_time_, 0));
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

    int64_t SpanEventImpl::generateNextSpanId() {
        next_span_id_ = generate_span_id();
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
        error_string_ = error_message;
    } CATCH_AND_LOG("set error")

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) {
        if (warnIfFinished()) return;
        SetError(error_name, error_message);

        const auto& cfg = span_->config_;
        if (!cfg->enable_callstack_trace) {
            return;
        }

        try {
            auto callstack = std::make_unique<CallStack>(error_message);
            reader.ForEach([&](std::string_view module, std::string_view function, std::string_view file, int line) {
                callstack->push(module, function, file, line);
                return;
            });
            recordException(std::move(callstack));
        } catch (const std::exception& e) {
            LOG_ERROR("call stack trace exception = {}", e.what());
        }
    }

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message,
                                 const std::vector<CallStackFrame>& frames) {
        if (warnIfFinished()) return;
        SetError(error_name, error_message);

        const auto& cfg = span_->config_;
        if (!cfg->enable_callstack_trace) {
            return;
        }

        try {
            auto callstack = std::make_unique<CallStack>(error_message);
            for (const auto& frame : frames) {
                callstack->push(frame.module, frame.function, frame.file, frame.line);
            }
            recordException(std::move(callstack));
        } catch (const std::exception& e) {
            LOG_ERROR("call stack trace exception = {}", e.what());
        }
    }

    void SpanEventImpl::recordException(std::unique_ptr<CallStack> callstack) {
        auto exception = std::make_unique<Exception>(std::move(callstack));
        const auto exception_id = exception->getId();
        if (span_->addException(std::move(exception))) {
            annotations_.AppendLong(ANNOTATION_EXCEPTION_ID, exception_id);
        }
    }

    void SpanEventImpl::SetSqlQuery(
        std::string_view sql_query,
        const std::vector<SqlBindValue>& bind_args) try {
        if (warnIfFinished()) return;
        const auto& config = span_->config_;
        const auto mode = config->sql.enable_sql_stats
            ? SqlMetaMode::Uid
            : SqlMetaMode::Id;
        auto prepared = agent_->prepareSql(sql_query, mode);
        if (!prepared || !*prepared) {
            return;
        }

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
        // field the gRPC worker reads once this event sits in a chunk, and a
        // finished event can outlive span_ itself (retired events are kept
        // alive by SpanData after the SpanImpl has been destroyed).
        if (warnIfFinished()) return;
        span_->injectContext(writer, generateNextSpanId(), destination_id_);
    } CATCH_AND_LOG("inject context")

    void DisabledSpanEvent::EndEvent() {
        // The shared per-span instance stands in for every overflowed event,
        // so it cannot carry a per-instance finished flag; the span's overflow
        // counter provides the duplicate-end guard instead (warns and refuses
        // to touch the real event stack once no overflow is pending).
        span_->endDisabledSpanEvent();
    }

    void DisabledSpanEvent::SetDestination(std::string_view dest) try {
        destination_id_ = dest;
    } CATCH_AND_LOG("set destination")

    void DisabledSpanEvent::InjectContext(TraceContextWriter& writer) try {
        // The overflowed event is never recorded, so the generated child span
        // id is not stored anywhere either — the same shape as the Java
        // agent, where recordNextSpanId on the DisableSpanEventRecorder is a
        // no-op but the full header set is still written.
        span_->injectContext(writer, generate_span_id(), destination_id_);
    } CATCH_AND_LOG("inject context")

} // namespace pinpoint
