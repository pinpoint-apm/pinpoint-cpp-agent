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
#include <gtest/gtest.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <unistd.h>

namespace pinpoint {

// env:: constants hold only the suffix; the agent reads "<prefix>_<suffix>".
// These tests drive the OS environment variables the agent consumes, so compose
// the full default-prefixed name here.
static std::string full_env(const char* suffix) {
    return std::string(env::DEFAULT_PREFIX) + "_" + suffix;
}

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Save current environment variables
        SaveEnvironmentVariables();
        
        // Clear any existing config
        set_config_string("");
        // Reset the env var prefix to its default so a prior test cannot leak it.
        set_env_prefix("");

        // Create a temporary directory for test files
        temp_dir_ = "/tmp/pinpoint_config_test_" + std::to_string(getpid());
        mkdir(temp_dir_.c_str(), 0755);
    }

    void TearDown() override {
        // Restore environment variables
        RestoreEnvironmentVariables();
        
        // Clean up temporary files
        system(("rm -rf " + temp_dir_).c_str());
    }

private:
    void SaveEnvironmentVariables() {
        // Save environment variables that might affect config
        saved_env_vars_[full_env(env::ENABLE)] = GetEnvVar(full_env(env::ENABLE));
        saved_env_vars_[full_env(env::APPLICATION_NAME)] = GetEnvVar(full_env(env::APPLICATION_NAME));
        saved_env_vars_[full_env(env::AGENT_ID)] = GetEnvVar(full_env(env::AGENT_ID));
        saved_env_vars_[full_env(env::AGENT_NAME)] = GetEnvVar(full_env(env::AGENT_NAME));
        saved_env_vars_[full_env(env::UID_VERSION)] = GetEnvVar(full_env(env::UID_VERSION));
        saved_env_vars_[full_env(env::SERVICE_NAME)] = GetEnvVar(full_env(env::SERVICE_NAME));
        saved_env_vars_[full_env(env::API_KEY)] = GetEnvVar(full_env(env::API_KEY));
        saved_env_vars_[full_env(env::LOG_LEVEL)] = GetEnvVar(full_env(env::LOG_LEVEL));
        saved_env_vars_[full_env(env::GRPC_HOST)] = GetEnvVar(full_env(env::GRPC_HOST));
        saved_env_vars_[full_env(env::GRPC_AGENT_PORT)] = GetEnvVar(full_env(env::GRPC_AGENT_PORT));
        saved_env_vars_[full_env(env::GRPC_SPAN_PORT)] = GetEnvVar(full_env(env::GRPC_SPAN_PORT));
        saved_env_vars_[full_env(env::GRPC_STAT_PORT)] = GetEnvVar(full_env(env::GRPC_STAT_PORT));
        saved_env_vars_[full_env(env::COLLECTOR_HOST)] = GetEnvVar(full_env(env::COLLECTOR_HOST));
        saved_env_vars_[full_env(env::COLLECTOR_AGENT_PORT)] = GetEnvVar(full_env(env::COLLECTOR_AGENT_PORT));
        saved_env_vars_[full_env(env::COLLECTOR_SPAN_PORT)] = GetEnvVar(full_env(env::COLLECTOR_SPAN_PORT));
        saved_env_vars_[full_env(env::COLLECTOR_STAT_PORT)] = GetEnvVar(full_env(env::COLLECTOR_STAT_PORT));
        const std::vector<std::string> grpc_env_vars = {
            full_env(env::GRPC_SSL_TRUST_CERT_FILE_PATH),
            full_env(env::GRPC_SSL_ROOT_CERT_FILE_PATH),
            full_env(env::GRPC_SSL_ENABLE),
            full_env(env::GRPC_KEEPALIVE_TIME_MS),
            full_env(env::GRPC_KEEPALIVE_TIMEOUT_MS),
            full_env(env::GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS),
            full_env(env::GRPC_MAX_SEND_MESSAGE_SIZE),
            full_env(env::GRPC_MAX_RECEIVE_MESSAGE_SIZE),
            full_env(env::GRPC_SENDER_QUEUE_SIZE),
        };
        for (const std::string& name : grpc_env_vars) {
            saved_env_vars_[name] = GetEnvVar(name);
        }
        saved_env_vars_[full_env(env::SAMPLING_TYPE)] = GetEnvVar(full_env(env::SAMPLING_TYPE));
        saved_env_vars_[full_env(env::SAMPLING_PERCENT_RATE)] = GetEnvVar(full_env(env::SAMPLING_PERCENT_RATE));
        saved_env_vars_[full_env(env::IS_CONTAINER)] = GetEnvVar(full_env(env::IS_CONTAINER));
        saved_env_vars_[full_env(env::CONFIG_FILE)] = GetEnvVar(full_env(env::CONFIG_FILE));
        saved_env_vars_[full_env(env::SQL_MAX_BIND_ARGS_SIZE)] = GetEnvVar(full_env(env::SQL_MAX_BIND_ARGS_SIZE));
        saved_env_vars_[full_env(env::SQL_ENABLE_SQL_STATS)] = GetEnvVar(full_env(env::SQL_ENABLE_SQL_STATS));
        saved_env_vars_[full_env(env::SQL_ENABLE_RAW_SQL_CACHE)] = GetEnvVar(full_env(env::SQL_ENABLE_RAW_SQL_CACHE));
        saved_env_vars_[full_env(env::SQL_TRACE_BIND_VALUE)] = GetEnvVar(full_env(env::SQL_TRACE_BIND_VALUE));
        saved_env_vars_[full_env(env::ENABLE_CALLSTACK_TRACE)] = GetEnvVar(full_env(env::ENABLE_CALLSTACK_TRACE));
        saved_env_vars_[full_env(env::HTTP_COLLECT_URL_STAT)] = GetEnvVar(full_env(env::HTTP_COLLECT_URL_STAT));
        saved_env_vars_[full_env(env::HTTP_URL_STAT_LIMIT)] = GetEnvVar(full_env(env::HTTP_URL_STAT_LIMIT));
        saved_env_vars_[full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH)] = GetEnvVar(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH));
        saved_env_vars_[full_env(env::HTTP_URL_STAT_TRIM_PATH_DEPTH)] = GetEnvVar(full_env(env::HTTP_URL_STAT_TRIM_PATH_DEPTH));
        saved_env_vars_[full_env(env::HTTP_URL_STAT_METHOD_PREFIX)] = GetEnvVar(full_env(env::HTTP_URL_STAT_METHOD_PREFIX));
        saved_env_vars_[full_env(env::HTTP_SERVER_STATUS_CODE_ERRORS)] = GetEnvVar(full_env(env::HTTP_SERVER_STATUS_CODE_ERRORS));
        saved_env_vars_[full_env(env::HTTP_SERVER_EXCLUDE_URL)] = GetEnvVar(full_env(env::HTTP_SERVER_EXCLUDE_URL));
        saved_env_vars_[full_env(env::HTTP_SERVER_EXCLUDE_METHOD)] = GetEnvVar(full_env(env::HTTP_SERVER_EXCLUDE_METHOD));
        saved_env_vars_[full_env(env::HTTP_SERVER_RECORD_REQUEST_HEADER)] = GetEnvVar(full_env(env::HTTP_SERVER_RECORD_REQUEST_HEADER));
        saved_env_vars_[full_env(env::HTTP_SERVER_RECORD_REQUEST_COOKIE)] = GetEnvVar(full_env(env::HTTP_SERVER_RECORD_REQUEST_COOKIE));
        saved_env_vars_[full_env(env::HTTP_SERVER_RECORD_RESPONSE_HEADER)] = GetEnvVar(full_env(env::HTTP_SERVER_RECORD_RESPONSE_HEADER));
        saved_env_vars_[full_env(env::HTTP_CLIENT_RECORD_REQUEST_HEADER)] = GetEnvVar(full_env(env::HTTP_CLIENT_RECORD_REQUEST_HEADER));
        saved_env_vars_[full_env(env::HTTP_CLIENT_RECORD_REQUEST_COOKIE)] = GetEnvVar(full_env(env::HTTP_CLIENT_RECORD_REQUEST_COOKIE));
        saved_env_vars_[full_env(env::HTTP_CLIENT_RECORD_RESPONSE_HEADER)] = GetEnvVar(full_env(env::HTTP_CLIENT_RECORD_RESPONSE_HEADER));
        saved_env_vars_[full_env(env::SPAN_QUEUE_SIZE)] = GetEnvVar(full_env(env::SPAN_QUEUE_SIZE));
        saved_env_vars_[full_env(env::SPAN_MAX_EVENT_DEPTH)] = GetEnvVar(full_env(env::SPAN_MAX_EVENT_DEPTH));
        saved_env_vars_[full_env(env::SPAN_MAX_EVENT_SEQUENCE)] = GetEnvVar(full_env(env::SPAN_MAX_EVENT_SEQUENCE));
        saved_env_vars_[full_env(env::SPAN_EVENT_CHUNK_SIZE)] = GetEnvVar(full_env(env::SPAN_EVENT_CHUNK_SIZE));
        saved_env_vars_[full_env(env::AGENT_INFO_REFRESH_INTERVAL_MS)] = GetEnvVar(full_env(env::AGENT_INFO_REFRESH_INTERVAL_MS));
        saved_env_vars_[full_env(env::AGENT_INFO_SEND_RETRY_INTERVAL_MS)] = GetEnvVar(full_env(env::AGENT_INFO_SEND_RETRY_INTERVAL_MS));
        saved_env_vars_[full_env(env::AGENT_INFO_MAX_TRY_PER_ATTEMPT)] = GetEnvVar(full_env(env::AGENT_INFO_MAX_TRY_PER_ATTEMPT));
        saved_env_vars_[full_env(env::STAT_ENABLE)] = GetEnvVar(full_env(env::STAT_ENABLE));
        saved_env_vars_[full_env(env::STAT_BATCH_COUNT)] = GetEnvVar(full_env(env::STAT_BATCH_COUNT));
        saved_env_vars_[full_env(env::STAT_BATCH_INTERVAL)] = GetEnvVar(full_env(env::STAT_BATCH_INTERVAL));
        saved_env_vars_[full_env(env::AGENT_NAME)] = GetEnvVar(full_env(env::AGENT_NAME));

        // Clear environment variables for clean test
        for (const auto& pair : saved_env_vars_) {
            unsetenv(pair.first.c_str());
        }
    }
    
    void RestoreEnvironmentVariables() {
        for (const auto& pair : saved_env_vars_) {
            if (!pair.second.empty()) {
                setenv(pair.first.c_str(), pair.second.c_str(), 1);
            } else {
                unsetenv(pair.first.c_str());
            }
        }
    }
    
    std::string GetEnvVar(const std::string& name) {
        const char* value = std::getenv(name.c_str());
        return value ? std::string(value) : std::string();
    }

