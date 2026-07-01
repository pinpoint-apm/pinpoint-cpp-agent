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
#include <iterator>
#include <utility>
#include <mutex>
#include <optional>
#include <vector>

#include "logging.h"
#include "noop.h"
#include "agent.h"
#include "utility.h"

namespace pinpoint {

    // Constants
    constexpr int kCacheSize = 1024;

    // Global agent singleton with thread-safe access
    namespace {
        std::mutex global_agent_mutex;

        // The holder is intentionally heap-allocated and never destroyed so
        // that ~AgentImpl can never run from a static destructor. Tearing the
        // agent down during __cxa_atexit (thread joins, gRPC channel teardown,
        // logging through possibly-destroyed singletons) is unsafe for a
        // library embedded in a host application; teardown must only happen
        // through an explicit Shutdown() or a user-released reference.
        std::shared_ptr<AgentImpl>& global_agent() {
            static auto* holder = new std::shared_ptr<AgentImpl>();
            return *holder;
        }
    }

    AgentImpl::AgentImpl(std::shared_ptr<const Config> cfg,
                         std::unique_ptr<GrpcAgent> grpc_agent,
                         std::unique_ptr<GrpcMetadata> grpc_metadata,
                         std::unique_ptr<GrpcSpan> grpc_span,
                         std::unique_ptr<GrpcStats> grpc_stat,
                         std::unique_ptr<GrpcCommand> grpc_command) :
        config_(std::shared_ptr<const Config>(std::move(cfg))),
        grpc_agent_(std::move(grpc_agent)),
        grpc_metadata_(std::move(grpc_metadata)),
        grpc_span_(std::move(grpc_span)),
        grpc_stat_(std::move(grpc_stat)),
        grpc_command_(std::move(grpc_command)),
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        trace_id_sequence_(1) {

        // Snapshot the immutable identity fields once. isReloadable() guarantees
        // they never change for this agent, so the per-request getters below can
        // serve them without touching the atomic config_.
        if (const auto initial_cfg = config_.load()) {
            app_name_ = initial_cfg->app_name_;
            app_type_ = initial_cfg->app_type_;
            agent_id_ = initial_cfg->agent_id_;
            agent_name_ = initial_cfg->agent_name_;
            service_name_ = initial_cfg->service_name_;
        }

        agent_stats_ = std::make_unique<AgentStats>(this);
        url_stats_ = std::make_unique<UrlStats>(this);

        api_cache_ = std::make_unique<ApiIdCache>(kCacheSize);
        error_cache_ = std::make_unique<IdCache>(kCacheSize);
        sql_cache_ = std::make_unique<IdCache>(kCacheSize);
        sql_uid_cache_ = std::make_unique<SqlUidCache>(kCacheSize);
        
        // Initial build: no previous config, so every component is created.
        // config_ was already set from the ctor init-list above; apply_config
        // only builds the derived components and does not touch config_.
        apply_config(nullptr, config_.load());

        // Start the config-file watcher BEFORE spawning init_thread_. Both can
        // throw (std::filesystem errors, thread-creation failure under resource
        // pressure). If the watcher threw while init_thread_ were already
        // joinable, the constructor's unwind would run ~std::thread on a
        // joinable thread and call std::terminate(), crashing the host. Doing
        // the watcher first means no joinable std::thread member exists yet, so
        // a throw unwinds normally and surfaces to make_agent() as a failed
        // (noop) agent.
        start_config_file_watcher();

        try {
            init_thread_ = std::thread{&AgentImpl::init_grpc_workers, this};
        } catch (...) {
            // The watcher is already running; stop it so a failed construction
            // does not leak the watcher thread. (init_thread_ never became
            // joinable here, so it needs no cleanup.)
            stop_config_file_watcher();
            throw;
        }
    }

    std::shared_ptr<const Config> AgentImpl::getConfig() const {
        return config_.load();
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
        return agent_id_;
    }

    const std::string& AgentImpl::getAgentName() const {
        return agent_name_;
    }

    const std::string& AgentImpl::getServiceName() const {
        return service_name_;
    }

