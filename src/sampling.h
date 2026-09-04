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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include "agent_service.h"
#include "limiter.h"

namespace pinpoint {

    // constexpr string_views (not const std::strings): these are read by the
    // config-reload path (build_runtime), which the config-file watcher thread
    // can still run during static destruction when the process exits without
    // Shutdown() — a dynamic destructor here would make that a use-after-free.
    /// @brief Sampling mode that relies on counter-based periodic selection.
    inline constexpr std::string_view COUNTER_SAMPLING = "COUNTER";
    /// @brief Java's name for counter sampling (`SamplerType.COUNTING`), accepted as an alias.
    inline constexpr std::string_view COUNTING_SAMPLING = "COUNTING";
    /// @brief Sampling mode that uses percentage-based selection.
    inline constexpr std::string_view PERCENT_SAMPLING = "PERCENT";
    /// @brief Maximum supported percent rate (stored as hundredths of a percent).
    constexpr int MAX_PERCENT_RATE = 100 * 100;

    /// @brief Base sampler interface that decides whether a trace should be sampled.
    class Sampler {
    public:
        Sampler () : rate_(0), sampling_count_(0) {}
        virtual ~Sampler() = default;

        /// @brief Decides whether the current event should be sampled.
        virtual bool isSampled() noexcept = 0;

    protected:
        int rate_;
        std::atomic<uint64_t> sampling_count_;
    };

    /// @brief Samples every Nth request based on a counter.
    class CounterSampler final : public Sampler {
    public:
        explicit CounterSampler (const int rate) {
            rate_ = rate;
        }

        /// @brief Returns `true` on the first call and every `rate_` calls after it.
        bool isSampled() noexcept override;
    };

    /// @brief Samples requests based on a configured percentage.
    class PercentSampler final : public Sampler {
    public:
        explicit PercentSampler(const double rate) {
            // Round to nearest hundredth-of-a-percent instead of truncating:
            // 0.29 * 100 is 28.999999999999996 in double, so a plain cast
            // would yield 28 and silently sample 0.28% instead of 0.29%.
            // Clamp to [0, MAX_PERCENT_RATE] so the class stays well-defined
            // (never-sample / always-sample) even for out-of-range rates that
            // bypass the config validation.
            rate_ = static_cast<int>(std::clamp<long>(
                std::lround(rate * 100), 0, MAX_PERCENT_RATE));
        }

        /// @brief Returns `true` deterministically based on the sampling counter and percent rate.
        bool isSampled() noexcept override;
    };


    /**
     * @brief Trace sampler with throughput limits for new and continuing traces.
     *
     * A non-positive limit creates no limiter for that side, so
     * `{agent, sampler, 0, 0}` is the plain pass-through sampler.
     */
    class TraceSampler final {
    public:
        TraceSampler(AgentService* agent, std::unique_ptr<Sampler> sampler,
                     const int new_tps, const int continue_tps)
            : agent_(agent), sampler_(std::move(sampler)) {
            if (new_tps > 0) {
                new_limiter_ = std::make_unique<RateLimiter>(new_tps);
            }
            if (continue_tps > 0) {
                cont_limiter_ = std::make_unique<RateLimiter>(continue_tps);
            }
        }

        /// @brief Determines if a new trace should be sampled.
        bool isNewSampled() noexcept;
        /// @brief Determines if a continued trace should be sampled.
        bool isContinueSampled() noexcept;

    private:
        // Non-owning. AgentImpl owns the sampler (sampler_ AtomicSharedPtr
        // member) and only drives it from NewSpan while the caller holds the
        // agent alive, so agent_ never dangles. A shared_ptr here would form a
        // cycle and leak the agent.
        AgentService* agent_;
        std::unique_ptr<Sampler> sampler_;
        std::unique_ptr<RateLimiter> new_limiter_{nullptr};
        std::unique_ptr<RateLimiter> cont_limiter_{nullptr};
    };

}
