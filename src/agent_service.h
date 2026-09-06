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

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "pinpoint/tracer.h"
// prepareSql returns PreparedSqlResult by value, so the cache types must be
// complete here.
#include "cache.h"

 namespace pinpoint {
 
   struct Config;
   struct ApiMeta;
   struct StringMeta;
   struct SqlUidMeta;
   class Exception;
   class PinpointAnnotation;
   class SpanChunk;
   struct UrlStatEntry;
   class AgentStats;
   class UrlStats;
 
   /// @brief Identifies the type of statistics pushed to the collector.
   enum StatsType {AGENT_STATS, URL_STATS};

   /**
    * @brief Internal distributed trace identifier: agent id, start time and sequence.
    *
    * Internal-only — not part of the public API. Application code observes a
    * trace id through Span::GetTraceId(), which returns the serialized wire
    * form (`agentId^startTime^sequence`) as a std::string.
    */
   struct TraceId {
      // Held by shared_ptr so the copies (SpanData storing it, NewAsyncSpan
      // cloning the parent's, ExceptionMeta queueing it) never re-copy the
      // agent-id bytes. May be null (default/unset).
      std::shared_ptr<const std::string> AgentId;
      /// Epoch time (milliseconds) when the agent started.
      int64_t StartTime = 0;
      /// Sequence number that disambiguates traces created at the same start time.
      int64_t Sequence = 0;

      TraceId() = default;
      TraceId(std::shared_ptr<const std::string> agent_id, int64_t start_time, int64_t sequence)
         : AgentId(std::move(agent_id)), StartTime(start_time), Sequence(sequence) {}
      // For call sites holding the id by value/view (tests, mocks,
      // parseTraceId): copies the bytes into a fresh shared string.
      TraceId(std::string_view agent_id, int64_t start_time, int64_t sequence)
         : AgentId(std::make_shared<const std::string>(agent_id)),
           StartTime(start_time), Sequence(sequence) {}

      /**
       * @brief Parses a wire-form trace id (`agentId^startTime^sequence`).
       *
       * @param txid Inbound HEADER_TRACE_ID value.
       * @return The parsed TraceId, or an empty one (empty()) when @p txid is
       *         malformed or an allocation fails. Never throws.
       */
      static TraceId parseTraceId(std::string_view txid) noexcept;

      /// @brief Agent-id bytes, or an empty view when unset.
      std::string_view agentId() const noexcept {
         return AgentId ? std::string_view(*AgentId) : std::string_view{};
      }

      /// @brief True when this is an empty/invalid trace id (no agent id set).
      ///        parseTraceId() returns such a value on failure, which
      ///        readInboundTrace() reads as "not a continued trace" — NewSpan
      ///        then starts a new one rather than recording an unattached span.
      bool empty() const noexcept { return AgentId == nullptr; }

      /// @brief Serializes to the wire format (`agentId^startTime^sequence`).
      std::string toString() const {
         // to_chars + one reserved string, not an ostringstream: this runs on
         // every outbound call of a traced request, and ostringstream pays for
         // locale, a virtual streambuf and multiple allocations.
         const std::string_view agent = agentId();
         char num[20];  // widest int64_t is 20 chars incl. sign
         std::string out;
         out.reserve(agent.size() + 2 + 2 * sizeof(num));
         out.append(agent);
         out.push_back('^');
         auto st = std::to_chars(num, num + sizeof(num), StartTime);
         out.append(num, st.ptr);
         out.push_back('^');
         auto sq = std::to_chars(num, num + sizeof(num), Sequence);
         out.append(num, sq.ptr);
         return out;
      }
   };

   /**
    * @brief Abstract service boundary used by collectors and workers to report
    *        data, exposing the hooks the agent subsystems (gRPC clients, URL
    *        statistics, samplers) need without leaking the concrete agent.
    */
   class AgentService {
   public:
      virtual ~AgentService() = default;

      /// @brief True once the shutdown sequence is in progress.
      virtual bool isExiting() const = 0;

      /**
       * @brief Shared handle that keeps the agent alive.
       *
       * Production spans capture a non-null handle so user code holding a span
       * past release of the last external agent reference keeps the agent
       * alive. Null when the instance is not shared_ptr-owned (test fixtures).
       */
      virtual std::shared_ptr<AgentService> selfRef() noexcept { return nullptr; }

      // These identity fields are immutable for the agent's lifetime:
      // Config::retainNonReloadableFrom() overwrites any reload that tries to
      // change them with the running values, so returning a const reference is
      // safe and lets per-request hot paths avoid a string copy.
      // Implementations must back them with storage outliving the agent.
      virtual const std::string& getAppName() const = 0;
      virtual int32_t getAppType() const = 0;
      /// @brief Agent identifier used as the collector instance key.
      virtual const std::string& getAgentId() const = 0;
      /// @brief Only populated for uid version v4; empty for v1/v3 (mirrors
      ///        Java ObjectName.getServiceName).
      virtual const std::string& getServiceName() const = 0;
      virtual std::shared_ptr<const Config> getConfig() const = 0;
      /// @brief Agent start timestamp (epoch milliseconds).
      virtual int64_t getStartTime() const = 0;
      /// @brief Reloads config-dependent helpers (samplers, filters, recorders).
      virtual void reloadConfig(std::shared_ptr<const Config> cfg) = 0;

      virtual TraceId generateTraceId() = 0;
      /// @brief Queues a span chunk for asynchronous collector delivery.
      virtual void recordSpan(std::unique_ptr<SpanChunk> span) const = 0;
      /// @brief Queues a per-request URL statistic for aggregation.
      virtual void recordUrlStat(UrlStatEntry stat) const = 0;
      /**
       * @brief Queues a URL statistic using the caller's config snapshot.
       *
       * For hot-path callers (spans) that already hold a snapshot: skips the
       * atomic config load the single-argument overload pays per record.
       * `config` need only outlive the call. The default implementation lives
       * in agent.cpp (an inline body would need UrlStatEntry complete) and
       * forwards to the single-argument overload, so mocks keep working.
       */
      virtual void recordUrlStat(UrlStatEntry stat, const Config& config) const;
      /// @brief Queues exceptions captured during span processing.
      virtual void recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                                   std::vector<std::unique_ptr<Exception>>&& exceptions) const = 0;
      /// @brief Queues agent- or URL-level statistics for delivery.
      virtual void recordStats(StatsType stats) const = 0;

      /// @brief Stores an API string and returns its cached numeric identifier.
      virtual int32_t cacheApi(std::string_view api_str, int32_t api_type) const = 0;
      virtual void removeCacheApi(const ApiMeta& api_meta) const = 0;
      /// @brief Stores an error string and returns its cached identifier.
      virtual int32_t cacheError(std::string_view error_name) const = 0;
      virtual void removeCacheError(const StringMeta& error_meta) const = 0;
      /// @brief Stores an SQL string and returns its cached identifier.
      virtual int32_t cacheSql(std::string_view sql_query) const = 0;
      /**
       * @brief Resolves a raw SQL statement to its normalized form, extracted
       *        literal parameters, and collector identity.
       *
       * Implementations may cache by the raw statement; the returned shared
       * ownership keeps cached strings alive for asynchronous serialization.
       * The identity is resolved per call and is not part of what is cached,
       * so it stays correct after a metadata send fails (see PreparedSql).
       */
      virtual std::optional<PreparedSqlResult> prepareSql(
          std::string_view raw_sql, SqlMetaMode mode) const = 0;
      virtual void removeCacheSql(const StringMeta& sql_meta) const = 0;
      /// @brief Caches the normalized SQL UID. Returns the 16-byte UID, or
      ///        nullopt when the agent is disabled or generation fails.
      virtual std::optional<SqlUid> cacheSqlUid(std::string_view sql) const = 0;
      virtual void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const = 0;

      /// @brief Whether an HTTP status should be treated as a failure.
      virtual bool isStatusFail(int status) const = 0;
      /// @brief Records server-side headers into the supplied annotation.
      virtual void recordServerHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const = 0;
      /// @brief Records client-side headers into the supplied annotation.
      virtual void recordClientHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const = 0;

      virtual AgentStats& getAgentStats() = 0;
      virtual UrlStats& getUrlStats() = 0;
   };

   /// @brief Builds the resolved SpanConfigSnapshot (revision included) from
   ///        one agent identity + config generation. Shared by
   ///        SpanImpl::GetConfigSnapshot (the generation the span captured)
   ///        and AgentImpl::GetConfigSnapshot (the current generation).
   ///        Defined in span.cpp.
   SpanConfigSnapshot make_config_snapshot(const AgentService& agent, const Config& config);

 }  // namespace pinpoint
