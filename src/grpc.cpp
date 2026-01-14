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

#include <memory>
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

    namespace {
        // gRPC channel configuration constants
        constexpr int KEEPALIVE_TIME_MS = 30 * 1000;        // 30 seconds
        constexpr int KEEPALIVE_TIMEOUT_MS = 60 * 1000;     // 60 seconds
        constexpr int MAX_MESSAGE_LENGTH = 4 * 1024 * 1024; // 4 MB
    }

    static void build_grpc_context(grpc::ClientContext* context, const AgentService* agent, int socket_id) {
        auto& config = agent->getConfig();

        context->AddMetadata("applicationname", config.app_name_);
        context->AddMetadata("agentid", config.agent_id_);
        context->AddMetadata("starttime", std::to_string(agent->getStartTime()));

        if (!config.agent_name_.empty()) {
            context->AddMetadata("agentname", config.agent_name_);
        }
        if (socket_id > 0) {
            context->AddMetadata("socketid", std::to_string(socket_id));
        }
    }

    static void build_agent_info(const Config& config, v1::PAgentInfo* agent_info, google::protobuf::Arena* arena) {
        agent_info->set_hostname(get_host_name());
        agent_info->set_ip(get_host_ip_addr());
        agent_info->set_servicetype(config.app_type_);
        agent_info->set_pid(getpid());
        agent_info->set_agentversion(VERSION_STRING);
        agent_info->set_container(config.is_container);

        const auto meta_data = google::protobuf::Arena::Create<v1::PServerMetaData>(arena);
        meta_data->set_serverinfo("C/C++");
        meta_data->add_vmarg(to_config_string(config));

        agent_info->set_allocated_servermetadata(meta_data);
    }

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
            accept_event->set_allocated_parentinfo(parent_info);
        }

        return accept_event;
    }

    static void build_annotation(v1::PAnnotation *annotation, int32_t key, 
                               const std::shared_ptr<AnnotationData>& val,
                               google::protobuf::Arena* arena) {
        annotation->set_key(key);
        auto* annotation_value = google::protobuf::Arena::Create<v1::PAnnotationValue>(arena);

        if (val->dataType == ANNOTATION_TYPE_INT) {
            annotation_value->set_intvalue(val->data.intValue);
        } else if (val->dataType == ANNOTATION_TYPE_LONG) {
            annotation_value->set_longvalue(val->data.longValue);
        } else if (val->dataType == ANNOTATION_TYPE_STRING) {
            annotation_value->set_stringvalue(val->data.stringValue);
        } else if (val->dataType == ANNOTATION_TYPE_STRING_STRING) {
            auto* ssv = google::protobuf::Arena::Create<v1::PStringStringValue>(arena);
            auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s1->set_value(val->data.stringStringValue.stringValue1);
            ssv->set_allocated_stringvalue1(s1);

            auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s2->set_value(val->data.stringStringValue.stringValue2);
            ssv->set_allocated_stringvalue2(s2);

            annotation_value->set_allocated_stringstringvalue(ssv);
        } else if (val->dataType == ANNOTATION_TYPE_INT_STRING_STRING) {
            auto* issv = google::protobuf::Arena::Create<v1::PIntStringStringValue>(arena);
            issv->set_intvalue(val->data.intStringStringValue.intValue);

            auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s1->set_value(val->data.intStringStringValue.stringValue1);
            issv->set_allocated_stringvalue1(s1);

            auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s2->set_value(val->data.intStringStringValue.stringValue2);
            issv->set_allocated_stringvalue2(s2);

            annotation_value->set_allocated_intstringstringvalue(issv);
        } else if (val->dataType == ANNOTATION_TYPE_LONG_INT_INT_BYTE_BYTE_STRING) {
            auto* liibbsv = google::protobuf::Arena::Create<v1::PLongIntIntByteByteStringValue>(arena);
            liibbsv->set_longvalue(val->data.longIntIntByteByteStringValue.longValue);
            liibbsv->set_intvalue1(val->data.longIntIntByteByteStringValue.intValue1);
            liibbsv->set_intvalue2(val->data.longIntIntByteByteStringValue.intValue2);
            liibbsv->set_bytevalue1(val->data.longIntIntByteByteStringValue.byteValue1);
            liibbsv->set_bytevalue2(val->data.longIntIntByteByteStringValue.byteValue2);

            auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s->set_value(val->data.longIntIntByteByteStringValue.stringValue);
            liibbsv->set_allocated_stringvalue(s);

            annotation_value->set_allocated_longintintbytebytestringvalue(liibbsv);
        } else if (val->dataType == ANNOTATION_TYPE_BYTES_STRING_STRING) {
            auto* bssv = google::protobuf::Arena::Create<v1::PBytesStringStringValue>(arena);
            auto& bv = val->data.bytesStringStringValue.bytesValue;
            
            // zero-copy conversion from vector<unsigned char> to string
            std::string_view bytes_view(
                reinterpret_cast<const char*>(bv.data()),
                bv.size()
            );
            bssv->set_bytesvalue(std::string(bytes_view));

            auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s1->set_value(val->data.bytesStringStringValue.stringValue1);
            bssv->set_allocated_stringvalue1(s1);

            auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s2->set_value(val->data.bytesStringStringValue.stringValue2);
            bssv->set_allocated_stringvalue2(s2);

            annotation_value->set_allocated_bytesstringstringvalue(bssv);
        }
        annotation->set_allocated_value(annotation_value);
    }

    static void build_string_annotation(v1::PAnnotation *annotation, int32_t key, std::string& val, google::protobuf::Arena* arena) {
        annotation->set_key(key);
        const auto annotation_value = google::protobuf::Arena::Create<v1::PAnnotationValue>(arena);
        annotation_value->set_stringvalue(val);
        annotation->set_allocated_value(annotation_value);
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
            next_event->set_allocated_messageevent(message_event);
            span_event->set_allocated_nextevent(next_event);
        }

        if (auto api_id = se->getApiId(); api_id > 0) {
            span_event->set_apiid(api_id);
        } else {
            auto operation_name = se->getOperationName();
            build_string_annotation(span_event->add_annotation(), ANNOTATION_API, operation_name, arena);
        }

        auto& annotations = se->getAnnotations()->getAnnotations();
        for (const auto& [key, val] : annotations) {  // const auto& for shared_ptr
            build_annotation(span_event->add_annotation(), key, val, arena);
        }

        if (auto err_str = se->getErrorString(); !err_str.empty()) {
            auto* exceptInfo = google::protobuf::Arena::Create<v1::PIntStringValue>(arena);
            exceptInfo->set_intvalue(se->getErrorFuncId());

            auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s->set_value(err_str);
            exceptInfo->set_allocated_stringvalue(s);
            span_event->set_allocated_exceptioninfo(exceptInfo);
        }
    }

    static v1::PSpan* build_grpc_span(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena) {
        const auto span = chunk->getSpanData().get();
        auto* grpc_span = google::protobuf::Arena::Create<v1::PSpan>(arena);

        grpc_span->set_version(1);
        auto* tid = build_transaction_id(span->getTraceId(), arena);
        grpc_span->set_allocated_transactionid(tid);

        grpc_span->set_spanid(span->getSpanId());
        grpc_span->set_parentspanid(span->getParentSpanId());
        grpc_span->set_starttime(span->getStartTime());
        grpc_span->set_elapsed(span->getElapsed());
        grpc_span->set_servicetype(span->getServiceType());
        grpc_span->set_applicationservicetype(span->getAppType());

        auto* accept_event = build_accept_event(span, arena);
        grpc_span->set_allocated_acceptevent(accept_event);

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
            exceptInfo->set_allocated_stringvalue(s);
            grpc_span->set_allocated_exceptioninfo(exceptInfo);
        }

        return grpc_span;
    }

    static v1::PSpanChunk* build_grpc_span_chunk(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena) {
        const auto span = chunk->getSpanData().get();
        auto* grpc_span = google::protobuf::Arena::Create<v1::PSpanChunk>(arena);
        grpc_span->set_version(1);

        auto* tid = build_transaction_id(span->getTraceId(), arena);
        grpc_span->set_allocated_transactionid(tid);

        grpc_span->set_spanid(span->getSpanId());
        grpc_span->set_keytime(chunk->getKeyTime());
        grpc_span->set_endpoint(span->getEndPoint());
        grpc_span->set_applicationservicetype(span->getAppType());

        if (span->isAsyncSpan()) {
            auto* aid = google::protobuf::Arena::Create<v1::PLocalAsyncId>(arena);
            aid->set_asyncid(span->getAsyncId());
            aid->set_sequence(span->getAsyncSequence());
            grpc_span->set_allocated_localasyncid(aid);
        }

        auto& events = chunk->getSpanEventChunk();
        for (const auto& e : events) {
            build_span_event(grpc_span->add_spanevent(), e, arena);
        }

        return grpc_span;
    }

    static void build_agent_stat(v1::PAgentStat *agent_stat, const AgentStatsSnapshot& stat, google::protobuf::Arena* arena) {
        agent_stat->set_timestamp(stat.sample_time_);
        agent_stat->set_collectinterval(5000);

        auto* memory_stat = google::protobuf::Arena::Create<v1::PJvmGc>(arena);
        memory_stat->set_type(v1::JVM_GC_TYPE_UNKNOWN);
        memory_stat->set_jvmmemoryheapused(stat.heap_alloc_size_);
        memory_stat->set_jvmmemoryheapmax(stat.heap_max_size_);
        memory_stat->set_jvmmemorynonheapused(0);
        memory_stat->set_jvmmemorynonheapmax(0);
        memory_stat->set_jvmgcoldcount(0);
        memory_stat->set_jvmgcoldtime(0);
        agent_stat->set_allocated_gc(memory_stat);

        auto* cpu_load = google::protobuf::Arena::Create<v1::PCpuLoad>(arena);
        cpu_load->set_jvmcpuload(stat.process_cpu_time_);
        cpu_load->set_systemcpuload(stat.system_cpu_time_);
        agent_stat->set_allocated_cpuload(cpu_load);

        auto* tran = google::protobuf::Arena::Create<v1::PTransaction>(arena);
        tran->set_samplednewcount(stat.num_sample_new_);
        tran->set_sampledcontinuationcount(stat.num_sample_cont_);
        tran->set_unsamplednewcount(stat.num_unsample_new_);
        tran->set_unsampledcontinuationcount(stat.num_unsample_cont_);
        tran->set_skippednewcount(stat.num_skip_new_);
        tran->set_skippedcontinuationcount(stat.num_skip_cont_);
        agent_stat->set_allocated_transaction(tran);

        auto* active_trace = google::protobuf::Arena::Create<v1::PActiveTrace>(arena);
        auto* histogram = google::protobuf::Arena::Create<v1::PActiveTraceHistogram>(arena);
        histogram->set_version(1);
        histogram->set_histogramschematype(2);
        for (int32_t c : stat.active_requests_) {
            histogram->add_activetracecount(c);
        }
        active_trace->set_allocated_histogram(histogram);
        agent_stat->set_allocated_activetrace(active_trace);

        auto* response_time = google::protobuf::Arena::Create<v1::PResponseTime>(arena);
        response_time->set_avg(stat.response_time_avg_);
        response_time->set_max(stat.response_time_max_);
        agent_stat->set_allocated_responsetime(response_time);

        auto* total_thread = google::protobuf::Arena::Create<v1::PTotalThread>(arena);
        total_thread->set_totalthreadcount(stat.num_threads_);
        agent_stat->set_allocated_totalthread(total_thread);
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
        url_stat->set_allocated_totalhistogram(total);

        auto* fail = google::protobuf::Arena::Create<v1::PUriHistogram>(arena);
        build_url_histogram(fail, each_stats->getFailHistogram());
        url_stat->set_allocated_failedhistogram(fail);

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

    GrpcClient::GrpcClient(AgentService* agent, ClientType client_type) : agent_{agent} {
        auto& config = agent_->getConfig();
        auto addr = absl::StrCat(config.collector.host, ":");

        if (client_type == AGENT) {
            addr = absl::StrCat(addr, config.collector.agent_port);
            client_name_ = "agent";
        } else if (client_type == SPAN) {
            addr = absl::StrCat(addr, config.collector.span_port);
            client_name_ = "span";
        } else {
            addr = absl::StrCat(addr, config.collector.stat_port);
            client_name_ = "stats";
        }

        grpc::ChannelArguments channel_args;

        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, KEEPALIVE_TIME_MS);
        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, KEEPALIVE_TIMEOUT_MS);
        channel_args.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, MAX_MESSAGE_LENGTH);

        channel_ = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), channel_args);
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

    //GrpcAgent

    GrpcAgent::GrpcAgent(AgentService* agent) : GrpcClient(agent, AGENT) {
        set_agent_stub(v1::Agent::NewStub(channel_));
        set_meta_stub(v1::Metadata::NewStub(channel_));
    }

    constexpr auto REGISTER_TIMEOUT = std::chrono::seconds(60);
    constexpr auto META_TIMEOUT = std::chrono::seconds(5);

    static void set_deadline(grpc::ClientContext& ctx, const std::chrono::seconds timeout) {
        auto deadline = std::chrono::system_clock::now() + timeout;
        ctx.set_deadline(deadline);
    }

    GrpcRequestStatus GrpcAgent::registerAgent() {
        grpc::ClientContext ctx;
        v1::PResult reply;

        std::unique_lock<std::mutex> lock(channel_mutex_);
        build_grpc_context(&ctx, agent_, 0);

        google::protobuf::Arena arena;
        auto* agent_info = google::protobuf::Arena::Create<v1::PAgentInfo>(&arena);
        build_agent_info(agent_->getConfig(), agent_info, &arena);

        set_deadline(ctx, REGISTER_TIMEOUT);
        const grpc::Status status = agent_stub_->RequestAgentInfo(&ctx, *agent_info, &reply);

        if (status.ok()) {
            LOG_INFO("success to register the agent");  
            return SEND_OK;
        }

        LOG_ERROR("failed to register the agent: {}, {}", static_cast<int>(status.error_code()), status.error_message());
        return SEND_FAIL;
    }

    template<typename Request, typename StubMethod>
    GrpcRequestStatus GrpcAgent::send_meta_helper(StubMethod stub_method, Request& request, std::string_view operation_name) {
        v1::PResult reply;
        grpc::ClientContext ctx;
        std::unique_lock<std::mutex> lock(channel_mutex_);

        build_grpc_context(&ctx, agent_, 0);
        set_deadline(ctx, META_TIMEOUT);
        
        const grpc::Status status = stub_method(&ctx, request, &reply);

        if (status.ok()) {
            LOG_DEBUG("success to send {} metadata", operation_name);
            return SEND_OK;
        }

        LOG_ERROR("failed to send {} metadata: {}, {}", 
                  operation_name, static_cast<int>(status.error_code()), status.error_message());
        return SEND_FAIL;
    }

    GrpcRequestStatus GrpcAgent::send_api_meta(ApiMeta& api_meta) {
        v1::PApiMetaData grpc_api_meta;

        grpc_api_meta.set_apiid(api_meta.id_);
        grpc_api_meta.set_apiinfo(api_meta.api_str_);
        grpc_api_meta.set_type(api_meta.type_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PApiMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestApiMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_api_meta, "api");
    }

    GrpcRequestStatus GrpcAgent::send_error_meta(StringMeta& error_meta) {
        v1::PStringMetaData grpc_error_meta;

        grpc_error_meta.set_stringid(error_meta.id_);
        grpc_error_meta.set_stringvalue(error_meta.str_val_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PStringMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestStringMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_error_meta, "error");
    }

    GrpcRequestStatus GrpcAgent::send_sql_meta(StringMeta& sql_meta) {
        v1::PSqlMetaData grpc_sql_meta;

        grpc_sql_meta.set_sqlid(sql_meta.id_);
        grpc_sql_meta.set_sql(sql_meta.str_val_);

        auto stub_method = [this](grpc::ClientContext* ctx, const v1::PSqlMetaData& req, v1::PResult* reply) {
            return meta_stub_->RequestSqlMetaData(ctx, req, reply);
        };

        return send_meta_helper(stub_method, grpc_sql_meta, "sql");
    }

    GrpcRequestStatus GrpcAgent::send_sql_uid_meta(SqlUidMeta& sql_uid_meta) {
        v1::PSqlUidMetaData grpc_sql_uid_meta;

        // zero-copy conversion from vector<unsigned char> to string
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

    GrpcRequestStatus GrpcAgent::send_exception_meta(ExceptionMeta& exception_meta) {
        google::protobuf::Arena arena;
        auto* grpc_exception_meta = google::protobuf::Arena::Create<v1::PExceptionMetaData>(&arena);

        grpc_exception_meta->set_allocated_transactionid(build_transaction_id(exception_meta.txid_, &arena));
        grpc_exception_meta->set_spanid(exception_meta.span_id_);
        grpc_exception_meta->set_uritemplate(exception_meta.url_template_);

        for (const auto& exception : exception_meta.exceptions_) {
            auto* grpc_exception = grpc_exception_meta->add_exceptions();
            auto callstack = exception->getCallStack();

            grpc_exception->set_exceptionid(exception->getId());
            grpc_exception->set_exceptionclassname(callstack->getModuleName());
            grpc_exception->set_exceptionmessage(callstack->getErrorMessage());
            grpc_exception->set_starttime(callstack->getErrorTime());
            grpc_exception->set_exceptiondepth(1);

            for (const auto& frame : callstack->getStack()) {
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

    void GrpcAgent::enqueueMeta(std::unique_ptr<MetaData> meta) noexcept try {
        std::unique_lock<std::mutex> lock(meta_queue_mutex_);

        if (auto& config = agent_->getConfig(); meta_queue_.size() < config.span.queue_size) {
            meta_queue_.push(std::move(meta));
        } else {
            LOG_DEBUG("drop metadata: overflow max queue size {}", config.span.queue_size);
        }

        meta_queue_cv_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue metadata: exception = {}", e.what());
    }

    void GrpcAgent::sendMetaWorker() try {
        std::unique_lock<std::mutex> lock(meta_queue_mutex_);

        while (true) {
            meta_queue_cv_.wait(lock, [this]{
                return !meta_queue_.empty() || agent_->isExiting();
            });
            if (agent_->isExiting()) {
                break;
            }

            const auto meta = std::move(meta_queue_.front());
            meta_queue_.pop();
            lock.unlock();

            // Use std::visit for type-safe variant handling
            std::visit([this](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ApiMeta>) {
                    if (send_api_meta(value) != SEND_OK) {
                        agent_->removeCacheApi(value);
                    }
                } else if constexpr (std::is_same_v<T, StringMeta>) {
                    if (value.type_ == STRING_META_ERROR) {
                        if (send_error_meta(value) != SEND_OK) {
                            agent_->removeCacheError(value);
                        }
                    } else if (value.type_ == STRING_META_SQL) {
                        if (send_sql_meta(value) != SEND_OK) {
                            agent_->removeCacheSql(value);
                        }
                    }
                } else if constexpr (std::is_same_v<T, SqlUidMeta>) {
                    if (send_sql_uid_meta(value) != SEND_OK) {
                        agent_->removeCacheSqlUid(value);
                    }
                } else if constexpr (std::is_same_v<T, ExceptionMeta>) {
                    send_exception_meta(value);
                }
            }, meta->value_);

            lock.lock();
        }
        LOG_INFO("send meta worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("failed to send grpc meta: exception = {}", e.what());
    }

    void GrpcAgent::stopMetaWorker() {
        std::unique_lock<std::mutex> lock(meta_queue_mutex_);
        meta_queue_cv_.notify_one();
    }

    // Ping Stream

    bool GrpcAgent::start_ping_stream() {
        LOG_DEBUG("start_ping_stream");
        if (!readyChannel()) {
            return false;
        }

        stream_context_ = std::make_unique<grpc::ClientContext>();
        build_grpc_context(stream_context_.get(), agent_, ++socket_id_);
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

    GrpcSpan::GrpcSpan(AgentService* agent) : GrpcClient(agent, SPAN) {
        set_span_stub(v1::Span::NewStub(channel_));
    }

    bool GrpcSpan::start_span_stream() {
        LOG_DEBUG("start_span_stream");
        if (!readyChannel()) {
            return false;
        }

        stream_context_ = std::make_unique<grpc::ClientContext>();
        build_grpc_context(stream_context_.get(), agent_, 0);
        span_stub_->async()->SendSpan(stream_context_.get(), &reply_, this);

        AddHold();
        StartCall();
        return true;
    }

    GrpcStreamStatus GrpcSpan::write_and_await_span_stream() {
        LOG_DEBUG("write_and_await_span_stream");

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
            LOG_ERROR("failed to send span: {}, {}",
                     static_cast<int>(stream_status_.error_code()), stream_status_.error_message());
        }

        return grpc_status_;
    }

    void GrpcSpan::finish_span_stream() {
        LOG_DEBUG("finish_span_stream");
        StartWritesDone();
        RemoveHold();

        std::unique_lock<std::mutex> lock(stream_mutex_);
        stream_cv_.wait(lock, [this] {
            return grpc_status_ == STREAM_DONE;
        });
    }

    void GrpcSpan::OnWriteDone(bool ok) {
        LOG_DEBUG("span - OnWriteDone: {}", ok);
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

    void GrpcSpan::OnDone(const grpc::Status& status) {
        LOG_DEBUG("span - OnDone: {}", static_cast<int>(status.error_code()));

        std::unique_lock<std::mutex> lock(stream_mutex_);
        stream_status_ = status;
        grpc_status_ = STREAM_DONE;
        stream_cv_.notify_one();
    }

    GrpcStreamStatus GrpcSpan::next_write() try {
        LOG_DEBUG("span - next_write");
        // should be hold stream_mutex_

        std::unique_ptr<SpanChunk> span_chunk;
        {
            std::unique_lock<std::mutex> lock(span_queue_mutex_);
            if (agent_->isExiting() || span_queue_.empty()) {
                LOG_DEBUG("span - queue empty");
                return STREAM_CONTINUE;
            }

            span_chunk = std::move(span_queue_.front());
            span_queue_.pop();
        }

        const auto span = span_chunk->getSpanData();
        msg_ = google::protobuf::Arena::Create<v1::PSpanMessage>(&arena_);
        if (!span_chunk->isFinal() || span->isAsyncSpan()) {
            msg_->set_allocated_spanchunk(build_grpc_span_chunk(std::move(span_chunk), &arena_));
        } else {
            msg_->set_allocated_span(build_grpc_span(std::move(span_chunk), &arena_));
        }

        //msg_->PrintDebugString();
        return STREAM_WRITE;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to send span: exception = {}", e.what());
        return STREAM_EXCEPTION;
    }

    void GrpcSpan::enqueueSpan(std::unique_ptr<SpanChunk> span) noexcept try {
        std::unique_lock<std::mutex> lock(span_queue_mutex_);

        if (auto& config = agent_->getConfig(); span_queue_.size() < config.span.queue_size) {
            span_queue_.push(std::move(span));
        } else {
            LOG_DEBUG("drop span: overflow max queue size {}", config.span.queue_size);
            force_queue_empty_ = true;
        }

        span_queue_cv_.notify_one();
    } catch (const std::exception &e) {
        LOG_ERROR("failed to enqueue span: exception = {}", e.what());
    }

    void GrpcSpan::empty_span_queue() noexcept try {
        std::queue<std::unique_ptr<SpanChunk>> temp_queue;
        
        {
            std::unique_lock<std::mutex> lock(span_queue_mutex_);
            
            // use swap to minimize lock hold time
            // swap operation is O(1) and only exchanges internal pointers
            span_queue_.swap(temp_queue);
            force_queue_empty_ = false;
            
        }
    } catch (const std::exception &e) {
        LOG_ERROR("failed to empty span queue: exception = {}", e.what());
    }

    void GrpcSpan::sendSpanWorker() try {
        if (!start_span_stream()) {
            return;
        }

        std::unique_lock<std::mutex> lock(span_queue_mutex_);
        while (true) {
            span_queue_cv_.wait(lock, [this]{
                return !span_queue_.empty() || agent_->isExiting();
            });
            lock.unlock();

            if (agent_->isExiting()) {
                finish_span_stream();
                break;
            }

            if (write_and_await_span_stream() == STREAM_DONE) {
                if (!start_span_stream()) {
                    break;
                }
                if (force_queue_empty_) {
                    empty_span_queue();
                }
            }

            lock.lock();
        }
        LOG_INFO("grpc span worker end");
    } catch (const std::exception& e) {
        LOG_ERROR("grpc span worker exception = {}", e.what());
    }

    void GrpcSpan::stopSpanWorker() {
        std::unique_lock<std::mutex> lock(span_queue_mutex_);
        span_queue_cv_.notify_one();
    }

    //GrpcStat

    GrpcStats::GrpcStats(AgentService* agent): GrpcClient(agent, STATS) {
        set_stats_stub(v1::Stat::NewStub(channel_));
    }

    bool GrpcStats::start_stats_stream() {
        LOG_DEBUG("start_stats_stream");
        if (!readyChannel()) {
            return false;
        }

        stream_context_ = std::make_unique<grpc::ClientContext>();
        build_grpc_context(stream_context_.get(), agent_, 0);
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
            msg_->set_allocated_agentstatbatch(build_agent_stat_batch(agent_->getAgentStats().getSnapshots(), &arena_));
        } else {
            auto snapshot = agent_->getUrlStats().takeSnapshot();
            msg_->set_allocated_agenturistat(build_url_stat(snapshot.get(), &arena_));
        }

        return STREAM_WRITE;
    } catch (const std::exception &e) {
        LOG_ERROR("failed to send stats: exception = {}", e.what());
        return STREAM_EXCEPTION;
    }

    constexpr size_t MAX_STATS_QUEUE_SIZE = 2;

    void GrpcStats::enqueueStats(const StatsType stats) noexcept try {
        if (auto& config = agent_->getConfig(); !config.stat.enable && !config.http.url_stat.enable) {
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
        if (auto& config = agent_->getConfig(); !config.stat.enable && !config.http.url_stat.enable) {
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
        if (auto& config = agent_->getConfig(); !config.stat.enable && !config.http.url_stat.enable) {
            return;
        }

        std::unique_lock<std::mutex> lock(stats_queue_mutex_);
        stats_queue_cv_.notify_one();
    }
}
