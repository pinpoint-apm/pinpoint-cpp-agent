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

#include "agent.h"
#include "span.h"
#include "span_event.h"
#include "sql.h"
#include "utility.h"

namespace pinpoint {

    SpanEventImpl::SpanEventImpl(SpanData* span, std::string_view operation) :
        parent_span_(span),
        service_type_{DEFAULT_SPAN_EVENT_SERVICE_TYPE},
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
            api_id_ = parent_span_->getAgent()->cacheApi(operation, API_TYPE_DEFAULT);
        }
    }

    void SpanEventImpl::finish() {
        parent_span_->decrEventDepth();
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
        error_func_id_ = parent_span_->getAgent()->cacheError(error_name);
        error_string_ = error_message;
    }

    void SpanEventImpl::SetSqlQuery(std::string_view sql_query, std::string_view args) {
        SqlNormalizer normalizer(64*1024);
        SqlNormalizeResult result = normalizer.normalize(sql_query);

        auto& config = parent_span_->getAgent()->getConfig();
        if (config.sql.enable_sql_stats) {
            auto sql_uid = parent_span_->getAgent()->cacheSqlUid(result.normalized_sql);
            if (!sql_uid.empty()) {
                annotations_->AppendBytesStringString(ANNOTATION_SQL_UID, sql_uid, result.parameters, args);
            }
        } else {
            auto sql_id = parent_span_->getAgent()->cacheSql(result.normalized_sql);
            if (sql_id) {
                annotations_->AppendIntStringString(ANNOTATION_SQL_ID, sql_id, result.parameters, args);
            }
        }
    }

    void SpanEventImpl::RecordHeader(HeaderType which, HeaderReader& reader) {
        parent_span_->getAgent()->recordClientHeader(which, reader, annotations_);
    }

} // namespace pinpoint
