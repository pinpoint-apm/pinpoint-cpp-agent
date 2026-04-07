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

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../src/agent_service.h"
#include "../src/config.h"
#include "../src/span.h"
#include "../src/stat.h"
#include "../src/url_stat.h"

namespace pinpoint {

class MockAgentService : public AgentService {
public:
    MockAgentService()
        : is_exiting_(false),
          start_time_(1234567890),
          cached_start_time_str_(std::to_string(start_time_)),
          trace_id_counter_(0) {}

    // AgentService interface implementation
    bool isExiting() const override { return is_exiting_; }
    std::string getAppName() const override { return app_name_; }
    int32_t getAppType() const override { return app_type_; }
    std::string getAgentId() const override { return agent_id_; }
    std::string getAgentName() const override { return agent_name_; }
    std::shared_ptr<const Config> getConfig() const override { return config_; }
    int64_t getStartTime() const override { return start_time_; }
    void reloadConfig(std::shared_ptr<const Config> cfg) override {
        if (cfg) {
            *config_ = *cfg;
        }
    }

    TraceId generateTraceId() override {
        return TraceId{agent_id_, start_time_, trace_id_counter_++};
    }

    void recordSpan(std::unique_ptr<SpanChunk> span) const override {
        recorded_spans_.push_back(std::move(span));
    }

    void recordUrlStat(std::unique_ptr<UrlStatEntry> stat) const override {
        recorded_url_stats_++;
        if (stat) {
            last_url_stat_url_ = stat->url_pattern_;
            last_url_stat_method_ = stat->method_;
            last_url_stat_status_code_ = stat->status_code_;
        }
    }

    void recordException(SpanData* span_data) const override {
        recorded_exceptions_++;
    }

    void recordStats(StatsType stats) const override {
        recorded_stats_calls_++;
        last_stats_type_ = stats;
    }

    int32_t cacheApi(std::string_view api_str, int32_t api_type) const override {
        auto key = std::string(api_str);
        if (cached_apis_.find(key) == cached_apis_.end()) {
            cached_apis_[key] = api_id_counter_++;
        }
        return cached_apis_[key];
    }

    void removeCacheApi(const ApiMeta& api_meta) const override {}

    int32_t cacheError(std::string_view error_name) const override {
        auto key = std::string(error_name);
        if (cached_errors_.find(key) == cached_errors_.end()) {
            cached_errors_[key] = error_id_counter_++;
        }
        return cached_errors_[key];
    }

    void removeCacheError(const StringMeta& error_meta) const override {}

    int32_t cacheSql(std::string_view sql_query) const override {
        auto key = std::string(sql_query);
        if (cached_sqls_.find(key) == cached_sqls_.end()) {
            cached_sqls_[key] = sql_id_counter_++;
        }
        return cached_sqls_[key];
    }

    void removeCacheSql(const StringMeta& sql_meta) const override {}

    std::vector<unsigned char> cacheSqlUid(std::string_view sql) const override {
        return {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    }

    void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override {}

    bool isStatusFail(int status) const override {
        return status >= 400;
    }

    void recordServerHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        recorded_server_headers_++;
    }

    void recordClientHeader(HeaderType which, HeaderReader& reader, const AnnotationPtr& annotation) const override {
        recorded_client_headers_++;
    }

    AgentStats& getAgentStats() override {
        if (!agent_stats_) {
            agent_stats_ = std::make_unique<AgentStats>(this);
        }
        return *agent_stats_;
    }

    UrlStats& getUrlStats() override {
        if (!url_stats_) {
            url_stats_ = std::make_unique<UrlStats>(this);
        }
        return *url_stats_;
    }

    // Test helpers - setters
    void setExiting(bool exiting) { is_exiting_ = exiting; }
    void setAppName(const std::string& name) { app_name_ = name; }
    void setAppType(int32_t type) { app_type_ = type; }
    void setAgentId(const std::string& id) { agent_id_ = id; }
    void setAgentName(const std::string& name) { agent_name_ = name; }
    void setStartTime(int64_t time) {
        start_time_ = time;
        cached_start_time_str_ = std::to_string(time);
    }

    // Direct config access for test customization
    std::shared_ptr<Config>& mutableConfig() { return config_; }

    // Test helpers - accessors
    int32_t getCachedApiId(const std::string& api_str) const {
        auto it = cached_apis_.find(api_str);
        return it != cached_apis_.end() ? it->second : -1;
    }
    int32_t getCachedErrorId(const std::string& error_name) const {
        auto it = cached_errors_.find(error_name);
        return it != cached_errors_.end() ? it->second : -1;
    }
    int32_t getCachedSqlId(const std::string& sql_query) const {
        auto it = cached_sqls_.find(sql_query);
        return it != cached_sqls_.end() ? it->second : -1;
    }
    int32_t getSqlIdCounter() const { return sql_id_counter_; }
    size_t getRecordedSpansCount() const { return recorded_spans_.size(); }
    const SpanChunk* getLastRecordedSpan() const {
        return recorded_spans_.empty() ? nullptr : recorded_spans_.back().get();
    }

    // Test-observable state
    mutable std::vector<std::unique_ptr<SpanChunk>> recorded_spans_;
    mutable int recorded_url_stats_ = 0;
    mutable int recorded_exceptions_ = 0;
    mutable int recorded_stats_calls_ = 0;
    mutable StatsType last_stats_type_ = AGENT_STATS;
    mutable int recorded_server_headers_ = 0;
    mutable int recorded_client_headers_ = 0;
    mutable std::string last_url_stat_url_;
    mutable std::string last_url_stat_method_;
    mutable int last_url_stat_status_code_ = 0;
    mutable std::map<std::string, int32_t> cached_apis_;
    mutable std::map<std::string, int32_t> cached_errors_;
    mutable std::map<std::string, int32_t> cached_sqls_;
    mutable int32_t api_id_counter_ = 100;
    mutable int32_t error_id_counter_ = 200;
    mutable int32_t sql_id_counter_ = 300;

private:
    bool is_exiting_;
    int64_t start_time_;
    std::string cached_start_time_str_;
    int64_t trace_id_counter_;
    std::string app_name_ = "TestApp";
    int32_t app_type_ = 1300;
    std::string agent_id_ = "test-agent-001";
    std::string agent_name_ = "TestAgent";
    std::shared_ptr<Config> config_ = std::make_shared<Config>();
    mutable std::unique_ptr<AgentStats> agent_stats_;
    mutable std::unique_ptr<UrlStats> url_stats_;
};

}  // namespace pinpoint
