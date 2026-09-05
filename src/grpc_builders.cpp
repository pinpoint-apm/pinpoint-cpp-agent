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

#include <cassert>
#include <memory>
#include <string_view>
#include <unordered_map>
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
        // Shared by build_span_event and build_grpc_span: fills the
        // exceptioninfo field from the recorded error func id and message.
        template <typename Proto>
        void set_exception_info(Proto* dst, int32_t func_id, const std::string& err_str,
                                google::protobuf::Arena* arena) {
            auto* except_info = google::protobuf::Arena::Create<v1::PIntStringValue>(arena);
            except_info->set_intvalue(func_id);

            auto* s = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
            s->set_value(err_str);
            except_info->unsafe_arena_set_allocated_stringvalue(s);
            dst->unsafe_arena_set_allocated_exceptioninfo(except_info);
        }
        template<class... Ts>
        struct overloaded : Ts... { using Ts::operator()...; };
        template<class... Ts>
        overloaded(Ts...) -> overloaded<Ts...>;

        // Java SpanMessageMapper.DEFAULT_END_POINT / DEFAULT_REMOTE_ADDRESS:
        // an accept event whose endpoint or remote address was never set goes
        // out as "UNKNOWN", not as an empty string. The distinction is
        // collector-visible — the server UI renders an empty caller address as
        // a blank cell, while "UNKNOWN" reads as "the agent could not tell".
        // Only PAcceptEvent gets the default in Java; PSpanChunk.endPoint and
        // PMessageEvent.endPoint keep their empty value, so neither is touched.
        constexpr std::string_view kUnknownAddress = "UNKNOWN";

        std::string_view or_unknown(const std::string& value) {
            return value.empty() ? kUnknownAddress : std::string_view(value);
        }

        v1::PAcceptEvent* build_accept_event(SpanData* span, google::protobuf::Arena* arena) {
            auto* accept_event = google::protobuf::Arena::Create<v1::PAcceptEvent>(arena);

            const auto end_point = or_unknown(span->getEndPoint());
            accept_event->set_endpoint(end_point.data(), end_point.size());
            accept_event->set_rpc(span->getRpcName());

            const auto remote_addr = or_unknown(span->getRemoteAddr());
            accept_event->set_remoteaddr(remote_addr.data(), remote_addr.size());

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
                [&](const std::pair<std::string, std::string>& v) {
                    auto* ssv = google::protobuf::Arena::Create<v1::PStringStringValue>(arena);
                    auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s1->set_value(v.first);
                    ssv->unsafe_arena_set_allocated_stringvalue1(s1);

                    auto* s2 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    s2->set_value(v.second);
                    ssv->unsafe_arena_set_allocated_stringvalue2(s2);

                    annotation_value->unsafe_arena_set_allocated_stringstringvalue(ssv);
                },
                [&](const IntStringStringValue& v) {
                    auto* issv = google::protobuf::Arena::Create<v1::PIntStringStringValue>(arena);
                    issv->set_intvalue(v.intValue);

                    auto* s1 = google::protobuf::Arena::Create<google::protobuf::StringValue>(arena);
                    const auto string_value1 = v.stringValue1View();
                    s1->set_value(string_value1.data(), string_value1.size());
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
                    const auto string_value1 = v.stringValue1View();
                    s1->set_value(string_value1.data(), string_value1.size());
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
                              SpanEventImpl* se,
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
                message_event->set_endpoint(se->getEndPoint());
                message_event->set_destinationid(se->getDestinationId());
                next_event->unsafe_arena_set_allocated_messageevent(message_event);
                span_event->unsafe_arena_set_allocated_nextevent(next_event);
            }

            if (auto api_id = se->getApiId(); api_id > 0) {
                span_event->set_apiid(api_id);
            } else if (const auto& operation = se->getOperationName(); !operation.empty()) {
                // Only when there is a name to fall back to: NewSpanEvent("")
                // leaves both the id and the name empty, and an ANNOTATION_API
                // carrying an empty string is worse than none — the collector
                // shows a blank api instead of the caller's service type. Go
                // skips the fallback on an empty operationName and Java's
                // AbstractRecorder.recordApi records nothing for a null
                // descriptor; match them.
                build_string_annotation(span_event->add_annotation(), ANNOTATION_API, operation, arena);
            }

            if (const auto& annotations = se->getAnnotations()->getAnnotations();
                    !annotations.empty()) {
                span_event->mutable_annotation()->Reserve(
                    span_event->annotation_size() + static_cast<int>(annotations.size()));
                for (const auto& [key, val] : annotations) {
                    build_annotation(span_event->add_annotation(), key, val, arena);
                }
            }

            if (const auto& err_str = se->getErrorString(); !err_str.empty()) {
                set_exception_info(span_event, se->getErrorFuncId(), err_str, arena);
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
            // Not collected by the C++ agent: report the uncollected sentinel
            // rather than 0, which the UI would plot as a real measurement.
            memory_stat->set_jvmmemorynonheapused(UNCOLLECTED_STAT_VALUE);
            memory_stat->set_jvmmemorynonheapmax(UNCOLLECTED_STAT_VALUE);
            memory_stat->set_jvmgcoldcount(UNCOLLECTED_STAT_VALUE);
            memory_stat->set_jvmgcoldtime(UNCOLLECTED_STAT_VALUE);
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
            histogram->set_histogramschematype(ACTIVE_TRACE_HISTOGRAM_SCHEMA_TYPE);
            histogram->mutable_activetracecount()->Reserve(
                static_cast<int>(std::size(stat.active_requests_)));
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

            // Already the uncollected sentinel when the platform reading
            // failed (see AgentStatsSnapshot::open_fd_count_), so it travels
            // as-is; Java's AgentStatCollector sends the field the same way.
            auto* file_descriptor = google::protobuf::Arena::Create<v1::PFileDescriptor>(arena);
            file_descriptor->set_openfiledescriptorcount(stat.open_fd_count_);
            agent_stat->unsafe_arena_set_allocated_filedescriptor(file_descriptor);
        }

        void build_url_histogram(v1::PUriHistogram* grpc_histogram, const UrlStatHistogram& url_histogram) {
            grpc_histogram->set_total(url_histogram.total());
            grpc_histogram->set_max(url_histogram.max());
            grpc_histogram->mutable_histogram()->Reserve(URL_STATS_BUCKET_SIZE);
            for (auto i = 0; i < URL_STATS_BUCKET_SIZE; i++) {
                grpc_histogram->add_histogram(url_histogram.histogram(i));
            }
        }

        void build_each_url_stat(v1::PEachUriStat* url_stat,
                                 const UrlKey& key,
                                 const EachUrlStat& each_stats,
                                 google::protobuf::Arena* arena) {
            url_stat->set_uri(key.url_);

            auto* total = google::protobuf::Arena::Create<v1::PUriHistogram>(arena);
            build_url_histogram(total, each_stats.total);
            url_stat->unsafe_arena_set_allocated_totalhistogram(total);

            auto* fail = google::protobuf::Arena::Create<v1::PUriHistogram>(arena);
            build_url_histogram(fail, each_stats.fail);
            url_stat->unsafe_arena_set_allocated_failedhistogram(fail);

            url_stat->set_timestamp(key.tick_);
        }
    }  // namespace

    v1::PTransactionId* build_grpc_transaction_id(const TraceId& tid, google::protobuf::Arena* arena) {
        auto* ptid = google::protobuf::Arena::Create<v1::PTransactionId>(arena);

        // Empty/invalid trace ids never reach serialization: NewSpan turns a
        // failed parseTraceId()/generateTraceId() into a noop span that is never
        // recorded, so any tid arriving here has a valid (non-null) agent id.
        // The runtime guard backs the assert up in release builds (NDEBUG
        // strips it), where a violated invariant must degrade to an empty
        // agent id on one span rather than a null deref inside the host.
        assert(!tid.empty() && "build_grpc_transaction_id requires a valid trace id");
        if (tid.AgentId) {
            ptid->set_agentid(*tid.AgentId);
        }
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

        // Same empty-name guard as build_span_event, and it is the common
        // case here rather than the edge one: an async child span is created
        // with an empty operation on purpose (see NewAsyncSpan), so every one
        // of them used to carry a blank ANNOTATION_API.
        if (auto api_id = span->getApiId(); api_id > 0) {
            grpc_span->set_apiid(api_id);
        } else if (const auto& operation = span->getOperationName(); !operation.empty()) {
            build_string_annotation(grpc_span->add_annotation(), ANNOTATION_API, operation, arena);
        }
        grpc_span->set_loggingtransactioninfo(span->getLoggingFlag());
        grpc_span->set_flag(span->getFlags());
        grpc_span->set_err(span->getErr());

        // Reserve up front: RepeatedPtrField doubles its pointer array as it
        // grows, and on an arena each regrow strands the old array in the
        // arena. Count is known here, so one allocation suffices.
        const auto& events = chunk->getSpanEventChunk();
        grpc_span->mutable_spanevent()->Reserve(static_cast<int>(events.size()));
        for (const auto& e : events) {
            build_span_event(grpc_span->add_spanevent(), e, arena);
        }

        const auto& annotations = span->getAnnotations()->getAnnotations();
        grpc_span->mutable_annotation()->Reserve(
            grpc_span->annotation_size() + static_cast<int>(annotations.size()));
        for (const auto& [key, val] : annotations) {
            build_annotation(grpc_span->add_annotation(), key, val, arena);
        }

        if (const auto& err_str = span->getErrorString(); !err_str.empty()) {
            set_exception_info(grpc_span, span->getErrorFuncId(), err_str, arena);
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
        // The chunk's snapshot, not span->getEndPoint(): the span is still
        // live here and its endpoint_ may be mutated concurrently by the
        // owning thread (see SpanChunk::endpoint_).
        grpc_span->set_endpoint(chunk->getEndPoint());
        grpc_span->set_applicationservicetype(span->getAppType());

        if (span->isAsyncSpan()) {
            auto* aid = google::protobuf::Arena::Create<v1::PLocalAsyncId>(arena);
            aid->set_asyncid(span->getAsyncId());
            aid->set_sequence(span->getAsyncSequence());
            grpc_span->unsafe_arena_set_allocated_localasyncid(aid);
        }

        auto& events = chunk->getSpanEventChunk();
        grpc_span->mutable_spanevent()->Reserve(static_cast<int>(events.size()));
        for (const auto& e : events) {
            build_span_event(grpc_span->add_spanevent(), e, arena);
        }

        return grpc_span;
    }

    v1::PAgentStatBatch* build_agent_stat_batch(const std::vector<AgentStatsSnapshot>& stats,
                                                google::protobuf::Arena* arena) {
        auto* grpc_stat = google::protobuf::Arena::Create<v1::PAgentStatBatch>(arena);

        grpc_stat->mutable_agentstat()->Reserve(static_cast<int>(stats.size()));
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
        uri_stat->mutable_eachuristat()->Reserve(static_cast<int>(m.size()));
        for (const auto& [key, each_stats] : m) {
            auto* url_stat = uri_stat->add_eachuristat();
            build_each_url_stat(url_stat, key, each_stats, arena);
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

        // Reserve both levels for the same reason as build_grpc_span's span
        // events: a growing RepeatedPtrField doubles its pointer array and
        // strands the old one in the arena. Both counts are known and bounded
        // (SpanImpl::kMaxBufferedExceptions exceptions, CallStack::kMaxFrames
        // frames each), so one allocation per field suffices.
        grpc_exception_meta->mutable_exceptions()->Reserve(static_cast<int>(exceptions.size()));
        // Call stacks sharing an exception id are the links of one cause chain
        // and are numbered 0..n in record order, like Java's
        // ExceptionWrapperFactory; a lone exception is depth 0.
        std::unordered_map<int64_t, int32_t> chain_depths;
        for (const auto& exception : exceptions) {
            auto* grpc_exception = grpc_exception_meta->add_exceptions();
            const auto& callstack = exception->getCallStack();
            // The caller-supplied error name is the class name; only a call
            // stack recorded without one falls back to the top frame's module.
            const auto& error_name = callstack.getErrorName();

            grpc_exception->set_exceptionid(exception->getId());
            grpc_exception->set_exceptionclassname(error_name.empty() ? callstack.getModuleName() : error_name);
            grpc_exception->set_exceptionmessage(callstack.getErrorMessage());
            grpc_exception->set_starttime(callstack.getErrorTime());
            grpc_exception->set_exceptiondepth(chain_depths[exception->getId()]++);

            const auto& frames = callstack.getStack();
            grpc_exception->mutable_stacktraceelement()->Reserve(static_cast<int>(frames.size()));
            for (const auto& frame : frames) {
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
