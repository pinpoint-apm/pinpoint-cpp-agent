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
#include <sys/times.h>
#include <mutex>
#include <cctype>
#include <unistd.h>
#include <functional>
#include <thread>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/task_info.h>
#endif

#include "config.h"
#include "logging.h"
#include "utility.h"
#include "stat.h"

namespace pinpoint {

    AgentStats::AgentStats(AgentService* agent) : agent_(agent) {
        sc_clk_tck_ = sysconf(_SC_CLK_TCK);
        sc_nprocessors_onln_ = sysconf(_SC_NPROCESSORS_ONLN);
    }

    // RAII wrapper for FILE*
    struct FileCloser {
        FILE* fp;
        explicit FileCloser(FILE* f) : fp(f) {}
        ~FileCloser() { if (fp) fclose(fp); }
        FileCloser(const FileCloser&) = delete;
        FileCloser& operator=(const FileCloser&) = delete;
    };

    // Constants for buffer sizes (Linux /proc readers only)
#ifndef __APPLE__
    constexpr size_t kProcStatBufferSize = 256;
    constexpr size_t kProcStatusBufferSize = 256;
#endif

    // A failed reading is left empty rather than reported as 0: writing 0 into
    // the last-sample bookkeeping would make the next cycle's delta span the
    // whole process lifetime and clamp to a bogus 100% load spike.
    struct CpuTimeSample {
        std::optional<clock_t> sys_time;
        std::optional<clock_t> proc_time;
    };

    static CpuTimeSample get_cpu_time() {
        CpuTimeSample sample;

#ifdef __APPLE__
        // System-wide CPU ticks via Mach. Ticks are reported in CLK_TCK units
        // (matches sysconf(_SC_CLK_TCK)) and aggregated across all CPUs, so the
        // value is directly comparable to /proc/stat's first line on Linux.
        host_cpu_load_info_data_t cpu_info{};
        mach_msg_type_number_t info_count = HOST_CPU_LOAD_INFO_COUNT;
        // mach_host_self() returns a send right that must be released, and
        // this runs every collection interval for the life of the process.
        // The host port is process-global and stable, so acquire it once
        // and reuse it instead of leaking a port reference on every call.
        static const host_t host_port = mach_host_self();
        if (host_statistics(host_port, HOST_CPU_LOAD_INFO,
                            reinterpret_cast<host_info_t>(&cpu_info),
                            &info_count) == KERN_SUCCESS) {
            sample.sys_time = static_cast<clock_t>(cpu_info.cpu_ticks[CPU_STATE_USER]) +
                              static_cast<clock_t>(cpu_info.cpu_ticks[CPU_STATE_SYSTEM]) +
                              static_cast<clock_t>(cpu_info.cpu_ticks[CPU_STATE_NICE]);
        } else {
            LOG_WARN("host_statistics(HOST_CPU_LOAD_INFO) failed");
        }
#else
        FILE *fd = fopen("/proc/stat", "r");
        if (fd != nullptr) {
            FileCloser closer(fd);  // RAII: Automatically closes on scope exit

            char buf[kProcStatBufferSize] = {};
            // glibc's clock_t is signed; scan into unsigned long (what %lu
            // expects) and convert, instead of aliasing signed storage.
            unsigned long user = 0, nice = 0, system = 0;
            if (fgets(buf, sizeof(buf) - 1, fd) != nullptr &&
                sscanf(buf, "%*s %lu %lu %lu", &user, &nice, &system) == 3) {
                sample.sys_time = static_cast<clock_t>(user + nice + system);
            } else {
                LOG_WARN("Failed to parse /proc/stat format");
            }
        }
#endif

        // times() is POSIX and works on both Linux and macOS; tms_utime/tms_stime
        // are reported in clock ticks (sysconf(_SC_CLK_TCK)) for the calling process.
        struct tms proc_time_sample{};
        if (times(&proc_time_sample) != static_cast<clock_t>(-1)) {
            sample.proc_time = proc_time_sample.tms_utime + proc_time_sample.tms_stime;
        }

        return sample;
    }

