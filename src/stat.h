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
#include <condition_variable>
#include <mutex>
#include <vector>

#include "agent_service.h"

namespace pinpoint {
    /**
     * @brief Snapshot of runtime statistics collected from the agent process.
     */
    struct AgentStatsSnapshot {
        int64_t    sample_time_{};
        double     system_cpu_time_{};
        double     process_cpu_time_{};
        int64_t    num_threads_{};
        int64_t    heap_alloc_size_{};
        int64_t    heap_max_size_{};
        int64_t    response_time_avg_{};
        int64_t    response_time_max_{};
        int64_t    num_sample_new_{};
        int64_t    num_sample_cont_{};
        int64_t    num_unsample_new_{};
        int64_t    num_unsample_cont_{};
        int64_t    num_skip_new_{};
        int64_t    num_skip_cont_{};
        int32_t    active_requests_[4]{};
    };

    /**
     * @brief Initializes the statistics subsystem.
     */
    void init_agent_stats();
    /**
     * @brief Fills the provided snapshot with the latest metrics.
     *
     * @param stat Snapshot to populate.
     */
    void collect_agent_stat(AgentStatsSnapshot &stat);
    /// @brief Updates the response time histogram.
    void collect_response_time(int64_t resTime);
    /// @brief Increments the counter for sampled new traces.
    void incr_sample_new();
    /// @brief Increments the counter for unsampled new traces.
    void incr_unsample_new();
    /// @brief Increments the counter for sampled continuing traces.
    void incr_sample_cont();
    /// @brief Increments the counter for unsampled continuing traces.
    void incr_unsample_cont();
    /// @brief Increments the counter for skipped new traces.
    void incr_skip_new();
    /// @brief Increments the counter for skipped continuing traces.
    void incr_skip_cont();

    /// @brief Registers an active span with its start time.
    void add_active_span(int64_t spanId, int64_t start_time);
    /// @brief Removes an active span from tracking.
    void drop_active_span(int64_t spanId);

    /**
     * @brief Worker responsible for periodically sending agent statistics to the collector.
     */
    class AgentStats {
    public:
        explicit AgentStats(AgentService* agent) : agent_(agent) {}
        /// @brief Background loop that gathers and sends agent statistics.
        void agentStatsWorker();
        /// @brief Signals the worker to stop processing.
        void stopAgentStatsWorker();

    private:
        AgentService* agent_{};
        std::mutex mutex_{};
        std::condition_variable cond_var_{};
    };

    /**
     * @brief Returns the shared buffer of collected agent stats snapshots.
     */
    std::vector<AgentStatsSnapshot>& get_agent_stat_snapshots();
}
