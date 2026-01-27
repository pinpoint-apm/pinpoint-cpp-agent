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
#include <mutex>

#include <yaml-cpp/yaml.h>
#include "absl/strings/str_cat.h"

#include "logging.h"
#include "noop.h"
#include "agent.h"
#include "utility.h"

namespace pinpoint {

    // Constants
    constexpr int kCacheSize = 1024;

    // Global agent singleton with thread-safe access
    namespace {
        std::shared_ptr<AgentImpl> global_agent{nullptr};
        std::mutex global_agent_mutex;
    }

    AgentImpl::AgentImpl(const Config& cfg) :
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        trace_id_sequence_(1) {

        agent_stats_ = std::make_unique<AgentStats>(this);
        url_stats_ = std::make_unique<UrlStats>(this);

        api_cache_ = std::make_unique<IdCache>(kCacheSize);
        error_cache_ = std::make_unique<IdCache>(kCacheSize);
        sql_cache_ = std::make_unique<IdCache>(kCacheSize);
        sql_uid_cache_ = std::make_unique<SqlUidCache>(kCacheSize);
        
        reloadConfig(cfg);

        init_thread_ = std::thread{&AgentImpl::init_grpc_workers, this};
    }

    void AgentImpl::init_header_recorders() {
        // Server-side header recorders
        struct HeaderRecorderConfig {
            HeaderType type;
            int32_t annotation_key;
            const std::vector<std::string>& config_value;
        };

        const HeaderRecorderConfig server_configs[] = {
            {HTTP_REQUEST, ANNOTATION_HTTP_REQUEST_HEADER, config_.http.server.rec_request_header},
            {HTTP_RESPONSE, ANNOTATION_HTTP_RESPONSE_HEADER, config_.http.server.rec_response_header},
            {HTTP_COOKIE, ANNOTATION_HTTP_COOKIE, config_.http.server.rec_request_cookie}
        };

        const HeaderRecorderConfig client_configs[] = {
            {HTTP_REQUEST, ANNOTATION_HTTP_REQUEST_HEADER, config_.http.client.rec_request_header},
            {HTTP_RESPONSE, ANNOTATION_HTTP_RESPONSE_HEADER, config_.http.client.rec_response_header},
            {HTTP_COOKIE, ANNOTATION_HTTP_COOKIE, config_.http.client.rec_request_cookie}
        };

        std::shared_ptr<HttpHeaderRecorder> new_srv[3]{};
        std::shared_ptr<HttpHeaderRecorder> new_cli[3]{};

        for (const auto& cfg : server_configs) {
            if (!cfg.config_value.empty()) {
                new_srv[cfg.type] = std::make_shared<HttpHeaderRecorder>(cfg.annotation_key, cfg.config_value);
            }
        }

        for (const auto& cfg : client_configs) {
            if (!cfg.config_value.empty()) {
                new_cli[cfg.type] = std::make_shared<HttpHeaderRecorder>(cfg.annotation_key, cfg.config_value);
            }
        }

        for (size_t i = 0; i < 3; ++i) {
            std::atomic_store(&http_srv_header_recorder_[i], new_srv[i]);
            std::atomic_store(&http_cli_header_recorder_[i], new_cli[i]);
        }
    }

