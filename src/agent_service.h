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
      class SpanChunk;
      struct UrlStat;
      enum StatsType {AGENT_STATS, URL_STATS};

      class AgentService {
      public:
         virtual ~AgentService() = default;
 
         virtual bool isExiting() const = 0;
 
         virtual std::string_view getAppName() const = 0;
         virtual int32_t getAppType() const = 0;
         virtual std::string_view getAgentId() const = 0;
         virtual std::string_view getAgentName() const = 0;
         virtual const Config& getConfig() const = 0;
         virtual int64_t getStartTime() const = 0;
 
         virtual TraceId generateTraceId() = 0;
         virtual void recordSpan(std::unique_ptr<SpanChunk> span) const = 0;
         virtual void recordUrlStat(std::unique_ptr<UrlStat> stat) const = 0;
         virtual void recordStats(StatsType stats) const = 0;
 
         virtual int32_t cacheApi(std::string_view api_str, int32_t api_type) const = 0;
         virtual void removeCacheApi(const ApiMeta& api_meta) const = 0;
         virtual int32_t cacheError(std::string_view error_name) const = 0;
         virtual void removeCacheError(const StringMeta& str_meta) const = 0;
 
         virtual bool isStatusFail(int status) const = 0;
         virtual void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const = 0;
         virtual void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const = 0;
      };

 }  // namespace pinpoint
