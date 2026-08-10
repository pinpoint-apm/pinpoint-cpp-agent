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

#include "stat.h"
#include "sampling.h"

namespace pinpoint {

    bool CounterSampler::isSampled() noexcept {
        // rate_ <= 0 means disabled. A negative rate must be caught here:
        // count is uint64_t, so `count % rate_` would convert a negative rate_
        // to a huge unsigned value and silently "almost never" sample instead.
        if (rate_ <= 0) {
            return false;
        }

        // Pure counter with no cross-thread ordering requirement: relaxed
        // avoids the full barrier the default seq_cst RMW costs on every
        // sampling decision (this runs once per incoming request).
        const auto count = sampling_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t r = count % rate_;
        return r == 0;
    }

    bool PercentSampler::isSampled() noexcept {
        // The constructor clamps rate_ to [0, MAX_PERCENT_RATE]; <= 0 keeps
        // the disabled-rate guard uniform with CounterSampler.
        if (rate_ <= 0) {
            return false;
        }

        // Relaxed for the same reason as CounterSampler: only the counter
        // value itself matters, not its ordering against other memory.
        const auto count = sampling_count_.fetch_add(rate_, std::memory_order_relaxed) + rate_;
        const uint64_t r = count % MAX_PERCENT_RATE;
        return static_cast<int>(r) < rate_;
    }

    bool TraceSampler::isNewSampled() noexcept {
        auto sampled = sampler_ ? sampler_->isSampled() : false;
        auto& stats = agent_->getAgentStats();
        
        if (sampled) {
            if (new_limiter_) {
                sampled = new_limiter_->allow();
                if (sampled) {
                    stats.incrSampleNew();
                } else {
                    stats.incrSkipNew();
                }
            } else {
                stats.incrSampleNew();
            }
        } else {
            stats.incrUnsampleNew();
        }

        return sampled;
    }

    bool TraceSampler::isContinueSampled() noexcept {
        auto sampled = true;
        auto& stats = agent_->getAgentStats();
        
        if (cont_limiter_) {
            sampled = cont_limiter_->allow();
            if (sampled) {
                stats.incrSampleCont();
            } else {
                stats.incrSkipCont();
            }
        } else {
            stats.incrSampleCont();
        }

        return sampled;
    }

}
