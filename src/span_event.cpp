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
#include <utility>

#include "cache.h"
#include "callstack.h"
#include "logging.h"
#include "noop.h"
#include "span.h"
#include "span_event.h"
#include "utility.h"

namespace pinpoint {

    std::atomic<int64_t> Exception::exception_id_gen{1};

    SpanEventImpl::SpanEventImpl(SpanImpl* span, std::string_view operation) :
        span_(span),
        agent_(span->getAgent()),
        service_type_{defaults::SPAN_EVENT_SERVICE_TYPE},
        operation_{operation},
        sequence_{0},
        depth_{0},
        start_time_{to_milli_seconds(std::chrono::system_clock::now())},
        start_elapsed_{0},
        elapsed_{0},
        next_span_id_{0},
        endpoint_{},
        destination_id_{},
        error_func_id_{0},
        error_string_{},
        async_id_{NONE_ASYNC_ID},
        async_seq_gen_{0},
        api_id_{0} {
        assert(span_ != nullptr);
        assert(agent_ != nullptr);

        if (!operation_.empty()) {
            api_id_ = agent_->cacheApi(operation, API_TYPE_DEFAULT);
        }
    }

    PinpointAnnotation* SpanEventImpl::ensureAnnotations() const {
        if (!annotations_) {
            annotations_ = std::make_unique<PinpointAnnotation>();
        }
        return annotations_.get();
    }

    AnnotationPtr SpanEventImpl::GetAnnotations() const {
        // A finished event may already sit in a chunk being serialized on the
        // gRPC worker thread; handing out the live container would let user
        // code append concurrently with the worker's iteration. Every other
        // accessor degrades to a safe no-op after EndEvent — so does this.
        if (finished_) {
            LOG_WARN("span event is already finished");
            return noopAnnotation();
        }
        return ensureAnnotations();
    }

    void SpanEventImpl::EndEvent() {
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
    }

    void SpanEventImpl::finish() {
        // Ended through an internal path (event-stack pop): mark it so a
        // later user-level EndEvent on this event is rejected by the guard.
        finished_.store(true);
        span_->decrEventDepth();
        // system_clock can step backwards (NTP); never report a negative
        // elapsed time.
        elapsed_ = static_cast<int32_t>(
            std::max<int64_t>(to_milli_seconds(std::chrono::system_clock::now()) - start_time_, 0));
    }

    int64_t SpanEventImpl::generateNextSpanId() {
        next_span_id_ = generate_span_id();
        return next_span_id_;
    }

    void SpanEventImpl::SetError(std::string_view error_message) {
        SetError("Error", error_message);
    }

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message) {
        error_func_id_ = agent_->cacheError(error_name);
        error_string_ = error_message;
    }

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) {
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

            auto exception = std::make_unique<Exception>(std::move(callstack));
            const auto exception_id = exception->getId();
            if (span_->addException(std::move(exception))) {
                ensureAnnotations()->AppendLong(ANNOTATION_EXCEPTION_ID, exception_id);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("call stack trace exception = {}", e.what());
        }
    }

    void SpanEventImpl::SetSqlQuery(
        std::string_view sql_query,
        const std::vector<std::string_view>& args) {
        const auto& config = span_->config_;
        std::string joined_args;
        if (config->sql.trace_bind_value) {
            const int configured_max_size = config->sql.max_bind_args_size;
            const std::size_t max_size = configured_max_size > 0
                ? static_cast<std::size_t>(configured_max_size)
                : 0;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const std::size_t separator_size = i == 0 ? 0 : 1;
                const std::size_t remaining_size = max_size - joined_args.size();
                if (separator_size > remaining_size ||
                    args[i].size() > remaining_size - separator_size) {
                    joined_args.append("...(");
                    joined_args.append(std::to_string(configured_max_size));
                    joined_args.push_back(')');
                    break;
                }

                if (i != 0) {
                    joined_args.push_back(',');
                }
                joined_args.append(args[i]);
            }
        }

        const auto mode = config->sql.enable_sql_stats
            ? SqlMetaMode::Uid
            : SqlMetaMode::Id;
        auto prepared = agent_->prepareSql(sql_query, mode);
        if (!prepared || !*prepared) {
            return;
        }

        const auto& value = **prepared;
        // Aliasing shares PreparedSql's existing control block; no allocation
        // or parameter-string copy occurs on a raw-cache hit. It also protects
        // the bytes until asynchronous span serialization completes.
        auto parameters = std::shared_ptr<const std::string>(
            *prepared, &value.parameters);

        if (mode == SqlMetaMode::Uid) {
            if (const auto* uid = std::get_if<SqlUid>(&value.identity)) {
                ensureAnnotations()->AppendData(
                    ANNOTATION_SQL_UID,
                    AnnotationData(*uid, std::move(parameters), joined_args));
            }
            return;
        }

        if (const auto* sql_id = std::get_if<int32_t>(&value.identity)) {
            ensureAnnotations()->AppendData(
                ANNOTATION_SQL_ID,
                AnnotationData(*sql_id, std::move(parameters), joined_args));
        }
    }

    void SpanEventImpl::RecordHeader(HeaderType which, HeaderReader& reader) {
        agent_->recordClientHeader(which, reader, ensureAnnotations());
    }

    void SpanEventImpl::InjectContext(TraceContextWriter& writer) {
        span_->injectContext(writer, generateNextSpanId(), destination_id_);
    }

    AnnotationPtr DisabledSpanEvent::GetAnnotations() const {
        return noopAnnotation();
    }

    void DisabledSpanEvent::EndEvent() {
        // The shared per-span instance stands in for every overflowed event,
        // so it cannot carry a per-instance finished flag; the span's overflow
        // counter provides the duplicate-end guard instead (warns and refuses
        // to touch the real event stack once no overflow is pending).
        span_->endDisabledSpanEvent();
    }

    void DisabledSpanEvent::InjectContext(TraceContextWriter& writer) {
        // The overflowed event is never recorded, so the generated child span
        // id is not stored anywhere either — the same shape as the Java
        // agent, where recordNextSpanId on the DisableSpanEventRecorder is a
        // no-op but the full header set is still written.
        span_->injectContext(writer, generate_span_id(), destination_id_);
    }

} // namespace pinpoint
