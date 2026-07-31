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
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <deque>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <grpc/grpc.h>
#include <grpcpp/alarm.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <google/protobuf/arena.h>

#include "v1/Service.grpc.pb.h"

#include "agent_service.h"
#include "callstack.h"
#include "sharded_bounded_queue.h"
#include "span.h"
#include "utility.h"

namespace pinpoint {
    /**
     * @brief Return codes used by gRPC request helpers.
     */
    enum GrpcRequestStatus {SEND_OK, SEND_FAIL};
    /**
     * @brief State machine transitions used while streaming data to the collector.
     */
    enum GrpcStreamStatus {STREAM_WRITE, STREAM_CONTINUE, STREAM_DONE, STREAM_EXCEPTION};
    /**
     * @brief Identifies the type of gRPC client sharing common facilities.
     */
    enum ClientType {AGENT, METADATA, SPAN, STATS};

    /**
     * @brief Build the ordered gRPC metadata header set for a request.
     *
     * Mirrors Java's ClientHeaderFactoryV1 / ClientHeaderFactoryV4: v1/v3 send
     * protocol.version=100 (agentname only when present), v4 sends
     * protocol.version=400 plus agentname (always), servicename and apikey.
     * Extracted as a pure function so the per-version header set is unit-testable.
     *
     * @p agent_id is passed separately (rather than read from @p config) so
     * the value always comes from the agent's resolved identity snapshot,
     * which the gRPC clients read through AgentService::getAgentId().
     */
    std::vector<std::pair<std::string, std::string>>
    build_grpc_metadata(const Config& config, std::string_view agent_id,
                        int64_t start_time, int32_t app_type, unsigned long socket_id);

    /**
     * @brief Internal timing and capacity knobs shared by the gRPC clients.
     *
     * These are deliberately NOT part of Config: they are transport-internal
     * tuning values with no user-facing contract. The defaults are the
     * production values; tests inject shortened timeouts and small caps so
     * timing-dependent behavior (deadlines, retries, restart pacing,
     * overflow) runs in milliseconds instead of seconds. Injected once at
     * construction and treated as immutable after workers start.
     */
    struct GrpcClientTuning {
        /// Deadline applied to every unary RPC.
        std::chrono::milliseconds request_timeout{5000};

        /// Reconnect backoff for channel readiness and stream re-connects.
        std::chrono::milliseconds reconnect_initial_interval{3000};
        double reconnect_multiplier{1.2};
        double reconnect_randomization_factor{0.3};
        std::chrono::milliseconds reconnect_max_interval{30000};
        /// Slice for the channel-ready wait loop so stop requests are
        /// noticed while a longer backoff delay elapses.
        std::chrono::milliseconds backoff_sleep_slice{1000};
        /// Channel recovery at least this slow stales client-owned queues
        /// (see on_slow_channel_recovery()).
        std::chrono::seconds slow_recovery_threshold{5};

        /// Bounded wait for a stream to deliver OnDone at shutdown before
        /// escalating to TryCancel.
        std::chrono::milliseconds stream_finish_timeout{3000};
        /// Bounded wait for a stream write to complete (for the ping stream:
        /// write + server pong) before cancellation is requested and the
        /// worker begins stream cleanup.
        std::chrono::milliseconds stream_write_timeout{5000};
        /// Pause before a worker loop is restarted after an unexpected
        /// exception, so a persistent failure cannot become a hot spin.
        std::chrono::milliseconds worker_restart_delay{1000};

        /// Scheduled retries for one metadata item before its cache entry is
        /// released, and the delay between them.
        int meta_retry_max_attempts{3};
        std::chrono::milliseconds meta_retry_delay{1000};
        /// Concurrently in-flight metadata RPCs. Unary sends are pipelined
        /// behind this permit cap instead of serialized one blocking call at
        /// a time: a high error rate produces one exception metadata per
        /// errored span, and a serial worker caps throughput at ~1/RTT.
        int meta_max_concurrent_requests{4};
        /// Bounded wait for in-flight metadata calls at shutdown before
        /// TryCancel is requested (and again after that request).
        std::chrono::milliseconds meta_shutdown_await_timeout{3000};

