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
#include <charconv>

#include "http.h"
#include "logging.h"
#include "noop.h"
#include "stat.h"
#include "span.h"

namespace pinpoint {

    static std::atomic<int32_t> async_id_gen{1};

    SpanData::SpanData(std::string_view operation, int32_t app_type, int32_t api_id) :
        app_type_{app_type},
        // Kept only as the fallback for a span with no api id: whenever the id
        // is positive the builders send it and never read the name (see
        // build_grpc_span), so storing it would allocate once per span (an
        // operation name usually exceeds libstdc++'s 15-char SSO threshold)
        // for bytes nothing looks at.
        operation_{api_id > 0 ? std::string_view{} : operation},
        api_id_{api_id},
        start_time_{to_milli_seconds(std::chrono::system_clock::now())} {}

    SpanEventImpl* SpanData::addSpanEvent(std::unique_ptr<SpanEventImpl> se) {
        const auto [sequence, depth] = nextEventSequenceAndDepth();
        se->setSequence(sequence);
        se->setDepth(depth);
 
        auto* event = se.get();
        event_stack_.push_back(std::move(se));
        return event;
    }

    void SpanData::finishSpanEvent(SpanEventImpl* expected) {
        if (topSpanEvent() != expected) {
            LOG_WARN("finishSpanEvent: span event ended out of order; implicitly finishing intermediate events");
        }
        // `expected` is always on the stack when this is reached: every pop
        // path marks the event finished, and EndEvent's finished_ exchange
        // rejects an already-popped event before delegating here. So this
        // loop is a single pop in the well-nested case and stops at
        // `expected` when unwinding out-of-order ends.
        while (auto se = popSpanEvent()) {
            const bool found = se.get() == expected;
            se->finish();
            storeFinishedEvent(std::move(se));
            if (found) {
                return;
            }
        }
        LOG_WARN("finishSpanEvent: abnormal span - ended event not on stack");
    }

    size_t SpanData::finishOpenSpanEvents() {
        size_t count = 0;
        while (auto se = popSpanEvent()) {
            se->finish();
            storeFinishedEvent(std::move(se));
            ++count;
        }
        return count;
    }

    void SpanData::storeFinishedEvent(std::unique_ptr<SpanEventImpl> se) try {
        const auto sequence = se->getSequence();
        if (finished_events.empty() || finished_events.back()->getSequence() < sequence) {
            finished_events.emplace_back(std::move(se));
            return;
        }
        auto pos = std::lower_bound(
            finished_events.begin(), finished_events.end(), sequence,
            [](const std::unique_ptr<SpanEventImpl>& event, int32_t sequence) {
                return event->getSequence() < sequence;
            });
        finished_events.insert(pos, std::move(se));
    } catch (...) {
        // Insertion throws only on allocation, before the element is moved in
        // (unique_ptr moves are noexcept), so `se` still owns the event. It
        // must survive the unwind: user code may hold its raw SpanEventPtr,
        // and the duplicate-EndEvent no-op promised in pinpoint/tracer.h needs
        // a live object. Retire it unsent instead.
        try {
            retired_events_.emplace_back(std::move(se));
            LOG_ERROR("store finished event: allocation failed; span event dropped from trace");
        } catch (...) {
            // Even the one-pointer fallback failed: leak the event rather
            // than free memory a user-held SpanEventPtr may still point at.
            se.release();
            LOG_ERROR("store finished event: allocation failed; span event leaked to keep user handles valid");
        }
    }

