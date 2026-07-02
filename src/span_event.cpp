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

#include <cassert>
#include <utility>

#include "callstack.h"
#include "logging.h"
#include "noop.h"
#include "span.h"
#include "span_event.h"
#include "sql.h"
#include "utility.h"

namespace pinpoint {

    std::atomic<int32_t> Exception::exception_id_gen{1};

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

    void SpanEventImpl::EndEvent() {
        // Atomic exchange so only the first end proceeds: ending an event
        // twice would pop a DIFFERENT (still-active) event from the span's
        // stack and desync the whole call tree.
        if (finished_.exchange(true)) {
            LOG_WARN("span event is already finished");
            return;
        }
        span_->endSpanEvent();
    }

    void SpanEventImpl::finish() {
        // Ended through an internal path (event-stack pop): mark it so a
        // later user-level EndEvent on this event is rejected by the guard.
        finished_.store(true);
        span_->decrEventDepth();
        elapsed_ = to_milli_seconds(std::chrono::system_clock::now()) - start_time_;
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
            ensureAnnotations()->AppendLong(ANNOTATION_EXCEPTION_ID, exception->getId());
            span_->addException(std::move(exception));
        } catch (const std::exception& e) {
            LOG_ERROR("call stack trace exception = {}", e.what());
        }
    }

    void SpanEventImpl::SetSqlQuery(std::string_view sql_query, std::string_view args) {
        // Use a thread-local or static instance since SqlNormalizer is now stateless/thread-safe for normalize()
        static const SqlNormalizer normalizer(64*1024);
        SqlNormalizeResult result = normalizer.normalize(sql_query);

        const auto& config = span_->config_;
        if (config->sql.enable_sql_stats) {
            auto sql_uid = agent_->cacheSqlUid(result.normalized_sql);
            if (sql_uid) {
                // result.parameters is dead after this call: move it into the
                // annotation instead of copying through the string_view path.
                ensureAnnotations()->AppendData(ANNOTATION_SQL_UID,
                    AnnotationData(ANNOTATION_TYPE_BYTES_STRING_STRING, *sql_uid,
                                   std::move(result.parameters), args));
            }
        } else {
            auto sql_id = agent_->cacheSql(result.normalized_sql);
            if (sql_id) {
                ensureAnnotations()->AppendData(ANNOTATION_SQL_ID,
                    AnnotationData(ANNOTATION_TYPE_INT_STRING_STRING, sql_id,
                                   std::move(result.parameters), args));
            }
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
