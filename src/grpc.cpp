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
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <grpcpp/client_context.h>

#include "absl/strings/str_cat.h"
#include "annotation.h"
#include "version.h"
#include "logging.h"
#include "stat.h"
#include "grpc.h"

namespace pinpoint {

    static v1::PTransactionId* build_transaction_id(TraceId& tid, google::protobuf::Arena* arena) {
        auto& [agent_id, start_time, sequence] = tid;
        auto* ptid = google::protobuf::Arena::Create<v1::PTransactionId>(arena);

        ptid->set_agentid(agent_id);
        ptid->set_agentstarttime(start_time);
        ptid->set_sequence(sequence);

        return ptid;
    }

    static v1::PAcceptEvent* build_accept_event(SpanData *span, google::protobuf::Arena* arena) {
        auto* accept_event = google::protobuf::Arena::Create<v1::PAcceptEvent>(arena);

        accept_event->set_endpoint(span->getEndPoint());
        accept_event->set_rpc(span->getRpcName());

        if (auto& remote_addr = span->getRemoteAddr(); !remote_addr.empty()) {
            accept_event->set_remoteaddr(remote_addr);
        }

        if (!span->getParentAppName().empty()) {
            auto* parent_info = google::protobuf::Arena::Create<v1::PParentInfo>(arena);

            parent_info->set_parentapplicationname(span->getParentAppName());
            parent_info->set_parentapplicationtype(span->getParentAppType());
            parent_info->set_acceptorhost(span->getAcceptorHost());
            accept_event->unsafe_arena_set_allocated_parentinfo(parent_info);
        }

        return accept_event;
    }

    static void build_annotation(v1::PAnnotation *annotation, int32_t key,
                               const AnnotationData& val,
                               google::protobuf::Arena* arena) {
        annotation->set_key(key);
        auto* annotation_value = google::protobuf::Arena::Create<v1::PAnnotationValue>(arena);

        if (val.dataType == ANNOTATION_TYPE_INT) {
            annotation_value->set_intvalue(std::get<int32_t>(val.data));
        } else if (val.dataType == ANNOTATION_TYPE_LONG) {
            annotation_value->set_longvalue(std::get<int64_t>(val.data));
        } else if (val.dataType == ANNOTATION_TYPE_STRING) {
            annotation_value->set_stringvalue(std::get<std::string>(val.data));
        } else if (val.dataType == ANNOTATION_TYPE_STRING_STRING) {
            const auto& v = std::get<StringStringValue>(val.data);
            auto* ssv = google::protobuf::Arena::Create<v1::PStringStringValue>(arena);
            auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s1->set_value(v.stringValue1);
            ssv->unsafe_arena_set_allocated_stringvalue1(s1);

            auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s2->set_value(v.stringValue2);
            ssv->unsafe_arena_set_allocated_stringvalue2(s2);

            annotation_value->unsafe_arena_set_allocated_stringstringvalue(ssv);
        } else if (val.dataType == ANNOTATION_TYPE_INT_STRING_STRING) {
            const auto& v = std::get<IntStringStringValue>(val.data);
            auto* issv = google::protobuf::Arena::Create<v1::PIntStringStringValue>(arena);
            issv->set_intvalue(v.intValue);

            auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s1->set_value(v.stringValue1);
            issv->unsafe_arena_set_allocated_stringvalue1(s1);

            auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s2->set_value(v.stringValue2);
            issv->unsafe_arena_set_allocated_stringvalue2(s2);

            annotation_value->unsafe_arena_set_allocated_intstringstringvalue(issv);
        } else if (val.dataType == ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING) {
            const auto& v = std::get<LongIntIntByteByteStringValue>(val.data);
            auto* liibbsv = google::protobuf::Arena::Create<v1::PLongIntIntByteByteStringValue>(arena);
            liibbsv->set_longvalue(v.longValue);
            liibbsv->set_intvalue1(v.intValue1);
            liibbsv->set_intvalue2(v.intValue2);
            liibbsv->set_bytevalue1(v.byteValue1);
            liibbsv->set_bytevalue2(v.byteValue2);

            auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s->set_value(v.stringValue);
            liibbsv->unsafe_arena_set_allocated_stringvalue(s);

            annotation_value->unsafe_arena_set_allocated_longintintbytebytestringvalue(liibbsv);
        } else if (val.dataType == ANNOTATION_TYPE_BYTES_STRING_STRING) {
            const auto& v = std::get<BytesStringStringValue>(val.data);
            auto* bssv = google::protobuf::Arena::Create<v1::PBytesStringStringValue>(arena);

            // convert vector<unsigned char> to string (copy)
            std::string_view bytes_view(
                reinterpret_cast<const char*>(v.bytesValue.data()),
                v.bytesValue.size()
            );
            bssv->set_bytesvalue(std::string(bytes_view));

            auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s1->set_value(v.stringValue1);
            bssv->unsafe_arena_set_allocated_stringvalue1(s1);

            auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s2->set_value(v.stringValue2);
            bssv->unsafe_arena_set_allocated_stringvalue2(s2);

            annotation_value->unsafe_arena_set_allocated_bytesstringstringvalue(bssv);
        }
        annotation->unsafe_arena_set_allocated_value(annotation_value);
    }

    static void build_string_annotation(v1::PAnnotation *annotation, int32_t key, std::string& val, google::protobuf::Arena* arena) {
        annotation->set_key(key);
        const auto annotation_value = google::protobuf::Arena::Create<v1::PAnnotationValue>(arena);
        annotation_value->set_stringvalue(val);
        annotation->unsafe_arena_set_allocated_value(annotation_value);
    }

