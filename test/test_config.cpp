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

#include "../src/config.h"
#include "../src/logging.h"
#include "../src/object_name.h"
#include <gtest/gtest.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <unistd.h>

extern char **environ;

namespace pinpoint {

// env:: constants hold only the suffix; the agent reads "<prefix>_<suffix>".
// These tests drive the OS environment variables the agent consumes, so compose
// the full default-prefixed name here.
static std::string full_env(const char* suffix) {
    return std::string(env::DEFAULT_PREFIX) + "_" + suffix;
}

class ConfigTest : public ::testing::Test {
protected:
    // The production API takes configuration sources through AgentOptions.
    // These fixture-level shims keep the historical two-step test shape
    // (set_config_string(...) then make_config()) while routing through the
    // real make_config(options, old): unqualified calls in the test bodies
    // resolve to these members.
    AgentOptions options_;
    void set_config_string(std::string_view yaml) { options_.config_yaml = yaml; }
    void set_config_file_path(std::string_view path) { options_.config_file_path = path; }
    void set_env_prefix(std::string_view prefix) { options_.env_prefix = prefix; }
    std::shared_ptr<Config> make_config(const std::shared_ptr<const Config>& old = nullptr) {
        return pinpoint::make_config(options_, old);
    }

    void SetUp() override {
        // Snapshot the whole environment; TearDown restores it exactly, so
        // tests may set or unset any variable without hand-listing names.
        for (char** e = environ; *e != nullptr; ++e) {
            const std::string_view entry(*e);
            const auto eq = entry.find('=');
            saved_env_[std::string(entry.substr(0, eq))] = std::string(entry.substr(eq + 1));
        }

        // Drop every default-prefixed variable so each test starts from a
        // clean agent environment.
        const std::string prefix = std::string(env::DEFAULT_PREFIX) + "_";
        for (const auto& [name, value] : saved_env_) {
            if (name.rfind(prefix, 0) == 0) {
                unsetenv(name.c_str());
            }
        }

        // Start each test from pristine options.
        options_ = AgentOptions{};

        // Create a temporary directory for test files
        temp_dir_ = "/tmp/pinpoint_config_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        // Restore the snapshot. Names are collected before any unsetenv():
        // removing an entry mutates environ while it is being walked.
        std::vector<std::string> current_names;
        for (char** e = environ; *e != nullptr; ++e) {
            const std::string_view entry(*e);
            current_names.emplace_back(entry.substr(0, entry.find('=')));
        }
        for (const auto& name : current_names) {
            if (saved_env_.find(name) == saved_env_.end()) {
                unsetenv(name.c_str());
            }
        }
        for (const auto& [name, value] : saved_env_) {
            setenv(name.c_str(), value.c_str(), 1);
        }

        // Clean up temporary files
        std::filesystem::remove_all(temp_dir_);
    }

protected:
    std::map<std::string, std::string> saved_env_;
    std::string temp_dir_;
    
    // Test YAML configurations
    const std::string complete_config_yaml_ = R"(
ApplicationName: "MyTestApp"
AgentName: "TestAgentName"
Enable: true
IsContainer: true

Log:
  Level: "debug"
  FilePath: "/tmp/pinpoint.log"
  MaxFileSize: 20

Collector:
  GrpcHost: "test.collector.host"
  GrpcAgentPort: 9000
  GrpcSpanPort: 9001
  GrpcStatPort: 9002
  Grpc:
    SslEnable: true
    TrustCertFilePath: "/tmp/trust.pem"
    RootCertFilePath: "/tmp/root.pem"
    KeepAliveTimeMs: 31000
    KeepAliveTimeoutMs: 62000
    KeepAlivePermitWithoutCalls: true
    MaxSendMessageSize: 5242880
    MaxReceiveMessageSize: 6291456
    SenderQueueSize: 1100
    ChannelMaxAgeMs: 3600000
    StreamMaxAgeMs: 1800000
  AgentInfo:
    RefreshIntervalMs: 60000
    SendRetryIntervalMs: 25
    MaxTryPerAttempt: 2

Sampling:
  Type: "PERCENT"
  CounterRate: 20
  PercentRate: 0.1
  NewThroughput: 50
  ContinueThroughput: 60

Span:
  QueueSize: 512
  MaxEventDepth: 32
  MaxEventSequence: 512
  EventChunkSize: 50

Stat:
  Enable: true
  BatchCount: 10
  BatchInterval: 7000

Http:
  CollectUrlStat: true
  UrlStatLimit: 2048
  UrlStatQueueSize: 4096
  UrlStatEnableTrimPath: false
  UrlStatTrimPathDepth: 3
  UrlStatMethodPrefix: true
  
  Server:
    StatusCodeErrors: ["5xx", "401", "403"]
    ExcludeUrl: ["/health", "/metrics"]
    ExcludeMethod: ["PUT", "DELETE"]
    RecordRequestHeader: ["Authorization", "Accept"]
    RecordRequestCookie: ["session"]
    RecordResponseHeader: ["Content-Type"]
  
  Client:
    RecordRequestHeader: ["User-Agent"]
    RecordRequestCookie: ["tracking"]
    RecordResponseHeader: ["headers-all"]

Sql:
  MaxBindArgsSize: 2048
  EnableSqlStats: true
  EnableRawSqlCache: true
  TraceBindValue: true
)";

    const std::string partial_config_yaml_ = R"(
ApplicationName: "PartialApp"

Collector:
  GrpcHost: "partial.host"
  GrpcAgentPort: 8000

Sampling:
  Type: "COUNTER"
  CounterRate: 5

Http:
  Server:
    StatusCodeErrors: ["4xx"]

Sql:
  MaxBindArgsSize: 512
  EnableSqlStats: false
)";

    const std::string invalid_yaml_ = R"(
ApplicationName: "InvalidApp"
  InvalidIndentation: true
UnmatchedQuote: "this is invalid
)";

    const std::string extreme_values_yaml_ = R"(
Sampling:
  CounterRate: -100
  PercentRate: 150.5
  NewThroughput: -50
  ContinueThroughput: -30

Span:
  QueueSize: 0
  MaxEventDepth: -1
  MaxEventSequence: -1
  EventChunkSize: -1

Http:
  UrlStatLimit: -1
  UrlStatQueueSize: 0
)";
};

// ========== Default Configuration Tests ==========

// Test default configuration values
TEST_F(ConfigTest, DefaultConfigurationTest) {
    auto config = make_config();
    
    // Test basic default values
    EXPECT_EQ(config->app_name_, "") << "Default app name should be empty";
    EXPECT_FALSE(config->agent_id_.empty()) << "Agent ID should be generated when empty, so not empty after make_config";
    EXPECT_EQ(config->agent_name_, "") << "Default agent name should be empty";
    EXPECT_TRUE(config->enable) << "Should be enabled by default";
    
    // Test log defaults
    EXPECT_EQ(config->log.level, "info") << "Default log level should be info";
    EXPECT_EQ(config->log.file_path, "") << "Default log file path should be empty";
    EXPECT_EQ(config->log.max_file_size, 10) << "Default max file size should be 10MB";
    
    // Test collector defaults
    EXPECT_EQ(config->collector.host, "") << "Default collector host should be empty";
    EXPECT_EQ(config->collector.agent_port, 9991) << "Default agent port should be 9991";
    EXPECT_EQ(config->collector.span_port, 9993) << "Default span port should be 9993";
    EXPECT_EQ(config->collector.stat_port, 9992) << "Default stat port should be 9992";

    // Test gRPC channel defaults
    EXPECT_FALSE(config->collector.grpc.ssl.enable) << "gRPC SSL should be disabled by default";
    EXPECT_EQ(config->collector.grpc.channel.keepalive_time_ms, 30000) << "Default keepalive time should be 30s";
    EXPECT_EQ(config->collector.grpc.channel.keepalive_timeout_ms, 60000) << "Default keepalive timeout should be 60s";
    EXPECT_EQ(config->collector.grpc.channel.max_send_message_size, 4 * 1024 * 1024) << "Default max send size should be 4MiB";
    EXPECT_EQ(config->collector.grpc.channel.max_receive_message_size, 4 * 1024 * 1024) << "Default max receive size should be 4MiB";
    // Connection renewal is opt-in: existing deployments must keep one
    // channel and one stream for the process lifetime.
    EXPECT_EQ(config->collector.grpc.channel.channel_max_age_ms, 0) << "Channel rotation should be disabled by default";
    EXPECT_EQ(config->collector.grpc.channel.stream_max_age_ms, 0) << "Stream max age should be disabled by default";

    // Test sampling defaults
    EXPECT_EQ(config->sampling.type, "COUNTER") << "Default sampling type should be COUNTER";
    EXPECT_EQ(config->sampling.counter_rate, 1) << "Default counter rate should be 1";
    EXPECT_EQ(config->sampling.percent_rate, 100) << "Default percent rate should be 100";
    EXPECT_EQ(config->sampling.new_throughput, 0) << "Default new throughput should be 0";
    EXPECT_EQ(config->sampling.cont_throughput, 0) << "Default continue throughput should be 0";
    
    // Test span defaults
    EXPECT_EQ(config->span.queue_size, 1024) << "Default queue size should be 1024";
    EXPECT_EQ(config->span.max_event_depth, 64) << "Default max event depth should be 64";
    EXPECT_EQ(config->span.max_event_sequence, 5000) << "Default max event sequence should be 5000";
    EXPECT_EQ(config->span.event_chunk_size, 20) << "Default event chunk size should be 20";

    EXPECT_EQ(config->collector.agent_info.refresh_interval_ms, defaults::AGENT_INFO_REFRESH_INTERVAL_MS)
        << "Default AgentInfo refresh interval should match Java daily refresh";
    EXPECT_EQ(config->collector.agent_info.send_retry_interval_ms, defaults::AGENT_INFO_SEND_RETRY_INTERVAL_MS)
        << "Default AgentInfo retry interval should be 3000ms";
    EXPECT_EQ(config->collector.agent_info.max_try_per_attempt, defaults::AGENT_INFO_MAX_TRY_PER_ATTEMPT)
        << "Default AgentInfo max try count should be 3";
    
    // Test stat defaults
    EXPECT_TRUE(config->stat.enable) << "Stat should be enabled by default";
    EXPECT_EQ(config->stat.batch_count, 6) << "Default batch count should be 6";
    EXPECT_EQ(config->stat.collect_interval, 5000) << "Default collect interval should be 5000ms";
    
    // Test HTTP defaults
    EXPECT_FALSE(config->http.url_stat.enable) << "URL stat should be disabled by default";
    EXPECT_EQ(config->http.url_stat.limit, 1024) << "Default URL stat limit should be 1024";
    EXPECT_EQ(config->http.url_stat.queue_size, 1024) << "Default URL stat queue size should be 1024";
    EXPECT_TRUE(config->http.url_stat.enable_trim_path) << "Enable trim path should be true by default";
    EXPECT_EQ(config->http.url_stat.trim_path_depth, 1) << "Default path depth should be 1";
    EXPECT_FALSE(config->http.url_stat.method_prefix) << "Method prefix should be false by default";
    
    // Test HTTP server defaults
    EXPECT_EQ(config->http.server.status_errors.size(), 1) << "Should have default status error";
    EXPECT_EQ(config->http.server.status_errors[0], "5xx") << "Default status error should be 5xx";
    EXPECT_TRUE(config->http.server.exclude_url.empty()) << "Exclude URL list should be empty by default";
    EXPECT_TRUE(config->http.server.exclude_method.empty()) << "Exclude method list should be empty by default";
    EXPECT_TRUE(config->http.server.rec_request_header.empty()) << "Request header list should be empty by default";
    EXPECT_TRUE(config->http.server.rec_request_cookie.empty()) << "Request cookie list should be empty by default";
    EXPECT_TRUE(config->http.server.rec_response_header.empty()) << "Response header list should be empty by default";
    
    // Test HTTP client defaults
    EXPECT_TRUE(config->http.client.rec_request_header.empty()) << "Client request header list should be empty by default";
    EXPECT_TRUE(config->http.client.rec_request_cookie.empty()) << "Client request cookie list should be empty by default";
    EXPECT_TRUE(config->http.client.rec_response_header.empty()) << "Client response header list should be empty by default";
    
    // Test SQL defaults
    EXPECT_EQ(config->sql.max_bind_args_size, 1024) << "Default max bind args size should be 1024";
    EXPECT_FALSE(config->sql.enable_sql_stats) << "SQL stats should be disabled by default";
    EXPECT_TRUE(config->sql.enable_raw_sql_cache) << "Raw SQL cache should be enabled by default";
    EXPECT_TRUE(config->sql.trace_bind_value) << "SQL bind value tracing should be enabled by default";
    
    // Test CallStack trace default
    EXPECT_FALSE(config->enable_callstack_trace) << "CallStack trace should be disabled by default";
}

TEST_F(ConfigTest, RevisionStampsEachGeneration) {
    // Not a configuration input: 1 on the first load, +1 per reload, so
    // binding layers can detect a published generation change by value.
    auto first = make_config();
    EXPECT_EQ(first->revision, 1);

    auto reloaded = make_config(first);
    EXPECT_EQ(reloaded->revision, 2);
    EXPECT_EQ(make_config(reloaded)->revision, 3);

    // A fresh first load (no old config) restarts at 1.
    EXPECT_EQ(make_config()->revision, 1);
}

TEST_F(ConfigTest, GeneratedAgentIdTest) {
    auto config = make_config();
    
    EXPECT_FALSE(config->agent_id_.empty()) << "Agent ID should be generated when not provided";
    EXPECT_GE(config->agent_id_.length(), 5) << "Generated agent ID should have reasonable length";
    
    // Test that multiple calls generate different IDs
    auto config2 = make_config();
    EXPECT_NE(config->agent_id_, config2->agent_id_) << "Multiple calls should generate different agent IDs";
}

// ========== YAML Configuration Tests ==========