    AgentStats::CpuLoad AgentStats::getCpuLoad(std::chrono::milliseconds dur) {
        // Millisecond precision: truncating the elapsed period to whole
        // seconds would inflate the reported load by up to ~25% when the
        // collection timer fires just short of the interval (e.g. 4.99s → 4s).
        double total_cpu = static_cast<double>(dur.count()) / 1000.0
                           * static_cast<double>(sc_clk_tck_ * sc_nprocessors_onln_);

        if (total_cpu <= 0) total_cpu = 1; // Prevent division by zero

        const auto sample = get_cpu_time();

        // A load is computed only when both the current reading and the
        // previous baseline exist; otherwise 0 is reported for this cycle and
        // the baseline is left untouched (or re-established) so a transient
        // read failure never turns into a full-lifetime delta next cycle.
        double sys_load = 0.0;
        if (sample.sys_time) {
            if (last_sys_cpu_time_) {
                sys_load = static_cast<double>(*sample.sys_time - *last_sys_cpu_time_) / total_cpu;
                if (sys_load > 1.0) { sys_load = 1.0; }
                if (sys_load < 0.0) { sys_load = 0.0; }
            }
            last_sys_cpu_time_ = sample.sys_time;
        }

        double proc_load = 0.0;
        if (sample.proc_time) {
            if (last_proc_cpu_time_) {
                proc_load = static_cast<double>(*sample.proc_time - *last_proc_cpu_time_) / total_cpu;
                if (proc_load > 1.0) { proc_load = 1.0; }
                if (proc_load < 0.0) { proc_load = 0.0; }
            }
            last_proc_cpu_time_ = sample.proc_time;
        }

        return CpuLoad{sys_load, proc_load};
    }

#ifndef __APPLE__
    // Helper to parse integers from /proc/self/status lines
    static std::optional<int64_t> parse_int_value(const char* buf, size_t buf_size, const char* prefix) {
        size_t prefix_len = strlen(prefix);
        if (strncmp(buf, prefix, prefix_len) != 0) {
            return std::nullopt;
        }

        const char* p = buf + prefix_len;
        const char* buf_end = buf + buf_size;

        // Skip whitespace (cast: passing a negative char to isspace/isdigit is UB)
        while (p < buf_end && isspace(static_cast<unsigned char>(*p))) p++;

        // Extract digits only (values may have trailing units like "kB")
        const char* digit_start = p;
        while (p < buf_end && isdigit(static_cast<unsigned char>(*p))) p++;
        if (p > digit_start) {
            return stoi_(std::string_view(digit_start, p - digit_start));
        }
        return std::nullopt;
    }
#endif

    AgentStats::ProcessStatus AgentStats::getProcessStatus() {
        ProcessStatus status{0, 0, 0};

#ifdef __APPLE__
        // Resident set size (current and peak) via Mach task_info.
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t info_count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                      reinterpret_cast<task_info_t>(&info), &info_count) == KERN_SUCCESS) {
            status.heap_alloc = static_cast<int64_t>(info.resident_size);
            status.heap_max   = static_cast<int64_t>(info.resident_size_max);
        } else {
            LOG_WARN("task_info(MACH_TASK_BASIC_INFO) failed");
        }

        // Thread count via task_threads. The kernel allocates the array, so we must
        // release the per-thread send rights and the array itself.
        thread_array_t threads = nullptr;
        mach_msg_type_number_t thread_count = 0;
        if (task_threads(mach_task_self(), &threads, &thread_count) == KERN_SUCCESS) {
            status.num_threads = static_cast<int64_t>(thread_count);
            for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
                mach_port_deallocate(mach_task_self(), threads[i]);
            }
            vm_deallocate(mach_task_self(),
                          reinterpret_cast<vm_address_t>(threads),
                          thread_count * sizeof(thread_t));
        } else {
            LOG_WARN("task_threads() failed");
        }
#else
        FILE* fd = fopen("/proc/self/status", "r");
        if (fd == nullptr) {
            return status;
        }
        FileCloser closer(fd);  // RAII: Automatically closes on scope exit

        char buf[kProcStatusBufferSize] = {};
        int found = 0;
        while (fgets(buf, sizeof(buf), fd) != nullptr) {
            if (auto val = parse_int_value(buf, sizeof(buf), "VmRSS:")) {
                status.heap_alloc = *val * 1024;  // kB to bytes
                found++;
            } else if (auto val = parse_int_value(buf, sizeof(buf), "VmPeak:")) {
                status.heap_max = *val * 1024;  // kB to bytes
                found++;
            } else if (auto val = parse_int_value(buf, sizeof(buf), "Threads:")) {
                status.num_threads = *val;
                found++;
            }
            if (found == 3) break;
        }
