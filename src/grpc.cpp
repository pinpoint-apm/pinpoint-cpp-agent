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
#include "version.h"
#include "logging.h"
#include "stat.h"
#include "grpc.h"

namespace pinpoint {

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

    void build_agent_info(const Config& config, v1::PAgentInfo &agent_info) {
        agent_info.set_hostname(get_host_name());
        agent_info.set_ip(get_host_ip_addr());
        agent_info.set_servicetype(config.app_type_);
        agent_info.set_pid(getpid());
        agent_info.set_agentversion(VERSION_STRING);
        agent_info.set_container(config.is_container);

        const auto meta_data = new v1::PServerMetaData();
        meta_data->set_serverinfo("C/C++");
        meta_data->add_vmarg(to_config_string(config));

        agent_info.set_allocated_servermetadata(meta_data);
    }

    v1::PTransactionId* build_transaction_id(SpanData *span) {
        auto& [agent_id, start_time, sequence] = span->getTraceId();
        const auto tid = new v1::PTransactionId();

        tid->set_agentid(agent_id);
        tid->set_agentstarttime(start_time);
        tid->set_sequence(sequence);

        return tid;
    }

    v1::PAcceptEvent* build_accept_event(SpanData *span) {
        const auto accept_event = new v1::PAcceptEvent();

        accept_event->set_endpoint(span->getEndPoint());
        accept_event->set_rpc(span->getRpcName());

        if (auto& remote_addr = span->getRemoteAddr(); !remote_addr.empty()) {
            accept_event->set_remoteaddr(remote_addr);
        }

        if (!span->getParentAppName().empty()) {
            const auto parent_info = new v1::PParentInfo();

            parent_info->set_parentapplicationname(span->getParentAppName());
            parent_info->set_parentapplicationtype(span->getParentAppType());
            parent_info->set_acceptorhost(span->getAcceptorHost());
            accept_event->set_allocated_parentinfo(parent_info);
        }

        return accept_event;
    }

    void build_annotation(v1::PAnnotation *annotation, int32_t key, std::shared_ptr<AnnotationData> val) {
        annotation->set_key(key);
        const auto annotation_value = new v1::PAnnotationValue();

        if (val->dataType == 0) {
            annotation_value->set_intvalue(val->data.intValue);
        } else if (val->dataType == 1) {
            annotation_value->set_stringvalue(val->data.stringValue);
        } else if (val->dataType == 2) {
            const auto ssv = new v1::PStringStringValue();
            const auto s1 = new google::protobuf::StringValue();
            s1->set_value(val->data.stringStringValue.stringValue1);
            ssv->set_allocated_stringvalue1(s1);

            const auto s2 = new google::protobuf::StringValue();
            s2->set_value(val->data.stringStringValue.stringValue2);
            ssv->set_allocated_stringvalue2(s2);

            annotation_value->set_allocated_stringstringvalue(ssv);
        } else if (val->dataType == 3) {
            auto issv = new v1::PIntStringStringValue();
            issv->set_intvalue(val->data.intStringStringValue.intValue);

            const auto s1 = new google::protobuf::StringValue();
            s1->set_value(val->data.intStringStringValue.stringValue1);
            issv->set_allocated_stringvalue1(s1);

            const auto s2 = new google::protobuf::StringValue();
            s2->set_value(val->data.intStringStringValue.stringValue2);
            issv->set_allocated_stringvalue2(s2);

            annotation_value->set_allocated_intstringstringvalue(issv);
        } else if (val->dataType == 4) {
            auto liibbsv = new v1::PLongIntIntByteByteStringValue();
            liibbsv->set_longvalue(val->data.longIntIntByteByteStringValue.longValue);
            liibbsv->set_intvalue1(val->data.longIntIntByteByteStringValue.intValue1);
            liibbsv->set_intvalue2(val->data.longIntIntByteByteStringValue.intValue2);
            liibbsv->set_bytevalue1(val->data.longIntIntByteByteStringValue.byteValue1);
            liibbsv->set_bytevalue2(val->data.longIntIntByteByteStringValue.byteValue2);

            const auto s = new google::protobuf::StringValue();
            s->set_value(val->data.longIntIntByteByteStringValue.stringValue);
            liibbsv->set_allocated_stringvalue(s);

            annotation_value->set_allocated_longintintbytebytestringvalue(liibbsv);
        }
        annotation->set_allocated_value(annotation_value);
    }

