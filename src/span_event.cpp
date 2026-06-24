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

    SpanEventImpl::SpanEventImpl(const std::shared_ptr<SpanData>& span, std::string_view operation,
                                 std::weak_ptr<Span> parent_span_ref) :
        parent_span_(span),
        parent_span_ref_(std::move(parent_span_ref)),
        service_type_{defaults::SPAN_EVENT_SERVICE_TYPE},
        operation_{operation},
        sequence_{span->getEventSequence()},
        depth_{span->getEventDepth()},
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
        api_id_{0},
        annotations_{std::make_shared<PinpointAnnotation>()} {

        if (!operation_.empty()) {
            api_id_ = span->getAgent()->cacheApi(operation, API_TYPE_DEFAULT);
        }
    }

    SpanPtr SpanEventImpl::GetParentSpan() const {
        if (auto parent_span = parent_span_ref_.lock()) {
            return parent_span;
        }
        return noopSpan();
    }

    void SpanEventImpl::EndEvent() {
        if (auto parent_span = parent_span_ref_.lock()) {
            parent_span->EndSpanEvent();
            return;
        }
        // The owning span is already gone (or was never shared_ptr-owned);
        // there is no safe object left to finish this event against.
        LOG_WARN("EndEvent: parent span no longer alive - event discarded");
    }

    void SpanEventImpl::finish() {
        if (const auto span = parent_span_.lock()) {
            span->decrEventDepth();
        }
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
        const auto span = parent_span_.lock();
        if (!span) {
            return;
        }
        error_func_id_ = span->getAgent()->cacheError(error_name);
        error_string_ = error_message;
    }

    void SpanEventImpl::SetError(std::string_view error_name, std::string_view error_message, CallStackReader& reader) {
        const auto span = parent_span_.lock();
        if (!span) {
            return;
        }
        SetError(error_name, error_message);

        const auto cfg = span->getAgent()->getConfig();
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
            annotations_->AppendLong(ANNOTATION_EXCEPTION_ID, exception->getId());
            span->addException(std::move(exception));
        } catch (const std::exception& e) {
            LOG_ERROR("call stack trace exception = {}", e.what());
        }
    }

    void SpanEventImpl::SetSqlQuery(std::string_view sql_query, std::string_view args) {
        const auto span = parent_span_.lock();
        if (!span) {
            return;
        }

        // Use a thread-local or static instance since SqlNormalizer is now stateless/thread-safe for normalize()
        static const SqlNormalizer normalizer(64*1024);
        SqlNormalizeResult result = normalizer.normalize(sql_query);

        const auto config = span->getAgent()->getConfig();
        if (config->sql.enable_sql_stats) {
            auto sql_uid = span->getAgent()->cacheSqlUid(result.normalized_sql);
            if (sql_uid) {
                annotations_->AppendSqlUidStringString(ANNOTATION_SQL_UID, *sql_uid,
                    result.parameters, args);
            }
        } else {
            auto sql_id = span->getAgent()->cacheSql(result.normalized_sql);
            if (sql_id) {
                annotations_->AppendIntStringString(ANNOTATION_SQL_ID, sql_id, result.parameters, args);
            }
        }
    }

    void SpanEventImpl::RecordHeader(HeaderType which, HeaderReader& reader) {
        const auto span = parent_span_.lock();
        if (!span) {
            return;
        }
        span->getAgent()->recordClientHeader(which, reader, annotations_);
    }

} // namespace pinpoint