TEST_F(ConfigTest, CompleteYamlConfigurationTest) {
    set_config_string(complete_config_yaml_);
    auto config = make_config();
    
    // Test basic values
    EXPECT_EQ(config->app_name_, "MyTestApp") << "App name should match YAML";
    EXPECT_EQ(config->agent_id_.size(), 22u)
        << "the Agent ID is always auto-generated (base64 UUIDv7)";
    EXPECT_EQ(config->agent_name_, "TestAgentName") << "Agent name should match YAML";
    EXPECT_TRUE(config->enable) << "Enable should match YAML";
    EXPECT_TRUE(config->is_container) << "IsContainer should match YAML";
    
    // Test log configuration
    EXPECT_EQ(config->log.level, "debug") << "Log level should match YAML";
    EXPECT_EQ(config->log.file_path, "/tmp/pinpoint.log") << "Log file path should match YAML";
    EXPECT_EQ(config->log.max_file_size, 20) << "Log max file size should match YAML";
    
    // Test collector configuration
    EXPECT_EQ(config->collector.host, "test.collector.host") << "Collector host should match YAML";
    EXPECT_EQ(config->collector.agent_port, 9000) << "Agent port should match YAML";
    EXPECT_EQ(config->collector.span_port, 9001) << "Span port should match YAML";
    EXPECT_EQ(config->collector.stat_port, 9002) << "Stat port should match YAML";

    // Test gRPC channel configuration
    EXPECT_EQ(config->collector.grpc.ssl.trust_cert_file_path, "/tmp/trust.pem") << "Trust cert path should match YAML";
    EXPECT_EQ(config->collector.grpc.ssl.root_cert_file_path, "/tmp/root.pem") << "Root cert path should match YAML";
    EXPECT_TRUE(config->collector.grpc.ssl.enable) << "gRPC SSL enable should match YAML";
    EXPECT_EQ(config->collector.grpc.channel.keepalive_time_ms, 31000) << "gRPC keepalive time should match YAML";
    EXPECT_EQ(config->collector.grpc.channel.keepalive_timeout_ms, 62000) << "gRPC keepalive timeout should match YAML";
    EXPECT_TRUE(config->collector.grpc.channel.keepalive_permit_without_calls) << "gRPC keepalive permit should match YAML";
    EXPECT_EQ(config->collector.grpc.channel.max_send_message_size, 5242880) << "gRPC max send size should match YAML";
    EXPECT_EQ(config->collector.grpc.channel.max_receive_message_size, 6291456) << "gRPC max receive size should match YAML";
    EXPECT_EQ(config->collector.grpc.channel.sender_queue_size, 1100) << "gRPC sender queue size should match YAML";
    EXPECT_EQ(config->collector.grpc.channel.channel_max_age_ms, 3600000) << "gRPC channel max age should match YAML";
    EXPECT_EQ(config->collector.grpc.channel.stream_max_age_ms, 1800000) << "gRPC stream max age should match YAML";

    // Test sampling configuration
    EXPECT_EQ(config->sampling.type, "PERCENT") << "Sampling type should match YAML";
    EXPECT_EQ(config->sampling.counter_rate, 20) << "Counter rate should match YAML";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 0.1) << "Percent rate should match YAML";
    EXPECT_EQ(config->sampling.new_throughput, 50) << "New throughput should match YAML";
    EXPECT_EQ(config->sampling.cont_throughput, 60) << "Continue throughput should match YAML";
    
    // Test span configuration
    EXPECT_EQ(config->span.queue_size, 512) << "Queue size should match YAML";
    EXPECT_EQ(config->span.max_event_depth, 32) << "Max event depth should match YAML";
    EXPECT_EQ(config->span.max_event_sequence, 512) << "Max event sequence should match YAML";
    EXPECT_EQ(config->span.event_chunk_size, 50) << "Event chunk size should match YAML";

    EXPECT_EQ(config->collector.agent_info.refresh_interval_ms, 60000) << "AgentInfo refresh interval should match YAML";
    EXPECT_EQ(config->collector.agent_info.send_retry_interval_ms, 25) << "AgentInfo retry interval should match YAML";
    EXPECT_EQ(config->collector.agent_info.max_try_per_attempt, 2) << "AgentInfo max try count should match YAML";
    
    // Test stat configuration
    EXPECT_TRUE(config->stat.enable) << "Stat enable should match YAML";
    EXPECT_EQ(config->stat.batch_count, 10) << "Batch count should match YAML";
    EXPECT_EQ(config->stat.collect_interval, 7000) << "Collect interval should match YAML";
    
    // Test HTTP configuration
    EXPECT_TRUE(config->http.url_stat.enable) << "URL stat enable should match YAML";
    EXPECT_EQ(config->http.url_stat.limit, 2048) << "URL stat limit should match YAML";
    EXPECT_EQ(config->http.url_stat.queue_size, 4096) << "URL stat queue size should match YAML";
    EXPECT_FALSE(config->http.url_stat.enable_trim_path) << "URL stat enable trim path should match YAML";
    EXPECT_EQ(config->http.url_stat.trim_path_depth, 3) << "URL stat path depth should match YAML";
    EXPECT_TRUE(config->http.url_stat.method_prefix) << "URL stat method prefix should match YAML";
    
    // Test HTTP server configuration
    EXPECT_EQ(config->http.server.status_errors.size(), 3) << "Should have 3 status errors";
    EXPECT_EQ(config->http.server.status_errors[0], "5xx") << "First status error should be 5xx";
    EXPECT_EQ(config->http.server.status_errors[1], "401") << "Second status error should be 401";
    EXPECT_EQ(config->http.server.status_errors[2], "403") << "Third status error should be 403";
    
    EXPECT_EQ(config->http.server.exclude_url.size(), 2) << "Should have 2 exclude URLs";
    EXPECT_EQ(config->http.server.exclude_url[0], "/health") << "First exclude URL should be /health";
    EXPECT_EQ(config->http.server.exclude_url[1], "/metrics") << "Second exclude URL should be /metrics";
    
    EXPECT_EQ(config->http.server.exclude_method.size(), 2) << "Should have 2 exclude methods";
    EXPECT_EQ(config->http.server.exclude_method[0], "PUT") << "First exclude method should be PUT";
    EXPECT_EQ(config->http.server.exclude_method[1], "DELETE") << "Second exclude method should be DELETE";
    
    EXPECT_EQ(config->http.server.rec_request_header.size(), 2) << "Should have 2 request headers";
    EXPECT_EQ(config->http.server.rec_request_header[0], "Authorization") << "First request header should be Authorization";
    EXPECT_EQ(config->http.server.rec_request_header[1], "Accept") << "Second request header should be Accept";
    
    EXPECT_EQ(config->http.server.rec_request_cookie.size(), 1) << "Should have 1 request cookie";
    EXPECT_EQ(config->http.server.rec_request_cookie[0], "session") << "Request cookie should be session";
    
    EXPECT_EQ(config->http.server.rec_response_header.size(), 1) << "Should have 1 response header";
    EXPECT_EQ(config->http.server.rec_response_header[0], "Content-Type") << "Response header should be Content-Type";
    
    // Test HTTP client configuration
    EXPECT_EQ(config->http.client.rec_request_header.size(), 1) << "Should have 1 client request header";
    EXPECT_EQ(config->http.client.rec_request_header[0], "User-Agent") << "Client request header should be User-Agent";
    
    EXPECT_EQ(config->http.client.rec_request_cookie.size(), 1) << "Should have 1 client request cookie";
    EXPECT_EQ(config->http.client.rec_request_cookie[0], "tracking") << "Client request cookie should be tracking";
    
    EXPECT_EQ(config->http.client.rec_response_header.size(), 1) << "Should have 1 client response header";
    EXPECT_EQ(config->http.client.rec_response_header[0], "headers-all") << "Client response header should be headers-all";
    
    // Test SQL configuration
    EXPECT_EQ(config->sql.max_bind_args_size, 2048) << "Max bind args size should match YAML";
    EXPECT_TRUE(config->sql.enable_sql_stats) << "SQL stats should be enabled as per YAML";
    EXPECT_TRUE(config->sql.enable_raw_sql_cache) << "Raw SQL cache should be enabled as per YAML";
    EXPECT_TRUE(config->sql.trace_bind_value) << "SQL bind value tracing should be enabled as per YAML";
}

TEST_F(ConfigTest, PartialYamlConfigurationTest) {
    set_config_string(partial_config_yaml_);
    auto config = make_config();
    
    // Test overridden values
    EXPECT_EQ(config->app_name_, "PartialApp") << "App name should match partial YAML";
    EXPECT_EQ(config->collector.host, "partial.host") << "Collector host should match partial YAML";
    EXPECT_EQ(config->collector.agent_port, 8000) << "Agent port should match partial YAML";
    EXPECT_EQ(config->sampling.type, "COUNTER") << "Sampling type should match partial YAML";
    EXPECT_EQ(config->sampling.counter_rate, 5) << "Counter rate should match partial YAML";
    
    EXPECT_EQ(config->http.server.status_errors.size(), 1) << "Should have 1 status error from partial YAML";
    EXPECT_EQ(config->http.server.status_errors[0], "4xx") << "Status error should be 4xx from partial YAML";
    
    // Test values that should remain default
    EXPECT_EQ(config->collector.span_port, 9993) << "Span port should remain default";
    EXPECT_EQ(config->collector.stat_port, 9992) << "Stat port should remain default";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 100) << "Percent rate should remain default";
    EXPECT_EQ(config->log.level, "info") << "Log level should remain default";
    EXPECT_EQ(config->span.queue_size, 1024) << "Queue size should remain default";
    
    // Test SQL configuration from partial YAML
    EXPECT_EQ(config->sql.max_bind_args_size, 512) << "Max bind args size should match partial YAML";
    EXPECT_FALSE(config->sql.enable_sql_stats) << "SQL stats should be disabled as per partial YAML";
    EXPECT_TRUE(config->sql.trace_bind_value) << "SQL bind value tracing should remain enabled by default";
}

TEST_F(ConfigTest, EmptyYamlConfigurationTest) {
    set_config_string("");
    auto config = make_config();
    
    // Should have all default values
    EXPECT_EQ(config->app_name_, "") << "App name should be default (empty)";
    EXPECT_EQ(config->log.level, "info") << "Log level should be default";
    EXPECT_EQ(config->collector.agent_port, 9991) << "Agent port should be default";
}

// ========== Environment Variable Tests ==========

TEST_F(ConfigTest, EnvironmentVariableConfigurationTest) {
    // Set environment variables
    setenv(full_env(env::APPLICATION_NAME).c_str(), "EnvApp", 1);
    setenv(full_env(env::AGENT_NAME).c_str(), "env-agent-456", 1);
    setenv(full_env(env::LOG_LEVEL).c_str(), "error", 1);
    setenv(full_env(env::GRPC_HOST).c_str(), "env.collector.host", 1);
    setenv(full_env(env::GRPC_AGENT_PORT).c_str(), "8888", 1);
    setenv(full_env(env::SAMPLING_TYPE).c_str(), "PERCENT", 1);
    setenv(full_env(env::SAMPLING_PERCENT_RATE).c_str(), "25.5", 1);
    setenv(full_env(env::IS_CONTAINER).c_str(), "true", 1);
    setenv(full_env(env::SQL_MAX_BIND_ARGS_SIZE).c_str(), "4096", 1);
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "true", 1);
    setenv(full_env(env::SQL_ENABLE_RAW_SQL_CACHE).c_str(), "true", 1);
    setenv(full_env(env::SQL_TRACE_BIND_VALUE).c_str(), "true", 1);
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "false", 1);
    setenv(full_env(env::HTTP_URL_STAT_QUEUE_SIZE).c_str(), "333", 1);
    setenv(full_env(env::AGENT_INFO_REFRESH_INTERVAL_MS).c_str(), "120000", 1);
    setenv(full_env(env::AGENT_INFO_SEND_RETRY_INTERVAL_MS).c_str(), "50", 1);
    setenv(full_env(env::AGENT_INFO_MAX_TRY_PER_ATTEMPT).c_str(), "4", 1);
    setenv(full_env(env::GRPC_SSL_TRUST_CERT_FILE_PATH).c_str(), "/env/trust.pem", 1);
    setenv(full_env(env::GRPC_SSL_ENABLE).c_str(), "true", 1);
    setenv(full_env(env::GRPC_KEEPALIVE_TIME_MS).c_str(), "22222", 1);
    setenv(full_env(env::GRPC_MAX_SEND_MESSAGE_SIZE).c_str(), "33333", 1);
    setenv(full_env(env::GRPC_SENDER_QUEUE_SIZE).c_str(), "4444", 1);
    setenv(full_env(env::GRPC_MAX_RECEIVE_MESSAGE_SIZE).c_str(), "55555", 1);
    setenv(full_env(env::GRPC_CHANNEL_MAX_AGE_MS).c_str(), "600000", 1);
    setenv(full_env(env::GRPC_STREAM_MAX_AGE_MS).c_str(), "300000", 1);

    auto config = make_config();
    
    // Test environment variable values
    EXPECT_EQ(config->app_name_, "EnvApp") << "App name should match environment variable";
    EXPECT_EQ(config->agent_name_, "env-agent-456") << "Agent name should match environment variable";
    EXPECT_EQ(config->agent_id_.size(), 22u)
        << "the Agent ID is always auto-generated (base64 UUIDv7)";
    EXPECT_EQ(config->log.level, "error") << "Log level should match environment variable";
    EXPECT_EQ(config->collector.host, "env.collector.host") << "Collector host should match environment variable";
    EXPECT_EQ(config->collector.agent_port, 8888) << "Agent port should match environment variable";
    EXPECT_EQ(config->sampling.type, "PERCENT") << "Sampling type should match environment variable";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 25.5) << "Percent rate should match environment variable";
    EXPECT_TRUE(config->is_container) << "IsContainer should match environment variable";
    
    // Test SQL environment variable values
    EXPECT_EQ(config->sql.max_bind_args_size, 4096) << "Max bind args size should match environment variable";
    EXPECT_TRUE(config->sql.enable_sql_stats) << "SQL stats should be enabled as per environment variable";
    EXPECT_TRUE(config->sql.enable_raw_sql_cache) << "Raw SQL cache should be enabled as per environment variable";
    EXPECT_TRUE(config->sql.trace_bind_value) << "SQL bind value tracing should be enabled as per environment variable";
    
    // Test HTTP environment variable values
    EXPECT_FALSE(config->http.url_stat.enable_trim_path) << "URL stat enable trim path should match environment variable";
    EXPECT_EQ(config->http.url_stat.queue_size, 333) << "URL stat queue size should match environment variable";

    EXPECT_EQ(config->collector.agent_info.refresh_interval_ms, 120000) << "AgentInfo refresh interval should match environment variable";
    EXPECT_EQ(config->collector.agent_info.send_retry_interval_ms, 50) << "AgentInfo retry interval should match environment variable";
    EXPECT_EQ(config->collector.agent_info.max_try_per_attempt, 4) << "AgentInfo max try count should match environment variable";

    // Test gRPC environment variable values
    EXPECT_EQ(config->collector.grpc.ssl.trust_cert_file_path, "/env/trust.pem") << "Trust cert path should match environment variable";
    EXPECT_TRUE(config->collector.grpc.ssl.enable) << "gRPC SSL enable should match environment variable";
    EXPECT_EQ(config->collector.grpc.channel.keepalive_time_ms, 22222) << "gRPC keepalive time should match environment variable";
    EXPECT_EQ(config->collector.grpc.channel.max_send_message_size, 33333) << "gRPC max send size should match environment variable";
    EXPECT_EQ(config->collector.grpc.channel.sender_queue_size, 4444) << "gRPC sender queue should match environment variable";
    EXPECT_EQ(config->collector.grpc.channel.max_receive_message_size, 55555) << "gRPC max receive size should match environment variable";
    EXPECT_EQ(config->collector.grpc.channel.channel_max_age_ms, 600000) << "gRPC channel max age should match environment variable";
    EXPECT_EQ(config->collector.grpc.channel.stream_max_age_ms, 300000) << "gRPC stream max age should match environment variable";
}

