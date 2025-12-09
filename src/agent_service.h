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

 #include "pinpoint/tracer.h"
 
 namespace pinpoint {
 
   struct Config;
   struct ApiMeta;
   struct StringMeta;
   struct SqlUidMeta;
   class SpanChunk;
   class SpanData;
   struct UrlStat;
   class AgentStats;
 
   /**
    * @brief Identifies the type of statistics pushed to the collector.
    */
   enum StatsType {AGENT_STATS, URL_STATS};
 
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
 
      /// @brief Returns the configured application name.
      virtual std::string_view getAppName() const = 0;
      /// @brief Returns the configured application type.
      virtual int32_t getAppType() const = 0;
      /// @brief Returns the unique agent identifier.
      virtual std::string_view getAgentId() const = 0;
      /// @brief Returns the human-readable agent name.
      virtual std::string_view getAgentName() const = 0;
      /// @brief Returns the resolved runtime configuration.
      virtual const Config& getConfig() const = 0;
      /// @brief Returns the agent's start timestamp (epoch milliseconds).
      virtual int64_t getStartTime() const = 0;
 
      /**
       * @brief Generates a new distributed trace identifier.
       *
       * @return Newly generated `TraceId`.
       */
      virtual TraceId generateTraceId() = 0;
      /**
       * @brief Sends a span chunk to the collector.
       *
       * @param span Span chunk ownership is transferred to the implementation.
       */
      virtual void recordSpan(std::unique_ptr<SpanChunk> span) const = 0;
      /**
       * @brief Sends an aggregated URL statistic to the collector.
       *
       * @param stat URL statistic record to be transferred.
       */
      virtual void recordUrlStat(std::unique_ptr<UrlStat> stat) const = 0;
      /**
       * @brief Reports an exception captured during span processing.
       *
       * @param span_data Span data that carries exception metadata.
       */
      virtual void recordException(SpanData* span_data) const = 0;
      /**
       * @brief Pushes agent- or URL-level statistics to the collector.
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
      /// @brief Removes a previously cached SQL string.
      virtual void removeCacheSql(const StringMeta& sql_meta) const = 0;
      /**
       * @brief Stores the normalized SQL UID and returns its cached byte sequence.
       *
       * @param sql Normalized SQL query to cache.
       * @return UID byte sequence identifying the query.
       */
      virtual std::vector<unsigned char> cacheSqlUid(std::string_view sql) const = 0;
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
      virtual void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const = 0;
      /**
       * @brief Records client-side headers into the supplied annotation.
       *
       * @param which Which header set to capture.
       * @param reader Header accessor provided by user code.
       * @param annotation Destination annotation aggregator.
       */
      virtual void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const = 0;

      /**
       * @brief Returns a reference to the AgentStats instance.
       *
       * @return Reference to AgentStats for direct stat collection.
       */
      virtual AgentStats& getAgentStats() = 0;
   };

 }  // namespace pinpoint
