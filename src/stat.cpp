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

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "sys/times.h"
#include <map>
#include <mutex>
#include <cctype>
#include <unistd.h>

#include "config.h"
#include "logging.h"
#include "utility.h"
#include "stat.h"

namespace pinpoint {

    // Global singleton pointer for C-style wrapper functions
    static AgentStats* g_agent_stats_instance = nullptr;

    AgentStats* AgentStats::getInstance() {
        return g_agent_stats_instance;
    }

    void AgentStats::setInstance(AgentStats* instance) {
        g_agent_stats_instance = instance;
    }

    AgentStats::AgentStats(AgentService* agent) : agent_(agent) {
        sc_clk_tck_ = sysconf(_SC_CLK_TCK);
        sc_nprocessors_onln_ = sysconf(_SC_NPROCESSORS_ONLN);
        setInstance(this);
    }

    // RAII wrapper for FILE*
    struct FileCloser {
        FILE* fp;
        explicit FileCloser(FILE* f) : fp(f) {}
        ~FileCloser() { if (fp) fclose(fp); }
        FileCloser(const FileCloser&) = delete;
        FileCloser& operator=(const FileCloser&) = delete;
    };

    static void get_cpu_time(clock_t *sys_time, clock_t *proc_time) {
        if (sys_time) {
            FILE *fd = fopen("/proc/stat", "r");
            if (fd == nullptr) {
                return;
            }
            FileCloser closer(fd);  // RAII: Automatically closes on scope exit
            
            char buf[256];
            clock_t user = 0, nice = 0, system = 0;
            memset(buf, 0, sizeof(buf));

            if (fgets(buf, sizeof(buf) - 1, fd) != nullptr) {
                if (sscanf(buf, "%*s %lu %lu %lu", &user, &nice, &system) == 3) {
                    *sys_time = user + system + nice;
                } else {
                    LOG_WARN("Failed to parse /proc/stat format");
                    *sys_time = 0;
                }
            }
        }

        if (proc_time) {
            struct tms proc_time_sample{};
            times(&proc_time_sample);
            *proc_time = proc_time_sample.tms_utime + proc_time_sample.tms_stime;
        }
    }

    void AgentStats::getCpuLoad(std::chrono::seconds dur, double* sys_load, double* proc_load) {
        clock_t sys_time = 0, proc_time = 0;
        double total_cpu = static_cast<double>(dur.count() * sc_clk_tck_ * sc_nprocessors_onln_);
        
        if (total_cpu <= 0) total_cpu = 1; // Prevent division by zero

        get_cpu_time(&sys_time, &proc_time);

        clock_t sys_cpu = sys_time - last_sys_cpu_time_;
        *sys_load = static_cast<double>(sys_cpu) / total_cpu;
        if (*sys_load > 1.0) { *sys_load = 1.0; }
        if (*sys_load < 0.0) { *sys_load = 0.0; }

        clock_t proc_cpu = proc_time - last_proc_cpu_time_;
        *proc_load = static_cast<double>(proc_cpu) / total_cpu;
        if (*proc_load > 1.0) { *proc_load = 1.0; }
        if (*proc_load < 0.0) { *proc_load = 0.0; }

        last_sys_cpu_time_ = sys_time;
        last_proc_cpu_time_ = proc_time;
    }

    // Helper to parse integers from buffer
    static int parse_int_value(const char* buf, size_t buf_size, const char* prefix) {
        size_t prefix_len = strlen(prefix);
        if (strncmp(buf, prefix, prefix_len) != 0) {
            return -1; // Not a match
        }
        
        const char* p = buf + prefix_len;
        const char* buf_end = buf + buf_size;
        
        // Skip whitespace
        while (p < buf_end && isspace(*p)) p++;
        
        // Find digits
        if (p < buf_end && isdigit(*p)) {
            auto result = stoi_(p);
            return result.value_or(0);
        }
        return 0;
    }