// Regression: is_container is reloadable and is NOT restored by
// retainNonReloadableFrom(), so on reload it must be inherited from the running
// config when the file does not override it. Previously make_config(old) re-ran
// is_container_env() auto-detection here, clobbering the env-sourced value.
// Both directions are checked so the test is independent of the host's actual
// container state: whichever value differs from auto-detection flips pre-fix,
// while the fix preserves both, so neither direction can false-fail.
TEST_F(ConfigTest, IsContainerSurvivesReload) {
    // A resolvable identity keeps make_config() from logging resolution errors;
    // is_container resolution is independent of it.
    setenv(full_env(env::APPLICATION_NAME).c_str(), "ReloadApp", 1);
    setenv(full_env(env::AGENT_NAME).c_str(), "reload-agent", 1);
    setenv(full_env(env::IS_CONTAINER).c_str(), "true", 1);
    auto first_true = make_config();
    ASSERT_NE(first_true, nullptr);
    ASSERT_TRUE(first_true->is_container);
    auto reloaded_true = make_config(first_true);
    ASSERT_NE(reloaded_true, nullptr);
    EXPECT_TRUE(reloaded_true->is_container)
        << "Reload must preserve env-sourced is_container=true";

    setenv(full_env(env::IS_CONTAINER).c_str(), "false", 1);
    auto first_false = make_config();
    ASSERT_NE(first_false, nullptr);
    ASSERT_FALSE(first_false->is_container);
    auto reloaded_false = make_config(first_false);
    ASSERT_NE(reloaded_false, nullptr);
    EXPECT_FALSE(reloaded_false->is_container)
        << "Reload must preserve env-sourced is_container=false";
}

// Test the preferred COLLECTOR_* environment variables
TEST_F(ConfigTest, CollectorEnvironmentVariableTest) {
    setenv(full_env(env::COLLECTOR_HOST).c_str(), "collector.env.host", 1);
    setenv(full_env(env::COLLECTOR_AGENT_PORT).c_str(), "7001", 1);
    setenv(full_env(env::COLLECTOR_SPAN_PORT).c_str(), "7002", 1);
    setenv(full_env(env::COLLECTOR_STAT_PORT).c_str(), "7003", 1);

    auto config = make_config();

    EXPECT_EQ(config->collector.host, "collector.env.host") << "Collector host should match COLLECTOR_HOST";
    EXPECT_EQ(config->collector.agent_port, 7001) << "Agent port should match COLLECTOR_AGENT_PORT";
    EXPECT_EQ(config->collector.span_port, 7002) << "Span port should match COLLECTOR_SPAN_PORT";
    EXPECT_EQ(config->collector.stat_port, 7003) << "Stat port should match COLLECTOR_STAT_PORT";
}

// Test COLLECTOR_* takes precedence over the deprecated GRPC_* variables
TEST_F(ConfigTest, CollectorEnvironmentVariableOverridesDeprecatedTest) {
    setenv(full_env(env::GRPC_HOST).c_str(), "deprecated.host", 1);
    setenv(full_env(env::GRPC_AGENT_PORT).c_str(), "8001", 1);
    setenv(full_env(env::COLLECTOR_HOST).c_str(), "preferred.host", 1);
    setenv(full_env(env::COLLECTOR_AGENT_PORT).c_str(), "9001", 1);

    auto config = make_config();

    EXPECT_EQ(config->collector.host, "preferred.host")
        << "COLLECTOR_HOST should take precedence over deprecated GRPC_HOST";
    EXPECT_EQ(config->collector.agent_port, 9001)
        << "COLLECTOR_AGENT_PORT should take precedence over deprecated GRPC_AGENT_PORT";
}

// Test the preferred Collector.* YAML keys
TEST_F(ConfigTest, CollectorYamlKeyTest) {
    set_config_string(R"(
ApplicationName: "MyApp"
Collector:
  Host: "yaml.collector.host"
  AgentPort: 7101
  SpanPort: 7102
  StatPort: 7103
)");

    auto config = make_config();

    EXPECT_EQ(config->collector.host, "yaml.collector.host") << "Collector host should match Collector.Host";
    EXPECT_EQ(config->collector.agent_port, 7101) << "Agent port should match Collector.AgentPort";
    EXPECT_EQ(config->collector.span_port, 7102) << "Span port should match Collector.SpanPort";
    EXPECT_EQ(config->collector.stat_port, 7103) << "Stat port should match Collector.StatPort";
}

// Test Collector.Host takes precedence over the deprecated Collector.GrpcHost
TEST_F(ConfigTest, CollectorYamlKeyOverridesDeprecatedTest) {
    set_config_string(R"(
ApplicationName: "MyApp"
Collector:
  GrpcHost: "deprecated.yaml.host"
  GrpcAgentPort: 8101
  Host: "preferred.yaml.host"
  AgentPort: 9101
)");

    auto config = make_config();

    EXPECT_EQ(config->collector.host, "preferred.yaml.host")
        << "Collector.Host should take precedence over deprecated Collector.GrpcHost";
    EXPECT_EQ(config->collector.agent_port, 9101)
        << "Collector.AgentPort should take precedence over deprecated Collector.GrpcAgentPort";
}

// Config keys must resolve regardless of case (lowercase / UPPERCASE / mixed),
// for both section names and leaf keys.
TEST_F(ConfigTest, CaseInsensitiveYamlKeyTest) {
    set_config_string(R"(
applicationname: "MyApp"
collector:
  host: "yaml.collector.host"
  agentport: 7101
GRPC: {}
stat:
  ENABLE: false
  BatchCount: 42
http:
  server:
    STATUSCODEERRORS: ["4xx", "5xx"]
sampling:
  PercentRate: 12.5
)");

    auto config = make_config();

    EXPECT_EQ(config->app_name_, "MyApp") << "top-level key should resolve case-insensitively";
    EXPECT_EQ(config->collector.host, "yaml.collector.host") << "nested section + leaf key should resolve case-insensitively";
    EXPECT_EQ(config->collector.agent_port, 7101) << "lowercase leaf key should resolve";
    EXPECT_FALSE(config->stat.enable) << "uppercase leaf key should resolve";
    EXPECT_EQ(config->stat.batch_count, 42) << "mixed-case leaf key should resolve";
    ASSERT_EQ(config->http.server.status_errors.size(), 2u) << "uppercase vector key should resolve";
    EXPECT_EQ(config->http.server.status_errors[0], "4xx");
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 12.5) << "mixed-case double key should resolve";
}

// Span.IgnoreErrors is a list of maps, so it needs its own yaml-cpp decoder;
// this pins the accepted spellings and the all-empty rule guard.
TEST_F(ConfigTest, SpanIgnoreErrorsYamlTest) {
    set_config_string(R"(
ApplicationName: "MyApp"
Span:
  IgnoreErrors:
    - name: "NotFound"
    - message_contains: "canceled by client"
    - name: "HttpError"
      MessageContains: "404"
    - {}
)");

    auto config = make_config();

    ASSERT_EQ(config->span.ignore_errors.size(), 3u) << "the rule with no matcher must be dropped";
    EXPECT_EQ(config->span.ignore_errors[0], (IgnoreErrorRule{"NotFound", ""}));
    EXPECT_EQ(config->span.ignore_errors[1], (IgnoreErrorRule{"", "canceled by client"}));
    EXPECT_EQ(config->span.ignore_errors[2], (IgnoreErrorRule{"HttpError", "404"}));

    EXPECT_TRUE(is_ignored_error(config->span.ignore_errors, "NotFound", "anything"));
    EXPECT_TRUE(is_ignored_error(config->span.ignore_errors, "AnyError", "request canceled by client"));
    EXPECT_TRUE(is_ignored_error(config->span.ignore_errors, "HttpError", "status 404"));
    EXPECT_FALSE(is_ignored_error(config->span.ignore_errors, "HttpError", "status 500"))
        << "name and message of one rule are ANDed";
    EXPECT_FALSE(is_ignored_error(config->span.ignore_errors, "SQLException", "connection refused"));
}

// The flat env spelling: comma separated "<name>[@<message substring>]".
TEST_F(ConfigTest, SpanIgnoreErrorsEnvTest) {
    setenv(full_env(env::APPLICATION_NAME).c_str(), "MyApp", 1);
    setenv(full_env(env::SPAN_IGNORE_ERRORS).c_str(), "NotFound,@canceled,HttpError@404,", 1);

    auto config = make_config();

    ASSERT_EQ(config->span.ignore_errors.size(), 3u);
    EXPECT_EQ(config->span.ignore_errors[0], (IgnoreErrorRule{"NotFound", ""}));
    EXPECT_EQ(config->span.ignore_errors[1], (IgnoreErrorRule{"", "canceled"}));
    EXPECT_EQ(config->span.ignore_errors[2], (IgnoreErrorRule{"HttpError", "404"}));
}

TEST_F(ConfigTest, EnvironmentVariableOverrideYamlTest) {
    // Set YAML config
    set_config_string(partial_config_yaml_);
    
    // Set environment variables that should override YAML
    setenv(full_env(env::APPLICATION_NAME).c_str(), "EnvOverrideApp", 1);
    setenv(full_env(env::GRPC_HOST).c_str(), "env.override.host", 1);
    setenv(full_env(env::SQL_MAX_BIND_ARGS_SIZE).c_str(), "8192", 1);
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "true", 1);
    
    auto config = make_config();
    
    // Environment variables should override YAML
    EXPECT_EQ(config->app_name_, "EnvOverrideApp") << "Environment variable should override YAML app name";
    EXPECT_EQ(config->collector.host, "env.override.host") << "Environment variable should override YAML collector host";
    EXPECT_EQ(config->sql.max_bind_args_size, 8192) << "Environment variable should override YAML max bind args size";
    EXPECT_TRUE(config->sql.enable_sql_stats) << "Environment variable should override YAML SQL stats setting";
    
    // YAML values should remain where no environment variable is set
    EXPECT_EQ(config->collector.agent_port, 8000) << "Agent port should remain from YAML";
}

// ========== File Configuration Tests ==========

TEST_F(ConfigTest, ConfigurationFileReadingTest) {
    // Create a temporary config file
    std::string config_file = temp_dir_ + "/test_config.yaml";
    std::ofstream file(config_file);
    file << partial_config_yaml_;
    file.close();
    
    // Set environment variable to point to config file
    setenv(full_env(env::CONFIG_FILE).c_str(), config_file.c_str(), 1);
    
    auto config = make_config();
    
    // Values should be loaded from file
    EXPECT_EQ(config->app_name_, "PartialApp") << "App name should be loaded from file";
    EXPECT_EQ(config->collector.host, "partial.host") << "Collector host should be loaded from file";
}

TEST_F(ConfigTest, MissingConfigurationFileTest) {
    // Set environment variable to point to non-existent file
    std::string missing_file = temp_dir_ + "/missing_config.yaml";
    setenv(full_env(env::CONFIG_FILE).c_str(), missing_file.c_str(), 1);
    
    auto config = make_config();
    
    // Should use default values when file is missing
    EXPECT_EQ(config->app_name_, "") << "App name should be default when file is missing";
}

// ========== Validation Logic Tests ==========

// Test value validation and correction
TEST_F(ConfigTest, ValueValidationTest) {
    set_config_string(extreme_values_yaml_);
    auto config = make_config();
    
    // Test sampling value corrections
    EXPECT_EQ(config->sampling.counter_rate, 0) << "Negative counter rate should be corrected to 0";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 100) << "Percent rate > 100 should be corrected to 100";
    EXPECT_EQ(config->sampling.new_throughput, 0) << "Negative new throughput should be corrected to 0";
    EXPECT_EQ(config->sampling.cont_throughput, 0) << "Negative continue throughput should be corrected to 0";
    
    // Test span value corrections
    EXPECT_EQ(config->span.queue_size, 1024) << "Queue size < 1 should be corrected to default (1024)";
    EXPECT_EQ(config->span.max_event_depth, INT32_MAX) << "Max event depth -1 should be corrected to INT32_MAX";
    EXPECT_EQ(config->span.max_event_sequence, INT32_MAX) << "Max event sequence -1 should be corrected to INT32_MAX";
    EXPECT_EQ(config->span.event_chunk_size, 20)
        << "Negative event chunk size should be corrected to default (20), not wrap to a huge unsigned value";

    // A negative URL stat limit would cast to a huge size_t and disable the cap.
    EXPECT_EQ(config->http.url_stat.limit, 1024) << "Negative URL stat limit should be corrected to default (1024)";
    EXPECT_EQ(config->http.url_stat.queue_size, 1024) << "URL stat queue size < 1 should be corrected to default (1024)";
}

// Test edge case percent rates
TEST_F(ConfigTest, PercentRateEdgeCasesTest) {
    // Test very small percent rate
    std::string small_percent_yaml = R"(
Sampling:
  PercentRate: 0.005
)";
    set_config_string(small_percent_yaml);
    auto config = make_config();
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 0.01) << "Very small percent rate should be corrected to minimum (0.01)";
    
    // Test negative percent rate
    std::string negative_percent_yaml = R"(
Sampling:
  PercentRate: -5.0
)";
    set_config_string(negative_percent_yaml);
    config = make_config();
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 0) << "Negative percent rate should be corrected to 0";
}

// Java spells the counter mode COUNTING (SamplerType), so a Java config must
// port over unchanged; anything unrecognised falls back to COUNTER the way
// Java's SamplerType.of() does.
TEST_F(ConfigTest, SamplingTypeCountingAliasTest) {
    std::string counting_yaml = R"(
Sampling:
  Type: "COUNTING"
)";
    set_config_string(counting_yaml);
    auto config = make_config();
    EXPECT_EQ(config->sampling.type, "COUNTER") << "COUNTING should be canonicalised to COUNTER";

    std::string lowercase_counting_yaml = R"(
