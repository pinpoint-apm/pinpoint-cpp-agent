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

#include "atomic_shared_ptr.h"  // pinpoint::localize_shared
#include "http.h"
#include "logging.h"
#include "stat.h"
#include "utility.h"
#include "noop.h"

namespace pinpoint {

    // The noop singletons below are intentionally heap-allocated and never
    // destroyed, mirroring the Logger and global-agent singletons.
    // noopSpanEvent() hands out raw pointers, and a host or detached thread
    // may still trace during process-exit static destruction. A function-local value static would be destroyed by then,
    // turning those late calls into use-after-destruction.

    SpanEventPtr noopSpanEvent() {
        static auto* event = new NoopSpanEvent();
        return event;
    }

    static SpanPtr& noopSpanHolder() {
        static auto* span = new SpanPtr(std::make_shared<NoopSpan>());
        return *span;
    }

    SpanPtr noopSpan() {
        // Per-thread localized owner of the one noop span. noopSpan() is
        // returned on every filtered / disabled-agent / failed-admission
        // request, and copying one process-wide shared_ptr there would put an
        // increment and decrement on a single control block per request — the
        // cache-line ping-pong ThreadCached exists to remove. localize_shared()
        // keeps the pointee identical (so `.get()` and `==` against the
        // singleton still hold) while giving each thread its own control block.
        // thread_local_lazy keeps this callable during the thread's own TLS
        // teardown; a re-entry there leaks one aliased owner of an immortal
        // object.
        try {
            return thread_local_lazy<SpanPtr>(
                [] { return new SpanPtr(localize_shared(noopSpanHolder())); });
        } catch (...) {
            // Localizing is a pure optimization, so a failure to set it up
            // (realistically only bad_alloc) must not change what the caller
            // gets. Fall back to a copy of the shared holder: contended, but
            // correct, and identical to the behavior before this thread-local
            // owner existed — including that the holder's own first-call
            // allocation is the one remaining path that can throw from here.
            return noopSpanHolder();
        }
    }

    AgentPtr noopAgent() {
        static auto* agent = new AgentPtr(std::make_shared<NoopAgent>());
        return *agent;
    }

    UnsampledSpan::UnsampledSpan(AgentService *agent,
                                 std::shared_ptr<const AgentRuntime> runtime) : NoopSpan(),
        span_id_(generate_span_id()),
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        runtime_(std::move(runtime)),
        // With a runtime snapshot carrying the stats sinks (every
        // build_runtime() snapshot does), this span never touches the agent
        // after construction: AgentStats, the url-stat sink and the
        // status-error set all belong to runtime_, which the span holds through
        // a thread-localized control block. Skipping selfRef() removes a
        // per-request CAS on the agent's single shared control block —
        // unsampled spans are the majority path when sampling is on, and that
        // CAS measured as half this path's four-thread cost
        // (span_lifecycle_benchmark). Without the sinks (tests, hand-built
        // runtimes) the legacy keep-alive covers the agent_ fallbacks below.
        agent_ref_(runtime_ && runtime_->stats && runtime_->url_stats
                       ? nullptr
                       : (agent != nullptr ? agent->selfRef() : nullptr)),
        agent_(agent) {
        // Null only when constructed without an agent AND without a runtime
        // (some unit tests) — then there is simply nothing to record into.
        if (auto* stats = statsSink()) {
            stats->addActiveSpan(active_node_, span_id_, start_time_);
        }
    }

    void UnsampledSpanEvent::SetError(std::string_view error_name, std::string_view error_message) {
        // Mirrors SpanEventImpl::SetError routing to SpanImpl::markSpanError:
        // an exception on a step fails the transaction. Nothing else of the
        // error is kept — an unsampled span has nowhere to keep it.
        owner_->markError(error_name, error_message);
    }

    void UnsampledSpan::markError(std::string_view error_name, std::string_view error_message) try {
        // Span.IgnoreErrors must apply here too: an error the operator has
        // excluded would otherwise fail the URL stat of unsampled requests
        // while sparing sampled ones. Without a runtime snapshot (tests) there
        // is no config to filter against, so the error stands.
        if (runtime_ &&
            is_ignored_error(runtime_->config->span.ignore_errors, error_name, error_message)) {
            return;
        }
        err_.store(true, std::memory_order_relaxed);
    } CATCH_AND_LOG("set error")

    AgentStats* UnsampledSpan::statsSink() const {
        if (runtime_ && runtime_->stats) {
            return runtime_->stats.get();
        }
        // Fallback deref is safe: whenever it is reachable the constructor
        // took agent_ref_ (or the caller owns a test agent outliving the span).
        return agent_ != nullptr ? &agent_->getAgentStats() : nullptr;
    }

    UnsampledSpan::~UnsampledSpan() {
        // Self-heals spans dropped without EndSpan, mirroring ~SpanImpl, and is
        // the hard backstop for the intrusive node: a still-linked node here
        // would leave dangling pointers in its shard list. Checking the node
        // rather than finished_ still covers an EndSpan failure before its
        // drop, while a normally ended span returns after one atomic load
        // without resolving the sink, so a non-owning test agent_ is never
        // touched. The registry outlives this unlink either way: runtime_ owns
        // the AgentStats, or the fallback's agent_ref_ keeps the agent alive.
        if (active_node_.isLinked()) {
            if (auto* stats = statsSink()) {
                try {
                    stats->dropActiveSpan(active_node_);
                } catch (...) {
                }
            }
        }
    }

