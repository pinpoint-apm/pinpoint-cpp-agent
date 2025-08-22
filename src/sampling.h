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

#include <atomic>
#include <memory>
#include <string>

#include "limiter.h"

namespace pinpoint {

    const std::string COUNTER_SAMPLING = "COUNTER";
    const std::string PERCENT_SAMPLING = "PERCENT";
    constexpr int MAX_PERCENT_RATE = 100 * 100;

    class Sampler {
    public:
        Sampler () : rate_(0), sampling_count_(0) {}
        virtual ~Sampler() = default;

        virtual bool isSampled() noexcept = 0;

    protected:
        int rate_;
        std::atomic<uint64_t> sampling_count_;
    };

    class CounterSampler final : public Sampler {
    public:
        explicit CounterSampler (const int rate) {
            rate_ = rate;
        }

        bool isSampled() noexcept override;
    };

    class PercentSampler final : public Sampler {
    public:
        explicit PercentSampler(const double rate) {
            rate_ = static_cast<int>(rate * 100);
        }

        bool isSampled() noexcept override;
    };


    class TraceSampler {
    public:
        TraceSampler () = default;
        virtual ~TraceSampler() {
            sampler_ = nullptr;
        }

        virtual bool isNewSampled() noexcept = 0;
        virtual bool isContinueSampled() noexcept = 0;

    protected:
        std::unique_ptr<Sampler> sampler_{nullptr};
    };

    class BasicTraceSampler final : public TraceSampler {
    public:
        explicit BasicTraceSampler (std::unique_ptr<Sampler> sampler) {
            sampler_ = std::move(sampler);
        }

        bool isNewSampled() noexcept override;
        bool isContinueSampled() noexcept override;
    };

    class ThroughputLimitTraceSampler final : public TraceSampler {
    public:
        explicit ThroughputLimitTraceSampler (std::unique_ptr<Sampler> sampler, const int new_tps, const int continue_tps) {
            sampler_ = std::move(sampler);
            if (new_tps > 0) {
                new_limiter_ = std::make_unique<RateLimiter>(new_tps);
            }
            if (continue_tps > 0) {
                cont_limiter_ = std::make_unique<RateLimiter>(continue_tps);
            }
        }

        ~ThroughputLimitTraceSampler() override {
            new_limiter_ = nullptr;
            cont_limiter_ = nullptr;
        }

        bool isNewSampled() noexcept override;
        bool isContinueSampled() noexcept override;

    private:
        std::unique_ptr<RateLimiter> new_limiter_{nullptr};
        std::unique_ptr<RateLimiter> cont_limiter_{nullptr};
    };

}
