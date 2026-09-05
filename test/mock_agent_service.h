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

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/agent_service.h"
#include "../src/cache.h"
#include "../src/config.h"
#include "../src/http.h"
#include "../src/span.h"
#include "../src/sql.h"
#include "../src/stat.h"
#include "../src/url_stat.h"

namespace pinpoint {

class MockAgentService : public AgentService {
public:
    MockAgentService()
        : is_exiting_(false),
          start_time_(1234567890),
          trace_id_counter_(0) {}

    // AgentService interface implementation
    bool isExiting() const override { return is_exiting_.load(std::memory_order_relaxed); }
    const std::string& getAppName() const override { return app_name_; }
    int32_t getAppType() const override { return app_type_; }
    const std::string& getAgentId() const override { return agent_id_; }
    const std::string& getServiceName() const override { return service_name_; }
    // Locked so a publishConfig() swap cannot race a worker thread's read of
    // the handle. Reading the pointed-to Config needs no lock: published
    // snapshots are never mutated (see publishConfig / mutableConfig).
    std::shared_ptr<const Config> getConfig() const override {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_;
    }
    int64_t getStartTime() const override { return start_time_; }
    void reloadConfig(std::shared_ptr<const Config> cfg) override {
        if (cfg) {
            // Publish a fresh snapshot instead of writing through the live
            // one: getConfig()'s contract is that published snapshots are
            // never mutated, so readers holding the old snapshot keep a
            // consistent view (same shape as publishConfig below).
            auto next = std::make_shared<Config>(*cfg);
            std::lock_guard<std::mutex> lock(config_mutex_);
            config_ = std::move(next);
        }
    }

    TraceId generateTraceId() override {
        return TraceId{agent_id_, start_time_, trace_id_counter_++};
    }

    void recordSpan(std::unique_ptr<SpanChunk> span) const override {
        recorded_spans_.push_back(std::move(span));
    }

    void recordUrlStat(UrlStatEntry stat) const override {
        recorded_url_stats_++;
        last_url_stat_url_ = stat.url_pattern_;
        last_url_stat_method_ = stat.method_;
        last_url_stat_status_code_ = stat.status_code_;
        last_url_stat_failed_ = stat.failed_;
    }

    void recordException(const TraceId& trace_id, int64_t span_id, std::string_view url_template,
                         std::vector<std::unique_ptr<Exception>>&& exceptions) const override {
        recorded_exceptions_++;
    }

    void recordStats(StatsType stats) const override {
        recorded_stats_calls_++;
        last_stats_type_ = stats;
        // Fault injection for the worker-supervisor tests: throw for the
        // next N calls, then behave normally.
        if (stats_throws_remaining_.load(std::memory_order_relaxed) > 0) {
            stats_throws_remaining_--;
            throw std::runtime_error("injected recordStats failure");
        }
    }

    int32_t cacheApi(std::string_view api_str, int32_t api_type) const override {
        // The real agent returns 0 when it is disabled or the cache throws;
        // spans then fall back to sending the operation name itself, so tests
        // need a way to reach that branch.
        if (api_caching_disabled_) {
            return 0;
        }
        auto key = std::string(api_str);
        if (cached_apis_.find(key) == cached_apis_.end()) {
            cached_apis_[key] = api_id_counter_++;
        }
        return cached_apis_[key];
    }

    /// @brief Makes cacheApi() report failure, as a disabled agent does.
    void setApiCachingDisabled(bool disabled) { api_caching_disabled_ = disabled; }

    void removeCacheApi(const ApiMeta& api_meta) const override {
        removed_api_count_++;
    }

    int32_t cacheError(std::string_view error_name) const override {
        auto key = std::string(error_name);
        if (cached_errors_.find(key) == cached_errors_.end()) {
            cached_errors_[key] = error_id_counter_++;
        }
        return cached_errors_[key];
    }

    void removeCacheError(const StringMeta& error_meta) const override {
        removed_error_count_++;
    }