    void SpanData::takeFinishedEvents(std::vector<SpanEventImpl*>& out) {
        out.clear();
        if (finished_events.empty()) {
            return;
        }

        out.reserve(finished_events.size());
        // No reserve() on retired_events_: reserve(size + batch) allocates
        // exactly that on every chunk flush, so a long-lived span reallocates
        // and copies the whole vector per flush (O(E^2/C)). Plain emplace_back
        // grows geometrically instead — O(log E) reallocations overall.
        try {
            for (auto& event : finished_events) {
                out.push_back(event.get());
                retired_events_.emplace_back(std::move(event));
            }
        } catch (...) {
            // A mid-loop failure leaves already-processed events owned by
            // retired_events_ and their finished_events slots null. Compact
            // the nulls away — a later flush must not hand a chunk null
            // pointers — then rethrow to record_chunk / EndSpan, which logs
            // and abandons the chunk.
            finished_events.erase(
                std::remove_if(finished_events.begin(), finished_events.end(),
                               [](const std::unique_ptr<SpanEventImpl>& event) {
                                   return event == nullptr;
                               }),
                finished_events.end());
            out.clear();
            throw;
        }
        finished_events.clear();
    }

    SpanChunk::SpanChunk(const std::shared_ptr<SpanData>& span_data, const bool final) :
                         span_data_(span_data),
                         event_chunk_{},
                         final_(final), key_time_(0),
                         endpoint_(span_data->getEndPoint()) {
        span_data_->takeFinishedEvents(event_chunk_);
    }

    SpanChunk::~SpanChunk() {
        // A chunk dies either consumed by the gRPC builders (which copy every
        // field into arena protobuf) or dropped unsent; either way it was the
        // last reader of its events' payload — accessors have been
        // finished_-guarded no-ops since finish() — so free it now instead of
        // holding it until the span data dies. The event objects stay alive in
        // retired_events_ as tombstones, keeping user-held raw SpanEventPtr
        // handles valid. Ordering comes from the span queue's shard mutex: the
        // destroying thread acquired the lock the owning thread released at
        // enqueue, so the payload writes are visible here.
        for (auto* se : event_chunk_) {
            se->releaseRetiredPayload();
        }
    }

