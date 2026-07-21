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
#include "agent_service.h"

namespace pinpoint {

    /**
     * @brief Concrete agent implementation that wires together configuration, samplers and transports.
     *
     * `AgentImpl` orchestrates span creation, metadata caching, gRPC workers and statistics collection.
     * It implements both `Agent` (SDK surface) and `AgentService` (internal service boundary).
     */
    class AgentImpl final : public Agent, public AgentService,
                            public std::enable_shared_from_this<AgentImpl> {
    public:
		/**
		 * @brief Constructs an agent using the provided configuration.
		 *
		 * @param options Resolved agent configuration.
		 */
		AgentImpl(std::shared_ptr<const Config> options,
				  std::unique_ptr<GrpcAgent> grpc_agent,
				  std::unique_ptr<GrpcMetadata> grpc_metadata,
				  std::unique_ptr<GrpcSpan> grpc_span,
				  std::unique_ptr<GrpcStats> grpc_stat,
				  std::unique_ptr<GrpcCommand> grpc_command = nullptr,
				  int32_t app_type = DEFAULT_APP_TYPE);
        ~AgentImpl() noexcept override;

		/**
		 * @brief Creates a new span for an outbound operation.
		 *
		 * @param operation Logical operation name.
		 * @param rpc_point RPC endpoint or service name.
		 */
		SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) override;
		/**
		 * @brief Creates a span and extracts the context from an incoming request.
		 *
		 * @param operation Logical operation name.
		 * @param rpc_point RPC endpoint or service name.
		 * @param reader Trace context reader provided by user code.
		 */
		SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, TraceContextReader& reader) override;
    	/**
    	 * @brief Creates a span for HTTP requests while recording request method.
    	 *
    	 * @param operation Operation name.
    	 * @param rpc_point RPC endpoint.
    	 * @param method HTTP method name.
    	 * @param reader Trace context reader provided by user code.
    	 */
    	SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method, TraceContextReader& reader) override;
		/// @brief Brings the agent online in the current process: performs this
		/// process's first grpc_init by opening channels, spawns workers, starts
		/// the config watcher and regenerates a process-unique agent id for
		/// forked workers. Idempotent (per process) and non-blocking. See
		/// Agent::Start().
		void Start() noexcept override;
		/// @brief Returns whether the agent is enabled for tracing.
		bool Enable() override;
		/// @brief Initiates a graceful shutdown of the agent.
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
    	const std::string& getAgentName() const override;
    	const std::string& getServiceName() const override;

    	std::shared_ptr<const Config> getConfig() const override;
    	int64_t getStartTime() const override { return start_time_; }
		/// @brief Reloads configuration-dependent helpers (samplers, filters, recorders).
    	void reloadConfig(std::shared_ptr<const Config> cfg) override;
    	void onAgentInfoSent() override;

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
    	void recordServerHeader(HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const override;
    	void recordClientHeader(HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const override;

    	AgentStats& getAgentStats() override { return *agent_stats_; }
    	UrlStats& getUrlStats() override { return *url_stats_; }

    private:

        // Single source of truth for the config and its derived components
        // (see AgentRuntime). One load() per request yields a mutually
        // consistent snapshot of all of them.
        AtomicSharedPtr<const AgentRuntime> runtime_;

    	// Identity fields snapshotted once at construction. Config::isReloadable()
    	// guarantees these never change while this agent lives, so they are served
    	// directly — without an atomic runtime_ load (shared_mutex lock + shared_ptr
    	// copy) or a string copy — on the per-request hot path (e.g. InjectContext,
    	// SpanData ctor, generateTraceId).
    	std::string app_name_;
    	int32_t app_type_{};
    	// Held as a shared string so generateTraceId() can seed every new
    	// TraceId with a refcount bump instead of a per-trace allocation.
    	// Written only by the ctor and refresh_agent_id_for_process() (early in
    	// Start(), before the init thread that can flip enabled_ exists), so it
    	// is immutable by the time NewSpan can mint a sampled span. Never null.
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
		// needs this one flag, so a relaxed load here replaces a full
		// runtime_.load() (shared-lock + two shared_ptr refcount bumps) on
		// that hot path. Benignly stale for the instant around a reload.
		std::atomic<bool> raw_sql_cache_enabled_{true};

    	std::unique_ptr<GrpcAgent> grpc_agent_{};
    	std::unique_ptr<GrpcMetadata> grpc_metadata_{};
    	std::unique_ptr<GrpcSpan> grpc_span_{};
    	std::unique_ptr<GrpcStats> grpc_stat_{};
		std::unique_ptr<GrpcCommand> grpc_command_{};
    	std::unique_ptr<UrlStats> url_stats_{};
    	std::unique_ptr<AgentStats> agent_stats_{};

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
    	// load-build-store read-modify-write of runtime_: two concurrent
    	// reloads — e.g. a CreateAgent()-driven reload (holding
    	// global_agent_mutex) racing the config-file watcher thread (which does
    	// NOT hold it) — could otherwise both build from the same old runtime
    	// and lose one of the updates. Readers on the hot path stay lock-free
    	// via a single AtomicSharedPtr::load(); only writers take this.
    	std::mutex reload_mutex_;

        int64_t start_time_{};
    	std::atomic<uint64_t> trace_id_sequence_{};
    	std::atomic<bool> enabled_{false};
    	std::atomic<bool> shutting_down_{false};
    	// Fork-safe lifecycle. Start() flips started_ and records owner_pid_ (the
    	// pid that actually brought the agent online); teardown abandons the
    	// handles instead of joining when it runs in a different process (a
    	// forked child inheriting dead thread handles). create_pid_ is the pid that constructed the agent
    	// (CreateAgent time); Start() compares against it to detect that it is
    	// running in a forked child and must give this worker a unique agent id.
    	std::atomic<bool> started_{false};
    	pid_t create_pid_{};
    	pid_t owner_pid_{};

    	// Serializes Start() against do_shutdown(). Without it a concurrent
    	// Shutdown() races Start()'s writes to owner_pid_ and init_thread_
    	// (both plain members), or completes teardown first — after which the
    	// init thread and workers Start() spawns would never be joined and
    	// would dereference a destroyed agent. Start() checks shutting_down_
    	// under this lock and refuses to bring a torn-down agent back up.
    	std::mutex lifecycle_mutex_;

    	/// @brief Builds a new AgentRuntime for cfg, rebuilding only the
    	/// components whose backing configuration changed relative to old_rt;
    	/// unchanged components are shared with the previous runtime so their
    	/// warmed-up state (e.g. throughput-sampler counters) survives. A null
    	/// old_rt forces a full build and is used for the initial construction.
    	std::shared_ptr<const AgentRuntime> build_runtime(
    	        const std::shared_ptr<const AgentRuntime>& old_rt,
    	        std::shared_ptr<const Config> cfg);
    	/// @brief Builds and atomically publishes the runtime for cfg.
    	void apply_config(const std::shared_ptr<const AgentRuntime>& old_rt,
    	                  std::shared_ptr<const Config> cfg);
    	/// @brief Populates rt's HTTP header recorders for server and client.
    	static void build_header_recorders(AgentRuntime& rt, const Config& cfg);
    	/// @brief Opens the gRPC channels and starts background threads
    	/// responsible for gRPC communication. Runs on init_thread_.
    	void init_grpc_workers();
    	/// @brief Regenerates a process-unique agent id when Start() runs in a
    	/// forked child (create_pid_ != current pid). A pinned id gets a pid
    	/// suffix; an auto-generated id is replaced with a fresh one. A no-op in
    	/// the process that constructed the agent, so non-fork behavior is
    	/// unchanged.
    	void refresh_agent_id_for_process();
    	/// @brief Abandons (never joins or detaches — see abandon_thread())
    	/// every worker thread handle. Used when tearing down an agent inherited
    	/// across fork(), where the handles are joinable but reference threads
    	/// that do not exist in this process.
    	void abandon_grpc_workers() noexcept;
    	/// @brief Signals all gRPC workers to stop and joins their threads.
    	void close_grpc_workers();
    	/// @brief Waits for all gRPC workers to finish execution.
    	void wait_grpc_workers();
    	/// @brief Performs the actual shutdown work (workers, watcher, logger)
    	/// without touching the global_agent singleton. Safe to call from the
    	/// destructor — does not lock global_agent_mutex, never throws.
    	void do_shutdown() noexcept;
    };

    // Test helpers for managing the global agent singleton
    void set_global_agent(std::shared_ptr<AgentImpl> agent);
    void reset_global_agent();

}  // namespace pinpoint