protected:
    std::map<std::string, std::string> saved_env_vars_;
    std::string temp_dir_;
    
    // Test YAML configurations
    const std::string complete_config_yaml_ = R"(
ApplicationName: "MyTestApp"
AgentId: "test-agent-123"
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

// Test generated agent ID
TEST_F(ConfigTest, GeneratedAgentIdTest) {
    auto config = make_config();
    
    EXPECT_FALSE(config->agent_id_.empty()) << "Agent ID should be generated when not provided";
    EXPECT_GE(config->agent_id_.length(), 5) << "Generated agent ID should have reasonable length";
    
    // Test that multiple calls generate different IDs
    auto config2 = make_config();
    EXPECT_NE(config->agent_id_, config2->agent_id_) << "Multiple calls should generate different agent IDs";
}

// ========== YAML Configuration Tests ==========

// Test complete YAML configuration
TEST_F(ConfigTest, CompleteYamlConfigurationTest) {
    set_config_string(complete_config_yaml_);
    auto config = make_config();
    
    // Test basic values
    EXPECT_EQ(config->app_name_, "MyTestApp") << "App name should match YAML";
    EXPECT_EQ(config->agent_id_, "test-agent-123") << "Agent ID should match YAML";
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

// Test partial YAML configuration
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

// Test empty YAML configuration
TEST_F(ConfigTest, EmptyYamlConfigurationTest) {
    set_config_string("");
    auto config = make_config();
    
    // Should have all default values
    EXPECT_EQ(config->app_name_, "") << "App name should be default (empty)";
    EXPECT_EQ(config->log.level, "info") << "Log level should be default";
    EXPECT_EQ(config->collector.agent_port, 9991) << "Agent port should be default";
}

// ========== Environment Variable Tests ==========

// Test environment variable configuration
TEST_F(ConfigTest, EnvironmentVariableConfigurationTest) {
    // Set environment variables
    setenv(full_env(env::APPLICATION_NAME).c_str(), "EnvApp", 1);
    setenv(full_env(env::AGENT_ID).c_str(), "env-agent-456", 1);
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
    setenv(full_env(env::AGENT_INFO_REFRESH_INTERVAL_MS).c_str(), "120000", 1);
    setenv(full_env(env::AGENT_INFO_SEND_RETRY_INTERVAL_MS).c_str(), "50", 1);
    setenv(full_env(env::AGENT_INFO_MAX_TRY_PER_ATTEMPT).c_str(), "4", 1);
    setenv(full_env(env::GRPC_SSL_TRUST_CERT_FILE_PATH).c_str(), "/env/trust.pem", 1);
    setenv(full_env(env::GRPC_SSL_ENABLE).c_str(), "true", 1);
    setenv(full_env(env::GRPC_KEEPALIVE_TIME_MS).c_str(), "22222", 1);
    setenv(full_env(env::GRPC_MAX_SEND_MESSAGE_SIZE).c_str(), "33333", 1);
    setenv(full_env(env::GRPC_SENDER_QUEUE_SIZE).c_str(), "4444", 1);
    setenv(full_env(env::GRPC_MAX_RECEIVE_MESSAGE_SIZE).c_str(), "55555", 1);
    
    auto config = make_config();
    
    // Test environment variable values
    EXPECT_EQ(config->app_name_, "EnvApp") << "App name should match environment variable";
    EXPECT_EQ(config->agent_id_, "env-agent-456") << "Agent ID should match environment variable";
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
    setenv(full_env(env::AGENT_ID).c_str(), "reload-agent", 1);
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

// Test environment variable override YAML
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

// Test configuration file reading
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

// Test missing configuration file
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

// ========== Error Handling Tests ==========

// Test invalid YAML handling
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
    setenv(full_env(env::AGENT_ID).c_str(), "env-agent-recovered", 1);
    setenv(full_env(env::GRPC_HOST).c_str(), "env.collector.host", 1);
    setenv(full_env(env::GRPC_AGENT_PORT).c_str(), "70000", 1);
    setenv(full_env(env::SPAN_MAX_EVENT_DEPTH).c_str(), "1", 1);
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "true", 1);
    setenv(full_env(env::LOG_LEVEL).c_str(), "error", 1);

    auto config = make_config();

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "EnvRecoveredApp")
        << "Environment app name should apply even when YAML parsing fails";
    EXPECT_EQ(config->agent_id_, "env-agent-recovered")
        << "Environment agent ID should apply and survive identity resolution";
    EXPECT_EQ(config->agent_name_, "env-agent-recovered")
        << "Identity resolution should still derive the default agent name";
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