    void SpanChunk::optimizeSpanEvents() {
        if (event_chunk_.empty()) {
            return;
        }

        int64_t prev_start_time = 0;
        int32_t prev_depth = 0;

        // The startElapsed casts below assume epoch-millisecond timestamps
        // whose in-chunk deltas fit int32 (the wire field): a delta beyond
        // INT32_MAX ms — or a negative one from a user-supplied start time —
        // wraps unclamped, unlike the elapsed computation in finish().
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

    // retval unparenthesized so an empty argument yields a plain `return;`.
    #define CHECK_FINISHED_WITH_RETURN(retval) \
        do { \
            if (finished_) { \
                LOG_WARN("span is already finished"); \
                return retval; \
            } \
        } while(0)

    #define CHECK_FINISHED() CHECK_FINISHED_WITH_RETURN()

    #define CHECK_OVERFLOW_WITH_RETURN(retval) \
        do { \
            if (overflow_ > 0) { return (retval); } \
        } while(0)

    SpanImpl::~SpanImpl() {
        // Self-heals spans dropped without EndSpan, and is the hard backstop
        // for the intrusive node: active_node_ lives inside this object, so a
        // still-linked node here would leave dangling pointers in its shard
        // list. Checking the node rather than finished_ still covers an
        // EndSpan that failed between setting finished_ and its drop, while a
        // normally ended span returns after one atomic load without touching
        // agent_ — which matters for non-owning AgentService implementations
        // whose selfRef() is empty.
        if (agent_ != nullptr && active_node_.isLinked()) {
            try {
                agent_->getAgentStats().dropActiveSpan(active_node_);
            } catch (...) {
            }
        }
    }

    SpanImpl::SpanImpl(AgentService* agent, std::string_view operation, std::string_view rpc_point,
                       std::shared_ptr<const AgentRuntime> runtime) :
        agent_ref_(agent != nullptr ? agent->selfRef() : nullptr),
        agent_(agent),
        runtime_(std::move(runtime)),
        data_(nullptr),
        overflow_(0),
        finished_(false),
        url_stat_{},
        exceptions_{} {
        assert(agent_ != nullptr);
        config_ = runtime_ ? runtime_->config : agent_->getConfig();
        const auto app_type = agent_->getAppType();
        // Async child spans are created with an empty operation (see
        // NewAsyncSpan): skip the api-cache lookup — api_id 0 is simply not
        // serialized (grpc_builders guards on api_id > 0).
        const auto api_id = operation.empty() ? 0 : agent_->cacheApi(operation, API_TYPE_WEB_REQUEST);
        data_ = std::make_shared<SpanData>(operation, app_type, api_id);
        data_->setRpcName(rpc_point);
    }

    SpanConfigSnapshot make_config_snapshot(const AgentService& agent, const Config& config) {
        SpanConfigSnapshot snapshot;
        snapshot.application_name = agent.getAppName();
        snapshot.application_type = agent.getAppType();
        snapshot.service_name = agent.getServiceName();
        snapshot.max_event_depth = config.span.max_event_depth;
        snapshot.max_event_sequence = config.span.max_event_sequence;
        snapshot.http_server_headers[HTTP_REQUEST] = config.http.server.rec_request_header;
        snapshot.http_server_headers[HTTP_RESPONSE] = config.http.server.rec_response_header;
        snapshot.http_server_headers[HTTP_COOKIE] = config.http.server.rec_request_cookie;
        snapshot.http_client_headers[HTTP_REQUEST] = config.http.client.rec_request_header;
        snapshot.http_client_headers[HTTP_RESPONSE] = config.http.client.rec_response_header;
        snapshot.http_client_headers[HTTP_COOKIE] = config.http.client.rec_request_cookie;
        snapshot.sql_trace_bind_value = config.sql.trace_bind_value;
        snapshot.revision = config.revision;
        return snapshot;
    }

    SpanConfigSnapshot SpanImpl::GetConfigSnapshot() const try {
        return config_ ? make_config_snapshot(*agent_, *config_) : SpanConfigSnapshot{};
    } catch (...) {
        // Public API boundary, same as AgentImpl::GetConfigSnapshot: snapshot
        // building only copies strings/vectors, but an allocation failure
        // must not leak into the embedder.
        return {};
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
            // Throttled: an app that routinely exceeds the limits would hit
            // this once per discarded event, serializing its request threads
            // on the logger mutex and its per-line write+flush.
            LOG_WARN_THROTTLED("span event maximum depth/sequence exceeded. (depth:{}, seq:{})", depth, seq);
            // Overflow is a profiling depth limit, not a sampling decision:
            // the returned event records nothing but still propagates the full
            // trace context, so the distributed trace is not cut here.
            return disabledSpanEvent();
        }

        auto se = std::make_unique<SpanEventImpl>(this, operation);
        se->SetServiceType(service_type);
        return data_->addSpanEvent(std::move(se));
    } CATCH_AND_LOG_RETURN("new span event", noopSpanEvent())

    SpanEventPtr SpanImpl::GetSpanEvent() try {
        CHECK_FINISHED_WITH_RETURN(noopSpanEvent());
        // While overflowed, the top of the stack is a discarded event: hand
        // out the disabled event so InjectContext keeps working (see
        // NewSpanEvent).
        CHECK_OVERFLOW_WITH_RETURN(disabledSpanEvent());

        auto se = data_->topSpanEvent();
        if (!se) {
            LOG_WARN("GetSpanEvent: abnormal span - has no event");
            return noopSpanEvent();
        }
        return se;
    } CATCH_AND_LOG_RETURN("get span event", noopSpanEvent())

    void SpanImpl::SetAnnotation(int32_t key, int32_t value) try {
        // A finished span's annotation list may already be under serialization
        // on the gRPC worker; appending would race its iteration.
        CHECK_FINISHED();
        data_->getAnnotations()->AppendData(key, AnnotationData(value));
    } CATCH_AND_LOG("set annotation")

    void SpanImpl::SetAnnotation(int32_t key, int64_t value) try {
        CHECK_FINISHED();
        data_->getAnnotations()->AppendData(key, AnnotationData(value));
    } CATCH_AND_LOG("set annotation")