        /// Interval between ping writes on the agent ping stream.
        std::chrono::milliseconds ping_interval{60000};

        /// Interval between active-thread-count stream responses, and the cap
        /// on concurrently served streams (each owns a dedicated thread).
        std::chrono::milliseconds active_thread_count_flush_interval{1000};
        size_t max_active_thread_count_streams{10};

        /// Bounded wait for in-flight SendSpanBatch calls at shutdown before
        /// TryCancel is requested (and again after that request).
        std::chrono::milliseconds span_shutdown_await_timeout{3000};
        /// Minimum spacing between cumulative span-queue-drop reports.
        std::chrono::seconds span_queue_drop_log_interval{60};

        /// Stats payloads pending on the stream before new ones are dropped
        /// (an overflow also marks the queued stats stale).
        size_t max_stats_queue_size{2};
    };

    /**
     * @brief Exponential backoff with jitter for reconnect attempts.
     */
    class ExponentialBackoff {
    public:
        explicit ExponentialBackoff(const GrpcClientTuning& tuning)
            : ExponentialBackoff(tuning.reconnect_initial_interval,
                                 tuning.reconnect_multiplier,
                                 tuning.reconnect_randomization_factor,
                                 tuning.reconnect_max_interval) {}
        ExponentialBackoff(std::chrono::milliseconds initial_interval,
                           double multiplier,
                           double randomization_factor,
                           std::chrono::milliseconds max_interval);

        std::chrono::milliseconds next_delay();
        void reset();

    private:
        std::chrono::milliseconds initial_interval_;
        double multiplier_;
        double randomization_factor_;
        std::chrono::milliseconds max_interval_;
        int attempt_{0};
        std::mt19937_64 rng_;
    };

    /**
     * @brief Base client that encapsulates channel management shared by all gRPC workers.
     */
    class GrpcClient {
    public:
        /**
         * @brief Constructs a client for the given client type.
         *
         * @param client_type Which collector service this client targets.
         * @param config Agent configuration (collector address, batch sizes).
         * @param tuning Transport-internal knobs; defaults are the production
         *        values, tests inject shortened ones (see GrpcClientTuning).
         */
        GrpcClient(ClientType client_type, std::shared_ptr<const Config> config,
                   const GrpcClientTuning& tuning = {});
        /**
         * @brief Injects the agent service.
         *
         * @param agent Owning agent service.
         */
        void setAgentService(AgentService* agent);
        virtual ~GrpcClient() = default;
        /**
         * @brief Opens the gRPC channel and creates the client stub.
         *
         * Deferred out of the constructor so that agent construction stays
         * "cold": `grpc::CreateCustomChannel()` triggers `grpc_init` and starts
         * gRPC's own background threads, which must not happen until the agent
         * is started in the process that will actually use it (StartAgent()
         * runs in each worker, after any fork). Idempotent: a second call
         * while a channel already exists is a no-op.
         */
        void openChannel();
        /**
         * @brief Ensures the gRPC channel is connected and ready for use.
         *
         * @return `true` once the channel is ready; `false` if the client is
         *         stopping first.
         */
        virtual bool readyChannel();
        /// @brief Releases the current channel handle.
        void closeChannel() {
            std::unique_lock<std::mutex> lock(channel_mutex_);
            channel_.reset();
        }

    protected:
        // Non-owning. AgentImpl owns every gRPC client (unique_ptr members) and
        // joins this client's worker before its own destruction, so agent_ never
        // dangles. A shared_ptr here would form a cycle and leak the agent.
        AgentService* agent_{};
        std::shared_ptr<const Config> config_{};
        // Immutable once any worker runs. Non-const only so testable
        // subclasses can shorten values between construction and starting a
        // worker; production code never writes it after the constructor.
        GrpcClientTuning tuning_{};
        std::shared_ptr<grpc::Channel> channel_{};
        std::mutex channel_mutex_{};
        std::string client_name_{};
        ClientType client_type_;

