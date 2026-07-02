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
#include <cassert>

#include "logging.h"
#include "noop.h"
#include "stat.h"
#include "span.h"

namespace pinpoint {

    static std::atomic<int32_t> async_id_gen{1};

    SpanData::SpanData(std::string_view operation, int32_t app_type, int32_t api_id) :
        trace_id_{},
        span_id_{},
        parent_span_id_{-1},
        parent_app_name_{},
        parent_app_type_{1},
        parent_app_namespace_{},
        parent_service_name_{},
        app_type_{app_type},
        service_type_{defaults::SPAN_SERVICE_TYPE},
        operation_{operation},
        api_id_{api_id},
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
        annotations_{std::make_unique<PinpointAnnotation>()} {}

    SpanEventImpl* SpanData::addSpanEvent(std::unique_ptr<SpanEventImpl> se) {
        const auto [sequence, depth] = nextEventSequenceAndDepth();
        se->setSequence(sequence);
        se->setDepth(depth);
 
        auto* event = se.get();
        event_stack_.push(std::move(se));
        return event;
    }

    void SpanData::finishSpanEvent() {
        auto se = event_stack_.pop();
        if (se) {
            se->finish();
            storeFinishedEvent(std::move(se));
        } else {
            LOG_WARN("finishSpanEvent: abnormal span - has no event");
        }
    }

    void SpanData::storeFinishedEvent(std::unique_ptr<SpanEventImpl> se) {
        const auto sequence = se->getSequence();
        if (finished_events.empty() || finished_events.back()->getSequence() < sequence) {
            finished_events.emplace_back(std::move(se));
            return;
        }
        if (sequence < finished_events.front()->getSequence()) {
            finished_events.emplace_front(std::move(se));
            return;
        }

        auto pos = std::lower_bound(
            finished_events.begin(), finished_events.end(), sequence,
            [](const std::unique_ptr<SpanEventImpl>& event, int32_t sequence) {
                return event->getSequence() < sequence;
            });
        finished_events.insert(pos, std::move(se));
    }

    void SpanData::takeFinishedEvents(std::vector<std::unique_ptr<SpanEventImpl>>& out) {
        out.clear();
        if (finished_events.empty()) {
            return;
        }

        out.reserve(finished_events.size());
        for (auto& event : finished_events) {
            out.emplace_back(std::move(event));
        }
        finished_events.clear();
    }

    void SpanData::parseTraceId(std::string &txid) noexcept {
        constexpr size_t kMaxAgentIdLength = 24;
        constexpr size_t kMaxInt64StringLength = 20; // max digits of int64_t

        std::string_view sv(txid);

        // Parse AgentId (first field before '^')
        const auto pos1 = sv.find('^');
        if (pos1 == std::string_view::npos) {
            LOG_WARN("parsing Txid: invalid txid format = {}", sv);
            return;
        }
        if (pos1 > kMaxAgentIdLength) {
            LOG_WARN("parsing Txid: AgentId too long (length={}, max={})", pos1, kMaxAgentIdLength);
            return;
        }
        trace_id_.AgentId = std::string(sv.substr(0, pos1));

        // Parse StartTime (second field)
        const auto pos2 = sv.find('^', pos1 + 1);
        if (pos2 == std::string_view::npos) {
            LOG_WARN("parsing Txid: invalid txid format = {}", sv);
            return;
        }
        const auto start_time_len = pos2 - pos1 - 1;
        if (start_time_len > kMaxInt64StringLength) {
            LOG_WARN("parsing Txid: StartTime too long (length={}, max={})", start_time_len, kMaxInt64StringLength);
            return;
        }
        trace_id_.StartTime = stoll_(sv.substr(pos1 + 1, start_time_len)).value_or(0);

        // Parse Sequence (third field)
        const auto sequence_str = sv.substr(pos2 + 1);
        if (sequence_str.length() > kMaxInt64StringLength) {
            LOG_WARN("parsing Txid: Sequence too long (length={}, max={})", sequence_str.length(), kMaxInt64StringLength);
            return;
        }
        trace_id_.Sequence = stoll_(sequence_str).value_or(0);
    }

    SpanChunk::SpanChunk(const std::shared_ptr<SpanData>& span_data, const bool final) :
                         span_data_(span_data),
                         event_chunk_{},
                         final_(final), key_time_(0) {
        span_data_->takeFinishedEvents(event_chunk_);
    }

