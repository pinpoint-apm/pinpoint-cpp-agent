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

#include <string>
#include <exception>
#include <utility>

#include <yaml-cpp/yaml.h>
#include "absl/strings/str_cat.h"

#include "logging.h"
#include "noop.h"
#include "agent.h"

namespace pinpoint {

    constexpr int kCacheSize = 1024;
    constexpr int kMaxAppNameLength = 24;
    constexpr int kMaxAgentIdLength = 24;
    constexpr int kMaxAgentNameLength = 255;

    static AgentPtr global_agent{nullptr};

    AgentImpl::AgentImpl(const Config& options) :
        config_(options),
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        trace_id_sequence_(1) {

        std::unique_ptr<Sampler> sampler;
        if (compare_string(config_.sampling.type, PERCENT_SAMPLING)) {
            sampler = std::make_unique<PercentSampler>(config_.sampling.percent_rate);
        } else {
            sampler = std::make_unique<CounterSampler>(config_.sampling.counter_rate);
        }

        if (config_.sampling.new_throughput > 0 || config_.sampling.cont_throughput > 0) {
            sampler_ = std::make_unique<ThroughputLimitTraceSampler>(std::move(sampler),
                                                                     config_.sampling.new_throughput,
                                                                     config_.sampling.cont_throughput);
        } else {
            sampler_ = std::make_unique<BasicTraceSampler>(std::move(sampler));
        }

        api_cache_ = std::make_unique<IdCache>(kCacheSize);
        error_cache_ = std::make_unique<IdCache>(kCacheSize);

        if (!config_.http.server.exclude_url.empty()) {
            http_url_filter_ = std::make_unique<HttpUrlFilter>(config_.http.server.exclude_url);
        }
        if (!config_.http.server.exclude_method.empty()) {
            http_method_filter_ = std::make_unique<HttpMethodFilter>(config_.http.server.exclude_method);
        }
        if (!config_.http.server.status_errors.empty()) {
            http_status_errors_ = std::make_unique<HttpStatusErrors>(config_.http.server.status_errors);
        }

        http_srv_header_recorder_[HTTP_REQUEST] = nullptr;
        http_srv_header_recorder_[HTTP_RESPONSE] = nullptr;
        http_srv_header_recorder_[HTTP_COOKIE] = nullptr;

        if (!config_.http.server.rec_request_header.empty()) {
            http_srv_header_recorder_[HTTP_REQUEST] =
                std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_REQUEST_HEADER, config_.http.server.rec_request_header);
        }
        if (!config_.http.server.rec_response_header.empty()) {
            http_srv_header_recorder_[HTTP_RESPONSE] =
                std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_RESPONSE_HEADER, config_.http.server.rec_response_header);
        }
        if (!config_.http.server.rec_request_cookie.empty()) {
            http_srv_header_recorder_[HTTP_COOKIE] =
                std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_COOKIE, config_.http.server.rec_request_cookie);
        }


        http_cli_header_recorder_[HTTP_REQUEST] = nullptr;
        http_cli_header_recorder_[HTTP_RESPONSE] = nullptr;
        http_cli_header_recorder_[HTTP_COOKIE] = nullptr;

        if (!config_.http.client.rec_request_header.empty()) {
            http_cli_header_recorder_[HTTP_REQUEST] =
                std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_REQUEST_HEADER, config_.http.client.rec_request_header);
        }
        if (!config_.http.client.rec_response_header.empty()) {
            http_cli_header_recorder_[HTTP_RESPONSE] =
                std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_RESPONSE_HEADER, config_.http.client.rec_response_header);
        }
        if (!config_.http.client.rec_request_cookie.empty()) {
            http_cli_header_recorder_[HTTP_COOKIE] =
                std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_COOKIE, config_.http.client.rec_request_cookie);
        }

        init_url_stat();
        init_thread_ = std::thread{&AgentImpl::init_grpc_workers, this};
    }

    void AgentImpl::init_grpc_workers() try {
        grpc_agent_ = std::make_unique<GrpcAgent>(this);
        grpc_span_ = std::make_unique<GrpcSpan>(this);
        grpc_stat_ = std::make_unique<GrpcStats>(this);
        url_stats_ = std::make_unique<UrlStats>(this);
        agent_stats_ = std::make_unique<AgentStats>(this);

        do {
            if (!grpc_agent_->readyChannel()) {
                return;
            }
        } while (grpc_agent_->registerAgent() != SEND_OK);

        ping_thread_ = std::thread{&GrpcAgent::sendPingWorker, grpc_agent_.get()};
        meta_thread_ = std::thread{&GrpcAgent::sendMetaWorker, grpc_agent_.get()};
        span_thread_ = std::thread{&GrpcSpan::sendSpanWorker, grpc_span_.get()};
        stat_thread_ = std::thread{&GrpcStats::sendStatsWorker, grpc_stat_.get()};
        url_stat_add_thread_ = std::thread{&UrlStats::addUrlStatsWorker, url_stats_.get()};
        url_stat_send_thread_ = std::thread{&UrlStats::sendUrlStatsWorker, url_stats_.get()};
        agent_stat_thread_ = std::thread{&AgentStats::agentStatsWorker, agent_stats_.get()};

        enabled_ = true;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to init grpc workers: exception = {}", e.what());
        enabled_ = false;
        return;
    }

    void AgentImpl::close_grpc_workers() {
        url_stats_->stopAddUrlStatsWorker();
        url_stats_->stopSendUrlStatsWorker();
        agent_stats_->stopAgentStatsWorker();
        grpc_agent_->stopPingWorker();
        grpc_agent_->stopMetaWorker();
        grpc_span_->stopSpanWorker();
        grpc_stat_->stopStatsWorker();

        wait_grpc_workers();

        grpc_agent_->closeChannel();
        grpc_stat_->closeChannel();
        grpc_span_->closeChannel();

        LOG_INFO("close grpc workers done");
    }

    void AgentImpl::wait_grpc_workers() {
        std::mutex m;
        std::condition_variable cv;

        std::thread t1([this, &m, &cv] {
            init_thread_.join();
            url_stat_add_thread_.join();
            url_stat_send_thread_.join();
            agent_stat_thread_.join();
            ping_thread_.join();
            meta_thread_.join();
            span_thread_.join();
            stat_thread_.join();

            std::unique_lock<std::mutex> l(m);
            cv.notify_one();
        });

        std::unique_lock<std::mutex> l(m);
        auto s = cv.wait_for(l, std::chrono::seconds(5));
        if (s == std::cv_status::timeout) {
            LOG_INFO("wait grpc workers: timeout");
        }
        t1.detach();
    }

    AgentImpl::~AgentImpl() {
        if (enabled_) {
            Shutdown();
        }
    }

	SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point) {
        SpanPtr span;

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
        if (!enabled_) {
            return noopSpan();
        }
        if (http_url_filter_ && http_url_filter_->isFiltered(rpc_point)) {
            return noopSpan();
        }
        if (!method.empty() && http_method_filter_ && http_method_filter_->isFiltered(method)) {
            return noopSpan();
        }

        if (const auto parent_sampling = reader.Get(HEADER_SAMPLED); parent_sampling == "s0") {
            return std::make_shared<UnsampledSpan>(this);
        }

        bool my_sampling = false;
        if (const auto tid = reader.Get(HEADER_TRACE_ID); tid.has_value()) {
            my_sampling = sampler_->isContinueSampled();
        } else {
            my_sampling = sampler_->isNewSampled();
        }

        SpanPtr span;
        if (my_sampling) {
            span = std::make_shared<SpanImpl>(this, operation, rpc_point);
        } else {
            span = std::make_shared<UnsampledSpan>(this);
        }
        span->ExtractContext(reader);
        return span;
    } catch (const std::exception& e) {
        LOG_ERROR("new span exception = {}", e.what());
        return noopSpan();
    }

	bool AgentImpl::Enable() {
    	return enabled_;
	}

    void AgentImpl::Shutdown() {
        if (shutting_down_) {
            return;
        }

        LOG_INFO("agent shutdown");
        enabled_ = false;
        shutting_down_ = true;
        global_agent = noopAgent();
        close_grpc_workers();
        shutdown_logger();
    }

    TraceId AgentImpl::generateTraceId() {
        TraceId tid;

        tid.AgentId = config_.agent_id_;
        tid.StartTime = start_time_;
        tid.Sequence = trace_id_sequence_.fetch_add(1);
        return tid;
    }

    void AgentImpl::recordSpan(std::unique_ptr<SpanChunk> span) const {
        if (enabled_) {
            grpc_span_->enqueueSpan(std::move(span));
        }
    }

    void AgentImpl::recordUrlStat(std::unique_ptr<UrlStat> stat) const {
        if (enabled_) {
            url_stats_->enqueueUrlStats(std::move(stat));
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

        const auto key = absl::StrCat(api_str.data(), "_", api_type);
        const auto [id, old] = api_cache_->get(key);
        if (old) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(META_API, id, api_type, api_str);
        grpc_agent_->enqueueMeta(std::move(meta));

        return id;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache api meta: exception = {}", e.what());
        return 0;
    }

    void AgentImpl::removeCacheApi(const ApiMeta& api_meta) const {
        if (enabled_) {
            const auto key = absl::StrCat(api_meta.api_str_, "_", api_meta.type_);
            api_cache_->remove(key);
        }
    }

    int32_t AgentImpl::cacheError(std::string_view error_name) const try {
        if (!enabled_) {
            return 0;
        }

        const auto key = std::string(error_name.data());
        const auto [id, old] = error_cache_->get(key);
        if (old) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(META_STRING, id, error_name);
        grpc_agent_->enqueueMeta(std::move(meta));

        return id;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache error meta: exception = {}", e.what());
        return 0;
    }

    void AgentImpl::removeCacheError(const StringMeta& str_meta) const {
        if (enabled_) {
            error_cache_->remove(str_meta.str_val_);
        }
    }

    bool AgentImpl::isStatusFail(const int status) const {
        if (enabled_ && http_status_errors_) {
            return http_status_errors_->isErrorCode(status);
        }
        return false;
    }

    void AgentImpl::recordServerHeader(const HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const {
        if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        if (enabled_ && http_srv_header_recorder_[which]) {
            http_srv_header_recorder_[which]->recordHeader(reader, annotation);
        }
    }

    void AgentImpl::recordClientHeader(const HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const {
        if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        if (enabled_ && http_cli_header_recorder_[which]) {
            http_cli_header_recorder_[which]->recordHeader(reader, annotation);
        }
    }

    static AgentPtr make_agent(Config& cfg) {
        if (!cfg.enable) {
            return noopAgent();
        }

        if (cfg.collector.host.empty()) {
            LOG_ERROR("address of collector is required");
            return noopAgent();
        }
        if (cfg.app_name_.empty()) {
            LOG_ERROR("application name is required");
            return noopAgent();
        }
        if (cfg.app_name_.size() > kMaxAppNameLength) {
            LOG_ERROR("application name is too long - max length: {}", kMaxAppNameLength);
            return noopAgent();
        }
        if (cfg.agent_id_.size() > kMaxAgentIdLength) {
            LOG_ERROR("agent id is too long - max length: {}", kMaxAgentIdLength);
            return noopAgent();
        }
        if (cfg.agent_name_.size() > kMaxAgentNameLength) {
            LOG_ERROR("agent name is too long - max length: {}", kMaxAgentNameLength);
            return noopAgent();
        }

        try {
            return std::make_shared<AgentImpl>(cfg);
        } catch (const std::exception& e) {
            LOG_ERROR("make agent exception = {}", e.what());
            return noopAgent();
        }
    }

    void SetConfigFilePath(std::string_view config_file_path) {
        read_config_from_file(config_file_path.data());
    }

    void SetConfigString(std::string_view config_string) {
        set_config_string(config_string);
    }

    static AgentPtr create_agent_helper(Config& cfg) {
        if (global_agent != nullptr) {
            LOG_WARN("agent: pinpoint agent is already created");
            return global_agent;
        }

        global_agent = make_agent(cfg);
        return global_agent;
    }

    AgentPtr CreateAgent() {
        Config cfg = make_config();
        return create_agent_helper(cfg);
    }

    AgentPtr CreateAgent(int32_t app_type) {
        Config cfg = make_config();
        cfg.app_type_ = app_type;
        return create_agent_helper(cfg);
    }

    AgentPtr GlobalAgent() {
        if (global_agent == nullptr) {
            return noopAgent();
        }
        return global_agent;
    }

}  // namespace pinpoint