    void build_string_annotation(v1::PAnnotation *annotation, int32_t key, std::string& val) {
        annotation->set_key(key);
        const auto annotation_value = new v1::PAnnotationValue();
        annotation_value->set_stringvalue(val);
        annotation->set_allocated_value(annotation_value);
    }

    void build_span_event(v1::PSpanEvent* span_event, std::shared_ptr<SpanEventImpl> se) {
        span_event->set_sequence(se->getSequence());
        span_event->set_depth(se->getDepth());
        span_event->set_startelapsed(se->getStartElapsed());
        span_event->set_endelapsed(se->getEndElapsed());
        span_event->set_servicetype(se->getServiceType());
        span_event->set_asyncevent(se->getAsyncId());

        if (!se->getDestinationId().empty()) {
            const auto next_event = new v1::PNextEvent();
            const auto message_event = new v1::PMessageEvent();

            message_event->set_nextspanid(se->getNextSpanId());
            message_event->set_destinationid(se->getDestinationId());
            next_event->set_allocated_messageevent(message_event);
            span_event->set_allocated_nextevent(next_event);
        }

        if (auto api_id = se->getApiId(); api_id > 0) {
            span_event->set_apiid(api_id);
        } else {
            build_string_annotation(span_event->add_annotation(), ANNOTATION_API, se->getOperationName());
        }

        auto& annotations = se->getAnnotations()->getAnnotations();
        for (auto &[key, val] : annotations) {
            build_annotation(span_event->add_annotation(), key, val);
        }

        if (auto err_str = se->getErrorString(); !err_str.empty()) {
            const auto exceptInfo = new v1::PIntStringValue();
            exceptInfo->set_intvalue(se->getErrorFuncId());

            const auto s = new google::protobuf::StringValue();
            s->set_value(err_str);
            exceptInfo->set_allocated_stringvalue(s);
            span_event->set_allocated_exceptioninfo(exceptInfo);
        }
    }

    v1::PSpan* build_grpc_span(std::unique_ptr<SpanChunk> chunk) {
        const auto span = chunk->getSpanData().get();
        const auto grpc_span = new v1::PSpan();

        grpc_span->set_version(1);
        const auto tid = build_transaction_id(span);
        grpc_span->set_allocated_transactionid(tid);

        grpc_span->set_spanid(span->getSpanId());
        grpc_span->set_parentspanid(span->getParentSpanId());
        grpc_span->set_starttime(span->getStartTime());
        grpc_span->set_elapsed(span->getElapsed());
        grpc_span->set_servicetype(span->getServiceType());
        grpc_span->set_applicationservicetype(span->getAppType());

        const auto accept_event = build_accept_event(span);
        grpc_span->set_allocated_acceptevent(accept_event);

        if (auto api_id = span->getApiId(); api_id > 0) {
            grpc_span->set_apiid(api_id);
        } else {
            build_string_annotation(grpc_span->add_annotation(), ANNOTATION_API, span->getOperationName());
        }
        grpc_span->set_flag(span->getFlags());
        grpc_span->set_err(span->getErr());

        const auto& events = chunk->getSpanEventChunk();
        for (auto& e : events) {
            build_span_event(grpc_span->add_spanevent(), e);
        }

        const auto& annotations = span->getAnnotations()->getAnnotations();
        for (const auto& [key, val] : annotations) {
            build_annotation(grpc_span->add_annotation(), key, val);
        }

        grpc_span->set_err(span->getErr());
        if (auto err_str = span->getErrorString(); !err_str.empty()) {
            const auto exceptInfo = new v1::PIntStringValue();
            exceptInfo->set_intvalue(span->getErrorFuncId());

            const auto s = new google::protobuf::StringValue();
            s->set_value(err_str);
            exceptInfo->set_allocated_stringvalue(s);
            grpc_span->set_allocated_exceptioninfo(exceptInfo);
        }

        return grpc_span;
    }