    void AgentImpl::init_header_recorders(const std::shared_ptr<const Config>& cfg) {
        // Server-side header recorders
        struct HeaderRecorderConfig {
            HeaderType type;
            int32_t annotation_key;
            const std::vector<std::string>& config_value;
        };

        const HeaderRecorderConfig server_configs[] = {
            {HTTP_REQUEST, ANNOTATION_HTTP_REQUEST_HEADER, cfg->http.server.rec_request_header},
            {HTTP_RESPONSE, ANNOTATION_HTTP_RESPONSE_HEADER, cfg->http.server.rec_response_header},
            {HTTP_COOKIE, ANNOTATION_HTTP_COOKIE, cfg->http.server.rec_request_cookie}
        };

        const HeaderRecorderConfig client_configs[] = {
            {HTTP_REQUEST, ANNOTATION_HTTP_REQUEST_HEADER, cfg->http.client.rec_request_header},
            {HTTP_RESPONSE, ANNOTATION_HTTP_RESPONSE_HEADER, cfg->http.client.rec_response_header},
            {HTTP_COOKIE, ANNOTATION_HTTP_COOKIE, cfg->http.client.rec_request_cookie}
        };

        std::shared_ptr<HttpHeaderRecorder> new_srv[3]{};
        std::shared_ptr<HttpHeaderRecorder> new_cli[3]{};

        for (const auto& recorder_cfg : server_configs) {
            if (!recorder_cfg.config_value.empty()) {
                new_srv[recorder_cfg.type] =
                    std::make_shared<HttpHeaderRecorder>(recorder_cfg.annotation_key, recorder_cfg.config_value);
            }
        }

        for (const auto& recorder_cfg : client_configs) {
            if (!recorder_cfg.config_value.empty()) {
                new_cli[recorder_cfg.type] =
                    std::make_shared<HttpHeaderRecorder>(recorder_cfg.annotation_key, recorder_cfg.config_value);
            }
        }

        for (size_t i = 0; i < 3; ++i) {
            http_srv_header_recorder_[i].store(new_srv[i]);
            http_cli_header_recorder_[i].store(new_cli[i]);
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

    void AgentImpl::apply_config(const std::shared_ptr<const Config>& old_cfg,
                                 const std::shared_ptr<const Config>& cfg) {
        // A null old_cfg means the initial construction, where nothing has been
        // built yet, so every component must be created. On a reload we already
        // have live components and only rebuild the ones whose backing config
        // changed — rebuilding unchanged components would needlessly discard
        // their accumulated state (e.g. the throughput sampler's counters).
        const bool first_build = (old_cfg == nullptr);

        // Rebuild sampler
        if (first_build || sampling_config_changed(*old_cfg, *cfg)) {
            std::unique_ptr<Sampler> sampler;
            if (compare_string(cfg->sampling.type, PERCENT_SAMPLING)) {
                sampler = std::make_unique<PercentSampler>(cfg->sampling.percent_rate);
            } else {
                sampler = std::make_unique<CounterSampler>(cfg->sampling.counter_rate);
            }

            std::shared_ptr<TraceSampler> new_sampler;
            if (cfg->sampling.new_throughput > 0 || cfg->sampling.cont_throughput > 0) {
                new_sampler = std::make_shared<ThroughputLimitTraceSampler>(this, std::move(sampler),
                                                                            cfg->sampling.new_throughput,
                                                                            cfg->sampling.cont_throughput);
            } else {
                new_sampler = std::make_shared<BasicTraceSampler>(this, std::move(sampler));
            }
            sampler_.store(std::move(new_sampler));
        }

        // Rebuild HTTP filters
        if (first_build || old_cfg->http.server.exclude_url != cfg->http.server.exclude_url) {
            std::shared_ptr<HttpUrlFilter> new_url_filter;
            if (!cfg->http.server.exclude_url.empty()) {
                new_url_filter = std::make_shared<HttpUrlFilter>(cfg->http.server.exclude_url);
            }
            http_url_filter_.store(std::move(new_url_filter));
        }

        if (first_build || old_cfg->http.server.exclude_method != cfg->http.server.exclude_method) {
            std::shared_ptr<HttpMethodFilter> new_method_filter;
            if (!cfg->http.server.exclude_method.empty()) {
                new_method_filter = std::make_shared<HttpMethodFilter>(cfg->http.server.exclude_method);
            }
            http_method_filter_.store(std::move(new_method_filter));
        }

        if (first_build || old_cfg->http.server.status_errors != cfg->http.server.status_errors) {
            std::shared_ptr<HttpStatusErrors> new_status_errors;
            if (!cfg->http.server.status_errors.empty()) {
                new_status_errors = std::make_shared<HttpStatusErrors>(cfg->http.server.status_errors);
            }
            http_status_errors_.store(std::move(new_status_errors));
        }

        // Rebuild header recorders
        if (first_build || header_recorder_config_changed(*old_cfg, *cfg)) {
            init_header_recorders(cfg);
        }

        if (grpc_agent_) {
            grpc_agent_->refreshAgentInfo();
        }
    }

    void AgentImpl::reloadConfig(std::shared_ptr<const Config> cfg) {
        // Serialize writers so a CreateAgent()-driven reload and the config-file
        // watcher thread cannot interleave their stores and publish a mixed
        // configuration. Readers remain lock-free (AtomicSharedPtr::load()).
        std::lock_guard<std::mutex> reload_lock(reload_mutex_);

        const auto old_cfg = config_.load();
        config_.store(std::move(cfg));
        apply_config(old_cfg, config_.load());
    }

    void AgentImpl::init_grpc_workers() try {
        grpc_agent_->setAgentService(this);
        grpc_metadata_->setAgentService(this);
        grpc_span_->setAgentService(this);
        grpc_stat_->setAgentService(this);
        if (grpc_command_) {
            grpc_command_->setAgentService(this);
        }

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
    } catch (const std::exception &e) {
        LOG_ERROR("failed to init grpc workers: exception = {}", e.what());
        enabled_ = false;
        return;
    } catch (...) {
        LOG_ERROR("failed to init grpc workers: unknown exception");
        enabled_ = false;
        return;
    }

    void AgentImpl::onAgentInfoSent() {
        // An in-flight registerAgent may complete just after shutdown began;
        // it must not re-enable span recording into workers being torn down.
        if (!shutting_down_) {
            enabled_ = true;
        }
    }

    void AgentImpl::close_grpc_workers() {
        grpc_agent_->stopAgentInfo();
        url_stats_->stopAddUrlStatsWorker();
        url_stats_->stopSendUrlStatsWorker();
        agent_stats_->stopAgentStatsWorker();
        grpc_agent_->stopPingWorker();
        grpc_metadata_->stopMetaWorker();
        grpc_span_->stopSpanWorker();
        grpc_stat_->stopStatsWorker();
        if (grpc_command_) {
            grpc_command_->stopCommandWorker();
        }

        wait_grpc_workers();

        grpc_agent_->closeChannel();
        grpc_metadata_->closeChannel();
        grpc_stat_->closeChannel();
        grpc_span_->closeChannel();
        if (grpc_command_) {
            grpc_command_->closeChannel();
        }

        LOG_INFO("close grpc workers done");
    }

    void AgentImpl::wait_grpc_workers() {
        // The worker thread handles are moved out of the agent into a
        // heap-allocated state block and joined on a helper thread, purely so
        // the timed wait below can emit a diagnostic when shutdown runs long.
        // The workers run member functions of this agent and its gRPC clients
        // and dereference `this` (isExiting/getConfig/getAgentStats); they are
        // therefore ALWAYS joined — never detached — before this returns.
        // Abandoning a straggler while ~AgentImpl tears those objects down
        // underneath it would be a use-after-free. Every worker's blocking
        // points are bounded (per-request gRPC deadlines plus the stream
        // cancellation performed by the stopXWorker() calls that precede this),
        // so the unconditional join still completes in bounded time.
        struct JoinState {
            std::mutex m;
            std::condition_variable cv;
            bool finished{false};
            std::vector<std::thread> threads;
        };
        auto state = std::make_shared<JoinState>();

        // init_grpc_workers assigns the other thread members; join it first so
        // moving them below cannot race those assignments. The init thread only
        // spawns workers and returns, so this join is quick.
        if (init_thread_.joinable()) {
            init_thread_.join();
        }

        std::thread* workers[] = {
            &url_stat_add_thread_, &url_stat_send_thread_,
            &agent_stat_thread_, &ping_thread_, &meta_thread_,
            &span_thread_, &stat_thread_, &command_thread_,
        };
        state->threads.reserve(std::size(workers));
        for (auto* worker : workers) {
            if (worker->joinable()) {
                state->threads.push_back(std::move(*worker));
            }
        }

        std::thread joiner([state] {
            for (auto& worker : state->threads) {
                worker.join();
            }
            {
                std::lock_guard<std::mutex> l(state->m);
                state->finished = true;
            }
            state->cv.notify_one();
        });

        bool finished;
        {
            std::unique_lock<std::mutex> l(state->m);
            finished = state->cv.wait_for(l, std::chrono::seconds(5),
                [&state] { return state->finished; });
        }

        if (!finished) {
            // Do NOT detach: the workers still reference `this` and the gRPC
            // client members, which ~AgentImpl is about to destroy. Keep
            // waiting — their blocking points are bounded, so this returns
            // shortly; we only note that shutdown ran long.
            LOG_WARN("wait grpc workers: still draining after 5s; waiting for completion");
        }
        joiner.join();
    }

    AgentImpl::~AgentImpl() noexcept {
        do_shutdown();

        // Belt-and-braces: if do_shutdown() bailed out early for any reason,
        // a joinable std::thread member would call std::terminate() when its
        // destructor runs. Detach any stragglers so member destruction stays
        // benign. Detach (not join) is used because by this point the
        // process is likely on its way down and we don't want to block.
        auto safe_detach = [](std::thread& t) noexcept {
            try { if (t.joinable()) t.detach(); } catch (...) {}
        };
        safe_detach(init_thread_);
        safe_detach(ping_thread_);
        safe_detach(meta_thread_);
        safe_detach(span_thread_);
        safe_detach(stat_thread_);
        safe_detach(command_thread_);
        safe_detach(url_stat_add_thread_);
        safe_detach(url_stat_send_thread_);
        safe_detach(agent_stat_thread_);
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
        const auto url_filter = http_url_filter_.load();
        if (url_filter && url_filter->isFiltered(rpc_point)) {
            return noopSpan();
        }
        const auto method_filter = http_method_filter_.load();
        if (!method.empty() && method_filter && method_filter->isFiltered(method)) {
            return noopSpan();
        }

        if (const auto parent_sampling = reader.Get(HEADER_SAMPLED); parent_sampling == "s0") {
            agent_stats_->incrUnsampleCont();
            return std::make_shared<UnsampledSpan>(this);
        }

        auto sampler = sampler_.load();
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
    } catch (...) {
        LOG_ERROR("new span unknown exception");
        return noopSpan();
    }

	bool AgentImpl::Enable() {
    	return enabled_;
	}

    void AgentImpl::Shutdown() noexcept {
        // Keep *this alive across do_shutdown(): if the global handle holds the
        // last reference, resetting it would destroy the agent mid-call when
        // Shutdown() was reached through a raw pointer or reference.
        std::shared_ptr<AgentImpl> self;
        try {
            std::lock_guard<std::mutex> lock(global_agent_mutex);
            auto& agent = global_agent();
            if (agent.get() == this) {
                self = std::move(agent);
                agent.reset();
            }
        } catch (...) {}
        do_shutdown();
    }

    void AgentImpl::do_shutdown() noexcept {
        if (shutting_down_.exchange(true)) {
            return;
        }

        enabled_ = false;
        try { LOG_INFO("agent shutdown"); } catch (...) {}
        try { stop_config_file_watcher(); } catch (...) {}
        try { close_grpc_workers(); } catch (...) {}
        try { shutdown_logger(); } catch (...) {}
    }

    TraceId AgentImpl::generateTraceId() {
        TraceId tid;

        tid.AgentId = agent_id_;
        tid.StartTime = start_time_;
        tid.Sequence = trace_id_sequence_.fetch_add(1);
        return tid;
    }

    void AgentImpl::recordSpan(std::unique_ptr<SpanChunk> span) const {
        if (enabled_) {
            grpc_span_->enqueueSpan(std::move(span));
        }
    }

    void AgentImpl::recordUrlStat(UrlStatEntry stat) const {
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

    void AgentImpl::removeCacheApi(const ApiMeta& api_meta) const {
        if (enabled_) {
            api_cache_->remove(ApiCacheKey{api_meta.api_str_, api_meta.type_});
        }
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

    void AgentImpl::removeCacheError(const StringMeta& error_meta) const {
        if (enabled_) {
            error_cache_->remove(error_meta.str_val_);
        }
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

    void AgentImpl::removeCacheSql(const StringMeta& sql_meta) const {
        if (enabled_) {
            sql_cache_->remove(sql_meta.str_val_);
        }
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

    void AgentImpl::removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const {
        if (enabled_) {
            sql_uid_cache_->remove(sql_uid_meta.sql_);
        }
    }

    void AgentImpl::recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                                    std::vector<std::unique_ptr<Exception>>&& exceptions) const {
        const auto cfg = getConfig();
        if (!enabled_ || !cfg->enable_callstack_trace) {
            return;
        }

        auto meta = std::make_unique<MetaData>(META_EXCEPTION, trace_id, span_id, url_template,
                                               std::move(exceptions));
        grpc_metadata_->enqueueMeta(std::move(meta));
    }

    bool AgentImpl::isStatusFail(const int status) const {
        const auto status_errors = http_status_errors_.load();
        if (enabled_ && status_errors) {
            return status_errors->isErrorCode(status);
        }
        return false;
    }

    void AgentImpl::recordServerHeader(const HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const {
        if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto recorder = http_srv_header_recorder_[which].load();
        if (enabled_ && recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    void AgentImpl::recordClientHeader(const HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const {
        if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto recorder = http_cli_header_recorder_[which].load();
        if (enabled_ && recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    struct ServerMetaData {
        std::string server_info;
        std::vector<std::string> args;
        std::vector<std::string> libs;
    };

    static std::shared_ptr<AgentImpl> make_agent(std::shared_ptr<const Config> cfg,
                                                 const std::optional<ServerMetaData>& server_meta_data) {
        if (!cfg->enable) {
            return nullptr;
        }
        try {
            auto grpc_agent = std::make_unique<GrpcAgent>(cfg);
            if (server_meta_data.has_value()) {
                grpc_agent->setServerMetaData(server_meta_data->server_info,
                                              server_meta_data->args,
                                              server_meta_data->libs);
            }
            auto grpc_metadata = std::make_unique<GrpcMetadata>(cfg);
            auto grpc_span = std::make_unique<GrpcSpan>(cfg);
            auto grpc_stat = std::make_unique<GrpcStats>(cfg);
            auto grpc_command = std::make_unique<GrpcCommand>(cfg);
            return std::make_shared<AgentImpl>(cfg,
                std::move(grpc_agent), std::move(grpc_metadata), std::move(grpc_span),
                std::move(grpc_stat), std::move(grpc_command));
        } catch (const std::exception& e) {
            LOG_ERROR("make agent exception = {}", e.what());
            return nullptr;
        } catch (...) {
            LOG_ERROR("make agent unknown exception");
            return nullptr;
        }
    }

    void SetConfigFilePath(std::string_view config_file_path) {
        set_config_file_path(config_file_path);
    }

    void SetConfigString(std::string_view config_string) {
        set_config_string(config_string);
    }

    static AgentPtr create_agent_helper(std::shared_ptr<Config> cfg,
                                        const std::optional<ServerMetaData>& server_meta_data) {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        auto& agent = global_agent();

        if (agent != nullptr) {
            // A new config always triggers a reload. Non-reloadable fields
            // (identity, collector endpoint, gRPC transport) cannot change on a
            // live agent, so retain the running values (warning on any attempted
            // change) before reloading so only the reloadable fields take effect.
            cfg->retainNonReloadableFrom(agent->getConfig());
            agent->reloadConfig(std::move(cfg));
            LOG_INFO("agent config reloaded");
            return agent;
        }

        if (!cfg->check()) {
            return noopAgent();
        }
        agent = make_agent(std::move(cfg), server_meta_data);
        if (agent == nullptr) {
            return noopAgent();
        }
        return agent;
    }

    static AgentPtr create_agent_helper(std::shared_ptr<Config> cfg) {
        return create_agent_helper(std::move(cfg), std::nullopt);
    }

    // Public entry points: a failure to configure or construct the agent must
    // surface as a noop agent, never as an exception in the host application.
    AgentPtr CreateAgent() try {
        const auto cfg = make_config();
        if (!cfg) {
            return noopAgent();
        }
        return create_agent_helper(std::move(cfg));
    } catch (...) {
        return noopAgent();
    }

    AgentPtr CreateAgent(std::string_view server_info,
                         const std::vector<std::string>& args,
                         const std::vector<std::string>& libs) try {
        const auto cfg = make_config();
        if (!cfg) {
            return noopAgent();
        }
        return create_agent_helper(std::move(cfg),
                                   ServerMetaData{std::string(server_info), args, libs});
    } catch (...) {
        return noopAgent();
    }

    AgentPtr CreateAgent(int32_t app_type) try {
        auto cfg = make_config();
        if (!cfg) {
            return noopAgent();
        }
        cfg->app_type_ = app_type;
        return create_agent_helper(std::move(cfg));
    } catch (...) {
        return noopAgent();
    }

    AgentPtr CreateAgent(int32_t app_type,
                         std::string_view server_info,
                         const std::vector<std::string>& args,
                         const std::vector<std::string>& libs) try {
        auto cfg = make_config();
        if (!cfg) {
            return noopAgent();
        }
        cfg->app_type_ = app_type;
        return create_agent_helper(std::move(cfg),
                                   ServerMetaData{std::string(server_info), args, libs});
    } catch (...) {
        return noopAgent();
    }

    AgentPtr GlobalAgent() {
        std::lock_guard<std::mutex> lock(global_agent_mutex);

        if (global_agent() == nullptr) {
            return noopAgent();
        }
        return global_agent();
    }

    bool global_agent_exists() {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        return global_agent() != nullptr;
    }

    void set_global_agent(std::shared_ptr<AgentImpl> agent) {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        global_agent() = std::move(agent);
    }

    void reset_global_agent() {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        global_agent().reset();
    }

}  // namespace pinpoint
