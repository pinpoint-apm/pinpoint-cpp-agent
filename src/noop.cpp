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

    static Noop& getNoop() {
        static Noop noop;
        return noop;
    }

    AnnotationPtr noopAnnotation() {
        return getNoop().annotation();
    }

    SpanEventPtr noopSpanEvent() {
        return getNoop().spanEvent();
    }

    SpanPtr noopSpan() {
        return getNoop().span();
    }

    AgentPtr noopAgent() {
        return getNoop().agent();
    }

    UnsampledSpan::UnsampledSpan(AgentService *agent) : NoopSpan(),
        span_id_(generate_span_id()),
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        url_stat_(),
        agent_ref_(agent != nullptr ? agent->selfRef() : nullptr),
        agent_(agent) {
        // Guard the deref to stay consistent with the null check on agent_ref_
        // above (agent_ is always the live AgentImpl in production).
        if (agent_ != nullptr) {
            agent_->getAgentStats().addActiveSpan(span_id_, start_time_);
        }
    }

    void UnsampledSpan::EndSpan() {
        // Atomic exchange so only the first caller proceeds: a check-then-set
        // would let two concurrent EndSpan calls both pass the guard and run
        // dropActiveSpan / collectResponseTime / recordUrlStat twice.
        if (finished_.exchange(true)) {
            LOG_WARN("span is already finished");
            return;
        }

        // Paired with the constructor's null guard: nothing to record without
        // an agent (agent_ is always valid in production).
        if (agent_ == nullptr) {
            return;
        }

        auto end_time_ = std::chrono::system_clock::now();
        auto elapsed_ = static_cast<int32_t>(to_milli_seconds(end_time_) - start_time_);

        auto& stats = agent_->getAgentStats();
        stats.collectResponseTime(elapsed_);
        stats.dropActiveSpan(span_id_);

        if (url_stat_) {
            url_stat_->end_time_ = end_time_;
            url_stat_->elapsed_ = elapsed_;
            agent_->recordUrlStat(std::move(*url_stat_));
            url_stat_.reset();
        }
    }

    void UnsampledSpan::SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) try {
        url_stat_.emplace(url_pattern, method, status_code);
    } catch (const std::exception& e) {
        LOG_ERROR("set url stat exception = {}", e.what());
    }

    void UnsampledSpan::InjectContext(TraceContextWriter& writer) {
        writer.Set(HEADER_SAMPLED, "s0");
    }
}