    void SpanImpl::SetAnnotation(int32_t key, std::string_view value) try {
        CHECK_FINISHED();
        data_->getAnnotations()->AppendData(key, AnnotationData(value));
    } CATCH_AND_LOG("set annotation")

    void SpanImpl::SetAnnotation(int32_t key,
                                 std::string_view value1,
                                 std::string_view value2) try {
        CHECK_FINISHED();
        data_->getAnnotations()->AppendData(key, AnnotationData(value1, value2));
    } CATCH_AND_LOG("set annotation")

    void SpanImpl::SetAnnotation(int32_t key, int64_t long_value,
                                 int32_t int_value1, int32_t int_value2,
                                 int32_t byte_value1, int32_t byte_value2,
                                 std::string_view string_value) try {
        CHECK_FINISHED();
        data_->getAnnotations()->AppendData(
            key, AnnotationData(long_value, int_value1, int_value2,
                                byte_value1, byte_value2, string_value));
    } CATCH_AND_LOG("set annotation")

    void SpanImpl::record_chunk(bool final) const try {
        auto chunk = std::make_unique<SpanChunk>(data_, final);
        chunk->optimizeSpanEvents();
        agent_->recordSpan(std::move(chunk));
    } CATCH_AND_LOG("record span chunk")

    void SpanImpl::endSpanEvent(SpanEventImpl* se) {
        CHECK_FINISHED();

        data_->finishSpanEvent(se);

        // event_chunk_size is clamped to >= 1 by make_config()'s range
        // validation (MIN_SPAN_EVENT_CHUNK_SIZE in config.cpp) before any
        // config reaches an agent, so the cast cannot produce a huge
        // unsigned value.
        if (data_->getFinishedEventsCount() >= static_cast<size_t>(config_->span.event_chunk_size)) {
            record_chunk(false);
        }
    }

    void SpanImpl::endDisabledSpanEvent() {
        CHECK_FINISHED();

        // Consume one overflow placeholder, using a CAS loop so two
        // concurrent calls cannot both decrement the same value and drive
        // overflow_ negative. Overflowed events are never on the stack, so
        // this must never fall through to a real pop.
        int32_t pending = overflow_.load();
        while (pending > 0) {
            if (overflow_.compare_exchange_weak(pending, pending - 1)) {
                return;
            }
        }
        LOG_WARN("span event is already finished");
    }

    void SpanImpl::EndSpan() try {
        // Atomic exchange so only the first caller proceeds: a check-then-set
        // would let two concurrent EndSpan calls both pass the guard and run
        // record_chunk / dropActiveSpan / collectResponseTime twice.
        if (finished_.exchange(true)) {
            LOG_WARN("span is already finished");
            return;
        }

        data_->setEndTime();

        // Drain every event still on the stack. An async span legitimately
        // has one (its root event stays open until EndSpan); anything beyond
        // that is a missed user-level EndEvent. Finishing them here keeps
        // them in the final chunk — and, for async spans, prevents a leftover
        // child from being finished in place of the async root.
        const auto open_events = data_->finishOpenSpanEvents();
        const auto expected_open = data_->isAsyncSpan() ? size_t{1} : size_t{0};
        if (open_events > expected_open) {
            LOG_WARN("EndSpan: {} span event(s) not ended by user code; finished implicitly",
                     open_events - expected_open);
        }

        if (data_->isAsyncSpan()) {
            if (open_events == 0) {
                LOG_WARN("EndSpan: abnormal async span - has no event");
            }
            // SetError on an async span event routes to that span's own
            // exception list, and the non-async branch below is skipped — so
            // flush here, or ANNOTATION_EXCEPTION_ID references metadata that
            // is never sent and the captured call stack is lost.
            sendExceptions();
        } else {
            auto& stats = agent_->getAgentStats();
            stats.dropActiveSpan(active_node_);
            stats.collectResponseTime(data_->getElapsed());
            // sendExceptions() must precede sendUrlStat(): the latter resets
            // url_stat_, which getUrlTemplate() reads to tag the exception with
            // its uri template.
            sendExceptions();
            sendUrlStat();
        }

        // Seal before the final chunk reaches the gRPC worker: an annotation
        // handle obtained while the span was active must no-op from here on,
        // not grow a vector the worker is iterating.
        data_->getAnnotations()->seal();
        record_chunk(true);
    } catch (const std::exception& e) {
        // Reached only on allocation failure. EndSpan is commonly called from
        // destructors in host code, so the exception must not escape.
        LOG_ERROR("end span exception = {}", e.what());
        releaseActiveSpanOnError();
    } catch (...) {
        LOG_ERROR("end span unknown exception");
        releaseActiveSpanOnError();
    }

