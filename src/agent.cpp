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

#include <cassert>
#include <csignal>
#include <string>
#include <exception>
#include <iterator>
#include <utility>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>
#include <pthread.h>
#include <unistd.h>

#include "logging.h"
#include "noop.h"
#include "agent.h"
#include "sql.h"
#include "utility.h"

namespace pinpoint {

    // Global agent singleton whose readers bypass the writer mutex
    namespace {
        // Serializes global-agent WRITERS only (StartAgent, Shutdown, the
        // test helpers). Readers never take it: GlobalAgent() goes through the
        // AtomicSharedPtr below, so per-request lookups cannot contend with a
        // create/reload that holds this mutex across make_config()'s file I/O
        // and YAML parsing.
        std::mutex global_agent_mutex;

        // The holder is intentionally heap-allocated and never destroyed so
        // its own static destruction cannot release the last AgentImpl.
        // Tearing the agent down during __cxa_atexit (thread joins, gRPC
        // channel teardown, logging through possibly-destroyed singletons) is
        // unsafe for a library embedded in a host application. A global agent
        // is therefore torn down through explicit Shutdown(); a non-global
        // instance can still die when its last owner releases it.
        //
        // SnapshotCache::Uncached (the AtomicSharedPtr default) is load-bearing
        // for the same invariant: a ThreadCached holder would pin the agent in
        // every reader thread's TLS until that thread's next load or exit,
        // deferring the final release — and ~AgentImpl — into thread/process
        // teardown, exactly where the leak above forbids it.
        AtomicSharedPtr<AgentImpl>& global_agent() {
            static auto* holder = new AtomicSharedPtr<AgentImpl>();
            return *holder;
        }

        // Hard wall-clock bound for the blocking phase of do_shutdown().
        // gRPC cancellation is best-effort (TryCancel gives no completion
        // bound) and the config watcher can sit in a filesystem call on a
        // hung mount, so the joins themselves cannot be made individually
        // bounded; instead the whole blocking teardown runs under this
        // deadline and is handed to a detached reaper when it expires (see
        // teardown_workers_with_deadline()). Overridable for tests via
        // set_agent_shutdown_deadline().
        constexpr std::chrono::milliseconds kDefaultShutdownDeadline{3000};
        std::atomic<std::chrono::milliseconds::rep> shutdown_deadline_ms{
            kDefaultShutdownDeadline.count()};

        std::chrono::milliseconds agent_shutdown_deadline() {
            return std::chrono::milliseconds{shutdown_deadline_ms.load(std::memory_order_relaxed)};
        }
    }

    void set_agent_shutdown_deadline(std::chrono::milliseconds deadline) {
        shutdown_deadline_ms.store(deadline.count() > 0 ? deadline.count()
                                                        : kDefaultShutdownDeadline.count(),
                                   std::memory_order_relaxed);
    }

    AgentImpl::AgentImpl(std::shared_ptr<const Config> cfg,
                         std::unique_ptr<GrpcAgent> grpc_agent,
                         std::unique_ptr<GrpcMetadata> grpc_metadata,
                         std::unique_ptr<GrpcSpan> grpc_span,
                         std::unique_ptr<GrpcStats> grpc_stat,
                         std::unique_ptr<GrpcCommand> grpc_command,
                         int32_t app_type,
                         size_t cache_size) :
        grpc_agent_(std::move(grpc_agent)),
        grpc_metadata_(std::move(grpc_metadata)),
        grpc_span_(std::move(grpc_span)),
        grpc_stat_(std::move(grpc_stat)),
        grpc_command_(std::move(grpc_command)),
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        trace_id_sequence_(1) {

        // cfg is required: build_runtime() below dereferences it, and every
        // caller (make_agent(), the tests) hands in a validated config. A null
        // cfg is a programming error, not a runtime condition to tolerate.
        assert(cfg);

        // Snapshot the immutable identity fields once. isReloadable() guarantees
        // they never change for this agent, so the per-request getters below can
        // serve them without touching the atomic runtime_.
        app_type_ = app_type;
        app_name_ = cfg->app_name_;
        agent_id_ = std::make_shared<const std::string>(cfg->agent_id_);
        agent_name_ = cfg->agent_name_;
        service_name_ = cfg->service_name_;

        agent_stats_ = std::make_unique<AgentStats>(this);
        url_stats_ = std::make_unique<UrlStats>(this);

        // Each cache shards its store by kDefaultCacheShardCount (see
        // ShardedLruCache): the api cache in particular is hit once per span
        // plus once per named span event, so a single rwlock's cache line
        // would ping-pong across all request threads.
        api_cache_ = std::make_unique<ApiIdCache>(cache_size);
        error_cache_ = std::make_unique<IdCache>(cache_size);
        sql_cache_ = std::make_unique<IdCache>(cache_size);
        sql_uid_cache_ = std::make_unique<SqlUidCache>(cache_size);
        raw_sql_id_cache_ = std::make_unique<RawSqlCache>(cache_size);
        raw_sql_uid_cache_ = std::make_unique<RawSqlCache>(cache_size);

        // Initial build: no previous runtime, so every component is created
        // and published together in one atomic store. The constructor stays
        // "cold": it starts no threads, opens no gRPC channel and installs no
        // config-file watcher — Start() does all of that. The split keeps
        // construction infallible past this point and lets tests exercise a
        // built-but-offline agent.
        apply_config(nullptr, std::move(cfg));
    }

    namespace {
        // Blocks (nearly) all signals on the calling thread for the enclosing
        // scope, restoring the previous mask on exit. Threads created inside
        // the scope inherit the blocked mask, and threads THEY create — gRPC's
        // internal threads and the worker pool spawned by the init thread —
        // inherit it transitively. Signal-driven hosts (an nginx worker's
        // event loop relies on a process-directed signal interrupting its own
        // epoll_wait) therefore keep receiving signals on a host thread,
        // never on an agent thread. The synchronous fatal signals — the
        // hardware faults and seccomp's SIGSYS — stay unblocked: a crash (or
        // seccomp violation) inside an agent thread must still raise the
        // normal fatal signal instead of undefined behavior.
        class ScopedSignalBlock {
        public:
            ScopedSignalBlock() noexcept {
                sigset_t all_blocked;
                sigfillset(&all_blocked);
                sigdelset(&all_blocked, SIGABRT);
                sigdelset(&all_blocked, SIGBUS);
                sigdelset(&all_blocked, SIGFPE);
                sigdelset(&all_blocked, SIGILL);
                sigdelset(&all_blocked, SIGSEGV);
                sigdelset(&all_blocked, SIGSYS);
                restore_ = pthread_sigmask(SIG_SETMASK, &all_blocked, &saved_) == 0;
            }
            ~ScopedSignalBlock() noexcept {
                if (restore_) {
                    pthread_sigmask(SIG_SETMASK, &saved_, nullptr);
                }
            }
            ScopedSignalBlock(const ScopedSignalBlock&) = delete;
            ScopedSignalBlock& operator=(const ScopedSignalBlock&) = delete;

        private:
            sigset_t saved_{};
            bool restore_{false};
        };
    }

