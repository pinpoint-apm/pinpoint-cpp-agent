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
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <unistd.h>
#include <grpcpp/client_context.h>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
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

        // Only keepalive and message-size limits are set here; the rest of
        // the HTTP/2 tuning the Java (DefaultChannelFactory.setupClientOption)
        // and Go (grpc.go dialOptions) agents configure is deliberately left
        // at the gRPC C-core defaults, which are already better or equal:
        //
        // - Flow control window: unset means BDP probing stays on
        //   (chttp2_transport.cc reads GRPC_ARG_HTTP2_BDP_PROBE with
        //   value_or(true)) and TransportFlowControl::PeriodicUpdate() sizes
        //   the receive window from the measured bandwidth-delay product,
        //   max(4MB, 2*BDP) under low memory pressure. Java calls
        //   NettyChannelBuilder.flowControlWindow(1MiB), which sets
        //   autoFlowControl=false, and Go calls WithInitialWindowSize /
        //   WithInitialConnWindowSize(1MiB), which set StaticWindowSize=true so
        //   no bdpEstimator is created (http2_client.go): both run a fixed 1MiB
        //   window with auto-tuning off. GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES
        //   would not give parity either: it only seeds the target that
        //   PeriodicUpdate() then overwrites on every BDP ping.
        // - Max header list size: Java/Go set 8KB for inbound metadata. The
        //   C-core default is already an 8KB soft / 16KB hard limit
        //   (metadata_info.h); setting GRPC_ARG_MAX_METADATA_SIZE=8KB would only
        //   shrink the hard limit to 10KB.
        // - Write buffer: GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE applies only to
        //   writes flagged GRPC_WRITE_BUFFER_HINT, which this agent never sets,
        //   so it would be a no-op. Go's WithWriteBufferSize (socket write
        //   batching) and Java's WRITE_BUFFER_WATER_MARK (Netty writability)
        //   have no C-core counterpart.
        // - Connect timeout: no C-core equivalent of Netty's
        //   CONNECT_TIMEOUT_MILLIS; the closest, GRPC_ARG_MIN_RECONNECT_BACKOFF_MS
        //   (doubles as the min connect timeout in subchannel.cc), is not
        //   needed because readyChannel() + ExponentialBackoff already manage
        //   reconnects. TCP_NODELAY is always on in the C-core posix engine.
        grpc::ChannelArguments make_channel_arguments(const Config::GrpcChannelOptions& options,
                                                      bool private_connection) {
            grpc::ChannelArguments channel_args;

            channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, options.keepalive_time_ms);
            channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, options.keepalive_timeout_ms);
            channel_args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,
                                options.keepalive_permit_without_calls ? 1 : 0);
            channel_args.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, options.max_send_message_size);
            channel_args.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, options.max_receive_message_size);

            // Channel rotation exists to land on a different backend, which
            // takes a new TCP connection. By default C-core shares
            // subchannels (connections) between channels with the same
            // target and arguments through a process-wide pool, so a rotation
            // successor would simply reuse the connection of the channel it
            // replaces — and the agent/metadata/command channels to the agent
            // port already ride one shared connection. A per-channel pool
            // gives each channel, and therefore each successor, its own
            // connection; the old one closes once the previous transport's
            // last snapshot is released. Left at the default while rotation
            // is off so existing deployments keep their connection count.
            //
            // The exception is the successor of a stall-forced rotation
            // (GrpcClient::record_stream_stall), which needs its own
            // connection whatever the configuration: in the shared pool it
            // would reuse the very connection the stalled backend sits on
            // and the rotation would be a no-op. Making the local pool
            // unconditional instead would cost every deployment two extra
            // connections to the agent port; a config flag for the escalation
            // would leave the stall loop in place for everyone who has not
            // heard of the flag. Paying the extra connection only from the
            // moment a stall actually forces a rotation keeps the default
            // count unchanged until the feature is needed. The connection the
            // stalled channel shared with the metadata/command channels stays
            // theirs — they carry no stall signal, same as in Java.
            if (options.channel_max_age_ms > 0 || private_connection) {
                channel_args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
            }

            return channel_args;
        }

        std::shared_ptr<grpc::Channel> build_channel(const Config& config, ClientType client_type,
                                                     bool private_connection = false) {
            const auto& options = config.collector.grpc.channel;
            const auto client_name = grpc_client_name(client_type);
            const auto addr = absl::StrCat(config.collector.host, ":", grpc_collector_port(config, client_type));
            auto credentials = make_channel_credentials(config.collector.grpc.ssl, client_name);
            auto channel_args = make_channel_arguments(options, private_connection);

            LOG_INFO("create {} grpc channel: addr={}, ssl={}, keepaliveTimeMs={}, keepaliveTimeoutMs={}, "
                     "maxSendMessageSize={}, maxReceiveMessageSize={}, channelMaxAgeMs={}, streamMaxAgeMs={}, "
                     "privateConnection={}",
                     client_name, addr, config.collector.grpc.ssl.enable, options.keepalive_time_ms,
                     options.keepalive_timeout_ms, options.max_send_message_size,
                     options.max_receive_message_size, options.channel_max_age_ms,
                     options.stream_max_age_ms, options.channel_max_age_ms > 0 || private_connection);
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

    // Java: IntervalFunction.ofRandomized(x, 0.1) for both renewal periods.
    constexpr double kRenewalJitterFactor = 0.1;

    std::chrono::milliseconds randomize_interval(std::chrono::milliseconds interval, double factor,
                                                 std::mt19937_64& rng) {
        const auto jitter = std::max(0.0, factor);
        if (interval.count() <= 0 || jitter <= 0.0) {
            return interval;
        }
        std::uniform_real_distribution<double> distribution(1.0 - jitter, 1.0 + jitter);
        const auto scaled = static_cast<double>(interval.count()) * distribution(rng);
        return std::chrono::milliseconds(
            std::max<std::chrono::milliseconds::rep>(1, std::llround(scaled)));
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
        // Idempotent on the channel, not the transport: a test-injected bare
        // stub (null channel) still lets this build the channel and run
        // create_stub(), exactly as before the transport bundling.
        if (const auto transport = transport_.load(); transport && transport->channel) {
            return;
        }
        create_stub(build_channel(*config_, client_type_));
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
                        int64_t start_time, int32_t app_type) {
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
        return headers;
    }

    void GrpcClient::build_grpc_context(grpc::ClientContext* context, unsigned long socket_id) const {
        assert(agent_ != nullptr && "setAgentService() must be called before build_grpc_context()");
        // The socket-id-independent headers are immutable once the agent is
        // started, and this runs for every unary request — build once, reuse.
        // call_once because some clients build contexts from more than one
        // thread (GrpcAgent: ping + AgentInfo; GrpcCommand: command worker +
        // active-thread-count streams).
        std::call_once(grpc_metadata_once_, [this] {
            grpc_metadata_cache_ = build_grpc_metadata(*config_, agent_->getAgentId(),
                                                       agent_->getStartTime(), agent_->getAppType());
        });
        for (const auto& [key, value] : grpc_metadata_cache_) {
            context->AddMetadata(key, value);
        }
        if (socket_id > 0) {
            context->AddMetadata(METADATA_SOCKET_ID, std::to_string(socket_id));
        }
    }

    bool GrpcClient::wait_channel_ready(grpc::Channel& channel, std::chrono::milliseconds delay) const {
        auto state = channel.GetState(true);
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
            channel.WaitForStateChange(state, std::chrono::system_clock::now() + wait_for);
            state = channel.GetState(false);
        }

        return state == GRPC_CHANNEL_READY;
    }

    bool GrpcClient::readyChannel() {
        std::unique_lock<std::mutex> lock(channel_mutex_);
        if (stopping()) {
            return false;
        }

        // Every worker passes here right before it uses its stub, so this is
        // the cycle boundary where an aged channel gets replaced. A no-op
        // unless rotation is configured and due.
        rotate_channel_if_due();

        const auto transport = transport_.load();
        if (!transport || !transport->channel) {
            // Never opened (or a test injected a bare stub), or closed.
            return false;
        }
        auto& channel = *transport->channel;

        if (channel.GetState(false) != GRPC_CHANNEL_READY) {
            while (true) {
                if (stopping()) {
                    return false;
                }
                const auto delay = channel_ready_backoff_.next_delay();
                LOG_INFO("{} grpc channel is not ready; retry for {}ms", client_name_, delay.count());
                if (wait_channel_ready(channel, delay)) {
                    channel_ready_backoff_.reset();
                    break;
                }
            }
        } else {
            channel_ready_backoff_.reset();
        }
        return true;
    }

    void GrpcClient::arm_channel_rotation(std::chrono::steady_clock::time_point from) {
        const auto max_age = std::chrono::milliseconds(config_->collector.grpc.channel.channel_max_age_ms);
        channel_rotate_at_ = max_age.count() > 0
            ? from + randomize_interval(max_age, kRenewalJitterFactor, jitter_rng_)
            : std::chrono::steady_clock::time_point::max();
    }

    void GrpcClient::record_stream_stall() {
        if (stream_stall_count_++ == 0) {
            first_stream_stall_at_ = std::chrono::steady_clock::now();
        }
    }

    bool GrpcClient::stream_stall_limit_reached(std::chrono::steady_clock::time_point now) const {
        return tuning_.stream_stall_limit_count > 0 &&
               stream_stall_count_ >= tuning_.stream_stall_limit_count &&
               now - first_stream_stall_at_ >= tuning_.stream_stall_limit_time;
    }

    void GrpcClient::rotate_channel_if_due() noexcept try {
        // Runs under channel_mutex_ (readyChannel), on the client's worker.
        const auto now = std::chrono::steady_clock::now();
        const bool stalled = stream_stall_limit_reached(now);
        if ((!stalled && now < channel_rotate_at_) || stopping()) {
            return;
        }
        const auto current = transport_.load();
        if (!current || !current->channel) {
            return;  // closed, or a test-injected bare stub: nothing to renew
        }
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - current->created_at).count();
        if (stalled) {
            LOG_WARN("{} grpc channel #{}: {} stream write timeouts in {}ms without a completed write; "
                     "connecting a successor", client_name_, current->generation, stream_stall_count_,
                     std::chrono::duration_cast<std::chrono::milliseconds>(now - first_stream_stall_at_).count());
            // This attempt consumes the recorded stalls whatever its outcome.
            // A successor that becomes READY deserves a clean slate; one that
            // does not (collector down, or a proxy admitting no further
            // connections) must not turn every following write timeout into
            // another channel_rotation_ready_timeout wait — with the counter
            // kept, each recycled stream would force a rotation at once and
            // the worker would spend its time building successors. Reset,
            // the next attempt needs the same evidence as this one: another
            // limit_count stalls spanning limit_time.
            stream_stall_count_ = 0;
        } else {
            LOG_INFO("{} grpc channel #{} is {}ms old (max age {}ms): connecting a successor",
                     client_name_, current->generation, age_ms,
                     config_->collector.grpc.channel.channel_max_age_ms);
        }

        // Make-before-break: the successor must be READY before it replaces
        // anything, and it is not required that the current channel be READY
        // — when the collector just came back, moving to the successor is the
        // faster recovery. wait_channel_ready() checks stopping() every
        // slice, so a shutdown starting during the wait abandons the rotation.
        // A stall-forced successor gets its own connection even while
        // rotation is off (see make_channel_arguments).
        auto successor = build_channel(*config_, client_type_, stalled);
        const bool ready = wait_channel_ready(*successor, tuning_.channel_rotation_ready_timeout);
        const auto ready_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - now).count();
        if (!ready || stopping()) {
            // Keep the working channel; the successor dies with this scope.
            // Retry one jittered period from now rather than in a tight loop.
            LOG_WARN("{} grpc channel rotation abandoned: successor not ready after {}ms (state {}); "
                     "keeping channel #{}", client_name_, ready_ms,
                     static_cast<int>(successor->GetState(false)), current->generation);
            arm_channel_rotation(std::chrono::steady_clock::now());
            return;
        }
        // Publishes the successor with its stub in one store and re-arms the
        // next rotation. This client's reference to the previous transport
        // ends here; calls still running on it hold their own snapshots.
        create_stub(successor);
        LOG_INFO("{} grpc channel #{} replaced by #{}: old age {}ms, successor ready in {}ms",
                 client_name_, current->generation, transport_generation_, age_ms, ready_ms);
    } catch (const std::exception& e) {
        LOG_ERROR("{} grpc channel rotation failed: {}; keeping the current channel", client_name_, e.what());
        arm_channel_rotation(std::chrono::steady_clock::now());
    } catch (...) {
        LOG_ERROR("{} grpc channel rotation failed: unknown exception; keeping the current channel", client_name_);
        arm_channel_rotation(std::chrono::steady_clock::now());
    }

    std::chrono::milliseconds GrpcClient::next_stream_max_age() {
        const auto max_age = std::chrono::milliseconds(config_->collector.grpc.channel.stream_max_age_ms);
        if (max_age.count() <= 0) {
            return std::chrono::milliseconds::zero();
        }
        return randomize_interval(max_age, kRenewalJitterFactor, jitter_rng_);
    }

    std::chrono::milliseconds GrpcClient::next_stream_renewal_delay() {
        auto delay = next_stream_max_age();
        if (channel_rotate_at_ == std::chrono::steady_clock::time_point::max()) {
            return delay;
        }

        // readyChannel() runs immediately before a stream is opened, so a due
        // rotation has normally just been completed (or a failed attempt has
        // been re-armed). Clamp a sub-millisecond remainder to 1ms so a valid
        // channel deadline never turns into the zero sentinel for "disabled".
        const auto remaining = channel_rotate_at_ - std::chrono::steady_clock::now();
        const auto channel_delay = std::max(
            std::chrono::milliseconds{1},
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        return delay.count() <= 0 ? channel_delay : std::min(delay, channel_delay);
    }

    void GrpcClient::arm_stream_expiry() {
        const auto renewal_delay = next_stream_renewal_delay();
        stream_expires_at_ = renewal_delay.count() > 0
            ? std::chrono::steady_clock::now() + renewal_delay
            : std::chrono::steady_clock::time_point::max();
    }

    //GrpcMetadata

    namespace {
        // Whether a failed metadata RPC is worth sending again. Go's agent
        // (grpc.go isRetryableError) retries exactly these two and nothing
        // else; every other code is a verdict the same request earns again,
        // so retrying it only burns permits and retry-schedule slots while a
        // schema mismatch or an auth failure rejects every item.
        //
        //   UNAVAILABLE       - collector restarting, LB draining, connection
        //                       dropped. The next attempt sees a new state.
        //   DEADLINE_EXCEEDED - slow collector or congested link; the item was
        //                       possibly never processed. Metadata upserts are
        //                       keyed by id and idempotent, so a duplicate
        //                       delivery is harmless.
        //
        // Deliberately not retried, each for its own reason:
        //
        //   RESOURCE_EXHAUSTED - either the message exceeds a size limit (a
        //     resend never gets smaller) or the collector is shedding load, in
        //     which case resending adds to what it is shedding — and
        //     meta_retry_delay is a fixed delay, not a backoff, so it cannot
        //     wait the overload out either.
        //   ABORTED - a concurrency/transaction conflict. Metadata writes are
        //     idempotent upserts with no such conflict to lose, and a retry
        //     would re-enter whatever conflict the collector did report.
        //   INTERNAL - a server-side bug or a broken framing/compression
        //     contract. Same request, same failure.
        //   UNKNOWN - a server exception carrying no status, or a status that
        //     could not be parsed; in Pinpoint's collector the common source
        //     is an application-level exception on the request itself, which
        //     repeats. Java's hedging config (HedgingServiceConfigBuilder
        //     DEFAULT_STATUS_CODES = UNKNOWN, UNAVAILABLE) does include it,
        //     but hedging fires a parallel attempt to beat latency rather
        //     than re-sending a verdict, so it is not evidence that a retry
        //     helps here. Go leaves it out; so do we.
        //
        // Every code left out still gets its cache entry released on the drop,
        // so the id is regenerated and re-sent from a later span once the
        // cause is gone — recovery without a retry loop.
        bool is_retryable_meta_status(grpc::StatusCode code) {
            return code == grpc::StatusCode::UNAVAILABLE ||
                   code == grpc::StatusCode::DEADLINE_EXCEEDED;
        }
    }

    // Heap-resident state for a single async metadata RPC. Lives as long as
    // the completion callback's shared_ptr keeps it alive.
    struct PendingMetaRpc {
        grpc::ClientContext ctx;
        google::protobuf::Arena arena;      // owns the request message
        v1::PResult reply;
        grpc::Status status;                // set by the completion callback
        std::unique_ptr<MetaData> meta;     // retained for retry / cache release
        int retry_count{0};
        std::string_view operation_name{};  // static literal, for logging
        // Pins the channel (and stub) this RPC was launched on until the
        // callback has run — see the stub access invariant in grpc.h.
        std::shared_ptr<const GrpcClient::Transport> transport;
    };

    // Metadata queued for (re)send.
    struct PendingMeta {
        std::unique_ptr<MetaData> meta;
        int retry_count{0};
        std::chrono::steady_clock::time_point available_at{};
    };

    // Every coordination point of the metadata pipeline under one mutex/cv:
    // producers enqueue, completion callbacks return permits and outcomes, and
    // the worker waits on the single cv for any of them. Shared with the
    // callbacks by shared_ptr — never GrpcMetadata's `this` — so a callback
    // delivered after the client is destroyed only touches live heap memory.
    struct MetaPipeline {
        std::mutex mutex;
        std::condition_variable cv;
        bool stop_requested{false};
        int permits{0};
        int max_permits{0};
        std::deque<PendingMeta> queue;
        std::multimap<std::chrono::steady_clock::time_point, PendingMeta> retry_queue;
        std::unordered_set<std::shared_ptr<PendingMetaRpc>> in_flight;
        // Outcomes drained by the worker. Double-buffered against the
        // worker's `done` vector via swap; both buffers are reserved well
        // beyond max_permits (here and in run_meta_worker), so the push_back
        // below cannot regrow in practice — the catch there still covers a
        // pathological bad_alloc.
        std::vector<std::shared_ptr<PendingMetaRpc>> completed;

        void completeCall(const std::shared_ptr<PendingMetaRpc>& call, grpc::Status status) {
            // Taken by value and moved, never copied: a copy assignment
            // duplicates the error strings and can throw between the permit
            // release and the notify_all below. gRPC's callback layer swallows
            // the escaping exception, so the unwind would skip the notify and
            // park the worker in its bare cv.wait() with a free permit and a
            // non-empty queue. Pinned here so a grpc::Status change that
            // breaks the noexcept move stops the build, not the worker.
            static_assert(std::is_nothrow_move_assignable<grpc::Status>::value &&
                              std::is_nothrow_move_constructible<grpc::Status>::value,
                          "completeCall relies on grpc::Status moves never throwing");
            bool released = false;
            bool outcome_kept = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                released = in_flight.erase(call) == 1;
                if (released) {
                    ++permits;
                    call->status = std::move(status);
                    try {
                        completed.push_back(call);
                        outcome_kept = true;
                    } catch (...) {
                        // Reported below, outside the lock.
                    }
                }
            }
            if (!released) {
                LOG_WARN("metadata completion ignored: call is not registered as in-flight");
                return;
            }
            if (!outcome_kept) {
                // The worker cannot see this outcome: no retry, and the cache
                // entry stays published (releasing it needs the agent, which
                // callbacks must not touch).
                LOG_ERROR("failed to record metadata completion; outcome dropped");
            }
            cv.notify_all();
        }
    };

    GrpcMetadata::GrpcMetadata(std::shared_ptr<const Config> config, const GrpcClientTuning& tuning)
        : GrpcClient(METADATA, std::move(config), tuning) {
        pipeline_ = std::make_shared<MetaPipeline>();
        pipeline_->max_permits = std::max(1, tuning_.meta_max_concurrent_requests);
        pipeline_->permits = pipeline_->max_permits;
        pipeline_->completed.reserve(static_cast<size_t>(pipeline_->max_permits) * 4);
    }

    void GrpcMetadata::create_stub(const std::shared_ptr<grpc::Channel>& channel) {
        set_meta_stub(v1::Metadata::NewStub(channel), channel);
    }

    void GrpcMetadata::launch_meta_rpc(std::unique_ptr<MetaData> meta, int retry_count) {
        // The caller holds one permit. Every failure path before the launch
        // must hand it back and route the item through the normal retry path;
        // once launched, the completion callback owns the release. The catch
        // must be catch-all: a non-std exception escaping here would leak the
        // permit permanently, shrinking the pipeline for the process lifetime.
        std::shared_ptr<PendingMetaRpc> call;
        bool registered = false;
        try {
            call = std::make_shared<PendingMetaRpc>();
            call->meta = std::move(meta);
            call->retry_count = retry_count;
            build_grpc_context(&call->ctx, 0);
            set_request_deadline(call->ctx);

            // Captures the shared pipeline, never `this`: the callback may
            // fire after this GrpcMetadata (or the whole agent) is destroyed.
            // The status is taken by value and moved through so completeCall
            // never copies its strings — see the no-throw contract there.
            auto state = pipeline_;
            auto on_done = [state, call](grpc::Status status) {
                state->completeCall(call, std::move(status));
            };

            // Registered before the launch: the callback (which may run
            // inline in tests) releases the permit by erasing this entry.
            {
                std::lock_guard<std::mutex> lock(pipeline_->mutex);
                pipeline_->in_flight.insert(call);
                registered = true;
            }

            // Not under channel_mutex_ (see the stub access invariant in
            // grpc.h): the stub comes from an owning transport snapshot that
            // `call` keeps until its completion callback has run, so a
            // channel rotation cannot destroy the channel this RPC is on.
            // Null only after closeChannel(); treat that like a failed launch
            // so the item takes the normal retry path.
            const auto transport = current_transport<MetaStub>();
            if (transport == nullptr) {
                throw std::runtime_error("metadata channel is closed");
            }
            call->transport = transport;
            auto* async_stub = transport->stub->async();
            std::visit([&](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ApiMeta>) {
                    call->operation_name = "api";
                    auto* request = google::protobuf::Arena::Create<v1::PApiMetaData>(&call->arena);
                    request->set_apiid(value.id_);
                    request->set_apiinfo(value.api_str_);
                    request->set_type(value.type_);
                    async_stub->RequestApiMetaData(&call->ctx, request, &call->reply, on_done);
                } else if constexpr (std::is_same_v<T, StringMeta>) {
                    if (value.type_ == STRING_META_ERROR) {
                        call->operation_name = "error";
                        auto* request = google::protobuf::Arena::Create<v1::PStringMetaData>(&call->arena);
                        request->set_stringid(value.id_);
                        request->set_stringvalue(value.str_val_);
                        async_stub->RequestStringMetaData(&call->ctx, request, &call->reply, on_done);
                    } else {
                        call->operation_name = "sql";
                        auto* request = google::protobuf::Arena::Create<v1::PSqlMetaData>(&call->arena);
                        request->set_sqlid(value.id_);
                        request->set_sql(value.str_val_);
                        async_stub->RequestSqlMetaData(&call->ctx, request, &call->reply, on_done);
                    }
                } else if constexpr (std::is_same_v<T, SqlUidMeta>) {
                    call->operation_name = "sql uid";
                    auto* request = google::protobuf::Arena::Create<v1::PSqlUidMetaData>(&call->arena);
                    request->set_sqluid(std::string(value.uid_.begin(), value.uid_.end()));
                    request->set_sql(value.sql_);
                    async_stub->RequestSqlUidMetaData(&call->ctx, request, &call->reply, on_done);
                } else if constexpr (std::is_same_v<T, ExceptionMeta>) {
                    call->operation_name = "exception";
                    auto* request = build_exception_metadata(value.txid_, value.span_id_,
                                                             value.url_template_, value.exceptions_,
                                                             &call->arena);
                    async_stub->RequestExceptionMetaData(&call->ctx, request, &call->reply, on_done);
                }
            }, *call->meta);
        } catch (const std::exception& e) {
            LOG_ERROR("metadata send threw an exception: {}", e.what());
            on_launch_failure(call, registered, std::move(meta), retry_count);
        } catch (...) {
            LOG_ERROR("metadata send threw an unknown exception");
            on_launch_failure(call, registered, std::move(meta), retry_count);
        }
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
        }, meta);
    }

    // Overflow policy: a full queue drops the NEWEST item — deliberately the
    // opposite of the span queue's head-drop. Metadata has no recency value
    // (an ApiMeta says the same thing whenever it is sent), so the span
    // argument for keeping the latest telemetry does not apply. What does
    // differ between old and new is how many recorded spans already reference
    // the id: the producer registers the id in the agent cache before
    // enqueueing, and every later cache hit reuses it without re-publishing.
    // A full queue means a stalled pipeline, so the item at the head has
    // accumulated references from every span that hit its cache entry while
    // it waited; dropping it (and releasing its entry, which any drop must
    // do — see below) orphans all of them. The newest item has been referenced
    // only by the trace that just created it, so dropping it orphans the
    // least. It also keeps the critical section to a size check and one
    // push_back, with no second item to carry out of the lock for release.
    // Java's metadata sender (GrpcDataSender: offer() failure) behaves the
    // same way.
    void GrpcMetadata::enqueueMeta(std::unique_ptr<MetaData> meta) noexcept try {
        if (meta == nullptr || (agent_ != nullptr && agent_->isExiting())) {
            return;
        }

        // Built before the critical section so that on every failure below —
        // overflow drop or a throwing queue insertion — the metadata is still
        // owned here and its cache entry can be released.
        PendingMeta pending{std::move(meta), 0, {}};
        const auto max_queue_size = static_cast<size_t>(config_->collector.grpc.channel.sender_queue_size);
        bool enqueue_threw = false;
        try {
            std::unique_lock<std::mutex> lock(pipeline_->mutex);

            // Only the new queue is charged: the retry schedule is bounded
            // separately (tuning_.meta_retry_queue_size). One shared budget
            // let a collector outage migrate items into retry_queue and
            // shrink the room for new metadata, and since every drop here
            // releases the cache entry, the same metadata came back from the
            // next span — drops feeding their own inflow.
            if (pipeline_->queue.size() < max_queue_size) {
                // deque::push_back gives the strong guarantee and PendingMeta's
                // move cannot throw, so `pending` still owns the metadata
                // whenever this block is left by exception.
                pipeline_->queue.push_back(std::move(pending));
            }
        } catch (const std::exception &e) {
            enqueue_threw = true;
            LOG_ERROR("failed to enqueue metadata: exception = {}", e.what());
        } catch (...) {
            // Catch everything, not just std::exception: unwinding past this
            // block would skip the cache release below and strand the id in
            // the agent caches for the rest of the process lifetime.
            enqueue_threw = true;
            LOG_ERROR("failed to enqueue metadata: unknown exception");
        }

        if (pending.meta != nullptr) {
            // Dropped on overflow, or never enqueued because the insertion
            // threw. Reported outside the lock, at WARN so outage data loss is
            // visible by default, rate-limited so a full queue cannot flood
            // the log. The label names this drop's cause; the count is
            // cumulative across every cause, retry-queue overflow included.
            if (const auto dropped = meta_drop_reporter_.record()) {
                LOG_WARN("metadata {}: {} dropped in total (max queue size {})",
                         enqueue_threw ? "enqueue failed" : "new queue overflow",
                         dropped, max_queue_size);
            }
            // The producer registered the id in the agent caches before
            // enqueueing (see AgentImpl::cacheApi) and a cache hit suppresses
            // re-publication, so dropping without releasing the entry would
            // leave spans referencing metadata the collector never receives.
            // Release it so the id is regenerated and re-sent, like the
            // retry-exhaustion path in retry_or_drop(). Outside the pipeline
            // mutex for the lock-order reasons documented there.
            if (agent_ != nullptr) {
                release_failed_cache(*pending.meta);
            }
            return;
        }

        // Notify after releasing the lock so the woken worker does not
        // immediately block on the pipeline mutex (matches enqueueSpan).
        pipeline_->cv.notify_all();
    } CATCH_AND_LOG("failed to enqueue metadata:")

    void GrpcMetadata::on_launch_failure(const std::shared_ptr<PendingMetaRpc>& call,
                                         bool registered,
                                         std::unique_ptr<MetaData> meta,
                                         int retry_count) {
        // Reclaim the permit exactly once: if the call was never registered
        // no callback exists for it, and if it is still registered the
        // callback has not run — remove the entry so it never will. A
        // registered-and-gone call means the callback already completed it
        // and released the permit; its outcome sits in `completed` and must
        // not be double-handled here.
        bool own_item = false;
        {
            std::lock_guard<std::mutex> lock(pipeline_->mutex);
            if (!registered || pipeline_->in_flight.erase(call) == 1) {
                ++pipeline_->permits;
                own_item = true;
            }
        }
        pipeline_->cv.notify_all();
        if (!own_item) {
            return;
        }
        // Treat a thrown build/launch exception like any other failed RPC so
        // the item is retried and, after exhaustion, its cache entry is
        // released. The status filter in process_completed does not apply
        // here: there is no reply to judge, and what throws on this path
        // (allocation, a closed channel) is the transient kind anyway.
        // The metadata is owned by the call once it was moved in;
        // before that, the `meta` parameter still owns it.
        auto owned = (call != nullptr && call->meta != nullptr) ? std::move(call->meta)
                                                                : std::move(meta);
        if (owned != nullptr) {
            retry_or_drop(std::move(owned), retry_count + 1);
        }
    }

    void GrpcMetadata::retry_or_drop(std::unique_ptr<MetaData> meta, int retry_count) {
        if (agent_->isExiting()) {
            return;
        }
        if (retry_count > tuning_.meta_retry_max_attempts) {
            LOG_INFO("drop metadata after retry exhaustion: retryCount={}", retry_count);
            // Outside the pipeline mutex: removeCache* takes the agent
            // caches' internal locks, and nesting those under the pipeline
            // mutex extends enqueueMeta contention on application threads
            // and is a latent lock-order hazard.
            release_failed_cache(*meta);
            return;
        }
        LOG_DEBUG("retry metadata send: retryCount={}/{}", retry_count, tuning_.meta_retry_max_attempts);
        PendingMeta pending{std::move(meta), retry_count,
                            std::chrono::steady_clock::now() + tuning_.meta_retry_delay};
        // Overflow policy: a full retry schedule head-drops — the opposite of
        // enqueueMeta's newest-drop, because the schedule is ordered by due
        // time and the incoming item is always the last one due (every retry
        // uses the same fixed delay). Dropping the newest would therefore
        // freeze the schedule on whatever entered it first and deny every
        // later failure a retry at all; head-dropping keeps the freshest
        // failures, which are the ones whose spans the collector is still
        // receiving, and guarantees forward progress under a sustained
        // outage. The dropped item is not lost for good: releasing its cache
        // entry (below, as every drop path must) re-registers and re-sends
        // the id on its next use, which is the recovery an unbounded,
        // memory-eating schedule buys nothing over.
        std::unique_ptr<MetaData> evicted;
        try {
            std::lock_guard<std::mutex> lock(pipeline_->mutex);
            if (!pipeline_->retry_queue.empty() &&
                pipeline_->retry_queue.size() >= tuning_.meta_retry_queue_size) {
                // Ownership moves out here and the release happens after the
                // lock, so this item is released exactly once even if the
                // emplace below throws.
                auto oldest = pipeline_->retry_queue.begin();
                evicted = std::move(oldest->second.meta);
                pipeline_->retry_queue.erase(oldest);
            }
            pipeline_->retry_queue.emplace(pending.available_at, std::move(pending));
            // No notify: this runs on the worker thread — the only waiter on
            // the pipeline cv — which re-examines the retry queue on its next
            // loop iteration.
        } catch (...) {
            // Enqueuing the retry threw. The item would otherwise be destroyed
            // with its cache id still marked published, leaving spans
            // referencing metadata the collector never receives — release the
            // entry so the id is regenerated and re-sent. The multimap
            // insertion throws from node allocation, before the element is
            // moved, so `pending.meta` is still valid.
            LOG_ERROR("failed to schedule metadata retry; releasing cache to allow re-send");
            if (pending.meta) {
                release_failed_cache(*pending.meta);
            }
        }
        if (evicted != nullptr) {
            // Same rate-limited reporter (and cumulative count) as the
            // enqueue-side drops, with its own label so an outage's retry
            // pressure is distinguishable from new-metadata pressure.
            if (const auto dropped = meta_drop_reporter_.record()) {
                LOG_WARN("metadata retry queue overflow: {} dropped in total "
                         "(oldest dropped, max retry queue size {})",
                         dropped, tuning_.meta_retry_queue_size);
            }
            // Outside the pipeline mutex, for the lock-order reason above.
            release_failed_cache(*evicted);
        }
    }

    void GrpcMetadata::process_completed(std::vector<std::shared_ptr<PendingMetaRpc>>& done) {
        // Both drop paths below release the cache entry themselves instead of
        // going through retry_or_drop: that release is the item's one and only
        // release (the retry path releases only on exhaustion), and it runs
        // outside the pipeline mutex — the worker holds no lock here — as the
        // lock order documented in retry_or_drop requires. The permit is not
        // touched: completeCall returned it before queueing the outcome.
        for (auto& call : done) {
            if (call->status.ok() && call->reply.success()) {
                LOG_DEBUG("success to send {} metadata", call->operation_name);
                continue;
            }

            if (!call->status.ok()) {
                if (!is_retryable_meta_status(call->status.error_code())) {
                    LOG_ERROR("drop {} metadata: status {} is not retryable, {}",
                              call->operation_name,
                              static_cast<int>(call->status.error_code()),
                              call->status.error_message());
                    release_failed_cache(*call->meta);
                    continue;
                }
                LOG_ERROR("failed to send {} metadata: {}, {}", call->operation_name,
                          static_cast<int>(call->status.error_code()),
                          call->status.error_message());
                retry_or_drop(std::move(call->meta), call->retry_count + 1);
                continue;
            }

            // Transport succeeded, the collector answered "no". PResult.success
            // is a verdict on the request's content (bad id, unsupported field,
            // rejected payload), and the retry would replay the same bytes for
            // the same verdict, so this is dropped like a non-retryable status
            // rather than retried. Java retries it (GrpcDataSender treats every
            // non-OK outcome alike) and Go never reads the field at all, so
            // neither agent is evidence that retrying works. Releasing the
            // cache entry keeps the recovery path: a later span re-registers
            // the id and sends a *new* request, which is the only thing that
            // can produce a different answer.
            LOG_ERROR("drop {} metadata: collector rejected it (PResult.success=false), {}",
                      call->operation_name, call->reply.message());
            release_failed_cache(*call->meta);
        }
    }

    void GrpcMetadata::await_in_flight_requests() {
        const auto wait_all = [this](std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(pipeline_->mutex);
            return pipeline_->cv.wait_for(lock, timeout, [this] {
                return pipeline_->permits >= pipeline_->max_permits;
            });
        };
        if (wait_all(tuning_.meta_shutdown_await_timeout)) {
            return;
        }

        // Slow collector: request cancellation for whatever is still in
        // flight. TryCancel is best-effort, but when honored it avoids waiting
        // out the full request deadline for callbacks to return their permits.
        std::vector<std::shared_ptr<PendingMetaRpc>> stragglers;
        {
            std::lock_guard<std::mutex> lock(pipeline_->mutex);
            stragglers.assign(pipeline_->in_flight.begin(), pipeline_->in_flight.end());
        }
        LOG_WARN("timed out waiting for in-flight metadata requests; cancelling {} request(s)",
                 stragglers.size());
        for (const auto& call : stragglers) {
            call->ctx.TryCancel();
        }

        if (!wait_all(tuning_.meta_shutdown_await_timeout)) {
            // Even now the callbacks stay memory-safe: they reference only
            // the shared pipeline state, never this client or the agent.
            LOG_WARN("in-flight metadata requests still pending after cancellation");
        }
    }

    void GrpcMetadata::sendMetaWorker() {
        // Supervised (see superviseWorker) so an unexpected exception cannot
        // kill metadata upload for the process lifetime. Exceptions from an
        // individual launch are contained by launch_meta_rpc(), which routes
        // the item through the normal retry path. Only a stop request or
        // agent exit ends the worker.
        superviseWorker("send meta worker", tuning_.worker_restart_delay,
                        pipeline_->mutex, pipeline_->cv,
                        [this] { return pipeline_->stop_requested || agent_->isExiting(); },
                        [this] { run_meta_worker(); return true; });
        // Runs on this worker thread, which the shutdown path joins under its
        // deadline — the signal phase (stopMetaWorker) stays non-blocking.
        await_in_flight_requests();
    }

    void GrpcMetadata::run_meta_worker() {
        std::vector<std::shared_ptr<PendingMetaRpc>> done;
        // The swap below hands this buffer to the pipeline as the next
        // `completed`; reserving it too keeps both halves of the double
        // buffer regrow-free, as the reserve at construction intends.
        done.reserve(static_cast<size_t>(pipeline_->max_permits) * 4);
        while (true) {
            // The inner loop exits with either completed outcomes swapped
            // into `done`, or one item popped with a permit held.
            PendingMeta item;
            {
                std::unique_lock<std::mutex> lock(pipeline_->mutex);
                while (true) {
                    if (pipeline_->stop_requested || agent_->isExiting()) {
                        return;
                    }
                    if (!pipeline_->completed.empty()) {
                        // Swap out and process outside the lock: retry
                        // scheduling and cache release must not run under the
                        // pipeline mutex.
                        done.swap(pipeline_->completed);
                        break;
                    }
                    if (pipeline_->permits > 0) {
                        if (!pipeline_->queue.empty()) {
                            item = std::move(pipeline_->queue.front());
                            pipeline_->queue.pop_front();
                            --pipeline_->permits;
                            break;
                        }
                        if (!pipeline_->retry_queue.empty() &&
                            pipeline_->retry_queue.begin()->first <= std::chrono::steady_clock::now()) {
                            auto retry = pipeline_->retry_queue.begin();
                            item = std::move(retry->second);
                            pipeline_->retry_queue.erase(retry);
                            --pipeline_->permits;
                            break;
                        }
                    }
                    // Nothing actionable. Wake on enqueue, completion or stop;
                    // when a permit is free and a retry is scheduled, also
                    // wake when it becomes due. All conditions are re-checked
                    // from the top, so a bare wait handles spurious wakeups.
                    if (pipeline_->permits > 0 && !pipeline_->retry_queue.empty()) {
                        pipeline_->cv.wait_until(lock, pipeline_->retry_queue.begin()->first);
                    } else {
                        pipeline_->cv.wait(lock);
                    }
                }
            }

            if (!done.empty()) {
                process_completed(done);
                done.clear();
                continue;
            }

            // Outside the pipeline lock: readyChannel() may block through its
            // whole reconnect backoff while completions accumulate. Must not
            // throw past this frame — the permit is held and the item owned
            // here, so an escaping exception would destroy the item without
            // releasing its cache entry AND leak the permit, permanently
            // shrinking the pipeline. Treat a throwing readiness check like an
            // unready channel: a failed attempt on the normal retry path.
            bool channel_ready = false;
            try {
                channel_ready = readyChannel();
            } catch (const std::exception& e) {
                LOG_ERROR("metadata channel readiness threw an exception: {}", e.what());
            } catch (...) {
                LOG_ERROR("metadata channel readiness threw an unknown exception");
            }
            if (!channel_ready) {
                // Channel unavailable (or stop requested): hand the permit
                // back and treat this attempt as failed, exactly like the old
                // blocking sender did, so the item re-enters the retry path
                // instead of being lost.
                {
                    std::lock_guard<std::mutex> lock(pipeline_->mutex);
                    ++pipeline_->permits;
                }
                pipeline_->cv.notify_all();
                retry_or_drop(std::move(item.meta), item.retry_count + 1);
                continue;
            }
            launch_meta_rpc(std::move(item.meta), item.retry_count);
        }
    }

    void GrpcMetadata::stopMetaWorker() {
        request_stop();
        std::unique_lock<std::mutex> lock(pipeline_->mutex);
        pipeline_->stop_requested = true;
        pipeline_->cv.notify_all();
    }

    //GrpcCommand

    namespace {
        int32_t command_code(const v1::PCmdRequest& request) {
            return static_cast<int32_t>(request.command_case());
        }

        // The commands dispatch_command() routes — keep in sync with its
        // switch. Ascending order: the support-command-code metadata header
        // lists codes sorted.
        constexpr int32_t kSupportedCommandCodes[] = {
            static_cast<int32_t>(v1::ECHO),
            static_cast<int32_t>(v1::ACTIVE_THREAD_COUNT),
        };
        static_assert(kSupportedCommandCodes[0] < kSupportedCommandCodes[1],
                      "support-command-code header lists codes in ascending order");

        std::string support_command_code_header() {
            return absl::StrJoin(kSupportedCommandCodes, ";");
        }
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

        // Signals the stream to stop without joining, so the shutdown signal
        // phase stays non-blocking; stop() performs the join.
        void request_stop() {
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
        }

        void stop() {
            request_stop();
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
            owner_->build_grpc_context(context.get(), socket_id_);
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

            // Snapshot held for the whole stream: pins the channel it runs
            // on across a command-channel rotation (stub access invariant,
            // grpc.h). Null only after closeChannel().
            const auto transport = owner_->current_transport<CommandStub>();
            google::protobuf::Empty reply;
            auto writer = transport
                ? transport->stub->CommandStreamActiveThreadCount(context_.get(), &reply)
                : nullptr;
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
        : GrpcClient(AGENT, std::move(config), tuning) {}

    void GrpcCommand::create_stub(const std::shared_ptr<grpc::Channel>& channel) {
        set_command_stub(v1::ProfilerCommandService::NewStub(channel), channel);
    }

    GrpcCommand::~GrpcCommand() {
        stopCommandWorker();
    }

    bool GrpcCommand::dispatch_command(
            const v1::PCmdRequest& request,
            grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream) {
        // Keep the cases in sync with kSupportedCommandCodes above.
        switch (command_code(request)) {
            case static_cast<int32_t>(v1::ECHO):
                return handle_echo(request, stream);
            case static_cast<int32_t>(v1::ACTIVE_THREAD_COUNT):
                return handle_active_thread_count(request, stream);
            default:
                return write_fail_message(request, stream, "NOT_SUPPORTED_REQUEST");
        }
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

        build_grpc_context(&ctx, 0);
        set_request_deadline(ctx);

        response.mutable_commonresponse()->set_responseid(request.requestid());
        response.set_message(request.commandecho().message());

        // Snapshot for this one unary call (stub access invariant, grpc.h).
        const auto transport = current_transport<CommandStub>();
        if (transport == nullptr) {
            return write_fail_message(request, stream, "command channel closed");
        }
        const auto status = transport->stub->CommandEcho(&ctx, response, &reply);
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

        // The collector may re-issue a command for a request id it already
        // owns (e.g. after a command-stream reconnect). Signal the old stream
        // to stop, but never join it here: the join would run under
        // active_streams_mutex_, which the shutdown signal phase must be able
        // to take without blocking, and a predecessor wedged in a
        // deadline-less Write()/Finish() has no completion bound. A later
        // cleanup sweeps it, or commandWorker() joins it on its way out; until
        // then it counts toward the stream cap below, degrading a wedged
        // re-issue to a logged rejection instead of an unbounded stall.
        for (auto& stream : active_thread_count_streams_) {
            if (stream->request_id() == request_id) {
                stream->request_stop();
            }
        }
        cleanup_active_thread_count_streams();

        if (active_thread_count_streams_.size() >= tuning_.max_active_thread_count_streams) {
            LOG_WARN("reject active thread count stream: requestId={}, activeStreams={}",
                     request_id, active_thread_count_streams_.size());
            return false;
        }

        // Reserve before start(): if push_back had to grow the vector and
        // threw, the unique_ptr's unwind would join the just-started stream
        // thread while holding active_streams_mutex_ — the unbounded join
        // this file forbids (see cleanup_active_thread_count_streams). With
        // capacity secured, the noexcept move push_back cannot throw.
        active_thread_count_streams_.reserve(active_thread_count_streams_.size() + 1);
        auto stream = std::make_unique<ActiveThreadCountStream>(this, ++socket_id_, request_id);
        stream->start();
        active_thread_count_streams_.push_back(std::move(stream));
        LOG_INFO("active thread count stream started: requestId={}", request_id);
        return true;
    }

    void GrpcCommand::cleanup_active_thread_count_streams() {
        // Runs under active_streams_mutex_, and erasing joins each swept
        // stream via its destructor. Sweeping only done() streams keeps that
        // join near-instant (done_ is the last statement of every run() exit
        // path): no join without a completion bound may run under this mutex,
        // or the shutdown signal phase could block behind it.
        active_thread_count_streams_.erase(
            std::remove_if(active_thread_count_streams_.begin(), active_thread_count_streams_.end(),
                [](const auto& stream) { return stream->done(); }),
            active_thread_count_streams_.end());
    }

    void GrpcCommand::request_stop_active_thread_count_streams() {
        // Signal-only: the streams stay owned by the vector so the joining
        // stop_active_thread_count_streams() — run by commandWorker() on its
        // way out, inside the shutdown deadline — still finds and joins them.
        std::unique_lock<std::mutex> lock(active_streams_mutex_);
        for (auto& stream : active_thread_count_streams_) {
            stream->request_stop();
        }
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
        // Supervised (see superviseWorker): a transient exception (e.g. one
        // escaping a command handler) must not kill the worker for the process
        // lifetime — the collector could never reach this agent again. Only a
        // stop request or agent exit ends the worker.
        if (!stopping()) {
            superviseWorker("grpc command worker", tuning_.worker_restart_delay,
                            command_worker_mutex_, command_worker_cv_,
                            [this] { return stopping(); },
                            [this] { run_command_worker(); return true; });
        }
        stop_active_thread_count_streams();
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
        // channel is READY but the collector terminates the stream at once
        // (auth rejection, version mismatch, wrong port). Back off
        // exponentially instead, resetting once a command is read.
        ExponentialBackoff reconnect_backoff{tuning_};

        while (!stopping()) {
            if (!readyChannel()) {
                break;
            }

            grpc::ClientContext context;
            build_grpc_context(&context, ++socket_id_);
            context.AddMetadata(METADATA_SUPPORT_COMMAND_CODE, support_command_code_header());
            // This worker sits in a blocking Read() for the stream's whole
            // life, so there is no loop boundary at which to check either the
            // stream max age or the channel-rotation deadline. End at the
            // earlier one; the resulting DEADLINE_EXCEEDED close is planned
            // and reconnects without the failure delay. readyChannel() then
            // rotates the channel when that was the deadline which fired.
            const auto renewal_delay = next_stream_renewal_delay();
            if (renewal_delay.count() > 0) {
                context.set_deadline(std::chrono::system_clock::now() + renewal_delay);
            }

            const StreamContextGuard context_guard(this, &context);

            // Re-check under the publish: a requestStopCommandWorker() that ran
            // between the loop condition and the guard publishing the context
            // found nothing to TryCancel, so it must be honored here.
            // Otherwise the RPC starts uncancellable and its deadline-less
            // Finish() could block on an unresponsive peer while shutdown
            // hangs in the worker join.
            if (stopping()) {
                break;
            }

            // Snapshot for the whole session (stub access invariant, grpc.h).
            // Null only after closeChannel(), which runs once this worker
            // has been signaled to stop.
            const auto transport = current_transport<CommandStub>();
            if (transport == nullptr) {
                break;
            }
            LOG_INFO("connect to command service stream");
            auto stream = transport->stub->HandleCommandV2(&context);
            bool expired_for_renewal = false;
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
                    const auto handled = dispatch_command(request, stream.get());
                    request.Clear();
                    if (!handled) {
                        LOG_INFO("command stream write failed while handling request");
                        break;
                    }
                }
                stream->WritesDone();
                const auto status = stream->Finish();
                expired_for_renewal = renewal_delay.count() > 0 &&
                                      status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED;
                if (expired_for_renewal) {
                    LOG_INFO("command service stream reached its renewal deadline ({}ms), reopening",
                             renewal_delay.count());
                } else if (!status.ok() && !stopping()) {
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

            if (stopping()) {
                break;
            }
            // A planned renewal close is not a failure: reopen at once.
            if (!expired_for_renewal && wait_reconnect_delay(reconnect_backoff.next_delay())) {
                break;
            }
        }
    }

    void GrpcCommand::requestStopCommandWorker() {
        request_stop();
        {
            // Pairs the stop with wait_reconnect_delay()'s predicate so a
            // worker sleeping out a reconnect delay wakes immediately.
            std::unique_lock<std::mutex> lock(command_worker_mutex_);
            command_worker_cv_.notify_all();
        }
        cancel_command_stream();
        // Signal-only: joining the stream threads here would block the
        // shutdown signal phase on a best-effort TryCancel with no completion
        // bound. commandWorker() joins them on its way out, which runs under
        // the shutdown deadline.
        request_stop_active_thread_count_streams();
    }

    void GrpcCommand::stopCommandWorker() {
        requestStopCommandWorker();
        stop_active_thread_count_streams();
    }

    //GrpcAgent

    GrpcAgent::GrpcAgent(std::shared_ptr<const Config> config, const GrpcClientTuning& tuning)
        : GrpcClient(AGENT, std::move(config), tuning) {}

    void GrpcAgent::create_stub(const std::shared_ptr<grpc::Channel>& channel) {
        set_agent_stub(v1::Agent::NewStub(channel), channel);
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

        // Not under channel_mutex_ (see the stub access invariant in grpc.h):
        // this thread takes its own owning transport snapshot and holds it
        // across the call, so the ping worker rotating the agent channel
        // meanwhile cannot destroy the channel this send is on. Holding the
        // mutex instead would stall the ping worker's readyChannel() for up
        // to the request deadline — and conversely readyChannel()'s unbounded
        // backoff would park this thread for a whole collector outage,
        // uninterruptible by stopAgentInfo().
        const auto transport = current_transport<AgentStub>();
        if (transport == nullptr) {
            LOG_ERROR("failed to register the agent: channel is closed");
            return SEND_FAIL;
        }
        build_grpc_context(&ctx, 0);

        google::protobuf::Arena arena;
        auto* agent_info = google::protobuf::Arena::Create<v1::PAgentInfo>(&arena);
        build_agent_info(agent_info, &arena);

        set_request_deadline(ctx);
        const grpc::Status status = transport->stub->RequestAgentInfo(&ctx, *agent_info, &reply);

        if (status.ok()) {
            LOG_INFO("success to register the agent");  
            return SEND_OK;
        }

        LOG_ERROR("failed to register the agent: {}, {}", static_cast<int>(status.error_code()), status.error_message());
        return SEND_FAIL;
    }

    bool GrpcAgent::registerAgentWithRetry() {
        // Boot-phase registration: keep trying until the collector accepts the
        // AgentInfo. Supervised per attempt, so a transient exception is
        // retried like a failed send. Only stopAgentInfo() or agent exit ends
        // the loop.
        const auto retry_interval = std::chrono::milliseconds(config_->collector.agent_info.send_retry_interval_ms);
        // Jittered, non-escalating delay (multiplier 1.0 keeps the base
        // interval): sibling pre-fork workers start simultaneously, and a
        // fixed interval would put every worker's registration in lockstep for
        // the whole outage. Constructed here — the worker's init thread,
        // always post-fork — so each worker seeds its own jitter sequence.
        ExponentialBackoff retry_backoff(retry_interval, 1.0,
                                         tuning_.reconnect_randomization_factor,
                                         retry_interval * 2);
        while (!agent_->isExiting()) {
            try {
                if (send_agent_info_once()) {
                    return true;
                }
            } CATCH_AND_LOG("register agent")
            if (wait_agent_info_until(std::chrono::steady_clock::now() + retry_backoff.next_delay())) {
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
            if (try_count + 1 < max_try_count &&
                wait_agent_info_until(std::chrono::steady_clock::now() + retry_interval)) {
                return false;
            }
        }
        return false;
    }

    bool GrpcAgent::wait_agent_info_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        agent_info_cv_.wait_until(lock, deadline, [this] { return should_stop_agent_info(); });
        return should_stop_agent_info();
    }

    void GrpcAgent::agent_info_worker() {
        // Supervised (see superviseWorker) like the other grpc workers: a
        // transient exception (e.g. bad_alloc while building AgentInfo under
        // memory pressure) must not kill the periodic re-send scheduler for
        // the process lifetime. Only a stop request or agent exit ends the
        // worker; crash restarts are paced by the restart delay.
        superviseWorker("AgentInfo scheduler", tuning_.worker_restart_delay,
                        agent_info_mutex_, agent_info_cv_,
                        [this] { return should_stop_agent_info(); },
                        [this] { run_agent_info_worker(); return true; });
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
        // readyChannel() just ran the rotation, and this worker is the sole
        // rotator, so this snapshot is the transport the session opens on
        // (stub access invariant, grpc.h). Null only after closeChannel().
        const auto transport = current_transport<AgentStub>();
        if (transport == nullptr) {
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
        // cannot fail. Should one throw anyway, no OnDone ever arrives for this
        // reactor, so restore STREAM_DONE before unwinding — otherwise
        // finish_ping_stream() and drain_ping_stream_on_error() wait forever
        // and hang shutdown. Rethrow rather than return false: the worker
        // loops read false as "stopping" and exit for the process lifetime,
        // while the supervisor retries a thrown transient failure.
        try {
            transport->stub->async()->PingSession(stream_context_.get(), this);
            arm_stream_expiry();

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
            // Counted towards a forced channel rotation: the recycled stream
            // alone would land on the same stalled backend (record_stream_stall).
            record_stream_stall();
            if (stream_context_ != nullptr) {
                stream_context_->TryCancel();
            }
            // TryCancel is best-effort. The reactor still must not be
            // abandoned while the call is outstanding, so wait for OnDone.
            stream_cv_.wait(lock, [this] { return grpc_status_ == STREAM_DONE; });
        } else if (grpc_status_ == STREAM_CONTINUE) {
            record_stream_write_ok();  // pong received: the backend is reading
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
        // Supervised (see superviseWorker): a transient exception must not kill
        // the worker for the process lifetime — without pings the collector
        // marks the agent dead. run_ping_worker() returning false (a stream
        // that failed to start) restarts with a fresh stream just like the
        // exception path, which drains the broken stream first. Only a stop
        // request or agent exit ends the worker.
        superviseWorker("grpc ping worker", tuning_.worker_restart_delay,
                        ping_worker_mutex_, ping_cv_,
                        [this] { return stopping(); },
                        [this] { return run_ping_worker(); },
                        [this] { drain_ping_stream_on_error(); });
    }

    bool GrpcAgent::run_ping_worker() {
        if (!start_ping_stream()) {
            return false;
        }

        std::unique_lock<std::mutex> lock(ping_worker_mutex_);

        while (true) {
            lock.unlock();
            if (stream_expired()) {
                // End through the normal finish path, then reopen. The expiry
                // is the earlier of StreamMaxAgeMs and the current channel's
                // rotation deadline, so ChannelMaxAgeMs works independently.
                // Checked once per ping interval, which is the effective
                // granularity of the deadline.
                LOG_INFO("ping stream reached its renewal deadline, reopening");
                finish_ping_stream();
                if (!start_ping_stream()) {
                    return false;
                }
            }
            if (write_and_await_ping_stream() == STREAM_DONE) {
                if (!start_ping_stream()) {
                    return false;
                }
            }

            lock.lock();
            if (ping_cv_.wait_for(lock, tuning_.ping_interval, [this]{ return stopping(); })) {
                lock.unlock();
                finish_ping_stream();
                return true;
            }
        }
    }

    void GrpcAgent::stopPingWorker() {
        request_stop();
        {
            // Notify under the wait mutex: a worker between its stopping()
            // check and blocking on the CV cannot miss the wakeup.
            std::unique_lock<std::mutex> lock(ping_worker_mutex_);
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
        // Pins the channel (and stub) this batch was launched on until the
        // callback has run — see the stub access invariant in grpc.h.
        std::shared_ptr<const GrpcClient::Transport> transport;
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

    void GrpcSpan::create_stub(const std::shared_ptr<grpc::Channel>& channel) {
        set_span_stub(v1::Span::NewStub(channel), channel);
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
    } CATCH_AND_LOG("failed to enqueue span:")

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
            // Snapshot before paying the serialization cost. It rides in
            // `pending`, so the channel this batch goes out on outlives the
            // call even if the worker rotates to a successor meanwhile (stub
            // access invariant, grpc.h). Null only after closeChannel():
            // treated like a synchronous launch failure.
            const auto transport = current_transport<SpanStub>();
            if (transport == nullptr) {
                throw std::runtime_error("span channel is closed");
            }
            pending->transport = transport;
            pending->request = google::protobuf::Arena::Create<v1::PSpanMessageBatch>(&pending->arena);
            // Reserve up front, like build_grpc_span does for span events: a
            // growing RepeatedPtrField doubles its pointer array, and on an arena
            // every regrow strands the old array there for the batch's lifetime.
            // The count is exactly batch.size().
            pending->request->mutable_span()->Reserve(static_cast<int>(batch.size()));

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
            transport->stub->async()->SendSpanBatch(ctx_ptr, request_ptr, reply_ptr,
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
        // Reached only by a throw before the permit is held (realistically
        // try_acquire_permit's lock/wait); every later path is covered by the
        // inner catch-all. Clear the batch like every other exit: the chunks
        // were not sent, and flush_remaining() keeps appending to the same
        // vector across calls, so leftovers here would be re-sent by its next
        // send_batch_async round.
        batch.clear();
        LOG_ERROR("failed to build span batch: exception = {}", e.what());
    } catch (...) {
        batch.clear();
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
        while (span_queue_.try_dequeue(span)) {
            remaining.push_back(std::move(span));
        }
        if (!remaining.empty()) {
            // readyChannel() refuses to wait once the agent is exiting, so probe
            // the channel state directly and send only over a live connection.
            // The transport (or its channel) is null if the agent was never
            // brought online via Start() (openChannel() opens it), in which
            // case there is nothing to flush to.
            const auto transport = current_transport<SpanStub>();
            const auto channel = transport ? transport->channel : nullptr;
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
            } CATCH_AND_LOG("grpc span worker")

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
        } CATCH_AND_LOG("grpc span worker flush")
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

    void GrpcStats::create_stub(const std::shared_ptr<grpc::Channel>& channel) {
        set_stats_stub(v1::Stat::NewStub(channel), channel);
    }

    bool GrpcStats::start_stats_stream() {
        LOG_DEBUG("start_stats_stream");
        if (!readyChannel()) {
            return false;
        }
        // See start_ping_stream(): this worker is the sole rotator, so the
        // transport readyChannel() left current is this session's.
        const auto transport = current_transport<StatStub>();
        if (transport == nullptr) {
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
        // Rethrow rather than return false: the worker loops read false as
        // "stopping" and exit for the process lifetime, while the supervisor
        // retries a thrown transient failure.
        try {
            transport->stub->async()->SendAgentStat(stream_context_.get(), &reply_, this);
            arm_stream_expiry();

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
                // Counted towards a forced channel rotation: the recycled
                // stream alone would land on the same stalled backend
                // (record_stream_stall).
                record_stream_stall();
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
            record_stream_write_ok();
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
            if (stopping() || stats_queue_.empty()) {
                LOG_DEBUG("stats - queue empty");
                return STREAM_CONTINUE;
            }

            stats = stats_queue_.front();
            stats_queue_.erase(stats_queue_.begin());
        }

        // The payload is read now, not when the token was enqueued: AgentStats
        // publishes its finished cycle before enqueuing (copySnapshots), and
        // the URL snapshot swap takes everything aggregated so far.
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
        // Treated like an empty queue: the worker waits for the next payload.
        return STREAM_CONTINUE;
    }

    void GrpcStats::enqueueStats(const StatsType stats) noexcept try {
        if (stats_disabled()) {
            return;
        }

        {
            std::unique_lock<std::mutex> lock(stats_queue_mutex_);
            // One token per type. The token carries no payload — next_write
            // reads the producer's buffer (the last completed AgentStats
            // cycle, the current URL snapshot) when it consumes the token —
            // so a second token of the same type queued behind a stalled
            // stream would only send that same buffer again. This also bounds
            // the queue at two entries without a capacity check, and a stall
            // never discards anything: the producers keep their data until
            // the stream drains it. (The former cap of two purged the queue,
            // the AgentStats counters and the URL snapshot on the third token
            // or on a channel recovery slower than 5s — every collector
            // restart cost up to a minute of stats.)
            if (std::find(stats_queue_.begin(), stats_queue_.end(), stats) == stats_queue_.end()) {
                stats_queue_.push_back(stats);
            }
        }

        // Notify after releasing the lock so the woken worker does not
        // immediately block on stats_queue_mutex_ (matches enqueueSpan).
        stats_queue_cv_.notify_one();
    } CATCH_AND_LOG("failed to enqueue stats:")

    void GrpcStats::sendStatsWorker() {
        // Boot-time decision: both flags are non-reloadable (see
        // Config::retainNonReloadableFrom and the invariant note in
        // UrlStats::addUrlStatsWorker), so a worker that returns here can
        // never be needed later. config_ is the pinned boot snapshot, so
        // stopStatsWorker's identical gate always agrees with this one.
        if (stats_disabled()) {
            return;
        }

        // Supervised (see superviseWorker) like the ping worker: a transient
        // exception (e.g. bad_alloc while building a stats message) must not
        // kill the worker for the process lifetime — stats would never be
        // reported again. Only a stop request or agent exit ends the worker.
        superviseWorker("grpc stats worker", tuning_.worker_restart_delay,
                        stats_queue_mutex_, stats_queue_cv_,
                        [this] { return stopping(); },
                        [this] { return run_stats_worker(); },
                        [this] { drain_stats_stream_on_error(); });
    }

    bool GrpcStats::run_stats_worker() {
        if (!start_stats_stream()) {
            return false;
        }

        std::unique_lock<std::mutex> lock(stats_queue_mutex_);
        while (true) {
            stats_queue_cv_.wait(lock, [this]{
                return !stats_queue_.empty() || stopping();
            });
            const bool stop = stopping();
            lock.unlock();

            if (stop) {
                finish_stats_stream();
                return true;
            }

            if (stream_expired()) {
                // Reopen between writes, never mid-write, so no stat payload
                // is lost. The expiry is the earlier of StreamMaxAgeMs and
                // the channel-rotation deadline, so ChannelMaxAgeMs works
                // independently; the queued token goes out on the fresh
                // stream after readyChannel() rotates when due.
                LOG_INFO("stats stream reached its renewal deadline, reopening");
                finish_stats_stream();
                if (!start_stats_stream()) {
                    return false;
                }
            }

            if (write_and_await_stats_stream() == STREAM_DONE) {
                if (!start_stats_stream()) {
                    return false;
                }
            }
            lock.lock();
        }
    }

    void GrpcStats::stopStatsWorker() {
        if (stats_disabled()) {
            return;
        }

        request_stop();
        {
            // Notify under the wait mutex: a worker between its stopping()
            // check and blocking on the CV cannot miss the wakeup.
            std::unique_lock<std::mutex> lock(stats_queue_mutex_);
            stats_queue_cv_.notify_one();
        }
        // The worker may be blocked in write_and_await_stats_stream() waiting
        // for a write completion a stalled collector never delivers; only
        // stream activity wakes that wait. Request cancellation to prompt the
        // pending operation and OnDone (TryCancel provides no hard completion
        // bound). Taken after releasing stats_queue_mutex_: the worker takes
        // that lock while holding stream_mutex_ (next_write), so the opposite
        // nesting here would risk deadlock.
        std::unique_lock<std::mutex> lock(stream_mutex_);
        if (stream_context_ != nullptr && grpc_status_ != STREAM_DONE) {
            stream_context_->TryCancel();
        }
    }
}