        // Socket-id-independent gRPC headers, built lazily on the first
        // build_grpc_context() call and reused for every request afterwards
        // (the underlying identity fields never change once the agent runs).
        mutable std::once_flag grpc_metadata_once_{};
        mutable std::vector<std::pair<std::string, std::string>> grpc_metadata_cache_{};

        std::unique_ptr<grpc::ClientContext> stream_context_{};
        std::mutex stream_mutex_{};
        std::condition_variable stream_cv_{};
        grpc::Status stream_status_{};
        // Idle state: no write in flight and the stream is not finished.
        GrpcStreamStatus grpc_status_{STREAM_CONTINUE};
        ExponentialBackoff channel_ready_backoff_;

        // Per-client stop, set by this client's stopXWorker(). Today
        // do_shutdown() always sets the agent-wide exiting flag before the
        // stop methods run, so the workers terminate either way — but the
        // wait loops (readyChannel's unbounded retry, the reconnect delays)
        // must not depend on that call-ordering contract: honoring the
        // per-client flag keeps a lone stopXWorker() call from joining a
        // thread that can block indefinitely during a collector outage.
        // Never reset: a stopped client is terminal, like agent shutdown.
        std::atomic<bool> stop_requested_{false};

        void request_stop() { stop_requested_.store(true, std::memory_order_relaxed); }
        /// @brief True once this client was stopped or the agent is exiting.
        bool stopping() const {
            return stop_requested_.load(std::memory_order_relaxed) || agent_->isExiting();
        }

        /**
         * @brief Blocks until the channel becomes ready or the delay is exceeded.
         */
        bool wait_channel_ready(std::chrono::milliseconds delay) const;

        /**
         * @brief Creates the concrete service stub from `channel_`.
         *
         * Called by openChannel() after the channel is built. Each derived
         * client binds its own generated stub type here.
         */
        virtual void create_stub() = 0;

        void build_grpc_context(grpc::ClientContext* context, unsigned long socket_id) const;

        /// @brief Applies the tuned unary request deadline to @p context.
        void set_request_deadline(grpc::ClientContext& context) const;

        /**
         * @brief Notifies derived clients that channel recovery took long enough to stale client-owned queues.
         */
        virtual void on_slow_channel_recovery(std::chrono::seconds) {}
    };

    /**
     * @brief Metadata describing an API string cached on the collector.
     */
    struct ApiMeta {
        int32_t id_;
        int32_t type_;
        std::string api_str_;
        
        ApiMeta(int32_t id, int32_t type, std::string_view api_str) 
            : id_(id), type_(type), api_str_(api_str) {}
    };

    /**
     * @brief Type tag for cached string metadata.
     */
    enum StringMetaType {
        STRING_META_ERROR,
        STRING_META_SQL
    };

    /**
     * @brief Metadata describing a cached string value (error or SQL).
     */
    struct StringMeta {
        int32_t id_;
        std::string str_val_;
        StringMetaType type_;
        
        StringMeta(int32_t id, std::string_view str_val, StringMetaType type) 
            : id_(id), str_val_(str_val), type_(type) {}
    };

    /**
     * @brief Metadata describing a cached SQL UID.
     */
    struct SqlUidMeta {
        SqlUid uid_;
        std::string sql_;

        SqlUidMeta(SqlUid uid, std::string_view sql)
            : uid_(uid), sql_(sql) {}
    };

    /**
     * @brief Metadata bundle carrying exception call stacks for a completed span.
     */
    struct ExceptionMeta {
        TraceId txid_;
        int64_t span_id_;
        std::string url_template_;
        std::vector<std::unique_ptr<Exception>> exceptions_;
        
        ExceptionMeta(TraceId txid, int64_t span_id, std::string_view url_template, std::vector<std::unique_ptr<Exception>>&& exceptions)
            : txid_(txid), span_id_(span_id), url_template_(url_template), exceptions_(std::move(exceptions)) {}
        
        // Delete copy constructor and copy assignment
        ExceptionMeta(const ExceptionMeta&) = delete;
        ExceptionMeta& operator=(const ExceptionMeta&) = delete;
        