    bool AgentImpl::Start() noexcept try {
        // Serialized against do_shutdown() (see lifecycle_mutex_): a
        // concurrent Shutdown() waits for the writes to owner_pid_ and
        // init_thread_ below to be published before tearing down, and a
        // Start() arriving after (or during) shutdown refuses instead of
        // spawning workers nobody will ever join.
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (shutting_down_) {
            // Terminal, not transient: shutting_down_ is never cleared, so this
            // instance stays offline for the rest of the process and every span
            // it hands out is a noop span. Logged at error level in addition
            // to the false return, because the usual cause is a host trying
            // to restart a shut-down agent instead of building a fresh one
            // through StartAgent().
            LOG_ERROR("agent start rejected: this agent was shut down and cannot "
                      "be restarted; create a new agent with StartAgent()");
            return false;
        }

        // An agent object inherited across fork() must not be (re)started
        // here: the parent's Start() already claimed it (its threads and any
        // gRPC runtime live in the parent only), and gRPC cannot be freshly
        // initialized in a child forked after grpc_init. The supported model
        // is StartAgent() in the process that traces — a master must not
        // start an agent before forking.
        if (owner_pid_ != 0 && owner_pid_ != getpid()) {
            warn_fork_inheritance();
            return false;
        }

        // Once a Start() launches initialization, later calls are successful
        // no-ops: the agent IS running. Synchronous setup failures reset
        // started_ in the catch handlers below, and the false return makes
        // StartAgent() skip publishing, so the next StartAgent() call
        // rebuilds and retries from scratch.
        if (started_.exchange(true)) {
            return true;
        }

        owner_pid_ = getpid();

        // NOTE on gRPC and fork(): the constructor triggers no grpc_init — no
        // channel, stub or credentials are built until openChannel() runs from
        // init_grpc_workers below — so this Start() performs this process's
        // first, fresh gRPC initialization. This is gRPC's own "instantiate
        // gRPC objects only after fork()" pattern, which needs no
        // pthread_atfork handlers or GRPC_ENABLE_FORK_SUPPORT. (It assumes the
        // HOST application likewise does not use gRPC before forking.)

        // Every agent thread descends from the two spawns below, so blocking
        // signals for the rest of this function keeps process-directed
        // signals away from all of them (see ScopedSignalBlock). The caller's
        // mask is restored when Start() returns or unwinds.
        ScopedSignalBlock signal_block;

        // Start the config-file watcher BEFORE spawning init_thread_ so that, if
        // thread creation throws, no joinable std::thread member exists yet and
        // the stack unwinds without hitting a joinable-thread destructor.
        // The watcher is opt-in (EnableConfigFileWatcher, default off) and the
        // toggle is consumed only here, which is why it is non-reloadable.
        const auto watch_path = resolve_config_file_path(options_);
        if (!watch_path.empty() && getConfig()->enable_config_file_watcher) {
            config_watcher_ = std::make_unique<ConfigFileWatcher>(watch_path, [this] {
                // make_config(options_, old) returns the final reload config:
                // non-reloadable fields retained from the running config and
                // the logger already reconfigured. It re-reads the config
                // file itself, so no separate read is needed here. This runs
                // on the watcher thread; teardown_workers() joins the watcher
                // while the agent is still guaranteed alive (by the shutdown
                // caller or the deadline runner's keep-alive), so `this`
                // stays valid for the whole callback.
                auto new_cfg = make_config(options_, getConfig());
                if (new_cfg) {
                    reloadConfig(std::move(new_cfg));
                    LOG_INFO("agent config reloaded");
                }
            });
            config_watcher_->start();
        }

        try {
            init_thread_ = std::thread{&AgentImpl::init_grpc_workers, this};
        } catch (...) {
            if (config_watcher_) {
                config_watcher_->stop();
            }
            throw;
        }
        return true;
    } catch (const std::exception& e) {
        try { LOG_ERROR("agent start failed: exception = {}", e.what()); } catch (...) {}
        enabled_ = false;
        started_ = false;
        return false;
    } catch (...) {
        try { LOG_ERROR("agent start failed: unknown exception"); } catch (...) {}
        enabled_ = false;
        started_ = false;
        return false;
    }

    void AgentImpl::warn_fork_inheritance() const noexcept {
        if (fork_misuse_warned_.exchange(true)) {
            return;
        }
        try {
            LOG_ERROR("agent was started in another process (pid {}); this process inherited it "
                      "across fork() and cannot use it — call StartAgent() in this process instead",
                      static_cast<long>(owner_pid_));
        } catch (...) {}
    }

    std::shared_ptr<const Config> AgentImpl::getConfig() const {
        const auto& runtime = runtime_.load_cached_ref();
        return runtime ? runtime->config : nullptr;
    }

    // Served from the construction-time snapshot (see ctor): these never change
    // for the agent's lifetime, so no atomic config_ load is needed.
    const std::string& AgentImpl::getAppName() const {
        return app_name_;
    }

    int32_t AgentImpl::getAppType() const {
        return app_type_;
    }

    const std::string& AgentImpl::getAgentId() const {
        return *agent_id_;
    }

    const std::string& AgentImpl::getAgentName() const {
        return agent_name_;
    }

    const std::string& AgentImpl::getServiceName() const {
        return service_name_;
    }

    void AgentImpl::build_header_recorders(AgentRuntime& rt, const Config& cfg) {
        struct HeaderRecorderConfig {
            HeaderType type;
            int32_t annotation_key;
            const std::vector<std::string>& config_value;
        };

        const HeaderRecorderConfig server_configs[] = {
            {HTTP_REQUEST, ANNOTATION_HTTP_REQUEST_HEADER, cfg.http.server.rec_request_header},
            {HTTP_RESPONSE, ANNOTATION_HTTP_RESPONSE_HEADER, cfg.http.server.rec_response_header},
            {HTTP_COOKIE, ANNOTATION_HTTP_COOKIE, cfg.http.server.rec_request_cookie}
        };

        const HeaderRecorderConfig client_configs[] = {
            {HTTP_REQUEST, ANNOTATION_HTTP_REQUEST_HEADER, cfg.http.client.rec_request_header},
            {HTTP_RESPONSE, ANNOTATION_HTTP_RESPONSE_HEADER, cfg.http.client.rec_response_header},
            {HTTP_COOKIE, ANNOTATION_HTTP_COOKIE, cfg.http.client.rec_request_cookie}
        };

        for (const auto& recorder_cfg : server_configs) {
            if (!recorder_cfg.config_value.empty()) {
                rt.http_srv_header_recorder[recorder_cfg.type] =
                    std::make_shared<HttpHeaderRecorder>(recorder_cfg.annotation_key, recorder_cfg.config_value);
            }
        }

        for (const auto& recorder_cfg : client_configs) {
            if (!recorder_cfg.config_value.empty()) {
                rt.http_cli_header_recorder[recorder_cfg.type] =
                    std::make_shared<HttpHeaderRecorder>(recorder_cfg.annotation_key, recorder_cfg.config_value);
            }
        }
    }

    namespace {
        // The reloadable components below are each derived from a specific slice
        // of Config. These predicates tell apply_config() which slices changed so
        // it can leave the other live components (and their warmed-up state)
        // untouched across a reload.
        bool sampling_config_changed(const Config& a, const Config& b) {
            return a.sampling.type != b.sampling.type
                || a.sampling.counter_rate != b.sampling.counter_rate
                || a.sampling.percent_rate != b.sampling.percent_rate
                || a.sampling.new_throughput != b.sampling.new_throughput
                || a.sampling.cont_throughput != b.sampling.cont_throughput;
        }

        bool header_recorder_config_changed(const Config& a, const Config& b) {
            return a.http.server.rec_request_header != b.http.server.rec_request_header
                || a.http.server.rec_response_header != b.http.server.rec_response_header
                || a.http.server.rec_request_cookie != b.http.server.rec_request_cookie
                || a.http.client.rec_request_header != b.http.client.rec_request_header
                || a.http.client.rec_response_header != b.http.client.rec_response_header
                || a.http.client.rec_request_cookie != b.http.client.rec_request_cookie;
        }
    }