TEST_F(ConfigTest, UidVersionV1ConfigurationToStringAndRoundTripTest) {
    set_config_string(R"(
ApplicationName: "UidV1App"
UidVersion: v1
Collector:
  GrpcHost: localhost
)");
    auto config = make_config();
    ASSERT_NE(config, nullptr);
    ASSERT_TRUE(config->identity_resolved_);
    EXPECT_EQ(config->uid_version_, "v1");
    EXPECT_EQ(config->object_name_version_, 1);

    std::string config_string = to_config_string(*config);
    EXPECT_TRUE(config_string.find("UidVersion: v1") != std::string::npos)
        << "Config string should contain v1 UID version";

    set_config_string(config_string);
    auto config2 = make_config();
    ASSERT_NE(config2, nullptr);
    EXPECT_EQ(config2->uid_version_, "v1");
    EXPECT_EQ(config2->object_name_version_, 1);
}

TEST_F(ConfigTest, UidVersionV3ConfigurationToStringAndRoundTripTest) {
    set_config_string(R"(
ApplicationName: "UidV3App"
UidVersion: v3
Collector:
  GrpcHost: localhost
)");
    auto config = make_config();
    ASSERT_NE(config, nullptr);
    ASSERT_TRUE(config->identity_resolved_);
    EXPECT_EQ(config->uid_version_, "v3");
    EXPECT_EQ(config->object_name_version_, 1);

    std::string config_string = to_config_string(*config);
    EXPECT_TRUE(config_string.find("UidVersion: v3") != std::string::npos)
        << "Config string should contain v3 UID version";

    set_config_string(config_string);
    auto config2 = make_config();
    ASSERT_NE(config2, nullptr);
    EXPECT_EQ(config2->uid_version_, "v3");
    EXPECT_EQ(config2->object_name_version_, 1);
}

