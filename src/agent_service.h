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

 namespace pinpoint {
 
   struct Config;
   struct ApiMeta;
   struct StringMeta;
   struct SqlUidMeta;
   struct PreparedSql;
   enum class SqlMetaMode : uint8_t;
   class Exception;
   class PinpointAnnotation;
   class SpanChunk;
   struct UrlStatEntry;
   class AgentStats;
   class UrlStats;
 
   /**
    * @brief Identifies the type of statistics pushed to the collector.
    */
   enum StatsType {AGENT_STATS, URL_STATS};

   /**
    * @brief Internal distributed trace identifier: agent id, start time and sequence.
    *
    * Internal-only — not part of the public API. Application code observes a
    * trace id through Span::GetTraceId(), which returns the serialized wire
    * form (`agentId^startTime^sequence`) as a std::string.
    */
   struct TraceId {
      // Held by shared_ptr so copying a TraceId — which happens when SpanData
      // stores it, when NewAsyncSpan clones the parent's, and when it is queued
      // in an ExceptionMeta — never re-copies the agent-id bytes.
      // generateTraceId hands out the agent's id; parseTraceId owns the id it
      // decodes from an inbound header. May be null (default/unset).
      std::shared_ptr<const std::string> AgentId;
      /// Epoch time (milliseconds) when the agent started.
      int64_t StartTime = 0;
      /// Sequence number that disambiguates traces created at the same start time.
      int64_t Sequence = 0;

      TraceId() = default;
      TraceId(std::shared_ptr<const std::string> agent_id, int64_t start_time, int64_t sequence)
         : AgentId(std::move(agent_id)), StartTime(start_time), Sequence(sequence) {}
      // Convenience for call sites that hold the id by value/view (tests, mocks,
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
      ///        parseTraceId() returns such a value on failure, and NewSpan
      ///        turns it into a noop span instead of recording it.
      bool empty() const noexcept { return AgentId == nullptr; }

      /**
       * @brief Serializes the trace identifier to the wire format (`agentId^startTime^sequence`).
       */
      std::string toString() const {
         // Built with to_chars + a single reserved string instead of an
         // ostringstream: this runs on every InjectContext()/SetLogging()
         // (i.e. every outbound call on a traced request), and ostringstream
         // pays for locale, a virtual streambuf and multiple allocations.
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
    * @brief Abstract service boundary used by collectors and workers to report data.
    *
    * `AgentService` exposes the minimal set of hooks needed by the agent subsystems
    * (gRPC clients, URL statistics, samplers, etc.) without leaking the concrete agent
    * implementation.
    */
   class AgentService {
   public:
      /// Virtual destructor for interface.
      virtual ~AgentService() = default;
 
      /**
       * @brief Indicates whether the agent is terminating and rejecting new work.
       *
       * @return `true` when the shutdown sequence is in progress.
       */
      virtual bool isExiting() const = 0;

      /**
       * @brief Returns a shared handle that keeps the agent alive.
       *
       * Production spans capture a non-null handle so user code holding a span
       * (or C span handle) past release of the last external agent reference
       * keeps the agent alive.
       *
       * @return Shared pointer to this service, or nullptr when the instance
       *         is not owned by a shared_ptr (e.g. test fixtures).
       */
      virtual std::shared_ptr<AgentService> selfRef() noexcept { return nullptr; }
 
      // These identity fields are immutable for the agent's lifetime:
      // Config::retainNonReloadableFrom() overwrites any reload that tries to
      // change them with the running values, so a
      // const-reference return is safe and lets per-request hot-path callers
      // (e.g. SpanEvent::InjectContext) avoid a string copy. Implementations must
      // back them with storage that outlives the agent, not a temporary.
      /// @brief Returns the configured application name.
      virtual const std::string& getAppName() const = 0;
      /// @brief Returns the configured application type.
      virtual int32_t getAppType() const = 0;
      /// @brief Returns the resolved agent identifier used as the collector
      ///        instance key.
      virtual const std::string& getAgentId() const = 0;
      /// @brief Returns the agent's own service name. Only populated for uid
      ///        version v4; empty for v1/v3 (mirrors Java ObjectName.getServiceName).
      virtual const std::string& getServiceName() const = 0;
      /// @brief Returns the resolved runtime configuration.
      virtual std::shared_ptr<const Config> getConfig() const = 0;
      /// @brief Returns the agent's start timestamp (epoch milliseconds).
      virtual int64_t getStartTime() const = 0;
      /// @brief Reloads configuration-dependent helpers (samplers, filters, recorders).
      virtual void reloadConfig(std::shared_ptr<const Config> cfg) = 0;

      /**
       * @brief Generates a new distributed trace identifier.
       *
       * @return Newly generated `TraceId`.
       */
      virtual TraceId generateTraceId() = 0;
      /**
       * @brief Queues a span chunk for asynchronous collector delivery.
       *
       * @param span Span chunk ownership is transferred to the implementation.
       */
      virtual void recordSpan(std::unique_ptr<SpanChunk> span) const = 0;
      /**
       * @brief Queues a per-request URL statistic for aggregation.
       *
       * @param stat URL statistic record to be transferred.
       */
      virtual void recordUrlStat(UrlStatEntry stat) const = 0;
      /**
       * @brief Queues a URL statistic using the caller's config snapshot.
       *
       * Overload for hot-path callers (spans) that already hold a config
       * snapshot: skips the atomic config load the single-argument overload
       * pays per record. `config` only has to stay alive for the duration of
       * the call. The default implementation (defined in agent.cpp — the
       * inline body would need UrlStatEntry complete) forwards to the
       * single-argument overload so mocks and test doubles keep working
       * unchanged.
       *
       * @param stat URL statistic record to be transferred.
       * @param config The caller's config snapshot.
       */
      virtual void recordUrlStat(UrlStatEntry stat, const Config& config) const;
      /**
       * @brief Queues exceptions captured during span processing for delivery.
       */
      virtual void recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                                   std::vector<std::unique_ptr<Exception>>&& exceptions) const = 0;
      /**
       * @brief Queues agent- or URL-level statistics for collector delivery.
       *
       * @param stats Statistic type selector.
       */
      virtual void recordStats(StatsType stats) const = 0;
 
      /**
       * @brief Stores an API string and returns its cached numeric identifier.
       *
       * @param api_str API signature.
       * @param api_type API type classification.
       * @return Numeric identifier for the API string.
       */
      virtual int32_t cacheApi(std::string_view api_str, int32_t api_type) const = 0;
      /// @brief Removes a previously cached API entry.
      virtual void removeCacheApi(const ApiMeta& api_meta) const = 0;
      /**
       * @brief Stores an error string and returns its cached numeric identifier.
       *
       * @param error_name Error description.
       * @return Numeric identifier for the error string.
       */
      virtual int32_t cacheError(std::string_view error_name) const = 0;
      /// @brief Removes a previously cached error string.
      virtual void removeCacheError(const StringMeta& error_meta) const = 0;
      /**
       * @brief Stores an SQL string and returns its cached numeric identifier.
       *
       * @param sql_query SQL statement to cache.
       * @return Numeric identifier for the SQL string.
       */
      virtual int32_t cacheSql(std::string_view sql_query) const = 0;
      /**
       * Resolves a raw SQL statement to its normalized form, extracted literal
       * parameters, and collector identity. Implementations may cache by the
       * raw statement; returned shared ownership keeps cached strings alive for
       * asynchronous span serialization.
       */
      virtual std::optional<std::shared_ptr<const PreparedSql>> prepareSql(
          std::string_view raw_sql, SqlMetaMode mode) const = 0;
      /// @brief Removes a previously cached SQL string.
      virtual void removeCacheSql(const StringMeta& sql_meta) const = 0;
      /**
       * @brief Stores the normalized SQL UID and returns its cached byte sequence.
       *
       * @param sql Normalized SQL query to cache.
       * @return 16-byte UID identifying the query, or std::nullopt when the agent
       *         is disabled or UID generation fails.
       */
      virtual std::optional<SqlUid> cacheSqlUid(std::string_view sql) const = 0;
      /// @brief Removes a previously cached SQL UID entry.
      virtual void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const = 0;
 
      /**
       * @brief Determines whether a HTTP status is considered a failure.
       *
       * @param status HTTP status code.
       * @return `true` if the status should be treated as failure.
       */
      virtual bool isStatusFail(int status) const = 0;
      /**
       * @brief Records server-side headers into the supplied annotation.
       *
       * @param which Which header set to capture.
       * @param reader Header accessor provided by user code.
       * @param annotation Destination annotation aggregator.
       */
      virtual void recordServerHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const = 0;
      /**
       * @brief Records client-side headers into the supplied annotation.
       *
       * @param which Which header set to capture.
       * @param reader Header accessor provided by user code.
       * @param annotation Destination annotation aggregator.
       */
      virtual void recordClientHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const = 0;

      /**
       * @brief Returns a reference to the AgentStats instance.
       *
       * @return Reference to AgentStats for direct stat collection.
       */
      virtual AgentStats& getAgentStats() = 0;

      /**
       * @brief Returns a reference to the UrlStats instance.
       *
       * @return Reference to UrlStats for URL stat management.
       */
      virtual UrlStats& getUrlStats() = 0;
   };

 }  // namespace pinpoint