    v1::PSpanChunk* build_grpc_span_chunk(std::unique_ptr<SpanChunk> chunk) {
        const auto span = chunk->getSpanData().get();
        const auto grpc_span = new v1::PSpanChunk();
        grpc_span->set_version(1);

        const auto tid = build_transaction_id(span);
        grpc_span->set_allocated_transactionid(tid);

        grpc_span->set_spanid(span->getSpanId());
        grpc_span->set_keytime(chunk->getKeyTime());
        grpc_span->set_endpoint(span->getEndPoint());
        grpc_span->set_applicationservicetype(span->getAppType());

        if (span->isAsyncSpan()) {
            const auto aid = new v1::PLocalAsyncId();
            aid->set_asyncid(span->getAsyncId());
            aid->set_sequence(span->getAsyncSequence());
            grpc_span->set_allocated_localasyncid(aid);
        }

        auto& events = chunk->getSpanEventChunk();
        for (const auto& e : events) {
            build_span_event(grpc_span->add_spanevent(), e);
        }

        return grpc_span;
    }

    void build_agent_stat(v1::PAgentStat *agent_stat, const AgentStatsSnapshot& stat) {
        agent_stat->set_timestamp(stat.sample_time_);
        agent_stat->set_collectinterval(5000);

        const auto memory_stat = new v1::PJvmGc();
        memory_stat->set_type(v1::JVM_GC_TYPE_UNKNOWN);
        memory_stat->set_jvmmemoryheapused(stat.heap_alloc_size_);
        memory_stat->set_jvmmemoryheapmax(stat.heap_max_size_);
        memory_stat->set_jvmmemorynonheapused(0);
        memory_stat->set_jvmmemorynonheapmax(0);
        memory_stat->set_jvmgcoldcount(0);
        memory_stat->set_jvmgcoldtime(0);
        agent_stat->set_allocated_gc(memory_stat);

        const auto cpu_load = new v1::PCpuLoad();
        cpu_load->set_jvmcpuload(stat.process_cpu_time_);
        cpu_load->set_systemcpuload(stat.system_cpu_time_);
        agent_stat->set_allocated_cpuload(cpu_load);

        const auto tran = new v1::PTransaction();
        tran->set_samplednewcount(stat.num_sample_new_);
        tran->set_sampledcontinuationcount(stat.num_sample_cont_);
        tran->set_unsamplednewcount(stat.num_unsample_new_);
        tran->set_unsampledcontinuationcount(stat.num_unsample_cont_);
        tran->set_skippednewcount(stat.num_skip_new_);
        tran->set_skippedcontinuationcount(stat.num_skip_cont_);
        agent_stat->set_allocated_transaction(tran);

        const auto active_trace = new v1::PActiveTrace();
        const auto histogram = new v1::PActiveTraceHistogram();
        histogram->set_version(1);
        histogram->set_histogramschematype(2);
        for (int32_t c : stat.active_requests_) {
            histogram->add_activetracecount(c);
        }
        active_trace->set_allocated_histogram(histogram);
        agent_stat->set_allocated_activetrace(active_trace);

        const auto response_time = new v1::PResponseTime();
        response_time->set_avg(stat.response_time_avg_);
        response_time->set_max(stat.response_time_max_);
        agent_stat->set_allocated_responsetime(response_time);

        const auto total_thread = new v1::PTotalThread();
        total_thread->set_totalthreadcount(stat.num_threads_);
        agent_stat->set_allocated_totalthread(total_thread);
    }

    v1::PAgentStatBatch* build_agent_stat_batch(const std::vector<AgentStatsSnapshot>& stats) {
        const auto grpc_stat = new v1::PAgentStatBatch();

        for (size_t i = 0; i < stats.size(); i++) {
            auto agent_stat = grpc_stat->add_agentstat();
            build_agent_stat(agent_stat, stats.at(i));
        }

        return grpc_stat;
    }

