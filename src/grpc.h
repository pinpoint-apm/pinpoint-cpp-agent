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
#include <map>
#include <memory>
#include <mutex>
#include <deque>
#include <queue>
#include <string>
#include <thread>
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
#include "span.h"

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
     * @brief Base client that encapsulates channel management shared by all gRPC workers.
     */
    class GrpcClient {
    public:
        /**
         * @brief Constructs a client for the given client type.
         *
         * @param client_type Which collector service this client targets.
         */
        GrpcClient(ClientType client_type, std::shared_ptr<const Config> config);
        /**
         * @brief Injects the agent service.
         *
         * @param agent Owning agent service.
         */
        void setAgentService(AgentService* agent);
        virtual ~GrpcClient() = default;
        /**
         * @brief Ensures the gRPC channel is connected and ready for use.
         *
         * @return `true` if the channel is ready or successfully re-initialized.
         */
        virtual bool readyChannel();
        /// @brief Releases the current channel handle.
        void closeChannel() { channel_.reset(); }

    protected:
        AgentService* agent_{};
        std::shared_ptr<const Config> config_{};
        std::shared_ptr<grpc::Channel> channel_{};
        std::mutex channel_mutex_{};
        std::string client_name_{};
        ClientType client_type_;

        std::unique_ptr<grpc::ClientContext> stream_context_{};
        std::mutex stream_mutex_{};
        std::condition_variable stream_cv_{};
        grpc::Status stream_status_{};
        GrpcStreamStatus grpc_status_{};
        std::atomic<bool> force_queue_empty_{false};

        /**
         * @brief Blocks until the channel becomes ready or the deadline is exceeded.
         */
        bool wait_channel_ready() const;

        void build_grpc_context(grpc::ClientContext* context, unsigned long socket_id) const;
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
        std::vector<unsigned char> uid_;
        std::string sql_;
        
        SqlUidMeta(std::vector<unsigned char> uid, std::string_view sql) 
            : uid_(std::move(uid)), sql_(sql) {}
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
        
        MetaData(MetaType meta_type, std::vector<unsigned char> uid, std::string_view sql)
            : meta_type_(meta_type), value_(SqlUidMeta(std::move(uid), sql)) {}
        
        MetaData(MetaType meta_type, TraceId txid, int64_t span_id, std::string_view url_template, std::vector<std::unique_ptr<Exception>>&& exceptions)
            : meta_type_(meta_type), value_(ExceptionMeta(txid, span_id, url_template, std::move(exceptions))) {}
    };

    /**
     * @brief gRPC client responsible for metadata upload.
     */
    class GrpcMetadata : public GrpcClient {
    public:
        explicit GrpcMetadata(std::shared_ptr<const Config> config);
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

    private:
        struct PendingMeta {
            std::unique_ptr<MetaData> meta;
            int retry_count{0};
            std::chrono::steady_clock::time_point available_at{};
            uint64_t sequence{0};
        };

        std::unique_ptr<v1::Metadata::StubInterface> meta_stub_{};

        std::deque<PendingMeta> meta_queue_{};
        std::multimap<std::chrono::steady_clock::time_point, PendingMeta> retry_queue_{};
        std::mutex meta_queue_mutex_{};
        std::condition_variable meta_queue_cv_{};
        uint64_t meta_sequence_{0};

        template<typename Request, typename StubMethod>
        GrpcRequestStatus send_meta_helper(StubMethod stub_method, Request& request, std::string_view operation_name);
        GrpcRequestStatus send_api_meta(ApiMeta& api_meta);
        GrpcRequestStatus send_error_meta(StringMeta& error_meta);
        GrpcRequestStatus send_sql_meta(StringMeta& sql_meta);
        GrpcRequestStatus send_sql_uid_meta(SqlUidMeta& sql_uid_meta);
        GrpcRequestStatus send_exception_meta(ExceptionMeta& exception_meta);
        GrpcRequestStatus send_meta(MetaData& meta);
        void release_failed_cache(const MetaData& meta) const;
        void schedule_retry(PendingMeta&& pending);
        bool pop_next_meta(PendingMeta& pending, std::unique_lock<std::mutex>& lock);
    };

    /**
     * @brief gRPC client responsible for agent registration and ping.
     */
    class GrpcAgent : public GrpcClient, public grpc::ClientBidiReactor<v1::PPing, v1::PPing> {
    public:
        explicit GrpcAgent(std::shared_ptr<const Config> config);
        ~GrpcAgent() override;

        /**
         * @brief Registers the agent with the collector and starts ping streaming.
         */
        virtual GrpcRequestStatus registerAgent();

        /// @brief Worker loop that periodically sends ping requests.
        void sendPingWorker();
        /// @brief Stops the ping worker loop.
        void stopPingWorker();
        /// @brief Starts the scheduled AgentInfo sender loop.
        void startAgentInfo();
        /// @brief Stops the scheduled AgentInfo sender loop.
        void stopAgentInfo();
        /// @brief Requests an immediate AgentInfo refresh attempt.
        void refreshAgentInfo();

        //grpc::ClientBidiReactor
        /// @brief Notification invoked after each write completes.
        void OnWriteDone(bool ok) override;
        /// @brief Notification invoked when a server ping response is available.
        void OnReadDone(bool ok) override;
        /// @brief Final notification when the stream terminates.
        void OnDone(const grpc::Status& s) override;

    protected:
        void set_agent_stub(std::unique_ptr<v1::Agent::StubInterface> stub) { agent_stub_ = std::move(stub); }

    private:
        std::unique_ptr<v1::Agent::StubInterface> agent_stub_{};

        v1::PPing ping_{}, pong_{};
        std::mutex ping_worker_mutex_{};
        std::condition_variable ping_cv_{};
        unsigned long socket_id_{0};

        std::thread agent_info_thread_;
        std::mutex agent_info_mutex_;
        std::condition_variable agent_info_cv_;
        bool agent_info_running_{false};
        bool agent_info_stop_requested_{false};
        bool agent_info_refresh_requested_{false};

        bool start_ping_stream();
        void finish_ping_stream();
        GrpcStreamStatus write_and_await_ping_stream();

        void agent_info_worker();
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
     *   @c shared_ptr captured into the callback, so it lives exactly until
     *   the callback fires.
     *
     * ### Concurrency control (permit-based semaphore)
     * - At most @c max_concurrent_requests SendSpanBatch RPCs may be
     *   in flight at the same time.
     * - If no permit is available within @c flush_interval_ms the batch is
     *   dropped and the event is logged at INFO.
     *
     * ### Queue overflow policy
     * - When the queue is full, @c enqueueSpan discards the *oldest* chunk
     *   to make room for the new one (head-drop). This matches Java's
     *   @c LinkedBlockingQueue.poll() then offer() behavior.
     *
     * ### partial_success handling
     * - Successful responses with @c rejected_spans > 0 are logged at WARN.
     * - Responses with no rejected spans but a non-empty @c error_message
     *   are logged at INFO.
     * - Rejected spans are not retried or re-queued (observability only).
     *
     * ### Shutdown
     * - On exit the worker drains any remaining chunks into one final
     *   batch (uncapped) and then blocks up to 3 s waiting for all in-flight
     *   permits to be returned before releasing the channel.
     */
    class GrpcSpan : public GrpcClient {
    public:
        explicit GrpcSpan(std::shared_ptr<const Config> config);
        ~GrpcSpan() override = default;

        /**
         * @brief Adds a span chunk to the outbound queue.
         *
         * On overflow the oldest queued chunk is dropped to make room
         * (head-drop), matching the Java sender behavior.
         *
         * @param span Span chunk payload (ownership transferred).
         */
        void enqueueSpan(std::unique_ptr<SpanChunk> span) noexcept;
        /// @brief Worker loop that drains the queue and sends spans in batches.
        void sendSpanWorker();
        /// @brief Signals the worker loop to stop; the loop flushes pending spans before exiting.
        void stopSpanWorker();

    protected:
        void set_span_stub(std::unique_ptr<v1::Span::StubInterface> stub) { span_stub_ = std::move(stub); }

    private:
        std::unique_ptr<v1::Span::StubInterface> span_stub_{};

        std::queue<std::unique_ptr<SpanChunk>> span_queue_{};
        std::mutex span_queue_mutex_{};
        std::condition_variable span_queue_cv_{};

        // Permit-based semaphore that caps the number of concurrently in-flight
        // SendSpanBatch RPCs. Mirrors Java's Semaphore(maxConcurrentRequests).
        int batch_permits_{0};
        int batch_max_permits_{0};
        std::mutex batch_permits_mutex_{};
        std::condition_variable batch_permits_cv_{};

        void collect_batch(std::vector<std::unique_ptr<SpanChunk>>& buffer);
        void send_batch_async(std::vector<std::unique_ptr<SpanChunk>>& batch);
        bool try_acquire_permit(std::chrono::milliseconds timeout);
        bool try_acquire_all_permits(std::chrono::milliseconds timeout);
        void release_permit();
        void await_in_flight_requests();
        void flush_remaining();
    };

    /**
     * @brief gRPC client that streams agent and URL statistics to the collector.
     */
    class GrpcStats : public GrpcClient, public grpc::ClientWriteReactor<v1::PStatMessage> {
    public:
        explicit GrpcStats(std::shared_ptr<const Config> config);
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

    private:
        std::unique_ptr<v1::Stat::StubInterface> stats_stub_{};
        google::protobuf::Arena arena_{};
        v1::PStatMessage* msg_{};
        google::protobuf::Empty reply_{};

        std::queue<StatsType> stats_queue_{};
        std::mutex stats_queue_mutex_{};
        std::condition_variable stats_queue_cv_{};

        bool start_stats_stream();
        GrpcStreamStatus write_and_await_stats_stream();
        void finish_stats_stream();

        GrpcStreamStatus next_write();
        void empty_stats_queue() noexcept;
    };
}  // namespace pinpoint
