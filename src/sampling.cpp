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

        // The pre-increment value is what gets tested, so the counter starts at
        // 0 and the first request after startup is sampled, then every rate_-th
        // one after it. Same phase as Java's CountingSampler
        // (counter.getAndIncrement()); testing the post-increment value instead
        // would sample requests N, 2N, ... and hide the first request on
        // low-traffic or test deployments.
        //
        // A config reload restarts this phase only when it actually changes
        // Sampling.*: build_runtime() carries the sampler over otherwise,
        // precisely so an unrelated edit does not re-sample the next request.
        //
        // Pure counter with no cross-thread ordering requirement: relaxed
        // avoids the full barrier the default seq_cst RMW costs on every
        // sampling decision (this runs once per incoming request).
        const auto count = sampling_count_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t r = count % rate_;
        return r == 0;
    }

    bool PercentSampler::isSampled() noexcept {
        // The constructor clamps rate_ to [0, MAX_PERCENT_RATE]; <= 0 keeps
        // the disabled-rate guard uniform with CounterSampler.
        if (rate_ <= 0) {
            return false;
        }

        // A full rate is always-sample. Java has no such branch because
        // PercentRateSampler rejects samplingRate >= MAX and PercentSamplerFactory
        // hands that case to TrueSampler; the constructor clamp folds it in here
        // instead, and the admission test below would never fire for it (the
        // remainder is 0 on every call).
        if (rate_ >= MAX_PERCENT_RATE) {
            return true;
        }

        // Relaxed for the same reason as CounterSampler: only the counter
        // value itself matters, not its ordering against other memory.
        //
        // The admission window is (0, rate_], matching Java's PercentRateSampler
        // (`remainder > 0 && remainder <= samplingRate`). Testing [0, rate_)
        // instead samples just as often but shifts the phase by one, so the first
        // transaction after startup (or a Sampling.* reload) is never sampled:
        // `PercentRate: 50` would admit the 2nd, 4th, ... request where Java
        // admits the 1st, 3rd, ... Same reasoning as CounterSampler testing the
        // pre-increment value.
        const auto count = sampling_count_.fetch_add(rate_, std::memory_order_relaxed) + rate_;
        const uint64_t r = count % MAX_PERCENT_RATE;
        return r > 0 && r <= static_cast<uint64_t>(rate_);
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
