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

#include "grpc_builders.h"

#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "annotation.h"
#include "callstack.h"
#include "span.h"
#include "span_event.h"
#include "stat.h"
#include "url_stat.h"
#include "v1/Service.grpc.pb.h"

namespace pinpoint {
    namespace {
        template<class... Ts>
        struct overloaded : Ts... { using Ts::operator()...; };
        template<class... Ts>
        overloaded(Ts...) -> overloaded<Ts...>;

        v1::PAcceptEvent* build_accept_event(SpanData* span, google::protobuf::Arena* arena) {
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
                parent_info->set_parentservicename(span->getParentServiceName());
                accept_event->unsafe_arena_set_allocated_parentinfo(parent_info);
            }

            return accept_event;
        }

        void build_annotation(v1::PAnnotation* annotation,
                              int32_t key,
                              const AnnotationData& val,
                              google::protobuf::Arena* arena) {
            annotation->set_key(key);
            auto* annotation_value = google::protobuf::Arena::Create<v1::PAnnotationValue>(arena);

            std::visit(overloaded{
                [&](const int32_t v) {
                    annotation_value->set_intvalue(v);
                },
                [&](const int64_t v) {
                    annotation_value->set_longvalue(v);
                },
                [&](const std::string& v) {
                    annotation_value->set_stringvalue(v);
                },
                [&](const StringStringValue& v) {
                    auto* ssv = google::protobuf::Arena::Create<v1::PStringStringValue>(arena);
                    auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s1->set_value(v.stringValue1);
                    ssv->unsafe_arena_set_allocated_stringvalue1(s1);

                    auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s2->set_value(v.stringValue2);
                    ssv->unsafe_arena_set_allocated_stringvalue2(s2);

                    annotation_value->unsafe_arena_set_allocated_stringstringvalue(ssv);
                },
                [&](const IntStringStringValue& v) {
                    auto* issv = google::protobuf::Arena::Create<v1::PIntStringStringValue>(arena);
                    issv->set_intvalue(v.intValue);

                    auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s1->set_value(v.stringValue1);
                    issv->unsafe_arena_set_allocated_stringvalue1(s1);

                    auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s2->set_value(v.stringValue2);
                    issv->unsafe_arena_set_allocated_stringvalue2(s2);

                    annotation_value->unsafe_arena_set_allocated_intstringstringvalue(issv);
                },
                [&](const LongIntIntByteByteStringValue& v) {
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
                },
                [&](const BytesStringStringValue& v) {
                    auto* bssv = google::protobuf::Arena::Create<v1::PBytesStringStringValue>(arena);

                    bssv->set_bytesvalue(reinterpret_cast<const char*>(v.bytesValue.data()), v.bytesValue.size());

                    auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s1->set_value(v.stringValue1);
                    bssv->unsafe_arena_set_allocated_stringvalue1(s1);

                    auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s2->set_value(v.stringValue2);
                    bssv->unsafe_arena_set_allocated_stringvalue2(s2);

                    annotation_value->unsafe_arena_set_allocated_bytesstringstringvalue(bssv);
                }
            }, val.data);
            annotation->unsafe_arena_set_allocated_value(annotation_value);
        }

        void build_string_annotation(v1::PAnnotation* annotation,
                                     int32_t key,
                                     std::string_view val,
                                     google::protobuf::Arena* arena) {
            annotation->set_key(key);
            const auto annotation_value = google::protobuf::Arena::Create<v1::PAnnotationValue>(arena);
            annotation_value->set_stringvalue(val.data(), val.size());
            annotation->unsafe_arena_set_allocated_value(annotation_value);
        }

        void build_span_event(v1::PSpanEvent* span_event,
                              const std::unique_ptr<SpanEventImpl>& se,
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
                build_string_annotation(span_event->add_annotation(), ANNOTATION_API, se->getOperationName(), arena);
            }

            auto& annotations = se->getAnnotations()->getAnnotations();
            for (const auto& [key, val] : annotations) {
                build_annotation(span_event->add_annotation(), key, val, arena);
            }