#endif

        return status;
    }

    void AgentStats::resetAgentStats() {
        {
            std::lock_guard<std::mutex> lock(response_time_snapshot_mutex_);
            pauseResponseTimeUpdates();
            for (auto& shard : response_time_shards_) {
                shard.acc_response_time_.store(0, std::memory_order_relaxed);
                shard.request_count_.store(0, std::memory_order_relaxed);
                shard.max_response_time_.store(0, std::memory_order_relaxed);
            }
            resumeResponseTimeUpdates();
        }
        
        sample_new_.store(0, std::memory_order_relaxed);
        un_sample_new_.store(0, std::memory_order_relaxed);
        sample_cont_.store(0, std::memory_order_relaxed);
        un_sample_cont_.store(0, std::memory_order_relaxed);
        skip_new_.store(0, std::memory_order_relaxed);
        skip_cont_.store(0, std::memory_order_relaxed);
    }

    void AgentStats::pauseResponseTimeUpdates() {
        // seq_cst pairs with the writer's fetch_add/load in collectResponseTime.
        // Both sides write one flag and then read the other's (store-buffering
        // pattern); with only acquire/release, this thread could read
        // writers_ == 0 while a writer simultaneously reads snapshotting_ ==
        // false — letting an update slip into the snapshot window.
        response_time_snapshotting_.store(true, std::memory_order_seq_cst);
        for (auto& shard : response_time_shards_) {
            while (shard.writers_.load(std::memory_order_seq_cst) != 0) {
                std::this_thread::yield();
            }
        }
    }

    void AgentStats::resumeResponseTimeUpdates() {
        response_time_snapshotting_.store(false, std::memory_order_release);
    }

    void AgentStats::collectAndResetResponseTime(int64_t& avg, int64_t& max) {
        std::lock_guard<std::mutex> lock(response_time_snapshot_mutex_);
        pauseResponseTimeUpdates();

        int64_t request_count = 0;
        int64_t acc_response_time = 0;
        max = 0;
        for (auto& shard : response_time_shards_) {
            request_count += shard.request_count_.exchange(0, std::memory_order_relaxed);
            acc_response_time += shard.acc_response_time_.exchange(0, std::memory_order_relaxed);
            const auto shard_max = shard.max_response_time_.exchange(0, std::memory_order_relaxed);
            if (shard_max > max) {
                max = shard_max;
            }
        }

        resumeResponseTimeUpdates();
        avg = request_count > 0 ? acc_response_time / request_count : 0;
    }

    void AgentStats::initAgentStats() {
        // Callers must NOT hold mutex_ (the worker calls this before taking
        // it; GrpcStats::empty_stats_queue calls it from the gRPC stats
        // thread on queue overflow / slow-channel recovery). Locking here
        // keeps batch_ and the collect-time/CPU-time bookkeeping from racing
        // the worker's collection cycle, which reads and writes the same
        // non-atomic fields under mutex_.
        std::lock_guard<std::mutex> lock(mutex_);
        // A failed reading leaves the baseline empty; getCpuLoad then reports
        // 0 and establishes the baseline on the first successful sample.
        const auto cpu_sample = get_cpu_time();
        last_sys_cpu_time_ = cpu_sample.sys_time;
        last_proc_cpu_time_ = cpu_sample.proc_time;

        resetAgentStats();

        last_collect_time_ = std::chrono::system_clock::now();
        batch_ = 0;
    }

    AgentStats::ResponseTimeShard& AgentStats::responseTimeShard() {
        // Hashed once per thread: each application thread sticks to one shard
        // for its lifetime, so its RMWs stay on a single cache line that no
        // other shard's threads touch.
        static const thread_local size_t shard_index =
            std::hash<std::thread::id>{}(std::this_thread::get_id()) % kResponseTimeShardCount;
        return response_time_shards_[shard_index];
    }

    void AgentStats::collectResponseTime(int64_t response_time) {
        auto& shard = responseTimeShard();
        for (;;) {
            while (response_time_snapshotting_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // seq_cst pairs with pauseResponseTimeUpdates (see comment there):
            // the increment must be globally visible before snapshotting_ is
            // re-read, or this writer could slip past a concurrent pause.
            shard.writers_.fetch_add(1, std::memory_order_seq_cst);
            if (!response_time_snapshotting_.load(std::memory_order_seq_cst)) {
                break;
            }
            shard.writers_.fetch_sub(1, std::memory_order_acq_rel);
        }

        shard.acc_response_time_.fetch_add(response_time, std::memory_order_relaxed);
        shard.request_count_.fetch_add(1, std::memory_order_relaxed);

        auto current_max = shard.max_response_time_.load(std::memory_order_relaxed);
        while (current_max < response_time &&
               !shard.max_response_time_.compare_exchange_weak(current_max, response_time,
                                                               std::memory_order_relaxed,
                                                               std::memory_order_relaxed)) {
        }

        shard.writers_.fetch_sub(1, std::memory_order_acq_rel);
    }

    AgentStats::ActiveSpanShard& AgentStats::activeSpanShard(int64_t spanId) {
        const auto shard_index = std::hash<int64_t>{}(spanId) % active_span_shards_.size();
        return active_span_shards_[shard_index];
    }

    void AgentStats::addActiveSpan(int64_t spanId, int64_t start_time) {
        auto& shard = activeSpanShard(spanId);
        std::lock_guard<std::mutex> lock(shard.mutex_);
        shard.spans_.insert(std::make_pair(spanId, start_time));
    }

    void AgentStats::dropActiveSpan(int64_t spanId) {
        auto& shard = activeSpanShard(spanId);
        std::lock_guard<std::mutex> lock(shard.mutex_);
        shard.spans_.erase(spanId);
    }

    void AgentStats::collectActiveRequests(int32_t active_requests[4], int64_t sample_time_ms) {
        active_requests[0] = 0;
        active_requests[1] = 0;
        active_requests[2] = 0;
        active_requests[3] = 0;

        for (auto& shard : active_span_shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex_);
            for (const auto& iter : shard.spans_) {
                auto active_time = sample_time_ms - iter.second;
                if (active_time < 1000) {
                    active_requests[0]++;
                } else if (active_time < 3000) {
                    active_requests[1]++;
                } else if (active_time < 5000) {
                    active_requests[2]++;
                } else {
                    active_requests[3]++;
                }
            }
        }
    }

    void AgentStats::collectAgentStat(AgentStatsSnapshot &stat) {
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        const auto period = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_collect_time_);
        last_collect_time_ = now;

        stat.sample_time_ = to_milli_seconds(now);
        stat.interval_ = collect_interval_;
        
        const auto cpu_load = getCpuLoad(period);
        stat.system_cpu_time_ = cpu_load.sys_load;
        stat.process_cpu_time_ = cpu_load.proc_load;
        
        const auto process_status = getProcessStatus();
        stat.heap_alloc_size_ = process_status.heap_alloc;
        stat.heap_max_size_ = process_status.heap_max;
        stat.num_threads_ = process_status.num_threads;

        // Calculate avg response time and snapshot max
        collectAndResetResponseTime(stat.response_time_avg_, stat.response_time_max_);

        // Snapshot atomics
        stat.num_sample_new_ = sample_new_.exchange(0);
        stat.num_sample_cont_ = sample_cont_.exchange(0);
        stat.num_unsample_new_ = un_sample_new_.exchange(0);
        stat.num_unsample_cont_ = un_sample_cont_.exchange(0);
        stat.num_skip_new_ = skip_new_.exchange(0);
        stat.num_skip_cont_ = skip_cont_.exchange(0);

        collectActiveRequests(stat.active_requests_, stat.sample_time_);
    }

    void AgentStats::agentStatsWorker() {
        // Boot-time decision: Stat.Enable is non-reloadable
        // (Config::retainNonReloadableFrom retains the whole stat block on a
        // reload), so a worker that returns here can never be needed later.
        const auto config = agent_->getConfig();
        if (!config->stat.enable) {
            return;
        }

        // Supervise the loop body so an unexpected exception (e.g. bad_alloc
        // while collecting a snapshot) cannot kill agent-stat reporting for
        // the process lifetime, mirroring the gRPC workers (sendMetaWorker,
        // GrpcStats::sendStatsWorker). Restarts are paced by the collect
        // interval; only agent exit ends the worker.
        while (true) {
            try {
                runAgentStatsWorker(*config);
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("agent stats worker exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("agent stats worker unknown exception");
            }

            std::unique_lock<std::mutex> lock(mutex_);
            if (cond_var_.wait_for(lock, std::chrono::milliseconds(config->stat.collect_interval),
                                   [this] { return agent_->isExiting(); })) {
                break;
            }
        }

        LOG_INFO("agent stats worker end");
    }

    void AgentStats::runAgentStatsWorker(const Config& config) {
        initAgentStats();

        {
            // Resize vector safely
            std::lock_guard<std::mutex> lock(mutex_);
            agent_stats_snapshots_.resize(config.stat.batch_count);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        collect_interval_ = config.stat.collect_interval;
        const auto timeout = std::chrono::milliseconds(collect_interval_);

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
    }

    void AgentStats::stopAgentStatsWorker() {
        std::lock_guard<std::mutex> lock(mutex_);
        cond_var_.notify_one();
    }
}