        // Default move constructor and move assignment
        ExceptionMeta(ExceptionMeta&&) = default;
        ExceptionMeta& operator=(ExceptionMeta&&) = default;
    };

    /**
     * @brief Type-safe variant covering all metadata variants queued by the agent.
     */
    using MetaValue = std::variant<ApiMeta, StringMeta, SqlUidMeta, ExceptionMeta>;

    /**
     * @brief Type discriminator for metadata payloads.
     */
    enum MetaType {META_API, META_STRING, META_SQL_UID, META_EXCEPTION};
    
    /**
     * @brief Metadata item queued for transmission to the collector.
     */
    struct MetaData {
        MetaType meta_type_;
        MetaValue value_;
        
        MetaData(MetaType meta_type, int32_t id, int32_t api_type, std::string_view api_str)
            : meta_type_(meta_type), value_(ApiMeta(id, api_type, api_str)) {}
        
        MetaData(MetaType meta_type, int32_t id, std::string_view str_val, StringMetaType str_type)
            : meta_type_(meta_type), value_(StringMeta(id, str_val, str_type)) {}
        
        MetaData(MetaType meta_type, SqlUid uid, std::string_view sql)
            : meta_type_(meta_type), value_(SqlUidMeta(uid, sql)) {}
        
        MetaData(MetaType meta_type, TraceId txid, int64_t span_id, std::string_view url_template, std::vector<std::unique_ptr<Exception>>&& exceptions)
            : meta_type_(meta_type), value_(ExceptionMeta(txid, span_id, url_template, std::move(exceptions))) {}
    };

    // Pipeline state shared between GrpcMetadata, its worker and the async
    // completion callbacks; definitions live in grpc.cpp.
    struct MetaPipeline;
    struct PendingMetaRpc;

    /**
     * @brief gRPC client responsible for metadata upload.
     *
     * Metadata items are independent unary RPCs (the collector keys each by
     * its id), so they are pipelined: the worker launches async sends behind
     * a small in-flight permit cap and processes completions as they arrive,
     * mirroring GrpcSpan's async batch path. Failed items re-enter the retry
     * schedule; exhausted items release their cache entry so the id is
     * regenerated and re-sent later.
     */
    class GrpcMetadata : public GrpcClient {
    public:
        explicit GrpcMetadata(std::shared_ptr<const Config> config,
                              const GrpcClientTuning& tuning = {});
        ~GrpcMetadata() override = default;

        /**
         * @brief Adds metadata to the outbound queue.
         *
         * @param meta Metadata payload (ownership transferred).
         */
        void enqueueMeta(std::unique_ptr<MetaData> meta) noexcept;
        /// @brief Worker loop that sends metadata payloads.
        void sendMetaWorker();
        /// @brief Stops the metadata worker loop.
        void stopMetaWorker();

    protected:
        void set_meta_stub(std::unique_ptr<v1::Metadata::StubInterface> stub) { meta_stub_ = std::move(stub); }
        void create_stub() override;

    private:
        std::unique_ptr<v1::Metadata::StubInterface> meta_stub_{};

        // Queues, retry schedule, permits, in-flight registry and completed
        // outcomes, all under the pipeline's one mutex. Heap-resident and
        // shared with the async completion callbacks — never `this` — so a
        // callback delivered after this client (or the whole agent) is
        // destroyed only touches live heap memory (see SpanBatchInflight).
        std::shared_ptr<MetaPipeline> pipeline_{};
        // Rate-limited overflow reporting (see QueueDropReporter).
        QueueDropReporter meta_drop_reporter_{};

