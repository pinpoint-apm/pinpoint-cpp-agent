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
#include <atomic>
#include <map>
#include <chrono>

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
     * @brief Returns the shared buffer of collected agent stats snapshots.
     */
    std::vector<AgentStatsSnapshot>& get_agent_stat_snapshots();

    /**
     * @brief Worker responsible for periodically sending agent statistics to the collector.
     */
    class AgentStats {
    public:
        explicit AgentStats(AgentService* agent);
        ~AgentStats() = default;

        /// @brief Background loop that gathers and sends agent statistics.
        void agentStatsWorker();
        /// @brief Signals the worker to stop processing.
        void stopAgentStatsWorker();

        // Public methods for data collection (called by global functions)
        void collectResponseTime(int64_t resTime);
        void addActiveSpan(int64_t spanId, int64_t start_time);
        void dropActiveSpan(int64_t spanId);
        
        // Counter incrementers
        void incrSampleNew() { sample_new_++; }
        void incrUnsampleNew() { un_sample_new_++; }
        void incrSampleCont() { sample_cont_++; }
        void incrUnsampleCont() { un_sample_cont_++; }
        void incrSkipNew() { skip_new_++; }
        void incrSkipCont() { skip_cont_++; }

        // Access to snapshots (for testing or global accessor)
        std::vector<AgentStatsSnapshot>& getSnapshots() { return agent_stats_snapshots_; }

        // Singleton instance accessor (for global C-style functions)
        static AgentStats* getInstance();
        static void setInstance(AgentStats* instance);

        void initAgentStats();
        void collectAgentStat(AgentStatsSnapshot &stat);
        void resetAgentStats();

    private:
        int64_t getResponseTimeAvg();
        
        // System metrics helpers
        void getCpuLoad(std::chrono::seconds dur, double* sys_load, double* proc_load);
        void getProcessStatus(int64_t *heap_alloc, int64_t *heap_max, int64_t *num_threads);

    private:
        AgentService* agent_{};
        std::mutex mutex_{};
        std::condition_variable cond_var_{};
        
        // Statistics Data
        std::chrono::system_clock::time_point last_collect_time_;
        clock_t last_sys_cpu_time_{0};
        clock_t last_proc_cpu_time_{0};
        
        int64_t acc_response_time_{0};
        int64_t request_count_{0};
        int64_t max_response_time_{0};
        std::mutex response_time_mutex_;
        
        std::atomic<int64_t> sample_new_{0};
        std::atomic<int64_t> un_sample_new_{0};
        std::atomic<int64_t> sample_cont_{0};
        std::atomic<int64_t> un_sample_cont_{0};
        std::atomic<int64_t> skip_new_{0};
        std::atomic<int64_t> skip_cont_{0};
        
        std::mutex active_span_mutex_;
        std::map<int64_t, int64_t> active_span_map_;
        
        std::vector<AgentStatsSnapshot> agent_stats_snapshots_;
        int batch_{0};
        
        // Cached system constants
        long sc_clk_tck_{0};
        long sc_nprocessors_onln_{0};
    };
}