Sampling:
  Type: "counting"
)";
    set_config_string(lowercase_counting_yaml);
    config = make_config();
    EXPECT_EQ(config->sampling.type, "COUNTER") << "The COUNTING alias should be case-insensitive";

    std::string unknown_yaml = R"(
Sampling:
  Type: "SOMETHING-ELSE"
)";
    set_config_string(unknown_yaml);
    config = make_config();
    EXPECT_EQ(config->sampling.type, "COUNTER") << "An unknown sampling type should fall back to COUNTER";

    std::string percent_yaml = R"(
Sampling:
  Type: "percent"
)";
    set_config_string(percent_yaml);
    config = make_config();
    EXPECT_EQ(config->sampling.type, "percent") << "A recognised type should be left as written";
}

// ========== Error Handling Tests ==========

TEST_F(ConfigTest, InvalidYamlHandlingTest) {
    set_config_string(invalid_yaml_);
    auto config = make_config();
    
    // Should fallback to default values when YAML is invalid
    EXPECT_EQ(config->app_name_, "") << "App name should be default when YAML is invalid";
    EXPECT_EQ(config->log.level, "info") << "Log level should be default when YAML is invalid";
}

// Test invalid YAML still allows environment overrides and post-load validation.
TEST_F(ConfigTest, InvalidYamlEnvironmentOverrideAndValidationTest) {
    set_config_string(invalid_yaml_);
    setenv(full_env(env::APPLICATION_NAME).c_str(), "EnvRecoveredApp", 1);
    setenv(full_env(env::AGENT_NAME).c_str(), "env-agent-recovered", 1);
    setenv(full_env(env::GRPC_HOST).c_str(), "env.collector.host", 1);
    setenv(full_env(env::GRPC_AGENT_PORT).c_str(), "70000", 1);
    setenv(full_env(env::SPAN_MAX_EVENT_DEPTH).c_str(), "1", 1);
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "true", 1);
    setenv(full_env(env::LOG_LEVEL).c_str(), "error", 1);

    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "EnvRecoveredApp")
        << "Environment app name should apply even when YAML parsing fails";
    EXPECT_EQ(config->agent_id_.size(), 22u)
        << "the Agent ID should still be auto-generated after YAML parse failure";
    EXPECT_EQ(config->agent_name_, "env-agent-recovered")
        << "Environment agent name should apply even when YAML parsing fails";
    EXPECT_TRUE(config->identity_resolved_)
        << "Identity resolution should run after YAML parse failure";
    EXPECT_EQ(config->collector.host, "env.collector.host")
        << "Environment collector host should apply after YAML parse failure";
    EXPECT_EQ(config->collector.agent_port, defaults::AGENT_PORT)
        << "Out-of-range environment port should still be clamped";
    EXPECT_EQ(config->span.max_event_depth, 2)
        << "Environment span depth should still be validated";
    EXPECT_TRUE(config->sql.enable_sql_stats)
        << "Boolean environment override should apply after YAML parse failure";
    EXPECT_EQ(config->log.level, "error")
        << "Logger config environment override should apply after YAML parse failure";
    EXPECT_TRUE(config->check())
        << "Recovered configuration should pass normal validation";
}

// ========== Configuration String Generation Tests ==========

// Test configuration to string conversion
TEST_F(ConfigTest, ConfigurationToStringTest) {
    set_config_string(complete_config_yaml_);
    auto config = make_config();
    
    std::string config_string = to_config_string(*config);
    
    // Check that generated string contains expected values
    EXPECT_TRUE(config_string.find("ApplicationName: MyTestApp") != std::string::npos)
        << "Config string should contain application name";
    EXPECT_TRUE(config_string.find("UidVersion:") != std::string::npos)
        << "Config string should contain UID version";
    EXPECT_TRUE(config_string.find("Host: test.collector.host") != std::string::npos)
        << "Config string should contain collector host";
    EXPECT_TRUE(config_string.find("Type: PERCENT") != std::string::npos) 
        << "Config string should contain sampling type";
    EXPECT_TRUE(config_string.find("Level: debug") != std::string::npos) 
        << "Config string should contain log level";
}

// Test round-trip configuration (string -> config -> string)
TEST_F(ConfigTest, ConfigurationRoundTripTest) {
    set_config_string(complete_config_yaml_);
    auto config = make_config();
    
    std::string generated_config_string = to_config_string(*config);
    
    // Use generated string as new config
    set_config_string(generated_config_string);
    auto config2 = make_config();
    
    // Both configs should have same values
    EXPECT_EQ(config->app_name_, config2->app_name_) << "App name should match after round-trip";
    EXPECT_EQ(config->collector.host, config2->collector.host) << "Collector host should match after round-trip";
    EXPECT_EQ(config->sampling.type, config2->sampling.type) << "Sampling type should match after round-trip";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, config2->sampling.percent_rate) << "Percent rate should match after round-trip";
    EXPECT_EQ(config->http.server.status_errors.size(), config2->http.server.status_errors.size()) 
        << "Status errors count should match after round-trip";
    EXPECT_EQ(config->uid_version_, config2->uid_version_) << "UID version should match after round-trip";
    EXPECT_EQ(config->agent_name_, config2->agent_name_)
        << "Explicit AgentName should match after round-trip";
    EXPECT_EQ(config->sql.max_bind_args_size, config2->sql.max_bind_args_size)
        << "Max bind args size should match after round-trip";
    EXPECT_EQ(config->sql.enable_sql_stats, config2->sql.enable_sql_stats)
        << "SQL stats enable should match after round-trip";
    EXPECT_EQ(config->sql.enable_raw_sql_cache, config2->sql.enable_raw_sql_cache)
        << "Raw SQL cache enable should match after round-trip";
    EXPECT_EQ(config->sql.trace_bind_value, config2->sql.trace_bind_value)
        << "SQL bind value tracing should match after round-trip";
}

// Runtime-generated identity must not become configuration input when a
// serialized config is loaded by a new agent. In particular, a defaulted
// AgentName must follow the newly generated AgentId instead of retaining the
// previous process's id as an explicit display name.
TEST_F(ConfigTest, GeneratedIdentityDoesNotLeakIntoRoundTripConfig) {
    set_config_string(R"(
ApplicationName: RoundTripIdentityApp
Collector:
  GrpcHost: localhost
)");
    auto first = make_config();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->agent_name_, first->agent_id_);

    const std::string generated_config = to_config_string(*first);
    EXPECT_EQ(generated_config.find("AgentId:"), std::string::npos);
    EXPECT_EQ(generated_config.find(first->agent_id_), std::string::npos)
        << "Serialized config must not contain the runtime-generated identity";

    set_config_string(generated_config);
    auto second = make_config();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second->agent_id_, first->agent_id_);
    EXPECT_EQ(second->agent_name_, second->agent_id_)
        << "Default AgentName must follow the new process's AgentId";
}

// Test empty configuration string generation
TEST_F(ConfigTest, EmptyConfigurationStringTest) {
    set_config_string("");
    auto config = make_config();
    
    std::string config_string = to_config_string(*config);
    
    // Should contain default values
    EXPECT_TRUE(config_string.find("Level: info") != std::string::npos)
        << "Config string should contain default log level";
    EXPECT_TRUE(config_string.find("Type: COUNTER") != std::string::npos) 
        << "Config string should contain default sampling type";
    EXPECT_TRUE(config_string.find("UidVersion:") != std::string::npos)
        << "Config string should contain default UID version field";
}

// UidVersion resolution at the config integration level: identity resolves,
// the raw string is preserved in uid_version_ (isReloadable compares the raw
// value, not the resolved version), and object_name_version_ follows the
// resolved version. An unrecognized value (anything not v1/v4, e.g. v2)
// resolves as v3 — object_name_version_ becomes VERSION_V1 (1) exactly like an
// explicit v3 — guarding the schema-drift / parse fallback beyond the
// parse_name_version unit test. Recognized versions additionally survive a
// to_config_string() round-trip.
TEST_F(ConfigTest, UidVersionResolutionAndRoundTripTest) {
    struct UidVersionCase {
        const char* version_str;
        int expected_object_name_version;
        bool round_trips;  // recognized versions also check to_config_string()
    };
    const UidVersionCase cases[] = {
        {"v1", 1, true},
        {"v3", 1, true},
        {"v2", 1, false},  // unrecognized: v3 fallback
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.version_str);
        set_config_string(std::string("ApplicationName: \"UidApp\"\n"
                                      "UidVersion: ") + c.version_str + "\n"
                                      "Collector:\n"
                                      "  GrpcHost: localhost\n");
        auto config = make_config();
        ASSERT_NE(config, nullptr);
        ASSERT_TRUE(config->identity_resolved_)
            << "identity must resolve even for an unrecognized uid version";
        EXPECT_EQ(config->uid_version_, c.version_str)
            << "the raw uid version string is preserved";
        EXPECT_EQ(config->object_name_version_, c.expected_object_name_version);

        if (!c.round_trips) continue;
        std::string config_string = to_config_string(*config);
        EXPECT_TRUE(config_string.find(std::string("UidVersion: ") + c.version_str)
                        != std::string::npos)
            << "Config string should contain the UID version";

        set_config_string(config_string);
        auto config2 = make_config();
        ASSERT_NE(config2, nullptr);
        EXPECT_EQ(config2->uid_version_, c.version_str);
        EXPECT_EQ(config2->object_name_version_, c.expected_object_name_version);
    }
}

// A well-formed YAML document whose section key carries the WRONG node shape
// (scalar or sequence where a map is expected) must degrade to defaults, never
// throw into the embedding application. find_node() guards non-map parents and
// make_config() wraps the load in a try/catch for anything a per-key getter misses.
TEST_F(ConfigTest, MalformedSectionShapeDegradesToDefaultsTest) {
    // Scalar where a map is expected.
    set_config_string(R"(
ApplicationName: "MalformedApp"
Sampling: "not-a-map"
Collector:
  GrpcHost: localhost
)");
    auto scalar_cfg = make_config();
    ASSERT_NE(scalar_cfg, nullptr) << "a scalar-shaped section must not crash make_config";
    EXPECT_EQ(scalar_cfg->sampling.type, "COUNTER") << "sampling degrades to its default type";
    EXPECT_EQ(scalar_cfg->sampling.counter_rate, 1) << "sampling degrades to its default rate";

    // Sequence where a map is expected.
    set_config_string(R"(
ApplicationName: "MalformedApp2"
Sampling:
  - 1
  - 2
Collector:
  GrpcHost: localhost
)");
    auto seq_cfg = make_config();
    ASSERT_NE(seq_cfg, nullptr) << "a sequence-shaped section must not crash make_config";
    EXPECT_EQ(seq_cfg->sampling.type, "COUNTER");

    // A whole top-level section reduced to a bare scalar degrades without throwing.
    set_config_string(R"(
ApplicationName: "MalformedApp3"
Collector: "localhost"
)");
    auto collector_cfg = make_config();
    ASSERT_NE(collector_cfg, nullptr) << "a scalar Collector section must not crash make_config";
    EXPECT_EQ(collector_cfg->collector.agent_port, 9991) << "collector fields degrade to defaults";
}

TEST_F(ConfigTest, UidVersionV4ConfigurationToStringMasksApiKeyTest) {
    set_config_string(R"(
ApplicationName: "UidV4App"
UidVersion: v4
ServiceName: "uid-v4-service"
ApiKey: "super-secret-key"
Collector:
  GrpcHost: localhost
)");
    auto config = make_config();
    ASSERT_NE(config, nullptr);
    ASSERT_TRUE(config->identity_resolved_);
    EXPECT_EQ(config->uid_version_, "v4");
    EXPECT_EQ(config->object_name_version_, 4);

    std::string config_string = to_config_string(*config);
    EXPECT_TRUE(config_string.find("UidVersion: v4") != std::string::npos)
        << "Config string should contain v4 UID version";
    EXPECT_TRUE(config_string.find("ServiceName: uid-v4-service") != std::string::npos)
        << "Config string should contain v4 service name";
    EXPECT_TRUE(config_string.find("****") != std::string::npos)
        << "Config string should mask v4 API key";
    EXPECT_TRUE(config_string.find("super-secret-key") == std::string::npos)
        << "Config string should not contain plaintext API key";
}

TEST_F(ConfigTest, NonDefaultConfigStringsTest) {
    Config config;
    EXPECT_TRUE(to_non_default_config_strings(config).empty())
        << "Default config should not produce config strings";

    config.log.level = "debug";
    config.span.max_event_depth = 32;
    config.http.url_stat.enable = true;
    config.sql.enable_sql_stats = true;
    config.sql.enable_raw_sql_cache = false;
    config.sql.trace_bind_value = false;
    config.sql.remove_comments = true;
    config.uid_version_ = "v4";

    const auto config_strings = to_non_default_config_strings(config);
    EXPECT_EQ(config_strings.size(), 8);

    auto contains_config = [&config_strings](const std::string& expected) {
        for (const auto& config_string : config_strings) {
            if (config_string == expected) {
                return true;
            }
        }
        return false;
    };

    EXPECT_TRUE(contains_config("UidVersion=v4"));
    EXPECT_TRUE(contains_config("Log.Level=debug"));
    EXPECT_TRUE(contains_config("Span.MaxEventDepth=32"));
    EXPECT_TRUE(contains_config("Http.CollectUrlStat=true"));
    EXPECT_TRUE(contains_config("Sql.EnableSqlStats=true"));
    EXPECT_TRUE(contains_config("Sql.EnableRawSqlCache=false"));
    EXPECT_TRUE(contains_config("Sql.TraceBindValue=false"));
    EXPECT_TRUE(contains_config("Sql.RemoveComments=true"));
}

// ========== Integration Tests ==========

TEST_F(ConfigTest, CompleteConfigurationFlowTest) {
    // Create config file
    std::string config_file = temp_dir_ + "/complete_config.yaml";
    std::ofstream file(config_file);
    file << complete_config_yaml_;
    file.close();
    
    // Set environment variables
    setenv(full_env(env::CONFIG_FILE).c_str(), config_file.c_str(), 1);
    setenv(full_env(env::APPLICATION_NAME).c_str(), "OverriddenApp", 1); // Should override file
    setenv(full_env(env::LOG_LEVEL).c_str(), "warn", 1); // Should override file
    
    auto config = make_config();
    
    // Environment variables should override file values
    EXPECT_EQ(config->app_name_, "OverriddenApp") << "Environment variable should override file app name";
    EXPECT_EQ(config->log.level, "warn") << "Environment variable should override file log level";
    
    // File values should be used where no environment variable exists
    EXPECT_EQ(config->collector.host, "test.collector.host") << "Collector host should come from file";
    EXPECT_EQ(config->agent_name_, "TestAgentName") << "Agent name should come from file";
    EXPECT_EQ(config->agent_id_.size(), 22u)
        << "the Agent ID is always auto-generated (base64 UUIDv7)";
}

