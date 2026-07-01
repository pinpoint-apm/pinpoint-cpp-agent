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

#include "pinpoint/tracer.h"
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
				  std::unique_ptr<GrpcCommand> grpc_command = nullptr);
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
        void recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                             std::vector<std::unique_ptr<Exception>>&& exceptions) const override;
    	void recordStats(StatsType stats) const override;

    	int32_t cacheApi(std::string_view api_str, int32_t api_type) const override;
    	void removeCacheApi(const ApiMeta& api_meta) const override;
    	int32_t cacheError(std::string_view error_name) const override;
    	void removeCacheError(const StringMeta& error_meta) const override;
    	int32_t cacheSql(std::string_view sql_query) const override;
    	void removeCacheSql(const StringMeta& sql_meta) const override;
    	std::optional<SqlUid> cacheSqlUid(std::string_view sql) const override;
    	void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override;

    	bool isStatusFail(int status) const override;
    	void recordServerHeader(HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const override;
    	void recordClientHeader(HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const override;

    	AgentStats& getAgentStats() override { return *agent_stats_; }
    	UrlStats& getUrlStats() override { return *url_stats_; }

    private:

        AtomicSharedPtr<const Config> config_;
        AtomicSharedPtr<TraceSampler> sampler_;

    	// Identity fields snapshotted once at construction. Config::isReloadable()
    	// guarantees these never change while this agent lives, so they are served
    	// directly — without an atomic config_ load (shared_mutex lock + shared_ptr
    	// copy) or a string copy — on the per-request hot path (e.g. InjectContext,
    	// SpanData ctor, generateTraceId).
    	std::string app_name_;
    	int32_t app_type_{};
    	std::string agent_id_;
    	std::string agent_name_;
    	std::string service_name_;

		std::unique_ptr<ApiIdCache> api_cache_{};
    	std::unique_ptr<IdCache> error_cache_{};
    	std::unique_ptr<IdCache> sql_cache_{};
    	std::unique_ptr<SqlUidCache> sql_uid_cache_{};

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

    	AtomicSharedPtr<HttpUrlFilter> http_url_filter_;
    	AtomicSharedPtr<HttpMethodFilter> http_method_filter_;
    	AtomicSharedPtr<HttpStatusErrors> http_status_errors_;
    	AtomicSharedPtr<HttpHeaderRecorder> http_srv_header_recorder_[3];
    	AtomicSharedPtr<HttpHeaderRecorder> http_cli_header_recorder_[3];

    	// Serializes reloadConfig() writers. reloadConfig performs a sequence of
    	// independent AtomicSharedPtr stores (config_, sampler_, filters, and the
    	// six header-recorder slots); each store is individually atomic, but the
    	// sequence is not. Two concurrent reloads — e.g. a CreateAgent()-driven
    	// reload (holding global_agent_mutex) racing the config-file watcher
    	// thread (which does NOT hold it) — could otherwise interleave their
    	// stores and leave a mixed configuration snapshot (config_ from one
    	// version, sampler_/recorders from another). Readers on the hot path
    	// stay lock-free via AtomicSharedPtr::load(); only writers take this.
    	std::mutex reload_mutex_;

        int64_t start_time_{};
    	std::atomic<uint64_t> trace_id_sequence_{};
    	std::atomic<bool> enabled_{false};
    	std::atomic<bool> shutting_down_{false};

    	/// @brief (Re)builds config-derived components (sampler, HTTP filters,
    	/// header recorders), skipping any whose backing configuration is
    	/// unchanged from old_cfg. A null old_cfg forces a full build and is used
    	/// for the initial construction path.
    	void apply_config(const std::shared_ptr<const Config>& old_cfg,
    	                  const std::shared_ptr<const Config>& cfg);
    	/// @brief Initializes HTTP header recorders for server and client.
    	void init_header_recorders(const std::shared_ptr<const Config>& cfg);
    	/// @brief Starts background threads responsible for gRPC communication.
    	void init_grpc_workers();
    	/// @brief Signals all gRPC workers to stop and joins their threads.
    	void close_grpc_workers();
    	/// @brief Waits for all gRPC workers to finish execution.
    	void wait_grpc_workers();
    	/// @brief Performs the actual shutdown work (workers, watcher, logger)
    	/// without touching the global_agent singleton. Safe to call from the
    	/// destructor — does not lock global_agent_mutex, never throws.
    	void do_shutdown() noexcept;
    };

    // Returns true once a real (non-noop) agent has been installed as the
    // global singleton. make_config() uses this to decide whether it is
    // building the initial config or rebuilding it for an already-running
    // agent (a reload), in which case environment variables are not re-read.
    bool global_agent_exists();

    // Test helpers for managing the global agent singleton
    void set_global_agent(std::shared_ptr<AgentImpl> agent);
    void reset_global_agent();

}  // namespace pinpoint