    void SpanChunk::optimizeSpanEvents() {
        if (event_chunk_.empty()) {
            return;
        }

        int64_t prev_start_time = 0;
        int32_t prev_depth = 0;

        for (size_t i = 0; i < event_chunk_.size(); i++) {
            const auto& se = event_chunk_[i];
            if (i == 0) {
                if (final_) {
                    key_time_ = span_data_->getStartTime();
                } else {
                    key_time_ =se->getStartTime();
                }
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
        agent_(agent),
        data_(nullptr),
        overflow_(0),
        finished_(false),
        url_stat_{},
        exceptions_{} {
        assert(agent_ != nullptr);
        config_ = agent_->getConfig();
        const auto app_type = agent_->getAppType();
        const auto api_id = agent_->cacheApi(operation, API_TYPE_WEB_REQUEST);
        data_ = std::make_shared<SpanData>(operation, app_type, api_id);
        data_->setRpcName(rpc_point);
    }

    void SpanImpl::checkOwnerThread() {
        const auto current = std::this_thread::get_id();
        // Fast path: an already-bound span does a plain relaxed load. Even a
        // failed compare_exchange is an RMW that takes the cache line
        // exclusive, so the CAS is reserved for the one-time binding below.
        auto owner = owner_thread_id_.load(std::memory_order_relaxed);
        if (owner == current) {
            return;
        }
        // Bind to the first thread that records a span event. The CAS resolves
        // the (already contract-violating) race of two threads both observing
        // an unbound span: exactly one binds, the loser falls through with
        // `owner` holding the winner and reports the violation below.
        if (owner == std::thread::id{} &&
            owner_thread_id_.compare_exchange_strong(owner, current, std::memory_order_relaxed)) {
            return;
        }
        LOG_ERROR("span accessed from another thread (owner hash={}, current hash={}): a span "
                  "must be used by a single thread; use NewAsyncSpan() to continue on another thread",
                  std::hash<std::thread::id>{}(owner), std::hash<std::thread::id>{}(current));
        assert(false && "SpanImpl accessed from a thread other than its owner");
    }

    SpanEventPtr SpanImpl::NewSpanEvent(std::string_view operation, int32_t service_type) try {
        CHECK_FINISHED_WITH_RETURN(noopSpanEvent());
        checkOwnerThread();

        const auto& cfg = config_;
        const auto depth = data_->getEventDepth();
        const auto seq = data_->getEventSequence();

        if (depth >= cfg->span.max_event_depth || seq >= cfg->span.max_event_sequence) {
            overflow_++;
            LOG_WARN("span event maximum depth/sequence exceeded. (depth:{}, seq:{})", depth, seq);
            return noopSpanEvent();
        }

        auto se = std::make_unique<SpanEventImpl>(this, operation);
        se->SetServiceType(service_type);
        return data_->addSpanEvent(std::move(se));
    } catch (const std::exception& e) {
        LOG_ERROR("new span event exception = {}", e.what());
        return noopSpanEvent();
    }

    SpanEventPtr SpanImpl::GetSpanEvent() {
        CHECK_FINISHED_WITH_RETURN(noopSpanEvent());
        CHECK_OVERFLOW_WITH_RETURN(noopSpanEvent());

        auto se = data_->topSpanEvent();
        if (!se) {
            LOG_WARN("GetSpanEvent: abnormal span - has no event");
            return noopSpanEvent();
        }
        return se;
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

        // Consume one overflow placeholder if any, using a CAS loop so two
        // concurrent EndSpanEvent calls cannot both decrement the same value
        // and drive overflow_ negative (which would later pop real events that
        // were never overflowed and desync the event stack).
        int32_t pending = overflow_.load();
        while (pending > 0) {
            if (overflow_.compare_exchange_weak(pending, pending - 1)) {
                return;
            }
        }

        data_->finishSpanEvent();

        if (data_->getFinishedEventsCount() >= config_->span.event_chunk_size) {
            record_chunk(false);
        }
    }

    void SpanImpl::EndSpan() {
        // Atomic exchange so only the first caller proceeds: a check-then-set
        // would let two concurrent EndSpan calls both pass the guard and run
        // record_chunk / dropActiveSpan / collectResponseTime twice.
        if (finished_.exchange(true)) {
            LOG_WARN("span is already finished");
            return;
        }

        data_->setEndTime();

        if (data_->isAsyncSpan()) {
            data_->finishSpanEvent(); //async span event
        } else {
            auto& stats = agent_->getAgentStats();
            stats.dropActiveSpan(data_->getSpanId());
            stats.collectResponseTime(data_->getElapsed());
            sendExceptions();
            sendUrlStat();
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
            // The agent's own service name is sent only when present, which (per
            // uid.version handling) means uid.version=v4 only; v1/v3 leave it empty
            // and the header is omitted. Mirrors Java DefaultRequestTraceWriter,
            // which writes Pinpoint-pServiceName only when serviceName != null.
            if (const auto& service_name = agent_->getServiceName(); !service_name.empty()) {
                writer.Set(HEADER_PARENT_SERVICE_NAME, service_name);
            }
            writer.Set(HEADER_PARENT_APP_NAMESPACE, "");
            writer.Set(HEADER_HOST, se->getDestinationId());
        }
    }

    void SpanImpl::ExtractContext(TraceContextReader& reader) {
        CHECK_FINISHED();
        extractContext(reader, reader.Get(HEADER_TRACE_ID));
    }

    void SpanImpl::extractContext(TraceContextReader& reader, std::optional<std::string> tid) {
        CHECK_FINISHED();

        if (!tid.has_value()) {
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

        if (const auto parent_app_namespace = reader.Get(HEADER_PARENT_APP_NAMESPACE); parent_app_namespace.has_value()) {
            data_->setParentAppNamespace(parent_app_namespace.value());
        }

        if (const auto parent_service_name = reader.Get(HEADER_PARENT_SERVICE_NAME); parent_service_name.has_value()) {
            data_->setParentServiceName(parent_service_name.value());
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

        agent_->getAgentStats().addActiveSpan(data_->getSpanId(), data_->getStartTime());
    }

    SpanPtr SpanImpl::NewAsyncSpan(std::string_view async_operation) try {
        CHECK_FINISHED_WITH_RETURN(noopSpan());
        CHECK_OVERFLOW_WITH_RETURN(noopSpan());

        auto se = data_->topSpanEvent();
        if (!se) {
            LOG_WARN("NewAsyncSpan: abnormal span - has no event");
            return noopSpan();
        }
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

        auto async_se = std::make_unique<SpanEventImpl>(async_span.get(), "");
        auto async_api_id = agent_->cacheApi(async_operation, API_TYPE_INVOCATION);
        async_se->setApiId(async_api_id);
        async_se->SetServiceType(SERVICE_TYPE_ASYNC);
        async_span->data_->addSpanEvent(std::move(async_se));

        return async_span;
    } catch (const std::exception& e) {
        LOG_ERROR("new async span exception = {}", e.what());
        return noopSpan();
    }

    void SpanImpl::decrEventDepth() {
        data_->decrEventDepth();
    }

    void SpanImpl::SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) {
        CHECK_FINISHED();
        url_stat_.emplace(url_pattern, method, status_code);
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

    void SpanImpl::SetAcceptorHost(std::string_view host) {
        CHECK_FINISHED();
        data_->setAcceptorHost(host);
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

    void SpanImpl::sendUrlStat() {
        if (!url_stat_) {
            return;
        }
        url_stat_->end_time_ = data_->getEndTime();
        url_stat_->elapsed_ = data_->getElapsed();
        url_stat_->failed_ = agent_->isStatusFail(url_stat_->status_code_);
        agent_->recordUrlStat(std::move(*url_stat_));
        url_stat_.reset();
    }

    void SpanImpl::sendExceptions() {
        if (!exceptions_.empty()) {
            agent_->recordException(data_->getTraceId(), data_->getSpanId(), getUrlTemplate(), takeExceptions());
        }
    }

    void SpanImpl::SetLogging(TraceContextWriter& writer) {
        CHECK_FINISHED();

        data_->setLoggingFlag();

        writer.Set(LOG_TRACE_ID_KEY, data_->getTraceId().ToString());
        writer.Set(LOG_SPAN_ID_KEY, std::to_string(data_->getSpanId()));
    }

}  // namespace pinpoint
