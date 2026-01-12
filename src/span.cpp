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

#include <algorithm>

#include "logging.h"
#include "noop.h"
#include "stat.h"
#include "span.h"

namespace pinpoint {

    static std::atomic<int32_t> async_id_gen{1};

    SpanData::SpanData(AgentService* agent, std::string_view operation) :
        trace_id_{},
        span_id_{},
        parent_span_id_{-1},
        parent_app_name_{},
        parent_app_type_{1},
        parent_app_namespace_{},
        app_type_{agent->getAppType()},
		service_type_{defaults::SPAN_SERVICE_TYPE},
        operation_{operation},
        api_id_{},
        rpc_name_{},
        endpoint_{},
        remote_addr_{},
        acceptor_host_{},
        event_sequence_{0},
        event_depth_(1),
        logging_flag_{SPAN_LOGGING_FLAG_OFF},
        flags_{SPAN_FLAG_NONE},
        err_{SPAN_ERR_NONE},
        error_func_id_{},
        error_string_{},
        start_time_{to_milli_seconds(std::chrono::system_clock::now())},
        end_time_{},
        elapsed_{},
        async_id_{NONE_ASYNC_ID},
        async_sequence_{},
        event_stack_{},
        finished_events{},
        span_event_lock_{},
        url_stat_{},
        annotations_{std::make_shared<PinpointAnnotation>()},
        agent_(agent) {
        api_id_ = agent_->cacheApi(operation, API_TYPE_WEB_REQUEST);
    }

    void SpanData::addSpanEvent(const std::shared_ptr<SpanEventImpl>& se) {
        std::unique_lock<std::mutex> lock(span_event_lock_);
        event_stack_.push(se);
        event_sequence_++;
        event_depth_++;
    }

    void SpanData::finishSpanEvent() {
        std::unique_lock<std::mutex> lock(span_event_lock_);
        const auto se = event_stack_.pop();
        se->finish();
        finished_events.push_back(se);
    }

    void SpanData::parseTraceId(std::string &tid) noexcept {
        std::stringstream ss(tid);
        std::string item;

        std::getline(ss, item, '^');
        trace_id_.AgentId = item;
        std::getline(ss, item, '^');
        auto start_time_result = stoll_(item);
        trace_id_.StartTime = start_time_result.value_or(0);
        std::getline(ss, item, '^');
        auto sequence_result = stoll_(item);
        trace_id_.Sequence = sequence_result.value_or(0);
    }

    void SpanData::setUrlStat(std::string_view url_pattern, std::string_view method, int status_code) try {
        url_stat_ = std::make_unique<UrlStatEntry>(url_pattern, method, status_code);
    } catch (const std::exception& e) {
        LOG_ERROR("set url stat exception = {}", e.what());
    }

    void SpanData::sendUrlStat() {
        if (url_stat_) {
            url_stat_->end_time_ = end_time_;
            url_stat_->elapsed_ = elapsed_;
            agent_->recordUrlStat(std::move(url_stat_));
        }
    }

    void SpanData::sendExceptions() {
        if (!exceptions_.empty()) {
            agent_->recordException(this);
        }
    }

    SpanChunk::SpanChunk(const std::shared_ptr<SpanData>& span_data, const bool final) :
                         span_data_(span_data), event_chunk_{}, final_(final), key_time_(0) {
        const auto& events = span_data_->getFinishedEvents();
        for (const auto& ptr : events) {
            event_chunk_.push_back(ptr);
        }
        span_data_->clearFinishedEvents();
    }

    void SpanChunk::optimizeSpanEvents() {
        if (event_chunk_.empty()) {
            return;
        }

        std::sort(event_chunk_.begin(), event_chunk_.end(),
            [](const std::shared_ptr<SpanEventImpl>& a, const std::shared_ptr<SpanEventImpl>& b)
                  { return a->getSequence() < b->getSequence(); });

        if (final_) {
            key_time_ = span_data_->getStartTime();
        } else {
            const auto& se = event_chunk_[0];
            key_time_ =se->getStartTime();
        }

        int64_t prev_start_time = 0;
        int32_t prev_depth = 0;

        for (size_t i = 0; i < event_chunk_.size(); i++) {
            const auto& se = event_chunk_[i];
            if (i == 0) {
                se->setStartElapsed(static_cast<int32_t>(se->getStartTime() - key_time_));
                prev_depth = se->getDepth();
            } else {
                se->setStartElapsed(static_cast<int32_t>(se->getStartTime() - prev_start_time));
                const auto cur_depth = se->getDepth();
                if (prev_depth == cur_depth) {
                    se->setDepth(0);
                }
                prev_depth = cur_depth;
            }
            prev_start_time = se->getStartTime();
        }
    }

    #define CHECK_FINISHED() \
        do { \
            if (finished_) { \
                LOG_WARN("span is already finished"); \
                return; \
            } \
        } while(0)

