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

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <atomic>
#include <chrono>
#include <array>
#include <optional>
#include <sys/types.h>

#include "active_span.h"
#include "agent_service.h"

namespace pinpoint {
    /// @brief Snapshot of runtime statistics collected from the agent process.
    struct AgentStatsSnapshot {
        int64_t    sample_time_{0};
        int64_t    interval_{0}; 
        double     system_cpu_time_{0};
        double     process_cpu_time_{0};
        int64_t    num_threads_{0};
        int64_t    heap_alloc_size_{0};
        int64_t    heap_max_size_{0};
        int64_t    response_time_avg_{0};
        int64_t    response_time_max_{0};
        int64_t    num_sample_new_{0};
        int64_t    num_sample_cont_{0};
        int64_t    num_unsample_new_{0};
        int64_t    num_unsample_cont_{0};
        int64_t    num_skip_new_{0};
        int64_t    num_skip_cont_{0};
        int32_t    active_requests_[4]{0, 0, 0, 0};
    };

    /// @brief Worker responsible for periodically sending agent statistics to the collector.
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
        // Links/unlinks the span's embedded registration node (see
        // active_span.h). span_id only picks the shard; the node carries the
        // data. dropActiveSpan is idempotent — a second call (destructor
        // backstop, releaseActiveSpanOnError) is an atomic load and return.
        void addActiveSpan(ActiveSpanNode& node, int64_t span_id, int64_t start_time);
        void dropActiveSpan(ActiveSpanNode& node);
        
        // Counter incrementers. Called once per request, routed to the
        // caller's per-thread shard (see ResponseTimeShard) so the RMW lands
        // on a cache line no other thread's requests touch. Relaxed: these are
        // independent counters with no ordering relationship to other data —
        // the collector just exchange()s and sums them per shard.
        void incrSampleNew() { responseTimeShard().sample_new_.fetch_add(1, std::memory_order_relaxed); }
        void incrUnsampleNew() { responseTimeShard().un_sample_new_.fetch_add(1, std::memory_order_relaxed); }
        void incrSampleCont() { responseTimeShard().sample_cont_.fetch_add(1, std::memory_order_relaxed); }
        void incrUnsampleCont() { responseTimeShard().un_sample_cont_.fetch_add(1, std::memory_order_relaxed); }
        void incrSkipNew() { responseTimeShard().skip_new_.fetch_add(1, std::memory_order_relaxed); }
        void incrSkipCont() { responseTimeShard().skip_cont_.fetch_add(1, std::memory_order_relaxed); }

        /**
         * @brief Returns a copy of the snapshot batch, taken under the stats mutex.
         *
         * Safe to serialize on another thread (the gRPC stats stream) while the
         * worker keeps overwriting slots for the next collection cycle — reading
         * the vector without the mutex races those writes.
         */
        std::vector<AgentStatsSnapshot> copySnapshots() {
            std::lock_guard<std::mutex> lock(mutex_);
            return agent_stats_snapshots_;
        }

        void initAgentStats();
        void collectAgentStat(AgentStatsSnapshot &stat);
        void collectActiveRequests(int32_t active_requests[4], int64_t sample_time_ms);
        void resetAgentStats();

    private:
        /// @brief One supervised run of the collect loop; agentStatsWorker
        /// restarts it after a transient exception.
        void runAgentStatsWorker(const Config& config);
        /**
         * @brief True when this object was created in another process, i.e.
         *        the host forked and the child reached an inherited agent.
         *
         * The active-span registry is the one per-request structure a span
         * touches without going through AgentImpl, so AgentImpl's own guards
         * (tracing_active, the pid-guarded teardown) do not cover it. Its
         * add/drop/collect all take a shard mutex, and a mutex some thread
         * held at the fork instant is inherited locked with no thread left in
         * the child to unlock it — the caller would block forever. A span
         * created before the fork is exactly such a caller: the child runs on
         * the forking thread's stack, so EndSpan on that span is reached
         * directly, not through the agent.
         */
        bool inheritedAcrossFork() const noexcept;
        /// @brief Drains every per-thread shard in one sweep: response-time
        /// avg/max plus the six sampler-outcome counts, all into `stat`.
        void collectAndResetRequestStats(AgentStatsSnapshot& stat);
        
        // System metrics structures
        struct CpuLoad {
            double sys_load;
            double proc_load;
        };
        
        struct ProcessStatus {
            int64_t heap_alloc;
            int64_t heap_max;
            int64_t num_threads;
        };
        
        // System metrics helpers
        CpuLoad getCpuLoad(std::chrono::milliseconds dur);
        ProcessStatus getProcessStatus();

    private:
        static constexpr size_t kResponseTimeShardCount = 16;

        // Cache-line-aligned per-thread shard: every request updates exactly
        // one shard (picked by thread id), so the per-request RMWs never
        // contend across threads. Besides the response-time fields it carries
        // the sampler-outcome counters: as process-wide singles they were one
        // shared line hit once per request — span_lifecycle_benchmark
        // measured that at ~23 ns/request of cross-core traffic at 4 threads,
        // the same contention this sharding exists to remove. Nine counters
        // exceed one 64-byte line, but both lines still belong to a single
        // thread's shard, which is the isolation that matters.
        struct alignas(64) ResponseTimeShard {
            std::atomic<int64_t> acc_response_time_{0};
            std::atomic<int64_t> request_count_{0};
            std::atomic<int64_t> max_response_time_{0};
            std::atomic<int64_t> sample_new_{0};
            std::atomic<int64_t> un_sample_new_{0};
            std::atomic<int64_t> sample_cont_{0};
            std::atomic<int64_t> un_sample_cont_{0};
            std::atomic<int64_t> skip_new_{0};
            std::atomic<int64_t> skip_cont_{0};
        };

        ResponseTimeShard& responseTimeShard();

        // Non-owning. The agent joins the stats worker before its own
        // destruction, and this object can now outlive the agent (it is
        // shared with every AgentRuntime snapshot, which live spans and TLS
        // caches keep alive) — but only the span-facing methods
        // (collectResponseTime, add/dropActiveSpan, the incr counters) run
        // in that afterlife, and none of them reads agent_. Everything that
        // does (the worker loop, collectAgentStat) runs on threads joined
        // while the agent is alive. A shared_ptr here would form a cycle
        // and leak the agent.
        AgentService* agent_{};
        std::mutex mutex_{};
        std::condition_variable cond_var_{};
        
        // Statistics Data
        std::chrono::system_clock::time_point last_collect_time_;
        // Empty until the first successful CPU-time reading: a failed reading
        // must not become a 0 baseline (it would make the next delta span the
        // whole process lifetime and clamp to a bogus 100% load).
        std::optional<clock_t> last_sys_cpu_time_{};
        std::optional<clock_t> last_proc_cpu_time_{};
        
        std::array<ResponseTimeShard, kResponseTimeShardCount> response_time_shards_;

        ActiveSpanRegistry active_spans_;
        // Process that created this object, so the registry's shard mutexes
        // are only ever locked by the process that owns them. Set in the
        // constructor, which is the last point at which "the process this
        // registry belongs to" is unambiguous. See inheritedAcrossFork().
        pid_t owner_pid_{0};

        std::vector<AgentStatsSnapshot> agent_stats_snapshots_;
        int batch_{0};
        int collect_interval_{0};
        
        // Cached system constants
        long sc_clk_tck_{0};
        long sc_nprocessors_onln_{0};
    };
}