// An unrecognized UidVersion (anything not v1/v4) resolves as v3: object_name_
// version_ becomes VERSION_V1 (1) exactly like an explicit v3, identity still
// resolves, and the raw string is preserved (isReloadable compares the raw
// uid_version_, not the resolved version). Guards the schema-drift / parse
// fallback at the config integration level, beyond the parse_name_version unit test.
TEST_F(ConfigTest, UidVersionInvalidValueFallsBackToV3Test) {
    set_config_string(R"(
ApplicationName: "UidInvalidApp"
UidVersion: v2
Collector:
  GrpcHost: localhost
)");
    auto config = make_config();
    ASSERT_NE(config, nullptr);
    EXPECT_TRUE(config->identity_resolved_)
        << "an invalid uid version must still resolve identity as v3";
    EXPECT_EQ(config->uid_version_, "v2") << "the raw uid version string is preserved";
    EXPECT_EQ(config->object_name_version_, 1)
        << "an unknown uid version maps to VERSION_V1 (v3 behavior)";
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
    config.uid_version_ = "v4";

    const auto config_strings = to_non_default_config_strings(config);
    EXPECT_EQ(config_strings.size(), 7);

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
}

// ========== Integration Tests ==========

// Test complete configuration flow
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
    EXPECT_EQ(config->agent_id_, "test-agent-123") << "Agent ID should come from file";
}

// ========== Exception Handling Tests ==========

// Test type conversion exception handling
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

// Test mixed valid and invalid configurations
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

// Test environment variable validation for valid values
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