        void release_failed_cache(const MetaData& meta) const;
        // Builds the type-specific request and launches the async RPC; the
        // caller's permit is owned by the completion callback once launched.
        void launch_meta_rpc(std::unique_ptr<MetaData> meta, int retry_count);
        // Reclaims the permit of a launch that threw and routes the item
        // through the normal retry path.
        void on_launch_failure(const std::shared_ptr<PendingMetaRpc>& call, bool registered,
                               std::unique_ptr<MetaData> meta, int retry_count);
        // Handles completed calls: success logs, failure re-enters retry.
        void process_completed(std::vector<std::shared_ptr<PendingMetaRpc>>& done);
        // Schedules the item's next retry, or releases its cache entry once
        // the retry budget is exhausted. `retry_count` counts this failure.
        void retry_or_drop(std::unique_ptr<MetaData> meta, int retry_count);
        // Bounded wait for in-flight calls at shutdown, escalating to
        // TryCancel (mirrors GrpcSpan::await_in_flight_requests).
        void await_in_flight_requests();
        // Worker loop body; sendMetaWorker() supervises it and restarts it
        // after a transient exception instead of letting the worker die.
        void run_meta_worker();
    };

    /**
     * @brief Dispatcher for collector-originated profiler commands.
     */
    class GrpcCommandDispatcher {
    public:
        using Handler = std::function<bool(const v1::PCmdRequest&, grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>*)>;

        void registerHandler(int32_t command_code, Handler handler);
        bool handle(const v1::PCmdRequest& request,
                    grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream) const;
        std::vector<int32_t> supportedCommandCodes() const;

    private:
        std::unordered_map<int32_t, Handler> handlers_{};
    };

    /**
     * @brief gRPC client responsible for the profiler command stream.
     */
    class GrpcCommand : public GrpcClient {
    public:
        explicit GrpcCommand(std::shared_ptr<const Config> config,
                             const GrpcClientTuning& tuning = {});
        ~GrpcCommand() override;

        /// @brief Worker loop that receives collector commands and dispatches them.
        void commandWorker();
        /// @brief Signals the worker and every active response stream to stop
        /// without joining anything, so the shutdown signal phase never blocks;
        /// commandWorker() joins the response streams on its way out.
        void requestStopCommandWorker();
        /// @brief Stops the command stream worker and any active response streams,
        /// joining the response stream threads.
        void stopCommandWorker();

    protected:
        void set_command_stub(std::unique_ptr<v1::ProfilerCommandService::StubInterface> stub) {
            command_stub_ = std::move(stub);
        }
        void create_stub() override;

    private:
        class ActiveThreadCountStream;

        std::unique_ptr<v1::ProfilerCommandService::StubInterface> command_stub_{};
        GrpcCommandDispatcher dispatcher_{};

        std::mutex command_worker_mutex_{};
        std::condition_variable command_worker_cv_{};
        grpc::ClientContext* command_stream_context_{nullptr};
        unsigned long socket_id_{0};

        std::mutex active_streams_mutex_{};
        std::vector<std::unique_ptr<ActiveThreadCountStream>> active_thread_count_streams_{};

        void register_default_handlers();
        bool handle_echo(const v1::PCmdRequest& request,
                         grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream);
        bool handle_active_thread_count(const v1::PCmdRequest& request,
                                        grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream);
        void build_command_context(grpc::ClientContext* context, unsigned long socket_id) const;
        void build_active_thread_count_response(v1::PCmdActiveThreadCountRes* response,
                                                int32_t request_id,
                                                int32_t sequence_id) const;
        bool add_active_thread_count_stream(int32_t request_id);
        void cleanup_active_thread_count_streams();
        void request_stop_active_thread_count_streams();
        void stop_active_thread_count_streams();
        bool write_fail_message(const v1::PCmdRequest& request,
                                grpc::ClientReaderWriterInterface<v1::PCmdMessage, v1::PCmdRequest>* stream,
                                std::string_view message) const;
        void cancel_command_stream();
        bool wait_reconnect_delay(std::chrono::milliseconds delay);
        // Worker loop body; commandWorker() supervises it and restarts it
        // after a transient exception instead of letting the worker die.
        void run_command_worker();
    };

    /**
     * @brief gRPC client responsible for agent registration and ping.
     */
    class GrpcAgent : public GrpcClient, public grpc::ClientBidiReactor<v1::PPing, v1::PPing> {
    public:
        explicit GrpcAgent(std::shared_ptr<const Config> config,
                           const GrpcClientTuning& tuning = {});
        ~GrpcAgent() override;

        /**
         * @brief Registers the agent with the collector and starts ping streaming.
         */
        virtual GrpcRequestStatus registerAgent();

