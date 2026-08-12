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

#pragma once

#include <array>
#include <memory>

namespace pinpoint {

    struct Config;
    class TraceSampler;
    class HttpUrlFilter;
    class HttpMethodFilter;
    class HttpStatusErrors;
    class HttpHeaderRecorder;
    class AgentStats;
    class UrlStats;

    /**
     * @brief Immutable bundle of the resolved config and every component derived from it.
     *
     * Covers the sampler, HTTP filters, header recorders and the agent's
     * stats sinks. Published behind a single AtomicSharedPtr so per-request
     * readers pay one atomic load for a mutually consistent set, and a reload
     * swap can never be observed half-applied.
     *
     * Lives in its own header (only forward declarations needed for the
     * shared_ptr members) so spans can hold the snapshot NewSpan admitted them
     * under — see SpanImpl::runtime_ / UnsampledSpan::runtime_ — without
     * span.h having to include agent.h, which includes span.h back.
     */
    struct AgentRuntime {
        std::shared_ptr<const Config> config;
        std::shared_ptr<TraceSampler> sampler;
        std::shared_ptr<HttpUrlFilter> http_url_filter;
        std::shared_ptr<HttpMethodFilter> http_method_filter;
        std::shared_ptr<HttpStatusErrors> http_status_errors;
        std::array<std::shared_ptr<HttpHeaderRecorder>, 3> http_srv_header_recorder;
        std::array<std::shared_ptr<HttpHeaderRecorder>, 3> http_cli_header_recorder;
        // Not config-derived: the agent's stats sinks, carried unchanged into
        // every generation (build_runtime), so spans admitted under different
        // generations aggregate into the same place. They are here so a span
        // holding this snapshot can reach them WITHOUT keeping the whole
        // agent alive: UnsampledSpan drops its per-request selfRef() — a CAS
        // on the agent's single control block, measured at half the
        // four-thread cost of the unsampled path (span_lifecycle_benchmark) —
        // because everything its EndSpan and destructor touch is owned right
        // here. Deliberately NOT handed to spans as shared_ptr copies: a
        // per-span copy would do its refcount RMW on these objects' one
        // control block and recreate the same contention one object over.
        // Spans ride the runtime snapshot's thread-localized control block
        // instead (AtomicSharedPtr's ThreadCached mode) and use these as
        // plain pointers.
        std::shared_ptr<AgentStats> stats;
        std::shared_ptr<UrlStats> url_stats;
    };

}  // namespace pinpoint
