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

    static std::chrono::system_clock::time_point last_collect_time;
    static int64_t acc_response_time, max_response_time, request_count;
    static std::atomic<int64_t> sample_new, un_sample_new, sample_cont, un_sample_cont, skip_new, skip_cont;
    static std::mutex response_time_mutex;
    static std::mutex active_span_mutex;
    static std::map<int64_t, int64_t> active_span;
    static clock_t last_sys_cpu_time, last_proc_cpu_time;

    static std::vector<AgentStatsSnapshot> agent_stats_snapshots;
    static int batch_ = 0;

    static void get_cpu_time(clock_t *sys_time, clock_t *proc_time) {
        if (sys_time) {
            FILE *fd = fopen("/proc/stat", "r");
            if (fd == nullptr) {
                return;
            }

            char buf[256];
            clock_t user = 0, nice = 0, system = 0;
            memset(buf, 0, sizeof(buf));

            if (fgets(buf, sizeof(buf) - 1, fd) == nullptr) {
                fclose(fd);
                return;
            }
            fclose(fd);

            sscanf(buf, "%*s %lu %lu %lu %*u %*u %*u %*u %*u %*u %*u", &user, &nice, &system);
            *sys_time = user + system + nice;
        }

        if (proc_time) {
            struct tms proc_time_sample{};

            times(&proc_time_sample);
            *proc_time = proc_time_sample.tms_utime + proc_time_sample.tms_stime;
        }
    }

    //CPU currently used by current process
    static void get_cpu_load(std::chrono::seconds dur, double* sys_load, double* proc_load) {
        clock_t sys_time = 0, proc_time = 0;
        double total_cpu = dur.count() * sysconf(_SC_CLK_TCK) * sysconf(_SC_NPROCESSORS_ONLN);

        get_cpu_time(&sys_time, &proc_time);

        clock_t sys_cpu = sys_time - last_sys_cpu_time;
        *sys_load = sys_cpu / total_cpu;
        if (*sys_load > 1) { *sys_load = 1; }

        clock_t proc_cpu = proc_time - last_proc_cpu_time;
        *proc_load = proc_cpu / total_cpu;
        if (*proc_load > 1) { *proc_load = 1; }

        last_sys_cpu_time = sys_time;
        last_proc_cpu_time = proc_time;
    }

    //parse string like: 'VmSize:    63580 kB'
    static int parse_mem_size(char* buf, size_t buf_size) {
        size_t i = strlen(buf);
        if (i >= buf_size) {
            return 0;
        }

        const char* p = buf;
        const char* bufEnd = buf + buf_size;

        while (!isdigit(*p)) {
            p++;
            if (p >= bufEnd) {
                return 0;
            }
        }

        buf[i - 3] = '\0';
        auto result = stoi_(p);
        return result.value_or(0);
    }


    static int parse_thread_count(const char* buf, size_t buf_size) {
        if (size_t i = strlen(buf); i >= buf_size) {
            return 0;
        }

        const char* p = buf;
        const char* buf_end = buf + buf_size;

        while (!isdigit(*p)) {
            p++;
            if (p >= buf_end) {
                return 0;
            }
        }

        auto result = stoi_(p);
        return result.value_or(0);
    }

    //Virtual Memory(Kb) currently used by current process
    static void get_process_status(int64_t *heap_alloc, int64_t *heap_max, int64_t *num_threads) {
        *heap_alloc = 0;
        *heap_max = 0;

        FILE* fd = fopen("/proc/self/status", "r");
        if (fd == nullptr) {
            return;
        }

        char buf[256] = {};
        while (fgets(buf, sizeof(buf), fd) != nullptr) {
            if (strncmp(buf, "VmSize:", 7) == 0) {
                *heap_alloc = parse_mem_size(buf, sizeof(buf));
            } else if (strncmp(buf, "VmPeak:", 7) == 0) {
                *heap_max = parse_mem_size(buf, sizeof(buf));
            } else if (strncmp(buf, "Threads:", 8) == 0) {
                *num_threads = parse_thread_count(buf, sizeof(buf));
            }
        }

        fclose(fd);
    }

    static void reset_agent_stats() {
        acc_response_time = 0;
        request_count = 0;
        max_response_time = 0;
        sample_new = 0;
        un_sample_new = 0;
        sample_cont = 0;
        un_sample_cont = 0;
        skip_new = 0;
        skip_cont = 0;
    }

    static int64_t get_response_time_avg() {
        if (request_count > 0) {
            return acc_response_time / request_count;
        }

        return 0;
    }

    void init_agent_stats() {
        last_sys_cpu_time = last_proc_cpu_time = 0;
        get_cpu_time(&last_sys_cpu_time, &last_proc_cpu_time);
        reset_agent_stats();
        last_collect_time = std::chrono::system_clock::now();
        batch_ = 0;
    }

    void collect_response_time(int64_t response_time) {
        std::unique_lock<std::mutex> lock(response_time_mutex);

        acc_response_time += response_time;
        request_count++;

        if (max_response_time < response_time) {
            max_response_time = response_time;
        }
    }

    void incr_sample_new() {
        ++sample_new;
    }

    void incr_unsample_new() {
        ++un_sample_new;
    }

    void incr_sample_cont() {
        ++sample_cont;
    }

    void incr_unsample_cont() {
        ++un_sample_cont;
    }

    void incr_skip_new() {
        ++skip_new;
    }

    void incr_skip_cont() {
        ++skip_cont;
    }

    void add_active_span(int64_t spanId, int64_t start_time) {
        std::unique_lock<std::mutex> lock(active_span_mutex);
        active_span.insert(std::make_pair(spanId, start_time));
    }

    void drop_active_span(int64_t spanId) {
        std::unique_lock<std::mutex> lock(active_span_mutex);
        active_span.erase(spanId);
    }

    void collect_agent_stat(AgentStatsSnapshot &stat) {
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        std::chrono::seconds period = std::chrono::duration_cast<std::chrono::seconds>(now - last_collect_time);
        last_collect_time = now;

        stat.sample_time_ = to_milli_seconds(now);
        get_cpu_load(period, &stat.system_cpu_time_, &stat.process_cpu_time_);
        get_process_status(&stat.heap_alloc_size_, &stat.heap_max_size_, &stat.num_threads_);

        stat.response_time_avg_ = get_response_time_avg();
        stat.response_time_max_ = max_response_time;

        stat.num_sample_new_ = sample_new;
        stat.num_sample_cont_ = sample_cont;
        stat.num_unsample_new_ = un_sample_new;
        stat.num_unsample_cont_ = un_sample_cont;
        stat.num_skip_new_ = skip_new;
        stat.num_skip_cont_ = skip_cont;

        stat.active_requests_[0] = 0;
        stat.active_requests_[1] = 0;
        stat.active_requests_[2] = 0;
        stat.active_requests_[3] = 0;

        std::unique_lock<std::mutex> lock(active_span_mutex);
        for (auto iter : active_span) {
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

        reset_agent_stats();
    }

    std::vector<AgentStatsSnapshot>& get_agent_stat_snapshots() {
        return agent_stats_snapshots;
    }

    void AgentStats::agentStatsWorker() try {
        auto& config = agent_->getConfig();
        if (!config.stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        const auto timeout = std::chrono::milliseconds(config.stat.collect_interval);

        init_agent_stats();
        agent_stats_snapshots.resize(config.stat.batch_count);

        while (!agent_->isExiting()) {
            if (!cond_var_.wait_for(lock, timeout, [this]{ return agent_->isExiting(); })) {
                collect_agent_stat(agent_stats_snapshots.at(batch_));
                batch_++;

                if (batch_ == config.stat.batch_count) {
                    agent_->recordStats(AGENT_STATS);
                    batch_ = 0;
                }
            }
        }

        LOG_INFO("agent stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("agent stats worker exception = {}", e.what());
    }

    void AgentStats::stopAgentStatsWorker() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_var_.notify_one();
    }
}