            if (const auto& err_str = se->getErrorString(); !err_str.empty()) {
                auto* except_info = google::protobuf::Arena::Create<v1::PIntStringValue>(arena);
                except_info->set_intvalue(se->getErrorFuncId());

                auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                s->set_value(err_str);
                except_info->unsafe_arena_set_allocated_stringvalue(s);
                span_event->unsafe_arena_set_allocated_exceptioninfo(except_info);
            }
        }

        void build_agent_stat(v1::PAgentStat* agent_stat,
                              const AgentStatsSnapshot& stat,
                              google::protobuf::Arena* arena) {
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

        void build_url_histogram(v1::PUriHistogram* grpc_histogram, const UrlStatHistogram& url_histogram) {
            grpc_histogram->set_total(url_histogram.total());
            grpc_histogram->set_max(url_histogram.max());
            for (auto i = 0; i < URL_STATS_BUCKET_SIZE; i++) {
                grpc_histogram->add_histogram(url_histogram.histogram(i));
            }
        }

        void build_each_url_stat(v1::PEachUriStat* url_stat,
                                 const UrlKey& key,
                                 EachUrlStat* each_stats,
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
    }  // namespace

    v1::PTransactionId* build_grpc_transaction_id(const TraceId& tid, google::protobuf::Arena* arena) {
        auto* ptid = google::protobuf::Arena::Create<v1::PTransactionId>(arena);

        ptid->set_agentid(tid.AgentId);
        ptid->set_agentstarttime(tid.StartTime);
        ptid->set_sequence(tid.Sequence);

        return ptid;
    }

    v1::PSpan* build_grpc_span(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena) {
        const auto span = chunk->getSpanData().get();
        auto* grpc_span = google::protobuf::Arena::Create<v1::PSpan>(arena);

        grpc_span->set_version(1);
        auto* tid = build_grpc_transaction_id(span->getTraceId(), arena);
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

        if (const auto& err_str = span->getErrorString(); !err_str.empty()) {
            auto* except_info = google::protobuf::Arena::Create<v1::PIntStringValue>(arena);
            except_info->set_intvalue(span->getErrorFuncId());

            auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s->set_value(err_str);
            except_info->unsafe_arena_set_allocated_stringvalue(s);
            grpc_span->unsafe_arena_set_allocated_exceptioninfo(except_info);
        }

        return grpc_span;
    }

    v1::PSpanChunk* build_grpc_span_chunk(std::unique_ptr<SpanChunk> chunk, google::protobuf::Arena* arena) {
        const auto span = chunk->getSpanData().get();
        auto* grpc_span = google::protobuf::Arena::Create<v1::PSpanChunk>(arena);
        grpc_span->set_version(1);

        auto* tid = build_grpc_transaction_id(span->getTraceId(), arena);
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

    v1::PAgentStatBatch* build_agent_stat_batch(const std::vector<AgentStatsSnapshot>& stats,
                                                google::protobuf::Arena* arena) {
        auto* grpc_stat = google::protobuf::Arena::Create<v1::PAgentStatBatch>(arena);

        for (const auto& stat : stats) {
            auto* agent_stat = grpc_stat->add_agentstat();
            build_agent_stat(agent_stat, stat, arena);
        }

        return grpc_stat;
    }

    v1::PAgentUriStat* build_url_stat(const UrlStatSnapshot* snapshot, google::protobuf::Arena* arena) {
        auto* uri_stat = google::protobuf::Arena::Create<v1::PAgentUriStat>(arena);

        uri_stat->set_bucketversion(URL_STATS_BUCKET_VERSION);
        const auto& m = snapshot->getEachStats();
        for (const auto& [key, each_stats] : m) {
            auto* url_stat = uri_stat->add_eachuristat();
            build_each_url_stat(url_stat, key, each_stats.get(), arena);
        }
        return uri_stat;
    }

    v1::PExceptionMetaData* build_exception_metadata(
            const TraceId& txid,
            int64_t span_id,
            std::string_view url_template,
            const std::vector<std::unique_ptr<Exception>>& exceptions,
            google::protobuf::Arena* arena) {
        auto* grpc_exception_meta = google::protobuf::Arena::Create<v1::PExceptionMetaData>(arena);

        grpc_exception_meta->unsafe_arena_set_allocated_transactionid(build_grpc_transaction_id(txid, arena));
        grpc_exception_meta->set_spanid(span_id);
        grpc_exception_meta->set_uritemplate(url_template.data(), url_template.size());

        for (const auto& exception : exceptions) {
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

        return grpc_exception_meta;
    }
}  // namespace pinpoint