    void UnsampledSpan::EndSpan() try {
        // Atomic exchange so only the first caller proceeds: a check-then-set
        // would let two concurrent EndSpan calls both pass the guard and run
        // dropActiveSpan / collectResponseTime / recordUrlStat twice.
        if (finished_.exchange(true)) {
            LOG_WARN("span is already finished");
            return;
        }

        // Paired with the constructor: no sink means nothing was recorded.
        auto* stats = statsSink();
        if (stats == nullptr) {
            return;
        }

        auto end_time_ = std::chrono::system_clock::now();
        // system_clock is not monotonic (NTP can step it backwards); clamp to
        // zero like SpanData::setEndTime so a negative duration never skews
        // the response-time stats.
        auto elapsed_ = static_cast<int32_t>(
            std::max<int64_t>(to_milli_seconds(end_time_) - start_time_, 0));

        stats->collectResponseTime(elapsed_);
        stats->dropActiveSpan(active_node_);

        // url_stat_mutex_ pairs with SetUrlStat(): its finished_ check runs
        // under the same lock, so a SetUrlStat racing this consume (documented
        // misuse) either lands before it or degrades to the warn/no-op below —
        // never a concurrent write to the moved-from entry.
        std::lock_guard<std::mutex> lock(url_stat_mutex_);
        if (url_stat_) {
            url_stat_->end_time_ = end_time_;
            url_stat_->elapsed_ = elapsed_;
            // Mirror SpanImpl::sendUrlStat: unsampled requests are the
            // majority when sampling is on, so missing this here would skew
            // the URL-stat failure rate toward zero. With a runtime snapshot
            // both the status check and the record skip the atomic runtime
            // loads agent_->isStatusFail()/getConfig() would pay.
            //
            // A recorded error fails the entry as well, for the same reason
            // and the same way SpanImpl::sendUrlStat ORs in PSpan.err.
            const bool err = err_.load(std::memory_order_relaxed);
            if (runtime_) {
                const auto& status_errors = runtime_->http_status_errors;
                url_stat_->failed_ = err ||
                    (status_errors && status_errors->isErrorCode(url_stat_->status_code_));
                if (runtime_->url_stats) {
                    // Straight into the runtime-owned sink, no agent deref:
                    // this span may hold no agent keep-alive (see the ctor).
                    // The agent's enabled_ gate that recordUrlStat used to
                    // apply is preserved by the sink's own accepting_ flag,
                    // flipped when shutdown begins.
                    runtime_->url_stats->enqueueUrlStats(std::move(*url_stat_),
                                                         *runtime_->config);
                } else if (agent_ != nullptr) {
                    // Runtime without a url-stat sink (hand-built): the ctor
                    // took agent_ref_ for exactly this fallback.
                    agent_->recordUrlStat(std::move(*url_stat_), *runtime_->config);
                }
            } else if (agent_ != nullptr) {
                url_stat_->failed_ = err || agent_->isStatusFail(url_stat_->status_code_);
                agent_->recordUrlStat(std::move(*url_stat_));
            }
            url_stat_.reset();
        }
    } catch (const std::exception& e) {
        // Mirror SpanImpl::EndSpan: EndSpan runs in host destructors, so the
        // exception must not escape — this is the majority path when
        // sampling is on (every unsampled request ends here).
        LOG_ERROR("end span exception = {}", e.what());
        releaseActiveSpanOnError();
    } catch (...) {
        LOG_ERROR("end span unknown exception");
        releaseActiveSpanOnError();
    }

    void UnsampledSpan::releaseActiveSpanOnError() noexcept {
        // Shared by EndSpan's catch handlers. The destructor would unlink
        // the node anyway, but user code may hold the span handle long after
        // a failed EndSpan — unlink now so the span stops counting as an
        // active request immediately. Idempotent (unlinked node → no-op).
        // Same shape as SpanImpl::releaseActiveSpanOnError.
        try {
            if (auto* stats = statsSink()) {
                stats->dropActiveSpan(active_node_);
            }
        } catch (...) {
        }
    }

    void UnsampledSpan::SetUrlStat(std::string_view url_pattern, std::string_view method, int status_code) try {
        // Gate at entry creation (see SpanImpl::SetUrlStat): with URL stats
        // disabled the entry's two heap string copies were built only to be
        // dropped in enqueueUrlStats(). Unlike SpanImpl there is no exception
        // url_template to preserve. Snapshot-gated: without one (tests),
        // legacy record-then-drop behavior.
        if (runtime_ && !runtime_->config->http.url_stat.enable) {
            return;
        }
        // Same guard as SpanImpl::SetUrlStat — after EndSpan the entry would
        // never be sent (EndSpan already consumed url_stat_) — but taken
        // under url_stat_mutex_ so the check-then-emplace cannot interleave
        // with EndSpan's exchange-then-consume.
        std::lock_guard<std::mutex> lock(url_stat_mutex_);
        if (finished_) {
            LOG_WARN("span is already finished");
            return;
        }
        url_stat_.emplace(url_pattern, method, status_code);
    } CATCH_AND_LOG("set url stat")

    void UnsampledSpanEvent::InjectContext(TraceContextWriter& writer) try {
        writer.Set(HEADER_SAMPLED, "s0");
    } CATCH_AND_LOG("inject context")
}