    void build_url_histogram(v1::PUriHistogram *grpc_histogram, const UrlStatHistogram& url_histogram) {
        grpc_histogram->set_total(url_histogram.total());
        grpc_histogram->set_max(url_histogram.max());
        for (auto i = 0; i < URL_STATS_BUCKET_SIZE; i++) {
            grpc_histogram->add_histogram(url_histogram.histogram(i));
        }
    }

    void build_each_url_stat(v1::PEachUriStat *url_stat, const UrlKey& key, EachUrlStat *each_stats) {
        url_stat->set_uri(key.url_);

        const auto total = new v1::PUriHistogram();
        build_url_histogram(total, each_stats->getTotalHistogram());
        url_stat->set_allocated_totalhistogram(total);

        const auto fail = new v1::PUriHistogram();
        build_url_histogram(fail, each_stats->getFailHistogram());
        url_stat->set_allocated_failedhistogram(fail);

        url_stat->set_timestamp(each_stats->tick());
    }

    v1::PAgentUriStat* build_url_stat(UrlStatSnapshot* snapshot) {
        const auto uri_stat = new v1::PAgentUriStat();

        uri_stat->set_bucketversion(URL_STATS_BUCKET_VERSION);
        const auto& m = snapshot->getEachStats();
        for(const auto& [key, each_stats] : m) {
            const auto url_stat = uri_stat->add_eachuristat();
            build_each_url_stat(url_stat, key, each_stats);
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

        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30*1000);
        channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 60*1000);
        channel_args.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, 4 * 1024 * 1024);

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

    static int socket_id = 0;

    GrpcAgent::GrpcAgent(AgentService* agent) : GrpcClient(agent, AGENT) {
        agent_stub_ = v1::Agent::NewStub(channel_);
        meta_stub_ = v1::Metadata::NewStub(channel_);
    }

    constexpr auto REGISTER_TIMEOUT = std::chrono::seconds(60);
    constexpr auto META_TIMEOUT = std::chrono::seconds(5);

    static void set_deadline(grpc::ClientContext& ctx, const std::chrono::seconds timeout) {
        auto deadline = std::chrono::system_clock::now() + timeout;
        ctx.set_deadline(deadline);
    }

    GrpcRequestStatus GrpcAgent::registerAgent() {
        std::unique_lock<std::mutex> lock(channel_mutex_);

        grpc::ClientContext ctx;
        v1::PAgentInfo agent_info;
        v1::PResult reply;

        build_grpc_context(&ctx, agent_, 0);
        build_agent_info(agent_->getConfig(), agent_info);

        set_deadline(ctx, REGISTER_TIMEOUT);
        const grpc::Status status = agent_stub_->RequestAgentInfo(&ctx, agent_info, &reply);

        if (status.ok()) {
            LOG_INFO("success to register the agent");
            return SEND_OK;
        }

        LOG_ERROR("failed to register the agent: {}, {}", static_cast<int>(status.error_code()), status.error_message());
        return SEND_FAIL;
    }

    GrpcRequestStatus GrpcAgent::send_api_meta(ApiMeta& api_meta) {
        std::unique_lock<std::mutex> lock(channel_mutex_);

        v1::PApiMetaData grpc_api_meta;

        grpc_api_meta.set_apiid(api_meta.id_);
        grpc_api_meta.set_apiinfo(api_meta.api_str_);
        grpc_api_meta.set_type(api_meta.type_);

        v1::PResult reply;
        grpc::ClientContext ctx;

        build_grpc_context(&ctx, agent_, 0);
        set_deadline(ctx, META_TIMEOUT);
        const grpc::Status status = meta_stub_->RequestApiMetaData(&ctx, grpc_api_meta, &reply);

        if (status.ok()) {
            LOG_DEBUG("success to send api metadata: {}, {}", api_meta.api_str_, api_meta.id_);
            return SEND_OK;
        }

        LOG_ERROR("failed to send api metadata: {}, {}", static_cast<int>(status.error_code()), status.error_message());
        return SEND_FAIL;
    }

    GrpcRequestStatus GrpcAgent::send_string_meta(StringMeta& str_meta) {
        std::unique_lock<std::mutex> lock(channel_mutex_);

        v1::PStringMetaData grpc_str_meta;

        grpc_str_meta.set_stringid(str_meta.id_);
        grpc_str_meta.set_stringvalue(str_meta.str_val_);

        v1::PResult reply;
        grpc::ClientContext ctx;

        build_grpc_context(&ctx, agent_, 0);
        set_deadline(ctx, META_TIMEOUT);
        const grpc::Status status = meta_stub_->RequestStringMetaData(&ctx, grpc_str_meta, &reply);

        if (status.ok()) {
            LOG_DEBUG("success to send string metadata");
            return SEND_OK;
        }

        LOG_ERROR("failed to send string metadata: {}, {}", static_cast<int>(status.error_code()), status.error_message());
        return SEND_FAIL;
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

            if (meta->meta_type_ == META_API) {
                if (send_api_meta(meta->value_.api_meta_) != SEND_OK) {
                    agent_->removeCacheApi(meta->value_.api_meta_);
                }
            } else if (meta->meta_type_ == META_STRING) {
                if (send_string_meta(meta->value_.str_meta_) != SEND_OK) {
                    agent_->removeCacheError(meta->value_.str_meta_);
                }
            }

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
        build_grpc_context(stream_context_.get(), agent_, ++socket_id);
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

    GrpcSpan::GrpcSpan(AgentService* agent) : GrpcClient(agent, SPAN), span_stub_(nullptr) {
        span_stub_ = v1::Span::NewStub(channel_);
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
            StartWrite(msg_.get());
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
        msg_ = nullptr;

        if (ok) {
            std::unique_lock<std::mutex> lock(stream_mutex_);
            grpc_status_ = next_write();

            if (grpc_status_ == STREAM_WRITE) {
                StartWrite(msg_.get());
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

        span_queue_mutex_.lock();
        if (agent_->isExiting() || span_queue_.empty()) {
            span_queue_mutex_.unlock();
            LOG_DEBUG("span - queue empty");
            return STREAM_CONTINUE;
        }

        auto span_chunk = std::move(span_queue_.front());
        span_queue_.pop();
        span_queue_mutex_.unlock();

        const auto span = span_chunk->getSpanData();
        msg_ = std::make_unique<v1::PSpanMessage>();
        if (!span_chunk->isFinal() || span->isAsyncSpan()) {
            msg_->set_allocated_spanchunk(build_grpc_span_chunk(std::move(span_chunk)));
        } else {
            msg_->set_allocated_span(build_grpc_span(std::move(span_chunk)));
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
        std::unique_lock<std::mutex> lock(span_queue_mutex_);

        while (!span_queue_.empty()) {
            auto p = std::move(span_queue_.front());
            span_queue_.pop();
        }
        force_queue_empty_= false;
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
        stats_stub_ = v1::Stat::NewStub(channel_);
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
            StartWrite(msg_.get());
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
        msg_ = nullptr;

        if (ok) {
            std::unique_lock<std::mutex> lock(stream_mutex_);
            grpc_status_ = next_write();

            if (grpc_status_ == STREAM_WRITE) {
                StartWrite(msg_.get());
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

        stats_queue_mutex_.lock();
        if (agent_->isExiting() || stats_queue_.empty()) {
            stats_queue_mutex_.unlock();
            LOG_DEBUG("stats - queue empty");
            return STREAM_CONTINUE;
        }

        auto stats = stats_queue_.front();
        stats_queue_.pop();
        stats_queue_mutex_.unlock();

        msg_ = std::make_unique<v1::PStatMessage>();
        if (stats == AGENT_STATS) {
            msg_->set_allocated_agentstatbatch(build_agent_stat_batch(get_agent_stat_snapshots()));
        } else {
            const auto snapshot = take_url_stat_snapshot();
            msg_->set_allocated_agenturistat(build_url_stat(snapshot));
            delete snapshot;
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
        std::unique_lock<std::mutex> lock(stats_queue_mutex_);
        while (!stats_queue_.empty()) {
            stats_queue_.pop();
        }

        init_agent_stats();
        delete take_url_stat_snapshot();
        force_queue_empty_ = false;
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