// Test environment variable validation for boolean edge cases
TEST_F(ConfigTest, EnvironmentVariableBooleanEdgeCasesTest) {
    // Test various valid boolean representations
    setenv(full_env(env::ENABLE).c_str(), "TRUE", 1);
    setenv(full_env(env::STAT_ENABLE).c_str(), "False", 1);
    setenv(full_env(env::IS_CONTAINER).c_str(), "yes", 1);
    setenv(full_env(env::HTTP_COLLECT_URL_STAT).c_str(), "NO", 1);
    
    auto config = make_config();
    
    EXPECT_TRUE(config->enable) << "TRUE should be parsed as true";
    EXPECT_FALSE(config->stat.enable) << "False should be parsed as false";
    EXPECT_TRUE(config->is_container) << "yes should be parsed as true";
    EXPECT_FALSE(config->http.url_stat.enable) << "NO should be parsed as false";
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

// Test SQL configuration edge cases
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

// Test SQL configuration round-trip
TEST_F(ConfigTest, SqlConfigurationRoundTripTest) {
    const std::string sql_config = R"(
ApplicationName: "SqlTestApp"
Sql:
  MaxBindArgsSize: 3072
  EnableSqlStats: true
  EnableRawSqlCache: true
  TraceBindValue: true
)";
    
    set_config_string(sql_config);
    auto config1 = make_config();
    
    std::string generated_config_string = to_config_string(*config1);
    
    // Use generated string as new config
    set_config_string(generated_config_string);
    auto config2 = make_config();
    
    // SQL configs should match after round-trip
    EXPECT_EQ(config1->sql.max_bind_args_size, config2->sql.max_bind_args_size) 
        << "Max bind args size should match after round-trip";
    EXPECT_EQ(config1->sql.enable_sql_stats, config2->sql.enable_sql_stats) 
        << "SQL stats enable should match after round-trip";
    EXPECT_EQ(config1->sql.enable_raw_sql_cache, config2->sql.enable_raw_sql_cache)
        << "Raw SQL cache enable should match after round-trip";
    EXPECT_EQ(config1->sql.trace_bind_value, config2->sql.trace_bind_value)
        << "SQL bind value tracing should match after round-trip";
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

// ========== CallStack Trace Configuration Tests ==========

// Test default callstack trace configuration
TEST_F(ConfigTest, CallstackTraceDefaultTest) {
    auto config = make_config();
    
    // Default should be false
    EXPECT_FALSE(config->enable_callstack_trace) << "CallStack trace should be disabled by default";
}

// Test enabling callstack trace via YAML
TEST_F(ConfigTest, CallstackTraceEnableViaYamlTest) {
    set_config_string(R"(
EnableCallstackTrace: true
)");
    auto config = make_config();
    
    EXPECT_TRUE(config->enable_callstack_trace) << "CallStack trace should be enabled as per YAML";
}

// Test disabling callstack trace via YAML
TEST_F(ConfigTest, CallstackTraceDisableViaYamlTest) {
    set_config_string(R"(
EnableCallstackTrace: false
)");
    auto config = make_config();
    
    EXPECT_FALSE(config->enable_callstack_trace) << "CallStack trace should be disabled as per YAML";
}

// Test enabling callstack trace via environment variable
TEST_F(ConfigTest, CallstackTraceEnableViaEnvironmentVariableTest) {
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "true", 1);
    
    auto config = make_config();
    
    EXPECT_TRUE(config->enable_callstack_trace) << "CallStack trace should be enabled as per environment variable";
}

// Test disabling callstack trace via environment variable
TEST_F(ConfigTest, CallstackTraceDisableViaEnvironmentVariableTest) {
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "false", 1);
    
    auto config = make_config();
    
    EXPECT_FALSE(config->enable_callstack_trace) << "CallStack trace should be disabled as per environment variable";
}

// Test environment variable overrides YAML for callstack trace
TEST_F(ConfigTest, CallstackTraceEnvironmentVariableOverrideYamlTest) {
    // Set YAML to disable
    set_config_string(R"(
EnableCallstackTrace: false
)");
    
    // Set environment variable to enable (should override YAML)
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "true", 1);
    
    auto config = make_config();
    
    EXPECT_TRUE(config->enable_callstack_trace) << "Environment variable should override YAML for callstack trace";
}

// Test environment variable overrides YAML (opposite case)
TEST_F(ConfigTest, CallstackTraceEnvironmentVariableOverrideYamlOppositeTest) {
    // Set YAML to enable
    set_config_string(R"(
EnableCallstackTrace: true
)");
    
    // Set environment variable to disable (should override YAML)
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "false", 1);
    
    auto config = make_config();
    
    EXPECT_FALSE(config->enable_callstack_trace) << "Environment variable should override YAML for callstack trace";
}

// Test callstack trace in complete configuration
TEST_F(ConfigTest, CallstackTraceInCompleteConfigurationTest) {
    const std::string complete_config = R"(
ApplicationName: "CallstackTestApp"
EnableCallstackTrace: true

Log:
  Level: "debug"

Collector:
  GrpcHost: "test.host"
  GrpcAgentPort: 9000
)";
    
    set_config_string(complete_config);
    auto config = make_config();
    
    EXPECT_EQ(config->app_name_, "CallstackTestApp") << "App name should match";
    EXPECT_TRUE(config->enable_callstack_trace) << "CallStack trace should be enabled";
    EXPECT_EQ(config->log.level, "debug") << "Other config values should also be loaded";
}

// Test callstack trace configuration string generation
TEST_F(ConfigTest, CallstackTraceConfigurationToStringTest) {
    set_config_string(R"(
EnableCallstackTrace: true
)");
    auto config = make_config();
    
    std::string config_string = to_config_string(*config);
    
    // Check that callstack trace configuration is included in generated string
    EXPECT_TRUE(config_string.find("EnableCallstackTrace: true") != std::string::npos) 
        << "Config string should contain EnableCallstackTrace setting";
}

// Test callstack trace configuration string generation when disabled
TEST_F(ConfigTest, CallstackTraceConfigurationToStringDisabledTest) {
    set_config_string(R"(
EnableCallstackTrace: false
)");
    auto config = make_config();
    
    std::string config_string = to_config_string(*config);
    
    // Check that callstack trace configuration is included in generated string
    EXPECT_TRUE(config_string.find("EnableCallstackTrace: false") != std::string::npos) 
        << "Config string should contain EnableCallstackTrace setting as false";
}