    void AgentStats::getProcessStatus(int64_t *heap_alloc, int64_t *heap_max, int64_t *num_threads) {
        *heap_alloc = 0;
        *heap_max = 0;
        *num_threads = 0;

        FILE* fd = fopen("/proc/self/status", "r");
        if (fd == nullptr) {
            return;
        }
        FileCloser closer(fd);  // RAII: Automatically closes on scope exit

        char buf[256] = {};
        while (fgets(buf, sizeof(buf), fd) != nullptr) {
            int val = -1;
            
            if ((val = parse_int_value(buf, sizeof(buf), "VmSize:")) != -1) {
                *heap_alloc = val;
            } else if ((val = parse_int_value(buf, sizeof(buf), "VmPeak:")) != -1) {
                *heap_max = val;
            } else if ((val = parse_int_value(buf, sizeof(buf), "Threads:")) != -1) {
                *num_threads = val;
            }
        }
    }

    void AgentStats::resetAgentStats() {
        std::lock_guard<std::mutex> lock(response_time_mutex_);
        acc_response_time_ = 0;
        request_count_ = 0;
        max_response_time_ = 0;
        
        sample_new_ = 0;
        un_sample_new_ = 0;
        sample_cont_ = 0;
        un_sample_cont_ = 0;
        skip_new_ = 0;
        skip_cont_ = 0;
    }

    int64_t AgentStats::getResponseTimeAvg() {
        // Access protected by mutex in collectAgentStat call context or we should lock here?
        // collectAgentStat calls this, and it is the only consumer that matters for averaging logic reset.
        // However, request_count_ can change.
        // For thread safety during calculation, we should hold the lock, but resetAgentStats also resets it.
        // Since collectAgentStat calls resetAgentStats immediately after, we are safe within the single thread of worker,
        // but other threads are updating request_count_.
        
        // Actually, simpler: accumulate in atomic or use lock. 
        // We already have response_time_mutex_ for updates. Let's use it.
        std::lock_guard<std::mutex> lock(response_time_mutex_);
        if (request_count_ > 0) {
            return acc_response_time_ / request_count_;
        }
        return 0;
    }

    void AgentStats::initAgentStats() {
        last_sys_cpu_time_ = 0;
        last_proc_cpu_time_ = 0;
        get_cpu_time(&last_sys_cpu_time_, &last_proc_cpu_time_);
        
        resetAgentStats();
        
        last_collect_time_ = std::chrono::system_clock::now();
        batch_ = 0;
    }

    void AgentStats::collectResponseTime(int64_t response_time) {
        std::lock_guard<std::mutex> lock(response_time_mutex_);

        acc_response_time_ += response_time;
        request_count_++;

        if (max_response_time_ < response_time) {
            max_response_time_ = response_time;
        }
    }

    void AgentStats::addActiveSpan(int64_t spanId, int64_t start_time) {
        std::lock_guard<std::mutex> lock(active_span_mutex_);
        active_span_map_.insert(std::make_pair(spanId, start_time));
    }

    void AgentStats::dropActiveSpan(int64_t spanId) {
        std::lock_guard<std::mutex> lock(active_span_mutex_);
        active_span_map_.erase(spanId);
    }

    void AgentStats::collectAgentStat(AgentStatsSnapshot &stat) {
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        std::chrono::seconds period = std::chrono::duration_cast<std::chrono::seconds>(now - last_collect_time_);
        last_collect_time_ = now;

        stat.sample_time_ = to_milli_seconds(now);
        getCpuLoad(period, &stat.system_cpu_time_, &stat.process_cpu_time_);
        getProcessStatus(&stat.heap_alloc_size_, &stat.heap_max_size_, &stat.num_threads_);

        // Calculate avg response time and snapshot max
        {
            std::lock_guard<std::mutex> lock(response_time_mutex_);
            if (request_count_ > 0) {
                stat.response_time_avg_ = acc_response_time_ / request_count_;
            } else {
                stat.response_time_avg_ = 0;
            }
            stat.response_time_max_ = max_response_time_;
            
            // Reset logic moved here to be under the same lock
            acc_response_time_ = 0;
            request_count_ = 0;
            max_response_time_ = 0;
        }

        // Snapshot atomics
        stat.num_sample_new_ = sample_new_.exchange(0);
        stat.num_sample_cont_ = sample_cont_.exchange(0);
        stat.num_unsample_new_ = un_sample_new_.exchange(0);
        stat.num_unsample_cont_ = un_sample_cont_.exchange(0);
        stat.num_skip_new_ = skip_new_.exchange(0);
        stat.num_skip_cont_ = skip_cont_.exchange(0);

        stat.active_requests_[0] = 0;
        stat.active_requests_[1] = 0;
        stat.active_requests_[2] = 0;
        stat.active_requests_[3] = 0;

        {
            std::lock_guard<std::mutex> lock(active_span_mutex_);
            for (const auto& iter : active_span_map_) {
                auto active_time = stat.sample_time_ - iter.second;
                if (active_time < 1000) {
                    stat.active_requests_[0]++;
                } else if (active_time < 3000) {
                    stat.active_requests_[1]++;
                } else if (active_time < 5000) {
                    stat.active_requests_[2]++;
                } else {
                    stat.active_requests_[3]++;
                }
            }
        }
    }