    static void build_span_event(v1::PSpanEvent* span_event, 
                               const std::shared_ptr<SpanEventImpl>& se,
                               google::protobuf::Arena* arena) {
        span_event->set_sequence(se->getSequence());
        span_event->set_depth(se->getDepth());
        span_event->set_startelapsed(se->getStartElapsed());
        span_event->set_endelapsed(se->getEndElapsed());
        span_event->set_servicetype(se->getServiceType());
        span_event->set_asyncevent(se->getAsyncId());

        if (!se->getDestinationId().empty()) {
            auto* next_event = google::protobuf::Arena::Create<v1::PNextEvent>(arena);
            auto* message_event = google::protobuf::Arena::Create<v1::PMessageEvent>(arena);

            message_event->set_nextspanid(se->getNextSpanId());
            message_event->set_destinationid(se->getDestinationId());
            next_event->unsafe_arena_set_allocated_messageevent(message_event);
            span_event->unsafe_arena_set_allocated_nextevent(next_event);
        }

        if (auto api_id = se->getApiId(); api_id > 0) {
            span_event->set_apiid(api_id);
        } else {
            auto operation_name = se->getOperationName();
            build_string_annotation(span_event->add_annotation(), ANNOTATION_API, operation_name, arena);
        }

        auto& annotations = se->getAnnotations()->getAnnotations();
        for (const auto& [key, val] : annotations) {
            build_annotation(span_event->add_annotation(), key, val, arena);
        }

        if (auto err_str = se->getErrorString(); !err_str.empty()) {
            auto* exceptInfo = google::protobuf::Arena::Create<v1::PIntStringValue>(arena);
            exceptInfo->set_intvalue(se->getErrorFuncId());

            auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s->set_value(err_str);
            exceptInfo->unsafe_arena_set_allocated_stringvalue(s);
            span_event->unsafe_arena_set_allocated_exceptioninfo(exceptInfo);
        }
    }

    static v1::PSpan* build_grpc_span(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena) {
        const auto span = chunk->getSpanData().get();
        auto* grpc_span = google::protobuf::Arena::Create<v1::PSpan>(arena);

        grpc_span->set_version(1);
        auto* tid = build_transaction_id(span->getTraceId(), arena);
        grpc_span->unsafe_arena_set_allocated_transactionid(tid);

        grpc_span->set_spanid(span->getSpanId());
        grpc_span->set_parentspanid(span->getParentSpanId());
        grpc_span->set_starttime(span->getStartTime());
        grpc_span->set_elapsed(span->getElapsed());
        grpc_span->set_servicetype(span->getServiceType());
        grpc_span->set_applicationservicetype(span->getAppType());

        auto* accept_event = build_accept_event(span, arena);
        grpc_span->unsafe_arena_set_allocated_acceptevent(accept_event);

        if (auto api_id = span->getApiId(); api_id > 0) {
            grpc_span->set_apiid(api_id);
        } else {
            build_string_annotation(grpc_span->add_annotation(), ANNOTATION_API, span->getOperationName(), arena);
        }
        grpc_span->set_loggingtransactioninfo(span->getLoggingFlag());
        grpc_span->set_flag(span->getFlags());
        grpc_span->set_err(span->getErr());

        const auto& events = chunk->getSpanEventChunk();
        for (const auto& e : events) {
            build_span_event(grpc_span->add_spanevent(), e, arena);
        }

        const auto& annotations = span->getAnnotations()->getAnnotations();
        for (const auto& [key, val] : annotations) {
            build_annotation(grpc_span->add_annotation(), key, val, arena);
        }

        if (auto err_str = span->getErrorString(); !err_str.empty()) {
            auto* exceptInfo = google::protobuf::Arena::Create<v1::PIntStringValue>(arena);
            exceptInfo->set_intvalue(span->getErrorFuncId());

            auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s->set_value(err_str);
            exceptInfo->unsafe_arena_set_allocated_stringvalue(s);
            grpc_span->unsafe_arena_set_allocated_exceptioninfo(exceptInfo);
        }

        return grpc_span;
    }

    static v1::PSpanChunk* build_grpc_span_chunk(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena) {
        const auto span = chunk->getSpanData().get();
        auto* grpc_span = google::protobuf::Arena::Create<v1::PSpanChunk>(arena);
        grpc_span->set_version(1);

        auto* tid = build_transaction_id(span->getTraceId(), arena);
        grpc_span->unsafe_arena_set_allocated_transactionid(tid);

        grpc_span->set_spanid(span->getSpanId());
        grpc_span->set_keytime(chunk->getKeyTime());
        grpc_span->set_endpoint(span->getEndPoint());
        grpc_span->set_applicationservicetype(span->getAppType());

        if (span->isAsyncSpan()) {
            auto* aid = google::protobuf::Arena::Create<v1::PLocalAsyncId>(arena);
            aid->set_asyncid(span->getAsyncId());
            aid->set_sequence(span->getAsyncSequence());
            grpc_span->unsafe_arena_set_allocated_localasyncid(aid);
        }

        auto& events = chunk->getSpanEventChunk();
        for (const auto& e : events) {
            build_span_event(grpc_span->add_spanevent(), e, arena);
        }

        return grpc_span;
    }

