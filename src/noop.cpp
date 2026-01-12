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

#include "logging.h"
#include "stat.h"
#include "utility.h"
#include "noop.h"

namespace pinpoint {

    static Noop noop;

    AnnotationPtr noopAnnotation() {
        return noop.annotation();
    }

    SpanEventPtr noopSpanEvent() {
        return noop.spanEvent();
    }

    SpanPtr noopSpan() {
        return noop.span();
    }

    AgentPtr noopAgent() {
        return noop.agent();
    }

    UnsampledSpan::UnsampledSpan(AgentService *agent) : NoopSpan(),
        span_id_(generate_span_id()),
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        url_stat_(nullptr), agent_(agent) {
        agent_->getAgentStats().addActiveSpan(span_id_, start_time_);
    }

    void UnsampledSpan::EndSpan() {
        auto end_time_ = std::chrono::system_clock::now();
        auto elapsed_ = static_cast<int32_t>(to_milli_seconds(end_time_) - start_time_);

        auto& stats = agent_->getAgentStats();
        stats.collectResponseTime(elapsed_);
        stats.dropActiveSpan(span_id_);

        if (url_stat_) {
            url_stat_->end_time_ = end_time_;
            url_stat_->elapsed_ = elapsed_;
            agent_->recordUrlStat(std::move(url_stat_));
        }
    }

    void UnsampledSpan::SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) try {
        url_stat_ = std::make_unique<UrlStatEntry>(url_pattern, method, status_code);
    } catch (const std::exception& e) {
        LOG_ERROR("set url stat exception = {}", e.what());
    }

    void UnsampledSpan::InjectContext(TraceContextWriter& writer) {
        writer.Set(HEADER_SAMPLED, "s0");
    }
}