    int32_t cacheSql(std::string_view sql_query) const override {
        // Test hook: emulate the collector rejecting a SQL id so prepareSql's
        // invalid-id guard can be exercised without touching cached_sqls_.
        if (force_sql_id_failure_.load(std::memory_order_relaxed)) {
            return 0;
        }
        auto key = std::string(sql_query);
        if (cached_sqls_.find(key) == cached_sqls_.end()) {
            cached_sqls_[key] = sql_id_counter_++;
        }
        return cached_sqls_[key];
    }

    std::optional<PreparedSqlResult> prepareSql(
            std::string_view raw_sql, SqlMetaMode mode) const override try {
        static const SqlNormalizer normalizer(kMaxNormalizedSqlLength);
        // One locked snapshot for the whole call: reading the config_ member
        // directly would race publishConfig()'s pointer swap on another
        // thread (and could even drop the last reference mid-read).
        const auto config = getConfig();

        // Mirrors AgentImpl::prepareSql: the raw cache holds the normalization
        // only, and the identity is resolved per call.
        auto prepare = [&]() -> PreparedSqlRef {
            sql_normalize_count_.fetch_add(1, std::memory_order_relaxed);
            auto normalized = normalizer.normalize(raw_sql);
            return std::make_shared<const PreparedSql>(PreparedSql{
                std::move(normalized.parameters),
                std::move(normalized.normalized_sql)});
        };
        auto& cache = (mode == SqlMetaMode::Id) ? raw_sql_id_cache_
                                                : raw_sql_uid_cache_;
        auto sql = config->sql.enable_raw_sql_cache ? cache.get(raw_sql, prepare).value
                                                    : prepare();

        if (mode == SqlMetaMode::Id) {
            const auto id = cacheSql(sql->normalized_sql);
            if (id <= 0) {
                return std::nullopt;
            }
            return PreparedSqlResult{std::move(sql), SqlIdentity{id}};
        }

        auto uid = cacheSqlUid(sql->normalized_sql);
        if (!uid) {
            return std::nullopt;
        }
        return PreparedSqlResult{std::move(sql), SqlIdentity{*uid}};
    } catch (const std::exception&) {
        // AgentImpl::prepareSql swallows preparation failures and returns
        // nullopt; the mock honors the same contract so callers observe
        // identical behavior instead of an escaping exception.
        return std::nullopt;
    }

    void removeCacheSql(const StringMeta& sql_meta) const override {
        removed_sql_count_++;
    }

