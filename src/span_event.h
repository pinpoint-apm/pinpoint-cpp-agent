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

#include "pinpoint/tracer.h"
#include "annotation.h"
#include "utility.h"

namespace pinpoint {

    class SpanData;

    class SpanEventImpl final : public SpanEvent {
    public:
        SpanEventImpl(SpanData* span, std::string_view operation);
        ~SpanEventImpl() override {}

        void SetServiceType(int32_t type) override { service_type_ = type; }
        void SetOperationName(std::string_view operationName) override { operation_ = operationName; }
        void SetStartTime(std::chrono::system_clock::time_point start_time) override { start_time_ = to_milli_seconds(start_time); }
        void SetDestination(std::string_view dest) override {destination_id_ = dest; }
        void SetEndPoint(std::string_view endpoint) override { endpoint_ = endpoint; }
        void SetError(std::string_view error_message) override;
        void SetError(std::string_view error_name, std::string_view error_message) override;
        void RecordHeader(HeaderType which, HeaderReader& reader) override;
        AnnotationPtr GetAnnotations() const override { return annotations_; }

        void finish();

        SpanData* getParentSpan() const { return parent_span_; }
        int32_t getServiceType() const { return service_type_; }
        std::string& getOperationName() { return operation_; }

        int64_t getStartTime() const { return start_time_; }
        void setStartElapsed(int32_t elapsed) { start_elapsed_ = elapsed; }
        int32_t getStartElapsed() const { return start_elapsed_; }
        int32_t getEndElapsed() const { return elapsed_; }

        int32_t getSequence() const { return sequence_; }
        void setDepth(int32_t depth) { depth_ = depth; }
        int32_t getDepth() const { return depth_; }

        int64_t generateNextSpanId();
        int64_t getNextSpanId() const { return next_span_id_; }

        std::shared_ptr<PinpointAnnotation>& getAnnotations() { return annotations_; }

        std::string& getEndPoint() { return endpoint_; }
        std::string& getDestinationId() { return destination_id_; }

        int32_t getErrorFuncId() const { return error_func_id_; }
        std::string& getErrorString() { return error_string_; }

        void setAsyncId(const int32_t async_id) { async_id_ = async_id; }
        int32_t getAsyncId() const { return async_id_; }

        void incrAsyncSeq() { async_seq_gen_++; }
        int32_t getAsyncSeqGen() const { return async_seq_gen_; }

        void setApiId(int32_t api_id) { api_id_ = api_id; }
        int32_t getApiId() const { return api_id_; }

    private:
        SpanData *parent_span_;
        int32_t service_type_;
        std::string operation_;
        int32_t sequence_;
        int32_t depth_;
        int64_t start_time_;
        int32_t start_elapsed_;
        int32_t elapsed_;
        int64_t next_span_id_;
        std::string endpoint_;
        std::string destination_id_;
        int32_t error_func_id_;
        std::string error_string_;
        int32_t async_id_;
        int32_t async_seq_gen_;
        int32_t api_id_;
        std::shared_ptr<PinpointAnnotation> annotations_;
    };

}  // namespace pinpoint