    std::shared_ptr<const AgentRuntime> AgentImpl::build_runtime(
            const std::shared_ptr<const AgentRuntime>& old_rt,
            std::shared_ptr<const Config> cfg) {
        // A null old_rt means the initial construction, where nothing has been
        // built yet, so every component must be created. On a reload we already
        // have live components and only rebuild the ones whose backing config
        // changed — rebuilding unchanged components would needlessly discard
        // their accumulated state (e.g. the throughput sampler's counters).
        // Unchanged components are shared with the previous runtime instead.
        const Config* old_cfg = old_rt ? old_rt->config.get() : nullptr;

        auto rt = std::make_shared<AgentRuntime>();
        rt->config = std::move(cfg);
        const Config& c = *rt->config;

        // Rebuild sampler
        if (!old_cfg || sampling_config_changed(*old_cfg, c)) {
            std::unique_ptr<Sampler> sampler;
            if (compare_string(c.sampling.type, PERCENT_SAMPLING)) {
                sampler = std::make_unique<PercentSampler>(c.sampling.percent_rate);
            } else {
                sampler = std::make_unique<CounterSampler>(c.sampling.counter_rate);
            }

            if (c.sampling.new_throughput > 0 || c.sampling.cont_throughput > 0) {
                rt->sampler = std::make_shared<ThroughputLimitTraceSampler>(this, std::move(sampler),
                                                                            c.sampling.new_throughput,
                                                                            c.sampling.cont_throughput);
            } else {
                rt->sampler = std::make_shared<BasicTraceSampler>(this, std::move(sampler));
            }
        } else {
            rt->sampler = old_rt->sampler;
        }

        // Rebuild HTTP filters
        if (!old_cfg || old_cfg->http.server.exclude_url != c.http.server.exclude_url) {
            if (!c.http.server.exclude_url.empty()) {
                rt->http_url_filter = std::make_shared<HttpUrlFilter>(c.http.server.exclude_url);
            }
        } else {
            rt->http_url_filter = old_rt->http_url_filter;
        }

        if (!old_cfg || old_cfg->http.server.exclude_method != c.http.server.exclude_method) {
            if (!c.http.server.exclude_method.empty()) {
                rt->http_method_filter = std::make_shared<HttpMethodFilter>(c.http.server.exclude_method);
            }
        } else {
            rt->http_method_filter = old_rt->http_method_filter;
        }

        if (!old_cfg || old_cfg->http.server.status_errors != c.http.server.status_errors) {
            if (!c.http.server.status_errors.empty()) {
                rt->http_status_errors = std::make_shared<HttpStatusErrors>(c.http.server.status_errors);
            }
        } else {
            rt->http_status_errors = old_rt->http_status_errors;
        }

        // Rebuild header recorders
        if (!old_cfg || header_recorder_config_changed(*old_cfg, c)) {
            build_header_recorders(*rt, c);
        } else {
            for (size_t i = 0; i < 3; ++i) {
                rt->http_srv_header_recorder[i] = old_rt->http_srv_header_recorder[i];
                rt->http_cli_header_recorder[i] = old_rt->http_cli_header_recorder[i];
            }
        }

        return rt;
    }

    void AgentImpl::apply_config(const std::shared_ptr<const AgentRuntime>& old_rt,
                                 std::shared_ptr<const Config> cfg) {
        // Refresh the prepareSql() fast-path flag (see raw_sql_cache_enabled_).
        // Relaxed and slightly ahead of the runtime_ swap below: prepareSql
        // tolerates a stale value for the instant around a reload.
        raw_sql_cache_enabled_.store(cfg->sql.enable_raw_sql_cache,
                                     std::memory_order_relaxed);
        runtime_.store(build_runtime(old_rt, std::move(cfg)));
    }

    void AgentImpl::reloadConfig(std::shared_ptr<const Config> cfg) {
        // Serialize writers: building the new runtime is a load-build-store
        // read-modify-write of runtime_, so a test-driven reload and
        // the config-file watcher thread could otherwise both build from the
        // same old runtime and lose one of the updates. Readers do not take
        // reload_mutex_; an unchanged generation-cache hit also avoids the
        // AtomicSharedPtr shared source.
        std::lock_guard<std::mutex> reload_lock(reload_mutex_);
        apply_config(runtime_.load_cached_ref(), std::move(cfg));
    }