        /// @brief Boot-phase registration: sends AgentInfo repeatedly until the
        /// collector accepts it. Blocks the caller (the agent's init thread);
        /// stopAgentInfo() or agent exit interrupts the retry wait.
        /// @return true once registration succeeded, false when stopped first.
        bool registerAgentWithRetry();

        /// @brief Worker loop that periodically sends ping requests.
        void sendPingWorker();
        /// @brief Stops the ping worker loop.
        void stopPingWorker();
        /// @brief Starts the periodic AgentInfo re-send scheduler.
        void startAgentInfo();
        /// @brief Non-blocking half of stopAgentInfo(): records the stop
        /// request and wakes a blocked registerAgentWithRetry() or scheduler
        /// wait without joining anything. Used by the shutdown signal phase
        /// so every worker winds down in parallel before the joins.
        void requestStopAgentInfo();
        /// @brief Stops the periodic AgentInfo re-send scheduler and wakes a
        /// blocked registerAgentWithRetry(); joins the scheduler thread.
        void stopAgentInfo();
        /// @brief Sets server metadata included in AgentInfo.
        void setServerMetaData(std::string_view server_info,
                               const std::vector<std::string>& args,
                               const std::vector<std::string>& libs);

        //grpc::ClientBidiReactor
        /// @brief Notification invoked after each write completes.
        void OnWriteDone(bool ok) override;
        /// @brief Notification invoked when a server ping response is available.
        void OnReadDone(bool ok) override;
        /// @brief Final notification when the stream terminates.
        void OnDone(const grpc::Status& s) override;

    protected:
        void set_agent_stub(std::unique_ptr<v1::Agent::StubInterface> stub) { agent_stub_ = std::move(stub); }
        void create_stub() override;

    private:
        struct ServerMetaData {
            std::string server_info;
            std::vector<std::string> vm_args;
            std::vector<std::string> service_libs;
        };

        std::unique_ptr<v1::Agent::StubInterface> agent_stub_{};

        v1::PPing ping_{}, pong_{};
        std::mutex ping_worker_mutex_{};
        std::condition_variable ping_cv_{};
        bool ping_stop_requested_{false};
        unsigned long socket_id_{0};
        // Set once per ping stream session when the stream starts shutting
        // down, so StartWritesDone()/RemoveHold() run exactly once no matter
        // which of the read-failure / write-failure / finish paths fires first.
        std::atomic<bool> ping_stream_closing_{false};

        std::thread agent_info_thread_;
        std::mutex agent_info_mutex_;
        std::condition_variable agent_info_cv_;
        bool agent_info_running_{false};
        bool agent_info_stop_requested_{false};
        bool server_meta_data_set_{false};
        ServerMetaData server_meta_data_;

        bool start_ping_stream();
        void close_ping_stream();
        void close_ping_stream_locked();
        void finish_ping_stream();
        void drain_ping_stream_on_error() noexcept;
        GrpcStreamStatus write_and_await_ping_stream();
        // Worker loop body; sendPingWorker() supervises it and restarts it
        // after a transient exception instead of letting the worker die.
        // Returns true when it ended on a stop request; false when a stream
        // start failed, so the supervisor retries with a fresh stream.
        bool run_ping_worker();

        void agent_info_worker();
        // Worker loop body; agent_info_worker() supervises it and restarts it
        // after a transient exception instead of letting the worker die.
        void run_agent_info_worker();
        bool send_agent_info_once();
        bool send_agent_info_with_retries(int max_try_count);
        bool wait_agent_info_retry(std::chrono::milliseconds delay);
        bool wait_agent_info_until(std::chrono::steady_clock::time_point deadline);
        bool should_stop_agent_info() const;

        void build_agent_info(v1::PAgentInfo* agent_info, google::protobuf::Arena* arena) const;
    };