    #define CHECK_FINISHED_WITH_RETURN(retval) \
        do { \
            if (finished_) { \
                LOG_WARN("span is already finished"); \
                return (retval); \
            } \
        } while(0)

    #define CHECK_OVERFLOW() \
        do { \
            if (overflow_ > 0) { return; } \
        } while(0)

    #define CHECK_OVERFLOW_WITH_RETURN(retval) \
        do { \
            if (overflow_ > 0) { return (retval); } \
        } while(0)

    SpanImpl::SpanImpl(AgentService* agent, std::string_view operation, std::string_view rpc_point) :
        agent_(agent), data_(nullptr), overflow_(0), finished_(false) {
        data_ = std::make_shared<SpanData>(agent, operation);
        data_->setRpcName(rpc_point);
    }

    SpanEventPtr SpanImpl::NewSpanEvent(std::string_view operation, int32_t service_type) try {
        CHECK_FINISHED_WITH_RETURN(noopSpanEvent());

        auto& cfg = agent_->getConfig();
        const auto depth = data_->getEventDepth();
        const auto seq = data_->getEventSequence();

        if (depth >= cfg.span.max_event_depth || seq >= cfg.span.max_event_sequence) {
            overflow_++;
            LOG_WARN("span event maximum depth/sequence exceeded. (depth:{}, seq:{})", depth, seq);
            return noopSpanEvent();
        }

        auto se = std::make_shared<SpanEventImpl>(data_.get(), operation);
        se->SetServiceType(service_type);
        data_->addSpanEvent(se);

        return se;
    } catch (const std::exception& e) {
        LOG_ERROR("new span event exception = {}", e.what());
        return noopSpanEvent();
    }

    SpanEventPtr SpanImpl::GetSpanEvent() {
        CHECK_FINISHED_WITH_RETURN(noopSpanEvent());
        CHECK_OVERFLOW_WITH_RETURN(noopSpanEvent());

        return data_->topSpanEvent();
    }

    void SpanImpl::record_chunk(bool final) const try {
        auto chunk = std::make_unique<SpanChunk>(data_, final);
        chunk->optimizeSpanEvents();
        agent_->recordSpan(std::move(chunk));
    } catch (const std::exception& e) {
        LOG_ERROR("record span chunk exception = {}", e.what());
    }

    void SpanImpl::EndSpanEvent() {
        CHECK_FINISHED();

        if (overflow_ > 0) {
            overflow_--;
            return;
        }

        data_->finishSpanEvent();

        if (data_->getFinishedEventsCount() >= agent_->getConfig().span.event_chunk_size) {
            record_chunk(false);
        }
    }

    void SpanImpl::EndSpan() {
        CHECK_FINISHED();

        finished_ = true;
        data_->setEndTime();

        if (data_->isAsyncSpan()) {
            data_->finishSpanEvent(); //async span event
        } else {
            auto& stats = data_->getAgent()->getAgentStats();
            stats.dropActiveSpan(data_->getSpanId());
            stats.collectResponseTime(data_->getElapsed());
            data_->sendExceptions();
            data_->sendUrlStat();
        }

        record_chunk(true);
    }

    void SpanImpl::InjectContext(TraceContextWriter& writer) {
        CHECK_FINISHED();
        CHECK_OVERFLOW();

        if (const auto se = data_->topSpanEvent()) {
            const auto& trace_id = data_->getTraceId();
            const int64_t next_span_id = se->generateNextSpanId();

            writer.Set(HEADER_TRACE_ID, trace_id.ToString());
            writer.Set(HEADER_SPAN_ID, std::to_string(next_span_id));
            writer.Set(HEADER_PARENT_SPAN_ID, std::to_string(data_->getSpanId()));
            writer.Set(HEADER_FLAG, std::to_string(data_->getFlags()));
            writer.Set(HEADER_PARENT_APP_NAME, agent_->getAppName());
            writer.Set(HEADER_PARENT_APP_TYPE, std::to_string(agent_->getAppType()));
            writer.Set(HEADER_PARENT_APP_NAMESPACE, "");
            writer.Set(HEADER_HOST, se->getDestinationId());
        }
    }

