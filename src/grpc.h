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

#include <condition_variable>
#include <string>
#include <queue>

#include <grpc/grpc.h>
#include <grpcpp/alarm.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

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
    enum ClientType {AGENT, SPAN, STATS};

    /**
     * @brief Base client that encapsulates channel management shared by all gRPC workers.
     */
    class GrpcClient {
    public:
        /**
         * @brief Constructs a client bound to an `AgentService` and client type.
         *
         * @param agent Owning agent service.
         * @param client_type Which collector service this client targets.
         */
        explicit GrpcClient(AgentService* agent, ClientType client_type);
        /**
         * @brief Ensures the gRPC channel is connected and ready for use.
         *
         * @return `true` if the channel is ready or successfully re-initialized.
         */
        bool readyChannel();
        /// @brief Releases the current channel handle.
        void closeChannel() { channel_.reset(); }

    protected:
        AgentService* agent_{};
        std::shared_ptr<grpc::Channel> channel_{};
        std::mutex channel_mutex_{};
        std::string client_name_{};

        std::unique_ptr<grpc::ClientContext> stream_context_{};
        std::mutex stream_mutex_{};
        std::condition_variable stream_cv_{};
        grpc::Status stream_status_{};
        GrpcStreamStatus grpc_status_{};
        bool force_queue_empty_{false};

        /**
         * @brief Blocks until the channel becomes ready or the deadline is exceeded.
         */
        bool wait_channel_ready() const;
    };

    /**
     * @brief Metadata describing an API string cached on the collector.
     */
    typedef struct ApiMeta {
        int32_t id_;
        int32_t type_;
        std::string api_str_;
        ApiMeta(int32_t id, int32_t type, std::string_view api_str) : id_(id), type_(type), api_str_(api_str) {}
        ~ApiMeta() {}
    } ApiMeta;

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
    typedef struct StringMeta {
        int32_t id_;
        std::string str_val_;
        StringMetaType type_;
        StringMeta(int32_t id, std::string_view str_val, StringMetaType type) 
            : id_(id), str_val_(str_val), type_(type) {}
        ~StringMeta() {}
    } StringMeta;

    /**
     * @brief Metadata describing a cached SQL UID.
     */
    typedef struct SqlUidMeta {
        std::vector<unsigned char> uid_;
        std::string sql_;
        SqlUidMeta(std::vector<unsigned char> uid, std::string_view sql) 
            : uid_(std::move(uid)), sql_(sql) {}
        ~SqlUidMeta() {}
    } SqlUidMeta;

    /**
     * @brief Metadata bundle carrying exception call stacks for a completed span.
     */
    typedef struct ExceptionMeta {
        TraceId txid_;
        int64_t span_id_;
        std::string url_template_;
        std::vector<std::unique_ptr<Exception>> exceptions_;
        ExceptionMeta(TraceId txid, int64_t span_id, std::string_view url_template, std::vector<std::unique_ptr<Exception>>&& exceptions)
            : txid_(txid), span_id_(span_id), url_template_(url_template), exceptions_(std::move(exceptions)) {}
        ~ExceptionMeta() {}
    } ExceptionMeta;

    /**
     * @brief Tagged union covering all metadata variants queued by the agent.
     */
    typedef union MetaValue {
        ApiMeta api_meta_;
        StringMeta str_meta_;
        SqlUidMeta sql_uid_meta_;
        ExceptionMeta exception_meta_;
        MetaValue(int32_t id, int32_t api_type, std::string_view api_str) : api_meta_(id, api_type, api_str) {}
        MetaValue(int32_t id, std::string_view str_val, StringMetaType type) : str_meta_(id, str_val, type) {}
        MetaValue(std::vector<unsigned char> uid, std::string_view sql) : sql_uid_meta_(std::move(uid), sql) {}
        MetaValue(TraceId txid, int64_t span_id, std::string_view url_template, std::vector<std::unique_ptr<Exception>>&& exceptions) 
            : exception_meta_(txid, span_id, url_template, std::move(exceptions)) {}
        ~MetaValue() {}
    } MetaValue;

    /**
     * @brief Type discriminator for metadata payloads.
     */
    enum MetaType {META_API, META_STRING, META_SQL_UID, META_EXCEPTION};
    /**
     * @brief Metadata item queued for transmission to the collector.
     */
    typedef struct MetaData {
        MetaType meta_type_;
        MetaValue value_;
        MetaData(enum MetaType meta_type, int32_t id, int32_t api_type, std::string_view api_str)
            : meta_type_(meta_type), value_(id, api_type, api_str) {}
        MetaData(enum MetaType meta_type, int32_t id, std::string_view str_val, StringMetaType str_type)
            : meta_type_(meta_type), value_(id, str_val, str_type) {}
        MetaData(enum MetaType meta_type, std::vector<unsigned char> uid, std::string_view sql)
            : meta_type_(meta_type), value_(std::move(uid), sql) {}
        MetaData(enum MetaType meta_type, TraceId txid, int64_t span_id, std::string_view url_template, std::vector<std::unique_ptr<Exception>>&& exceptions)
            : meta_type_(meta_type), value_(txid, span_id, url_template, std::move(exceptions)) {}
        ~MetaData() {}
    } MetaData;

    /**
     * @brief gRPC client responsible for agent registration, ping and metadata upload.
     */
    class GrpcAgent : public GrpcClient, public grpc::ClientBidiReactor<v1::PPing, v1::PPing> {
    public:
        explicit GrpcAgent(AgentService* agent);

        /**
         * @brief Registers the agent with the collector and starts ping streaming.
         */
        GrpcRequestStatus registerAgent();
        /**
         * @brief Adds metadata to the outbound queue.
         *
         * @param meta Metadata payload (ownership transferred).
         */
        void enqueueMeta(std::unique_ptr<MetaData> meta) noexcept;

        /// @brief Worker loop that periodically sends ping requests.
        void sendPingWorker();
        /// @brief Stops the ping worker loop.
        void stopPingWorker();
        /// @brief Worker loop that streams metadata payloads.
        void sendMetaWorker();
        /// @brief Stops the metadata worker loop.
        void stopMetaWorker();

        //grpc::ClientBidiReactor
        /// @brief Notification invoked after each write completes.
        void OnWriteDone(bool ok) override;
        /// @brief Notification invoked when a server ping response is available.
        void OnReadDone(bool ok) override;
        /// @brief Final notification when the stream terminates.
        void OnDone(const grpc::Status& s) override;

    protected:
        void set_agent_stub(std::unique_ptr<v1::Agent::StubInterface> stub) { agent_stub_ = std::move(stub); }
        void set_meta_stub(std::unique_ptr<v1::Metadata::StubInterface> stub) { meta_stub_ = std::move(stub); }

    private:
        std::unique_ptr<v1::Agent::StubInterface> agent_stub_{};
        std::unique_ptr<v1::Metadata::StubInterface> meta_stub_{};

        v1::PPing ping_{}, pong_{};
        std::mutex ping_worker_mutex_{};
        std::condition_variable ping_cv_{};

        std::queue<std::unique_ptr<MetaData>> meta_queue_{};
        std::mutex meta_queue_mutex_{};
        std::condition_variable meta_queue_cv_{};

        bool start_ping_stream();
        void finish_ping_stream();
        GrpcStreamStatus write_and_await_ping_stream();

        GrpcRequestStatus send_api_meta(ApiMeta& api_meta);
        GrpcRequestStatus send_error_meta(StringMeta& error_meta);
        GrpcRequestStatus send_sql_meta(StringMeta& sql_meta);
        GrpcRequestStatus send_sql_uid_meta(SqlUidMeta& sql_uid_meta);
        GrpcRequestStatus send_exception_meta(ExceptionMeta& exception_meta);
    };

    /**
     * @brief gRPC client that streams span chunks to the collector.
     */
    class GrpcSpan : public GrpcClient, public grpc::ClientWriteReactor<v1::PSpanMessage> {
    public:
        explicit GrpcSpan(AgentService* agent);

        /**
         * @brief Adds a span chunk to the outbound queue.
         *
         * @param span Span chunk payload (ownership transferred).
         */
        void enqueueSpan(std::unique_ptr<SpanChunk> span) noexcept;
        /// @brief Worker loop that streams queued spans.
        void sendSpanWorker();
        /// @brief Stops the span worker loop and drains the queue.
        void stopSpanWorker();

        //grpc::ClientWriteReactor
        /// @brief Invoked when a write completes on the stream.
        void OnWriteDone(bool ok) override;
        /// @brief Invoked when the stream finishes or errors.
        void OnDone(const grpc::Status& status) override;

    protected:
        void set_span_stub(std::unique_ptr<v1::Span::StubInterface> stub) { span_stub_ = std::move(stub); }
        
    private:
        std::unique_ptr<v1::Span::StubInterface> span_stub_{};
        std::unique_ptr<v1::PSpanMessage> msg_{};
        google::protobuf::Empty reply_{};

        std::queue<std::unique_ptr<SpanChunk>> span_queue_{};
        std::mutex span_queue_mutex_{};
        std::condition_variable span_queue_cv_{};

        bool start_span_stream();
        GrpcStreamStatus write_and_await_span_stream();
        void finish_span_stream();

        GrpcStreamStatus next_write();
        void empty_span_queue() noexcept;
    };

    /**
     * @brief gRPC client that streams agent and URL statistics to the collector.
     */
    class GrpcStats : public GrpcClient, public grpc::ClientWriteReactor<v1::PStatMessage> {
    public:
        explicit GrpcStats(AgentService* agent);

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
        std::unique_ptr<v1::PStatMessage> msg_{};
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