    static void build_agent_stat(v1::PAgentStat *agent_stat, const AgentStatsSnapshot& stat, google::protobuf::Arena* arena) {
        agent_stat->set_timestamp(stat.sample_time_);
        agent_stat->set_collectinterval(stat.interval_);

        auto* memory_stat = google::protobuf::Arena::Create<v1::PJvmGc>(arena);
        memory_stat->set_type(v1::JVM_GC_TYPE_UNKNOWN);
        memory_stat->set_jvmmemoryheapused(stat.heap_alloc_size_);
        memory_stat->set_jvmmemoryheapmax(stat.heap_max_size_);
        memory_stat->set_jvmmemorynonheapused(0);
        memory_stat->set_jvmmemorynonheapmax(0);
        memory_stat->set_jvmgcoldcount(0);
        memory_stat->set_jvmgcoldtime(0);
        agent_stat->unsafe_arena_set_allocated_gc(memory_stat);

        auto* cpu_load = google::protobuf::Arena::Create<v1::PCpuLoad>(arena);
        cpu_load->set_jvmcpuload(stat.process_cpu_time_);
        cpu_load->set_systemcpuload(stat.system_cpu_time_);
        agent_stat->unsafe_arena_set_allocated_cpuload(cpu_load);

        auto* tran = google::protobuf::Arena::Create<v1::PTransaction>(arena);
        tran->set_samplednewcount(stat.num_sample_new_);
        tran->set_sampledcontinuationcount(stat.num_sample_cont_);
        tran->set_unsamplednewcount(stat.num_unsample_new_);
        tran->set_unsampledcontinuationcount(stat.num_unsample_cont_);
        tran->set_skippednewcount(stat.num_skip_new_);
        tran->set_skippedcontinuationcount(stat.num_skip_cont_);
        agent_stat->unsafe_arena_set_allocated_transaction(tran);

        auto* active_trace = google::protobuf::Arena::Create<v1::PActiveTrace>(arena);
        auto* histogram = google::protobuf::Arena::Create<v1::PActiveTraceHistogram>(arena);
        histogram->set_version(1);
        histogram->set_histogramschematype(2);
        for (int32_t c : stat.active_requests_) {
            histogram->add_activetracecount(c);
        }
        active_trace->unsafe_arena_set_allocated_histogram(histogram);
        agent_stat->unsafe_arena_set_allocated_activetrace(active_trace);

        auto* response_time = google::protobuf::Arena::Create<v1::PResponseTime>(arena);
        response_time->set_avg(stat.response_time_avg_);
        response_time->set_max(stat.response_time_max_);
        agent_stat->unsafe_arena_set_allocated_responsetime(response_time);

        auto* total_thread = google::protobuf::Arena::Create<v1::PTotalThread>(arena);
        total_thread->set_totalthreadcount(stat.num_threads_);
        agent_stat->unsafe_arena_set_allocated_totalthread(total_thread);
    }

    static v1::PAgentStatBatch* build_agent_stat_batch(const std::vector<AgentStatsSnapshot>& stats, google::protobuf::Arena* arena) {
        auto* grpc_stat = google::protobuf::Arena::Create<v1::PAgentStatBatch>(arena);

        for (const auto& stat : stats) {
            auto* agent_stat = grpc_stat->add_agentstat();
            build_agent_stat(agent_stat, stat, arena);
        }

        return grpc_stat;
    }

    static void build_url_histogram(v1::PUriHistogram *grpc_histogram, const UrlStatHistogram& url_histogram) {
        grpc_histogram->set_total(url_histogram.total());
        grpc_histogram->set_max(url_histogram.max());
        for (auto i = 0; i < URL_STATS_BUCKET_SIZE; i++) {
            grpc_histogram->add_histogram(url_histogram.histogram(i));
        }
    }

    static void build_each_url_stat(v1::PEachUriStat *url_stat, const UrlKey& key, EachUrlStat *each_stats, 
                                    google::protobuf::Arena* arena) {
        url_stat->set_uri(key.url_);

        auto* total = google::protobuf::Arena::Create<v1::PUriHistogram>(arena);
        build_url_histogram(total, each_stats->getTotalHistogram());
        url_stat->unsafe_arena_set_allocated_totalhistogram(total);

        auto* fail = google::protobuf::Arena::Create<v1::PUriHistogram>(arena);
        build_url_histogram(fail, each_stats->getFailHistogram());
        url_stat->unsafe_arena_set_allocated_failedhistogram(fail);

        url_stat->set_timestamp(each_stats->tick());
    }