// Test callstack trace configuration round-trip
TEST_F(ConfigTest, CallstackTraceConfigurationRoundTripTest) {
    const std::string callstack_config = R"(
ApplicationName: "RoundTripApp"
EnableCallstackTrace: true
)";
    
    set_config_string(callstack_config);
    auto config1 = make_config();
    
    EXPECT_TRUE(config1->enable_callstack_trace) << "Initial config should have callstack trace enabled";
    
    std::string generated_config_string = to_config_string(*config1);
    
    // Use generated string as new config
    set_config_string(generated_config_string);
    auto config2 = make_config();
    
    // CallStack trace config should match after round-trip
    EXPECT_EQ(config1->enable_callstack_trace, config2->enable_callstack_trace) 
        << "CallStack trace setting should match after round-trip";
    EXPECT_TRUE(config2->enable_callstack_trace) << "CallStack trace should still be enabled after round-trip";
}

// Test invalid callstack trace environment variable value
TEST_F(ConfigTest, CallstackTraceInvalidEnvironmentVariableTest) {
    // Test invalid boolean value (should fallback to default)
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "invalid_bool", 1);
    
    auto config = make_config();
    
    // Should fallback to default (false) when environment variable is invalid
    EXPECT_FALSE(config->enable_callstack_trace) << "Should use default (false) when env var is invalid";
}

// Test various valid boolean representations for callstack trace
TEST_F(ConfigTest, CallstackTraceBooleanRepresentationsTest) {
    // Test "1" as true
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "1", 1);
    auto config1 = make_config();
    EXPECT_TRUE(config1->enable_callstack_trace) << "1 should be parsed as true";
    
    // Test "0" as false
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "0", 1);
    auto config2 = make_config();
    EXPECT_FALSE(config2->enable_callstack_trace) << "0 should be parsed as false";
    
    // Test "TRUE" as true
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "TRUE", 1);
    auto config3 = make_config();
    EXPECT_TRUE(config3->enable_callstack_trace) << "TRUE should be parsed as true";
    
    // Test "False" as false
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "False", 1);
    auto config4 = make_config();
    EXPECT_FALSE(config4->enable_callstack_trace) << "False should be parsed as false";
    
    // Test "yes" as true
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "yes", 1);
    auto config5 = make_config();
    EXPECT_TRUE(config5->enable_callstack_trace) << "yes should be parsed as true";
    
    // Test "NO" as false
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "NO", 1);
    auto config6 = make_config();
    EXPECT_FALSE(config6->enable_callstack_trace) << "NO should be parsed as false";
}

// Test callstack trace with invalid YAML type
TEST_F(ConfigTest, CallstackTraceInvalidYamlTypeTest) {
    // YAML with invalid type for EnableCallstackTrace
    const std::string invalid_type_yaml = R"(
EnableCallstackTrace: "not_a_boolean"
)";
    
    set_config_string(invalid_type_yaml);
    auto config = make_config();
    
    // Should use default value when YAML type is invalid
    EXPECT_FALSE(config->enable_callstack_trace) << "Should use default (false) when YAML type is invalid";
}

// Test callstack trace mixed with other configurations
TEST_F(ConfigTest, CallstackTraceMixedConfigurationTest) {
    const std::string mixed_config = R"(
ApplicationName: "MixedApp"
Enable: true
EnableCallstackTrace: true

Log:
  Level: "warn"
  MaxFileSize: 50

Collector:
  GrpcHost: "mixed.collector.host"

Sampling:
  Type: "PERCENT"
  PercentRate: 25.0

Sql:
  MaxBindArgsSize: 2048
  EnableSqlStats: true
)";
    
    set_config_string(mixed_config);
    auto config = make_config();
    
    // Verify all config values including callstack trace
    EXPECT_EQ(config->app_name_, "MixedApp") << "App name should match";
    EXPECT_TRUE(config->enable) << "Enable should be true";
    EXPECT_TRUE(config->enable_callstack_trace) << "CallStack trace should be enabled";
    EXPECT_EQ(config->log.level, "warn") << "Log level should match";
    EXPECT_EQ(config->log.max_file_size, 50) << "Log max file size should match";
    EXPECT_EQ(config->collector.host, "mixed.collector.host") << "Collector host should match";
    EXPECT_EQ(config->sampling.type, "PERCENT") << "Sampling type should match";
    EXPECT_DOUBLE_EQ(config->sampling.percent_rate, 25.0) << "Percent rate should match";
    EXPECT_EQ(config->sql.max_bind_args_size, 2048) << "SQL max bind args size should match";
    EXPECT_TRUE(config->sql.enable_sql_stats) << "SQL stats should be enabled";
}

