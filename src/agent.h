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

#include "pinpoint/tracer.h"
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
    class AgentImpl final : public Agent, public AgentService {
    public:
		/**
		 * @brief Constructs an agent using the provided configuration.
		 *
		 * @param options Resolved agent configuration.
		 */
		explicit AgentImpl(const Config& options);
        ~AgentImpl() override;

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
		void Shutdown() override;

    	bool isExiting() const override { return shutting_down_; }
    	std::string_view getAppName() const override { return config_.app_name_; }
    	int32_t getAppType() const override { return config_.app_type_; }
    	std::string_view getAgentId() const override { return config_.agent_id_; }
    	std::string_view getAgentName() const override {	return config_.agent_name_; }

    	const Config& getConfig() const override { return config_; }
    	int64_t getStartTime() const override { return start_time_; }

    	TraceId generateTraceId() override;
    	void recordSpan(std::unique_ptr<SpanChunk> span) const override;
    	void recordUrlStat(std::unique_ptr<UrlStat> stat) const override;
    	void recordException(SpanData* span_data) const override;
    	void recordStats(StatsType stats) const override;

    	int32_t cacheApi(std::string_view api_str, int32_t api_type) const override;
    	void removeCacheApi(const ApiMeta& api_meta) const override;
    	int32_t cacheError(std::string_view error_name) const override;
    	void removeCacheError(const StringMeta& error_meta) const override;
    	int32_t cacheSql(std::string_view sql_query) const override;
    	void removeCacheSql(const StringMeta& sql_meta) const override;
    	std::vector<unsigned char> cacheSqlUid(std::string_view sql) const override;
    	void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override;

    	bool isStatusFail(int status) const override;
    	void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override;
    	void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override;

    private:

        Config config_;
        std::unique_ptr<TraceSampler> sampler_{};
    	std::unique_ptr<IdCache> api_cache_{};
    	std::unique_ptr<IdCache> error_cache_{};
    	std::unique_ptr<IdCache> sql_cache_{};
    	std::unique_ptr<SqlUidCache> sql_uid_cache_{};

    	std::unique_ptr<GrpcAgent> grpc_agent_{};
    	std::unique_ptr<GrpcSpan> grpc_span_{};
    	std::unique_ptr<GrpcStats> grpc_stat_{};
    	std::unique_ptr<UrlStats> url_stats_{};
    	std::unique_ptr<AgentStats> agent_stats_{};

    	std::thread init_thread_;
    	std::thread ping_thread_;
    	std::thread meta_thread_;
    	std::thread span_thread_;
    	std::thread stat_thread_;
    	std::thread url_stat_add_thread_;
    	std::thread url_stat_send_thread_;
    	std::thread agent_stat_thread_;

    	std::unique_ptr<HttpUrlFilter> http_url_filter_{};
    	std::unique_ptr<HttpMethodFilter> http_method_filter_{};
    	std::unique_ptr<HttpStatusErrors> http_status_errors_{};
    	std::unique_ptr<HttpHeaderRecorder> http_srv_header_recorder_[3]{};
    	std::unique_ptr<HttpHeaderRecorder> http_cli_header_recorder_[3]{};

        int64_t start_time_{};
    	std::atomic<uint64_t> trace_id_sequence_{};
    	bool enabled_{false};
    	bool shutting_down_{false};

    	/// @brief Starts background threads responsible for gRPC communication.
    	void init_grpc_workers();
    	/// @brief Signals all gRPC workers to stop and joins their threads.
    	void close_grpc_workers();
    	/// @brief Waits for all gRPC workers to finish execution.
    	void wait_grpc_workers();
    };

}  // namespace pinpoint