    void SpanImpl::releaseActiveSpanOnError() noexcept {
        // The destructor would unlink the node anyway, but user code may hold
        // the span handle long after a failed EndSpan — unlink now so it stops
        // counting as an active request. Idempotent; async spans never link.
        try {
            if (agent_ != nullptr) {
                agent_->getAgentStats().dropActiveSpan(active_node_);
            }
        } catch (...) {
        }
    }

    SpanEventPtr SpanImpl::disabledSpanEvent() {
        if (!disabled_event_) {
            disabled_event_ = std::make_unique<DisabledSpanEvent>(this);
        }
        return disabled_event_.get();
    }

    void SpanImpl::injectContext(TraceContextWriter& writer, int64_t next_span_id, std::string_view host) {
        CHECK_FINISHED();

        // Runs on every outbound call: format numeric headers into a stack
        // buffer instead of std::to_string temporaries. Reusing one buffer
        // across Set calls is safe because Set must consume or copy each
        // borrowed view before returning.
        char buf[32];
        const auto num = [&buf](auto value) -> std::string_view {
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            (void)ec; // int64/int32 always fits in 32 chars
            return {buf, static_cast<size_t>(ptr - buf)};
        };

        writer.Set(HEADER_TRACE_ID, data_->getTraceIdWire());
        writer.Set(HEADER_SPAN_ID, num(next_span_id));
        writer.Set(HEADER_PARENT_SPAN_ID, num(data_->getSpanId()));
        writer.Set(HEADER_FLAG, num(data_->getFlags()));
        writer.Set(HEADER_PARENT_APP_NAME, agent_->getAppName());
        writer.Set(HEADER_PARENT_APP_TYPE, num(agent_->getAppType()));
        // Sent only when present — i.e. uid.version=v4; v1/v3 leave it empty
        // and the header is omitted. Mirrors Java DefaultRequestTraceWriter.
        if (const auto& service_name = agent_->getServiceName(); !service_name.empty()) {
            writer.Set(HEADER_PARENT_SERVICE_NAME, service_name);
        }
        writer.Set(HEADER_PARENT_APP_NAMESPACE, "");
        writer.Set(HEADER_HOST, host);
    }

    void SpanImpl::extractContext(TraceContextReader& reader, TraceId trace_id) {
        CHECK_FINISHED();

        // The trace id is resolved by NewSpan (parsed or generated) before this
        // call; an empty/failed one never reaches here — NewSpan hands back a
        // noop span in that case.
        data_->setTraceId(std::move(trace_id));

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
            // One copy, not three: the endpoint and remote address default to
            // the acceptor host in their getters (see SpanData).
            data_->setAcceptorHost(host.value());
        }

