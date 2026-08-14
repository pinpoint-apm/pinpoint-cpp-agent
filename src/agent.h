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

#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include <unistd.h>

#include "pinpoint/tracer.h"
#include "agent_runtime.h"
#include "atomic_shared_ptr.h"
#include "config.h"
#include "http.h"
#include "cache.h"
#include "sampling.h"
#include "span.h"
#include "stat.h"
#include "grpc.h"
#include "url_stat.h"
#include "utility.h"
#include "agent_service.h"

namespace pinpoint {

    /**
     * @brief Concrete agent: orchestrates span creation, metadata caching,
     *        gRPC workers and statistics collection, implementing both `Agent`
     *        (SDK surface) and `AgentService` (internal boundary).
     */
    class AgentImpl final : public Agent, public AgentService,
                            public std::enable_shared_from_this<AgentImpl> {
    public:
        /// Capacity of each metadata cache (api/error/sql/sql-uid/raw-sql).
        static constexpr size_t kDefaultCacheSize = 1024;

        /// @brief @p cache_size defaults to the production value; tests inject
        /// small sizes to exercise eviction-driven id reissue.
        AgentImpl(std::shared_ptr<const Config> options,
                  std::unique_ptr<GrpcAgent> grpc_agent,
                  std::unique_ptr<GrpcMetadata> grpc_metadata,
                  std::unique_ptr<GrpcSpan> grpc_span,
                  std::unique_ptr<GrpcStats> grpc_stat,
                  std::unique_ptr<GrpcCommand> grpc_command = nullptr,
                  int32_t app_type = DEFAULT_APP_TYPE,
                  size_t cache_size = kDefaultCacheSize);
        ~AgentImpl() noexcept override;

        /// @brief Deleter attached by createShared(): runs the (bounded)
        /// terminal teardown before destruction when the last reference is
        /// dropped without an explicit Shutdown(), and skips destruction
        /// entirely when the teardown runner took ownership of the object
        /// (deferred destroy).
        struct SharedDeleter {
            void operator()(AgentImpl* agent) const noexcept;
        };

        /// @brief Creates a shared agent whose final release stays bounded:
        /// dropping the last reference without Shutdown() runs the teardown
        /// under the shutdown deadline, and once that expires the teardown
        /// runner takes ownership and destroys the agent only after the
        /// straggling workers finish (a leak if they never do). A shared agent
        /// built without this factory keeps the unbounded-join fallback.
        template <typename... Args>
        static std::shared_ptr<AgentImpl> createShared(Args&&... args) {
            return std::shared_ptr<AgentImpl>(new AgentImpl(std::forward<Args>(args)...),
                                              SharedDeleter{});
        }

        /// @brief Creates a new span for an outbound operation.
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) override;
        /// @brief Creates a span and extracts the context from @p reader.
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, TraceContextReader& reader) override;
        /// @brief Creates an HTTP span, also recording the request method.
        SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method, TraceContextReader& reader) override;
        /// @brief Injects the StartAgent() options used by the config-file
        /// watcher and config reloads. Called by StartAgent() before Start();
        /// an agent built without options (tests) runs without a watcher.
        void setOptions(AgentOptions options) { options_ = std::move(options); }
        /// @brief Begins bringing the agent online: starts the config watcher
        /// (when enabled) and an init thread that opens channels and launches
        /// workers. Does not wait for collector registration.
        ///
        /// True when initialization was launched, or already had been
        /// (idempotent). False when refused — shut down (terminal:
        /// shutting_down_ is never cleared) or inherited across fork() — and
        /// when synchronous setup failed. A setup failure resets started_, and
        /// StartAgent() publishes the global instance only on success, so the
        /// next call retries from scratch.
        bool Start() noexcept;
        /// @brief Returns whether the agent is enabled for tracing. Always
        /// false (with a one-time error log) for an agent inherited across
        /// fork() — see warn_fork_inheritance().
        bool Enable() override;
        /// @brief True when this agent was started in the current process (or
        /// not started at all); false for a handle inherited across fork().
        bool ownedByThisProcess() const { return owner_pid_ == 0 || owner_pid_ == current_pid(); }
        /// @brief True when Start()'s async initialization failed terminally
        /// (channel bring-up or worker spawn threw on the init thread). Nothing
        /// re-runs init_grpc_workers, so such an agent can never come online
        /// and StartAgent() replaces it with a fresh one.
        bool initFailed() const { return init_failed_; }
        /// @brief Initiates a graceful shutdown of the agent. Terminal: this
        /// instance cannot be restarted, callers must build a new one through
        /// StartAgent(). See Agent::Shutdown().
        void Shutdown() noexcept override;

        bool isExiting() const override { return shutting_down_; }
        /// @brief Shared self-handle for span keep-alive; empty when the agent
        /// is not shared_ptr-owned (stack-constructed test instances).
        std::shared_ptr<AgentService> selfRef() noexcept override {
            return weak_from_this().lock();
        }
        const std::string& getAppName() const override;
        int32_t getAppType() const override;
        const std::string& getAgentId() const override;
        const std::string& getServiceName() const override;

        std::shared_ptr<const Config> getConfig() const override;
        int64_t getStartTime() const override { return start_time_; }
        /// @brief Reloads configuration-dependent helpers (samplers, filters, recorders).
        void reloadConfig(std::shared_ptr<const Config> cfg) override;

        TraceId generateTraceId() override;
        void recordSpan(std::unique_ptr<SpanChunk> span) const override;
        void recordUrlStat(UrlStatEntry stat) const override;
        void recordUrlStat(UrlStatEntry stat, const Config& config) const override;
        void recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                             std::vector<std::unique_ptr<Exception>>&& exceptions) const override;
        void recordStats(StatsType stats) const override;

        int32_t cacheApi(std::string_view api_str, int32_t api_type) const override;
        void removeCacheApi(const ApiMeta& api_meta) const override;
        int32_t cacheError(std::string_view error_name) const override;
        void removeCacheError(const StringMeta& error_meta) const override;
        int32_t cacheSql(std::string_view sql_query) const override;
        std::optional<PreparedSqlRef> prepareSql(std::string_view raw_sql, SqlMetaMode mode) const override;
        void removeCacheSql(const StringMeta& sql_meta) const override;
        std::optional<SqlUid> cacheSqlUid(std::string_view sql) const override;
        void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override;

        bool isStatusFail(int status) const override;
        void recordServerHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const override;
        void recordClientHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const override;
        void recordHttpHeader(bool server, HeaderType which, HeaderReader& reader,
                              PinpointAnnotation* annotation) const;

        AgentStats& getAgentStats() override { return *agent_stats_; }
        UrlStats& getUrlStats() override { return *url_stats_; }

    private:

        // Single source of truth for the config and its derived components
        // (see AgentRuntime). A generation-validated thread-local snapshot
        // gives a mutually consistent view without a shared lock on the
        // unchanged per-request path, and its per-thread control block keeps
        // owning NewSpan() copies from contending across request threads.
        // ThreadCached is safe here because AgentRuntime is passive data: a
        // reader's TLS may pin a snapshot past this agent's destruction, and
        // destroying it that late touches no agent state. Holders of active
        // objects (e.g. the global AgentImpl handle) must stay Uncached.
        AtomicSharedPtr<const AgentRuntime, SnapshotCache::ThreadCached> runtime_;

        // Identity fields snapshotted once at construction.
        // Config::retainNonReloadableFrom() keeps them fixed across reloads, so
        // they never change while this agent lives and are served
        // directly — without a runtime snapshot lookup, shared_ptr copy, or string
        // copy — on the per-request hot path (e.g. InjectContext, SpanData ctor,
        // generateTraceId).
        std::string app_name_;
        int32_t app_type_{};
        // Held as a shared string so generateTraceId() can seed every new
        // TraceId with a refcount bump instead of a per-trace allocation.
        // Written only by the ctor, so it is immutable for the agent's whole
        // lifetime and safe to read from any thread. Never null.
        std::shared_ptr<const std::string> agent_id_;
        std::string agent_name_;
        std::string service_name_;

        std::unique_ptr<ApiIdCache> api_cache_{};
        std::unique_ptr<IdCache> error_cache_{};
        std::unique_ptr<IdCache> sql_cache_{};
        std::unique_ptr<SqlUidCache> sql_uid_cache_{};
        std::unique_ptr<RawSqlCache> raw_sql_id_cache_{};
        std::unique_ptr<RawSqlCache> raw_sql_uid_cache_{};
        mutable std::atomic<uint64_t> sql_id_metadata_epoch_{0};
        mutable std::atomic<uint64_t> sql_uid_metadata_epoch_{0};
        // Mirror of config->sql.enable_raw_sql_cache, refreshed by
        // apply_config(). prepareSql() runs once per SQL statement and only
        // needs this one flag, so a relaxed load here avoids a runtime cache
        // lookup and owning Config snapshot on that hot path. Benignly stale
        // for the instant around a reload.
        std::atomic<bool> raw_sql_cache_enabled_{true};

        std::unique_ptr<GrpcAgent> grpc_agent_{};
        std::unique_ptr<GrpcMetadata> grpc_metadata_{};
        std::unique_ptr<GrpcSpan> grpc_span_{};
        std::unique_ptr<GrpcStats> grpc_stat_{};
        std::unique_ptr<GrpcCommand> grpc_command_{};
        // Shared, not unique: every AgentRuntime generation carries these two
        // sinks (build_runtime), so spans reach them through the snapshot they
        // already hold instead of keeping the whole agent alive. The last
        // release can therefore happen after ~AgentImpl — a late span, or a
        // thread's cached snapshot dying at thread exit — by which point the
        // workers are joined and both objects are passive data, safe to
        // destroy there.
        std::shared_ptr<UrlStats> url_stats_{};
        std::shared_ptr<AgentStats> agent_stats_{};

        std::thread init_thread_;
        std::thread ping_thread_;
        std::thread meta_thread_;
        std::thread span_thread_;
        std::thread stat_thread_;
        std::thread command_thread_;
        std::thread url_stat_add_thread_;
        std::thread url_stat_send_thread_;
        std::thread agent_stat_thread_;

        // Serializes reloadConfig() writers. Building a new AgentRuntime is a
        // load-build-store on runtime_, so two concurrent reloads could both
        // build from the same old runtime and lose one update. Readers do not
        // take this mutex; an unchanged generation-cache hit also avoids
        // AtomicSharedPtr's shared source.
        std::mutex reload_mutex_;

        int64_t start_time_{};
        std::atomic<uint64_t> trace_id_sequence_{};
        std::atomic<bool> enabled_{false};
        std::atomic<bool> shutting_down_{false};
        // Start() flips started_ and records owner_pid_. The supported model
        // starts the agent in the process that uses it (StartAgent() per
        // worker); owner_pid_ exists to keep MISUSE crash-free: an agent
        // inherited across fork() refuses Start()/Enable() with a one-time
        // error log, and teardown abandons the dead thread handles instead of
        // joining them. Atomic because Enable() reads it without a lock.
        std::atomic<bool> started_{false};
        std::atomic<pid_t> owner_pid_{0};
        // Set by init_grpc_workers' failure handlers: asynchronous
        // initialization failed after Start() already returned success, so
        // the agent is permanently offline while isExiting() stays false.
        // Consulted (alongside isExiting()) by StartAgent(), which would
        // otherwise keep returning the dead instance as the running agent.
        std::atomic<bool> init_failed_{false};
        // One-time fork-inheritance diagnostic (see warn_fork_inheritance()).
        mutable std::atomic<bool> fork_misuse_warned_{false};

        // StartAgent() inputs, kept for the config-file watcher's reloads.
        // Written once by setOptions() before Start(); read by the watcher
        // thread afterwards.
        AgentOptions options_;
        // Per-agent config-file watcher; null when no config file is watched.
        std::unique_ptr<ConfigFileWatcher> config_watcher_;

        // Serializes Start() against do_shutdown(). Without it a concurrent
        // Shutdown() races Start()'s writes to owner_pid_ and init_thread_, or
        // finishes teardown first — after which the threads Start() spawns are
        // never joined and dereference a destroyed agent. Start() checks
        // shutting_down_ under this lock and refuses to revive a torn-down agent.
        std::mutex lifecycle_mutex_;

        /// @brief Builds a new AgentRuntime for cfg. Every config-derived
        /// component is built fresh; reloads are rare (an actual config-file
        /// edit), so nothing is carried over from the previous runtime.
        std::shared_ptr<const AgentRuntime> build_runtime(std::shared_ptr<const Config> cfg);
        /// @brief Builds and atomically publishes the runtime for cfg.
        void apply_config(std::shared_ptr<const Config> cfg);
        /// @brief Populates rt's HTTP header recorders for server and client.
        static void build_header_recorders(AgentRuntime& rt, const Config& cfg);
        /// @brief Opens the gRPC channels, blocks until the boot-phase
        /// AgentInfo registration succeeds, then starts the background threads
        /// responsible for gRPC communication and enables the agent. Runs on
        /// init_thread_.
        void init_grpc_workers();
        /// @brief Logs, once per agent, that this handle was inherited across
        /// fork() and cannot be used in this process.
        void warn_fork_inheritance() const noexcept;
        /// @brief Sole admission check of the NewSpan funnel: true when the
        /// agent is enabled AND running in the process that started it. The pid
        /// check runs only on the enabled path, so an agent inherited across
        /// fork() hands out noop spans instead of recording into queues whose
        /// worker threads do not exist in this process.
        bool tracing_active() const noexcept;
        /// @brief Abandons (never joins or detaches — see abandon_thread())
        /// every worker thread handle. Used when tearing down an agent inherited
        /// across fork(), where the handles are joinable but reference threads
        /// that do not exist in this process.
        void abandon_grpc_workers() noexcept;
        /// @brief Sends every worker/watcher stop signal without blocking, so
        /// all of them wind down concurrently before the joins.
        void request_stop_workers() noexcept;
        /// @brief Blocking teardown tail: joins the config watcher, the
        /// AgentInfo scheduler, the init thread and every gRPC worker, then
        /// closes the channels. Unbounded — run it under
        /// teardown_workers_with_deadline().
        void teardown_workers() noexcept;
        /// @brief Runs teardown_workers() on a helper thread and waits for it
        /// up to the shutdown deadline (see agent_shutdown_deadline()). When
        /// the deadline expires the helper is detached and the object's
        /// lifetime is secured for the still-draining workers: with a
        /// shared_ptr self-reference while shared-owned (Shutdown() path), or
        /// by deferring destruction to the helper when the SharedDeleter is
        /// the caller (may_defer_destroy). Returns true when destruction was
        /// deferred — the caller must then not destroy the object.
        bool teardown_workers_with_deadline(bool may_defer_destroy) noexcept;
        /// @brief Joins the init thread and every gRPC worker thread.
        void wait_grpc_workers();
        /// @brief Performs the actual shutdown work (workers, watcher, logger)
        /// without touching the global_agent singleton. Safe to call from the
        /// destructor — does not lock global_agent_mutex, never throws.
        /// @param may_defer_destroy true only for the SharedDeleter, which owns
        ///        the object: a teardown outliving the deadline is then handed
        ///        the object for a deferred destroy instead of an unbounded join.
        /// @return true when destruction was deferred; the caller must not
        ///         destroy the object.
        bool do_shutdown(bool may_defer_destroy) noexcept;
    };

    // Test helpers for managing the global agent singleton
    void set_global_agent(std::shared_ptr<AgentImpl> agent);
    void reset_global_agent();

    /// @brief Overrides the hard wall-clock bound applied to the blocking
    ///        phase of agent shutdown; a non-positive value restores the
    ///        3-second default. Test helper, mirroring
    ///        set_config_watcher_poll_interval().
    void set_agent_shutdown_deadline(std::chrono::milliseconds deadline);

}  // namespace pinpoint