// ========== Exception Handling Tests ==========

TEST_F(ConfigTest, TypeConversionExceptionHandlingTest) {
    // YAML with invalid type conversions
    std::string invalid_type_yaml = R"(
ApplicationName: ValidString
Enable: "invalid_bool"           # Should be bool, will use default
Collector:
  GrpcAgentPort: "not_a_port"    # Should be int, will use default
  GrpcHost: 123                  # Should be string, but numeric should work
Sampling:
  PercentRate: "not_a_double"    # Should be double, will use default
Http:
  Server:
    StatusCodeErrors: "not_an_array"  # Should be array, will use default
)";
    
    set_config_string(invalid_type_yaml);
    
    // Capture log output to verify error messages are logged
    testing::internal::CaptureStderr();
    
    auto config = make_config();
    
    std::string captured_output = testing::internal::GetCapturedStderr();
    
    // Valid conversions should work
    EXPECT_EQ(config->app_name_, "ValidString") << "Valid string should be parsed correctly";
    
    // Invalid conversions should use default values
    EXPECT_TRUE(config->enable) << "Invalid bool should use default value (true)";
    EXPECT_EQ(config->collector.agent_port, 9991) << "Invalid port should use default value";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 100.0) << "Invalid double should use default value";
    EXPECT_EQ(config->http.server.status_errors.size(), 1) << "Invalid array should use default value";
    EXPECT_EQ(config->http.server.status_errors[0], "5xx") << "Default array should contain '5xx'";
    
    // Note: We can't easily check stderr output in this test environment without additional setup
    // but the error messages should be logged for invalid conversions
}

TEST_F(ConfigTest, MixedValidInvalidConfigurationTest) {
    std::string mixed_yaml = R"(
ApplicationName: ValidApp
Enable: true                     # Valid bool
Collector:
  GrpcHost: valid.host.com       # Valid string
  GrpcAgentPort: "invalid_port"  # Invalid int, should use default
  GrpcSpanPort: 9999             # Valid int
Sampling:
  Type: PERCENT                  # Valid string
  PercentRate: 50.5              # Valid double
  CounterRate: "not_a_number"    # Invalid int, should use default
Span:
  QueueSize: 2048                # Valid int
  MaxEventDepth: "invalid"       # Invalid int, should use default
)";
    
    set_config_string(mixed_yaml);
    auto config = make_config();
    
    // Valid values should be parsed correctly
    EXPECT_EQ(config->app_name_, "ValidApp") << "Valid app name should be parsed";
    EXPECT_TRUE(config->enable) << "Valid enable should be parsed";
    EXPECT_EQ(config->collector.host, "valid.host.com") << "Valid host should be parsed";
    EXPECT_EQ(config->collector.span_port, 9999) << "Valid span port should be parsed";
    EXPECT_EQ(config->sampling.type, "PERCENT") << "Valid sampling type should be parsed";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 50.5) << "Valid percent rate should be parsed";
    EXPECT_EQ(config->span.queue_size, 2048) << "Valid queue size should be parsed";
    
    // Invalid values should use defaults
    EXPECT_EQ(config->collector.agent_port, 9991) << "Invalid agent port should use default";
    EXPECT_EQ(config->sampling.counter_rate, 1) << "Invalid counter rate should use default";
    EXPECT_EQ(config->span.max_event_depth, 64) << "Invalid max event depth should use default";
}

// ========== Environment Variable Validation Tests ==========

// Test environment variable validation for invalid values
TEST_F(ConfigTest, EnvironmentVariableValidationTest) {
    // Set invalid environment variables
    setenv(full_env(env::ENABLE).c_str(), "invalid_bool", 1);                  // Invalid bool
    setenv(full_env(env::GRPC_AGENT_PORT).c_str(), "invalid_port", 1);         // Invalid int
    setenv(full_env(env::SAMPLING_PERCENT_RATE).c_str(), "not_a_double", 1);   // Invalid double
    setenv(full_env(env::STAT_ENABLE).c_str(), "maybe", 1);                    // Invalid bool
    setenv(full_env(env::SPAN_QUEUE_SIZE).c_str(), "abc", 1);                  // Invalid int
    
    auto config = make_config();
    
    // All invalid values should use defaults
    EXPECT_TRUE(config->enable) << "Invalid bool should use default value (true)";
    EXPECT_EQ(config->collector.agent_port, 9991) << "Invalid port should use default value";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 100.0) << "Invalid double should use default value";
    EXPECT_TRUE(config->stat.enable) << "Invalid bool should use default value (true)";
    EXPECT_EQ(config->span.queue_size, 1024) << "Invalid int should use default value";
}

TEST_F(ConfigTest, EnvironmentVariableValidValuesTest) {
    // Set valid environment variables
    setenv(full_env(env::ENABLE).c_str(), "false", 1);                  // Valid bool
    setenv(full_env(env::GRPC_AGENT_PORT).c_str(), "8080", 1);          // Valid int
    setenv(full_env(env::SAMPLING_PERCENT_RATE).c_str(), "75.5", 1);    // Valid double
    setenv(full_env(env::STAT_ENABLE).c_str(), "1", 1);                 // Valid bool
    setenv(full_env(env::SPAN_QUEUE_SIZE).c_str(), "2048", 1);          // Valid int
    
    auto config = make_config();
    
    // All valid values should be parsed correctly
    EXPECT_FALSE(config->enable) << "Valid bool should be parsed correctly";
    EXPECT_EQ(config->collector.agent_port, 8080) << "Valid port should be parsed correctly";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 75.5) << "Valid double should be parsed correctly";
    EXPECT_TRUE(config->stat.enable) << "Valid bool should be parsed correctly";
    EXPECT_EQ(config->span.queue_size, 2048) << "Valid int should be parsed correctly";
}

// Test environment variable validation for negative values
TEST_F(ConfigTest, EnvironmentVariableNegativeValuesTest) {
    // Set valid negative values where applicable
    setenv(full_env(env::SPAN_MAX_EVENT_DEPTH).c_str(), "-1", 1);       // Valid -1 (should be processed by make_config validation)
    setenv(full_env(env::SPAN_MAX_EVENT_SEQUENCE).c_str(), "-1", 1);    // Valid -1 (should be processed by make_config validation)
    
    auto config = make_config();
    
    // These should be parsed as -1 and then validated by make_config to INT32_MAX
    EXPECT_EQ(config->span.max_event_depth, INT32_MAX) << "-1 should be converted to INT32_MAX by make_config";
    EXPECT_EQ(config->span.max_event_sequence, INT32_MAX) << "-1 should be converted to INT32_MAX by make_config";
}

// ========== SQL Configuration Specific Tests ==========

// Test SQL configuration with various bind args sizes
TEST_F(ConfigTest, SqlMaxBindArgsSizeValidationTest) {
    // Test default
    auto config1 = make_config();
    EXPECT_EQ(config1->sql.max_bind_args_size, 1024) << "Default max bind args size should be 1024";
    
    // Test YAML configuration with different values
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 256
  EnableSqlStats: true
)");
    auto config2 = make_config();
    EXPECT_EQ(config2->sql.max_bind_args_size, 256) << "Max bind args size should match YAML";
    EXPECT_TRUE(config2->sql.enable_sql_stats) << "SQL stats should be enabled as per YAML";
    
    // Test environment variable override
    setenv(full_env(env::SQL_MAX_BIND_ARGS_SIZE).c_str(), "16384", 1);
    auto config3 = make_config();
    EXPECT_EQ(config3->sql.max_bind_args_size, 16384) << "Environment variable should override YAML";
}

// Test SQL stats enable/disable configurations  
TEST_F(ConfigTest, SqlStatsEnableTest) {
    // Test default (disabled)
    auto config1 = make_config();
    EXPECT_FALSE(config1->sql.enable_sql_stats) << "SQL stats should be disabled by default";
    
    // Test enabling via YAML
    set_config_string(R"(
Sql:
  EnableSqlStats: true
)");
    auto config2 = make_config();
    EXPECT_TRUE(config2->sql.enable_sql_stats) << "SQL stats should be enabled as per YAML";
    
    // Test enabling via environment variable
    set_config_string("");
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "true", 1);
    auto config3 = make_config();
    EXPECT_TRUE(config3->sql.enable_sql_stats) << "SQL stats should be enabled as per environment variable";
    
    // Test disabling via environment variable  
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "false", 1);
    auto config4 = make_config();
    EXPECT_FALSE(config4->sql.enable_sql_stats) << "SQL stats should be disabled as per environment variable";
}

TEST_F(ConfigTest, SqlTraceBindValueTest) {
    auto default_config = make_config();
    EXPECT_TRUE(default_config->sql.trace_bind_value)
        << "SQL bind value tracing should be enabled by default";

    set_config_string(R"(
Sql:
  TraceBindValue: false
)");
    auto yaml_config = make_config();
    EXPECT_FALSE(yaml_config->sql.trace_bind_value)
        << "SQL bind value tracing should be disabled by YAML";

    set_config_string("");
    setenv(full_env(env::SQL_TRACE_BIND_VALUE).c_str(), "false", 1);
    auto env_config = make_config();
    EXPECT_FALSE(env_config->sql.trace_bind_value)
        << "SQL bind value tracing should be disabled by environment variable";
}

TEST_F(ConfigTest, SqlRemoveCommentsTest) {
    auto default_config = make_config();
    EXPECT_FALSE(default_config->sql.remove_comments)
        << "SQL comments should be kept by default (Java agent parity)";

    set_config_string(R"(
Sql:
  RemoveComments: true
)");
    auto yaml_config = make_config();
    EXPECT_TRUE(yaml_config->sql.remove_comments)
        << "SQL comment removal should be enabled by YAML";

    set_config_string("");
    setenv(full_env(env::SQL_REMOVE_COMMENTS).c_str(), "true", 1);
    auto env_config = make_config();
    EXPECT_TRUE(env_config->sql.remove_comments)
        << "SQL comment removal should be enabled by environment variable";
}

TEST_F(ConfigTest, SqlRawSqlCacheTest) {
    auto default_config = make_config();
    EXPECT_TRUE(default_config->sql.enable_raw_sql_cache)
        << "Raw SQL cache should be enabled by default";

    set_config_string(R"(
Sql:
  EnableRawSqlCache: false
)");
    auto yaml_config = make_config();
    EXPECT_FALSE(yaml_config->sql.enable_raw_sql_cache)
        << "Raw SQL cache should be disabled by YAML";

    set_config_string("");
    setenv(full_env(env::SQL_ENABLE_RAW_SQL_CACHE).c_str(), "false", 1);
    auto env_config = make_config();
    EXPECT_FALSE(env_config->sql.enable_raw_sql_cache)
        << "Raw SQL cache should be disabled by environment variable";
}

TEST_F(ConfigTest, SqlCacheLengthLimitTest) {
    auto default_config = make_config();
    EXPECT_EQ(default_config->sql.cache_length_limit, 2048)
        << "SQL cache length limit should default to the Java agent's 2048";

    set_config_string(R"(
Sql:
  CacheLengthLimit: 4096
)");
    auto yaml_config = make_config();
    EXPECT_EQ(yaml_config->sql.cache_length_limit, 4096)
        << "SQL cache length limit should be read from YAML";

    set_config_string("");
    setenv(full_env(env::SQL_CACHE_LENGTH_LIMIT).c_str(), "512", 1);
    auto env_config = make_config();
    EXPECT_EQ(env_config->sql.cache_length_limit, 512)
        << "SQL cache length limit should be read from the environment";

    // -1 disables the bypass; anything below it would cast to a huge size_t
    // at the use site and disable it silently, so it falls back to the default.
    unsetenv(full_env(env::SQL_CACHE_LENGTH_LIMIT).c_str());
    set_config_string(R"(
Sql:
  CacheLengthLimit: -1
)");
    EXPECT_EQ(make_config()->sql.cache_length_limit, -1)
        << "-1 must survive as the 'cache everything' sentinel";

    set_config_string(R"(
Sql:
  CacheLengthLimit: -2
)");
    EXPECT_EQ(make_config()->sql.cache_length_limit, 2048)
        << "Out-of-range negative limit should fall back to the default";
}

TEST_F(ConfigTest, SqlConfigurationEdgeCasesTest) {
    // Test negative bind args size
    set_config_string(R"(
Sql:
  MaxBindArgsSize: -1
)");
    auto negative_config = make_config();
    EXPECT_EQ(negative_config->sql.max_bind_args_size, 0)
        << "Negative bind args size should be clamped to zero";

    // Test zero bind args size
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 0
  EnableSqlStats: false
)");
    auto config1 = make_config();
    EXPECT_EQ(config1->sql.max_bind_args_size, 0) << "Zero bind args size should be allowed";
    EXPECT_FALSE(config1->sql.enable_sql_stats) << "SQL stats should be disabled";
    
    // Test very large bind args size
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 1048576
  EnableSqlStats: true
)");
    auto config2 = make_config();
    EXPECT_EQ(config2->sql.max_bind_args_size, 1048576) << "Large bind args size should be allowed";
    EXPECT_TRUE(config2->sql.enable_sql_stats) << "SQL stats should be enabled";

    // Test negative bind args size from the environment
    set_config_string("");
    setenv(full_env(env::SQL_MAX_BIND_ARGS_SIZE).c_str(), "-1", 1);
    auto negative_env_config = make_config();
    EXPECT_EQ(negative_env_config->sql.max_bind_args_size, 0)
        << "Negative bind args size from the environment should be clamped to zero";
}

// Test SQL configuration string generation
TEST_F(ConfigTest, SqlConfigurationToStringTest) {
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 2048
  EnableSqlStats: true
  EnableRawSqlCache: true
  TraceBindValue: true
)");
    auto config = make_config();
    
    std::string config_string = to_config_string(*config);
    
    // Check that SQL configuration is included in generated string
    EXPECT_TRUE(config_string.find("MaxBindArgsSize: 2048") != std::string::npos) 
        << "Config string should contain SQL max bind args size";
    EXPECT_TRUE(config_string.find("EnableSqlStats: true") != std::string::npos) 
        << "Config string should contain SQL stats enable setting";
    EXPECT_TRUE(config_string.find("EnableRawSqlCache: true") != std::string::npos)
        << "Config string should contain raw SQL cache enable setting";
    EXPECT_TRUE(config_string.find("TraceBindValue: true") != std::string::npos)
        << "Config string should contain SQL bind value tracing setting";
}