    void AgentImpl::reloadConfig(const Config& cfg) {
        config_ = cfg;

        // Rebuild sampler
        std::unique_ptr<Sampler> sampler;
        if (compare_string(config_.sampling.type, PERCENT_SAMPLING)) {
            sampler = std::make_unique<PercentSampler>(config_.sampling.percent_rate);
        } else {
            sampler = std::make_unique<CounterSampler>(config_.sampling.counter_rate);
        }

        std::shared_ptr<TraceSampler> new_sampler;
        if (config_.sampling.new_throughput > 0 || config_.sampling.cont_throughput > 0) {
            new_sampler = std::make_shared<ThroughputLimitTraceSampler>(this, std::move(sampler),
                                                                        config_.sampling.new_throughput,
                                                                        config_.sampling.cont_throughput);
        } else {
            new_sampler = std::make_shared<BasicTraceSampler>(this, std::move(sampler));
        }
        std::atomic_store(&sampler_, new_sampler);

        // Rebuild HTTP filters
        std::shared_ptr<HttpUrlFilter> new_url_filter;
        if (!config_.http.server.exclude_url.empty()) {
            new_url_filter = std::make_shared<HttpUrlFilter>(config_.http.server.exclude_url);
        }
        std::atomic_store(&http_url_filter_, new_url_filter);

        std::shared_ptr<HttpMethodFilter> new_method_filter;
        if (!config_.http.server.exclude_method.empty()) {
            new_method_filter = std::make_shared<HttpMethodFilter>(config_.http.server.exclude_method);
        }
        std::atomic_store(&http_method_filter_, new_method_filter);

        std::shared_ptr<HttpStatusErrors> new_status_errors;
        if (!config_.http.server.status_errors.empty()) {
            new_status_errors = std::make_shared<HttpStatusErrors>(config_.http.server.status_errors);
        }
        std::atomic_store(&http_status_errors_, new_status_errors);

        // Rebuild header recorders
        init_header_recorders();
    }

