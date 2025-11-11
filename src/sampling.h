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

    /// @brief Sampling mode that relies on counter-based periodic selection.
    const std::string COUNTER_SAMPLING = "COUNTER";
    /// @brief Sampling mode that uses percentage-based selection.
    const std::string PERCENT_SAMPLING = "PERCENT";
    /// @brief Maximum supported percent rate (stored as hundredths of a percent).
    constexpr int MAX_PERCENT_RATE = 100 * 100;

    /**
     * @brief Base sampler interface that decides whether a trace should be sampled.
     */
    class Sampler {
    public:
        Sampler () : rate_(0), sampling_count_(0) {}
        virtual ~Sampler() = default;

        /**
         * @brief Decides whether the current event should be sampled.
         */
        virtual bool isSampled() noexcept = 0;

    protected:
        int rate_;
        std::atomic<uint64_t> sampling_count_;
    };

    /**
     * @brief Samples every Nth request based on a counter.
     */
    class CounterSampler final : public Sampler {
    public:
        explicit CounterSampler (const int rate) {
            rate_ = rate;
        }

        /**
         * @brief Returns `true` every `rate_` calls.
         */
        bool isSampled() noexcept override;
    };

    /**
     * @brief Samples requests based on a configured percentage.
     */
    class PercentSampler final : public Sampler {
    public:
        explicit PercentSampler(const double rate) {
            rate_ = static_cast<int>(rate * 100);
        }

        /**
         * @brief Returns `true` using probabilistic selection based on the percent rate.
         */
        bool isSampled() noexcept override;
    };


    /**
     * @brief Base class that differentiates between new and continued trace sampling.
     */
    class TraceSampler {
    public:
        TraceSampler () = default;
        virtual ~TraceSampler() {
            sampler_ = nullptr;
        }

        /**
         * @brief Determines if a new trace should be sampled.
         */
        virtual bool isNewSampled() noexcept = 0;
        /**
         * @brief Determines if a continued trace should be sampled.
         */
        virtual bool isContinueSampled() noexcept = 0;

    protected:
        std::unique_ptr<Sampler> sampler_{nullptr};
    };

    /**
     * @brief Pass-through trace sampler that delegates to the underlying `Sampler`.
     */
    class BasicTraceSampler final : public TraceSampler {
    public:
        explicit BasicTraceSampler (std::unique_ptr<Sampler> sampler) {
            sampler_ = std::move(sampler);
        }

        bool isNewSampled() noexcept override;
        bool isContinueSampled() noexcept override;
    };

    /**
     * @brief Trace sampler with throughput limits for new and continuing traces.
     */
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
