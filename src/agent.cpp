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
#include <string>
#include <exception>
#include <iterator>
#include <utility>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>
#include <unistd.h>

#include "logging.h"
#include "noop.h"
#include "agent.h"
#include "object_name.h"
#include "sql.h"
#include "utility.h"

namespace pinpoint {

    // Constants
    constexpr int kCacheSize = 1024;
    constexpr int kRawSqlCacheShards = 16;

    // Global agent singleton with lock-free reader access
    namespace {
        // Serializes global-agent WRITERS only (CreateAgent, Shutdown, the
        // test helpers). Readers never take it: GlobalAgent() goes through the
        // AtomicSharedPtr below, so per-request lookups cannot contend with a
        // create/reload that holds this mutex across make_config()'s file I/O
        // and YAML parsing.
        std::mutex global_agent_mutex;

        // The holder is intentionally heap-allocated and never destroyed so
        // that ~AgentImpl can never run from a static destructor. Tearing the
        // agent down during __cxa_atexit (thread joins, gRPC channel teardown,
        // logging through possibly-destroyed singletons) is unsafe for a
        // library embedded in a host application; teardown must only happen
        // through an explicit Shutdown() or a user-released reference.
        AtomicSharedPtr<AgentImpl>& global_agent() {
            static auto* holder = new AtomicSharedPtr<AgentImpl>();
            return *holder;
        }
    }

    AgentImpl::AgentImpl(std::shared_ptr<const Config> cfg,
                         std::unique_ptr<GrpcAgent> grpc_agent,
                         std::unique_ptr<GrpcMetadata> grpc_metadata,
                         std::unique_ptr<GrpcSpan> grpc_span,
                         std::unique_ptr<GrpcStats> grpc_stat,
                         std::unique_ptr<GrpcCommand> grpc_command,
                         int32_t app_type) :
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

        api_cache_ = std::make_unique<ApiIdCache>(kCacheSize);
        error_cache_ = std::make_unique<IdCache>(kCacheSize);
        sql_cache_ = std::make_unique<IdCache>(kCacheSize);
        sql_uid_cache_ = std::make_unique<SqlUidCache>(kCacheSize);
        raw_sql_id_cache_ = std::make_unique<RawSqlCache>(kCacheSize, kRawSqlCacheShards);
        raw_sql_uid_cache_ = std::make_unique<RawSqlCache>(kCacheSize, kRawSqlCacheShards);

        // Initial build: no previous runtime, so every component is created
        // and published together in one atomic store.
        apply_config(nullptr, std::move(cfg));