    void SpanImpl::ExtractContext(TraceContextReader& reader) {
        CHECK_FINISHED();

        if (auto tid = reader.Get(HEADER_TRACE_ID); !tid.has_value()) {
            data_->setTraceId(agent_->generateTraceId());
        } else {
            data_->parseTraceId(tid.value());
        }

        if (const auto span_id = reader.Get(HEADER_SPAN_ID); !span_id.has_value()) {
            data_->setSpanId(generate_span_id());
        } else {
            auto result = stoll_(span_id.value());
            if (result.has_value()) {
                data_->setSpanId(result.value());
            }
        }

        if (const auto parent_span_id = reader.Get(HEADER_PARENT_SPAN_ID); parent_span_id.has_value()) {
            auto result = stoll_(parent_span_id.value());
            if (result.has_value()) {
                data_->setParentSpanId(result.value());
            }
        }

        if (const auto parent_app_name = reader.Get(HEADER_PARENT_APP_NAME); parent_app_name.has_value()) {
            data_->setParentAppName(parent_app_name.value());
        }

        if (const auto parent_app_type = reader.Get(HEADER_PARENT_APP_TYPE); parent_app_type.has_value()) {
            auto result = stoi_(parent_app_type.value());
            if (result.has_value()) {
                data_->setParentAppType(result.value());
            }
        }

        if (const auto flag = reader.Get(HEADER_FLAG); flag.has_value()) {
            auto result = stoi_(flag.value());
            if (result.has_value()) {
                data_->setFlags(result.value());
            }
        }

        if (const auto host = reader.Get(HEADER_HOST); host.has_value()) {
            const auto& v = host.value();
            data_->setAcceptorHost(v);
            data_->setEndPoint(v);
            data_->setRemoteAddr(v);
        }

        data_->getAgent()->getAgentStats().addActiveSpan(data_->getSpanId(), data_->getStartTime());
    }

    SpanPtr SpanImpl::NewAsyncSpan(std::string_view async_operation) try {
        CHECK_FINISHED_WITH_RETURN(noopSpan());
        CHECK_OVERFLOW_WITH_RETURN(noopSpan());

        auto se = data_->topSpanEvent();
        auto async_span = std::make_shared<SpanImpl>(agent_, "", "");

        async_span->data_->setTraceId(data_->getTraceId());
        async_span->data_->setSpanId(data_->getSpanId());

        if (se->getAsyncId() == NONE_ASYNC_ID) {
            int32_t async_id;
            do {
                async_id = async_id_gen.fetch_add(1);
            } while (async_id == NONE_ASYNC_ID);
            se->setAsyncId(async_id);
        }
        se->incrAsyncSeq();

        async_span->data_->setAsyncId(se->getAsyncId());
        async_span->data_->setAsyncSequence(se->getAsyncSeqGen());

        const auto async_se = std::make_shared<SpanEventImpl>(async_span->data_.get(), "");
        auto async_api_id = agent_->cacheApi(async_operation, API_TYPE_INVOCATION);
        async_se->setApiId(async_api_id);
        async_se->SetServiceType(SERVICE_TYPE_ASYNC);
        async_span->data_->addSpanEvent(async_se);

        return async_span;
    } catch (const std::exception& e) {
        LOG_ERROR("new async span exception = {}", e.what());
        return noopSpan();
    }

    void SpanImpl::SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) {
        CHECK_FINISHED();
        data_->setUrlStat(url_pattern, method, status_code);
    }

    void SpanImpl::SetServiceType(int32_t service_type) {
        CHECK_FINISHED();
        data_->setServiceType(service_type);
    }

    void SpanImpl::SetStartTime(std::chrono::system_clock::time_point start_time) {
        CHECK_FINISHED();
        data_->setStartTime(start_time);
    }

    void SpanImpl::SetRemoteAddress(std::string_view address) {
        CHECK_FINISHED();
        data_->setRemoteAddr(address);
    }

    void SpanImpl::SetEndPoint(std::string_view endpoint) {
        CHECK_FINISHED();
        data_->setEndPoint(endpoint);
    }

    void SpanImpl::SetError(std::string_view error_message) {
        CHECK_FINISHED();
        SetError("Error", error_message);
    }

    void SpanImpl::SetError(std::string_view error_name, std::string_view error_message) {
        CHECK_FINISHED();

        data_->setErrorFuncId(agent_->cacheError(error_name));
        data_->setErrorString(error_message);
        data_->setErr(1);
    }

    void SpanImpl::SetStatusCode(int status) {
        CHECK_FINISHED();

        data_->getAnnotations()->AppendInt(ANNOTATION_HTTP_STATUS_CODE, status);
        if (agent_->isStatusFail(status)) {
            data_->setErr(1);
        }
    }

    void SpanImpl::RecordHeader(HeaderType which, HeaderReader& reader) {
        CHECK_FINISHED();
        agent_->recordServerHeader(which, reader, data_->getAnnotations());
    }

	const std::string LOG_TRACE_ID_KEY = "PtxId";
	const std::string LOG_SPAN_ID_KEY = "PspanId";

    void SpanImpl::SetLogging(TraceContextWriter& writer) {
        CHECK_FINISHED();

        data_->setLoggingFlag();

        writer.Set(LOG_TRACE_ID_KEY, data_->getTraceId().ToString());
        writer.Set(LOG_SPAN_ID_KEY, std::to_string(data_->getSpanId()));
    }

}  // namespace pinpoint