// Test invalid SQL environment variable values
TEST_F(ConfigTest, SqlInvalidEnvironmentVariableTest) {
    // Test invalid max bind args size (should fallback to default)
    setenv(full_env(env::SQL_MAX_BIND_ARGS_SIZE).c_str(), "invalid", 1);
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "invalid", 1);
    setenv(full_env(env::SQL_TRACE_BIND_VALUE).c_str(), "invalid", 1);
    
    auto config = make_config();
    
    // Should fallback to defaults when environment variable is invalid
    EXPECT_EQ(config->sql.max_bind_args_size, 1024) << "Should use default when env var is invalid";
    EXPECT_FALSE(config->sql.enable_sql_stats) << "Should use default when env var is invalid";
    EXPECT_TRUE(config->sql.trace_bind_value) << "Should use default when env var is invalid";
}

// ========== Boolean Config Key Tests ==========

// Every boolean key flows through the same table-driven loaders, so each key
// is exercised once per source/override shape here instead of in per-key test
// families.
TEST_F(ConfigTest, BooleanConfigKeysTest) {
    struct BoolKey {
        const char* section;            // enclosing YAML section ("" = top level)
        const char* key;                // YAML leaf key
        const char* env_suffix;         // env:: variable name suffix
        bool (*get)(const Config&);
        bool default_value;
        bool fixed_default;             // false: auto-detected default (IsContainer)
        const char* config_string_key;  // nullptr: not a unique to_config_string token
    };
    const BoolKey keys[] = {
        {"", "Enable", env::ENABLE,
         [](const Config& c) { return c.enable; }, true, true, nullptr},
        {"", "IsContainer", env::IS_CONTAINER,
         [](const Config& c) { return c.is_container; }, false, false, "IsContainer"},
        {"", "EnableCallstackTrace", env::ENABLE_CALLSTACK_TRACE,
         [](const Config& c) { return c.enable_callstack_trace; }, false, true, "EnableCallstackTrace"},
        {"", "EnableConfigFileWatcher", env::ENABLE_CONFIG_FILE_WATCHER,
         [](const Config& c) { return c.enable_config_file_watcher; }, false, true, "EnableConfigFileWatcher"},
        {"Stat", "Enable", env::STAT_ENABLE,
         [](const Config& c) { return c.stat.enable; }, true, true, nullptr},
        {"Http", "CollectUrlStat", env::HTTP_COLLECT_URL_STAT,
         [](const Config& c) { return c.http.url_stat.enable; }, false, true, "CollectUrlStat"},
        {"Http", "UrlStatEnableTrimPath", env::HTTP_URL_STAT_ENABLE_TRIM_PATH,
         [](const Config& c) { return c.http.url_stat.enable_trim_path; }, true, true, "UrlStatEnableTrimPath"},
    };

    const auto yaml_for = [](const BoolKey& k, const char* value) {
        const std::string nesting = *k.section ? std::string(k.section) + ":\n  " : "";
        return nesting + k.key + ": " + value + "\n";
    };

    for (const auto& k : keys) {
        SCOPED_TRACE(k.env_suffix);
        const std::string env_name = full_env(k.env_suffix);
        const char* default_text = k.default_value ? "true" : "false";
        const char* non_default_text = k.default_value ? "false" : "true";

        if (k.fixed_default) {
            set_config_string("");
            auto config = make_config();
            EXPECT_EQ(k.get(*config), k.default_value) << "default value";
            if (k.config_string_key) {
                const std::string entry = std::string(k.config_string_key) + ": " + default_text;
                EXPECT_TRUE(to_config_string(*config).find(entry) != std::string::npos)
                    << "default value should appear in to_config_string()";
            }
        }

        set_config_string(yaml_for(k, "true"));
        EXPECT_TRUE(k.get(*make_config())) << "YAML true";
        set_config_string(yaml_for(k, "false"));
        EXPECT_FALSE(k.get(*make_config())) << "YAML false";

        set_config_string("");
        setenv(env_name.c_str(), "true", 1);
        EXPECT_TRUE(k.get(*make_config())) << "environment variable true";
        setenv(env_name.c_str(), "false", 1);
        EXPECT_FALSE(k.get(*make_config())) << "environment variable false";

        set_config_string(yaml_for(k, "false"));
        setenv(env_name.c_str(), "true", 1);
        EXPECT_TRUE(k.get(*make_config())) << "environment variable true should override YAML";
        set_config_string(yaml_for(k, "true"));
        setenv(env_name.c_str(), "false", 1);
        EXPECT_FALSE(k.get(*make_config())) << "environment variable false should override YAML";

        if (k.fixed_default) {
            set_config_string("");
            setenv(env_name.c_str(), "invalid_bool", 1);
            EXPECT_EQ(k.get(*make_config()), k.default_value)
                << "invalid environment value should fall back to the default";
        }
        unsetenv(env_name.c_str());

        if (k.config_string_key) {
            set_config_string(yaml_for(k, non_default_text));
            auto config = make_config();
            const std::string config_string = to_config_string(*config);
            EXPECT_TRUE(config_string.find(std::string(k.config_string_key) + ": " + non_default_text)
                            != std::string::npos)
                << "non-default value should appear in to_config_string()";
            set_config_string(config_string);
            EXPECT_EQ(k.get(*make_config()), !k.default_value)
                << "value should survive a to_config_string() round-trip";
        }
    }
}

// The boolean value parsers are shared by every key, so the accepted spellings
// are walked once: YAML spellings against UrlStatEnableTrimPath (default true,
// making a parsed false observable) and environment spellings against
// EnableCallstackTrace (default false, making a parsed true observable).
// Unparseable spellings fall back to the key's default.
TEST_F(ConfigTest, BooleanValueSpellingsTest) {
    struct Spelling { const char* text; bool expected; };

    const Spelling yaml_spellings[] = {
        {"true", true}, {"false", false},
        {"TRUE", true}, {"FALSE", false},
        {"yes", true},  {"no", false},
        {"not_a_boolean", true},  // invalid type -> default
    };
    for (const auto& s : yaml_spellings) {
        SCOPED_TRACE(std::string("yaml: ") + s.text);
        set_config_string(std::string("Http:\n  UrlStatEnableTrimPath: ") + s.text + "\n");
        EXPECT_EQ(make_config()->http.url_stat.enable_trim_path, s.expected);
    }

    set_config_string("");
    const std::string env_name = full_env(env::ENABLE_CALLSTACK_TRACE);
    const Spelling env_spellings[] = {
        {"true", true}, {"false", false},
        {"TRUE", true}, {"False", false},
        {"1", true},    {"0", false},
        {"yes", true},  {"no", false}, {"NO", false},
        {"invalid_bool", false},  // invalid value -> default
    };
    for (const auto& s : env_spellings) {
        SCOPED_TRACE(std::string("env: ") + s.text);
        setenv(env_name.c_str(), s.text, 1);
        EXPECT_EQ(make_config()->enable_callstack_trace, s.expected);
    }
    unsetenv(env_name.c_str());
}

// ========== Config File Watcher Configuration Tests ==========

// The toggle is consumed once at Start() (the watcher either was or was not
// installed), so a reload must never flip it — in either direction.
TEST_F(ConfigTest, ConfigFileWatcherToggleIsNonReloadable) {
    set_config_string(R"(
ApplicationName: "WatcherApp"
EnableConfigFileWatcher: true
)");
    auto enabled = make_config();
    ASSERT_NE(enabled, nullptr);
    ASSERT_TRUE(enabled->enable_config_file_watcher);

    set_config_string(R"(
ApplicationName: "WatcherApp"
EnableConfigFileWatcher: false
)");
    auto reloaded_off = make_config(enabled);
    ASSERT_NE(reloaded_off, nullptr);
    EXPECT_TRUE(reloaded_off->enable_config_file_watcher)
        << "Reload must retain the running watcher toggle (true)";

    set_config_string(R"(
ApplicationName: "WatcherApp"
)");
    auto disabled = make_config();
    ASSERT_NE(disabled, nullptr);
    ASSERT_FALSE(disabled->enable_config_file_watcher);

    set_config_string(R"(
ApplicationName: "WatcherApp"
EnableConfigFileWatcher: true
)");
    auto reloaded_on = make_config(disabled);
    ASSERT_NE(reloaded_on, nullptr);
    EXPECT_FALSE(reloaded_on->enable_config_file_watcher)
        << "Reload must retain the running watcher toggle (false)";
}

// ========== Config::check() Validation Tests ==========

TEST_F(ConfigTest, CheckPassesWithValidConfigTest) {
    Config config;
    config.collector.host = "localhost";
    config.app_name_ = "MyApp";
    config.agent_id_ = "agent-001";
    config.agent_name_ = "AgentOne";

    EXPECT_TRUE(config.check()) << "Valid config should pass check";
}

TEST_F(ConfigTest, CheckFailsEmptyCollectorHostTest) {
    Config config;
    config.collector.host = "";
    config.app_name_ = "MyApp";
    config.agent_id_ = "agent-001";

    EXPECT_FALSE(config.check()) << "Empty collector host should fail check";
}

TEST_F(ConfigTest, CheckFailsEmptyAppNameTest) {
    Config config;
    config.collector.host = "localhost";
    config.app_name_ = "";

    EXPECT_FALSE(config.check()) << "Empty app name should fail check";
}

// Test check() fails when agent identity resolution failed.
// Per-version identity length/charset validation now lives in
// resolve_object_name() (version-aware: e.g. applicationName <=24 for v1 vs
// <=254 for v3); make_config() records the outcome in identity_resolved_.
// See test_object_name.cpp for the boundary-value coverage.
TEST_F(ConfigTest, CheckFailsWhenIdentityUnresolvedTest) {
    Config config;
    config.collector.host = "localhost";
    config.app_name_ = "MyApp";
    config.agent_id_ = "agent-001";
    config.identity_resolved_ = false;

    EXPECT_FALSE(config.check()) << "Unresolved identity should fail check";

    config.identity_resolved_ = true;
    EXPECT_TRUE(config.check()) << "Resolved identity should pass check";
}

// Test make_config() flags an over-length v1 application name as unresolved so
// that check() (and therefore agent startup) fails.
TEST_F(ConfigTest, MakeConfigV1RejectsLongApplicationNameTest) {
    const std::string yaml =
        "ApplicationName: " + std::string(25, 'A') + "\n"
        "UidVersion: v1\n"
        "Collector:\n"
        "  GrpcHost: localhost\n";
    set_config_string(yaml);

    auto config = make_config();
    ASSERT_NE(config, nullptr);
    EXPECT_FALSE(config->identity_resolved_)
        << "v1 application name > 24 chars should not resolve";
    EXPECT_FALSE(config->check());
}

// Test make_config() accepts a long application name under the v3 default (<=254).
TEST_F(ConfigTest, MakeConfigV3AllowsLongApplicationNameTest) {
    const std::string yaml =
        "ApplicationName: " + std::string(100, 'A') + "\n"
        "Collector:\n"
        "  GrpcHost: localhost\n";
    set_config_string(yaml);

    auto config = make_config();
    ASSERT_NE(config, nullptr);
    EXPECT_TRUE(config->identity_resolved_)
        << "v3 (default) application name <=254 chars should resolve";
    EXPECT_EQ(config->object_name_version_, 1);
    EXPECT_TRUE(config->check());
}

// Test make_config() resolves a full v4 identity and rejects missing requirements.
TEST_F(ConfigTest, MakeConfigV4IdentityTest) {
    const std::string base =
        "ApplicationName: my-app\n"
        "UidVersion: v4\n"
        "Collector:\n"
        "  GrpcHost: localhost\n";

    // Missing apiKey -> unresolved (serviceName alone defaults, see below).
    set_config_string(base);
    auto missing = make_config();
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->object_name_version_, 4);
    EXPECT_FALSE(missing->identity_resolved_);
    EXPECT_FALSE(missing->check());

    // Missing serviceName -> resolved with the "DEFAULT" fallback.
    set_config_string(base + "ApiKey: secret\n");
    auto defaulted = make_config();
    ASSERT_NE(defaulted, nullptr);
    EXPECT_TRUE(defaulted->identity_resolved_);
    EXPECT_EQ(defaulted->service_name_, "DEFAULT");
    EXPECT_TRUE(defaulted->check());

    // Full v4 identity -> resolved, agent id is a 22-char base64 UUID.
    set_config_string(base + "ServiceName: my-service\nApiKey: secret\n");
    auto full = make_config();
    ASSERT_NE(full, nullptr);
    EXPECT_TRUE(full->identity_resolved_);
    EXPECT_EQ(full->object_name_version_, 4);
    EXPECT_EQ(full->protocol_version(), 400);
    EXPECT_TRUE(full->is_v4());
    EXPECT_EQ(full->service_name_, "my-service");
    EXPECT_EQ(full->agent_id_.size(), 22u);
    EXPECT_TRUE(full->check());
}

// Regression: a v4 reload must carry the running (auto-generated) agent id
// through identity resolution instead of minting a fresh UUID. A fresh id
// would make isReloadable() false inside retainNonReloadableFrom() and log a
// spurious "non-reloadable config fields changed" warning on every reload,
// even when the config file did not change at all.
TEST_F(ConfigTest, MakeConfigV4ReloadKeepsAgentIdWithoutWarning) {
    set_config_string(
        "ApplicationName: v4-reload-app\n"
        "UidVersion: v4\n"
        "ServiceName: v4-service\n"
        "ApiKey: v4-key\n"
        "Collector:\n"
        "  GrpcHost: localhost\n");
    auto first = make_config();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->identity_resolved_);
    const std::string running_id = first->agent_id_;
    ASSERT_EQ(running_id.size(), 22u);

    // The agent logs to stdout when no file sink is configured.
    testing::internal::CaptureStdout();
    auto reloaded = make_config(first);
    const std::string log_output = testing::internal::GetCapturedStdout();

    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->agent_id_, running_id)
        << "a v4 reload must keep the running agent id";
    EXPECT_TRUE(reloaded->isReloadable(first))
        << "an unchanged v4 reload must not differ in non-reloadable fields";
    EXPECT_EQ(log_output.find("non-reloadable config fields changed"), std::string::npos)
        << "an unchanged v4 reload must not warn about non-reloadable changes; log:\n"
        << log_output;
}

// ========== Config::isReloadable() Tests ==========