        // Remember the process that constructed the agent. CreateAgent() is
        // deliberately "cold": it starts no threads, opens no gRPC channel and
        // installs no config-file watcher here — Start() does all of that. This
        // makes the agent safe to construct in a master process that will
        // fork(): there is nothing live to break at the fork point. Start()
        // later compares getpid() against create_pid_ to tell whether it is
        // running in a forked child (and must mint a unique agent id).
        create_pid_ = getpid();
    }

    void AgentImpl::Start() noexcept try {
        // Serialized against do_shutdown() (see lifecycle_mutex_): a
        // concurrent Shutdown() waits for the writes to owner_pid_ and
        // init_thread_ below to be published before tearing down, and a
        // Start() arriving after (or during) shutdown refuses instead of
        // spawning workers nobody will ever join.
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (shutting_down_) {
            LOG_WARN("agent start rejected: agent is shut down");
            return;
        }

        // Idempotent: only the first Start() in the object's life brings it
        // online. The recommended fork model is a COLD CreateAgent() in the
        // master (started_ == false) followed by Start() in each child, so a
        // child always takes the full path below. The discouraged "Start() in
        // the master, then fork" case leaves an inherited started_ == true in
        // the child; Start() there is a no-op (its inherited worker handles are
        // dead and re-spawning over them would terminate), and teardown stays
        // crash-free via the pid guard in do_shutdown().
        if (started_.exchange(true)) {
            return;
        }

        // Give forked workers a distinct identity before any channel/metadata
        // is built (the gRPC headers read the id via getAgentId()).
        refresh_agent_id_for_process();
        owner_pid_ = getpid();

        // Fork safety rests on an invariant, not on GRPC_ENABLE_FORK_SUPPORT:
        // CreateAgent() is cold (it triggers no grpc_init — no channel, stub or
        // credentials are built until openChannel() runs from init_grpc_workers
        // below), so the master holds NO live gRPC runtime at the fork point.
        // Each child's Start() then performs that child's first, fresh grpc_init,
        // inheriting no gRPC state to recover. This is gRPC's own "instantiate
        // gRPC objects only after fork()" pattern, which needs no pthread_atfork
        // fork handlers. GRPC_ENABLE_FORK_SUPPORT would only matter if grpc_init
        // had run before the fork; the cold model guarantees it did not. (This
        // assumes the HOST application likewise does not use gRPC before forking
        // — an env var set here, post-fork in the child, could not fix that.)

        // Start the config-file watcher BEFORE spawning init_thread_ so that, if
        // thread creation throws, no joinable std::thread member exists yet and
        // the stack unwinds without hitting a joinable-thread destructor.
        start_config_file_watcher();

        try {
            init_thread_ = std::thread{&AgentImpl::init_grpc_workers, this};
        } catch (...) {
            stop_config_file_watcher();
            throw;
        }
    } catch (const std::exception& e) {
        try { LOG_ERROR("agent start failed: exception = {}", e.what()); } catch (...) {}
        enabled_ = false;
        started_ = false;
    } catch (...) {
        try { LOG_ERROR("agent start failed: unknown exception"); } catch (...) {}
        enabled_ = false;
        started_ = false;
    }

    void AgentImpl::refresh_agent_id_for_process() {
        // In the process that constructed the agent, keep the configured id
        // exactly as resolved — non-fork behavior is unchanged.
        if (create_pid_ == getpid()) {
            return;
        }

        const auto cfg = getConfig();
        const std::string old_id = *agent_id_;
        std::string new_id;
        if (cfg && cfg->agent_id_pinned_) {
            // Explicit id pinned by the user: keep it recognizable but append a
            // pid so sibling workers differ. Cap at the max id length.
            const std::string suffix = "-" + std::to_string(static_cast<long>(getpid()));
            const size_t max_len = object_name::AGENT_ID_MAX_LEN;
            std::string base = old_id;
            if (base.size() + suffix.size() > max_len && suffix.size() < max_len) {
                base.resize(max_len - suffix.size());
            }
            new_id = base + suffix;
        } else {
            // Auto-generated id: mint a fresh one per worker.
            new_id = base64_encode_uuid(generate_uuid_v7());
        }

        // Only the agent id is made process-unique. agent_id is the unique
        // instance key on the collector; agent_name is a display alias (and is
        // still served from the captured config in the gRPC headers), so it is
        // deliberately left untouched to keep the two consistent.
        agent_id_ = std::make_shared<const std::string>(new_id);
        LOG_INFO("fork-safe start: agent id '{}' -> '{}'", old_id, new_id);
    }

    std::shared_ptr<const Config> AgentImpl::getConfig() const {
        const auto runtime = runtime_.load();
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
        runtime_.store(build_runtime(old_rt, std::move(cfg)));

        if (grpc_agent_) {
            grpc_agent_->refreshAgentInfo();
        }
    }

    void AgentImpl::reloadConfig(std::shared_ptr<const Config> cfg) {
        // Serialize writers: building the new runtime is a load-build-store
        // read-modify-write of runtime_, so a CreateAgent()-driven reload and
        // the config-file watcher thread could otherwise both build from the
        // same old runtime and lose one of the updates. Readers remain
        // lock-free (a single AtomicSharedPtr::load()).
        std::lock_guard<std::mutex> reload_lock(reload_mutex_);
        apply_config(runtime_.load(), std::move(cfg));
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
        // CreateAgent() path so a master can fork before Start().
        grpc_agent_->openChannel();
        grpc_metadata_->openChannel();
        grpc_span_->openChannel();
        grpc_stat_->openChannel();
        if (grpc_command_) {
            grpc_command_->openChannel();
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

        std::thread joiner;
        try {
            joiner = std::thread([state] {
                for (auto& worker : state->threads) {
                    worker.join();
                }
                {
                    std::lock_guard<std::mutex> l(state->m);
                    state->finished = true;
                }
                state->cv.notify_one();
            });
        } catch (...) {
            // Thread creation failing (EAGAIN) is precisely the resource-
            // exhaustion case. Unwinding with joinable handles still in
            // `state` would run ~thread on them and std::terminate the host,
            // so join inline — losing only the slow-shutdown diagnostic.
            try { LOG_WARN("wait grpc workers: joiner thread unavailable, joining inline"); } catch (...) {}
            for (auto& worker : state->threads) {
                worker.join();
            }
            return;
        }

        bool finished;
        {
            std::unique_lock<std::mutex> l(state->m);
            finished = state->cv.wait_for(l, std::chrono::seconds(5),
                [&state] { return state->finished; });
        }

        if (!finished) {
            // Do NOT abandon them: the workers still reference `this` and the gRPC
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
        // One atomic load covers the filters and the sampler for this request.
        const auto runtime = runtime_.load();
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
            return std::make_shared<UnsampledSpan>(this);
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
            // Pass this runtime's config so the span skips a second atomic load
            // and lives on the same config generation its admission was decided
            // under, and hand the resolved trace id to the impl-level extract.
            auto span = std::make_shared<SpanImpl>(this, operation, rpc_point, runtime->config);
            span->extractContext(reader, std::move(trace_id));
            return span;
        }
        return std::make_shared<UnsampledSpan>(this);
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
            self = global_agent().load();
            if (self.get() == this) {
                global_agent().store(nullptr);
            } else {
                self.reset();
            }
        } catch (...) {}
        do_shutdown();
    }

    void AgentImpl::do_shutdown() noexcept {
        if (shutting_down_.exchange(true)) {
            return;
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
            // The gRPC clients inherited from the parent are intentionally
            // leaked, but not here: releasing the unique_ptrs would null them
            // while a racing thread that loaded enabled_ == true just before
            // the store above may still be about to dereference grpc_span_ /
            // grpc_metadata_ (recordSpan, cacheApi, ...) — a null operator->
            // crash in the host. The release is deferred to ~AgentImpl, so
            // the pointers stay valid for the whole lifetime of this object.
            return;
        }

        try { LOG_INFO("agent shutdown"); } catch (...) {}
        try { stop_config_file_watcher(); } catch (...) {}
        try { close_grpc_workers(); } catch (...) {}
        try { shutdown_logger(); } catch (...) {}
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
        static const SqlNormalizer normalizer(64 * 1024);
        const bool enable_raw_sql_cache = getConfig()->sql.enable_raw_sql_cache;

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
                                    std::vector<std::unique_ptr<Exception>>&& exceptions) const {
        const auto cfg = getConfig();
        if (!enabled_ || !cfg->enable_callstack_trace) {
            return;
        }

        auto meta = std::make_unique<MetaData>(META_EXCEPTION, trace_id, span_id, url_template,
                                               std::move(exceptions));
        grpc_metadata_->enqueueMeta(std::move(meta));
    }

    // These check enabled_ before runtime_.load(): the load is a
    // shared-lock + shared_ptr copy on platforms without the C++20 atomic
    // shared_ptr (see AtomicSharedPtr), so ordering the cheap flag first
    // makes the disabled-agent path free.
    bool AgentImpl::isStatusFail(const int status) const {
        if (!enabled_) {
            return false;
        }
        const auto runtime = runtime_.load();
        if (runtime->http_status_errors) {
            return runtime->http_status_errors->isErrorCode(status);
        }
        return false;
    }

    void AgentImpl::recordServerHeader(const HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const {
        if (!enabled_ || which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto runtime = runtime_.load();
        const auto& recorder = runtime->http_srv_header_recorder[which];
        if (recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    void AgentImpl::recordClientHeader(const HeaderType which, HeaderReader& reader, AnnotationPtr annotation) const {
        if (!enabled_ || which < HTTP_REQUEST || which > HTTP_COOKIE) {
            return;
        }
        const auto runtime = runtime_.load();
        const auto& recorder = runtime->http_cli_header_recorder[which];
        if (recorder) {
            recorder->recordHeader(reader, annotation);
        }
    }

    struct ServerMetaData {
        std::string server_info;
        std::vector<std::string> args;
        std::vector<std::string> libs;
    };

    static std::shared_ptr<AgentImpl> make_agent(std::shared_ptr<const Config> cfg,
                                                 int32_t app_type,
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
                std::move(grpc_stat), std::move(grpc_command), app_type);
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

    void SetConfigEnvVarPrefix(std::string_view prefix) {
        set_env_prefix(prefix);
    }

    static AgentPtr create_agent_helper(int32_t app_type,
                                        const std::optional<ServerMetaData>& server_meta_data) {
        std::lock_guard<std::mutex> lock(global_agent_mutex);
        auto agent = global_agent().load();

        // Build the config under the lock so the running-config snapshot
        // handed to make_config() cannot race with a concurrent CreateAgent()
        // or Shutdown() swapping the global agent.
        auto cfg = make_config(agent ? agent->getConfig() : nullptr);
        if (!cfg) {
            return noopAgent();
        }

        if (agent != nullptr) {
            // A new config always triggers a reload. make_config() already
            // returned the final reload config (non-reloadable fields retained
            // from the running config, logger reconfigured). app_type is part
            // of the agent identity and likewise fixed for the agent's
            // lifetime, so the incoming value is ignored on reload.
            agent->reloadConfig(std::move(cfg));
            LOG_INFO("agent config reloaded");
            return agent;
        }

        if (!cfg->check()) {
            return noopAgent();
        }
        agent = make_agent(std::move(cfg), app_type, server_meta_data);
        if (agent == nullptr) {
            return noopAgent();
        }
        global_agent().store(agent);
        return agent;
    }

    // Public entry point: a failure to configure or construct the agent must
    // surface as a noop agent, never as an exception in the host application.
    AgentPtr CreateAgent(int32_t app_type,
                         std::string_view server_info,
                         const std::vector<std::string>& args,
                         const std::vector<std::string>& libs) try {
        return create_agent_helper(app_type,
                                   ServerMetaData{std::string(server_info), args, libs});
    } catch (...) {
        return noopAgent();
    }

    AgentPtr GlobalAgent() {
        // Reader path: a single AtomicSharedPtr load, no global_agent_mutex.
        // Host applications call this per request, and taking the writers'
        // mutex here would stall every request behind a reload in progress.
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