    void AgentImpl::init_grpc_workers() try {
        grpc_agent_->setAgentService(this);
        grpc_metadata_->setAgentService(this);
        grpc_span_->setAgentService(this);
        grpc_stat_->setAgentService(this);
        if (grpc_command_) {
            grpc_command_->setAgentService(this);
        }

        // Open the channels here (not at construction): this is where grpc_init
        // and gRPC's background threads come up, kept out of the cold
        // constructor so no gRPC state exists until this process starts the
        // agent.
        grpc_agent_->openChannel();
        grpc_metadata_->openChannel();
        grpc_span_->openChannel();
        grpc_stat_->openChannel();
        if (grpc_command_) {
            grpc_command_->openChannel();
        }

        // Boot-phase registration: block here until the collector accepts the
        // first AgentInfo, retrying indefinitely. The other workers are only
        // spawned after that first success. A shutdown during the wait aborts
        // the bring-up: request_stop_workers() signals stopAgentInfo's cv
        // before this thread is joined, which wakes the retry sleep.
        if (!grpc_agent_->registerAgentWithRetry()) {
            return;
        }

        // Registered: start the periodic AgentInfo re-sender and the rest of
        // the workers. Post-boot AgentInfo send failures are tolerated and
        // never touch enabled_.
        grpc_agent_->startAgentInfo();

        ping_thread_ = std::thread{&GrpcAgent::sendPingWorker, grpc_agent_.get()};
        meta_thread_ = std::thread{&GrpcMetadata::sendMetaWorker, grpc_metadata_.get()};
        span_thread_ = std::thread{&GrpcSpan::sendSpanWorker, grpc_span_.get()};
        stat_thread_ = std::thread{&GrpcStats::sendStatsWorker, grpc_stat_.get()};
        if (grpc_command_) {
            command_thread_ = std::thread{&GrpcCommand::commandWorker, grpc_command_.get()};
        }
        url_stat_add_thread_ = std::thread{&UrlStats::addUrlStatsWorker, url_stats_.get()};
        url_stat_send_thread_ = std::thread{&UrlStats::sendUrlStatsWorker, url_stats_.get()};
        agent_stat_thread_ = std::thread{&AgentStats::agentStatsWorker, agent_stats_.get()};

        // All workers are up: enable span recording. A shutdown racing this
        // init must not re-enable an agent being torn down.
        if (!shutting_down_) {
            enabled_ = true;
            // Close the check-then-store window: do_shutdown() may have run
            // entirely between the check above and the store (setting
            // shutting_down_ then enabled_ = false), which would leave a
            // torn-down agent permanently enabled. In the seq_cst total
            // order either this re-check observes shutting_down_ and rolls
            // back, or the store above preceded do_shutdown()'s
            // enabled_ = false — both end with the agent disabled.
            if (shutting_down_) {
                enabled_ = false;
            }
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to init grpc workers: exception = {}", e.what());
        enabled_ = false;
        return;
    } catch (...) {
        LOG_ERROR("failed to init grpc workers: unknown exception");
        enabled_ = false;
        return;
    }

    void AgentImpl::request_stop_workers() noexcept {
        // Every call here only sets flags, notifies condition variables or
        // issues a best-effort TryCancel — none of them blocks. Signaling
        // everything before any join lets all workers wind down in parallel
        // inside the shutdown deadline instead of serially behind each other.
        // Per-call try/catch so one failing signal cannot skip the rest.
        try { if (config_watcher_) config_watcher_->requestStop(); } catch (...) {}
        try { grpc_agent_->requestStopAgentInfo(); } catch (...) {}
        try { url_stats_->stopAddUrlStatsWorker(); } catch (...) {}
        try { url_stats_->stopSendUrlStatsWorker(); } catch (...) {}
        try { agent_stats_->stopAgentStatsWorker(); } catch (...) {}
        try { grpc_agent_->stopPingWorker(); } catch (...) {}
        try { grpc_metadata_->stopMetaWorker(); } catch (...) {}
        try { grpc_span_->stopSpanWorker(); } catch (...) {}
        try { grpc_stat_->stopStatsWorker(); } catch (...) {}
        try { if (grpc_command_) grpc_command_->stopCommandWorker(); } catch (...) {}
    }

    void AgentImpl::teardown_workers() noexcept {
        // The watcher's reload callback dereferences this agent, so it is
        // joined before anything else; the caller (inline teardown or the
        // deadline runner's keep-alive) guarantees the agent outlives this
        // whole function.
        try { if (config_watcher_) config_watcher_->stop(); } catch (...) {}
        try {
            grpc_agent_->stopAgentInfo();
            wait_grpc_workers();

            grpc_agent_->closeChannel();
            grpc_metadata_->closeChannel();
            grpc_stat_->closeChannel();
            grpc_span_->closeChannel();
            if (grpc_command_) {
                grpc_command_->closeChannel();
            }

            LOG_INFO("close grpc workers done");
        } catch (const std::exception& e) {
            try { LOG_ERROR("agent teardown failed: exception = {}", e.what()); } catch (...) {}
        } catch (...) {
            try { LOG_ERROR("agent teardown failed: unknown exception"); } catch (...) {}
        }
    }

    void AgentImpl::abandon_grpc_workers() noexcept {
        // Abandon (never join or detach) every worker handle, including
        // handles inherited across fork(), on which even pthread_detach is
        // unsafe — see abandon_thread().
        abandon_thread(init_thread_);
        abandon_thread(ping_thread_);
        abandon_thread(meta_thread_);
        abandon_thread(span_thread_);
        abandon_thread(stat_thread_);
        abandon_thread(command_thread_);
        abandon_thread(url_stat_add_thread_);
        abandon_thread(url_stat_send_thread_);
        abandon_thread(agent_stat_thread_);
    }

    void AgentImpl::wait_grpc_workers() {
        // init_grpc_workers assigns the other thread members; join it first so
        // the joins below cannot race those assignments. The init thread may
        // be blocked in the boot-phase registration, but request_stop_workers()
        // has already signaled stopAgentInfo's cv, which interrupts that retry
        // loop, and an in-flight registerAgent() call has a request deadline.
        if (init_thread_.joinable()) {
            init_thread_.join();
        }

        std::thread* workers[] = {
            &url_stat_add_thread_, &url_stat_send_thread_,
            &agent_stat_thread_, &ping_thread_, &meta_thread_,
            &span_thread_, &stat_thread_, &command_thread_,
        };
        for (auto* worker : workers) {
            if (worker->joinable()) {
                worker->join();
            }
        }
    }

    bool AgentImpl::leak_agent_for_stragglers(bool may_defer_destroy) noexcept {
        // Resource exhaustion left no helper thread to bound (or even watch)
        // the joins, and joining inline would reintroduce the unbounded
        // shutdown. Leak the whole object instead: the stop signals are
        // already sent, so the workers wind down on their own, and everything
        // they dereference (this agent and its members) simply stays alive
        // forever. Arranging that requires the power to prevent destruction —
        // available while shared-owned (SharedDeleter consults
        // leak_on_release_ at final release) or when the SharedDeleter itself
        // is the caller (may_defer_destroy). An object that is neither dies
        // with its scope no matter what, so the leak cannot be arranged for
        // it and the caller must join inline.
        if (!may_defer_destroy && weak_from_this().expired()) {
            return false;
        }
        leak_on_release_.store(true);
        // The thread handles are deliberately left joinable: a leaked object
        // never runs its destructors, so nothing ever joins or terminates on
        // them, and touching the handles here could race the init thread
        // still assigning the worker members.
        try {
            LOG_WARN("agent shutdown: teardown thread unavailable; leaking the agent "
                     "and letting its workers wind down unjoined");
        } catch (...) {}
        return true;
    }

    bool AgentImpl::teardown_workers_with_deadline(bool may_defer_destroy) noexcept {
        // The workers run member functions of this agent and its gRPC clients
        // and dereference `this` (isExiting/getConfig/getAgentStats); they
        // must be joined, never detached, before the members they use are
        // destroyed — abandoning a straggler around the object's destruction
        // would be a use-after-free. Unary calls have deadlines and the stop
        // signals request stream cancellation, but gRPC TryCancel() is
        // best-effort and callback completion has no hard wall-clock bound,
        // so the joins themselves are unbounded. To still bound shutdown, the
        // whole blocking teardown runs on a helper thread: if it beats the
        // deadline the helper is joined and shutdown completes as before;
        // otherwise the helper is detached and the object's lifetime is
        // secured for it — via a keep-alive self-reference (Shutdown() path)
        // or by deferring destruction to the helper (SharedDeleter path).
        struct TeardownState {
            std::mutex m;
            std::condition_variable cv;
            bool finished{false};
            bool abandoned{false};
            // At most one of the two is set when the deadline expires.
            // keep_alive keeps this agent (and every member the
            // still-draining workers use) alive until the detached runner
            // finishes; it is released when the runner drops its state
            // reference, which may run the SharedDeleter on the runner
            // thread — by then every worker is joined, so destruction is
            // benign. delete_when_done instead hands the runner ownership of
            // an object whose final reference is already gone (SharedDeleter
            // caller): the runner deletes it after the joins — a leak only
            // if the stragglers never finish.
            std::shared_ptr<AgentImpl> keep_alive;
            bool delete_when_done{false};
        };

        std::shared_ptr<TeardownState> state;
        std::thread runner;
        try {
            state = std::make_shared<TeardownState>();
            runner = std::thread([this, state] {
                teardown_workers();
                bool abandoned;
                bool delete_when_done;
                {
                    std::lock_guard<std::mutex> l(state->m);
                    state->finished = true;
                    abandoned = state->abandoned;
                    delete_when_done = state->delete_when_done;
                }
                state->cv.notify_all();
                if (abandoned) {
                    // do_shutdown() already returned and ran its logger
                    // flush; flush again for everything the stragglers
                    // logged since. Logger is a never-destroyed singleton
                    // with an idempotent, mutex-guarded shutdown.
                    try { LOG_INFO("agent shutdown: background teardown finished"); } catch (...) {}
                    try { shutdown_logger(); } catch (...) {}
                }
                if (delete_when_done) {
                    // Deferred destroy: the SharedDeleter skipped destruction
                    // when the deadline expired and handed the object here.
                    // Every worker is joined now, so the destructor is
                    // benign (its own do_shutdown call no-ops).
                    delete this;
                }
            });
        } catch (...) {
            // Allocation or thread creation (EAGAIN) failed: no runner can
            // bound the joins. Leak the agent instead of joining unbounded;
            // only an object that is neither shared-owned nor deleter-owned
            // (it dies with its scope regardless) still requires the inline
            // join for lifetime safety.
            if (leak_agent_for_stragglers(may_defer_destroy)) {
                return may_defer_destroy;
            }
            try { LOG_WARN("agent shutdown: teardown thread unavailable, tearing down inline"); } catch (...) {}
            teardown_workers();
            return false;
        }

        // From here every path must leave `runner` joined or detached —
        // destroying a joinable std::thread would std::terminate the host.
        bool finished = false;
        try {
            std::unique_lock<std::mutex> l(state->m);
            finished = state->cv.wait_for(l, agent_shutdown_deadline(),
                                          [&state] { return state->finished; });
        } catch (...) {
            finished = false;
        }
        if (finished) {
            runner.join();
            return false;
        }

        // Deadline expired. Detaching requires securing the object for the
        // runner: a keep-alive self-reference while shared-owned (Shutdown()
        // path), or handing the runner ownership when the SharedDeleter is
        // the caller. An object that is neither (a stack-constructed
        // instance — none exist in this codebase; every shared agent comes
        // from createShared()) dies with its scope no matter what, so the
        // unbounded join remains its only lifetime-safe option.
        std::shared_ptr<AgentImpl> keep_alive = weak_from_this().lock();
        if (keep_alive == nullptr && !may_defer_destroy) {
            try {
                LOG_WARN("agent shutdown exceeded the {}ms deadline; not shared-owned, "
                         "waiting for workers to finish",
                         agent_shutdown_deadline().count());
            } catch (...) {}
            runner.join();
            return false;
        }

        bool abandoned = false;
        const bool defer_destroy = keep_alive == nullptr;
        try {
            std::lock_guard<std::mutex> l(state->m);
            // The runner may have finished between the timed wait and this
            // lock; hand the object over only while it is still running.
            if (!state->finished) {
                if (defer_destroy) {
                    state->delete_when_done = true;
                } else {
                    state->keep_alive = std::move(keep_alive);
                }
                state->abandoned = true;
                abandoned = true;
            }
        } catch (...) {}
        if (!abandoned) {
            runner.join();
            return false;
        }
        runner.detach();
        try {
            LOG_WARN("agent shutdown exceeded the {}ms deadline; workers keep draining "
                     "in the background and the agent is {} when they finish",
                     agent_shutdown_deadline().count(),
                     defer_destroy ? "destroyed" : "released");
        } catch (...) {}
        return defer_destroy;
    }

    AgentImpl::~AgentImpl() noexcept {
        // No deferral from a running destructor — the object dies when it
        // returns. For createShared() agents this call is a no-op: the
        // SharedDeleter already ran do_shutdown(true) before delete.
        do_shutdown(false);

        // Belt-and-braces: if do_shutdown() bailed out early for any reason,
        // a joinable std::thread member would call std::terminate() when its
        // destructor runs. Abandon any stragglers so member destruction stays
        // benign. Abandon (not join) is used because by this point the
        // process is likely on its way down and we don't want to block.
        abandon_grpc_workers();

        // Destroying in a forked child gRPC clients created in the parent is
        // unsafe: they own internal threads (e.g. GrpcAgent's AgentInfo
        // scheduler, the command dispatcher) that do not exist in this
        // process, and their destructors would join the dead handles and
        // abort. Intentionally leak the client objects instead — the child
        // either builds its own agent via Start() or is short-lived. Done
        // here rather than in do_shutdown() so the pointers stay valid while
        // the object is alive (see the forked-child branch there).
        if (owner_pid_ != 0 && owner_pid_ != getpid()) {
            (void)grpc_agent_.release();
            (void)grpc_metadata_.release();
            (void)grpc_span_.release();
            (void)grpc_stat_.release();
            (void)grpc_command_.release();
        }
    }

    bool AgentImpl::tracing_active() const noexcept {
        // Fast reject first: a disabled agent pays nothing extra here.
        if (!enabled_) {
            return false;
        }
        // One getpid() per span creation, only on the enabled path. An agent
        // inherited across fork() carries the parent's enabled_ == true, but
        // its worker threads do not exist in this process: recording would
        // enqueue real spans into queues nothing drains — silently, since
        // even the queue-drop reporter runs on the missing worker thread.
        // Reading owner_pid_ is race-free: it is written before the init
        // thread that publishes enabled_ = true is spawned, so any thread
        // observing enabled_ == true also observes the final owner_pid_.
        if (owner_pid_ != 0 && owner_pid_ != getpid()) {
            warn_fork_inheritance();
            return false;
        }
        return true;
    }

	SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point) {
        SpanPtr span;

        // Cheap pre-filter only: the funnel end (the method+reader overload
        // below) performs the authoritative tracing_active() check, so the
        // pid guard runs once per span, not once per overload hop.
        if (enabled_) {
            NoopTraceContextReader reader;
            span = NewSpan(operation, rpc_point, reader);
        } else {
            span = noopSpan();
        }
        return span;
    }

    SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point,
                               TraceContextReader& reader) {
        return NewSpan(operation, rpc_point, "", reader);
    }

	SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point,
	                           std::string_view method, TraceContextReader& reader) try {
        // Every NewSpan overload funnels through here, so this is the single
        // authoritative admission check: enabled AND owned by this process.
        // An agent inherited across fork() therefore hands out noop spans
        // (with a one-time error log) instead of recording into dead queues.
        if (!tracing_active()) {
            return noopSpan();
        }
        // One owning snapshot covers the filters and the sampler for this
        // request and then moves into the span, so the function still costs a
        // single control-block RMW pair. Owning — not load_cached_ref() — is
        // required here: reader.Get() below runs host code, and a re-entrant
        // load of runtime_ racing a config reload would refresh the TLS entry
        // and could destroy the snapshot the references below point into.
        auto runtime = runtime_.load();
        const auto& url_filter = runtime->http_url_filter;
        if (url_filter && url_filter->isFiltered(rpc_point)) {
            return noopSpan();
        }
        const auto& method_filter = runtime->http_method_filter;
        if (!method.empty() && method_filter && method_filter->isFiltered(method)) {
            return noopSpan();
        }

        if (const auto parent_sampling = reader.Get(HEADER_SAMPLED); parent_sampling == "s0") {
            agent_stats_->incrUnsampleCont();
            return std::make_shared<UnsampledSpan>(this, std::move(runtime));
        }

        const auto& sampler = runtime->sampler;
        if (!sampler) {
            return noopSpan();
        }

        const auto tid = reader.Get(HEADER_TRACE_ID);
        const bool my_sampling = tid.has_value() ? sampler->isContinueSampled()
                                                 : sampler->isNewSampled();

        if (my_sampling) {
            // Resolve the trace id up front: parse the inbound header (continued
            // trace) or mint a fresh one. Parsing can fail (malformed header /
            // bad_alloc) and return an empty trace id — in that case drop to a
            // noop span rather than record a trace with no agent id.
            TraceId trace_id = tid.has_value() ? TraceId::parseTraceId(tid.value())
                                               : generateTraceId();
            if (trace_id.empty()) {
                return noopSpan();
            }
            // Pass this runtime snapshot so the span skips further atomic
            // loads (config limits, status-error checks, URL-stat gating) and
            // lives on the same config generation its admission was decided
            // under, and hand the resolved trace id to the impl-level extract.
            auto span = std::make_shared<SpanImpl>(this, operation, rpc_point, std::move(runtime));
            span->extractContext(reader, std::move(trace_id));
            return span;
        }
        return std::make_shared<UnsampledSpan>(this, std::move(runtime));
    } catch (const std::exception& e) {
        LOG_ERROR("new span exception = {}", e.what());
        return noopSpan();
    } catch (...) {
        LOG_ERROR("new span unknown exception");
        return noopSpan();
    }

	bool AgentImpl::Enable() {
    	// Same fork-inheritance guard as the span path (tracing_active()),
    	// but checked before enabled_: an inherited agent must report
    	// disabled even while a torn-down enabled_ flag is still flipping.
    	if (owner_pid_ != 0 && owner_pid_ != getpid()) {
    		warn_fork_inheritance();
    		return false;
    	}
    	return enabled_;
	}

    void AgentImpl::Shutdown() noexcept {
        // Keep *this alive across do_shutdown(): if the global handle holds the
        // last reference, resetting it would destroy the agent mid-call when
        // Shutdown() was reached through a raw pointer or reference.
        std::shared_ptr<AgentImpl> self;
        try {
            std::lock_guard<std::mutex> lock(global_agent_mutex);
            self = global_agent().load();
            if (self.get() == this) {
                global_agent().store(nullptr);
            } else {
                self.reset();
            }
        } catch (...) {}
        // No deferred destroy from here: `self` (or the caller's own
        // reference) keeps the object shared-owned, so a teardown that
        // outlives the deadline is parked on a keep-alive instead.
        do_shutdown(false);
    }

    void AgentImpl::SharedDeleter::operator()(AgentImpl* agent) const noexcept {
        if (agent == nullptr) {
            return;
        }
        // Final release. When the host never called Shutdown(), the terminal
        // teardown runs here, bounded by the shutdown deadline; on timeout
        // the teardown runner assumes ownership (deferred destroy) and this
        // deleter must not touch the object again.
        if (agent->do_shutdown(true)) {
            return;
        }
        // A resource-exhausted shutdown (now or earlier) abandoned the
        // workers without joining them; they dereference the object
        // indefinitely, so it must be leaked, never destroyed.
        if (agent->leak_on_release_.load()) {
            return;
        }
        delete agent;
    }

    bool AgentImpl::do_shutdown(bool may_defer_destroy) noexcept {
        if (shutting_down_.exchange(true)) {
            return false;
        }

        // Wait for an in-flight Start() to finish publishing owner_pid_ and
        // init_thread_ before reading them below; Start() calls arriving
        // after this point observe shutting_down_ under the same lock and
        // refuse. try/catch keeps the noexcept contract if locking ever
        // fails — proceeding unserialized then matches the old behavior.
        std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::defer_lock);
        try { lifecycle_lock.lock(); } catch (...) {}

        enabled_ = false;

        // PID-guarded teardown. When an agent that was started in a parent is
        // torn down in a forked child (owner_pid_ != getpid()), its worker and
        // watcher threads and its gRPC runtime do not exist in this process.
        // Joining those dead handles would abort; touching the inherited gRPC
        // stack is unsafe. Abandon the handles and skip the normal teardown.
        if (owner_pid_ != 0 && owner_pid_ != getpid()) {
            try { LOG_INFO("agent shutdown in forked child: abandoning inherited workers"); } catch (...) {}
            abandon_grpc_workers();
            // The watcher thread does not exist in this process either; its
            // stop() abandons the inherited dead handle via its own pid guard.
            try { if (config_watcher_) config_watcher_->stop(); } catch (...) {}
            // The gRPC clients inherited from the parent are intentionally
            // leaked, but not here: releasing the unique_ptrs would null them
            // while a racing thread that loaded enabled_ == true just before
            // the store above may still be about to dereference grpc_span_ /
            // grpc_metadata_ (recordSpan, cacheApi, ...) — a null operator->
            // crash in the host. The release is deferred to ~AgentImpl, so
            // the pointers stay valid for the whole lifetime of this object.
            return false;
        }

        // Serialization is achieved: acquiring the lock above waited out any
        // in-flight Start() and published its writes, and later Start() calls
        // refuse on shutting_down_. Release it before the teardown — on the
        // deferred-destroy path the teardown runner may delete this object as
        // soon as ownership is handed over, and this frame must not be
        // holding a member mutex when that happens (unlocking a destroyed
        // mutex is undefined behavior).
        if (lifecycle_lock.owns_lock()) {
            lifecycle_lock.unlock();
        }

        try { LOG_INFO("agent shutdown"); } catch (...) {}
        request_stop_workers();

        // A never-launched agent (Start() never ran, or its synchronous
        // failure already reset started_ and stopped the watcher) has no init
        // thread and no workers, so the blocking teardown cannot actually
        // block. Run it inline: this skips the deadline runner thread — and
        // the leak fallback its creation failure would force — which matters
        // on the StartAgent() failure path, where the failure may itself have
        // been thread-creation exhaustion. Reading started_ here is stable:
        // the lifecycle lock above waited out any in-flight Start(), and
        // later Start() calls refuse on shutting_down_ before touching it.
        if (!started_) {
            teardown_workers();
            try { shutdown_logger(); } catch (...) {}
            return false;
        }

        const bool destroy_deferred = teardown_workers_with_deadline(may_defer_destroy);
        // Nothing below may touch members: on the deferred-destroy path the
        // runner owns the object from here and may already have deleted it.
        try { shutdown_logger(); } catch (...) {}
        return destroy_deferred;
    }

    TraceId TraceId::parseTraceId(std::string_view txid) noexcept try {
        constexpr size_t kMaxAgentIdLength = 24;
        constexpr size_t kMaxInt64StringLength = 20; // max digits of int64_t

        const std::string_view sv = txid;

        // Validate the structure before building anything: any malformation
        // yields an empty TraceId so the caller drops to a noop span.
        // AgentId (first field before '^')
        const auto pos1 = sv.find('^');
        if (pos1 == std::string_view::npos) {
            LOG_WARN("parsing Txid: invalid txid format = {}", sv);
            return {};
        }
        if (pos1 == 0) {
            LOG_WARN("parsing Txid: empty AgentId = {}", sv);
            return {};
        }
        if (pos1 > kMaxAgentIdLength) {
            LOG_WARN("parsing Txid: AgentId too long (length={}, max={})", pos1, kMaxAgentIdLength);
            return {};
        }
        // StartTime (second field)
        const auto pos2 = sv.find('^', pos1 + 1);
        if (pos2 == std::string_view::npos) {
            LOG_WARN("parsing Txid: invalid txid format = {}", sv);
            return {};
        }
        const auto start_time_len = pos2 - pos1 - 1;
        if (start_time_len > kMaxInt64StringLength) {
            LOG_WARN("parsing Txid: StartTime too long (length={}, max={})", start_time_len, kMaxInt64StringLength);
            return {};
        }
        // Sequence (third and final field). The wire form is exactly
        // agentId^startTime^sequence, so any further '^' means extra fields or a
        // trailing separator. Reject it structurally: otherwise the surplus is
        // absorbed into the Sequence field, fails to parse, and silently
        // degrades to sequence 0 via value_or(0) below — recording a bogus live
        // trace on which every distinct malformed header collides at
        // (agentId, startTime, 0).
        const auto sequence_str = sv.substr(pos2 + 1);
        if (sequence_str.find('^') != std::string_view::npos) {
            LOG_WARN("parsing Txid: invalid txid format = {}", sv);
            return {};
        }
        if (sequence_str.length() > kMaxInt64StringLength) {
            LOG_WARN("parsing Txid: Sequence too long (length={}, max={})", sequence_str.length(), kMaxInt64StringLength);
            return {};
        }

        // Non-numeric fields are rejected structurally like the malformations
        // above: absorbing them as 0 via value_or would record a live trace on
        // which every distinct malformed header collides at (agentId, 0, 0).
        const auto start_time = stoll_(sv.substr(pos1 + 1, start_time_len));
        const auto sequence = stoll_(sequence_str);
        if (!start_time || !sequence) {
            LOG_WARN("parsing Txid: invalid txid format = {}", sv);
            return {};
        }

        TraceId tid;
        tid.AgentId = std::make_shared<const std::string>(sv.substr(0, pos1));
        tid.StartTime = *start_time;
        tid.Sequence = *sequence;
        return tid;
    } catch (...) {
        // This function allocates (AgentId copy, log formatting); without this
        // handler an OOM would hit the noexcept boundary and terminate the host.
        // Degrade to an empty trace id (caller drops to a noop span) instead. No
        // logging here: formatting allocates too, and a throw escaping a catch
        // block would still terminate.
        return {};
    }

    TraceId AgentImpl::generateTraceId() {
        // Allocation-free: agent_id_ is already a shared string that is
        // immutable once spans can be created (see its declaration), so every
        // TraceId minted here — and every downstream copy (async child span,
        // queued ExceptionMeta) — only bumps its refcount.
        return TraceId{agent_id_, start_time_,
                       static_cast<int64_t>(trace_id_sequence_.fetch_add(1))};
    }

    void AgentImpl::recordSpan(std::unique_ptr<SpanChunk> span) const {
        if (enabled_) {
            grpc_span_->enqueueSpan(std::move(span));
        }
    }

    // Default for implementations without the snapshot-taking override
    // (mocks/test doubles): fall back to the config-loading overload. Defined
    // out-of-line because an inline body in agent_service.h would need
    // UrlStatEntry complete there.
    void AgentService::recordUrlStat(UrlStatEntry stat, const Config& /*config*/) const {
        recordUrlStat(std::move(stat));
    }

    void AgentImpl::recordUrlStat(UrlStatEntry stat) const {
        if (enabled_) {
            url_stats_->enqueueUrlStats(std::move(stat));
        }
    }

    void AgentImpl::recordUrlStat(UrlStatEntry stat, const Config& config) const {
        if (enabled_) {
            url_stats_->enqueueUrlStats(std::move(stat), config);
        }
    }

    void AgentImpl::recordStats(const StatsType stats) const {
        if (enabled_) {
            grpc_stat_->enqueueStats(stats);
        }
    }

    int32_t AgentImpl::cacheApi(std::string_view api_str, int32_t api_type) const try {
        if (!enabled_) {
            return 0;
        }

        const auto [id, found] = api_cache_->get(ApiCacheKey{api_str, api_type});
        if (found) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(META_API, id, api_type, api_str);
        grpc_metadata_->enqueueMeta(std::move(meta));

        return id;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache api meta: exception = {}", e.what());
        return 0;
    } catch (...) {
        LOG_ERROR("failed to cache api meta: unknown exception");
        return 0;
    }

    // The removeCache* functions carry the same exception boundary as their
    // cache* siblings: they build allocating keys, and their caller is the
    // meta worker loop — an escaping exception would trip its supervisor
    // restart for what is only a best-effort cache eviction.
    void AgentImpl::removeCacheApi(const ApiMeta& api_meta) const try {
        if (enabled_) {
            api_cache_->remove(ApiCacheKey{api_meta.api_str_, api_meta.type_});
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to remove cached api meta: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to remove cached api meta: unknown exception");
    }

    int32_t AgentImpl::cacheError(std::string_view error_name) const try {
        if (!enabled_) {
            return 0;
        }

        const auto [id, found] = error_cache_->get(error_name);
        if (found) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(META_STRING, id, error_name, STRING_META_ERROR);
        grpc_metadata_->enqueueMeta(std::move(meta));

        return id;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache error meta: exception = {}", e.what());
        return 0;
    } catch (...) {
        LOG_ERROR("failed to cache error meta: unknown exception");
        return 0;
    }

    void AgentImpl::removeCacheError(const StringMeta& error_meta) const try {
        if (enabled_) {
            error_cache_->remove(error_meta.str_val_);
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to remove cached error meta: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to remove cached error meta: unknown exception");
    }

    int32_t AgentImpl::cacheSql(std::string_view sql_query) const try {
        if (!enabled_) {
            return 0;
        }

        const auto [id, found] = sql_cache_->get(sql_query);
        if (found) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(META_STRING, id, sql_query, STRING_META_SQL);
        grpc_metadata_->enqueueMeta(std::move(meta));

        return id;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache sql meta: exception = {}", e.what());
        return 0;
    } catch (...) {
        LOG_ERROR("failed to cache sql meta: unknown exception");
        return 0;
    }

    std::optional<PreparedSqlRef> AgentImpl::prepareSql(
            std::string_view raw_sql, SqlMetaMode mode) const try {
        if (!enabled_) {
            return std::nullopt;
        }

        // SqlNormalizer has immutable configuration and normalize() keeps all
        // state local, so one process-wide instance is safe for concurrent use.
        // Leaked like the other exit-surviving singletons (Logger, the global
        // agent holder): user threads can still call prepareSql during static
        // destruction when the process exits without Shutdown(), and a
        // function-local static would already be destroyed at that point.
        static const SqlNormalizer& normalizer = *new SqlNormalizer(64 * 1024);
        // One relaxed load instead of a runtime snapshot lookup plus owning
        // Config copy: this runs once per SQL statement and only needs this flag.
        const bool enable_raw_sql_cache =
            raw_sql_cache_enabled_.load(std::memory_order_relaxed);

        if (mode == SqlMetaMode::Id) {
            auto prepare = [&]() -> PreparedSqlRef {
                auto normalized = normalizer.normalize(raw_sql);
                const auto id = cacheSql(normalized.normalized_sql);
                if (id <= 0) {
                    // Throwing prevents LruCacheImpl from inserting an invalid
                    // value; cacheSql() already logged the underlying failure.
                    throw std::runtime_error("SQL ID unavailable");
                }
                return std::make_shared<const PreparedSql>(PreparedSql{
                    std::move(normalized.normalized_sql),
                    std::move(normalized.parameters),
                    SqlIdentity{id}});
            };
            if (enable_raw_sql_cache) {
                const auto epoch = sql_id_metadata_epoch_.load(std::memory_order_acquire);
                return raw_sql_id_cache_->get(raw_sql, epoch, prepare).value;
            }
            return prepare();
        }

        if (mode == SqlMetaMode::Uid) {
            auto prepare = [&]() -> PreparedSqlRef {
                auto normalized = normalizer.normalize(raw_sql);
                auto uid = cacheSqlUid(normalized.normalized_sql);
                if (!uid) {
                    throw std::runtime_error("SQL UID unavailable");
                }
                return std::make_shared<const PreparedSql>(PreparedSql{
                    std::move(normalized.normalized_sql),
                    std::move(normalized.parameters),
                    SqlIdentity{*uid}});
            };
            if (enable_raw_sql_cache) {
                const auto epoch = sql_uid_metadata_epoch_.load(std::memory_order_acquire);
                return raw_sql_uid_cache_->get(raw_sql, epoch, prepare).value;
            }
            return prepare();
        }

        return std::nullopt;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to prepare raw sql: exception = {}", e.what());
        return std::nullopt;
    } catch (...) {
        LOG_ERROR("failed to prepare raw sql: unknown exception");
        return std::nullopt;
    }

    void AgentImpl::removeCacheSql(const StringMeta& sql_meta) const try {
        if (enabled_) {
            sql_cache_->remove(sql_meta.str_val_);
            // Raw entries cache the assigned ID. Advancing the epoch makes all
            // of them unreachable after metadata retry exhaustion, without a
            // reverse index from normalized SQL to every raw variant.
            sql_id_metadata_epoch_.fetch_add(1, std::memory_order_release);
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to remove cached sql meta: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to remove cached sql meta: unknown exception");
    }

    std::optional<SqlUid> AgentImpl::cacheSqlUid(std::string_view sql) const try {
        if (!enabled_) {
            return std::nullopt;
        }

        const auto [uid, found] = sql_uid_cache_->get(sql);
        if (found) {
            return uid;
        }

        // Cold path (first time this SQL is seen): enqueue the UID for the collector.
        auto meta = std::make_unique<MetaData>(META_SQL_UID, uid, sql);
        grpc_metadata_->enqueueMeta(std::move(meta));

        return uid;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache sql uid meta: exception = {}", e.what());
        return std::nullopt;
    } catch (...) {
        LOG_ERROR("failed to cache sql uid meta: unknown exception");
        return std::nullopt;
    }

    void AgentImpl::removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const try {
        if (enabled_) {
            sql_uid_cache_->remove(sql_uid_meta.sql_);
            sql_uid_metadata_epoch_.fetch_add(1, std::memory_order_release);
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to remove cached sql uid meta: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to remove cached sql uid meta: unknown exception");
    }

    void AgentImpl::recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                                    std::vector<std::unique_ptr<Exception>>&& exceptions) const try {
        // Cheap flag first, config load second (same ordering as the getters
        // below): a disabled agent must not look up or retain a runtime snapshot.
        if (!enabled_ || !getConfig()->enable_callstack_trace) {
            return;
        }

        auto meta = std::make_unique<MetaData>(META_EXCEPTION, trace_id, span_id, url_template,
                                               std::move(exceptions));
        grpc_metadata_->enqueueMeta(std::move(meta));
    } catch (const std::exception& e) {
        LOG_ERROR("failed to record exception meta: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to record exception meta: unknown exception");
    }

    // These check enabled_ before touching the runtime generation so the
    // disabled-agent path remains free.
    bool AgentImpl::isStatusFail(const int status) const {
        if (!enabled_) {
            return false;
        }
        const auto& runtime = runtime_.load_cached_ref();
        if (runtime->http_status_errors) {
            return runtime->http_status_errors->isErrorCode(status);
        }
        return false;
    }

    void AgentImpl::recordServerHeader(const HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const {
        if (!enabled_ || which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto& runtime = runtime_.load_cached_ref();
        // Owning copy of the recorder, not a reference into the snapshot:
        // recordHeader() runs host code (reader.Get()), and a re-entrant load
        // of runtime_ racing a config reload would refresh the TLS entry the
        // snapshot lives in, destroying a merely-referenced recorder mid-call.
        const auto recorder = runtime->http_srv_header_recorder[which];
        if (recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    void AgentImpl::recordClientHeader(const HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const {
        if (!enabled_ || which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto& runtime = runtime_.load_cached_ref();
        // Owning copy for the same reason as recordServerHeader() above.
        const auto recorder = runtime->http_cli_header_recorder[which];
        if (recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    static std::shared_ptr<AgentImpl> make_agent(std::shared_ptr<const Config> cfg,
                                                 const AgentOptions& options) {
        if (!cfg->enable) {
            return nullptr;
        }
        try {
            auto grpc_agent = std::make_unique<GrpcAgent>(cfg);
            grpc_agent->setServerMetaData(options.server_info, options.args, options.libs);
            auto grpc_metadata = std::make_unique<GrpcMetadata>(cfg);
            auto grpc_span = std::make_unique<GrpcSpan>(cfg);
            auto grpc_stat = std::make_unique<GrpcStats>(cfg);
            auto grpc_command = std::make_unique<GrpcCommand>(cfg);
            // createShared (not make_shared): its SharedDeleter keeps the
            // final release bounded when the host drops the last reference
            // without calling Shutdown().
            return AgentImpl::createShared(cfg,
                std::move(grpc_agent), std::move(grpc_metadata), std::move(grpc_span),
                std::move(grpc_stat), std::move(grpc_command), options.app_type);
        } catch (const std::exception& e) {
            LOG_ERROR("make agent exception = {}", e.what());
            return nullptr;
        } catch (...) {
            LOG_ERROR("make agent unknown exception");
            return nullptr;
        }
    }

    // Public entry point: a failure to configure or construct the agent must
    // surface as a noop agent, never as an exception in the host application.
    AgentPtr StartAgent(const AgentOptions& options) try {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        auto agent = global_agent().load();

        if (agent != nullptr) {
            // A global agent inherited across fork() is unusable here (its
            // threads and gRPC runtime live in the parent), and a fresh agent
            // cannot be built either: the parent's Start() ran grpc_init, and
            // gRPC must not be re-initialized in a process that forked after
            // grpc_init. Refuse loudly — the master process must not call
            // StartAgent() before forking.
            if (!agent->ownedByThisProcess()) {
                LOG_ERROR("StartAgent() refused: the global agent was started in another process "
                          "(a master must not start an agent before fork()); tracing stays "
                          "disabled in this process");
                // Evict the inherited handle so GlobalAgent() degrades to the
                // noop agent from now on, matching the message above. Dropping
                // what may be the last reference is safe: destruction in a
                // forked child goes through the pid-guarded teardown, which
                // abandons the inherited thread handles and leaks the parent's
                // gRPC clients instead of touching them.
                global_agent().store(nullptr);
                agent.reset();
                return noopAgent();
            }

            // Already running in this process: StartAgent() is one-shot per
            // process. Config changes flow through the config-file watcher,
            // not through repeated StartAgent() calls.
            if (!agent->isExiting()) {
                LOG_WARN("StartAgent() called again in this process; returning the running agent");
                return agent;
            }

            // A shut-down agent is not restartable: drop it from the
            // singleton and fall through to build a fresh agent. (Clearing
            // the singleton here also keeps GlobalAgent() degrading to the
            // noop agent if the rebuild below fails.)
            LOG_WARN("global agent is shut down; replacing it with a new agent");
            global_agent().store(nullptr);
            agent.reset();
        }

        auto cfg = make_config(options);
        if (!cfg || !cfg->check()) {
            return noopAgent();
        }
        agent = make_agent(std::move(cfg), options);
        if (agent == nullptr) {
            return noopAgent();
        }
        agent->setOptions(options);
        // Publish only a successfully launched agent. A synchronous Start()
        // failure (watcher allocation, init-thread creation) must not install
        // a permanently cold instance: every later StartAgent() would treat
        // it as the running agent and never call Start() again. Returning the
        // noop agent instead leaves the singleton empty, so the next
        // StartAgent() call rebuilds and retries from scratch. The failed
        // agent is destroyed here through its never-started cold teardown.
        if (!agent->Start()) {
            return noopAgent();
        }
        global_agent().store(agent);
        return agent;
    } catch (...) {
        return noopAgent();
    }

    AgentPtr GlobalAgent() {
        // Reader path: a single owning load, no global_agent_mutex — taking
        // the writers' mutex here would stall every request behind a reload in
        // progress. Deliberately uncached (see global_agent()): a TLS snapshot
        // would pin the agent per thread, and the host may keep the returned
        // AgentPtr beyond this call, so an owning copy is needed regardless.
        auto agent = global_agent().load();
        if (agent == nullptr) {
            return noopAgent();
        }
        return agent;
    }

    void set_global_agent(std::shared_ptr<AgentImpl> agent) {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        global_agent().store(std::move(agent));
    }

    void reset_global_agent() {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        global_agent().store(nullptr);
    }

}  // namespace pinpoint