    /**
     * @brief gRPC client that sends span batches to the collector via the
     *        unary @c SendSpanBatch RPC.
     *
     * Mirrors the policy implemented in the Java agent's
     * @c SpanBatchGrpcDataSender. Configuration lives under
     * @c Config::span::batch (size / flush_interval_ms /
     * collect_deadline_ms / max_concurrent_requests).
     *
     * ### Hybrid batch collection (size or time bounded)
     * - The worker blocks up to @c flush_interval_ms waiting for the first
     *   queued chunk.
     * - Once the first chunk arrives, more chunks are gathered until either
     *   the batch reaches @c size or @c collect_deadline_ms elapses since
     *   the first chunk — whichever comes first.
     *
     * ### Asynchronous unary transmission
     * - Each batch is sent via @c span_stub_->async()->SendSpanBatch() with
     *   a completion callback.
     * - Per-call state (ClientContext, arena, request, reply) is owned by a
     *   @c shared_ptr captured into the callback, so it remains alive through
     *   callback completion.
     *
     * ### Concurrency control (permit-based semaphore)
     * - At most @c max_concurrent_requests SendSpanBatch RPCs may be
     *   in flight at the same time.
     * - If no permit is available within @c flush_interval_ms the batch is
     *   dropped and the event is logged at INFO.
     *
     * ### Queue overflow policy
     * - The configured capacity is assigned to producer shards as transferable
     *   quotas. When a shard's quota is full, @c enqueueSpan replaces that
     *   shard's *oldest* chunk (head-drop), matching Java's preference for
     *   retaining the newest telemetry without returning to a process-wide
     *   lock. FIFO order is preserved per shard; cross-shard order is
     *   intentionally unspecified.
     * - By default once per minute, the worker logs the cumulative
     *   oldest-drop count at WARN only when it has increased since the
     *   previous report.
     *
     * ### partial_success handling
     * - Successful responses with @c rejected_spans > 0 are logged at WARN.
     * - Responses with no rejected spans but a non-empty @c error_message
     *   are logged at INFO.
     * - Rejected spans are not retried or re-queued (observability only).
     *
     * ### Shutdown
     * - On exit the worker drains any remaining chunks and, if the channel
     *   is already connected, sends them in batches of at most @c size. It
     *   then waits up to @c span_shutdown_await_timeout for in-flight permits,
     *   requests best-effort cancellation with TryCancel when calls remain,
     *   and waits for the same interval once more.
     * - Completion callbacks share state with the client only through a
     *   @c shared_ptr (no raw @c this capture), so a callback that fires
     *   after the GrpcSpan instance is destroyed remains memory-safe.
     */
    struct SpanBatchInflight;

    class GrpcSpan : public GrpcClient {
    public:
        explicit GrpcSpan(std::shared_ptr<const Config> config,
                          const GrpcClientTuning& tuning = {});
        ~GrpcSpan() override = default;

        /**
         * @brief Adds a span chunk to the outbound queue.
         *
         * On overflow the oldest chunk in the producer's shard is dropped to
         * make room (head-drop), retaining the newest telemetry.
         *
         * @param span Span chunk payload (ownership transferred).
         */
        void enqueueSpan(std::unique_ptr<SpanChunk> span) noexcept;
        /// @brief Worker loop that drains the queue and sends spans in batches.
        void sendSpanWorker();
        /// @brief Signals the worker loop to stop; when the channel is already
        ///        connected, the loop attempts to send pending spans before
        ///        exiting.
        void stopSpanWorker();

    protected:
        void set_span_stub(std::unique_ptr<v1::Span::StubInterface> stub) { span_stub_ = std::move(stub); }
        void create_stub() override;

    private:
        std::unique_ptr<v1::Span::StubInterface> span_stub_{};

        ShardedBoundedQueue<std::unique_ptr<SpanChunk>> span_queue_;

        // Queue operations never take this wait mutex. It only closes the race
        // between the single consumer checking all shards and sleeping.
        // Producers touch it only while the consumer is actually waiting; the
        // queue's short-held locks are independent per producer shard.
        std::mutex span_wait_mutex_{};
        std::condition_variable span_queue_cv_{};
        std::atomic<bool> span_consumer_waiting_{false};
        // Rate-limited overflow reporting, fed by the queue's own drop
        // counter via report_if_due() — see maybe_log_span_queue_drops().
        QueueDropReporter span_drop_reporter_;

