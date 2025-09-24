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

namespace pinpoint {

	class GrpcAgent;
	class GrpcSpan;
	class GrpcStats;
	class SpanData;
	struct ApiMeta;
	struct StringMeta;

    class AgentImpl final : public Agent {
    public:
		explicit AgentImpl(const Config& options);
        ~AgentImpl() override;

		SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point) override;
		SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, TraceContextReader& reader) override;
    	SpanPtr NewSpan(std::string_view operation, std::string_view rpc_point, std::string_view method, TraceContextReader& reader) override;
		bool Enable() override;
		void Shutdown() override;
    	bool isExiting() const { return shutting_down_; }

    	std::string_view getAppName() const { return config_.app_name_; }
    	int32_t getAppType() const { return config_.app_type_; }
    	std::string_view getAgentId() const { return config_.agent_id_; }
    	std::string_view getAgentName() const {	return config_.agent_name_; }
    	const Config& getConfig() const { return config_; }
    	int64_t getStartTime() const { return start_time_; }

    	TraceId generateTraceId();
    	void recordSpan(std::unique_ptr<SpanChunk> span) const;
    	void recordUrlStat(std::unique_ptr<UrlStat> stat) const;
    	void recordStats(StatsType stats) const;

    	int32_t cacheApi(std::string_view api_str, int32_t api_type) const;
    	void removeCacheApi(const ApiMeta& api_meta) const;
    	int32_t cacheError(std::string_view error_name) const;
    	void removeCacheError(const StringMeta& str_meta) const;

    	bool isStatusFail(int status) const;
    	void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const;
    	void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const;

    private:

        Config config_;
        std::unique_ptr<TraceSampler> sampler_{};
    	std::unique_ptr<IdCache> api_cache_{};
    	std::unique_ptr<IdCache> error_cache_{};

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

    	void init_grpc_workers();
    	void close_grpc_workers();
    	void wait_grpc_workers();
    };

}  // namespace pinpoint

