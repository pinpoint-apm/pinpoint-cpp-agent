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

namespace pinpoint {
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

    void init_agent_stats();
    void collect_agent_stat(AgentStatsSnapshot &stat);
    void collect_response_time(int64_t resTime);
    void incr_sample_new();
    void incr_unsample_new();
    void incr_sample_cont();
    void incr_unsample_cont();
    void incr_skip_new();
    void incr_skip_cont();

    void add_active_span(int64_t spanId, int64_t start_time);
    void drop_active_span(int64_t spanId);

    enum StatsType {AGENT_STATS, URL_STATS};
    class AgentImpl;

    class AgentStats {
    public:
        explicit AgentStats(AgentImpl* agent) : agent_(agent) {}
        void agentStatsWorker();
        void stopAgentStatsWorker();

    private:
        AgentImpl* agent_{};
        std::mutex mutex_{};
        std::condition_variable cond_var_{};
    };

    std::vector<AgentStatsSnapshot>& get_agent_stat_snapshots();
}