TEST_F(ConfigTest, IsReloadableReturnsTrueWhenCriticalFieldsMatchTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->agent_id_ = "agent-001";
    old_config->agent_name_ = "Agent";
    old_config->collector.host = "localhost";
    old_config->collector.agent_port = 9991;
    old_config->collector.span_port = 9993;
    old_config->collector.stat_port = 9992;

    Config new_config = *old_config;
    // Change non-critical fields
    new_config.log.level = "debug";
    new_config.sampling.percent_rate = 50.0;
    new_config.enable_callstack_trace = true;

    EXPECT_TRUE(new_config.isReloadable(old_config))
        << "Should be reloadable when only non-critical fields change";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenAppNameChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "OldApp";
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.app_name_ = "NewApp";

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when app name changes";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenRawUidVersionChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->uid_version_ = "v1";
    old_config->object_name_version_ = 1;
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.uid_version_ = "v3";

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when raw UID version changes";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenUidVersionChangesFromDefaultRawValueTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->uid_version_ = "";
    old_config->object_name_version_ = 1;
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.uid_version_ = "v3";

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when UID version is explicitly changed";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenCollectorHostChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "old.host";

    Config new_config = *old_config;
    new_config.collector.host = "new.host";

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when collector host changes";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenPortChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";
    old_config->collector.agent_port = 9991;

    Config new_config = *old_config;
    new_config.collector.agent_port = 8888;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when agent port changes";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenGrpcChannelOptionsChangeTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.collector.grpc.ssl.enable = true;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when gRPC channel options change";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenStatOptionsChangeTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.stat.enable = !old_config->stat.enable;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when stat options change";
}

// Test isReloadable() returns false when http url_stat options change
TEST_F(ConfigTest, IsReloadableReturnsFalseWhenUrlStatOptionsChangeTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.http.url_stat.enable = !old_config->http.url_stat.enable;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when http url_stat options change";
}

// Test isReloadable() returns false when collector agent_info options change
TEST_F(ConfigTest, IsReloadableReturnsFalseWhenCollectorAgentInfoChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.collector.agent_info.refresh_interval_ms += 1000;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when collector agent_info options change";
}

// Test isReloadable() returns false when collector span_batch options change
TEST_F(ConfigTest, IsReloadableReturnsFalseWhenCollectorSpanBatchChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.collector.span_batch.size += 5;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when collector span_batch options change";
}

TEST_F(ConfigTest, IsReloadableReturnsFalseWhenSpanQueueSizeChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";
    old_config->span.queue_size = 1024;

    Config new_config = *old_config;
    new_config.span.queue_size = 2048;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when span queue size changes";
}

TEST_F(ConfigTest, IsReloadableReturnsTrueWhenOldConfigIsNullTest) {
    Config new_config;
    new_config.app_name_ = "MyApp";

    EXPECT_TRUE(new_config.isReloadable(nullptr))
        << "Should be reloadable when old config is null";
}

// ========== Config::retainNonReloadableFrom() Tests ==========

// Test retainNonReloadableFrom() overwrites every non-reloadable field with the
// running config's value, making the two configs reloadable-equivalent.
TEST_F(ConfigTest, RetainNonReloadableFromCopiesNonReloadableFieldsTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "OldApp";
    old_config->agent_id_ = "old-agent-id";
    old_config->agent_name_ = "old-agent-name";
    old_config->uid_version_ = "v1";
    old_config->service_name_ = "old-service";
    old_config->api_key_ = "old-key";
    old_config->object_name_version_ = 1;
    old_config->identity_resolved_ = true;
    old_config->collector.host = "old.host";
    old_config->collector.agent_port = 9991;
    old_config->collector.span_port = 9993;
    old_config->collector.stat_port = 9992;
    old_config->collector.grpc.ssl.enable = true;
    old_config->collector.grpc.ssl.trust_cert_file_path = "/old/trust.pem";

    Config new_config;
    new_config.app_name_ = "NewApp";
    new_config.agent_id_ = "new-agent-id";
    new_config.agent_name_ = "new-agent-name";
    new_config.uid_version_ = "v4";
    new_config.service_name_ = "new-service";
    new_config.api_key_ = "new-key";
    new_config.object_name_version_ = 4;
    new_config.identity_resolved_ = false;
    new_config.collector.host = "new.host";
    new_config.collector.agent_port = 8888;
    new_config.collector.grpc.ssl.enable = false;
    new_config.collector.grpc.ssl.trust_cert_file_path = "/new/trust.pem";

    new_config.retainNonReloadableFrom(old_config);

    EXPECT_EQ(new_config.app_name_, "OldApp");
    EXPECT_EQ(new_config.agent_id_, "old-agent-id");
    EXPECT_EQ(new_config.agent_name_, "old-agent-name");
    EXPECT_EQ(new_config.uid_version_, "v1");
    EXPECT_EQ(new_config.service_name_, "old-service");
    EXPECT_EQ(new_config.api_key_, "old-key");
    EXPECT_EQ(new_config.object_name_version_, 1);
    EXPECT_TRUE(new_config.identity_resolved_);
    EXPECT_EQ(new_config.collector.host, "old.host");
    EXPECT_EQ(new_config.collector.agent_port, 9991);
    EXPECT_EQ(new_config.collector.span_port, 9993);
    EXPECT_EQ(new_config.collector.stat_port, 9992);
    EXPECT_TRUE(new_config.collector.grpc.ssl.enable);
    EXPECT_EQ(new_config.collector.grpc.ssl.trust_cert_file_path, "/old/trust.pem");

    // After retaining, all non-reloadable fields match the running config.
    EXPECT_TRUE(new_config.isReloadable(old_config))
        << "retainNonReloadableFrom should leave the configs reloadable-equivalent";
}

// Test retainNonReloadableFrom() leaves reloadable fields untouched, so a reload
// still applies them while only reverting the non-reloadable ones.
TEST_F(ConfigTest, RetainNonReloadableFromPreservesReloadableFieldsTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "OldApp";
    old_config->collector.host = "old.host";
    old_config->sampling.counter_rate = 1;
    old_config->log.level = "info";
    old_config->http.server.exclude_url = {"/old"};

    Config new_config = *old_config;
    // Reloadable changes that must survive the retain.
    new_config.sampling.counter_rate = 99;
    new_config.log.level = "debug";
    new_config.http.server.exclude_url = {"/new1", "/new2"};
    // Non-reloadable change that must be reverted to the running value.
    new_config.collector.host = "new.host";

    new_config.retainNonReloadableFrom(old_config);

    EXPECT_EQ(new_config.sampling.counter_rate, 99);
    EXPECT_EQ(new_config.log.level, "debug");
    EXPECT_EQ(new_config.http.server.exclude_url,
              (std::vector<std::string>{"/new1", "/new2"}));
    EXPECT_EQ(new_config.collector.host, "old.host");
}

// Test retainNonReloadableFrom() reverts stat and http url_stat (non-reloadable)
// to the running values while leaving http.server (reloadable) changes in place.
TEST_F(ConfigTest, RetainNonReloadableFromRevertsStatAndUrlStatTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";
    old_config->stat.enable = true;
    old_config->stat.batch_count = 6;
    old_config->stat.collect_interval = 5000;
    old_config->http.url_stat.enable = false;
    old_config->http.url_stat.limit = 1024;
    old_config->http.url_stat.trim_path_depth = 1;

    Config new_config = *old_config;
    // Non-reloadable changes that must be reverted to the running values.
    new_config.stat.enable = false;
    new_config.stat.batch_count = 42;
    new_config.stat.collect_interval = 12000;
    new_config.http.url_stat.enable = true;
    new_config.http.url_stat.limit = 4096;
    new_config.http.url_stat.trim_path_depth = 5;
    // Reloadable http.server change that must survive the retain.
    new_config.http.server.exclude_url = {"/new"};

    new_config.retainNonReloadableFrom(old_config);

    EXPECT_TRUE(new_config.stat.enable);
    EXPECT_EQ(new_config.stat.batch_count, 6);
    EXPECT_EQ(new_config.stat.collect_interval, 5000);
    EXPECT_FALSE(new_config.http.url_stat.enable);
    EXPECT_EQ(new_config.http.url_stat.limit, 1024);
    EXPECT_EQ(new_config.http.url_stat.trim_path_depth, 1);
    EXPECT_EQ(new_config.http.server.exclude_url,
              (std::vector<std::string>{"/new"}));

    EXPECT_TRUE(new_config.isReloadable(old_config))
        << "retainNonReloadableFrom should leave the configs reloadable-equivalent";
}

// Test retainNonReloadableFrom() reverts span.queue_size (non-reloadable) to the
// running value while leaving the other span fields (reloadable) in place.
TEST_F(ConfigTest, RetainNonReloadableFromRevertsSpanQueueSizeTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";
    old_config->span.queue_size = 1024;
    old_config->span.max_event_depth = 64;

    Config new_config = *old_config;
    // Non-reloadable change that must be reverted to the running value.
    new_config.span.queue_size = 4096;
    // Reloadable span change that must survive the retain.
    new_config.span.max_event_depth = 128;

    new_config.retainNonReloadableFrom(old_config);

    EXPECT_EQ(new_config.span.queue_size, 1024u);
    EXPECT_EQ(new_config.span.max_event_depth, 128);

    EXPECT_TRUE(new_config.isReloadable(old_config))
        << "retainNonReloadableFrom should leave the configs reloadable-equivalent";
}

// Test retainNonReloadableFrom(nullptr) is a no-op (initial-build path has no
// running config to retain from).
TEST_F(ConfigTest, RetainNonReloadableFromNullOldIsNoopTest) {
    Config new_config;
    new_config.app_name_ = "NewApp";
    new_config.collector.host = "new.host";
    new_config.sampling.counter_rate = 42;

    new_config.retainNonReloadableFrom(nullptr);

    EXPECT_EQ(new_config.app_name_, "NewApp");
    EXPECT_EQ(new_config.collector.host, "new.host");
    EXPECT_EQ(new_config.sampling.counter_rate, 42);
}

// ========== Environment Variable Prefix Tests ==========

// Test a custom prefix makes make_config read <prefix>_<suffix> instead of the
// default PINPOINT_CPP_ names.
TEST_F(ConfigTest, EnvVarPrefixCustomTest) {
    set_env_prefix("MYAPP");
    setenv("MYAPP_APPLICATION_NAME", "custom-prefixed-app", 1);
    setenv("MYAPP_SAMPLING_COUNTER_RATE", "7", 1);
    // The default-prefixed names must be ignored while a custom prefix is active.
    setenv("PINPOINT_CPP_APPLICATION_NAME", "default-prefixed-app", 1);

    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "custom-prefixed-app");
    EXPECT_EQ(config->sampling.counter_rate, 7);
}

// ========== Log File Path Placeholder Tests ==========

// %pid% in Log.FilePath expands to the process id when the logger sink is
// applied; the Config keeps the raw value so reload comparisons and
// to_config_string() round-trips stay stable.
TEST_F(ConfigTest, LogFilePathExpandsPidPlaceholder) {
    const std::string raw_path = temp_dir_ + "/agent-%pid%.log";
    const std::string expanded_path =
        temp_dir_ + "/agent-" + std::to_string(getpid()) + ".log";
    set_config_string("ApplicationName: LogPathApp\nLog:\n  FilePath: " + raw_path + "\n");

    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->log.file_path, raw_path) << "Config must keep the raw path";
    EXPECT_TRUE(std::filesystem::exists(expanded_path))
        << "the logger must open the expanded path: " << expanded_path;

    Logger::getInstance().shutdown();
}

// ========== Agent Identity Tests ==========

// A legacy AgentId key in the config is ignored: the id is always
// auto-generated.
TEST_F(ConfigTest, AgentIdConfigKeyIsIgnored) {
    set_config_string(R"(
ApplicationName: IdentityApp
AgentId: configured-id
)");
    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->agent_id_.size(), 22u);
    EXPECT_EQ(config->agent_id_.find("configured-id"), std::string::npos)
        << "a configured AgentId must not leak into the identity: " << config->agent_id_;
}

// An explicitly configured AgentName is used verbatim (it need not be
// unique); a missing name falls back to the auto-generated agent id.
TEST_F(ConfigTest, AgentNameUsedVerbatimAndDefaultsToAgentId) {
    set_config_string(R"(
ApplicationName: IdentityApp
AgentName: worker-name
)");
    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->agent_name_, "worker-name");

    set_config_string(R"(
ApplicationName: IdentityApp
)");
    config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->agent_name_, config->agent_id_)
        << "a missing AgentName must fall back to the agent id";
}

// Test an empty prefix resets to the default PINPOINT_CPP prefix.
TEST_F(ConfigTest, EnvVarPrefixEmptyResetsToDefaultTest) {
    set_env_prefix("MYAPP");
    set_env_prefix("");  // reset to default

    setenv("PINPOINT_CPP_APPLICATION_NAME", "default-app", 1);
    setenv("MYAPP_APPLICATION_NAME", "should-be-ignored", 1);

    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "default-app");
}

// Test the default (no override) prefix is PINPOINT_CPP.
TEST_F(ConfigTest, EnvVarPrefixDefaultIsPinpointCppTest) {
    setenv("PINPOINT_CPP_APPLICATION_NAME", "default-app", 1);

    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "default-app");
}

// ========== Port Clamping Validation Tests ==========

// Test port clamping for out-of-range values
TEST_F(ConfigTest, PortClampingOutOfRangeTest) {
    // Port = 0 (below min 1)
    std::string yaml = R"(
Collector:
  GrpcHost: "localhost"
  GrpcAgentPort: 0
  GrpcSpanPort: 70000
  GrpcStatPort: -1
)";
    set_config_string(yaml);
    auto config = make_config();

    EXPECT_EQ(config->collector.agent_port, defaults::AGENT_PORT)
        << "Port 0 should be clamped to default";
    EXPECT_EQ(config->collector.span_port, defaults::SPAN_PORT)
        << "Port > 65535 should be clamped to default";
    EXPECT_EQ(config->collector.stat_port, defaults::STAT_PORT)
        << "Negative port should be clamped to default";
}

TEST_F(ConfigTest, PortBoundaryValuesTest) {
    std::string yaml = R"(
Collector:
  GrpcHost: "localhost"
  GrpcAgentPort: 1
  GrpcSpanPort: 65535
  GrpcStatPort: 80
)";
    set_config_string(yaml);
    auto config = make_config();

    EXPECT_EQ(config->collector.agent_port, 1) << "Port 1 (min) should be valid";
    EXPECT_EQ(config->collector.span_port, 65535) << "Port 65535 (max) should be valid";
    EXPECT_EQ(config->collector.stat_port, 80) << "Port 80 should be valid";
}

