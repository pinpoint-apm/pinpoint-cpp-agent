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
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <grpcpp/client_context.h>

#include "absl/strings/str_cat.h"
#include "version.h"
#include "logging.h"
#include "stat.h"
#include "grpc.h"
#include "grpc_builders.h"

namespace pinpoint {

    namespace {
        int grpc_collector_port(const Config& config, ClientType client_type) {
            switch (client_type) {
                case AGENT:
                case METADATA:
                    return config.collector.agent_port;
                case SPAN:
                    return config.collector.span_port;
                case STATS:
                    return config.collector.stat_port;
            }
            return config.collector.agent_port;
        }

        std::string grpc_client_name(ClientType client_type) {
            switch (client_type) {
                case AGENT:
                    return "agent";
                case METADATA:
                    return "metadata";
                case SPAN:
                    return "span";
                case STATS:
                    return "stats";
            }
            return "agent";
        }

        std::string read_file(std::string_view path) {
            std::ifstream file(std::string(path), std::ios::binary);
            if (!file.is_open()) {
                throw std::runtime_error(absl::StrCat("failed to open gRPC TLS certificate file: ", path));
            }

            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        std::shared_ptr<grpc::ChannelCredentials> make_channel_credentials(
                const Config::GrpcSslOptions& ssl,
                std::string_view client_name) {
            if (!ssl.enable) {
                return grpc::InsecureChannelCredentials();
            }

            grpc::SslCredentialsOptions ssl_options;
            const auto& cert_path = !ssl.trust_cert_file_path.empty()
                ? ssl.trust_cert_file_path
                : ssl.root_cert_file_path;
            if (!cert_path.empty()) {
                ssl_options.pem_root_certs = read_file(cert_path);
            }

            LOG_INFO("create {} grpc TLS channel credentials: trustCertFilePath='{}', rootCertFilePath='{}'",
                     client_name, ssl.trust_cert_file_path, ssl.root_cert_file_path);
            return grpc::SslCredentials(ssl_options);
        }

        grpc::ChannelArguments make_channel_arguments(const Config::GrpcChannelOptions& options) {
            grpc::ChannelArguments channel_args;

            channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, options.keepalive_time_ms);
            channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, options.keepalive_timeout_ms);
            channel_args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,
                                options.keepalive_permit_without_calls ? 1 : 0);
            channel_args.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, options.max_send_message_size);
            channel_args.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, options.max_receive_message_size);

            return channel_args;
        }

        std::shared_ptr<grpc::Channel> build_channel(const Config& config, ClientType client_type) {
            const auto& options = config.collector.grpc.channel;
            const auto client_name = grpc_client_name(client_type);
            const auto addr = absl::StrCat(config.collector.host, ":", grpc_collector_port(config, client_type));
            auto credentials = make_channel_credentials(config.collector.grpc.ssl, client_name);
            auto channel_args = make_channel_arguments(options);

            LOG_INFO("create {} grpc channel: addr={}, ssl={}, keepaliveTimeMs={}, keepaliveTimeoutMs={}, "
                     "maxSendMessageSize={}, maxReceiveMessageSize={}",
                     client_name, addr, config.collector.grpc.ssl.enable, options.keepalive_time_ms,
                     options.keepalive_timeout_ms, options.max_send_message_size,
                     options.max_receive_message_size);
            return grpc::CreateCustomChannel(addr, credentials, channel_args);
        }

    }

    ExponentialBackoff::ExponentialBackoff(std::chrono::milliseconds initial_interval,
                                           double multiplier,
                                           double randomization_factor,
                                           std::chrono::milliseconds max_interval)
        : initial_interval_(initial_interval),
          multiplier_(multiplier),
          randomization_factor_(randomization_factor),
          max_interval_(max_interval),
          rng_(std::random_device{}()) {}

    std::chrono::milliseconds ExponentialBackoff::next_delay() {
        const auto attempt = attempt_++;
        double delay_ms = static_cast<double>(initial_interval_.count()) * std::pow(multiplier_, attempt);
        const auto max_ms = static_cast<double>(max_interval_.count());
        if (!std::isfinite(delay_ms) || delay_ms > max_ms) {
            delay_ms = max_ms;
        }

        const auto jitter = std::max(0.0, randomization_factor_);
        if (jitter > 0.0) {
            std::uniform_real_distribution<double> distribution(1.0 - jitter, 1.0 + jitter);
            delay_ms *= distribution(rng_);
        }

        if (!std::isfinite(delay_ms) || delay_ms <= 0.0) {
            return initial_interval_;
        }
        return std::chrono::milliseconds(
            static_cast<std::chrono::milliseconds::rep>(std::llround(delay_ms)));
    }

    void ExponentialBackoff::reset() {
        attempt_ = 0;
    }

    GrpcClient::GrpcClient(ClientType client_type, std::shared_ptr<const Config> config,
                           const GrpcClientTuning& tuning)
        : config_{std::move(config)}, tuning_{tuning}, client_type_(client_type),
          channel_ready_backoff_{tuning} {
        client_name_ = grpc_client_name(client_type_);
        // The channel and stub are NOT built here: doing so would trigger
        // grpc_init (and gRPC's background threads) at agent construction
        // time. openChannel(), called when the agent starts, performs that
        // work.
    }

    void GrpcClient::set_request_deadline(grpc::ClientContext& context) const {
        context.set_deadline(std::chrono::system_clock::now() + tuning_.request_timeout);
    }

    void GrpcClient::openChannel() {
        std::unique_lock<std::mutex> lock(channel_mutex_);
        if (channel_) {
            return;
        }
        channel_ = build_channel(*config_, client_type_);
        create_stub();
    }

    void GrpcClient::setAgentService(AgentService* agent) {
        agent_ = agent;
    }

    // gRPC metadata key constants
    const std::string METADATA_APPLICATION_NAME = "applicationname";
    const std::string METADATA_AGENT_ID = "agentid";
    const std::string METADATA_START_TIME = "starttime";
    const std::string METADATA_SERVICE_TYPE = "servicetype";
    const std::string METADATA_AGENT_NAME = "agentname";
    const std::string METADATA_SOCKET_ID = "socketid";
    const std::string METADATA_SUPPORT_COMMAND_CODE = "supportcommandcode";
    const std::string METADATA_PROTOCOL_VERSION = "protocol.version";
    const std::string METADATA_SERVICE_NAME = "servicename";
    const std::string METADATA_API_KEY = "apikey";

    std::vector<std::pair<std::string, std::string>>
    build_grpc_metadata(const Config& config, std::string_view agent_id,
                        int64_t start_time, int32_t app_type, unsigned long socket_id) {
        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back(METADATA_APPLICATION_NAME, config.app_name_);
        headers.emplace_back(METADATA_AGENT_ID, std::string(agent_id));
        headers.emplace_back(METADATA_START_TIME, std::to_string(start_time));
        headers.emplace_back(METADATA_SERVICE_TYPE, std::to_string(app_type));
        headers.emplace_back(METADATA_PROTOCOL_VERSION, std::to_string(config.protocol_version()));

        if (config.is_v4()) {
            // v4 (ClientHeaderFactoryV4): agentname always sent, plus servicename + apikey.
            headers.emplace_back(METADATA_AGENT_NAME, config.agent_name_);
            headers.emplace_back(METADATA_SERVICE_NAME, config.service_name_);
            headers.emplace_back(METADATA_API_KEY, config.api_key_);
        } else if (!config.agent_name_.empty()) {
            // v1/v3 (ClientHeaderFactoryV1): agentname only when present.
            headers.emplace_back(METADATA_AGENT_NAME, config.agent_name_);
        }
        if (socket_id > 0) {
            headers.emplace_back(METADATA_SOCKET_ID, std::to_string(socket_id));
        }
        return headers;
    }

    void GrpcClient::build_grpc_context(grpc::ClientContext* context, unsigned long socket_id) const {
        assert(agent_ != nullptr && "setAgentService() must be called before build_grpc_context()");
        // The socket-id-independent headers are immutable once the agent is
        // started (agent id / start time / app type are all fixed before any
        // worker runs), and this runs for every unary request — build them
        // once and reuse. call_once because some clients build contexts from
        // more than one thread (GrpcAgent: ping + AgentInfo workers,
        // GrpcCommand: command worker + active-thread-count streams).
        std::call_once(grpc_metadata_once_, [this] {
            grpc_metadata_cache_ = build_grpc_metadata(*config_, agent_->getAgentId(),
                                                       agent_->getStartTime(), agent_->getAppType(), 0);
        });
        for (const auto& [key, value] : grpc_metadata_cache_) {
            context->AddMetadata(key, value);
        }
        if (socket_id > 0) {
            context->AddMetadata(METADATA_SOCKET_ID, std::to_string(socket_id));
        }
    }

    bool GrpcClient::wait_channel_ready(std::chrono::milliseconds delay) const {
        auto state = channel_->GetState(true);
        if (state == GRPC_CHANNEL_READY) {
            return true;
        }

        LOG_INFO("wait {} grpc channel ready: state = {}", client_name_, static_cast<int>(state));

        // Elapsed-time math on steady_clock: a wall-clock step (NTP) must not
        // stretch or truncate the retry window. gRPC's WaitForStateChange only
        // accepts a system_clock deadline, so that one call converts the
        // bounded slice back to wall time.
        const auto deadline = std::chrono::steady_clock::now() + delay;
        while (state != GRPC_CHANNEL_READY) {
            if (stopping()) {
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto wait_for = std::min(remaining, tuning_.backoff_sleep_slice);
            channel_->WaitForStateChange(state, std::chrono::system_clock::now() + wait_for);
            state = channel_->GetState(false);
        }

        return state == GRPC_CHANNEL_READY;
    }

    bool GrpcClient::readyChannel() {
        std::unique_lock<std::mutex> lock(channel_mutex_);
        if (stopping()) {
            return false;
        }

        const auto wait_start = std::chrono::steady_clock::now();
        if (channel_->GetState(false) != GRPC_CHANNEL_READY) {
            while (true) {
                if (stopping()) {
                    return false;
                }
                const auto delay = channel_ready_backoff_.next_delay();
                LOG_INFO("{} grpc channel is not ready; retry for {}ms", client_name_, delay.count());
                if (wait_channel_ready(delay)) {
                    channel_ready_backoff_.reset();
                    break;
                }
            }
        } else {
            channel_ready_backoff_.reset();
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - wait_start);
        if (elapsed >= tuning_.slow_recovery_threshold) {
            on_slow_channel_recovery(elapsed);
        }
        return true;
    }

    //GrpcMetadata

    GrpcMetadata::GrpcMetadata(std::shared_ptr<const Config> config, const GrpcClientTuning& tuning)
        : GrpcClient(METADATA, std::move(config), tuning) {}

    void GrpcMetadata::create_stub() {
        set_meta_stub(v1::Metadata::NewStub(channel_));
    }

    template<typename Request, typename StubMethod>
    GrpcRequestStatus GrpcMetadata::send_meta_helper(StubMethod stub_method, Request& request, std::string_view operation_name) {
        if (!readyChannel()) {
            return SEND_FAIL;
        }

        v1::PResult reply;
        grpc::ClientContext ctx;

        // Deliberately NOT under channel_mutex_: meta_stub_ is created by
        // openChannel() before this worker starts and gRPC stubs are
        // thread-safe, while holding the mutex across a blocking unary call
        // would serialize it against readyChannel()'s (potentially long)
        // backoff loop for no benefit.
        build_grpc_context(&ctx, 0);
        set_request_deadline(ctx);

        const grpc::Status status = stub_method(&ctx, request, &reply);

        if (!status.ok()) {
            LOG_ERROR("failed to send {} metadata: {}, {}",
                      operation_name, static_cast<int>(status.error_code()), status.error_message());
            return SEND_FAIL;
        }

        if (!reply.success()) {
            LOG_INFO("failed to send {} metadata: PResult.success=false", operation_name);
            return SEND_FAIL;
        }

        LOG_DEBUG("success to send {} metadata", operation_name);
        return SEND_OK;
    }

    GrpcRequestStatus GrpcMetadata::send_api_meta(ApiMeta& api_meta) {
        v1::PApiMetaData grpc_api_meta;

        grpc_api_meta.set_apiid(api_meta.id_);
        grpc_api_meta.set_apiinfo(api_meta.api_str_);
        grpc_api_meta.set_type(api_meta.type_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PApiMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestApiMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_api_meta, "api");
    }

    GrpcRequestStatus GrpcMetadata::send_error_meta(StringMeta& error_meta) {
        v1::PStringMetaData grpc_error_meta;

        grpc_error_meta.set_stringid(error_meta.id_);
        grpc_error_meta.set_stringvalue(error_meta.str_val_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PStringMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestStringMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_error_meta, "error");
    }

    GrpcRequestStatus GrpcMetadata::send_sql_meta(StringMeta& sql_meta) {
        v1::PSqlMetaData grpc_sql_meta;

        grpc_sql_meta.set_sqlid(sql_meta.id_);
        grpc_sql_meta.set_sql(sql_meta.str_val_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PSqlMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestSqlMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_sql_meta, "sql");
    }

    GrpcRequestStatus GrpcMetadata::send_sql_uid_meta(SqlUidMeta& sql_uid_meta) {
        v1::PSqlUidMetaData grpc_sql_uid_meta;

        grpc_sql_uid_meta.set_sqluid(std::string(sql_uid_meta.uid_.begin(), sql_uid_meta.uid_.end()));
        grpc_sql_uid_meta.set_sql(sql_uid_meta.sql_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PSqlUidMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestSqlUidMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_sql_uid_meta, "sql uid");
    }

    GrpcRequestStatus GrpcMetadata::send_exception_meta(ExceptionMeta& exception_meta) {
        google::protobuf::Arena arena;
        auto* grpc_exception_meta = build_exception_metadata(exception_meta.txid_,
                                                            exception_meta.span_id_,
                                                            exception_meta.url_template_,
                                                            exception_meta.exceptions_,
                                                            &arena);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PExceptionMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestExceptionMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, *grpc_exception_meta, "exception");
    }

    GrpcRequestStatus GrpcMetadata::send_meta(MetaData& meta) {
        return std::visit([this](auto&& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ApiMeta>) {
                return send_api_meta(value);
            } else if constexpr (std::is_same_v<T, StringMeta>) {
                if (value.type_ == STRING_META_ERROR) {
                    return send_error_meta(value);
                }
                return send_sql_meta(value);
            } else if constexpr (std::is_same_v<T, SqlUidMeta>) {
                return send_sql_uid_meta(value);
            } else if constexpr (std::is_same_v<T, ExceptionMeta>) {
                return send_exception_meta(value);
            }
            return SEND_FAIL;
        }, meta.value_);
    }

    void GrpcMetadata::release_failed_cache(const MetaData& meta) const {
        std::visit([this](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ApiMeta>) {
                agent_->removeCacheApi(value);
            } else if constexpr (std::is_same_v<T, StringMeta>) {
                if (value.type_ == STRING_META_ERROR) {
                    agent_->removeCacheError(value);
                } else if (value.type_ == STRING_META_SQL) {
                    agent_->removeCacheSql(value);
                }
            } else if constexpr (std::is_same_v<T, SqlUidMeta>) {
                agent_->removeCacheSqlUid(value);
            }
        }, meta.value_);
    }

    void GrpcMetadata::enqueueMeta(std::unique_ptr<MetaData> meta) noexcept try {
        if (meta == nullptr || (agent_ != nullptr && agent_->isExiting())) {
            return;
        }

        // Built before the critical section so that on every failure below —
        // overflow drop or a throwing queue insertion — the metadata is still
        // owned here and its cache entry can be released.
        PendingMeta pending{std::move(meta), 0, {}};
        const auto max_queue_size = static_cast<size_t>(config_->collector.grpc.channel.sender_queue_size);
        try {
            std::unique_lock<std::mutex> lock(meta_queue_mutex_);

            if (meta_queue_.size() + retry_queue_.size() < max_queue_size) {
                // deque::push_back gives the strong guarantee and PendingMeta's
                // move cannot throw, so `pending` still owns the metadata
                // whenever this block is left by exception.
                meta_queue_.push_back(std::move(pending));
            }
        } catch (const std::exception &e) {
            LOG_ERROR("failed to enqueue metadata: exception = {}", e.what());
        }

        if (pending.meta != nullptr) {
            // Dropped on overflow, or never enqueued because the insertion
            // threw. Reported outside the lock, at WARN so outage data loss
            // is visible at the default log level, rate-limited so a full
            // queue cannot flood the log from request threads.
            if (const auto dropped = meta_drop_reporter_.record()) {
                LOG_WARN("metadata queue overflow: {} dropped in total (max queue size {})",
                         dropped, max_queue_size);
            }
            // The producer registered the id in the agent caches before
            // enqueueing (see AgentImpl::cacheApi), and a cache hit
            // suppresses re-publication — dropping the message without
            // releasing the entry would leave spans referencing metadata the
            // collector never receives, for the rest of the process lifetime.
            // Release it so the id is regenerated and re-sent, exactly like
            // the retry-exhaustion path in run_meta_worker(). Outside
            // meta_queue_mutex_ for the lock-order reasons documented there.
            if (agent_ != nullptr) {
                release_failed_cache(*pending.meta);
            }
            return;
        }

        // Notify after releasing the lock so the woken worker does not
        // immediately block on meta_queue_mutex_ (matches enqueueSpan).
        meta_queue_cv_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue metadata: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to enqueue metadata: unknown exception");
    }

    void GrpcMetadata::schedule_retry(PendingMeta&& pending) {
        // No notify: this runs on the worker thread — the only waiter on
        // meta_queue_cv_ — which re-examines the retry queue in
        // pop_next_meta() right after scheduling.
        pending.available_at = std::chrono::steady_clock::now() + tuning_.meta_retry_delay;
        retry_queue_.emplace(pending.available_at, std::move(pending));
    }

    bool GrpcMetadata::pop_next_meta(PendingMeta& pending, std::unique_lock<std::mutex>& lock) {
        while (true) {
            if (meta_stop_requested_ || agent_->isExiting()) {
                return false;
            }

            if (!meta_queue_.empty()) {
                pending = std::move(meta_queue_.front());
                meta_queue_.pop_front();
                return true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (!retry_queue_.empty()) {
                auto retry = retry_queue_.begin();
                if (retry->first <= now) {
                    pending = std::move(retry->second);
                    retry_queue_.erase(retry);
                    return true;
                }

                meta_queue_cv_.wait_until(lock, retry->first, [this] {
                    return !meta_queue_.empty() || meta_stop_requested_ || agent_->isExiting();
                });
            } else {
                meta_queue_cv_.wait(lock, [this] {
                    return !meta_queue_.empty() || !retry_queue_.empty() || meta_stop_requested_ || agent_->isExiting();
                });
            }
        }
    }

    void GrpcMetadata::sendMetaWorker() {
        // Supervise the loop body so an unexpected exception cannot kill
        // metadata upload for the process lifetime. Exceptions from an
        // individual send are contained by run_meta_worker() and converted to
        // SEND_FAIL, preserving the popped item for the normal retry path.
        // Only a stop request or agent exit ends the worker.
        while (true) {
            try {
                run_meta_worker();
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("failed to send grpc meta: exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("failed to send grpc meta: unknown exception");
            }

            std::unique_lock<std::mutex> lock(meta_queue_mutex_);
            if (meta_queue_cv_.wait_for(lock, tuning_.worker_restart_delay, [this] {
                    return meta_stop_requested_ || agent_->isExiting();
                })) {
                break;
            }
        }
        LOG_INFO("send meta worker end");
    }

    void GrpcMetadata::run_meta_worker() {
        while (true) {
            PendingMeta pending;
            {
                std::unique_lock<std::mutex> lock(meta_queue_mutex_);
                if (!pop_next_meta(pending, lock)) {
                    break;
                }
            }

            auto send_status = SEND_FAIL;
            try {
                send_status = send_meta(*pending.meta);
            } catch (const std::exception& e) {
                // The item has already been removed from the queue. Treat a
                // thrown send/build exception like any other failed RPC so it
                // is retried and, after exhaustion, its cache entry is
                // released. Letting it reach the outer supervisor would
                // destroy `pending` and permanently suppress re-publication.
                LOG_ERROR("metadata send threw an exception: {}", e.what());
            } catch (...) {
                LOG_ERROR("metadata send threw an unknown exception");
            }

            const auto sent = send_status == SEND_OK;
            if (sent) {
                continue;
            }

            ++pending.retry_count;
            if (agent_->isExiting()) {
                break;
            }

            if (pending.retry_count <= tuning_.meta_retry_max_attempts) {
                LOG_DEBUG("retry metadata send: retryCount={}/{}", pending.retry_count, tuning_.meta_retry_max_attempts);
                try {
                    std::unique_lock<std::mutex> lock(meta_queue_mutex_);
                    schedule_retry(std::move(pending));
                } catch (...) {
                    // Enqueuing the retry threw (e.g. bad_alloc). The popped item
                    // would otherwise be destroyed here with its cache id still
                    // marked published, leaving spans referencing metadata the
                    // collector never receives. Release the cache entry so the id
                    // is regenerated and re-sent later. schedule_retry only throws
                    // from the queue insertion, before `pending` is moved, so
                    // `pending.meta` is still valid.
                    LOG_ERROR("failed to schedule metadata retry; releasing cache to allow re-send");
                    if (pending.meta) {
                        release_failed_cache(*pending.meta);
                    }
                }
            } else {
                LOG_INFO("drop metadata after retry exhaustion: retryCount={}", pending.retry_count);
                // Outside meta_queue_mutex_: removeCache* takes the agent
                // caches' internal locks, and nesting those under the queue
                // mutex extends enqueueMeta contention on application threads
                // and is a latent lock-order hazard.
                release_failed_cache(*pending.meta);
            }
        }
    }

    void GrpcMetadata::stopMetaWorker() {
        request_stop();
        std::unique_lock<std::mutex> lock(meta_queue_mutex_);
        meta_stop_requested_ = true;
        meta_queue_cv_.notify_all();
    }

    //GrpcCommand

    namespace {
        int32_t command_code(const v1::PCmdRequest& request) {
            return static_cast<int32_t>(request.command_case());
        }

        std::string support_command_code_header(const std::vector<int32_t>& command_codes) {
            std::string value;
            for (auto code : command_codes) {
                if (!value.empty()) {
                    value.append(";");
                }
                value.append(std::to_string(code));
            }
            return value;
        }
    }

    void GrpcCommandDispatcher::registerHandler(int32_t command_code, Handler handler) {
        handlers_[command_code] = std::move(handler);
    }

    std::vector<int32_t> GrpcCommandDispatcher::supportedCommandCodes() const {
        std::vector<int32_t> codes;
        codes.reserve(handlers_.size());
        for (const auto& [code, handler] : handlers_) {
            codes.push_back(code);
        }
        std::sort(codes.begin(), codes.end());
        return codes;
    }

    bool GrpcCommandDispatcher::handle(
            const v1::PCmdRequest& request,
            grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream) const {
        const auto code = command_code(request);
        const auto handler = handlers_.find(code);
        if (handler == handlers_.end()) {
            v1::PCmdMessage fail_message;
            auto* fail = fail_message.mutable_failmessage();
            fail->set_responseid(request.requestid());
            fail->mutable_message()->set_value("NOT_SUPPORTED_REQUEST");
            return stream == nullptr || stream->Write(fail_message);
        }
        return handler->second(request, stream);
    }

    class GrpcCommand::ActiveThreadCountStream {
    public:
        ActiveThreadCountStream(GrpcCommand* owner, unsigned long socket_id, int32_t request_id)
            : owner_(owner), socket_id_(socket_id), request_id_(request_id) {}

        ~ActiveThreadCountStream() {
            stop();
        }

        void start() {
            worker_ = std::thread{&ActiveThreadCountStream::run, this};
        }

        void stop() {
            {
                // Setting the flag under cv_mutex_ prevents a lost wakeup when
                // run() is between checking the predicate and blocking.
                std::unique_lock<std::mutex> lock(cv_mutex_);
                stop_requested_ = true;
            }
            {
                std::unique_lock<std::mutex> lock(context_mutex_);
                if (context_ != nullptr) {
                    context_->TryCancel();
                }
            }
            cv_.notify_all();
            if (worker_.joinable()) {
                worker_.join();
            }
        }

        bool done() const {
            return done_;
        }

        int32_t request_id() const {
            return request_id_;
        }

    private:
        GrpcCommand* owner_;
        unsigned long socket_id_;
        int32_t request_id_;
        std::atomic<bool> stop_requested_{false};
        std::atomic<bool> done_{false};
        std::thread worker_;
        std::mutex context_mutex_;
        std::unique_ptr<grpc::ClientContext> context_{nullptr};
        std::mutex cv_mutex_;
        std::condition_variable cv_;

        void run() try {
            auto context = std::make_unique<grpc::ClientContext>();
            owner_->build_command_context(context.get(), socket_id_);
            {
                std::unique_lock<std::mutex> lock(context_mutex_);
                context_ = std::move(context);
                // Re-check under the same mutex stop() takes: a stop that ran
                // before context_ was published found nothing to TryCancel, so
                // it must be honored here. Otherwise the RPC below would start
                // uncancellable, and its deadline-less Finish() could block on
                // an unresponsive collector while stop() hangs in join(),
                // stalling the whole agent shutdown.
                if (stop_requested_.load()) {
                    context_.reset();
                    done_ = true;
                    return;
                }
            }

            google::protobuf::Empty reply;
            auto writer = owner_->command_stub_->CommandStreamActiveThreadCount(context_.get(), &reply);
            if (writer == nullptr) {
                LOG_WARN("failed to start active thread count stream: writer is null");
                {
                    std::unique_lock<std::mutex> lock(context_mutex_);
                    context_.reset();
                }
                done_ = true;
                return;
            }

            int32_t sequence_id = 0;
            while (!stop_requested_ && !owner_->agent_->isExiting()) {
                v1::PCmdActiveThreadCountRes response;
                owner_->build_active_thread_count_response(&response, request_id_, ++sequence_id);
                if (!writer->Write(response)) {
                    LOG_INFO("active thread count stream write failed: requestId={}, socketId={}", request_id_, socket_id_);
                    break;
                }

                std::unique_lock<std::mutex> lock(cv_mutex_);
                cv_.wait_for(lock, owner_->tuning_.active_thread_count_flush_interval, [this] {
                    return stop_requested_.load() || owner_->agent_->isExiting();
                });
            }

            writer->WritesDone();
            const auto status = writer->Finish();
            if (!status.ok() && !stop_requested_ && !owner_->agent_->isExiting()) {
                LOG_INFO("active thread count stream closed: {}, {}",
                         static_cast<int>(status.error_code()), status.error_message());
            }
            {
                std::unique_lock<std::mutex> lock(context_mutex_);
                context_.reset();
            }
            done_ = true;
        } catch (const std::exception& e) {
            {
                std::unique_lock<std::mutex> lock(context_mutex_);
                if (context_) {
                    // The sync ClientWriter was destroyed by the unwind
                    // without WritesDone()/Finish(); cancel the still-active
                    // RPC before releasing its context so gRPC tears the
                    // call down cleanly.
                    context_->TryCancel();
                }
                context_.reset();
            }
            LOG_ERROR("active thread count stream exception = {}", e.what());
            done_ = true;
        } catch (...) {
            {
                std::unique_lock<std::mutex> lock(context_mutex_);
                if (context_) {
                    context_->TryCancel();
                }
                context_.reset();
            }
            LOG_ERROR("active thread count stream unknown exception");
            done_ = true;
        }
    };

    GrpcCommand::GrpcCommand(std::shared_ptr<const Config> config, const GrpcClientTuning& tuning)
        : GrpcClient(AGENT, std::move(config), tuning) {
        register_default_handlers();
    }

    void GrpcCommand::create_stub() {
        set_command_stub(v1::ProfilerCommandService::NewStub(channel_));
    }

    GrpcCommand::~GrpcCommand() {
        stopCommandWorker();
    }

    void GrpcCommand::register_default_handlers() {
        dispatcher_.registerHandler(static_cast<int32_t>(v1::ECHO),
            [this](const v1::PCmdRequest& request,
                   grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream) {
                return handle_echo(request, stream);
            });
        dispatcher_.registerHandler(static_cast<int32_t>(v1::ACTIVE_THREAD_COUNT),
            [this](const v1::PCmdRequest& request,
                   grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream) {
                return handle_active_thread_count(request, stream);
            });
    }

    void GrpcCommand::build_command_context(grpc::ClientContext* context, unsigned long socket_id) const {
        build_grpc_context(context, socket_id);
    }

    bool GrpcCommand::write_fail_message(
            const v1::PCmdRequest& request,
            grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream,
            std::string_view message) const {
        if (stream == nullptr) {
            return false;
        }

        v1::PCmdMessage fail_message;
        auto* fail = fail_message.mutable_failmessage();
        fail->set_responseid(request.requestid());
        fail->mutable_message()->set_value(std::string(message));
        return stream->Write(fail_message);
    }

    bool GrpcCommand::handle_echo(
            const v1::PCmdRequest& request,
            grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream) {
        if (!request.has_commandecho()) {
            return write_fail_message(request, stream, "invalid echo command");
        }

        grpc::ClientContext ctx;
        google::protobuf::Empty reply;
        v1::PCmdEchoResponse response;

        build_command_context(&ctx, 0);
        set_request_deadline(ctx);

        response.mutable_commonresponse()->set_responseid(request.requestid());
        response.set_message(request.commandecho().message());

        const auto status = command_stub_->CommandEcho(&ctx, response, &reply);
        if (!status.ok()) {
            LOG_INFO("command echo response failed: {}, {}",
                     static_cast<int>(status.error_code()), status.error_message());
            return write_fail_message(request, stream, status.error_message());
        }
        return true;
    }

    bool GrpcCommand::handle_active_thread_count(
            const v1::PCmdRequest& request,
            grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream) {
        if (!request.has_commandactivethreadcount()) {
            return write_fail_message(request, stream, "invalid active thread count command");
        }

        if (!add_active_thread_count_stream(request.requestid())) {
            return write_fail_message(request, stream, "too many active thread count streams");
        }
        return true;
    }

    void GrpcCommand::build_active_thread_count_response(
            v1::PCmdActiveThreadCountRes* response,
            int32_t request_id,
            int32_t sequence_id) const {
        const auto now = to_milli_seconds(std::chrono::system_clock::now());
        int32_t active_requests[4]{0, 0, 0, 0};
        agent_->getAgentStats().collectActiveRequests(active_requests, now);

        auto* header = response->mutable_commonstreamresponse();
        header->set_responseid(request_id);
        header->set_sequenceid(sequence_id);
        response->set_histogramschematype(ACTIVE_TRACE_HISTOGRAM_SCHEMA_TYPE);
        response->set_timestamp(now);

        for (const auto count : active_requests) {
            response->add_activethreadcount(count);
        }
    }

    bool GrpcCommand::add_active_thread_count_stream(int32_t request_id) {
        std::unique_lock<std::mutex> lock(active_streams_mutex_);

        // The collector may re-issue the command for a request id it already
        // owns (e.g. after a command-stream reconnect); stop the old stream
        // so it is swept by the cleanup below.
        for (auto& stream : active_thread_count_streams_) {
            if (stream->request_id() == request_id) {
                stream->stop();
            }
        }
        cleanup_active_thread_count_streams();

        if (active_thread_count_streams_.size() >= tuning_.max_active_thread_count_streams) {
            LOG_WARN("reject active thread count stream: requestId={}, activeStreams={}",
                     request_id, active_thread_count_streams_.size());
            return false;
        }

        auto stream = std::make_unique<ActiveThreadCountStream>(this, ++socket_id_, request_id);
        stream->start();
        active_thread_count_streams_.push_back(std::move(stream));
        LOG_INFO("active thread count stream started: requestId={}", request_id);
        return true;
    }

    void GrpcCommand::cleanup_active_thread_count_streams() {
        active_thread_count_streams_.erase(
            std::remove_if(active_thread_count_streams_.begin(), active_thread_count_streams_.end(),
                [](const auto& stream) { return stream->done(); }),
            active_thread_count_streams_.end());
    }

    void GrpcCommand::stop_active_thread_count_streams() {
        std::vector<std::unique_ptr<ActiveThreadCountStream>> streams;
        {
            std::unique_lock<std::mutex> lock(active_streams_mutex_);
            streams.swap(active_thread_count_streams_);
        }
        for (auto& stream : streams) {
            stream->stop();
        }
    }

    void GrpcCommand::cancel_command_stream() {
        std::unique_lock<std::mutex> lock(command_worker_mutex_);
        if (command_stream_context_ != nullptr) {
            command_stream_context_->TryCancel();
        }
        command_worker_cv_.notify_all();
    }

    bool GrpcCommand::wait_reconnect_delay(std::chrono::milliseconds delay) {
        std::unique_lock<std::mutex> lock(command_worker_mutex_);
        return command_worker_cv_.wait_for(lock, delay, [this] {
            return stopping();
        });
    }

    void GrpcCommand::commandWorker() {
        // Supervise the loop body: a transient exception (e.g. one escaping a
        // command handler) must not kill the worker for the process lifetime —
        // the collector could never reach this agent again. Only a stop
        // request or agent exit ends the worker.
        while (!stopping()) {
            try {
                run_command_worker();
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("grpc command worker exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("grpc command worker unknown exception");
            }

            if (wait_reconnect_delay(tuning_.worker_restart_delay)) {
                break;
            }
        }
        stop_active_thread_count_streams();
        LOG_INFO("grpc command worker end");
    }

    void GrpcCommand::run_command_worker() {
        // Clears command_stream_context_ before the stack-allocated context it
        // points to is destroyed — including when the loop unwinds via an
        // exception — so cancel_command_stream() never calls TryCancel() on a
        // dangling pointer.
        struct StreamContextGuard {
            StreamContextGuard(GrpcCommand* self, grpc::ClientContext* context) : self_(self) {
                std::unique_lock<std::mutex> lock(self_->command_worker_mutex_);
                self_->command_stream_context_ = context;
            }
            ~StreamContextGuard() {
                std::unique_lock<std::mutex> lock(self_->command_worker_mutex_);
                self_->command_stream_context_ = nullptr;
            }
            GrpcCommand* self_;
        };

        // readyChannel() backs off only while the channel itself is down. A
        // fixed short delay here would churn once a second forever when the
        // channel is READY but the collector terminates the stream
        // immediately (auth/api-key rejection, version mismatch, port
        // pointing at a non-command service) — two INFO lines per second for
        // the process lifetime. Back off exponentially instead, resetting
        // once the stream demonstrably works (a command is read).
        ExponentialBackoff reconnect_backoff{tuning_};

        while (!stopping()) {
            if (!readyChannel()) {
                break;
            }

            grpc::ClientContext context;
            build_command_context(&context, ++socket_id_);
            const auto command_codes = dispatcher_.supportedCommandCodes();
            context.AddMetadata(METADATA_SUPPORT_COMMAND_CODE, support_command_code_header(command_codes));

            const StreamContextGuard context_guard(this, &context);

            // Re-check under the publish: a stopCommandWorker() that ran
            // between the loop condition above and the guard publishing the
            // context found nothing to TryCancel, so it must be honored here.
            // Otherwise the RPC below would start uncancellable, and its
            // deadline-less Finish() could block on an unresponsive peer
            // while agent shutdown hangs in the unconditional worker join —
            // the same window the active-thread-count stream closes after
            // publishing its own context.
            if (stopping()) {
                break;
            }

            LOG_INFO("connect to command service stream");
            auto stream = command_stub_->HandleCommandV2(&context);
            if (stream == nullptr) {
                LOG_WARN("failed to connect to command service stream: stream is null");
            } else {
                v1::PCmdRequest request;
                while (!stopping() && stream->Read(&request)) {
                    // The stream demonstrably works: reconnect promptly if it
                    // later drops (e.g. collector restart).
                    reconnect_backoff.reset();
                    LOG_DEBUG("received command request: requestId={}, commandCode={}",
                              request.requestid(), command_code(request));
                    const auto handled = dispatcher_.handle(request, stream.get());
                    request.Clear();
                    if (!handled) {
                        LOG_INFO("command stream write failed while handling request");
                        break;
                    }
                }
                stream->WritesDone();
                const auto status = stream->Finish();
                if (!status.ok() && !stopping()) {
                    LOG_INFO("command service stream closed: {}, {}",
                             static_cast<int>(status.error_code()), status.error_message());
                } else {
                    LOG_INFO("command service stream completed");
                }
            }

            {
                std::unique_lock<std::mutex> lock(active_streams_mutex_);
                cleanup_active_thread_count_streams();
            }

            if (stopping() || wait_reconnect_delay(reconnect_backoff.next_delay())) {
                break;
            }
        }
    }

    void GrpcCommand::stopCommandWorker() {
        request_stop();
        {
            // Pairs the stop with wait_reconnect_delay()'s predicate so a
            // worker sleeping out a reconnect delay wakes immediately.
            std::unique_lock<std::mutex> lock(command_worker_mutex_);
            command_worker_cv_.notify_all();
        }
        cancel_command_stream();
        stop_active_thread_count_streams();
    }

    //GrpcAgent

    GrpcAgent::GrpcAgent(std::shared_ptr<const Config> config, const GrpcClientTuning& tuning)
        : GrpcClient(AGENT, std::move(config), tuning) {}

    void GrpcAgent::create_stub() {
        set_agent_stub(v1::Agent::NewStub(channel_));
    }

    GrpcAgent::~GrpcAgent() {
        stopAgentInfo();
    }

    void GrpcAgent::setServerMetaData(std::string_view server_info,
                                      const std::vector<std::string>& args,
                                      const std::vector<std::string>& libs) {
        server_meta_data_.server_info = std::string(server_info);
        server_meta_data_.vm_args = args;
        server_meta_data_.service_libs = libs;
        server_meta_data_set_ = true;
    }

    void GrpcAgent::build_agent_info(v1::PAgentInfo* agent_info, google::protobuf::Arena* arena) const {
        agent_info->set_hostname(get_host_name());
        agent_info->set_ip(get_host_ip_addr());
        agent_info->set_servicetype(agent_->getAppType());
        agent_info->set_pid(getpid());
        agent_info->set_agentversion(VERSION_STRING);
        agent_info->set_container(config_->is_container);

        const auto meta_data = google::protobuf::Arena::Create<v1::PServerMetaData>(arena);
        if (server_meta_data_set_) {
            meta_data->set_serverinfo(server_meta_data_.server_info);
            for (const auto& arg : server_meta_data_.vm_args) {
                meta_data->add_vmarg(arg);
            }
            if (!server_meta_data_.service_libs.empty()) {
                auto* service_info = meta_data->add_serviceinfo();
                service_info->set_servicename("Libraries");
                for (const auto& lib : server_meta_data_.service_libs) {
                    service_info->add_servicelib(lib);
                }
            }
        } else {
            meta_data->set_serverinfo("C/C++ Application");
        }

        auto* config_service_info = meta_data->add_serviceinfo();
        config_service_info->set_servicename("Pinpoint Agent");
        for (const auto& config_string : to_non_default_config_strings(*config_)) {
            config_service_info->add_servicelib(config_string);
        }

        agent_info->unsafe_arena_set_allocated_servermetadata(meta_data);
    }

    GrpcRequestStatus GrpcAgent::registerAgent() {
        grpc::ClientContext ctx;
        v1::PResult reply;

        // Deliberately NOT under channel_mutex_: agent_stub_ is created by
        // openChannel() before any AgentInfo send runs and gRPC stubs are
        // thread-safe. Holding the mutex across this blocking unary call would
        // stall the ping worker's readyChannel() for up to the request
        // deadline — and conversely readyChannel()'s unbounded backoff loop
        // would park this thread on the mutex for a whole collector outage,
        // uninterruptible by stopAgentInfo().
        build_grpc_context(&ctx, 0);

        google::protobuf::Arena arena;
        auto* agent_info = google::protobuf::Arena::Create<v1::PAgentInfo>(&arena);
        build_agent_info(agent_info, &arena);

        set_request_deadline(ctx);
        const grpc::Status status = agent_stub_->RequestAgentInfo(&ctx, *agent_info, &reply);

        if (status.ok()) {
            LOG_INFO("success to register the agent");  
            return SEND_OK;
        }

        LOG_ERROR("failed to register the agent: {}, {}", static_cast<int>(status.error_code()), status.error_message());
        return SEND_FAIL;
    }

    bool GrpcAgent::registerAgentWithRetry() {
        // Boot-phase registration: keep trying until the collector accepts
        // the AgentInfo. Supervised per attempt — a transient exception (e.g.
        // bad_alloc while building AgentInfo under memory pressure) is
        // retried like a failed send, so the agent still comes up once the
        // condition clears. Only stopAgentInfo() or agent exit ends the loop.
        const auto retry_interval = std::chrono::milliseconds(config_->collector.agent_info.send_retry_interval_ms);
        // Jittered, non-escalating delay (multiplier 1.0 keeps the base
        // interval): sibling pre-fork workers start simultaneously, and an
        // identical fixed retry interval would send every worker's
        // registration to the collector in lockstep for the whole outage.
        // Constructed here — in the worker's init thread, always post-fork —
        // so each worker seeds its own jitter sequence.
        ExponentialBackoff retry_backoff(retry_interval, 1.0,
                                         tuning_.reconnect_randomization_factor,
                                         retry_interval * 2);
        while (!agent_->isExiting()) {
            try {
                if (send_agent_info_once()) {
                    return true;
                }
            } catch (const std::exception& e) {
                LOG_ERROR("register agent exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("register agent unknown exception");
            }
            if (wait_agent_info_retry(retry_backoff.next_delay())) {
                return false;
            }
        }
        return false;
    }

    void GrpcAgent::startAgentInfo() {
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        if (agent_info_running_) {
            return;
        }
        // Stop is sticky once shutdown began: the async init thread may reach
        // here after the shutdown path already ran requestStopAgentInfo(), and
        // it must not clear the stop request and spawn a worker nobody will
        // join.
        if (agent_ != nullptr && agent_->isExiting()) {
            return;
        }
        agent_info_stop_requested_ = false;
        agent_info_running_ = true;
        agent_info_thread_ = std::thread{&GrpcAgent::agent_info_worker, this};
    }

    void GrpcAgent::requestStopAgentInfo() {
        // Set and signal even when no scheduler thread is running: the
        // boot-phase registerAgentWithRetry() (on the agent's init thread)
        // waits on this cv too and must wake promptly.
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        agent_info_stop_requested_ = true;
        agent_info_cv_.notify_all();
    }

    void GrpcAgent::stopAgentInfo() {
        {
            std::unique_lock<std::mutex> lock(agent_info_mutex_);
            // Re-signal here so stopAgentInfo() remains complete on its own:
            // callers other than the shutdown path may not have gone through
            // requestStopAgentInfo() first.
            agent_info_stop_requested_ = true;
            agent_info_cv_.notify_all();
            if (!agent_info_running_ && !agent_info_thread_.joinable()) {
                return;
            }
        }
        if (agent_info_thread_.joinable()) {
            agent_info_thread_.join();
        }
        {
            std::unique_lock<std::mutex> lock(agent_info_mutex_);
            agent_info_running_ = false;
        }
        LOG_INFO("AgentInfo scheduler stopped");
    }

    bool GrpcAgent::should_stop_agent_info() const {
        return agent_info_stop_requested_ || agent_->isExiting();
    }

    bool GrpcAgent::send_agent_info_once() {
        if (agent_->isExiting()) {
            return false;
        }
        const auto status = registerAgent();
        if (status == SEND_OK) {
            LOG_INFO("AgentInfo sent");
            return true;
        }
        LOG_WARN("failed to send AgentInfo");
        return false;
    }

    bool GrpcAgent::send_agent_info_with_retries(const int max_try_count) {
        const auto retry_interval = std::chrono::milliseconds(config_->collector.agent_info.send_retry_interval_ms);
        for (int try_count = 0; try_count < max_try_count; ++try_count) {
            if (agent_->isExiting()) {
                return false;
            }
            if (send_agent_info_once()) {
                return true;
            }
            if (try_count + 1 < max_try_count && wait_agent_info_retry(retry_interval)) {
                return false;
            }
        }
        return false;
    }

    bool GrpcAgent::wait_agent_info_retry(std::chrono::milliseconds delay) {
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        agent_info_cv_.wait_for(lock, delay, [this] { return should_stop_agent_info(); });
        return should_stop_agent_info();
    }

    bool GrpcAgent::wait_agent_info_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        agent_info_cv_.wait_until(lock, deadline, [this] { return should_stop_agent_info(); });
        return should_stop_agent_info();
    }

    void GrpcAgent::agent_info_worker() {
        // Supervise the loop body like the other grpc workers: a transient
        // exception (e.g. bad_alloc while building AgentInfo under memory
        // pressure) must not kill the periodic re-send scheduler for the
        // process lifetime. Only a stop request or agent exit ends the worker.
        while (true) {
            try {
                run_agent_info_worker();
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("AgentInfo scheduler exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("AgentInfo scheduler unknown exception");
            }

            // Pace crash restarts before re-entering the periodic loop.
            std::unique_lock<std::mutex> lock(agent_info_mutex_);
            if (agent_info_cv_.wait_for(lock, tuning_.worker_restart_delay, [this] {
                    return should_stop_agent_info();
                })) {
                break;
            }
        }
    }

    void GrpcAgent::run_agent_info_worker() {
        // Boot-time registration already succeeded before this scheduler was
        // started (init_grpc_workers blocks on registerAgentWithRetry()), so
        // this loop only re-sends AgentInfo each refresh interval. Post-boot
        // sends are best-effort: a failed cycle simply waits for the next
        // interval and never affects the agent's enabled state.
        const auto refresh_interval = std::chrono::milliseconds(config_->collector.agent_info.refresh_interval_ms);
        auto next_refresh = std::chrono::steady_clock::now() + refresh_interval;
        while (true) {
            if (wait_agent_info_until(next_refresh)) {
                return;
            }

            send_agent_info_with_retries(config_->collector.agent_info.max_try_per_attempt);
            next_refresh = std::chrono::steady_clock::now() + refresh_interval;
        }
    }
    // Ping Stream

    bool GrpcAgent::start_ping_stream() {
        LOG_DEBUG("start_ping_stream");
        if (!readyChannel()) {
            return false;
        }

        // Build the context fully before publishing it: everything that can
        // throw runs while stream_context_/grpc_status_ still describe the
        // previous (finished) stream, so an exception here never leaves the
        // "live call" state (context set, status != STREAM_DONE) without an
        // actual call — drain_ping_stream_on_error() relies on that invariant.
        auto context = std::make_unique<grpc::ClientContext>();
        build_grpc_context(context.get(), ++socket_id_);

        {
            // Publish the new context under stream_mutex_ so stopPingWorker()
            // never observes a context mid-replacement when it cancels it.
            std::unique_lock<std::mutex> lock(stream_mutex_);
            stream_context_ = std::move(context);
            grpc_status_ = STREAM_CONTINUE;
        }
        // The published "live call" state above assumes the start calls below
        // cannot fail. Should one throw anyway (e.g. bad_alloc under memory
        // pressure), no OnDone will ever arrive for this reactor, so restore
        // STREAM_DONE before unwinding — otherwise finish_ping_stream() and
        // drain_ping_stream_on_error() would wait forever for it and hang
        // shutdown. Rethrow rather than return false: the worker loops treat
        // false as "stopping" and exit for the process lifetime, while the
        // supervisor (sendPingWorker) retries a thrown transient failure
        // after the worker restart delay.
        try {
            agent_stub_->async()->PingSession(stream_context_.get(), this);

            ping_stream_closing_ = false;

            AddHold();
            StartRead(&pong_);
            StartCall();
        } catch (...) {
            {
                std::unique_lock<std::mutex> lock(stream_mutex_);
                grpc_status_ = STREAM_DONE;
            }
            LOG_ERROR("start_ping_stream failed to launch the ping call");
            throw;
        }

        return true;
    }

    void GrpcAgent::close_ping_stream() {
        // stream_mutex_ makes the closing-flag check + StartWrite in
        // write_and_await_ping_stream() mutually exclusive with the
        // set-flag + StartWritesDone here; without it a reactor-thread close
        // can interleave with the worker's StartWrite, violating the gRPC
        // callback-API ordering contract (StartWrite after StartWritesDone).
        std::unique_lock<std::mutex> lock(stream_mutex_);
        close_ping_stream_locked();
    }

    void GrpcAgent::close_ping_stream_locked() {
        if (!ping_stream_closing_.exchange(true)) {
            StartWritesDone();
            RemoveHold();
        }
    }

    void GrpcAgent::finish_ping_stream() {
        LOG_DEBUG("finish_ping_stream");

        std::unique_lock<std::mutex> lock(stream_mutex_);
        close_ping_stream_locked();
        if (!stream_cv_.wait_for(lock, tuning_.stream_finish_timeout,
                                 [this] { return grpc_status_ == STREAM_DONE; })) {
            LOG_INFO("ping stream did not finish in time, cancelling");
            if (stream_context_ != nullptr) {
                stream_context_->TryCancel();
            }
            // TryCancel is best-effort. The reactor still must not be
            // abandoned while the call is outstanding, so wait for OnDone.
            stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
        }
    }

    void GrpcAgent::drain_ping_stream_on_error() noexcept try {
        // Last-resort cleanup when sendPingWorker() unwinds via an exception
        // with a ping stream possibly still live. This object IS the stream's
        // reactor, so a call left in flight would deliver OnDone into a
        // destroyed GrpcAgent once the client is torn down. Close and cancel
        // the call, then wait for OnDone before the worker exits.
        std::unique_lock<std::mutex> lock(stream_mutex_);
        if (stream_context_ == nullptr || grpc_status_ == STREAM_DONE) {
            // No call in flight: either no stream was ever started, or the
            // last one already delivered OnDone (start_ping_stream() publishes
            // the live-call state only after the call cannot fail to start).
            return;
        }
        close_ping_stream_locked();
        stream_context_->TryCancel();
        stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
    } catch (...) {
    }

    GrpcStreamStatus GrpcAgent::write_and_await_ping_stream() {
        LOG_DEBUG("write_and_await_ping_stream");
        std::unique_lock<std::mutex> lock(stream_mutex_);

        if (ping_stream_closing_) {
            // The stream broke while idle. StartWrite() is not allowed after
            // StartWritesDone(), so just wait for OnDone; the old call must
            // also fully finish before the caller replaces stream_context_.
            stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
            return STREAM_DONE;
        }

        grpc_status_ = STREAM_WRITE;
        StartWrite(&ping_);
        if (!stream_cv_.wait_for(lock, tuning_.stream_write_timeout,
                                 [this] { return grpc_status_ != STREAM_WRITE; })) {
            // The transport can stay "healthy" (an intermediary satisfies
            // HTTP/2 keepalive) while the collector backend never answers the
            // ping. An untimed wait would park this worker for the process
            // lifetime — pings stop and the stream never cycles to a healthy
            // backend. Cancel so the caller rebuilds the stream instead.
            LOG_WARN("ping response timed out, recycling ping stream");
            if (stream_context_ != nullptr) {
                stream_context_->TryCancel();
            }
            // TryCancel is best-effort. The reactor still must not be
            // abandoned while the call is outstanding, so wait for OnDone.
            stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
        }

        if (grpc_status_ == STREAM_DONE && !stream_status_.ok()) {
            LOG_ERROR("failed to send ping: {}, {}",
                     static_cast<int>(stream_status_.error_code()), stream_status_.error_message());
        }

        return grpc_status_;
    }

    //PingReactor

    void GrpcAgent::OnWriteDone(bool ok) {
        LOG_DEBUG("ping - OnWriteDone : {}", ok);

        if (!ok) {
            close_ping_stream();
        }
    }

    void GrpcAgent::OnReadDone(bool ok) {
        LOG_DEBUG("ping - OnReadDone : {}", ok);

        if (ok) {
            StartRead(&pong_);

            std::unique_lock<std::mutex> lock(stream_mutex_);
            grpc_status_ = STREAM_CONTINUE;
            stream_cv_.notify_one();
        } else {
            // A failed read means the stream is broken. Release the hold so
            // OnDone can be delivered; otherwise the ping worker waits on
            // stream_cv_ forever and shutdown hangs on join.
            close_ping_stream();
        }
    }

    void GrpcAgent::OnDone(const grpc::Status& status) {
        LOG_DEBUG("ping - OnDone : {}", static_cast<int>(status.error_code()));

        std::unique_lock<std::mutex> lock(stream_mutex_);
        stream_status_ = status;
        grpc_status_ = STREAM_DONE;
        stream_cv_.notify_one();
    }

    void GrpcAgent::sendPingWorker() {
        // Supervise the loop body: a transient exception must not kill the
        // worker for the process lifetime — without pings the collector marks
        // the agent dead. The stream is drained on error and a fresh one is
        // started on restart. Only a stop request or agent exit ends the
        // worker.
        while (true) {
            try {
                if (run_ping_worker()) {
                    break;
                }
                // A stream start failed without a stop request: fall through
                // to the restart delay and retry with a fresh stream, exactly
                // like the exception path below.
            } catch (const std::exception& e) {
                LOG_ERROR("grpc ping worker exception = {}", e.what());
                drain_ping_stream_on_error();
            } catch (...) {
                LOG_ERROR("grpc ping worker unknown exception");
                drain_ping_stream_on_error();
            }

            std::unique_lock<std::mutex> lock(ping_worker_mutex_);
            if (ping_cv_.wait_for(lock, tuning_.worker_restart_delay, [this] {
                    return ping_stop_requested_ || agent_->isExiting();
                })) {
                break;
            }
        }
        LOG_INFO("grpc ping worker end");
    }

    bool GrpcAgent::run_ping_worker() {
        if (!start_ping_stream()) {
            return false;
        }

        std::unique_lock<std::mutex> lock(ping_worker_mutex_);

        while (true) {
            lock.unlock();
            if (write_and_await_ping_stream() == STREAM_DONE) {
                if (!start_ping_stream()) {
                    return false;
                }
            }

            lock.lock();
            if (ping_cv_.wait_for(lock, tuning_.ping_interval, [this]{ return ping_stop_requested_ || agent_->isExiting(); })) {
                lock.unlock();
                finish_ping_stream();
                return true;
            }
        }
    }

    void GrpcAgent::stopPingWorker() {
        request_stop();
        {
            std::unique_lock<std::mutex> lock(ping_worker_mutex_);
            ping_stop_requested_ = true;
            ping_cv_.notify_one();
        }
        // The worker may be blocked in write_and_await_ping_stream() waiting
        // for a server pong that never arrives; only stream activity can wake
        // that wait. Request cancellation to prompt OnDone, which sets
        // STREAM_DONE; TryCancel itself provides no hard completion bound.
        std::unique_lock<std::mutex> lock(stream_mutex_);
        if (stream_context_ != nullptr && grpc_status_ != STREAM_DONE) {
            stream_context_->TryCancel();
        }
    }


    //GrpcSpan

    // Heap-resident state for a single async SendSpanBatch call. Lives as
    // long as the callback's shared_ptr keeps it alive.
    struct PendingSpanBatch {
        grpc::ClientContext ctx;
        google::protobuf::Arena arena;
        v1::PSpanMessageBatch* request{nullptr};
        v1::PSpanResultBatch reply;
    };

    // Permit accounting and in-flight call registry shared between GrpcSpan
    // and its async completion callbacks. The callbacks capture this state by
    // shared_ptr — never GrpcSpan's `this` — so a callback delivered after the
    // client (or the whole agent) is destroyed only touches live heap memory.
    struct SpanBatchInflight {
        std::mutex mutex;
        std::condition_variable cv;
        int permits{0};
        int max_permits{0};
        // Completion order is not observed. Key by call identity so callbacks
        // can remove their registry entry without a linear scan or shifts.
        std::unordered_set<std::shared_ptr<PendingSpanBatch>> pending;

        void completeCall(const std::shared_ptr<PendingSpanBatch>& call) {
            bool released = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                released = pending.erase(call) == 1;
                if (released) {
                    ++permits;
                }
            }
            if (!released) {
                LOG_WARN("SendSpanBatch completion ignored: call is not registered as in-flight");
                return;
            }
            cv.notify_one();
        }
    };

    GrpcSpan::GrpcSpan(std::shared_ptr<const Config> config, const GrpcClientTuning& tuning)
        : GrpcClient(SPAN, config, tuning), span_queue_(config->span.queue_size),
          span_drop_reporter_(tuning.span_queue_drop_log_interval) {
        inflight_ = std::make_shared<SpanBatchInflight>();
        inflight_->max_permits = config_->collector.span_batch.max_concurrent_requests;
        inflight_->permits = inflight_->max_permits;
    }

    void GrpcSpan::create_stub() {
        set_span_stub(v1::Span::NewStub(channel_));
    }

    bool GrpcSpan::try_acquire_permit(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(inflight_->mutex);
        if (!inflight_->cv.wait_for(lock, timeout, [this]{ return inflight_->permits > 0; })) {
            return false;
        }
        --inflight_->permits;
        return true;
    }

    bool GrpcSpan::try_acquire_all_permits(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(inflight_->mutex);
        return inflight_->cv.wait_for(lock, timeout, [this]{
            return inflight_->permits >= inflight_->max_permits;
        });
    }

    void GrpcSpan::release_permit() {
        {
            std::lock_guard<std::mutex> lock(inflight_->mutex);
            ++inflight_->permits;
        }
        inflight_->cv.notify_one();
    }

    void GrpcSpan::enqueueSpan(std::unique_ptr<SpanChunk> span) noexcept try {
        // Null guard mirrors enqueueMeta: a null chunk would pass through the
        // queue and crash the worker at getSpanData().
        if (span == nullptr || stop_requested_.load(std::memory_order_relaxed) ||
                (agent_ != nullptr && agent_->isExiting())) {
            return;
        }

        // Each service thread is mapped to a stable shard. Head-drop and insert
        // complete in one short shard-local critical section with no allocation
        // and no process-wide counter write on the hot path.
        span_queue_.enqueue(span);
        notify_span_worker();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue span: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to enqueue span: unknown exception");
    }

    void GrpcSpan::notify_span_worker() {
        // The fast path never takes the wait mutex. If the worker has announced
        // that it is about to sleep, pairing the notify with the same mutex used
        // by wait_dequeue_until closes the empty-check / sleep lost-wakeup gap.
        if (!span_consumer_waiting_.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lock(span_wait_mutex_);
        if (span_consumer_waiting_.load(std::memory_order_relaxed)) {
            span_queue_cv_.notify_one();
        }
    }

    bool GrpcSpan::wait_dequeue_until(
            std::unique_ptr<SpanChunk>& span,
            std::chrono::steady_clock::time_point deadline) {
        if (span_queue_.try_dequeue(span)) {
            return true;
        }

        std::unique_lock<std::mutex> lock(span_wait_mutex_);
        span_consumer_waiting_.store(true, std::memory_order_release);
        struct WaitingFlagReset {
            std::atomic<bool>& flag;
            ~WaitingFlagReset() { flag.store(false, std::memory_order_release); }
        } reset{span_consumer_waiting_};

        while (true) {
            // Double-check after publishing the waiting flag while holding the
            // wait mutex. An enqueue that raced the first empty check either is
            // visible here or must take this mutex before notifying.
            if (span_queue_.try_dequeue(span)) {
                return true;
            }
            if (stopping() || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            span_queue_cv_.wait_until(lock, deadline);
        }
    }

    void GrpcSpan::collect_batch(std::vector<std::unique_ptr<SpanChunk>>& buffer) {
        const auto& batch_cfg = config_->collector.span_batch;
        const auto flush_timeout = std::chrono::milliseconds(batch_cfg.flush_interval_ms);
        const auto collect_deadline_ms = std::chrono::milliseconds(batch_cfg.collect_deadline_ms);
        const auto batch_size = static_cast<size_t>(batch_cfg.size);

        std::unique_ptr<SpanChunk> span;
        // Drop-report latency is bounded without any deadline tweaking here:
        // this first wait returns at the flush deadline even when idle, and
        // the worker polls maybe_log_span_queue_drops() every cycle.
        const auto first_wait_deadline = std::chrono::steady_clock::now() + flush_timeout;
        if (!wait_dequeue_until(span, first_wait_deadline)) {
            return;
        }
        buffer.push_back(std::move(span));

        // Gather more items until either the batch is full or the collect
        // deadline elapses. Matches Java SpanBatchGrpcDataSender.collectBatch.
        const auto deadline = std::chrono::steady_clock::now() + collect_deadline_ms;
        while (buffer.size() < batch_size) {
            // Drain everything that is already published before paying the
            // cost of another sleep/wakeup round trip. The batch call locks
            // each active shard at most once, where dequeuing one item at a
            // time pays a lock per probed shard per item — O(active_shards)
            // mutex ops per item when a skewed producer load concentrates
            // the backlog in one shard.
            span_queue_.try_dequeue_batch(buffer, batch_size - buffer.size());
            if (buffer.size() >= batch_size || stopping() ||
                    std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            if (!wait_dequeue_until(span, deadline)) {
                break;
            }
            buffer.push_back(std::move(span));
        }
        LOG_DEBUG("collect_batch: collected={} batch_size_limit={}", buffer.size(), batch_size);
    }

    void GrpcSpan::maybe_log_span_queue_drops() {
        // Polled once per worker cycle (collect_batch's flush deadline bounds
        // the poll gap), feeding the queue's own cumulative drop counter into
        // the shared rate-limited reporter: producers already count
        // overwritten-oldest drops under their shard locks, so the enqueue
        // hot path pays no extra clock read or logging. WARN like the other
        // queue-overflow reports (metadata/stats/url-stat).
        if (const auto dropped = span_drop_reporter_.report_if_due(span_queue_.dropped_oldest())) {
            LOG_WARN("span queue overflow: {} dropped in total (oldest overwritten, max queue size {})",
                     dropped, config_->span.queue_size);
        }
    }

    void GrpcSpan::send_batch_async(std::vector<std::unique_ptr<SpanChunk>>& batch) try {
        if (batch.empty()) {
            return;
        }

        // Reserve an in-flight slot before serializing anything. When the
        // pipeline is saturated the batch is dropped without paying the
        // (potentially large) protobuf build cost, and backpressure engages
        // one batch sooner.
        const auto flush_timeout = std::chrono::milliseconds(config_->collector.span_batch.flush_interval_ms);
        if (!try_acquire_permit(flush_timeout)) {
            LOG_INFO("SendSpanBatch skipped: no available permits within {}ms",
                     flush_timeout.count());
            batch.clear();
            return;
        }

        // The permit is now held. Every path from here until the async call is
        // launched must release it; once launched, the completion callback owns
        // the release. The catch below must be catch-all: a non-std exception
        // escaping this window would leak the permit permanently, shrinking
        // the in-flight pipeline for the rest of the process lifetime.
        std::shared_ptr<PendingSpanBatch> pending;
        bool registered = false;
        const auto release_unlaunched = [this, &batch, &pending, &registered]() {
            batch.clear();
            // The call was never launched: hand the permit back, but exactly
            // once. Release only if this batch still owns the permit — either it
            // was never registered (so no completion callback exists for it), or
            // it is still in the registry (the callback has not completed it). If
            // it was registered and is already gone, completeCall() released the
            // permit, and a second ++permits here would permanently inflate the
            // concurrency cap.
            {
                std::lock_guard<std::mutex> lock(inflight_->mutex);
                if (!registered || inflight_->pending.erase(pending) == 1) {
                    ++inflight_->permits;
                }
            }
            inflight_->cv.notify_one();
        };
        try {
            pending = std::make_shared<PendingSpanBatch>();
            pending->request = google::protobuf::Arena::Create<v1::PSpanMessageBatch>(&pending->arena);

            for (auto& span_chunk : batch) {
                const auto& span = span_chunk->getSpanData();
                auto* msg = pending->request->add_span();
                if (!span_chunk->isFinal() || span->isAsyncSpan()) {
                    msg->unsafe_arena_set_allocated_spanchunk(build_grpc_span_chunk(std::move(span_chunk), &pending->arena));
                } else {
                    msg->unsafe_arena_set_allocated_span(build_grpc_span(std::move(span_chunk), &pending->arena));
                }
            }
            batch.clear();

            build_grpc_context(&pending->ctx, 0);
            set_request_deadline(pending->ctx);

            const int batch_count = pending->request->span_size();
            {
                std::lock_guard<std::mutex> lock(inflight_->mutex);
                inflight_->pending.insert(pending);
                registered = true;
                LOG_DEBUG("SendSpanBatch sending: batchSize={} concurrentRequests={}/{}",
                          batch_count, inflight_->max_permits - inflight_->permits, inflight_->max_permits);
            }

            auto* ctx_ptr = &pending->ctx;
            auto* request_ptr = pending->request;
            auto* reply_ptr = &pending->reply;
            // Captures the shared in-flight state, never `this`: the callback
            // may fire after this GrpcSpan (or the whole agent) is destroyed.
            auto state = inflight_;
            span_stub_->async()->SendSpanBatch(ctx_ptr, request_ptr, reply_ptr,
                [state, pending, batch_count](const grpc::Status& status) {
                    state->completeCall(pending);
                    if (!status.ok()) {
                        LOG_INFO("SendSpanBatch failed: {}, {}",
                                 static_cast<int>(status.error_code()), status.error_message());
                        return;
                    }
                    LOG_DEBUG("SendSpanBatch success: batchSize={}", batch_count);
                    if (!pending->reply.has_partial_success()) {
                        return;
                    }
                    const auto& ps = pending->reply.partial_success();
                    if (ps.rejected_spans() > 0) {
                        LOG_WARN("SendSpanBatch partial success: rejectedSpans={}, errorId={}, errorMessage={}",
                                 ps.rejected_spans(), ps.errorid(), ps.error_message());
                    } else if (!ps.error_message().empty()) {
                        LOG_INFO("SendSpanBatch warning: errorId={}, {}",
                                 ps.errorid(), ps.error_message());
                    }
                });
        } catch (const std::exception& e) {
            LOG_INFO("SendSpanBatch failed synchronously: exception = {}", e.what());
            release_unlaunched();
        } catch (...) {
            LOG_INFO("SendSpanBatch failed synchronously: unknown exception");
            release_unlaunched();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("failed to build span batch: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to build span batch: unknown exception");
    }

    void GrpcSpan::await_in_flight_requests() {
        if (try_acquire_all_permits(tuning_.span_shutdown_await_timeout)) {
            return;
        }

        // Slow collector: request cancellation for whatever is still in
        // flight. TryCancel is best-effort, but when honored it avoids waiting
        // out the full request deadline for callbacks to return their permits.
        std::vector<std::shared_ptr<PendingSpanBatch>> stragglers;
        {
            std::lock_guard<std::mutex> lock(inflight_->mutex);
            stragglers.assign(inflight_->pending.begin(), inflight_->pending.end());
        }
        LOG_WARN("timed out waiting for in-flight span requests; cancelling {} request(s)",
                 stragglers.size());
        for (const auto& call : stragglers) {
            call->ctx.TryCancel();
        }

        if (!try_acquire_all_permits(tuning_.span_shutdown_await_timeout)) {
            // Even now the callbacks stay memory-safe: they reference only the
            // shared in-flight state, never this client or the agent.
            LOG_WARN("in-flight span requests still pending after cancellation");
        }
    }

    void GrpcSpan::flush_remaining(std::vector<std::unique_ptr<SpanChunk>>& remaining) {
        // Reserve before removing anything so an allocation failure leaves the
        // queue intact instead of losing a chunk between dequeue and push_back.
        remaining.reserve(remaining.size() + span_queue_.capacity());
        std::unique_ptr<SpanChunk> span;
        while (span_queue_.try_dequeue_after_stop(span)) {
            remaining.push_back(std::move(span));
        }
        if (!remaining.empty()) {
            // readyChannel() refuses to wait once the agent is exiting, so probe
            // the channel state directly and send only over a live connection.
            // channel_ is null if the agent was never brought online via Start()
            // (openChannel() opens it), in which case there is nothing to flush to.
            const auto channel = channel_;
            if (channel && channel->GetState(false) == GRPC_CHANNEL_READY) {
                LOG_INFO("flushing {} remaining spans on shutdown", remaining.size());
                const auto batch_size = std::max<size_t>(1, static_cast<size_t>(config_->collector.span_batch.size));
                std::vector<std::unique_ptr<SpanChunk>> batch;
                batch.reserve(std::min(batch_size, remaining.size()));
                for (auto& chunk : remaining) {
                    batch.push_back(std::move(chunk));
                    if (batch.size() >= batch_size) {
                        send_batch_async(batch);
                    }
                }
                send_batch_async(batch);
            } else {
                LOG_INFO("drop {} remaining spans on shutdown: channel not ready", remaining.size());
            }
            remaining.clear();
        }
        await_in_flight_requests();
    }

    void GrpcSpan::sendSpanWorker() {
        // Supervise the loop body: a transient exception (e.g. bad_alloc
        // while serializing a batch) drops that batch but must not kill the
        // worker for the process lifetime — no spans would ever be reported
        // again. Only agent exit ends the worker, and the shutdown flush runs
        // on every path, including exception unwinds that previously skipped
        // it and silently dropped the queued chunks.
        std::vector<std::unique_ptr<SpanChunk>> pending_batch;
        while (!stopping()) {
            try {
                run_span_worker(pending_batch);
                break;
            } catch (const std::exception& e) {
                LOG_ERROR("grpc span worker exception = {}", e.what());
            } catch (...) {
                LOG_ERROR("grpc span worker unknown exception");
            }

            if (!stopping()) {
                // run_span_worker previously owned its batch as a local, so an
                // exception dropped those chunks before the supervisor restart.
                pending_batch.clear();
            }
            std::unique_lock<std::mutex> lock(span_wait_mutex_);
            span_queue_cv_.wait_for(lock, tuning_.worker_restart_delay, [this] {
                return stopping();
            });
        }

        try {
            flush_remaining(pending_batch);
        } catch (const std::exception& e) {
            LOG_ERROR("grpc span worker flush exception = {}", e.what());
        } catch (...) {
            LOG_ERROR("grpc span worker flush unknown exception");
        }
        LOG_INFO("grpc span worker end");
    }

    void GrpcSpan::run_span_worker(std::vector<std::unique_ptr<SpanChunk>>& batch) {
        batch.reserve(static_cast<size_t>(config_->collector.span_batch.size));
        while (!stopping()) {
            batch.clear();

            collect_batch(batch);
            maybe_log_span_queue_drops();
            if (batch.empty()) {
                continue;
            }

            if (readyChannel()) {
                send_batch_async(batch);
            } else if (stopping()) {
                // readyChannel() refuses to wait once the agent is exiting,
                // but these chunks were already collected out of the queue.
                // Keep the worker-owned batch out of the bounded queue and pass
                // it directly to flush_remaining(). Re-enqueueing can fail if
                // producers filled the freed slots during channel shutdown.
                return;
            } else {
                // Preserve the existing outage policy: a batch collected while
                // the channel cannot become ready is not retried indefinitely.
                batch.clear();
            }
        }
    }

    void GrpcSpan::stopSpanWorker() {
        request_stop();
        std::lock_guard<std::mutex> lock(span_wait_mutex_);
        span_queue_cv_.notify_all();
    }

    //GrpcStat

    GrpcStats::GrpcStats(std::shared_ptr<const Config> config, const GrpcClientTuning& tuning)
        : GrpcClient(STATS, std::move(config), tuning) {}

    void GrpcStats::create_stub() {
        set_stats_stub(v1::Stat::NewStub(channel_));
    }

    void GrpcStats::on_slow_channel_recovery(std::chrono::seconds elapsed) {
        force_stats_queue_empty_.store(true, std::memory_order_release);
        LOG_DEBUG("stats queue marked stale after slow channel recovery: {}s", elapsed.count());
    }

    bool GrpcStats::start_stats_stream() {
        LOG_DEBUG("start_stats_stream");
        if (!readyChannel()) {
            return false;
        }

        // Build the context fully before publishing it: everything that can
        // throw runs while stream_context_/grpc_status_ still describe the
        // previous (finished) stream, so an exception here never leaves the
        // "live call" state (context set, status != STREAM_DONE) without an
        // actual call — drain_stats_stream_on_error() relies on that invariant.
        auto context = std::make_unique<grpc::ClientContext>();
        build_grpc_context(context.get(), 0);

        {
            // Publish the new context under stream_mutex_ so stopStatsWorker()
            // never observes a context mid-replacement when it cancels it.
            // Also clears a stale STREAM_DONE left by the previous session so
            // finish_stats_stream() waits for this stream's own OnDone.
            std::unique_lock<std::mutex> lock(stream_mutex_);
            stream_context_ = std::move(context);
            grpc_status_ = STREAM_CONTINUE;
        }

        // See start_ping_stream(): if a start call below throws, no OnDone will
        // arrive for this reactor, so restore STREAM_DONE to keep
        // finish_stats_stream()/drain_stats_stream_on_error() from hanging.
        // Rethrow rather than return false: the worker loops treat false as
        // "stopping" and exit for the process lifetime, while the supervisor
        // (sendStatsWorker) retries a thrown transient failure after the
        // worker restart delay.
        try {
            stats_stub_->async()->SendAgentStat(stream_context_.get(), &reply_, this);

            stats_stream_closing_ = false;

            AddHold();
            StartCall();
        } catch (...) {
            {
                std::unique_lock<std::mutex> lock(stream_mutex_);
                grpc_status_ = STREAM_DONE;
            }
            LOG_ERROR("start_stats_stream failed to launch the stats call");
            throw;
        }
        return true;
    }

    GrpcStreamStatus GrpcStats::write_and_await_stats_stream() {
        LOG_DEBUG("write_and_await_stats_stream");

        std::unique_lock<std::mutex> lock(stream_mutex_);
        // Drain the queue from this worker thread: each iteration builds one
        // message (next_write), writes it and waits for the completion. The
        // message construction deliberately stays here — OnWriteDone only
        // reports the outcome — so protobuf building (snapshot copies, up to
        // a full URL-stat table) never runs on gRPC's shared callback threads.
        while (true) {
            grpc_status_ = next_write();
            if (grpc_status_ != STREAM_WRITE) {
                return grpc_status_;
            }
            StartWrite(msg_);

            if (!stream_cv_.wait_for(lock, tuning_.stream_write_timeout,
                                     [this] { return grpc_status_ != STREAM_WRITE; })) {
                // The transport can stay "healthy" (an intermediary satisfies
                // HTTP/2 keepalive) while the collector backend stops reading,
                // leaving the write blocked on flow control. An untimed wait
                // would park this worker for the process lifetime and make
                // shutdown hang on join. Cancel so the caller rebuilds the
                // stream instead.
                LOG_WARN("stats write timed out, recycling stats stream");
                // Close before cancelling: unlike the ping stream (whose
                // re-armed read always surfaces the cancellation), a
                // write-only stream has no pending op once a racing
                // OnWriteDone(true) slips in, so the hold must be released
                // here or OnDone could be withheld forever.
                close_stats_stream_locked();
                if (stream_context_ != nullptr) {
                    stream_context_->TryCancel();
                }
                // TryCancel is best-effort. With the hold released, keep the
                // reactor alive and wait for the call's eventual OnDone.
                stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
            }

            if (grpc_status_ == STREAM_DONE) {
                if (!stream_status_.ok()) {
                    LOG_ERROR("failed to send stats: {}, {}",
                              static_cast<int>(stream_status_.error_code()),
                              stream_status_.error_message());
                }
                return STREAM_DONE;
            }
            // STREAM_CONTINUE: the write completed; loop for the next payload.
        }
    }

    void GrpcStats::close_stats_stream_locked() {
        // Mirrors close_ping_stream_locked(): whichever of the write-failure /
        // finish paths runs first performs the single allowed
        // StartWritesDone()/RemoveHold() pair for this stream session.
        if (!stats_stream_closing_.exchange(true)) {
            StartWritesDone();
            RemoveHold();
        }
    }

    void GrpcStats::finish_stats_stream() {
        LOG_DEBUG("finish_stats_stream");

        std::unique_lock<std::mutex> lock(stream_mutex_);
        close_stats_stream_locked();
        if (!stream_cv_.wait_for(lock, tuning_.stream_finish_timeout,
                                 [this] { return grpc_status_ == STREAM_DONE; })) {
            LOG_INFO("stats stream did not finish in time, cancelling");
            if (stream_context_ != nullptr) {
                stream_context_->TryCancel();
            }
            // TryCancel is best-effort. The reactor still must not be
            // abandoned while the call is outstanding, so wait for OnDone.
            stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
        }
    }

    void GrpcStats::drain_stats_stream_on_error() noexcept try {
        // Last-resort cleanup when sendStatsWorker() unwinds via an exception
        // with a stats stream possibly still live. This object IS the stream's
        // reactor, so a call left in flight would deliver OnDone into a
        // destroyed GrpcStats once the client is torn down. Close and cancel
        // the call, then wait for OnDone before the worker exits.
        std::unique_lock<std::mutex> lock(stream_mutex_);
        if (stream_context_ == nullptr || grpc_status_ == STREAM_DONE) {
            return;
        }
        close_stats_stream_locked();
        stream_context_->TryCancel();
        stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
    } catch (...) {
    }

    void GrpcStats::OnWriteDone(bool ok) {
        LOG_DEBUG("stats - OnWriteDone: {}", ok);
        arena_.Reset(); // reset arena after write completes to free all memory at once

        if (ok) {
            // Only record the completion and wake the worker; the next
            // message is built and written by the worker loop in
            // write_and_await_stats_stream(), keeping protobuf construction
            // off gRPC's callback threads. The guard also means a completion
            // racing a write-timeout recycle (stream already closed) cannot
            // clobber a later state.
            std::unique_lock<std::mutex> lock(stream_mutex_);
            if (grpc_status_ == STREAM_WRITE) {
                grpc_status_ = STREAM_CONTINUE;
            }
            stream_cv_.notify_one();
        } else {
            // The write failed: the stream is broken. Close it (at most once
            // per session) so the hold is released and OnDone can be
            // delivered. stream_mutex_ keeps this mutually exclusive with the
            // worker's StartWrite, upholding the gRPC callback-API ordering
            // contract (no StartWrite after StartWritesDone).
            std::unique_lock<std::mutex> lock(stream_mutex_);
            close_stats_stream_locked();
        }
    }

    void GrpcStats::OnDone(const grpc::Status& status) {
        LOG_DEBUG("stats - OnDone: {}", static_cast<int>(status.error_code()));

        std::unique_lock<std::mutex> lock(stream_mutex_);
        stream_status_ = status;
        grpc_status_ = STREAM_DONE;
        stream_cv_.notify_one();
    }

    GrpcStreamStatus GrpcStats::next_write() try {
        LOG_DEBUG("stats - next_write");
        // should be hold stream_mutex_

        StatsType stats;
        {
            std::unique_lock<std::mutex> lock(stats_queue_mutex_);
            if (stats_stop_requested_ || agent_->isExiting() || stats_queue_.empty()) {
                LOG_DEBUG("stats - queue empty");
                return STREAM_CONTINUE;
            }

            stats = stats_queue_.front();
            stats_queue_.pop();
        }

        msg_ = google::protobuf::Arena::Create<v1::PStatMessage>(&arena_);
        if (stats == AGENT_STATS) {
            msg_->unsafe_arena_set_allocated_agentstatbatch(build_agent_stat_batch(agent_->getAgentStats().copySnapshots(), &arena_));
        } else {
            auto snapshot = agent_->getUrlStats().takeSnapshot();
            msg_->unsafe_arena_set_allocated_agenturistat(build_url_stat(snapshot.get(), &arena_));
        }

        return STREAM_WRITE;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to send stats: exception = {}", e.what());
        // No StartWrite was issued for this msg_, so OnWriteDone will not run to
        // reset the arena. Reset here so a failed build does not accumulate
        // allocations across consecutive failures (compounding memory pressure
        // in exactly the OOM scenario that triggers this path).
        arena_.Reset();
        return STREAM_EXCEPTION;
    }

    void GrpcStats::enqueueStats(const StatsType stats) noexcept try {
        const auto& config = config_;
        if (!config->stat.enable && !config->http.url_stat.enable) {
            return;
        }

        bool dropped_now = false;
        {
            std::unique_lock<std::mutex> lock(stats_queue_mutex_);

            if (stats_queue_.size() < tuning_.max_stats_queue_size) {
                stats_queue_.push(stats);
            } else {
                force_stats_queue_empty_.store(true, std::memory_order_release);
                dropped_now = true;
            }
        }

        // Reported outside the lock, at WARN so outage data loss is visible
        // at the default log level, rate-limited so a stalled stream cannot
        // repeat it every collect interval for hours.
        if (dropped_now) {
            if (const auto dropped = stats_drop_reporter_.record()) {
                LOG_WARN("stats queue overflow: {} dropped in total (max queue size {})",
                         dropped, tuning_.max_stats_queue_size);
            }
        }

        // Notify after releasing the lock so the woken worker does not
        // immediately block on stats_queue_mutex_ (matches enqueueSpan).
        stats_queue_cv_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue stats: exception = {}", e.what());
    } catch (...) {
        LOG_ERROR("failed to enqueue stats: unknown exception");
    }

    void GrpcStats::empty_stats_queue() noexcept try {
        std::queue<StatsType> temp_queue;
        
        {
            std::unique_lock<std::mutex> lock(stats_queue_mutex_);
            
            stats_queue_.swap(temp_queue);
        }
        
        agent_->getAgentStats().initAgentStats();
        // clear URL stat snapshot
        (void)agent_->getUrlStats().takeSnapshot();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to empty stats queue: exception = {}", e.what());
    }

    bool GrpcStats::empty_stats_queue_if_requested() noexcept try {
        if (!force_stats_queue_empty_.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }

        empty_stats_queue();
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to empty stats queue if requested: exception = {}", e.what());
        return false;
    } catch (...) {
        LOG_ERROR("failed to empty stats queue if requested: unknown exception");
        return false;
    }

    void GrpcStats::sendStatsWorker() {
        // Boot-time decision: both flags are non-reloadable (see
        // Config::retainNonReloadableFrom and the invariant note in
        // UrlStats::addUrlStatsWorker), so a worker that returns here can
        // never be needed later. config_ is the pinned boot snapshot, so
        // stopStatsWorker's identical gate always agrees with this one.
        const auto& config = config_;
        if (!config->stat.enable && !config->http.url_stat.enable) {
            return;
        }

        // Supervise the loop body: a transient exception (e.g. bad_alloc
        // while building a stats message) must not kill the worker for the
        // process lifetime — stats would never be reported again. The stream
        // is drained on error and a fresh one is started on restart. Only a
        // stop request or agent exit ends the worker.
        while (true) {
            try {
                if (run_stats_worker()) {
                    break;
                }
                // A stream start failed without a stop request: fall through
                // to the restart delay and retry with a fresh stream, exactly
                // like the exception path below.
            } catch (const std::exception& e) {
                LOG_ERROR("grpc stats worker: exception = {}", e.what());
                drain_stats_stream_on_error();
            } catch (...) {
                LOG_ERROR("grpc stats worker: unknown exception");
                drain_stats_stream_on_error();
            }

            std::unique_lock<std::mutex> lock(stats_queue_mutex_);
            if (stats_queue_cv_.wait_for(lock, tuning_.worker_restart_delay, [this] {
                    return stats_stop_requested_ || agent_->isExiting();
                })) {
                break;
            }
        }

        LOG_INFO("grpc stats worker end");
    }

    bool GrpcStats::run_stats_worker() {
        if (!start_stats_stream()) {
            return false;
        }

        std::unique_lock<std::mutex> lock(stats_queue_mutex_);
        while (true) {
            stats_queue_cv_.wait(lock, [this]{
                return !stats_queue_.empty() || stats_stop_requested_ || agent_->isExiting();
            });
            const bool stopping = stats_stop_requested_ || agent_->isExiting();
            lock.unlock();

            if (stopping) {
                finish_stats_stream();
                return true;
            }

            if (write_and_await_stats_stream() == STREAM_DONE) {
                if (!start_stats_stream()) {
                    return false;
                }
            }
            // The staleness flag is also set by a queue overflow while the
            // stream stays up; consume it every cycle so it cannot linger
            // and purge fresh stats at an unrelated reconnect much later.
            empty_stats_queue_if_requested();

            lock.lock();
        }
    }

    void GrpcStats::stopStatsWorker() {
        const auto& config = config_;
        if (!config->stat.enable && !config->http.url_stat.enable) {
            return;
        }

        request_stop();
        {
            std::unique_lock<std::mutex> lock(stats_queue_mutex_);
            stats_stop_requested_ = true;
            stats_queue_cv_.notify_one();
        }
        // The worker may be blocked in write_and_await_stats_stream() waiting
        // for a write completion that a stalled collector never delivers; only
        // stream activity can wake that wait. Request cancellation to prompt
        // the pending operation and OnDone; TryCancel itself provides no hard
        // completion bound. Taken after releasing
        // stats_queue_mutex_: the worker acquires stats_queue_mutex_ while
        // holding stream_mutex_ (next_write), so nesting them here in the
        // opposite order would risk deadlock.
        std::unique_lock<std::mutex> lock(stream_mutex_);
        if (stream_context_ != nullptr && grpc_status_ != STREAM_DONE) {
            stream_context_->TryCancel();
        }
    }
}