    void AgentImpl::init_grpc_workers() try {
        grpc_agent_ = std::make_unique<GrpcAgent>(this);
        grpc_span_ = std::make_unique<GrpcSpan>(this);
        grpc_stat_ = std::make_unique<GrpcStats>(this);

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
        bool finished = false;

        std::thread t1([this, &m, &cv, &finished] {
            // Join all worker threads
            if (init_thread_.joinable()) init_thread_.join();
            if (url_stat_add_thread_.joinable()) url_stat_add_thread_.join();
            if (url_stat_send_thread_.joinable()) url_stat_send_thread_.join();
            if (agent_stat_thread_.joinable()) agent_stat_thread_.join();
            if (ping_thread_.joinable()) ping_thread_.join();
            if (meta_thread_.joinable()) meta_thread_.join();
            if (span_thread_.joinable()) span_thread_.join();
            if (stat_thread_.joinable()) stat_thread_.join();

            {
                std::unique_lock<std::mutex> l(m);
                finished = true;
                cv.notify_one();
            }
        });

        {
            std::unique_lock<std::mutex> l(m);
            auto status = cv.wait_for(l, std::chrono::seconds(5), [&finished] { return finished; });
            
            if (!status) {
                LOG_WARN("wait grpc workers: timeout - some threads may still be running");
                t1.detach();  // Let it finish in background
            } else {
                t1.join();  // Clean join if completed in time
            }
        }
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
        const auto url_filter = std::atomic_load(&http_url_filter_);
        if (url_filter && url_filter->isFiltered(rpc_point)) {
            return noopSpan();
        }
        const auto method_filter = std::atomic_load(&http_method_filter_);
        if (!method.empty() && method_filter && method_filter->isFiltered(method)) {
            return noopSpan();
        }

        if (const auto parent_sampling = reader.Get(HEADER_SAMPLED); parent_sampling == "s0") {
            return std::make_shared<UnsampledSpan>(this);
        }

        auto sampler = std::atomic_load(&sampler_);
        if (!sampler) {
            return noopSpan();
        }

        bool my_sampling = false;
        if (const auto tid = reader.Get(HEADER_TRACE_ID); tid.has_value()) {
            my_sampling = sampler->isContinueSampled();
        } else {
            my_sampling = sampler->isNewSampled();
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
        
        {
            std::lock_guard<std::mutex> lock(global_agent_mutex);
            global_agent.reset();
        }
        
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

    void AgentImpl::recordUrlStat(std::unique_ptr<UrlStatEntry> stat) const {
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

        // Convert string_view to string to ensure null-termination for absl::StrCat
        const auto key = absl::StrCat(std::string(api_str), "_", api_type);
        const auto [id, found] = api_cache_->get(key);
        if (found) {
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

        const auto key = std::string(error_name);
        const auto [id, found] = error_cache_->get(key);
        if (found) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(META_STRING, id, error_name, STRING_META_ERROR);
        grpc_agent_->enqueueMeta(std::move(meta));

        return id;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache error meta: exception = {}", e.what());
        return 0;
    }

    void AgentImpl::removeCacheError(const StringMeta& error_meta) const {
        if (enabled_) {
            error_cache_->remove(error_meta.str_val_);
        }
    }

    int32_t AgentImpl::cacheSql(std::string_view sql_query) const try {
        if (!enabled_) {
            return 0;
        }

        const auto key = std::string(sql_query);
        const auto [id, found] = sql_cache_->get(key);
        if (found) {
            return id;
        }

        auto meta = std::make_unique<MetaData>(META_STRING, id, sql_query, STRING_META_SQL);
        grpc_agent_->enqueueMeta(std::move(meta));

        return id;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache sql meta: exception = {}", e.what());
        return 0;
    }

    void AgentImpl::removeCacheSql(const StringMeta& sql_meta) const {
        if (enabled_) {
            sql_cache_->remove(sql_meta.str_val_);
        }
    }

    std::vector<unsigned char> AgentImpl::cacheSqlUid(std::string_view sql) const try {
        if (!enabled_) {
            return {};
        }
        
        const auto key = std::string(sql);
        const auto [uid, found] = sql_uid_cache_->get(key);
        if (found) {
            return uid;
        }
        
        auto meta = std::make_unique<MetaData>(META_SQL_UID, uid, sql);
        grpc_agent_->enqueueMeta(std::move(meta));
        
        return uid;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to cache sql uid meta: exception = {}", e.what());
        return {};
    }

    void AgentImpl::removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const {
        if (enabled_) {
            sql_uid_cache_->remove(sql_uid_meta.sql_);
        }
    }

    void AgentImpl::recordException(SpanData* span_data) const {
        if (!enabled_ || !config_.enable_callstack_trace) {
            return;
        }

        auto meta = std::make_unique<MetaData>(META_EXCEPTION, span_data->getTraceId(), 
                                               span_data->getSpanId(), span_data->getUrlTemplate(),
                                               span_data->takeExceptions());
        grpc_agent_->enqueueMeta(std::move(meta));
    }

    bool AgentImpl::isStatusFail(const int status) const {
        const auto status_errors = std::atomic_load(&http_status_errors_);
        if (enabled_ && status_errors) {
            return status_errors->isErrorCode(status);
        }
        return false;
    }

    void AgentImpl::recordServerHeader(const HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const {
        if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto recorder = std::atomic_load(&http_srv_header_recorder_[which]);
        if (enabled_ && recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    void AgentImpl::recordClientHeader(const HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const {
        if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto recorder = std::atomic_load(&http_cli_header_recorder_[which]);
        if (enabled_ && recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    static std::shared_ptr<AgentImpl> make_agent(Config& cfg) {
        if (!cfg.enable) {
            return nullptr;
        }
        try {
            return std::make_shared<AgentImpl>(cfg);
        } catch (const std::exception& e) {
            LOG_ERROR("make agent exception = {}", e.what());
            return nullptr;
        }
    }

    void SetConfigFilePath(std::string_view config_file_path) {
        read_config_from_file(config_file_path.data());
    }

    void SetConfigString(std::string_view config_string) {
        set_config_string(config_string);
    }

    static AgentPtr create_agent_helper(Config& cfg) {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        
        if (!cfg.check()) {
            return noopAgent();
        }
        
        if (global_agent != nullptr) {
            if (cfg.isReloadable(global_agent->getConfig())) {
                global_agent->reloadConfig(cfg);
                return global_agent;
            } else {
                return noopAgent();
            }
        }

        global_agent = make_agent(cfg);
        if (global_agent == nullptr) {
            return noopAgent();
        }
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
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        
        if (global_agent == nullptr) {
            return noopAgent();
        }
        return global_agent;
    }

}  // namespace pinpoint