// Invalid channel values must not reach gRPC. Negative keepalive values,
// message sizes below gRPC's -1 (unlimited) sentinel, and queue sizes outside
// the bounded producer queue all fall back to the documented defaults.
TEST_F(ConfigTest, GrpcChannelInvalidValuesFallBackToDefaults) {
    set_config_string(R"(
Collector:
  Grpc:
    KeepAliveTimeMs: -1
    KeepAliveTimeoutMs: -1
    MaxSendMessageSize: -2
    MaxReceiveMessageSize: -2
    SenderQueueSize: 0
    ChannelMaxAgeMs: -5
    StreamMaxAgeMs: -1
)");

    const auto config = make_config();
    ASSERT_NE(config, nullptr);
    const auto& channel = config->collector.grpc.channel;
    EXPECT_EQ(channel.keepalive_time_ms, defaults::GRPC_KEEPALIVE_TIME_MS);
    EXPECT_EQ(channel.keepalive_timeout_ms, defaults::GRPC_KEEPALIVE_TIMEOUT_MS);
    EXPECT_EQ(channel.max_send_message_size, defaults::GRPC_MAX_MESSAGE_SIZE);
    EXPECT_EQ(channel.max_receive_message_size, defaults::GRPC_MAX_MESSAGE_SIZE);
    EXPECT_EQ(channel.sender_queue_size, defaults::GRPC_SENDER_QUEUE_SIZE);
    // Negative renewal ages mean "disabled", normalized to the 0 that
    // grpc.cpp treats as off.
    EXPECT_EQ(channel.channel_max_age_ms, 0);
    EXPECT_EQ(channel.stream_max_age_ms, 0);
}

// Zero is valid for gRPC keepalive controls, -1 means an unlimited message
// size, and both ends of the sender-queue range are accepted unchanged.
TEST_F(ConfigTest, GrpcChannelBoundaryValuesAreAccepted) {
    set_config_string(R"(
Collector:
  Grpc:
    KeepAliveTimeMs: 0
    KeepAliveTimeoutMs: 0
    MaxSendMessageSize: -1
    MaxReceiveMessageSize: -1
    SenderQueueSize: 1
)");

    auto config = make_config();
    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->collector.grpc.channel.keepalive_time_ms, 0);
    EXPECT_EQ(config->collector.grpc.channel.keepalive_timeout_ms, 0);
    EXPECT_EQ(config->collector.grpc.channel.max_send_message_size, -1);
    EXPECT_EQ(config->collector.grpc.channel.max_receive_message_size, -1);
    EXPECT_EQ(config->collector.grpc.channel.sender_queue_size, 1);

    set_config_string(R"(
Collector:
  Grpc:
    SenderQueueSize: 65536
)");
    config = make_config();
    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->collector.grpc.channel.sender_queue_size, 65536);
}

// ========== Stat Validation Tests ==========

TEST_F(ConfigTest, StatBatchCountOutOfRangeTest) {
    // Below minimum (1)
    std::string yaml = R"(
Stat:
  BatchCount: 0
  BatchInterval: 5000
)";
    set_config_string(yaml);
    auto config = make_config();
    EXPECT_EQ(config->stat.batch_count, defaults::STAT_BATCH_COUNT)
        << "batch_count 0 should be reset to default";

    // Above maximum (100)
    set_config_string(R"(
Stat:
  BatchCount: 101
)");
    config = make_config();
    EXPECT_EQ(config->stat.batch_count, defaults::STAT_BATCH_COUNT)
        << "batch_count 101 should be reset to default";

    // Boundary valid
    set_config_string(R"(
Stat:
  BatchCount: 1
)");
    config = make_config();
    EXPECT_EQ(config->stat.batch_count, 1) << "batch_count 1 (min) should be valid";

    set_config_string(R"(
Stat:
  BatchCount: 100
)");
    config = make_config();
    EXPECT_EQ(config->stat.batch_count, 100) << "batch_count 100 (max) should be valid";
}

TEST_F(ConfigTest, StatCollectIntervalOutOfRangeTest) {
    // Below minimum (1000)
    set_config_string(R"(
Stat:
  BatchInterval: 500
)");
    auto config = make_config();
    EXPECT_EQ(config->stat.collect_interval, defaults::STAT_INTERVAL_MS)
        << "Interval 500 should be reset to default";

    // Above maximum (60000)
    set_config_string(R"(
Stat:
  BatchInterval: 70000
)");
    config = make_config();
    EXPECT_EQ(config->stat.collect_interval, defaults::STAT_INTERVAL_MS)
        << "Interval 70000 should be reset to default";

    // Boundary valid
    set_config_string(R"(
Stat:
  BatchInterval: 1000
)");
    config = make_config();
    EXPECT_EQ(config->stat.collect_interval, 1000) << "Interval 1000 (min) should be valid";

    set_config_string(R"(
Stat:
  BatchInterval: 60000
)");
    config = make_config();
    EXPECT_EQ(config->stat.collect_interval, 60000) << "Interval 60000 (max) should be valid";
}

// ========== Span Min Clamping Tests ==========

// Test span max_event_depth minimum clamping (< 2 → 2, not -1)
TEST_F(ConfigTest, SpanMaxEventDepthMinClampingTest) {
    set_config_string(R"(
Span:
  MaxEventDepth: 0
)");
    auto config = make_config();
    EXPECT_EQ(config->span.max_event_depth, 2)
        << "max_event_depth 0 should be clamped to minimum 2";

    set_config_string(R"(
Span:
  MaxEventDepth: 1
)");
    config = make_config();
    EXPECT_EQ(config->span.max_event_depth, 2)
        << "max_event_depth 1 should be clamped to minimum 2";

    set_config_string(R"(
Span:
  MaxEventDepth: 2
)");
    config = make_config();
    EXPECT_EQ(config->span.max_event_depth, 2)
        << "max_event_depth 2 (min boundary) should be valid";
}

// Test span max_event_sequence minimum clamping (< 4 → 4, not -1)
TEST_F(ConfigTest, SpanMaxEventSequenceMinClampingTest) {
    set_config_string(R"(
Span:
  MaxEventSequence: 0
)");
    auto config = make_config();
    EXPECT_EQ(config->span.max_event_sequence, 4)
        << "max_event_sequence 0 should be clamped to minimum 4";

    set_config_string(R"(
Span:
  MaxEventSequence: 3
)");
    config = make_config();
    EXPECT_EQ(config->span.max_event_sequence, 4)
        << "max_event_sequence 3 should be clamped to minimum 4";

    set_config_string(R"(
Span:
  MaxEventSequence: 4
)");
    config = make_config();
    EXPECT_EQ(config->span.max_event_sequence, 4)
        << "max_event_sequence 4 (min boundary) should be valid";
}

// Test span queue size max boundary (65536)
TEST_F(ConfigTest, SpanQueueSizeMaxBoundaryTest) {
    set_config_string(R"(
Span:
  QueueSize: 65536
)");
    auto config = make_config();
    EXPECT_EQ(config->span.queue_size, 65536)
        << "queue_size 65536 (max) should be valid";

    set_config_string(R"(
Span:
  QueueSize: 65537
)");
    config = make_config();
    EXPECT_EQ(config->span.queue_size, defaults::SPAN_QUEUE_SIZE)
        << "queue_size > 65536 should be reset to default";
}

// ========== HTTP Comma-Separated Environment Variable Tests ==========

// Test HTTP server env vars with comma-separated values
TEST_F(ConfigTest, HttpServerCommaSeperatedEnvVarsTest) {
    setenv(full_env(env::HTTP_SERVER_STATUS_CODE_ERRORS).c_str(), "5xx,401,403", 1);
    setenv(full_env(env::HTTP_SERVER_EXCLUDE_URL).c_str(), "/health,/metrics,/ready", 1);
    setenv(full_env(env::HTTP_SERVER_EXCLUDE_METHOD).c_str(), "PUT,DELETE", 1);
    setenv(full_env(env::HTTP_SERVER_RECORD_REQUEST_HEADER).c_str(), "Authorization,Accept,Content-Type", 1);
    setenv(full_env(env::HTTP_SERVER_RECORD_REQUEST_COOKIE).c_str(), "session,tracking", 1);
    setenv(full_env(env::HTTP_SERVER_RECORD_RESPONSE_HEADER).c_str(), "Content-Type,X-Request-Id", 1);

    auto config = make_config();

    EXPECT_EQ(config->http.server.status_errors.size(), 3);
    EXPECT_EQ(config->http.server.status_errors[0], "5xx");
    EXPECT_EQ(config->http.server.status_errors[1], "401");
    EXPECT_EQ(config->http.server.status_errors[2], "403");

    EXPECT_EQ(config->http.server.exclude_url.size(), 3);
    EXPECT_EQ(config->http.server.exclude_url[0], "/health");
    EXPECT_EQ(config->http.server.exclude_url[2], "/ready");

    EXPECT_EQ(config->http.server.exclude_method.size(), 2);

    EXPECT_EQ(config->http.server.rec_request_header.size(), 3);
    EXPECT_EQ(config->http.server.rec_request_cookie.size(), 2);
    EXPECT_EQ(config->http.server.rec_response_header.size(), 2);
}

// Test HTTP client env vars with comma-separated values
TEST_F(ConfigTest, HttpClientCommaSeparatedEnvVarsTest) {
    setenv(full_env(env::HTTP_CLIENT_RECORD_REQUEST_HEADER).c_str(), "User-Agent,Accept", 1);
    setenv(full_env(env::HTTP_CLIENT_RECORD_REQUEST_COOKIE).c_str(), "session", 1);
    setenv(full_env(env::HTTP_CLIENT_RECORD_RESPONSE_HEADER).c_str(), "Content-Type,Set-Cookie,X-Trace-Id", 1);

    auto config = make_config();

    EXPECT_EQ(config->http.client.rec_request_header.size(), 2);
    EXPECT_EQ(config->http.client.rec_request_header[0], "User-Agent");
    EXPECT_EQ(config->http.client.rec_request_header[1], "Accept");

    EXPECT_EQ(config->http.client.rec_request_cookie.size(), 1);
    EXPECT_EQ(config->http.client.rec_request_cookie[0], "session");

    EXPECT_EQ(config->http.client.rec_response_header.size(), 3);
}

// Test HTTP env vars override YAML list values
TEST_F(ConfigTest, HttpEnvVarsOverrideYamlListTest) {
    set_config_string(R"(
Http:
  Server:
    StatusCodeErrors: ["5xx"]
    ExcludeUrl: ["/old-health"]
)");
    setenv(full_env(env::HTTP_SERVER_STATUS_CODE_ERRORS).c_str(), "4xx,5xx", 1);
    setenv(full_env(env::HTTP_SERVER_EXCLUDE_URL).c_str(), "/new-health,/new-metrics", 1);

    auto config = make_config();

    EXPECT_EQ(config->http.server.status_errors.size(), 2);
    EXPECT_EQ(config->http.server.status_errors[0], "4xx");
    EXPECT_EQ(config->http.server.status_errors[1], "5xx");

    EXPECT_EQ(config->http.server.exclude_url.size(), 2);
    EXPECT_EQ(config->http.server.exclude_url[0], "/new-health");
}

// ========== make_config log reload Tests ==========

class MakeConfigLogReloadTest : public ConfigTest {
protected:
    void TearDown() override {
        Logger::getInstance().shutdown();
        Logger::getInstance().setLogLevel("info");
        ConfigTest::TearDown();
    }

    std::string log_path() const { return temp_dir_ + "/make_config_log.log"; }

    std::string log_yaml() const {
        return "Log:\n  FilePath: \"" + log_path() + "\"\n";
    }

    static std::string read_file(const std::string& path) {
        std::ifstream ifs(path);
        return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
    }
};

// Setting Log.FilePath to an explicit empty string on a reload must switch
// logging back to stdout instead of keeping the stale file stream. (A key
// absent from the file keeps the running value by design, so disabling file
// output at runtime requires the explicit empty value.)
TEST_F(MakeConfigLogReloadTest, EmptyFilePathSwitchesBackToStdout) {
    set_config_string(log_yaml());
    auto with_file = make_config();
    ASSERT_NE(with_file, nullptr);
    Logger::getInstance().logInfo("test.cpp", 1, "to file");

    set_config_string("Log:\n  FilePath: \"\"\n");
    auto without_file = make_config(with_file);
    ASSERT_NE(without_file, nullptr);
    Logger::getInstance().logInfo("test.cpp", 1, "to stdout");
    Logger::getInstance().shutdown();

    const auto content = read_file(log_path());
    EXPECT_TRUE(content.find("to file") != std::string::npos);
    EXPECT_TRUE(content.find("to stdout") == std::string::npos)
        << "file logger must be released when the file path is set to empty";
}

// A key absent from the reloaded file keeps the running value instead of
// reverting to its default.
TEST_F(MakeConfigLogReloadTest, AbsentKeysKeepRunningValuesOnReload) {
    set_config_string("Log:\n  Level: \"debug\"\nSampling:\n  CounterRate: 42\n");
    auto cfg1 = make_config();
    ASSERT_NE(cfg1, nullptr);
    ASSERT_EQ(cfg1->log.level, "debug");
    ASSERT_EQ(cfg1->sampling.counter_rate, 42);

    // The reloaded file only touches sampling; the log level must not fall
    // back to the "info" default.
    set_config_string("Sampling:\n  CounterRate: 7\n");
    auto cfg2 = make_config(cfg1);
    ASSERT_NE(cfg2, nullptr);
    EXPECT_EQ(cfg2->sampling.counter_rate, 7);
    EXPECT_EQ(cfg2->log.level, "debug")
        << "keys absent from the reloaded file must keep their running values";
}

// A reload triggered by unrelated settings must not close and reopen the log
// file. Observable via an externally removed file: an untouched stream keeps
// writing to the deleted inode, while a reopen would recreate the file.
TEST_F(MakeConfigLogReloadTest, UnchangedFileLoggerIsNotReopened) {
    set_config_string(log_yaml());
    auto cfg1 = make_config();
    ASSERT_NE(cfg1, nullptr);

    std::filesystem::remove(log_path());
    set_config_string(log_yaml() + "Sampling:\n  CounterRate: 42\n");
    auto cfg2 = make_config(cfg1);
    ASSERT_NE(cfg2, nullptr);
    Logger::getInstance().logInfo("test.cpp", 1, "second");
    EXPECT_FALSE(std::filesystem::exists(log_path()))
        << "unchanged log settings must not reopen the file";

    set_config_string(log_yaml() + "  MaxFileSize: 20\n");
    auto cfg3 = make_config(cfg2);
    ASSERT_NE(cfg3, nullptr);
    Logger::getInstance().logInfo("test.cpp", 1, "third");
    Logger::getInstance().shutdown();
    EXPECT_TRUE(std::filesystem::exists(log_path()))
        << "changed log settings must reopen the file";
    EXPECT_TRUE(read_file(log_path()).find("third") != std::string::npos);
}

} // namespace pinpoint