    void AgentStats::agentStatsWorker() try {
        auto& config = agent_->getConfig();
        if (!config.stat.enable) {
            return;
        }

        initAgentStats();
        
        {
            // Resize vector safely
            std::lock_guard<std::mutex> lock(mutex_);
            agent_stats_snapshots_.resize(config.stat.batch_count);
        }
        
        std::unique_lock<std::mutex> lock(mutex_);
        const auto timeout = std::chrono::milliseconds(config.stat.collect_interval);

        while (!agent_->isExiting()) {
            if (!cond_var_.wait_for(lock, timeout, [this]{ return agent_->isExiting(); })) {
                // Period elapsed, collect stats
                // Unlock while collecting to not block other operations? 
                // collectAgentStat locks its own internal mutexes, so it's fine to hold outer lock or not.
                // But 'agent_stats_snapshots_' is protected by 'mutex_'.
                
                if (static_cast<size_t>(batch_) < agent_stats_snapshots_.size()) {
                    collectAgentStat(agent_stats_snapshots_[batch_]);
                    batch_++;
                }

                if (batch_ >= config.stat.batch_count) {
                    // Release lock while sending data to avoid blocking stop/collect
                    lock.unlock();
                    agent_->recordStats(AGENT_STATS);
                    lock.lock();
                    
                    batch_ = 0;
                }
            }
        }

        LOG_INFO("agent stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("agent stats worker exception = {}", e.what());
    }

    void AgentStats::stopAgentStatsWorker() {
        std::lock_guard<std::mutex> lock(mutex_);
        cond_var_.notify_one();
    }

    // Global Wrapper Functions
    void collect_response_time(int64_t resTime) {
        if (auto* instance = AgentStats::getInstance()) instance->collectResponseTime(resTime);
    }
    void incr_sample_new() {
        if (auto* instance = AgentStats::getInstance()) instance->incrSampleNew();
    }
    void incr_unsample_new() {
        if (auto* instance = AgentStats::getInstance()) instance->incrUnsampleNew();
    }
    void incr_sample_cont() {
        if (auto* instance = AgentStats::getInstance()) instance->incrSampleCont();
    }
    void incr_unsample_cont() {
        if (auto* instance = AgentStats::getInstance()) instance->incrUnsampleCont();
    }
    void incr_skip_new() {
        if (auto* instance = AgentStats::getInstance()) instance->incrSkipNew();
    }
    void incr_skip_cont() {
        if (auto* instance = AgentStats::getInstance()) instance->incrSkipCont();
    }
    void add_active_span(int64_t spanId, int64_t start_time) {
        if (auto* instance = AgentStats::getInstance()) instance->addActiveSpan(spanId, start_time);
    }
    void drop_active_span(int64_t spanId) {
        if (auto* instance = AgentStats::getInstance()) instance->dropActiveSpan(spanId);
    }
    std::vector<AgentStatsSnapshot>& get_agent_stat_snapshots() {
        static std::vector<AgentStatsSnapshot> empty;
        if (auto* instance = AgentStats::getInstance()) return instance->getSnapshots();
        return empty;
    }

}
