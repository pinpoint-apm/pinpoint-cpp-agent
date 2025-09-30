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
#include "span.h"

namespace pinpoint {
    enum GrpcRequestStatus {SEND_OK, SEND_FAIL};
    enum GrpcStreamStatus {STREAM_WRITE, STREAM_CONTINUE, STREAM_DONE, STREAM_EXCEPTION};
    enum ClientType {AGENT, SPAN, STATS};

    class GrpcClient {
    public:
        explicit GrpcClient(AgentService* agent, ClientType client_type);
        bool readyChannel();
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

        bool wait_channel_ready() const;
    };

    typedef struct ApiMeta {
        int32_t id_;
        int32_t type_;
        std::string api_str_;
        ApiMeta(int32_t id, int32_t type, std::string_view api_str) : id_(id), type_(type), api_str_(api_str) {}
        ~ApiMeta() {}
    } ApiMeta;

    enum StringMetaType {
        STRING_META_ERROR,
        STRING_META_SQL
    };

    typedef struct StringMeta {
        int32_t id_;
        std::string str_val_;
        StringMetaType type_;
        StringMeta(int32_t id, std::string_view str_val, StringMetaType type) 
            : id_(id), str_val_(str_val), type_(type) {}
        ~StringMeta() {}
    } StringMeta;

    typedef union MetaValue {
        ApiMeta api_meta_;
        StringMeta str_meta_;
        MetaValue(int32_t id, int32_t api_type, std::string_view api_str) : api_meta_(id, api_type, api_str) {}
        MetaValue(int32_t id, std::string_view str_val, StringMetaType type) : str_meta_(id, str_val, type) {}
        ~MetaValue() {}
    } MetaValue;

    enum MetaType {META_API, META_STRING};
    typedef struct MetaData {
        MetaType meta_type_;
        MetaValue value_;
        MetaData(enum MetaType meta_type, int32_t id, int32_t api_type, std::string_view api_str)
            : meta_type_(meta_type), value_(id, api_type, api_str) {}
        MetaData(enum MetaType meta_type, int32_t id, std::string_view str_val, StringMetaType str_type)
            : meta_type_(meta_type), value_(id, str_val, str_type) {}
        ~MetaData() {}
    } MetaData;

    class GrpcAgent : public GrpcClient, public grpc::ClientBidiReactor<v1::PPing, v1::PPing> {
    public:
        explicit GrpcAgent(AgentService* agent);

        GrpcRequestStatus registerAgent();
        void enqueueMeta(std::unique_ptr<MetaData> meta) noexcept;

        void sendPingWorker();
        void stopPingWorker();
        void sendMetaWorker();
        void stopMetaWorker();

        //grpc::ClientBidiReactor
        void OnWriteDone(bool ok) override;
        void OnReadDone(bool ok) override;
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
    };

    class GrpcSpan : public GrpcClient, public grpc::ClientWriteReactor<v1::PSpanMessage> {
    public:
        explicit GrpcSpan(AgentService* agent);

        void enqueueSpan(std::unique_ptr<SpanChunk> span) noexcept;
        void sendSpanWorker();
        void stopSpanWorker();

        //grpc::ClientWriteReactor
        void OnWriteDone(bool ok) override;
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

    class GrpcStats : public GrpcClient, public grpc::ClientWriteReactor<v1::PStatMessage> {
    public:
        explicit GrpcStats(AgentService* agent);

        void enqueueStats(StatsType stats) noexcept;
        void sendStatsWorker();
        void stopStatsWorker();

        //grpc::ClientWriteReactor
        void OnWriteDone(bool ok) override;
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