        // Permit-based semaphore that caps the number of concurrently in-flight
        // SendSpanBatch RPCs, plus a registry of the in-flight call contexts so
        // shutdown can cancel them. Heap-resident and shared with the async
        // completion callbacks so a late callback never touches this object.
        std::shared_ptr<SpanBatchInflight> inflight_{};

        void collect_batch(std::vector<std::unique_ptr<SpanChunk>>& buffer);
        void maybe_log_span_queue_drops();
        bool wait_dequeue_until(std::unique_ptr<SpanChunk>& span,
                                std::chrono::steady_clock::time_point deadline);
        void notify_span_worker();
        void send_batch_async(std::vector<std::unique_ptr<SpanChunk>>& batch);
        bool try_acquire_permit(std::chrono::milliseconds timeout);
        bool try_acquire_all_permits(std::chrono::milliseconds timeout);
        void release_permit();
        void await_in_flight_requests();
        void flush_remaining(std::vector<std::unique_ptr<SpanChunk>>& pending_batch);
        // Worker loop body; sendSpanWorker() supervises it and restarts it
        // after a transient exception instead of letting the worker die.
        void run_span_worker(std::vector<std::unique_ptr<SpanChunk>>& pending_batch);
    };

    /**
     * @brief gRPC client that streams agent and URL statistics to the collector.
     */
    class GrpcStats : public GrpcClient, public grpc::ClientWriteReactor<v1::PStatMessage> {
    public:
        explicit GrpcStats(std::shared_ptr<const Config> config,
                           const GrpcClientTuning& tuning = {});
        ~GrpcStats() override { arena_.Reset(); }

        /**
         * @brief Queues a statistics payload to be sent.
         *
         * @param stats Type selector that determines which payload to build.
         */
        void enqueueStats(StatsType stats) noexcept;
        /// @brief Worker loop that streams statistics.
        void sendStatsWorker();
        /// @brief Stops the statistics worker loop.
        void stopStatsWorker();

        //grpc::ClientWriteReactor
        /// @brief Invoked when a write completes on the stream.
        void OnWriteDone(bool ok) override;
        /// @brief Called when the stream finishes.
        void OnDone(const grpc::Status& status) override;

    protected:
        void set_stats_stub(std::unique_ptr<v1::Stat::StubInterface> stub) { stats_stub_ = std::move(stub); }
        void create_stub() override;
        void on_slow_channel_recovery(std::chrono::seconds elapsed) override;
        bool empty_stats_queue_if_requested() noexcept;

    private:
        std::unique_ptr<v1::Stat::StubInterface> stats_stub_{};
        google::protobuf::Arena arena_{};
        v1::PStatMessage* msg_{};
        google::protobuf::Empty reply_{};

        std::queue<StatsType> stats_queue_{};
        std::mutex stats_queue_mutex_{};
        std::condition_variable stats_queue_cv_{};
        // Rate-limited overflow reporting (see QueueDropReporter).
        QueueDropReporter stats_drop_reporter_{};
        bool stats_stop_requested_{false};
        std::atomic<bool> force_stats_queue_empty_{false};
        // Set once per stats stream session when the stream starts shutting
        // down, so StartWritesDone()/RemoveHold() run exactly once no matter
        // which of the write-failure / write-timeout / finish paths fires
        // first (mirrors GrpcAgent::ping_stream_closing_).
        std::atomic<bool> stats_stream_closing_{false};

        bool start_stats_stream();
        GrpcStreamStatus write_and_await_stats_stream();
        void finish_stats_stream();
        void close_stats_stream_locked();
        void drain_stats_stream_on_error() noexcept;

        GrpcStreamStatus next_write();
        void empty_stats_queue() noexcept;
        // Worker loop body; sendStatsWorker() supervises it and restarts it
        // after a transient exception instead of letting the worker die.
        // Returns true when it ended on a stop request; false when a stream
        // start failed, so the supervisor retries with a fresh stream.
        bool run_stats_worker();
    };
}  // namespace pinpoint