// Test callstack trace environment variable with other environment variables
TEST_F(ConfigTest, CallstackTraceEnvironmentVariableWithOthersTest) {
    setenv(full_env(env::APPLICATION_NAME).c_str(), "EnvMixedApp", 1);
    setenv(full_env(env::ENABLE_CALLSTACK_TRACE).c_str(), "true", 1);
    setenv(full_env(env::SQL_ENABLE_SQL_STATS).c_str(), "true", 1);
    setenv(full_env(env::LOG_LEVEL).c_str(), "error", 1);
    
    auto config = make_config();
    
    // Verify all environment variables are loaded correctly
    EXPECT_EQ(config->app_name_, "EnvMixedApp") << "App name should match env var";
    EXPECT_TRUE(config->enable_callstack_trace) << "CallStack trace should be enabled via env var";
    EXPECT_TRUE(config->sql.enable_sql_stats) << "SQL stats should be enabled via env var";
    EXPECT_EQ(config->log.level, "error") << "Log level should match env var";
}

// Test callstack trace default value is included in string output
TEST_F(ConfigTest, CallstackTraceDefaultInStringOutputTest) {
    // Don't set any config, use defaults
    auto config = make_config();
    
    std::string config_string = to_config_string(*config);
    
    // Check that callstack trace is included in output even with default value
    EXPECT_TRUE(config_string.find("EnableCallstackTrace") != std::string::npos) 
        << "Config string should contain EnableCallstackTrace key";
    EXPECT_TRUE(config_string.find("EnableCallstackTrace: false") != std::string::npos) 
        << "Config string should show default value (false)";
}

// ========== URL Stat Enable Trim Path Tests ==========

// Test URL stat enable trim path via YAML
TEST_F(ConfigTest, UrlStatEnableTrimPathViaYamlTest) {
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: false
)");
    auto config = make_config();
    
    EXPECT_FALSE(config->http.url_stat.enable_trim_path) << "URL stat enable trim path should be disabled via YAML";
}

// Test URL stat enable trim path via environment variable
TEST_F(ConfigTest, UrlStatEnableTrimPathViaEnvironmentVariableTest) {
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "false", 1);
    auto config = make_config();
    
    EXPECT_FALSE(config->http.url_stat.enable_trim_path) << "URL stat enable trim path should be disabled via environment variable";
}

// Test environment variable overrides YAML for enable trim path
TEST_F(ConfigTest, UrlStatEnableTrimPathEnvironmentVariableOverrideYamlTest) {
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: false
)");
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "true", 1);
    auto config = make_config();
    
    EXPECT_TRUE(config->http.url_stat.enable_trim_path) << "Environment variable should override YAML for enable trim path";
}

// Test environment variable overrides YAML (opposite case)
TEST_F(ConfigTest, UrlStatEnableTrimPathEnvironmentVariableOverrideYamlOppositeTest) {
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: true
)");
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "false", 1);
    auto config = make_config();
    
    EXPECT_FALSE(config->http.url_stat.enable_trim_path) << "Environment variable should override YAML for enable trim path (opposite)";
}

// Test invalid environment variable for enable trim path
TEST_F(ConfigTest, UrlStatEnableTrimPathInvalidEnvironmentVariableTest) {
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "invalid", 1);
    auto config = make_config();
    
    // Should use default value (true) when invalid
    EXPECT_TRUE(config->http.url_stat.enable_trim_path) << "Invalid environment variable should use default value";
}

// Test enable trim path with other URL stat settings
TEST_F(ConfigTest, UrlStatEnableTrimPathWithOtherSettingsTest) {
    set_config_string(R"(
Http:
  CollectUrlStat: true
  UrlStatLimit: 512
  UrlStatEnableTrimPath: false
  UrlStatTrimPathDepth: 2
  UrlStatMethodPrefix: true
)");
    auto config = make_config();
    
    EXPECT_TRUE(config->http.url_stat.enable) << "URL stat should be enabled";
    EXPECT_EQ(config->http.url_stat.limit, 512) << "URL stat limit should match YAML";
    EXPECT_FALSE(config->http.url_stat.enable_trim_path) << "Enable trim path should be false";
    EXPECT_EQ(config->http.url_stat.trim_path_depth, 2) << "Path depth should match YAML";
    EXPECT_TRUE(config->http.url_stat.method_prefix) << "Method prefix should be true";
}

// Test enable trim path boolean variations
TEST_F(ConfigTest, UrlStatEnableTrimPathBooleanVariationsTest) {
    // Test "yes"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: yes
)");
    auto config1 = make_config();
    EXPECT_TRUE(config1->http.url_stat.enable_trim_path) << "yes should be parsed as true";
    
    // Test "no"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: no
)");
    auto config2 = make_config();
    EXPECT_FALSE(config2->http.url_stat.enable_trim_path) << "no should be parsed as false";
    
    // Test "TRUE"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: TRUE
)");
    auto config3 = make_config();
    EXPECT_TRUE(config3->http.url_stat.enable_trim_path) << "TRUE should be parsed as true";
    
    // Test "FALSE"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: FALSE
)");
    auto config4 = make_config();
    EXPECT_FALSE(config4->http.url_stat.enable_trim_path) << "FALSE should be parsed as false";
}

