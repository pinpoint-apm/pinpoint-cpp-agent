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

    static Noop& getNoop() {
        // Intentionally heap-allocated and never destroyed, mirroring the
        // Logger and global-agent singletons. noopSpanEvent()/
        // unsampledSpanEvent() hand out raw pointers owned by this instance, and
        // a host or detached thread may still trace during process-exit static
        // destruction. A function-local value static would be destroyed by then,
        // turning those late calls into use-after-destruction.
        static Noop* noop = new Noop();
        return *noop;
    }

    SpanEventPtr noopSpanEvent() {
        return getNoop().spanEvent();
    }

    namespace {
        // Per-thread localized owner of the one noop span. noopSpan() is
        // returned on every filtered / disabled-agent / failed-admission
        // request (see AgentImpl::NewSpan), and copying one process-wide
        // shared_ptr there puts an atomic increment plus a later decrement on a
        // single control block for every such request — the same cache line
        // ping-pong across cores that AtomicSharedPtr's ThreadCached mode
        // exists to remove. localize_shared() keeps the pointee identical (so
        // `.get()` and `==` against the singleton still hold) while giving each
        // thread its own control block to hammer.
        //
        // Split into a trivially-destructible slot plus a separate reclaim
        // guard, exactly like AtomicSharedPtr::ThreadCacheSlot and
        // HttpUrlFilter::MatchScratchSlot, and for the same reason: noopSpan()
        // must stay callable while the calling thread is already running its
        // thread_local destructors — a host thread_local that records a final
        // span reaches here then — and a block-scope thread_local *with* a
        // destructor must not be entered again once destroyed
        // ([basic.start.term]). The slot has no destructor, so it is never
        // "destroyed"; only the SpanPtr it points at is reclaimed, by the guard.
        struct NoopSpanSlot {
            SpanPtr* local = nullptr;
            // Set by the guard: from then on noopSpan() takes the leak path
            // instead of re-registering a guard (impossible once TLS
            // destructors have started).
            bool reclaimed = false;
        };

        NoopSpanSlot& noop_span_slot() noexcept {
            // Trivially destructible and constant-initialized: safe to touch at
            // any point of the thread's lifetime.
            static thread_local NoopSpanSlot slot;
            return slot;
        }

        struct NoopSpanReclaim {
            ~NoopSpanReclaim() {
                auto& slot = noop_span_slot();
                delete slot.local;
                slot.local = nullptr;
                slot.reclaimed = true;
            }
        };
    }

    SpanPtr noopSpan() {
        auto& slot = noop_span_slot();
        if (slot.local != nullptr) {
            // Steady state: one refcount bump on this thread's own control
            // block, and nothing on this path can throw.
            return *slot.local;
        }
        try {
            auto localized = localize_shared(getNoop().span());
            slot.local = new SpanPtr(std::move(localized));
            if (!slot.reclaimed) {
                static thread_local NoopSpanReclaim reclaim;
                (void)reclaim;
            }
            // Otherwise this is teardown re-entry: the guard already ran and
            // cannot be registered again, so the replacement is deliberately
            // leaked — bounded at one aliased owner per thread that traces
            // during its own exit, and it keeps the immortal noop span alive
            // either way.
            return *slot.local;
        } catch (...) {
            // Localizing is a pure optimization, so a failure to set it up
            // (realistically only bad_alloc) must not change what the caller
            // gets. Fall back to a copy of the shared holder: contended, but
            // correct, and identical to the behavior before this thread-local
            // owner existed — including that getNoop()'s own first-call
            // allocation is the one remaining path that can throw from here.
            return getNoop().span();
        }
    }

    SpanEventPtr unsampledSpanEvent() {
        return getNoop().unsampledSpanEvent();
    }

    AgentPtr noopAgent() {
        return getNoop().agent();
    }

    UnsampledSpan::UnsampledSpan(AgentService *agent,
                                 std::shared_ptr<const AgentRuntime> runtime) : NoopSpan(),
        span_id_(generate_span_id()),
        start_time_(to_milli_seconds(std::chrono::system_clock::now())),
        url_stat_(),
        runtime_(std::move(runtime)),
        agent_ref_(agent != nullptr ? agent->selfRef() : nullptr),
        agent_(agent) {
        // Guard the deref to stay consistent with the null check on agent_ref_
        // above (agent_ is always the live AgentImpl in production).
        if (agent_ != nullptr) {
            agent_->getAgentStats().addActiveSpan(span_id_, start_time_);
        }
    }

    UnsampledSpan::~UnsampledSpan() {
        // Self-heal spans dropped without EndSpan, mirroring ~SpanImpl: the
        // active-span registration taken in the constructor must not outlive
        // the span. agent_ stays valid via agent_ref_.
        if (!finished_.load() && agent_ != nullptr) {
            try {
                agent_->getAgentStats().dropActiveSpan(span_id_);
            } catch (...) {
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

        // Paired with the constructor's null guard: nothing to record without
        // an agent (agent_ is always valid in production).
        if (agent_ == nullptr) {
            return;
        }

        auto end_time_ = std::chrono::system_clock::now();
        // system_clock is not monotonic (NTP can step it backwards); clamp to
        // zero like SpanData::setEndTime so a negative duration never skews
        // the response-time stats.
        auto elapsed_ = static_cast<int32_t>(
            std::max<int64_t>(to_milli_seconds(end_time_) - start_time_, 0));

        auto& stats = agent_->getAgentStats();
        stats.collectResponseTime(elapsed_);
        stats.dropActiveSpan(span_id_);

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
            if (runtime_) {
                const auto& status_errors = runtime_->http_status_errors;
                url_stat_->failed_ =
                    status_errors && status_errors->isErrorCode(url_stat_->status_code_);
                agent_->recordUrlStat(std::move(*url_stat_), *runtime_->config);
            } else {
                url_stat_->failed_ = agent_->isStatusFail(url_stat_->status_code_);
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
        // Shared by EndSpan's catch handlers: finished_ is set before they
        // run, which disables the destructor's self-heal, so release the
        // active-span registration here instead (a duplicate erase is a
        // no-op). Same shape as SpanImpl::releaseActiveSpanOnError.
        try {
            if (agent_ != nullptr) {
                agent_->getAgentStats().dropActiveSpan(span_id_);
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