        agent_->getAgentStats().addActiveSpan(active_node_, data_->getSpanId(), data_->getStartTime());
    }

    SpanPtr SpanImpl::NewAsyncSpan(std::string_view async_operation) try {
        CHECK_FINISHED_WITH_RETURN(noopSpan());
        CHECK_OVERFLOW_WITH_RETURN(noopSpan());

        auto se = data_->topSpanEvent();
        if (!se) {
            LOG_WARN_THROTTLED("NewAsyncSpan: abnormal span - has no event");
            return noopSpan();
        }
        // Hand down this span's runtime snapshot: the async child records into
        // the same trace, so it must run under the same config generation (and
        // it skips another atomic runtime load).
        auto async_span = std::make_shared<SpanImpl>(agent_, "", "", runtime_);

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
    } CATCH_AND_LOG_RETURN("new async span", noopSpan())

    SpanPtr SpanImpl::NewAsyncSpan(std::string_view async_operation,
                                   int32_t async_id, int32_t async_sequence) try {
        CHECK_FINISHED_WITH_RETURN(noopSpan());

        // The caller manages its span events outside this library (see
        // RecordSpanEvent), so the async link arrives as arguments instead of
        // being stamped onto this span's (empty) native event stack; the
        // caller flushes async_id with its own parent event.
        auto async_span = std::make_shared<SpanImpl>(agent_, "", "", runtime_);
        async_span->data_->setTraceId(data_->getTraceId());
        async_span->data_->setSpanId(data_->getSpanId());
        async_span->data_->setAsyncId(async_id);
        async_span->data_->setAsyncSequence(async_sequence);

        auto async_se = std::make_unique<SpanEventImpl>(async_span.get(), "");
        auto async_api_id = agent_->cacheApi(async_operation, API_TYPE_INVOCATION);
        async_se->setApiId(async_api_id);
        async_se->SetServiceType(SERVICE_TYPE_ASYNC);
        async_span->data_->addSpanEvent(std::move(async_se));

        return async_span;
    } CATCH_AND_LOG_RETURN("new async span", noopSpan())

    SpanEventPtr SpanImpl::RecordSpanEvent(std::string_view operation, int32_t service_type,
                                           int32_t sequence, int32_t depth,
                                           int64_t start_time_ms, int64_t end_time_ms,
                                           int32_t async_id) try {
        CHECK_FINISHED_WITH_RETURN(noopSpanEvent());
        checkOwnerThread();

        // Backstop only: a wrapper batching events enforces these limits at
        // event-creation time with its own copy of the config. Dropping here
        // keeps a misconfigured wrapper from growing the span unbounded.
        const auto& cfg = config_;
        if (depth >= cfg->span.max_event_depth || sequence >= cfg->span.max_event_sequence) {
            LOG_WARN_THROTTLED("recorded span event exceeds maximum depth/sequence. (depth:{}, seq:{})",
                               depth, sequence);
            return noopSpanEvent();
        }

        auto se = std::make_unique<SpanEventImpl>(this, operation);
        se->SetServiceType(service_type);
        // addSpanEvent reserves this span's own counters; a replayed event
        // carries its wrapper-assigned position instead, so overwrite them.
        // The depth reservation stays balanced — finish() decrements it.
        auto* event = data_->addSpanEvent(std::move(se));
        event->setSequence(sequence);
        event->setDepth(depth);
        event->setStartTime(start_time_ms);
        event->setEndTime(end_time_ms);
        if (async_id != NONE_ASYNC_ID) {
            event->setAsyncId(async_id);
        }
        return event;
    } CATCH_AND_LOG_RETURN("record span event", noopSpanEvent())

    void SpanImpl::SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) try {
        CHECK_FINISHED();
        // Gate at entry creation: with URL stats disabled (the default) the
        // entry would cost two heap string copies only to be dropped at
        // enqueue. It still doubles as recordException's url_template source
        // (see getUrlTemplate), so callstack tracing keeps it and
        // sendUrlStat() discards it without enqueueing. Snapshot-gated: spans
        // built without one (tests) keep the record-then-drop behavior.
        if (runtime_ && !config_->http.url_stat.enable && !config_->enable_callstack_trace) {
            return;
        }
        url_stat_.emplace(url_pattern, method, status_code);
    } CATCH_AND_LOG("set url stat")

    void SpanImpl::SetServiceType(int32_t service_type) {
        CHECK_FINISHED();
        data_->setServiceType(service_type);
    }

    void SpanImpl::SetStartTime(std::chrono::system_clock::time_point start_time) {
        CHECK_FINISHED();
        data_->setStartTime(start_time);
    }

    void SpanImpl::SetRemoteAddress(std::string_view address) try {
        CHECK_FINISHED();
        data_->setRemoteAddr(address);
    } CATCH_AND_LOG("set remote address")

    void SpanImpl::SetEndPoint(std::string_view endpoint) try {
        CHECK_FINISHED();
        data_->setEndPoint(endpoint);
    } CATCH_AND_LOG("set end point")

    void SpanImpl::SetAcceptorHost(std::string_view host) try {
        CHECK_FINISHED();
        data_->setAcceptorHost(host);
    } CATCH_AND_LOG("set acceptor host")

    void SpanImpl::SetError(std::string_view error_message) {
        SetError("Error", error_message);
    }

    void SpanImpl::SetError(std::string_view error_name, std::string_view error_message) try {
        CHECK_FINISHED();

        data_->setErrorFuncId(agent_->cacheError(error_name));
        data_->setErrorString(error_message);
        markSpanError();
    } CATCH_AND_LOG("set error")

    void SpanImpl::SetStatusCode(int status) {
        CHECK_FINISHED();

        data_->getAnnotations()->AppendInt(ANNOTATION_HTTP_STATUS_CODE, status);
        if (isStatusFail(status)) {
            markSpanError();
        }
    }

    bool SpanImpl::isStatusFail(const int status) const {
        // Evaluate against the span's own runtime generation: no atomic
        // runtime load, and the same status-error set for the span's whole
        // lifetime even when a config reload lands mid-span (matching how
        // config_ pins the span's limits).
        if (runtime_) {
            const auto& status_errors = runtime_->http_status_errors;
            return status_errors && status_errors->isErrorCode(status);
        }
        return agent_->isStatusFail(status);
    }

    void SpanImpl::RecordHeader(HeaderType which, HeaderReader& reader) try {
        CHECK_FINISHED();
        agent_->recordServerHeader(which, reader, data_->getAnnotations());
    } CATCH_AND_LOG("record header")

    std::string SpanImpl::GetTraceId() try {
        return data_->getTraceIdWire();
    } CATCH_AND_LOG_RETURN("get trace id", std::string{})

    void SpanImpl::sendUrlStat() {
        if (!url_stat_) {
            return;
        }
        // With URL stats disabled the entry only existed so sendExceptions()
        // could read the url template (see SetUrlStat) — discard it here.
        // Snapshot-gated like SetUrlStat: without one, legacy behavior.
        if (runtime_ && !config_->http.url_stat.enable) {
            url_stat_.reset();
            return;
        }
        url_stat_->end_time_ = data_->getEndTime();
        url_stat_->elapsed_ = data_->getElapsed();
        // A span-event exception (DB/external call) fails the transaction too,
        // matching Java's URI stat status = (errorCode == 0).
        url_stat_->failed_ = isStatusFail(url_stat_->status_code_) ||
                             data_->getErr() != SPAN_ERR_NONE;
        agent_->recordUrlStat(std::move(*url_stat_), *config_);
        url_stat_.reset();
    }

    void SpanImpl::sendExceptions() {
        if (!exceptions_.empty()) {
            agent_->recordException(data_->getTraceId(), data_->getSpanId(), getUrlTemplate(), takeExceptions());
        }
    }

    void SpanImpl::SetLogging(TraceContextWriter& writer) try {
        CHECK_FINISHED();

        data_->setLoggingFlag();

        // Stack to_chars buffer instead of std::to_string, matching
        // injectContext(): this runs per traced request.
        char span_id[32];
        const auto res = std::to_chars(span_id, span_id + sizeof(span_id), data_->getSpanId());

        writer.Set("PtxId", data_->getTraceIdWire());
        writer.Set("PspanId", std::string_view(span_id, static_cast<size_t>(res.ptr - span_id)));
    } CATCH_AND_LOG("set logging")

}  // namespace pinpoint