    std::optional<SqlUid> cacheSqlUid(std::string_view sql) const override {
        return SqlUid{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    }

    void removeCacheSqlUid(const SqlUidMeta& sql_uid_meta) const override {
        removed_sql_uid_count_++;
    }

    bool isStatusFail(int status) const override {
        // Mirror AgentImpl::isStatusFail: use the configurable HttpStatusErrors
        // (default {"5xx"}) so URL-stat failure tracks the same rule as spans.
        // The locked snapshot (not the raw config_ member) keeps the tokens
        // reference valid across a concurrent publishConfig() swap.
        const auto config = getConfig();
        const auto& tokens = config->http.server.status_errors;
        if (tokens.empty()) {
            return false;
        }
        return HttpStatusErrors(tokens).isErrorCode(status);
    }

    void recordServerHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const override {
        recorded_server_headers_++;
    }

    void recordClientHeader(HeaderType which, HeaderReader& reader, PinpointAnnotation* annotation) const override {
        recorded_client_headers_++;
    }

    // Lazily created, but the agent calls these from its worker threads: the
    // command service builds active-thread-count responses from several stream
    // threads at once, so a plain check-then-act here is a data race on the
    // handle (one thread reading it while another stores the new instance).
    // call_once both serializes construction and publishes it to every reader.
    AgentStats& getAgentStats() override {
        std::call_once(agent_stats_once_, [this]() {
            agent_stats_ = std::make_unique<AgentStats>(this);
        });
        return *agent_stats_;
    }

    UrlStats& getUrlStats() override {
        std::call_once(url_stats_once_, [this]() {
            url_stats_ = std::make_unique<UrlStats>(this);
        });
        return *url_stats_;
    }

    // Test helpers - setters
    void setExiting(bool exiting) { is_exiting_.store(exiting, std::memory_order_relaxed); }
    void setAppName(const std::string& name) { app_name_ = name; }
    void setAppType(int32_t type) { app_type_ = type; }
    void setAgentId(const std::string& id) { agent_id_ = id; }
    void setServiceName(const std::string& name) { service_name_ = name; }
    void setStartTime(int64_t time) { start_time_ = time; }

    // Direct config access for test customization.
    //
    // Setup only: this hands out the live snapshot, so writing through it while
    // a worker thread can read the config is a data race. Use publishConfig()
    // for a change that has to land after threads are already running.
    std::shared_ptr<Config>& mutableConfig() { return config_; }

    // Applies a config change the way AgentImpl::reloadConfig() does: build a
    // new snapshot and publish the pointer, instead of mutating the object that
    // readers already hold. A worker keeps reading the snapshot it captured
    // (nobody writes to it), and picks the new one up on its next getConfig().
    void publishConfig(const std::function<void(Config&)>& mutate) {
        auto next = std::make_shared<Config>(*getConfig());
        mutate(*next);
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = std::move(next);
    }

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
    uint64_t getSqlNormalizeCount() const {
        return sql_normalize_count_.load(std::memory_order_relaxed);
    }
    void setForceSqlIdFailure(bool fail) {
        force_sql_id_failure_.store(fail, std::memory_order_relaxed);
    }
    size_t getRecordedSpansCount() const { return recorded_spans_.size(); }

    // Test-observable state
    mutable std::vector<std::unique_ptr<SpanChunk>> recorded_spans_;
    mutable int recorded_url_stats_ = 0;
    mutable int recorded_exceptions_ = 0;
    // Atomic so tests can poll these from another thread while a worker runs
    mutable std::atomic<int> recorded_stats_calls_{0};
    mutable std::atomic<StatsType> last_stats_type_{AGENT_STATS};
    // See recordStats(): number of upcoming calls that throw.
    mutable std::atomic<int> stats_throws_remaining_{0};
    mutable int recorded_server_headers_ = 0;
    mutable int recorded_client_headers_ = 0;
    mutable std::string last_url_stat_url_;
    mutable std::string last_url_stat_method_;
    mutable int last_url_stat_status_code_ = 0;
    mutable bool last_url_stat_failed_ = false;
    mutable std::map<std::string, int32_t> cached_apis_;
    mutable std::map<std::string, int32_t> cached_errors_;
    mutable std::map<std::string, int32_t> cached_sqls_;
    mutable int32_t api_id_counter_ = 100;
    bool api_caching_disabled_ = false;
    mutable int32_t error_id_counter_ = 200;
    mutable int32_t sql_id_counter_ = 300;
    // Atomic so tests can poll these from another thread while a worker runs
    mutable std::atomic<int> removed_api_count_{0};
    mutable std::atomic<int> removed_error_count_{0};
    mutable std::atomic<int> removed_sql_count_{0};
    mutable std::atomic<int> removed_sql_uid_count_{0};
    mutable RawSqlCache raw_sql_id_cache_{128, 4};
    mutable RawSqlCache raw_sql_uid_cache_{128, 4};
    mutable std::atomic<uint64_t> sql_normalize_count_{0};
    mutable std::atomic<bool> force_sql_id_failure_{false};

private:
    // Toggled by the test body while gRPC worker threads poll it to decide
    // when to stop — the one mock field mutated mid-run by design, so it must
    // be atomic (relaxed suffices for a stop flag). Fields below are only set
    // before workers start.
    std::atomic<bool> is_exiting_;
    int64_t start_time_;
    int64_t trace_id_counter_;
    std::string app_name_ = "TestApp";
    int32_t app_type_ = 1300;
    std::string agent_id_ = "test-agent-001";
    std::string service_name_ = "";
    mutable std::mutex config_mutex_;
    std::shared_ptr<Config> config_ = std::make_shared<Config>();
    mutable std::unique_ptr<AgentStats> agent_stats_;
    mutable std::unique_ptr<UrlStats> url_stats_;
    mutable std::once_flag agent_stats_once_;
    mutable std::once_flag url_stats_once_;
};

}  // namespace pinpoint