    static v1::PAgentUriStat* build_url_stat(const UrlStatSnapshot* snapshot, google::protobuf::Arena* arena) {
        auto* uri_stat = google::protobuf::Arena::Create<v1::PAgentUriStat>(arena);

        uri_stat->set_bucketversion(URL_STATS_BUCKET_VERSION);
        const auto& m = snapshot->getEachStats();
        for(const auto& [key, each_stats] : m) {
            auto* url_stat = uri_stat->add_eachuristat();
            build_each_url_stat(url_stat, key, each_stats.get(), arena);
        }
        return uri_stat;
    }

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
                const Config::GrpcChannelOptions& channel_options,
                std::string_view client_name) {
            if (!channel_options.ssl_enable) {
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
            const auto& options = config.grpc.channel;
            const auto client_name = grpc_client_name(client_type);
            const auto addr = absl::StrCat(config.collector.host, ":", grpc_collector_port(config, client_type));
            auto credentials = make_channel_credentials(config.grpc.ssl, options, client_name);
            auto channel_args = make_channel_arguments(options);

            LOG_INFO("create {} grpc channel: addr={}, ssl={}, keepaliveTimeMs={}, keepaliveTimeoutMs={}, "
                     "maxSendMessageSize={}, maxReceiveMessageSize={}",
                     client_name, addr, options.ssl_enable, options.keepalive_time_ms,
                     options.keepalive_timeout_ms, options.max_send_message_size,
                     options.max_receive_message_size);
            return grpc::CreateCustomChannel(addr, credentials, channel_args);
        }

        constexpr int GRPC_REQUEST_TIMEOUT_MS = 5000;

        static void set_request_deadline(grpc::ClientContext& ctx) {
            auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(GRPC_REQUEST_TIMEOUT_MS);
            ctx.set_deadline(deadline);
        }
    }

    GrpcClient::GrpcClient(ClientType client_type, std::shared_ptr<const Config> config)
        : config_{std::move(config)}, client_type_(client_type) {
        client_name_ = grpc_client_name(client_type_);
        channel_ = build_channel(*config_, client_type_);
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

    void GrpcClient::build_grpc_context(grpc::ClientContext* context, unsigned long socket_id) const {
        assert(agent_ != nullptr && "setAgentService() must be called before build_grpc_context()");
        context->AddMetadata(METADATA_APPLICATION_NAME, config_->app_name_);
        context->AddMetadata(METADATA_AGENT_ID, config_->agent_id_);
        context->AddMetadata(METADATA_START_TIME, std::to_string(agent_->getStartTime()));
        context->AddMetadata(METADATA_SERVICE_TYPE, std::to_string(config_->app_type_));

        if (!config_->agent_name_.empty()) {
            context->AddMetadata(METADATA_AGENT_NAME, config_->agent_name_);
        }
        if (socket_id > 0) {
            context->AddMetadata(METADATA_SOCKET_ID, std::to_string(socket_id));
        }
    }

    bool GrpcClient::wait_channel_ready() const {
        auto state = channel_->GetState(true);
        if (state == GRPC_CHANNEL_READY) {
            return true;
        }

        LOG_INFO("wait {} grpc channel ready: state = {}", client_name_, static_cast<int>(state));

        for (int retry = 0; state != GRPC_CHANNEL_READY && retry < 12; retry++) {
            if (agent_->isExiting()) {
                return false;
            }
            channel_->WaitForStateChange(state, std::chrono::system_clock::now() + std::chrono::seconds(5));
            state = channel_->GetState(false);
        }

        return state == GRPC_CHANNEL_READY;
    }

    bool GrpcClient::readyChannel() {
        std::unique_lock<std::mutex> lock(channel_mutex_);
        if (agent_->isExiting()) {
            return false;
        }

        auto now = std::chrono::system_clock::now();
        if (channel_->GetState(false) != GRPC_CHANNEL_READY) {
            while (true) {
                if (agent_->isExiting()) {
                    return false;
                }
                if (wait_channel_ready()) {
                    break;
                }
            }
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - now).count() >= 5) {
            force_queue_empty_ = true;
        }
        return true;
    }

    constexpr int METADATA_RETRY_MAX_ATTEMPTS = 3;
    constexpr auto METADATA_RETRY_DELAY = std::chrono::milliseconds(1000);

    //GrpcMetadata

    GrpcMetadata::GrpcMetadata(std::shared_ptr<const Config> config) : GrpcClient(METADATA, std::move(config)) {
        set_meta_stub(v1::Metadata::NewStub(channel_));
    }

    template<typename Request, typename StubMethod>
    GrpcRequestStatus GrpcMetadata::send_meta_helper(StubMethod stub_method, Request& request, std::string_view operation_name) {
        v1::PResult reply;
        grpc::ClientContext ctx;
        std::unique_lock<std::mutex> lock(channel_mutex_);

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

        // convert vector<unsigned char> to string (copy)
        std::string_view uid_view(
            reinterpret_cast<const char*>(sql_uid_meta.uid_.data()),
            sql_uid_meta.uid_.size()
        );
        grpc_sql_uid_meta.set_sqluid(std::string(uid_view));
        grpc_sql_uid_meta.set_sql(sql_uid_meta.sql_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PSqlUidMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestSqlUidMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_sql_uid_meta, "sql uid");
    }

    GrpcRequestStatus GrpcMetadata::send_exception_meta(ExceptionMeta& exception_meta) {
        google::protobuf::Arena arena;
        auto* grpc_exception_meta = google::protobuf::Arena::Create<v1::PExceptionMetaData>(&arena);

        grpc_exception_meta->unsafe_arena_set_allocated_transactionid(build_transaction_id(exception_meta.txid_, &arena));
        grpc_exception_meta->set_spanid(exception_meta.span_id_);
        grpc_exception_meta->set_uritemplate(exception_meta.url_template_);

        for (const auto& exception : exception_meta.exceptions_) {
            auto* grpc_exception = grpc_exception_meta->add_exceptions();
            const auto& callstack = exception->getCallStack();

            grpc_exception->set_exceptionid(exception->getId());
            grpc_exception->set_exceptionclassname(callstack.getModuleName());
            grpc_exception->set_exceptionmessage(callstack.getErrorMessage());
            grpc_exception->set_starttime(callstack.getErrorTime());
            grpc_exception->set_exceptiondepth(1);

            for (const auto& frame : callstack.getStack()) {
                auto* grpc_callstack = grpc_exception->add_stacktraceelement();

                grpc_callstack->set_classname(frame.module);
                grpc_callstack->set_filename(frame.file);
                grpc_callstack->set_linenumber(frame.line);
                grpc_callstack->set_methodname(frame.function);
            }
        }

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

        std::unique_lock<std::mutex> lock(meta_queue_mutex_);

        const auto max_queue_size = static_cast<size_t>(config_->grpc.channel.sender_queue_size);
        if (meta_queue_.size() + retry_queue_.size() < max_queue_size) {
            meta_queue_.push_back(PendingMeta{std::move(meta), 0, {}, meta_sequence_++});
        } else {
            LOG_DEBUG("drop metadata: overflow max queue size {}", max_queue_size);
        }

        meta_queue_cv_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue metadata: exception = {}", e.what());
    }

    void GrpcMetadata::schedule_retry(PendingMeta&& pending) {
        pending.available_at = std::chrono::steady_clock::now() + METADATA_RETRY_DELAY;
        pending.sequence = meta_sequence_++;
        retry_queue_.emplace(pending.available_at, std::move(pending));
        meta_queue_cv_.notify_one();
    }

    bool GrpcMetadata::pop_next_meta(PendingMeta& pending, std::unique_lock<std::mutex>& lock) {
        while (true) {
            if (agent_->isExiting()) {
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
                    return !meta_queue_.empty() || agent_->isExiting();
                });
            } else {
                meta_queue_cv_.wait(lock, [this] {
                    return !meta_queue_.empty() || !retry_queue_.empty() || agent_->isExiting();
                });
            }
        }
    }

    void GrpcMetadata::sendMetaWorker() try {
        while (true) {
            PendingMeta pending;
            {
                std::unique_lock<std::mutex> lock(meta_queue_mutex_);
                if (!pop_next_meta(pending, lock)) {
                    break;
                }
            }

            const auto sent = send_meta(*pending.meta) == SEND_OK;
            if (sent) {
                continue;
            }

            ++pending.retry_count;
            if (agent_->isExiting()) {
                break;
            }

            std::unique_lock<std::mutex> lock(meta_queue_mutex_);
            if (pending.retry_count <= METADATA_RETRY_MAX_ATTEMPTS) {
                LOG_DEBUG("retry metadata send: retryCount={}/{}", pending.retry_count, METADATA_RETRY_MAX_ATTEMPTS);
                schedule_retry(std::move(pending));
            } else {
                LOG_INFO("drop metadata after retry exhaustion: retryCount={}", pending.retry_count);
                release_failed_cache(*pending.meta);
            }
        }
        LOG_INFO("send meta worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("failed to send grpc meta: exception = {}", e.what());
    }

    void GrpcMetadata::stopMetaWorker() {
        std::unique_lock<std::mutex> lock(meta_queue_mutex_);
        meta_queue_cv_.notify_all();
    }

    //GrpcCommand

    namespace {
        constexpr int ACTIVE_TRACE_HISTOGRAM_SCHEMA_TYPE = 2;
        constexpr auto COMMAND_RECONNECT_DELAY = std::chrono::milliseconds(1000);
        constexpr auto ACTIVE_THREAD_COUNT_FLUSH_DELAY = std::chrono::milliseconds(1000);

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
            stop_requested_ = true;
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
                cv_.wait_for(lock, ACTIVE_THREAD_COUNT_FLUSH_DELAY, [this] {
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
                context_.reset();
            }
            LOG_ERROR("active thread count stream exception = {}", e.what());
            done_ = true;
        }
    };

    GrpcCommand::GrpcCommand(std::shared_ptr<const Config> config) : GrpcClient(AGENT, std::move(config)) {
        set_command_stub(v1::ProfilerCommandService::NewStub(channel_));
        register_default_handlers();
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

        add_active_thread_count_stream(request.requestid());
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

    void GrpcCommand::add_active_thread_count_stream(int32_t request_id) {
        std::unique_lock<std::mutex> lock(active_streams_mutex_);
        cleanup_active_thread_count_streams();

        auto stream = std::make_unique<ActiveThreadCountStream>(this, ++socket_id_, request_id);
        stream->start();
        active_thread_count_streams_.push_back(std::move(stream));
        LOG_INFO("active thread count stream started: requestId={}", request_id);
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
            return agent_->isExiting();
        });
    }

    void GrpcCommand::commandWorker() try {
        while (!agent_->isExiting()) {
            if (!readyChannel()) {
                break;
            }

            grpc::ClientContext context;
            build_command_context(&context, ++socket_id_);
            const auto command_codes = dispatcher_.supportedCommandCodes();
            context.AddMetadata(METADATA_SUPPORT_COMMAND_CODE, support_command_code_header(command_codes));

            {
                std::unique_lock<std::mutex> lock(command_worker_mutex_);
                command_stream_context_ = &context;
            }

            LOG_INFO("connect to command service stream");
            auto stream = command_stub_->HandleCommandV2(&context);
            if (stream == nullptr) {
                LOG_WARN("failed to connect to command service stream: stream is null");
            } else {
                v1::PCmdRequest request;
                while (!agent_->isExiting() && stream->Read(&request)) {
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
                if (!status.ok() && !agent_->isExiting()) {
                    LOG_INFO("command service stream closed: {}, {}",
                             static_cast<int>(status.error_code()), status.error_message());
                } else {
                    LOG_INFO("command service stream completed");
                }
            }

            {
                std::unique_lock<std::mutex> lock(command_worker_mutex_);
                command_stream_context_ = nullptr;
            }
            {
                std::unique_lock<std::mutex> lock(active_streams_mutex_);
                cleanup_active_thread_count_streams();
            }

            if (agent_->isExiting() || wait_reconnect_delay(COMMAND_RECONNECT_DELAY)) {
                break;
            }
        }
        stop_active_thread_count_streams();
        LOG_INFO("grpc command worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("grpc command worker exception = {}", e.what());
        stop_active_thread_count_streams();
    }

    void GrpcCommand::stopCommandWorker() {
        cancel_command_stream();
        stop_active_thread_count_streams();
    }

    //GrpcAgent

    GrpcAgent::GrpcAgent(std::shared_ptr<const Config> config) : GrpcClient(AGENT, std::move(config)) {
        set_agent_stub(v1::Agent::NewStub(channel_));
    }

    GrpcAgent::~GrpcAgent() {
        stopAgentInfo();
    }

    void GrpcAgent::build_agent_info(v1::PAgentInfo* agent_info, google::protobuf::Arena* arena) const {
        agent_info->set_hostname(get_host_name());
        agent_info->set_ip(get_host_ip_addr());
        agent_info->set_servicetype(config_->app_type_);
        agent_info->set_pid(getpid());
        agent_info->set_agentversion(VERSION_STRING);
        agent_info->set_container(config_->is_container);

        const auto meta_data = google::protobuf::Arena::Create<v1::PServerMetaData>(arena);
        meta_data->set_serverinfo("C/C++");
        meta_data->add_vmarg(to_config_string(*config_));

        agent_info->unsafe_arena_set_allocated_servermetadata(meta_data);
    }

    GrpcRequestStatus GrpcAgent::registerAgent() {
        grpc::ClientContext ctx;
        v1::PResult reply;

        std::unique_lock<std::mutex> lock(channel_mutex_);
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

    void GrpcAgent::startAgentInfo() {
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        if (agent_info_running_) {
            return;
        }
        agent_info_stop_requested_ = false;
        agent_info_refresh_requested_ = false;
        agent_info_running_ = true;
        agent_info_thread_ = std::thread{&GrpcAgent::agent_info_worker, this};
    }

    void GrpcAgent::stopAgentInfo() {
        {
            std::unique_lock<std::mutex> lock(agent_info_mutex_);
            if (!agent_info_running_ && !agent_info_thread_.joinable()) {
                return;
            }
            agent_info_stop_requested_ = true;
            agent_info_cv_.notify_all();
        }
        if (agent_info_thread_.joinable()) {
            agent_info_thread_.join();
        }
        {
            std::unique_lock<std::mutex> lock(agent_info_mutex_);
            agent_info_running_ = false;
            agent_info_refresh_requested_ = false;
        }
        LOG_INFO("AgentInfo scheduler stopped");
    }

    void GrpcAgent::refreshAgentInfo() {
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        if (!agent_info_running_) {
            return;
        }
        agent_info_refresh_requested_ = true;
        agent_info_cv_.notify_all();
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
            agent_->onAgentInfoSent();
            return true;
        }
        LOG_WARN("failed to send AgentInfo");
        return false;
    }

    bool GrpcAgent::send_agent_info_with_retries(const int max_try_count) {
        const auto retry_interval = std::chrono::milliseconds(config_->agent_info.send_retry_interval_ms);
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
        agent_info_cv_.wait_for(lock, delay, [this] { return should_stop_agent_info() || agent_info_refresh_requested_; });
        return should_stop_agent_info();
    }

    bool GrpcAgent::wait_agent_info_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(agent_info_mutex_);
        agent_info_cv_.wait_until(lock, deadline, [this] { return should_stop_agent_info() || agent_info_refresh_requested_; });
        return should_stop_agent_info();
    }

    void GrpcAgent::agent_info_worker() try {
        if (!send_agent_info_with_retries(std::numeric_limits<int>::max())) {
            return;
        }

        const auto refresh_interval = std::chrono::milliseconds(config_->agent_info.refresh_interval_ms);
        auto next_refresh = std::chrono::steady_clock::now() + refresh_interval;
        while (true) {
            if (wait_agent_info_until(next_refresh)) {
                return;
            }

            {
                std::unique_lock<std::mutex> lock(agent_info_mutex_);
                agent_info_refresh_requested_ = false;
            }

            send_agent_info_with_retries(config_->agent_info.max_try_per_attempt);
            next_refresh = std::chrono::steady_clock::now() + refresh_interval;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("AgentInfo scheduler exception = {}", e.what());
    }
    // Ping Stream

    bool GrpcAgent::start_ping_stream() {
        LOG_DEBUG("start_ping_stream");
        if (!readyChannel()) {
            return false;
        }

        stream_context_ = std::make_unique<grpc::ClientContext>();
        build_grpc_context(stream_context_.get(), ++socket_id_);
        agent_stub_->async()->PingSession(stream_context_.get(), this);

        AddHold();
        StartRead(&pong_);
        StartCall();

        return true;
    }

    void GrpcAgent::finish_ping_stream() {
        LOG_DEBUG("finish_ping_stream");
        StartWritesDone();
        RemoveHold();

        std::unique_lock<std::mutex> lock(stream_mutex_);
        stream_cv_.wait(lock, [this] {
            return grpc_status_ == STREAM_DONE;
        });
    }

    GrpcStreamStatus GrpcAgent::write_and_await_ping_stream() {
        LOG_DEBUG("write_and_await_ping_stream");
        std::unique_lock<std::mutex> lock(stream_mutex_);

        grpc_status_ = STREAM_WRITE;
        StartWrite(&ping_);
        stream_cv_.wait(lock, [this] { return grpc_status_ != STREAM_WRITE; });

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
            StartWritesDone();
            RemoveHold();
        }
    }

    void GrpcAgent::OnReadDone(bool ok) {
        LOG_DEBUG("ping - OnReadDone : {}", ok);

        if (ok) {
            StartRead(&pong_);

            std::unique_lock<std::mutex> lock(stream_mutex_);
            grpc_status_ = STREAM_CONTINUE;
            stream_cv_.notify_one();
        }
    }

    void GrpcAgent::OnDone(const grpc::Status& status) {
        LOG_DEBUG("ping - OnDone : {}", static_cast<int>(status.error_code()));

        std::unique_lock<std::mutex> lock(stream_mutex_);
        stream_status_ = status;
        grpc_status_ = STREAM_DONE;
        stream_cv_.notify_one();
    }

    void GrpcAgent::sendPingWorker() try {
        if (!start_ping_stream()) {
            return;
        }

        constexpr auto timeout = std::chrono::seconds(60);
        std::unique_lock<std::mutex> lock(ping_worker_mutex_);

        while (true) {
            lock.unlock();
            if (write_and_await_ping_stream() == STREAM_DONE) {
                if (!start_ping_stream()) {
                    break;
                }
            }

            lock.lock();
            if (ping_cv_.wait_for(lock, timeout, [this]{ return agent_->isExiting(); })) {
                lock.unlock();
                finish_ping_stream();
                break;
            }
        }
        LOG_INFO("grpc ping worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("grpc ping worker exception = {}", e.what());
    }

    void GrpcAgent::stopPingWorker() {
        std::unique_lock<std::mutex> lock(ping_worker_mutex_);
        ping_cv_.notify_one();
    }


    //GrpcSpan

    namespace {
        constexpr auto SHUTDOWN_AWAIT_TIMEOUT = std::chrono::seconds(3);

        // Heap-resident state for a single async SendSpanBatch call. Lives as
        // long as the callback's shared_ptr keeps it alive.
        struct PendingSpanBatch {
            grpc::ClientContext ctx;
            google::protobuf::Arena arena;
            v1::PSpanMessageBatch* request{nullptr};
            v1::PSpanResultBatch reply;
        };
    }

    GrpcSpan::GrpcSpan(std::shared_ptr<const Config> config) : GrpcClient(SPAN, std::move(config)) {
        set_span_stub(v1::Span::NewStub(channel_));
        batch_max_permits_ = config_->span.batch.max_concurrent_requests;
        batch_permits_ = batch_max_permits_;
    }

    bool GrpcSpan::try_acquire_permit(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(batch_permits_mutex_);
        if (!batch_permits_cv_.wait_for(lock, timeout, [this]{ return batch_permits_ > 0; })) {
            return false;
        }
        --batch_permits_;
        return true;
    }

    bool GrpcSpan::try_acquire_all_permits(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(batch_permits_mutex_);
        return batch_permits_cv_.wait_for(lock, timeout, [this]{
            return batch_permits_ >= batch_max_permits_;
        });
    }

    void GrpcSpan::release_permit() {
        std::unique_lock<std::mutex> lock(batch_permits_mutex_);
        ++batch_permits_;
        batch_permits_cv_.notify_one();
    }

    void GrpcSpan::enqueueSpan(std::unique_ptr<SpanChunk> span) noexcept try {
        if (agent_ != nullptr && agent_->isExiting()) {
            return;
        }

        std::unique_lock<std::mutex> lock(span_queue_mutex_);

        const auto& config = config_;
        if (span_queue_.size() < config->span.queue_size) {
            span_queue_.push(std::move(span));
            LOG_DEBUG("enqueueSpan: queue_size={}", span_queue_.size());
        } else {
            // Head-drop: discard the oldest queued span and enqueue the new one.
            // Matches Java SpanBatchGrpcDataSender.send().
            span_queue_.pop();
            span_queue_.push(std::move(span));
            LOG_DEBUG("discard oldest span: overflow max queue size {}", config->span.queue_size);
        }

        span_queue_cv_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue span: exception = {}", e.what());
    }

    void GrpcSpan::collect_batch(std::vector<std::unique_ptr<SpanChunk>>& buffer) {
        const auto& batch_cfg = config_->span.batch;
        const auto flush_timeout = std::chrono::milliseconds(batch_cfg.flush_interval_ms);
        const auto collect_deadline_ms = std::chrono::milliseconds(batch_cfg.collect_deadline_ms);
        const auto batch_size = static_cast<size_t>(batch_cfg.size);

        std::unique_lock<std::mutex> lock(span_queue_mutex_);

        // Block (with timeout) until the first item arrives or the worker is asked to stop.
        if (!span_queue_cv_.wait_for(lock, flush_timeout, [this]{
                return !span_queue_.empty() || agent_->isExiting();
            })) {
            return;
        }
        if (span_queue_.empty()) {
            return;
        }

        buffer.push_back(std::move(span_queue_.front()));
        span_queue_.pop();

        // Gather more items until either the batch is full or the collect
        // deadline elapses. Matches Java SpanBatchGrpcDataSender.collectBatch.
        const auto deadline = std::chrono::steady_clock::now() + collect_deadline_ms;
        while (buffer.size() < batch_size) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (!span_queue_cv_.wait_for(lock, remaining, [this]{
                    return !span_queue_.empty() || agent_->isExiting();
                })) {
                break;
            }
            if (span_queue_.empty()) {
                break;
            }
            buffer.push_back(std::move(span_queue_.front()));
            span_queue_.pop();
        }
        LOG_DEBUG("collect_batch: collected={} batch_size_limit={} remaining_queue={}",
                  buffer.size(), batch_size, span_queue_.size());
    }

    void GrpcSpan::send_batch_async(std::vector<std::unique_ptr<SpanChunk>>& batch) try {
        if (batch.empty()) {
            return;
        }

        auto pending = std::make_shared<PendingSpanBatch>();
        pending->request = google::protobuf::Arena::Create<v1::PSpanMessageBatch>(&pending->arena);

        for (auto& span_chunk : batch) {
            const auto span = span_chunk->getSpanData();
            auto* msg = pending->request->add_span();
            if (!span_chunk->isFinal() || span->isAsyncSpan()) {
                msg->unsafe_arena_set_allocated_spanchunk(build_grpc_span_chunk(std::move(span_chunk), &pending->arena));
            } else {
                msg->unsafe_arena_set_allocated_span(build_grpc_span(std::move(span_chunk), &pending->arena));
            }
        }
        batch.clear();

        const auto flush_timeout = std::chrono::milliseconds(config_->span.batch.flush_interval_ms);
        if (!try_acquire_permit(flush_timeout)) {
            LOG_INFO("SendSpanBatch skipped: no available permits within {}ms",
                     flush_timeout.count());
            return;
        }

        build_grpc_context(&pending->ctx, 0);
        set_request_deadline(pending->ctx);

        const int batch_count = pending->request->span_size();
        LOG_DEBUG("SendSpanBatch sending: batchSize={} concurrentRequests={}/{}",
                  batch_count, batch_max_permits_ - batch_permits_, batch_max_permits_);

        try {
            auto* ctx_ptr = &pending->ctx;
            auto* request_ptr = pending->request;
            auto* reply_ptr = &pending->reply;
            span_stub_->async()->SendSpanBatch(ctx_ptr, request_ptr, reply_ptr,
                [this, pending, batch_count](const grpc::Status& status) {
                    release_permit();
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
            release_permit();
            LOG_INFO("SendSpanBatch failed synchronously: exception = {}", e.what());
        }
    } catch (const std::exception& e) {
        LOG_ERROR("failed to build span batch: exception = {}", e.what());
    }

    void GrpcSpan::await_in_flight_requests() {
        if (!try_acquire_all_permits(SHUTDOWN_AWAIT_TIMEOUT)) {
            LOG_WARN("timed out waiting for in-flight span requests to complete");
        }
    }

    void GrpcSpan::flush_remaining() {
        std::vector<std::unique_ptr<SpanChunk>> remaining;
        {
            std::unique_lock<std::mutex> lock(span_queue_mutex_);
            while (!span_queue_.empty()) {
                remaining.push_back(std::move(span_queue_.front()));
                span_queue_.pop();
            }
        }
        if (!remaining.empty()) {
            LOG_INFO("flushing {} remaining spans on shutdown", remaining.size());
            if (readyChannel()) {
                send_batch_async(remaining);
            }
        }
        await_in_flight_requests();
    }

    void GrpcSpan::sendSpanWorker() try {
        while (!agent_->isExiting()) {
            std::vector<std::unique_ptr<SpanChunk>> batch;
            batch.reserve(config_->span.batch.size);

            collect_batch(batch);
            if (batch.empty()) {
                continue;
            }

            if (readyChannel()) {
                send_batch_async(batch);
            }
        }
        flush_remaining();
        LOG_INFO("grpc span worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("grpc span worker exception = {}", e.what());
    }

    void GrpcSpan::stopSpanWorker() {
        std::unique_lock<std::mutex> lock(span_queue_mutex_);
        span_queue_cv_.notify_all();
    }

    //GrpcStat

    GrpcStats::GrpcStats(std::shared_ptr<const Config> config) : GrpcClient(STATS, std::move(config)) {
        set_stats_stub(v1::Stat::NewStub(channel_));
    }

    bool GrpcStats::start_stats_stream() {
        LOG_DEBUG("start_stats_stream");
        if (!readyChannel()) {
            return false;
        }

        stream_context_ = std::make_unique<grpc::ClientContext>();
        build_grpc_context(stream_context_.get(), 0);
        stats_stub_->async()->SendAgentStat(stream_context_.get(), &reply_, this);

        AddHold();
        StartCall();
        return true;
    }

    GrpcStreamStatus GrpcStats::write_and_await_stats_stream() {
        LOG_DEBUG("write_and_await_stats_stream");

        std::unique_lock<std::mutex> lock(stream_mutex_);
        grpc_status_ = next_write();

        if (grpc_status_ == STREAM_WRITE) {
            StartWrite(msg_);
        } else {
            return grpc_status_;
        }

        stream_cv_.wait(lock, [this] {
            return grpc_status_ != STREAM_WRITE;
        });

        if (grpc_status_ == STREAM_DONE && !stream_status_.ok()) {
            LOG_ERROR("failed to send stats: {}, {}",
                     static_cast<int>(stream_status_.error_code()), stream_status_.error_message());
        }

        return grpc_status_;
    }

    void GrpcStats::finish_stats_stream() {
        LOG_DEBUG("finish_stats_stream");
        StartWritesDone();
        RemoveHold();

        std::unique_lock<std::mutex> lock(stream_mutex_);
        stream_cv_.wait(lock, [this] {
            return grpc_status_ == STREAM_DONE;
        });
    }

    void GrpcStats::OnWriteDone(bool ok) {
        LOG_DEBUG("stats - OnWriteDone: {}", ok);
        arena_.Reset(); // reset arena after write completes to free all memory at once

        if (ok) {
            std::unique_lock<std::mutex> lock(stream_mutex_);
            grpc_status_ = next_write();

            if (grpc_status_ == STREAM_WRITE) {
                StartWrite(msg_);
            } else {
                stream_cv_.notify_one();
            }
        } else {
            StartWritesDone();
            RemoveHold();
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
            if (agent_->isExiting() || stats_queue_.empty()) {
                LOG_DEBUG("stats - queue empty");
                return STREAM_CONTINUE;
            }

            stats = stats_queue_.front();
            stats_queue_.pop();
        }

        msg_ = google::protobuf::Arena::Create<v1::PStatMessage>(&arena_);
        if (stats == AGENT_STATS) {
            msg_->unsafe_arena_set_allocated_agentstatbatch(build_agent_stat_batch(agent_->getAgentStats().getSnapshots(), &arena_));
        } else {
            auto snapshot = agent_->getUrlStats().takeSnapshot();
            msg_->unsafe_arena_set_allocated_agenturistat(build_url_stat(snapshot.get(), &arena_));
        }

        return STREAM_WRITE;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to send stats: exception = {}", e.what());
        return STREAM_EXCEPTION;
    }

    constexpr size_t MAX_STATS_QUEUE_SIZE = 2;

    void GrpcStats::enqueueStats(const StatsType stats) noexcept try {
        const auto& config = config_;
        if (!config->stat.enable && !config->http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(stats_queue_mutex_);

        if (stats_queue_.size() < MAX_STATS_QUEUE_SIZE) {
            stats_queue_.push(stats);
        } else {
            force_queue_empty_ = true;
            LOG_DEBUG("drop stats: overflow max queue size {}", MAX_STATS_QUEUE_SIZE);
        }

        stats_queue_cv_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue stats: exception = {}", e.what());
    }

    void GrpcStats::empty_stats_queue() noexcept try {
        std::queue<StatsType> temp_queue;
        
        {
            std::unique_lock<std::mutex> lock(stats_queue_mutex_);
            
            stats_queue_.swap(temp_queue);
            force_queue_empty_ = false;
        }
        
        agent_->getAgentStats().initAgentStats();
        // clear URL stat snapshot
        (void)agent_->getUrlStats().takeSnapshot();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to empty span queue: exception = {}", e.what());
    }

    void GrpcStats::sendStatsWorker() try {
        const auto& config = config_;
        if (!config->stat.enable && !config->http.url_stat.enable) {
            return;
        }

        if (!start_stats_stream()) {
            return;
        }

        std::unique_lock<std::mutex> lock(stats_queue_mutex_);
        while (true) {
            stats_queue_cv_.wait(lock, [this]{
                return !stats_queue_.empty() || agent_->isExiting();
            });
            lock.unlock();

            if (agent_->isExiting()) {
                finish_stats_stream();
                break;
            }

            if (write_and_await_stats_stream() == STREAM_DONE) {
                if (!start_stats_stream()) {
                    break;
                }
                if (force_queue_empty_) {
                    empty_stats_queue();
                }
            }

            lock.lock();
        }

        LOG_INFO("grpc stats worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("grpc stats worker: exception = {}", e.what());
    }

    void GrpcStats::stopStatsWorker() {
        const auto& config = config_;
        if (!config->stat.enable && !config->http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(stats_queue_mutex_);
        stats_queue_cv_.notify_one();
    }
}
