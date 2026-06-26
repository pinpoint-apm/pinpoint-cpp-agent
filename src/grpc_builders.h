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

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "pinpoint/tracer.h"

namespace google::protobuf {
    class Arena;
}

namespace v1 {
    class PAgentStatBatch;
    class PAgentUriStat;
    class PExceptionMetaData;
    class PSpan;
    class PSpanChunk;
    class PTransactionId;
}

namespace pinpoint {
    class Exception;
    class SpanChunk;
    class UrlStatSnapshot;
    struct AgentStatsSnapshot;

    v1::PTransactionId* build_grpc_transaction_id(const TraceId& tid, google::protobuf::Arena* arena);
    v1::PSpan* build_grpc_span(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena);
    v1::PSpanChunk* build_grpc_span_chunk(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena);
    v1::PAgentStatBatch* build_agent_stat_batch(const std::vector<AgentStatsSnapshot>& stats,
                                                google::protobuf::Arena* arena);
    v1::PAgentUriStat* build_url_stat(const UrlStatSnapshot* snapshot, google::protobuf::Arena* arena);
    v1::PExceptionMetaData* build_exception_metadata(
            const TraceId& txid,
            int64_t span_id,
            std::string_view url_template,
            const std::vector<std::unique_ptr<Exception>>& exceptions,
            google::protobuf::Arena* arena);
}  // namespace pinpoint
