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
#include <charconv>
#include <chrono>
#include <csignal>
#include <string>
#include <exception>
#include <utility>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>
#include <pthread.h>
#include <unistd.h>

#include "absl/strings/match.h"

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

        // Heap-allocated and never destroyed so its own static destruction
        // cannot release the last AgentImpl: tearing the agent down during
        // __cxa_atexit (thread joins, gRPC channel teardown, logging through
        // possibly-destroyed singletons) is unsafe for a library embedded in a
        // host application. A global agent is torn down through explicit
        // Shutdown(); a non-global instance can still die with its last owner.
        //
        // SnapshotCache::Uncached (the default) is load-bearing for the same
        // invariant: a ThreadCached holder would pin the agent in every reader
        // thread's TLS until its next load or exit, deferring the final
        // release — and ~AgentImpl — into thread/process teardown.
        AtomicSharedPtr<AgentImpl>& global_agent() {
            static auto* holder = new AtomicSharedPtr<AgentImpl>();
            return *holder;
        }

        // Hard wall-clock bound for the blocking phase of do_shutdown(). gRPC
        // cancellation is best-effort and the config watcher can sit in a
        // filesystem call on a hung mount, so the joins cannot be bounded
        // individually; the whole blocking teardown runs under this deadline
        // and is handed to a detached reaper when it expires (see
        // teardown_workers_with_deadline()).
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

        // Snapshot the immutable identity fields once. retainNonReloadableFrom()
        // keeps them fixed across reloads, so the per-request getters below can
        // serve them without touching the atomic runtime_.
        app_type_ = app_type;
        app_name_ = cfg->app_name_;
        agent_id_ = std::make_shared<const std::string>(cfg->agent_id_);
        agent_name_ = cfg->agent_name_;
        service_name_ = cfg->service_name_;

        // Shared (see the member declarations): build_runtime() below hangs
        // both sinks off every runtime generation so spans reach them without
        // an agent keep-alive.
        agent_stats_ = std::make_shared<AgentStats>(this);
        url_stats_ = std::make_shared<UrlStats>(this);

        // Each cache shards its store by kDefaultCacheShardCount (see
        // ShardedLruCache): the api cache in particular is hit once per span
        // plus once per named span event, so a single rwlock's cache line
        // would ping-pong across all request threads.
        //
        // Sql.CacheLengthLimit additionally caps what the SQL caches may
        // retain (see kNoCacheLengthLimit): the normalizer admits statements
        // up to kMaxNormalizedSqlLength, so without it a 1024-entry cache is
        // ~1GB in the worst case, three times over. Startup-only,
        // like the cache sizes themselves. Deliberately not applied to
        // sql_cache_: its ids come from a sequence, so a bypassed statement
        // would burn a fresh id — and a fresh StringMeta — on every single
        // use. Java bypasses only the UID cache, for the same reason.
        const size_t sql_cache_length_limit =
            cfg->sql.cache_length_limit < 0
                ? kNoCacheLengthLimit
                : static_cast<size_t>(cfg->sql.cache_length_limit);
        // Only the UID cache expires: a hit there suppresses re-publication of
        // metadata whose collector-side row has a TTL of its own, so an entry
        // that outlives that row leaves the UI with no SQL text (see
        // SqlUidCache). The id caches have no equivalent problem — Java gives
        // them no TTL either — so they are left alone.
        const CacheExpiry sql_uid_expiry{
            cfg->sql.cache_expire_hours > 0
                ? std::chrono::duration_cast<CacheExpiry::Clock::duration>(
                      std::chrono::hours(cfg->sql.cache_expire_hours))
                : CacheExpiry::Clock::duration::zero()};
        api_cache_ = std::make_unique<ApiIdCache>(cache_size);
        error_cache_ = std::make_unique<IdCache>(cache_size);
        sql_cache_ = std::make_unique<IdCache>(cache_size);
        sql_uid_cache_ = std::make_unique<SqlUidCache>(
            cache_size, kDefaultCacheShardCount, sql_cache_length_limit,
            sql_uid_expiry);
        raw_sql_id_cache_ = std::make_unique<RawSqlCache>(
            cache_size, kDefaultCacheShardCount, sql_cache_length_limit);
        raw_sql_uid_cache_ = std::make_unique<RawSqlCache>(
            cache_size, kDefaultCacheShardCount, sql_cache_length_limit);
        sql_normalizer_ = std::make_unique<const SqlNormalizer>(
            kMaxNormalizedSqlLength, cfg->sql.remove_comments);

        // Initial build: no previous runtime, so every component is created
        // and published together in one atomic store. The constructor stays
        // "cold": it starts no threads, opens no gRPC channel and installs no
        // config-file watcher — Start() does all of that. The split keeps
        // construction infallible past this point and lets tests exercise a
        // built-but-offline agent.
        apply_config(std::move(cfg));
    }

    namespace {
        // Blocks (nearly) all signals on the calling thread for the enclosing
        // scope, restoring the previous mask on exit. Threads created inside
        // the scope inherit the mask, and threads THEY create — gRPC's
        // internals and the init thread's worker pool — inherit it
        // transitively, so signal-driven hosts (an nginx worker relying on a
        // process-directed signal to interrupt its own epoll_wait) keep
        // receiving signals on a host thread. The synchronous fatal signals —
        // hardware faults and seccomp's SIGSYS — stay unblocked: a crash
        // inside an agent thread must still raise the normal fatal signal.
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
        if (owner_pid_ != 0 && owner_pid_ != current_pid()) {
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

        owner_pid_ = current_pid();

        // gRPC and fork(): the constructor triggers no grpc_init — nothing is
        // built until openChannel() runs from init_grpc_workers below — so
        // this Start() performs the process's first, fresh gRPC init. That is
        // gRPC's own "instantiate gRPC objects only after fork()" pattern,
        // needing no pthread_atfork handlers or GRPC_ENABLE_FORK_SUPPORT. (It
        // assumes the HOST likewise does not use gRPC before forking.)

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
                // make_config(options_, old) returns the final reload config —
                // non-reloadable fields retained, logger already reconfigured
                // — and re-reads the file itself. Runs on the watcher thread;
                // teardown_workers() joins the watcher while the agent is
                // still guaranteed alive, so `this` stays valid throughout.
                auto new_cfg = make_config(options_, getConfig());
                if (new_cfg) {
                    reloadConfig(std::move(new_cfg));
                    LOG_INFO("agent config reloaded");
                }
            });
            config_watcher_->start();
        }

        try {
            init_thread_ = spawn_worker(kInit, [this] { init_grpc_workers(); });
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

    SpanConfigSnapshot AgentImpl::GetConfigSnapshot() const try {
        const auto config = getConfig();
        return config ? make_config_snapshot(*this, *config) : SpanConfigSnapshot{};
    } catch (...) {
        // Public API boundary: snapshot building only copies strings/vectors,
        // but an allocation failure must not leak into the embedder.
        return {};
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

    const std::string& AgentImpl::getServiceName() const {
        return service_name_;
    }

    void AgentImpl::build_header_recorders(AgentRuntime& rt, const Config& cfg) {
        const struct {
            int32_t annotation_key;
            const std::vector<std::string>& config_value;
            std::shared_ptr<HttpHeaderRecorder>& dst;
        } rows[] = {
            {ANNOTATION_HTTP_REQUEST_HEADER,  cfg.http.server.rec_request_header,  rt.http_srv_header_recorder[HTTP_REQUEST]},
            {ANNOTATION_HTTP_RESPONSE_HEADER, cfg.http.server.rec_response_header, rt.http_srv_header_recorder[HTTP_RESPONSE]},
            {ANNOTATION_HTTP_COOKIE,          cfg.http.server.rec_request_cookie,  rt.http_srv_header_recorder[HTTP_COOKIE]},
            {ANNOTATION_HTTP_REQUEST_HEADER,  cfg.http.client.rec_request_header,  rt.http_cli_header_recorder[HTTP_REQUEST]},
            {ANNOTATION_HTTP_RESPONSE_HEADER, cfg.http.client.rec_response_header, rt.http_cli_header_recorder[HTTP_RESPONSE]},
            {ANNOTATION_HTTP_COOKIE,          cfg.http.client.rec_request_cookie,  rt.http_cli_header_recorder[HTTP_COOKIE]},
        };

        for (const auto& row : rows) {
            if (!row.config_value.empty()) {
                row.dst = std::make_shared<HttpHeaderRecorder>(row.annotation_key, row.config_value);
            }
        }
    }

    std::shared_ptr<const AgentRuntime> AgentImpl::build_runtime(
            std::shared_ptr<const Config> cfg) {
        // Stateless components are rebuilt unconditionally. A reload only fires
        // when the config file's mtime actually changed (see ConfigFileWatcher),
        // so this runs on operator edits, not on a timer: the cost is one
        // pattern compile per edit. The sampler and the exception-chain limiter
        // are the exception — they carry accumulated state, so they are
        // carried over whenever their backing config is unchanged (see below).
        //
        // Null on the constructor's first call, which builds everything fresh.
        const auto prev = runtime_.load();
        const Config* prev_cfg = prev ? prev->config.get() : nullptr;

        auto rt = std::make_shared<AgentRuntime>();
        rt->config = std::move(cfg);
        // Not config-derived: every generation shares the same stats sinks,
        // so spans admitted under different generations aggregate into one
        // place (see AgentRuntime::stats for why spans take them from the
        // runtime instead of an agent keep-alive).
        rt->stats = agent_stats_;
        rt->url_stats = url_stats_;
        const Config& c = *rt->config;

        // Carried over when every Sampling.* key is unchanged, matching Go
        // (newTraceSampler, gated on sameValues). Rebuilding restarts the
        // sampler's counter at 0, and the counter is tested pre-increment, so
        // `CounterRate: 100` would sample the very next request after any
        // unrelated config edit; the throughput buckets would likewise start
        // empty and hand out a fresh second of burst. Type is compared
        // case-insensitively, the way it is resolved below; swapping the
        // COUNTER/COUNTING aliases still rebuilds, which is rare and harmless.
        const bool same_sampling =
            prev_cfg != nullptr &&
            absl::EqualsIgnoreCase(prev_cfg->sampling.type, c.sampling.type) &&
            prev_cfg->sampling.counter_rate == c.sampling.counter_rate &&
            prev_cfg->sampling.percent_rate == c.sampling.percent_rate &&
            prev_cfg->sampling.new_throughput == c.sampling.new_throughput &&
            prev_cfg->sampling.cont_throughput == c.sampling.cont_throughput;

        if (same_sampling) {
            rt->sampler = prev->sampler;
        } else {
            std::unique_ptr<Sampler> sampler;
            if (absl::EqualsIgnoreCase(c.sampling.type, PERCENT_SAMPLING)) {
                sampler = std::make_unique<PercentSampler>(c.sampling.percent_rate);
            } else {
                sampler = std::make_unique<CounterSampler>(c.sampling.counter_rate);
            }
            // Non-positive throughput creates no limiter, so the default config
            // gets the plain pass-through sampler.
            rt->sampler = std::make_shared<TraceSampler>(this, std::move(sampler),
                                                         c.sampling.new_throughput,
                                                         c.sampling.cont_throughput);
        }

        // Java's ExceptionChainSampler: an error storm produces one exception
        // metadata per errored span, so new chains are admitted at most
        // CallstackTraceNewThroughput per second and the rest are recorded as
        // plain errors without a call stack. Non-positive is unlimited, null.
        //
        // Carried over on an unchanged throughput for the same reason as the
        // sampler (Go's newExceptionLimiter): a rebuilt bucket is a full second
        // of exception chains, so a reload during an error storm would lift the
        // cap it exists to enforce.
        if (prev_cfg != nullptr &&
            prev_cfg->callstack_trace_new_throughput == c.callstack_trace_new_throughput) {
            rt->exception_chain_limiter = prev->exception_chain_limiter;
        } else {
            rt->exception_chain_limiter = c.callstack_trace_new_throughput > 0
                ? std::make_shared<RateLimiter>(
                      static_cast<uint64_t>(c.callstack_trace_new_throughput))
                : nullptr;
        }

        // An empty config list means the filter is off, represented as null.
        rt->http_url_filter = c.http.server.exclude_url.empty()
            ? nullptr : std::make_shared<HttpUrlFilter>(c.http.server.exclude_url);
        rt->http_method_filter = c.http.server.exclude_method.empty()
            ? nullptr : std::make_shared<HttpMethodFilter>(c.http.server.exclude_method);
        rt->http_status_errors = c.http.server.status_errors.empty()
            ? nullptr : std::make_shared<HttpStatusErrors>(c.http.server.status_errors);

        build_header_recorders(*rt, c);
        return rt;
    }

    void AgentImpl::apply_config(std::shared_ptr<const Config> cfg) {
        // Refresh the prepareSql() fast-path flag (see raw_sql_cache_enabled_).
        // Relaxed and slightly ahead of the runtime_ swap below: prepareSql
        // tolerates a stale value for the instant around a reload.
        raw_sql_cache_enabled_.store(cfg->sql.enable_raw_sql_cache,
                                     std::memory_order_relaxed);
        runtime_.store(build_runtime(std::move(cfg)));
    }

    void AgentImpl::reloadConfig(std::shared_ptr<const Config> cfg) {
        // Serialize writers so the two stores in apply_config stay paired: a
        // test-driven reload and the config-file watcher thread could
        // otherwise interleave and leave raw_sql_cache_enabled_ describing a
        // different generation than the published runtime_. Readers do not
        // take reload_mutex_.
        std::lock_guard<std::mutex> reload_lock(reload_mutex_);
        apply_config(std::move(cfg));
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

        ping_thread_ = spawn_worker(kPing, [this] { grpc_agent_->sendPingWorker(); });
        meta_thread_ = spawn_worker(kMeta, [this] { grpc_metadata_->sendMetaWorker(); });
        span_thread_ = spawn_worker(kSpan, [this] { grpc_span_->sendSpanWorker(); });
        stat_thread_ = spawn_worker(kStat, [this] { grpc_stat_->sendStatsWorker(); });
        if (grpc_command_) {
            command_thread_ = spawn_worker(kCommand, [this] { grpc_command_->commandWorker(); });
        }
        url_stat_add_thread_ = spawn_worker(kUrlStatAdd, [this] { url_stats_->addUrlStatsWorker(); });
        url_stat_send_thread_ = spawn_worker(kUrlStatSend, [this] { url_stats_->sendUrlStatsWorker(); });
        agent_stat_thread_ = spawn_worker(kAgentStat, [this] { agent_stats_->agentStatsWorker(); });

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
        // Terminal, unlike a registration failure (which retries above):
        // nothing ever re-runs this function, so the agent can never come
        // online. Mark it so StartAgent() replaces this instance instead of
        // returning it as the running agent forever. Set after enabled_ so
        // an observer of init_failed_ never sees a still-enabled agent.
        init_failed_ = true;
        return;
    } catch (...) {
        LOG_ERROR("failed to init grpc workers: unknown exception");
        enabled_ = false;
        init_failed_ = true;
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
        try { if (grpc_command_) grpc_command_->requestStopCommandWorker(); } catch (...) {}
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

    std::thread AgentImpl::spawn_worker(Worker worker, std::function<void()> body) {
        const unsigned bit = 1u << worker;
        running_workers_.fetch_or(bit, std::memory_order_relaxed);
        try {
            // Clearing the bit dereferences `this` after the body returns,
            // which is safe for the same reason the body itself is: every
            // worker is joined (or kept alive by the teardown reaper) before
            // the agent is destroyed.
            return std::thread{[this, bit, body = std::move(body)] {
                body();
                running_workers_.fetch_and(~bit, std::memory_order_relaxed);
            }};
        } catch (...) {
            running_workers_.fetch_and(~bit, std::memory_order_relaxed);
            throw;
        }
    }

    std::string AgentImpl::running_worker_names() const {
        static constexpr const char* kNames[kWorkerCount] = {
            "init", "ping", "meta", "span", "stat", "command",
            "url-stat-add", "url-stat-send", "agent-stat",
        };
        const unsigned running = running_workers_.load(std::memory_order_relaxed);
        std::string names;
        for (unsigned i = 0; i < kWorkerCount; ++i) {
            if (running & (1u << i)) {
                names += names.empty() ? "" : ", ";
                names += kNames[i];
            }
        }
        // Empty means the straggler is outside these threads: the config
        // watcher's stop, the AgentInfo scheduler's join or a closeChannel().
        return names.empty() ? "none (config watcher, AgentInfo scheduler or channel close)" : names;
    }

    bool AgentImpl::teardown_workers_with_deadline(bool may_defer_destroy,
                                                   bool& runner_detached) noexcept {
        runner_detached = false;
        // The workers dereference `this` (isExiting/getConfig/getAgentStats),
        // so they must be joined, never detached, before the members they use
        // are destroyed — abandoning a straggler would be a use-after-free.
        // But TryCancel() is best-effort and callback completion has no hard
        // wall-clock bound, so the joins are unbounded. To still bound
        // shutdown, the whole blocking teardown runs on a helper thread: beat
        // the deadline and the helper is joined as before; otherwise it is
        // detached and the object's lifetime secured for it — via a keep-alive
        // self-reference (Shutdown()) or a deferred destroy (SharedDeleter).
        struct TeardownState {
            std::mutex m;
            std::condition_variable cv;
            bool finished{false};
            bool abandoned{false};
            // At most one of the two is set when the deadline expires.
            // keep_alive holds this agent (and every member the draining
            // workers use) alive until the detached runner finishes; releasing
            // it may run the SharedDeleter on the runner thread, benign there
            // since every worker is joined by then. delete_when_done instead
            // hands the runner ownership of an object whose final reference is
            // already gone: it deletes after the joins — a leak only if the
            // stragglers never finish.
            std::shared_ptr<AgentImpl> keep_alive;
            bool delete_when_done{false};
            std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
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
                    // do_shutdown() returned without closing the logger
                    // precisely so this line, and whatever the stragglers
                    // logged on the way out, are still recorded; closing it
                    // is this thread's job now that nothing else will write.
                    // Logger is a never-destroyed singleton with an
                    // idempotent, mutex-guarded shutdown.
                    try {
                        LOG_INFO("agent shutdown: background teardown finished {}ms after shutdown began",
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - state->started).count());
                    } catch (...) {}
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
            // bound the joins, so tear down inline. Unbounded, but the stop
            // signals are already sent and a process too resource-exhausted
            // to spawn one thread during shutdown is past caring about the
            // deadline.
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
        // runner: a keep-alive self-reference while shared-owned (Shutdown()),
        // or handing over ownership when the SharedDeleter is the caller. An
        // object that is neither dies with its scope regardless, so the
        // unbounded join stays its only lifetime-safe option.
        std::shared_ptr<AgentImpl> keep_alive = weak_from_this().lock();
        if (keep_alive == nullptr && !may_defer_destroy) {
            try {
                LOG_WARN("agent shutdown exceeded the {}ms deadline; not shared-owned, "
                         "waiting for workers to finish; worker threads not yet joined: {}",
                         agent_shutdown_deadline().count(), running_worker_names());
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
        // The helper owns the logger's close from here (see the header): its
        // completion line and everything the draining workers log still have
        // to get through.
        runner_detached = true;
        try {
            LOG_WARN("agent shutdown exceeded the {}ms deadline; workers keep draining "
                     "in the background and the agent is {} when they finish; "
                     "worker threads not yet joined: {}",
                     agent_shutdown_deadline().count(),
                     defer_destroy ? "destroyed" : "released", running_worker_names());
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

        // Destroying parent-created gRPC clients in a forked child is unsafe:
        // they own internal threads that do not exist in this process, and
        // their destructors would join the dead handles and abort. Leak the
        // client objects instead — the child either builds its own agent via
        // Start() or is short-lived. Done here rather than in do_shutdown() so
        // the pointers stay valid while the object is alive.
        //
        // The stats aggregators go the same way, for a second reason: they own
        // condition variables the abandoned url_stat/agent_stat workers were
        // waiting on when fork() ran. The waiter count lives in the condvar and
        // is inherited, so glibc's pthread_cond_destroy blocks forever on
        // waiters that do not exist here. They are shared-owned (every
        // AgentRuntime generation carries them), so nulling this member is not
        // enough — inherited snapshots in the runtime_ holder, live spans and
        // TLS caches hold them too, and whichever reference dies LAST would run
        // those destructors. Leak one extra strong reference so no release here
        // can be the last. nothrow keeps the noexcept contract; if even this
        // allocation fails the leak is skipped and the old exposure returns.
        if (owner_pid_ != 0 && owner_pid_ != current_pid()) {
            (void)grpc_agent_.release();
            (void)grpc_metadata_.release();
            (void)grpc_span_.release();
            (void)grpc_stat_.release();
            (void)grpc_command_.release();
            (void)new (std::nothrow) std::shared_ptr<UrlStats>(url_stats_);
            (void)new (std::nothrow) std::shared_ptr<AgentStats>(agent_stats_);
        }
    }

    bool AgentImpl::tracing_active() const noexcept {
        // Fast reject first: a disabled agent pays nothing extra here.
        if (!enabled_) {
            return false;
        }
        // One pid read per span creation, only on the enabled path — served
        // from current_pid()'s fork-hooked cache, so a relaxed atomic load
        // rather than the syscall getpid() is on Linux (see utility.h). An
        // agent inherited across fork() carries the parent's enabled_ == true,
        // but its worker threads do not exist here: recording would enqueue
        // spans into queues nothing drains, silently, since even the drop
        // reporter runs on the missing worker. Race-free: owner_pid_ is
        // written before the init thread that publishes enabled_ = true is
        // spawned.
        if (owner_pid_ != 0 && owner_pid_ != current_pid()) {
            warn_fork_inheritance();
            return false;
        }
        return true;
    }

    SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point) {
        NoopTraceContextReader reader;
        return NewSpan(operation, rpc_point, reader);
    }

    SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point,
                               TraceContextReader& reader) {
        return NewSpan(operation, rpc_point, "", reader);
    }

    namespace {
        // TraceContextReader over a caller-built map of canonical Pinpoint
        // propagation headers, so the map-based NewSpan overload reuses the
        // reader-based extraction funnel unchanged.
        class MapTraceContextReader final : public TraceContextReader {
        public:
            explicit MapTraceContextReader(const std::map<std::string, std::string>& headers)
                : headers_(headers) {}

            std::optional<std::string_view> Get(std::string_view key) const override {
                // Keys are the short canonical Pinpoint-* names, so the lookup
                // string stays within SSO.
                const auto it = headers_.find(std::string(key));
                if (it == headers_.end()) {
                    return std::nullopt;
                }
                return std::string_view(it->second);
            }

        private:
            const std::map<std::string, std::string>& headers_;
        };
    }

    SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point,
                               std::string_view method,
                               const std::map<std::string, std::string>& pinpoint_headers) {
        MapTraceContextReader reader(pinpoint_headers);
        return NewSpan(operation, rpc_point, method, reader);
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
        // request and then moves into the span. AtomicSharedPtr localizes the
        // cached snapshot's control block per reader thread, so this owning
        // copy keeps re-entrant reader.Get() calls safe without making request
        // threads contend on the shared runtime control block.
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

        auto tid = reader.Get(HEADER_TRACE_ID);
        if (tid.has_value() && tid->empty()) {
            // A present-but-blank header carries no trace id. Left as-is it
            // took the continued path: it spent a continue-sampler slot and
            // then failed to parse, so the request became a noop span and
            // never appeared in Pinpoint at all. Java and Go both read a blank
            // header as no header and start a new trace; drop it here so the
            // sampling decision, the parse and the extract all agree.
            tid.reset();
        }
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
            span->extractContext(reader, std::move(trace_id), tid.has_value());
            return span;
        }
        return std::make_shared<UnsampledSpan>(this, std::move(runtime));
    } CATCH_AND_LOG_RETURN("new span", noopSpan())

    bool AgentImpl::Enable() {
        return tracing_active();
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
        if (owner_pid_ != 0 && owner_pid_ != current_pid()) {
            try { LOG_INFO("agent shutdown in forked child: abandoning inherited workers"); } catch (...) {}
            abandon_grpc_workers();
            // The watcher thread does not exist in this process either; its
            // stop() abandons the inherited dead handle via its own pid guard.
            try { if (config_watcher_) config_watcher_->stop(); } catch (...) {}
            // The inherited gRPC clients are intentionally leaked, but not
            // here: nulling the unique_ptrs while a racing thread that read
            // enabled_ == true just before the store above is about to
            // dereference grpc_span_ / grpc_metadata_ would crash the host on
            // a null operator->. Deferred to ~AgentImpl instead, so the
            // pointers stay valid for this object's whole lifetime.
            return false;
        }

        // Serialization is achieved: the lock above waited out any in-flight
        // Start() and published its writes, and later calls refuse on
        // shutting_down_. Release it before the teardown — on the
        // deferred-destroy path the runner may delete this object as soon as
        // ownership is handed over, and unlocking a destroyed mutex is UB.
        if (lifecycle_lock.owns_lock()) {
            lifecycle_lock.unlock();
        }

        try { LOG_INFO("agent shutdown"); } catch (...) {}
        request_stop_workers();

        // A never-launched agent (Start() never ran, or its synchronous failure
        // already reset started_ and stopped the watcher) has no init thread
        // and no workers, so the blocking teardown cannot block. Run it inline
        // to skip the deadline runner thread — which matters on the
        // StartAgent() failure path, where the failure may itself have been
        // thread-creation exhaustion. Reading started_ is stable: the lifecycle
        // lock waited out any in-flight Start(), and later calls refuse on
        // shutting_down_ before touching it.
        if (!started_) {
            teardown_workers();
            try { shutdown_logger(); } catch (...) {}
            return false;
        }

        bool runner_detached = false;
        const bool destroy_deferred =
            teardown_workers_with_deadline(may_defer_destroy, runner_detached);
        // Nothing below may touch members: on the deferred-destroy path the
        // runner owns the object from here and may already have deleted it.
        // runner_detached is a local, so reading it stays safe.
        //
        // Closing the logger here would drop exactly the lines a detached
        // runner exists to produce — the stragglers' output and its own
        // "teardown finished" line — so on that path the runner closes it
        // instead, once there is nothing left to write.
        if (!runner_detached) {
            try { shutdown_logger(); } catch (...) {}
        }
        return destroy_deferred;
    }

    namespace {
        // Java's IdValidateUtils.checkId charset ([a-zA-Z0-9._-]+). The agent id
        // is echoed to every downstream call and displayed as HTML by the web UI
        // (TransactionIdUtils.java: "should not use html syntax"), so anything
        // else — '<', '>', CR/LF, control bytes — is rejected here.
        bool isIdChars(std::string_view s) noexcept {
            for (const unsigned char c : s) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                if (!ok) return false;
            }
            return true;
        }

        // Strict Long.parseLong: optional leading '+'/'-', then ASCII digits only,
        // no whitespace, range-checked. stoll_ (absl::SimpleAtoi) is not used
        // here because it tolerates surrounding whitespace, which Java rejects.
        std::optional<int64_t> parseLongStrict(std::string_view s) noexcept {
            const char* first = s.data();
            const char* const last = first + s.size();
            if (first != last && (*first == '+' || *first == '-')) ++first;
            if (first == last) return std::nullopt;
            for (const char* p = first; p != last; ++p) {
                if (*p < '0' || *p > '9') return std::nullopt;
            }
            int64_t v = 0;
            const char* const begin = (s.front() == '+') ? first : s.data(); // from_chars rejects '+'
            if (std::from_chars(begin, last, v).ec != std::errc{}) return std::nullopt;
            return v;
        }
    }

    // Mirrors Java TransactionIdUtils.parseTransactionId: agentId^startTime^sequence,
    // agent id checked for charset only (the 24-char limit applies to self
    // registration, not to inbound ids), a fourth field ignored. Warnings are
    // throttled per reason: a malformed header is peer-controlled input that can
    // recur once per request.
    TraceId TraceId::parseTraceId(std::string_view txid) noexcept try {
        constexpr size_t kMaxInt64StringLength = 20; // max digits of int64_t

        const std::string_view sv = txid;

        // Validate the structure before building anything: any malformation
        // yields an empty TraceId so the caller drops to a noop span.
        // AgentId (first field before '^')
        const auto pos1 = sv.find('^');
        if (pos1 == std::string_view::npos) {
            LOG_WARN_THROTTLED("parsing Txid: invalid txid format = {}", sv);
            return {};
        }
        if (pos1 == 0) {
            LOG_WARN_THROTTLED("parsing Txid: empty AgentId = {}", sv);
            return {};
        }
        if (!isIdChars(sv.substr(0, pos1))) {
            LOG_WARN_THROTTLED("parsing Txid: AgentId contains characters outside [a-zA-Z0-9._-] (length={})", pos1);
            return {};
        }
        // StartTime (second field)
        const auto pos2 = sv.find('^', pos1 + 1);
        if (pos2 == std::string_view::npos) {
            LOG_WARN_THROTTLED("parsing Txid: invalid txid format = {}", sv);
            return {};
        }
        const auto start_time_len = pos2 - pos1 - 1;
        if (start_time_len > kMaxInt64StringLength) {
            LOG_WARN_THROTTLED("parsing Txid: StartTime too long (length={}, max={})", start_time_len, kMaxInt64StringLength);
            return {};
        }
        // Sequence (third field), cut at the next '^' like Java: anything after
        // it is ignored rather than rejected. (Java: "next index may not exist
        // since default value does not have a delimiter after
        // transactionSequence. may need fixing when id spec changes".)
        const auto pos3 = sv.find('^', pos2 + 1);
        const auto sequence_str = sv.substr(pos2 + 1, pos3 == std::string_view::npos ? std::string_view::npos : pos3 - pos2 - 1);
        if (sequence_str.length() > kMaxInt64StringLength) {
            LOG_WARN_THROTTLED("parsing Txid: Sequence too long (length={}, max={})", sequence_str.length(), kMaxInt64StringLength);
            return {};
        }

        // Non-numeric fields are rejected structurally like the malformations
        // above: absorbing them as 0 via value_or would record a live trace on
        // which every distinct malformed header collides at (agentId, 0, 0).
        const auto start_time = parseLongStrict(sv.substr(pos1 + 1, start_time_len));
        const auto sequence = parseLongStrict(sequence_str);
        if (!start_time || !sequence) {
            LOG_WARN_THROTTLED("parsing Txid: invalid txid format = {}", sv);
            return {};
        }

        return TraceId{sv.substr(0, pos1), *start_time, *sequence};
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

        auto meta = std::make_unique<MetaData>(ApiMeta(id, api_type, api_str));
        grpc_metadata_->enqueueMeta(std::move(meta));

        return id;
    } CATCH_AND_LOG_RETURN("failed to cache api meta:", 0)

    // The removeCache* functions carry the same exception boundary as their
    // cache* siblings: they build allocating keys, and their caller is the
    // meta worker loop — an escaping exception would trip its supervisor
    // restart for what is only a best-effort cache eviction.
    void AgentImpl::removeCacheApi(const ApiMeta& api_meta) const try {
        if (enabled_) {
            api_cache_->remove(ApiCacheKey{api_meta.api_str_, api_meta.type_},
                               api_meta.id_);
        }
    } CATCH_AND_LOG("failed to remove cached api meta:")

    int32_t AgentImpl::cacheError(std::string_view error_name) const try {
        if (!enabled_) {
            return 0;
        }

        const auto [id, found] = error_cache_->get(error_name);
        if (found) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(StringMeta(id, error_name, STRING_META_ERROR));
        grpc_metadata_->enqueueMeta(std::move(meta));

        return id;
    } CATCH_AND_LOG_RETURN("failed to cache error meta:", 0)

    void AgentImpl::removeCacheError(const StringMeta& error_meta) const try {
        if (enabled_) {
            error_cache_->remove(error_meta.str_val_, error_meta.id_);
        }
    } CATCH_AND_LOG("failed to remove cached error meta:")

    int32_t AgentImpl::cacheSql(std::string_view sql_query) const try {
        if (!enabled_) {
            return 0;
        }

        const auto [id, found] = sql_cache_->get(sql_query);
        if (found) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(StringMeta(id, sql_query, STRING_META_SQL));
        grpc_metadata_->enqueueMeta(std::move(meta));

        return id;
    } CATCH_AND_LOG_RETURN("failed to cache sql meta:", 0)

    std::optional<PreparedSqlResult> AgentImpl::prepareSql(
            std::string_view raw_sql, SqlMetaMode mode) const try {
        if (!enabled_) {
            return std::nullopt;
        }
        if (mode != SqlMetaMode::Id && mode != SqlMetaMode::Uid) {
            return std::nullopt;
        }

        const SqlNormalizer& normalizer = *sql_normalizer_;
        // The raw cache holds the normalization result only. Resolving the
        // id/uid below instead of caching it here is what keeps one failed
        // metadata send from invalidating unrelated entries (see PreparedSql);
        // the cost is one shared-lock lookup in the id/uid cache per use.
        auto prepare = [&]() -> PreparedSqlRef {
            auto normalized = normalizer.normalize(raw_sql);
            return std::make_shared<const PreparedSql>(PreparedSql{
                std::move(normalized.parameters),
                std::move(normalized.normalized_sql)});
        };

        auto& cache = (mode == SqlMetaMode::Id) ? *raw_sql_id_cache_
                                                : *raw_sql_uid_cache_;
        // One relaxed load instead of a runtime snapshot lookup plus owning
        // Config copy: this runs once per SQL statement and only needs this flag.
        auto sql = raw_sql_cache_enabled_.load(std::memory_order_relaxed)
                       ? cache.get(raw_sql, prepare).value
                       : prepare();

        if (mode == SqlMetaMode::Id) {
            const auto id = cacheSql(sql->normalized_sql);
            if (id <= 0) {
                // cacheSql() already logged the underlying failure. Drop the
                // annotation rather than record an id the collector has no
                // metadata for; the cached normalization stays valid and the
                // next use retries the registration.
                return std::nullopt;
            }
            return PreparedSqlResult{std::move(sql), SqlIdentity{id}};
        }

        const auto uid = cacheSqlUid(sql->normalized_sql);
        if (!uid) {
            return std::nullopt;
        }
        return PreparedSqlResult{std::move(sql), SqlIdentity{*uid}};
    } CATCH_AND_LOG_RETURN("failed to prepare raw sql:", std::nullopt)

    void AgentImpl::removeCacheSql(const StringMeta& sql_meta) const try {
        if (enabled_) {
            // The raw cache needs no invalidation: it holds no ids, so the
            // next use of any raw variant re-resolves through this cache and
            // picks up the fresh id on its own.
            sql_cache_->remove(sql_meta.str_val_, sql_meta.id_);
        }
    } CATCH_AND_LOG("failed to remove cached sql meta:")

    std::optional<SqlUid> AgentImpl::cacheSqlUid(std::string_view sql) const try {
        if (!enabled_) {
            return std::nullopt;
        }

        const auto [uid, found] = sql_uid_cache_->get(sql);
        if (found) {
            return uid;
        }

        // Cold path (first time this SQL is seen): enqueue the UID for the
        // collector. A statement the cache bypassed has no entry to evict on
        // send failure, so the meta carries no cache key for it — otherwise
        // every use of a huge statement would park up to 1 MiB in the queue.
        auto meta = std::make_unique<MetaData>(
            SqlUidMeta(uid, sql, /*cached=*/!sql_uid_cache_->bypasses(sql)));
        grpc_metadata_->enqueueMeta(std::move(meta));

        return uid;
    } CATCH_AND_LOG_RETURN("failed to cache sql uid meta:", std::nullopt)

    void AgentImpl::removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const try {
        // An empty key means the uid cache bypassed the statement: nothing to evict.
        if (enabled_ && !sql_uid_meta.cache_key_.empty()) {
            sql_uid_cache_->remove(sql_uid_meta.cache_key_, sql_uid_meta.uid_);
        }
    } CATCH_AND_LOG("failed to remove cached sql uid meta:")

    void AgentImpl::recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                                    std::vector<std::unique_ptr<Exception>>&& exceptions) const try {
        // Cheap flag first, config load second (same ordering as the getters
        // below): a disabled agent must not look up or retain a runtime snapshot.
        if (!enabled_ || !getConfig()->enable_callstack_trace) {
            return;
        }

        auto meta = std::make_unique<MetaData>(ExceptionMeta(trace_id, span_id, url_template,
                                                             std::move(exceptions)));
        grpc_metadata_->enqueueMeta(std::move(meta));
    } CATCH_AND_LOG("failed to record exception meta:")

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

    void AgentImpl::recordHttpHeader(const bool server, const HeaderType which,
                                     HeaderReader& reader, PinpointAnnotation* annotation) const {
        if (!enabled_ || which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto& runtime = runtime_.load_cached_ref();
        // Owning copy of the recorder, not a reference into the snapshot:
        // recordHeader() runs host code (reader.Get()), and a re-entrant load
        // of runtime_ racing a config reload would refresh the TLS entry the
        // snapshot lives in, destroying a merely-referenced recorder mid-call.
        const auto recorder = server ? runtime->http_srv_header_recorder[which]
                                     : runtime->http_cli_header_recorder[which];
        if (recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    void AgentImpl::recordServerHeader(const HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const {
        recordHttpHeader(true, which, reader, annotation);
    }

    void AgentImpl::recordClientHeader(const HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const {
        recordHttpHeader(false, which, reader, annotation);
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
        } CATCH_AND_LOG_RETURN("make agent", nullptr)
    }

    // Public entry point: a failure to configure or construct the agent must
    // surface as a false return, never as an exception in the host
    // application.
    bool StartAgent(const AgentOptions& options) try {
        std::lock_guard<std::mutex> lock(global_agent_mutex);

        auto agent = global_agent().load();

        // Already running in this process: StartAgent() is one-shot per
        // process. Config changes flow through the config-file watcher, not
        // through repeated StartAgent() calls. Decided before the sink swap
        // below, so a repeated call cannot re-route the running agent's log.
        if (agent != nullptr && agent->ownedByThisProcess() &&
            !agent->isExiting() && !agent->initFailed()) {
            LOG_WARN("StartAgent() called again in this process; keeping the running agent");
            return true;
        }

        // Before anything that can log, so the host's own log pipeline also
        // gets the refusals and configuration errors below — those are exactly
        // the lines a host debugging a silent agent needs. An options object
        // without a sink clears any previous one, which is what a fresh start
        // means.
        Logger::getInstance().setSink(options.log_sink);

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
                return false;
            }

            // Neither a shut-down agent nor one whose async init failed is
            // restartable: drop it from the singleton and build a fresh one.
            // (Clearing it here also keeps GlobalAgent() degrading to the noop
            // agent if the rebuild fails.) Resetting what may be the last
            // reference tears the dead agent down through its SharedDeleter,
            // joining any workers a partial initialization spawned.
            if (agent->initFailed()) {
                LOG_WARN("global agent failed to initialize; replacing it with a new agent");
            } else {
                LOG_WARN("global agent is shut down; replacing it with a new agent");
            }
            global_agent().store(nullptr);
            agent.reset();
        }

        auto cfg = make_config(options);
        if (!cfg || !cfg->check()) {
            return false;
        }
        agent = make_agent(std::move(cfg), options);
        if (agent == nullptr) {
            return false;
        }
        agent->setOptions(options);
        // Publish only a successfully launched agent. A synchronous Start()
        // failure must not install a permanently cold instance: every later
        // StartAgent() would treat it as the running agent and never call
        // Start() again. Leaving the singleton empty makes the next call
        // rebuild and retry from scratch.
        if (!agent->Start()) {
            return false;
        }
        global_agent().store(agent);
        return true;
    } catch (...) {
        return false;
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