// Test enable trim path environment variable boolean variations
TEST_F(ConfigTest, UrlStatEnableTrimPathEnvironmentVariableBooleanVariationsTest) {
    // Test "1"
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "1", 1);
    auto config1 = make_config();
    EXPECT_TRUE(config1->http.url_stat.enable_trim_path) << "1 should be parsed as true";
    
    // Test "0"
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "0", 1);
    auto config2 = make_config();
    EXPECT_FALSE(config2->http.url_stat.enable_trim_path) << "0 should be parsed as false";
    
    // Test "yes"
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "yes", 1);
    auto config3 = make_config();
    EXPECT_TRUE(config3->http.url_stat.enable_trim_path) << "yes should be parsed as true";
    
    // Test "no"
    setenv(full_env(env::HTTP_URL_STAT_ENABLE_TRIM_PATH).c_str(), "no", 1);
    auto config4 = make_config();
    EXPECT_FALSE(config4->http.url_stat.enable_trim_path) << "no should be parsed as false";
}

// ========== Config::check() Validation Tests ==========

// Test check() passes with valid config
TEST_F(ConfigTest, CheckPassesWithValidConfigTest) {
    Config config;
    config.collector.host = "localhost";
    config.app_name_ = "MyApp";
    config.agent_id_ = "agent-001";
    config.agent_name_ = "AgentOne";

    EXPECT_TRUE(config.check()) << "Valid config should pass check";
}

// Test check() fails when collector host is empty
TEST_F(ConfigTest, CheckFailsEmptyCollectorHostTest) {
    Config config;
    config.collector.host = "";
    config.app_name_ = "MyApp";
    config.agent_id_ = "agent-001";

    EXPECT_FALSE(config.check()) << "Empty collector host should fail check";
}

// Test check() fails when app name is empty
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

    // Missing serviceName and apiKey -> unresolved.
    set_config_string(base);
    auto missing = make_config();
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->object_name_version_, 4);
    EXPECT_FALSE(missing->identity_resolved_);
    EXPECT_FALSE(missing->check());

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

// ========== Config::isReloadable() Tests ==========

// Test isReloadable() returns true when critical fields match
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

// Test isReloadable() returns false when app name changes
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

// Test isReloadable() returns false when collector host changes
TEST_F(ConfigTest, IsReloadableReturnsFalseWhenCollectorHostChangesTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "old.host";

    Config new_config = *old_config;
    new_config.collector.host = "new.host";

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when collector host changes";
}

// Test isReloadable() returns false when port changes
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

// Test isReloadable() returns false when gRPC channel options change
TEST_F(ConfigTest, IsReloadableReturnsFalseWhenGrpcChannelOptionsChangeTest) {
    auto old_config = std::make_shared<Config>();
    old_config->app_name_ = "MyApp";
    old_config->collector.host = "localhost";

    Config new_config = *old_config;
    new_config.collector.grpc.ssl.enable = true;

    EXPECT_FALSE(new_config.isReloadable(old_config))
        << "Should not be reloadable when gRPC channel options change";
}

// Test isReloadable() returns false when stat options change
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

// Test isReloadable() returns false when span queue size changes
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

// Test isReloadable() returns true when old config is null
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

    unsetenv("MYAPP_APPLICATION_NAME");
    unsetenv("MYAPP_SAMPLING_COUNTER_RATE");
    unsetenv("PINPOINT_CPP_APPLICATION_NAME");

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "custom-prefixed-app");
    EXPECT_EQ(config->sampling.counter_rate, 7);
}

// Test an empty prefix resets to the default PINPOINT_CPP prefix.
TEST_F(ConfigTest, EnvVarPrefixEmptyResetsToDefaultTest) {
    set_env_prefix("MYAPP");
    set_env_prefix("");  // reset to default

    setenv("PINPOINT_CPP_APPLICATION_NAME", "default-app", 1);
    setenv("MYAPP_APPLICATION_NAME", "should-be-ignored", 1);

    auto config = make_config();

    unsetenv("PINPOINT_CPP_APPLICATION_NAME");
    unsetenv("MYAPP_APPLICATION_NAME");

    ASSERT_NE(config, nullptr);
    EXPECT_EQ(config->app_name_, "default-app");
}

// Test the default (no override) prefix is PINPOINT_CPP.
TEST_F(ConfigTest, EnvVarPrefixDefaultIsPinpointCppTest) {
    setenv("PINPOINT_CPP_APPLICATION_NAME", "default-app", 1);

    auto config = make_config();

    unsetenv("PINPOINT_CPP_APPLICATION_NAME");

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

// Test port boundary values
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
)");

    const auto config = make_config();
    ASSERT_NE(config, nullptr);
    const auto& channel = config->collector.grpc.channel;
    EXPECT_EQ(channel.keepalive_time_ms, defaults::GRPC_KEEPALIVE_TIME_MS);
    EXPECT_EQ(channel.keepalive_timeout_ms, defaults::GRPC_KEEPALIVE_TIMEOUT_MS);
    EXPECT_EQ(channel.max_send_message_size, defaults::GRPC_MAX_MESSAGE_SIZE);
    EXPECT_EQ(channel.max_receive_message_size, defaults::GRPC_MAX_MESSAGE_SIZE);
    EXPECT_EQ(channel.sender_queue_size, defaults::GRPC_SENDER_QUEUE_SIZE);
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

// Test stat batch_count out of range
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

// Test stat collect_interval out of range
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
