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
        if (rate_ == 0) {
            return false;
        }

        const auto count = sampling_count_.fetch_add(1) + 1;
        const uint64_t r = count % rate_;
        return r == 0;
    }

    bool PercentSampler::isSampled() noexcept {
        if (rate_ == 0) {
            return false;
        }

        const auto count = sampling_count_.fetch_add(rate_) + rate_;
        const uint64_t r = count % MAX_PERCENT_RATE;
        return static_cast<int>(r) < rate_;
    }

    bool BasicTraceSampler::isNewSampled() noexcept {
        const auto sampled = sampler_ ? sampler_->isSampled() : false;
        if (sampled) {
            incr_sample_new();
        } else {
            incr_unsample_new();
        }
        return sampled;
    }

    bool BasicTraceSampler::isContinueSampled() noexcept {
        incr_sample_cont();
        return true;
    }

    bool ThroughputLimitTraceSampler::isNewSampled() noexcept {
        auto sampled = sampler_ ? sampler_->isSampled() : false;
        if (sampled) {
            if (new_limiter_) {
                sampled = new_limiter_->allow();
                if (sampled) {
                    incr_sample_new();
                } else {
                    incr_skip_new();
                }
            } else {
                incr_sample_new();
            }
        } else {
            incr_unsample_new();
        }

        return sampled;
    }

    bool ThroughputLimitTraceSampler::isContinueSampled() noexcept {
        auto sampled = true;
        if (cont_limiter_) {
            sampled = cont_limiter_->allow();
            if (sampled) {
                incr_sample_cont();
            } else {
                incr_skip_cont();
            }
        } else {
            incr_sample_cont();
        }

        return sampled;
    }

}
