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
#include <gtest/gtest.h>
#include <string>
#include <fstream>
#include <cstdlib>
#include <unistd.h>

namespace pinpoint {

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Save current environment variables
        SaveEnvironmentVariables();
        
        // Clear any existing config
        set_config_string("");
        
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
        saved_env_vars_[env::ENABLE] = GetEnvVar(env::ENABLE);
        saved_env_vars_[env::APPLICATION_NAME] = GetEnvVar(env::APPLICATION_NAME);
        saved_env_vars_[env::APPLICATION_TYPE] = GetEnvVar(env::APPLICATION_TYPE);
        saved_env_vars_[env::AGENT_ID] = GetEnvVar(env::AGENT_ID);
        saved_env_vars_[env::AGENT_NAME] = GetEnvVar(env::AGENT_NAME);
        saved_env_vars_[env::LOG_LEVEL] = GetEnvVar(env::LOG_LEVEL);
        saved_env_vars_[env::GRPC_HOST] = GetEnvVar(env::GRPC_HOST);
        saved_env_vars_[env::GRPC_AGENT_PORT] = GetEnvVar(env::GRPC_AGENT_PORT);
        saved_env_vars_[env::GRPC_SPAN_PORT] = GetEnvVar(env::GRPC_SPAN_PORT);
        saved_env_vars_[env::GRPC_STAT_PORT] = GetEnvVar(env::GRPC_STAT_PORT);
        saved_env_vars_[env::SAMPLING_TYPE] = GetEnvVar(env::SAMPLING_TYPE);
        saved_env_vars_[env::SAMPLING_PERCENT_RATE] = GetEnvVar(env::SAMPLING_PERCENT_RATE);
        saved_env_vars_[env::IS_CONTAINER] = GetEnvVar(env::IS_CONTAINER);
        saved_env_vars_[env::CONFIG_FILE] = GetEnvVar(env::CONFIG_FILE);
        saved_env_vars_[env::SQL_MAX_BIND_ARGS_SIZE] = GetEnvVar(env::SQL_MAX_BIND_ARGS_SIZE);
        saved_env_vars_[env::SQL_ENABLE_SQL_STATS] = GetEnvVar(env::SQL_ENABLE_SQL_STATS);
        saved_env_vars_[env::ENABLE_CALLSTACK_TRACE] = GetEnvVar(env::ENABLE_CALLSTACK_TRACE);
        saved_env_vars_[env::HTTP_COLLECT_URL_STAT] = GetEnvVar(env::HTTP_COLLECT_URL_STAT);
        saved_env_vars_[env::HTTP_URL_STAT_LIMIT] = GetEnvVar(env::HTTP_URL_STAT_LIMIT);
        saved_env_vars_[env::HTTP_URL_STAT_ENABLE_TRIM_PATH] = GetEnvVar(env::HTTP_URL_STAT_ENABLE_TRIM_PATH);
        saved_env_vars_[env::HTTP_URL_STAT_TRIM_PATH_DEPTH] = GetEnvVar(env::HTTP_URL_STAT_TRIM_PATH_DEPTH);
        saved_env_vars_[env::HTTP_URL_STAT_METHOD_PREFIX] = GetEnvVar(env::HTTP_URL_STAT_METHOD_PREFIX);
        
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
ApplicationType: 1300
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
)";

    const std::string partial_config_yaml_ = R"(
ApplicationName: "PartialApp"
ApplicationType: 1301

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
  EventChunkSize: 0
)";
};

// ========== Default Configuration Tests ==========

// Test default configuration values
TEST_F(ConfigTest, DefaultConfigurationTest) {
    Config config = make_config();
    
    // Test basic default values
    EXPECT_EQ(config.app_name_, "") << "Default app name should be empty";
    EXPECT_EQ(config.app_type_, 1300) << "Default app type should be 1300"; // defaults::APP_TYPE
    EXPECT_FALSE(config.agent_id_.empty()) << "Agent ID should be generated when empty, so not empty after make_config";
    EXPECT_EQ(config.agent_name_, "") << "Default agent name should be empty";
    EXPECT_TRUE(config.enable) << "Should be enabled by default";
    
    // Test log defaults
    EXPECT_EQ(config.log.level, "info") << "Default log level should be info";
    EXPECT_EQ(config.log.file_path, "") << "Default log file path should be empty";
    EXPECT_EQ(config.log.max_file_size, 10) << "Default max file size should be 10MB";
    
    // Test collector defaults
    EXPECT_EQ(config.collector.host, "") << "Default collector host should be empty";
    EXPECT_EQ(config.collector.agent_port, 9991) << "Default agent port should be 9991";
    EXPECT_EQ(config.collector.span_port, 9993) << "Default span port should be 9993";
    EXPECT_EQ(config.collector.stat_port, 9992) << "Default stat port should be 9992";
    
    // Test sampling defaults
    EXPECT_EQ(config.sampling.type, "COUNTER") << "Default sampling type should be COUNTER";
    EXPECT_EQ(config.sampling.counter_rate, 1) << "Default counter rate should be 1";
    EXPECT_EQ(config.sampling.percent_rate, 100) << "Default percent rate should be 100";
    EXPECT_EQ(config.sampling.new_throughput, 0) << "Default new throughput should be 0";
    EXPECT_EQ(config.sampling.cont_throughput, 0) << "Default continue throughput should be 0";
    
    // Test span defaults
    EXPECT_EQ(config.span.queue_size, 1024) << "Default queue size should be 1024";
    EXPECT_EQ(config.span.max_event_depth, 64) << "Default max event depth should be 64";
    EXPECT_EQ(config.span.max_event_sequence, 5000) << "Default max event sequence should be 5000";
    EXPECT_EQ(config.span.event_chunk_size, 20) << "Default event chunk size should be 20";
    
    // Test stat defaults
    EXPECT_TRUE(config.stat.enable) << "Stat should be enabled by default";
    EXPECT_EQ(config.stat.batch_count, 6) << "Default batch count should be 6";
    EXPECT_EQ(config.stat.collect_interval, 5000) << "Default collect interval should be 5000ms";
    
    // Test HTTP defaults
    EXPECT_FALSE(config.http.url_stat.enable) << "URL stat should be disabled by default";
    EXPECT_EQ(config.http.url_stat.limit, 1024) << "Default URL stat limit should be 1024";
    EXPECT_TRUE(config.http.url_stat.enable_trim_path) << "Enable trim path should be true by default";
    EXPECT_EQ(config.http.url_stat.trim_path_depth, 1) << "Default path depth should be 1";
    EXPECT_FALSE(config.http.url_stat.method_prefix) << "Method prefix should be false by default";
    
    // Test HTTP server defaults
    EXPECT_EQ(config.http.server.status_errors.size(), 1) << "Should have default status error";
    EXPECT_EQ(config.http.server.status_errors[0], "5xx") << "Default status error should be 5xx";
    EXPECT_TRUE(config.http.server.exclude_url.empty()) << "Exclude URL list should be empty by default";
    EXPECT_TRUE(config.http.server.exclude_method.empty()) << "Exclude method list should be empty by default";
    EXPECT_TRUE(config.http.server.rec_request_header.empty()) << "Request header list should be empty by default";
    EXPECT_TRUE(config.http.server.rec_request_cookie.empty()) << "Request cookie list should be empty by default";
    EXPECT_TRUE(config.http.server.rec_response_header.empty()) << "Response header list should be empty by default";
    
    // Test HTTP client defaults
    EXPECT_TRUE(config.http.client.rec_request_header.empty()) << "Client request header list should be empty by default";
    EXPECT_TRUE(config.http.client.rec_request_cookie.empty()) << "Client request cookie list should be empty by default";
    EXPECT_TRUE(config.http.client.rec_response_header.empty()) << "Client response header list should be empty by default";
    
    // Test SQL defaults
    EXPECT_EQ(config.sql.max_bind_args_size, 1024) << "Default max bind args size should be 1024";
    EXPECT_FALSE(config.sql.enable_sql_stats) << "SQL stats should be disabled by default";
    
    // Test CallStack trace default
    EXPECT_FALSE(config.enable_callstack_trace) << "CallStack trace should be disabled by default";
}

// Test generated agent ID
TEST_F(ConfigTest, GeneratedAgentIdTest) {
    Config config = make_config();
    
    EXPECT_FALSE(config.agent_id_.empty()) << "Agent ID should be generated when not provided";
    EXPECT_GE(config.agent_id_.length(), 5) << "Generated agent ID should have reasonable length";
    
    // Test that multiple calls generate different IDs
    Config config2 = make_config();
    EXPECT_NE(config.agent_id_, config2.agent_id_) << "Multiple calls should generate different agent IDs";
}

// ========== YAML Configuration Tests ==========

// Test complete YAML configuration
TEST_F(ConfigTest, CompleteYamlConfigurationTest) {
    set_config_string(complete_config_yaml_);
    Config config = make_config();
    
    // Test basic values
    EXPECT_EQ(config.app_name_, "MyTestApp") << "App name should match YAML";
    EXPECT_EQ(config.app_type_, 1300) << "App type should match YAML";
    EXPECT_EQ(config.agent_id_, "test-agent-123") << "Agent ID should match YAML";
    EXPECT_EQ(config.agent_name_, "TestAgentName") << "Agent name should match YAML";
    EXPECT_TRUE(config.enable) << "Enable should match YAML";
    EXPECT_TRUE(config.is_container) << "IsContainer should match YAML";
    
    // Test log configuration
    EXPECT_EQ(config.log.level, "debug") << "Log level should match YAML";
    EXPECT_EQ(config.log.file_path, "/tmp/pinpoint.log") << "Log file path should match YAML";
    EXPECT_EQ(config.log.max_file_size, 20) << "Log max file size should match YAML";
    
    // Test collector configuration
    EXPECT_EQ(config.collector.host, "test.collector.host") << "Collector host should match YAML";
    EXPECT_EQ(config.collector.agent_port, 9000) << "Agent port should match YAML";
    EXPECT_EQ(config.collector.span_port, 9001) << "Span port should match YAML";
    EXPECT_EQ(config.collector.stat_port, 9002) << "Stat port should match YAML";
    
    // Test sampling configuration
    EXPECT_EQ(config.sampling.type, "PERCENT") << "Sampling type should match YAML";
    EXPECT_EQ(config.sampling.counter_rate, 20) << "Counter rate should match YAML";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 0.1) << "Percent rate should match YAML";
    EXPECT_EQ(config.sampling.new_throughput, 50) << "New throughput should match YAML";
    EXPECT_EQ(config.sampling.cont_throughput, 60) << "Continue throughput should match YAML";
    
    // Test span configuration
    EXPECT_EQ(config.span.queue_size, 512) << "Queue size should match YAML";
    EXPECT_EQ(config.span.max_event_depth, 32) << "Max event depth should match YAML";
    EXPECT_EQ(config.span.max_event_sequence, 512) << "Max event sequence should match YAML";
    EXPECT_EQ(config.span.event_chunk_size, 50) << "Event chunk size should match YAML";
    
    // Test stat configuration
    EXPECT_TRUE(config.stat.enable) << "Stat enable should match YAML";
    EXPECT_EQ(config.stat.batch_count, 10) << "Batch count should match YAML";
    EXPECT_EQ(config.stat.collect_interval, 7000) << "Collect interval should match YAML";
    
    // Test HTTP configuration
    EXPECT_TRUE(config.http.url_stat.enable) << "URL stat enable should match YAML";
    EXPECT_EQ(config.http.url_stat.limit, 2048) << "URL stat limit should match YAML";
    EXPECT_FALSE(config.http.url_stat.enable_trim_path) << "URL stat enable trim path should match YAML";
    EXPECT_EQ(config.http.url_stat.trim_path_depth, 3) << "URL stat path depth should match YAML";
    EXPECT_TRUE(config.http.url_stat.method_prefix) << "URL stat method prefix should match YAML";
    
    // Test HTTP server configuration
    EXPECT_EQ(config.http.server.status_errors.size(), 3) << "Should have 3 status errors";
    EXPECT_EQ(config.http.server.status_errors[0], "5xx") << "First status error should be 5xx";
    EXPECT_EQ(config.http.server.status_errors[1], "401") << "Second status error should be 401";
    EXPECT_EQ(config.http.server.status_errors[2], "403") << "Third status error should be 403";
    
    EXPECT_EQ(config.http.server.exclude_url.size(), 2) << "Should have 2 exclude URLs";
    EXPECT_EQ(config.http.server.exclude_url[0], "/health") << "First exclude URL should be /health";
    EXPECT_EQ(config.http.server.exclude_url[1], "/metrics") << "Second exclude URL should be /metrics";
    
    EXPECT_EQ(config.http.server.exclude_method.size(), 2) << "Should have 2 exclude methods";
    EXPECT_EQ(config.http.server.exclude_method[0], "PUT") << "First exclude method should be PUT";
    EXPECT_EQ(config.http.server.exclude_method[1], "DELETE") << "Second exclude method should be DELETE";
    
    EXPECT_EQ(config.http.server.rec_request_header.size(), 2) << "Should have 2 request headers";
    EXPECT_EQ(config.http.server.rec_request_header[0], "Authorization") << "First request header should be Authorization";
    EXPECT_EQ(config.http.server.rec_request_header[1], "Accept") << "Second request header should be Accept";
    
    EXPECT_EQ(config.http.server.rec_request_cookie.size(), 1) << "Should have 1 request cookie";
    EXPECT_EQ(config.http.server.rec_request_cookie[0], "session") << "Request cookie should be session";
    
    EXPECT_EQ(config.http.server.rec_response_header.size(), 1) << "Should have 1 response header";
    EXPECT_EQ(config.http.server.rec_response_header[0], "Content-Type") << "Response header should be Content-Type";
    
    // Test HTTP client configuration
    EXPECT_EQ(config.http.client.rec_request_header.size(), 1) << "Should have 1 client request header";
    EXPECT_EQ(config.http.client.rec_request_header[0], "User-Agent") << "Client request header should be User-Agent";
    
    EXPECT_EQ(config.http.client.rec_request_cookie.size(), 1) << "Should have 1 client request cookie";
    EXPECT_EQ(config.http.client.rec_request_cookie[0], "tracking") << "Client request cookie should be tracking";
    
    EXPECT_EQ(config.http.client.rec_response_header.size(), 1) << "Should have 1 client response header";
    EXPECT_EQ(config.http.client.rec_response_header[0], "headers-all") << "Client response header should be headers-all";
    
    // Test SQL configuration
    EXPECT_EQ(config.sql.max_bind_args_size, 2048) << "Max bind args size should match YAML";
    EXPECT_TRUE(config.sql.enable_sql_stats) << "SQL stats should be enabled as per YAML";
}

// Test partial YAML configuration
TEST_F(ConfigTest, PartialYamlConfigurationTest) {
    set_config_string(partial_config_yaml_);
    Config config = make_config();
    
    // Test overridden values
    EXPECT_EQ(config.app_name_, "PartialApp") << "App name should match partial YAML";
    EXPECT_EQ(config.app_type_, 1301) << "App type should match partial YAML";
    EXPECT_EQ(config.collector.host, "partial.host") << "Collector host should match partial YAML";
    EXPECT_EQ(config.collector.agent_port, 8000) << "Agent port should match partial YAML";
    EXPECT_EQ(config.sampling.type, "COUNTER") << "Sampling type should match partial YAML";
    EXPECT_EQ(config.sampling.counter_rate, 5) << "Counter rate should match partial YAML";
    
    EXPECT_EQ(config.http.server.status_errors.size(), 1) << "Should have 1 status error from partial YAML";
    EXPECT_EQ(config.http.server.status_errors[0], "4xx") << "Status error should be 4xx from partial YAML";
    
    // Test values that should remain default
    EXPECT_EQ(config.collector.span_port, 9993) << "Span port should remain default";
    EXPECT_EQ(config.collector.stat_port, 9992) << "Stat port should remain default";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 100) << "Percent rate should remain default";
    EXPECT_EQ(config.log.level, "info") << "Log level should remain default";
    EXPECT_EQ(config.span.queue_size, 1024) << "Queue size should remain default";
    
    // Test SQL configuration from partial YAML
    EXPECT_EQ(config.sql.max_bind_args_size, 512) << "Max bind args size should match partial YAML";
    EXPECT_FALSE(config.sql.enable_sql_stats) << "SQL stats should be disabled as per partial YAML";
}

// Test empty YAML configuration
TEST_F(ConfigTest, EmptyYamlConfigurationTest) {
    set_config_string("");
    Config config = make_config();
    
    // Should have all default values
    EXPECT_EQ(config.app_name_, "") << "App name should be default (empty)";
    EXPECT_EQ(config.app_type_, 1300) << "App type should be default";
    EXPECT_EQ(config.log.level, "info") << "Log level should be default";
    EXPECT_EQ(config.collector.agent_port, 9991) << "Agent port should be default";
}

// ========== Environment Variable Tests ==========

// Test environment variable configuration
TEST_F(ConfigTest, EnvironmentVariableConfigurationTest) {
    // Set environment variables
    setenv(env::APPLICATION_NAME, "EnvApp", 1);
    setenv(env::APPLICATION_TYPE, "1302", 1);
    setenv(env::AGENT_ID, "env-agent-456", 1);
    setenv(env::LOG_LEVEL, "error", 1);
    setenv(env::GRPC_HOST, "env.collector.host", 1);
    setenv(env::GRPC_AGENT_PORT, "8888", 1);
    setenv(env::SAMPLING_TYPE, "PERCENT", 1);
    setenv(env::SAMPLING_PERCENT_RATE, "25.5", 1);
    setenv(env::IS_CONTAINER, "true", 1);
    setenv(env::SQL_MAX_BIND_ARGS_SIZE, "4096", 1);
    setenv(env::SQL_ENABLE_SQL_STATS, "true", 1);
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "false", 1);
    
    Config config = make_config();
    
    // Test environment variable values
    EXPECT_EQ(config.app_name_, "EnvApp") << "App name should match environment variable";
    EXPECT_EQ(config.app_type_, 1302) << "App type should match environment variable";
    EXPECT_EQ(config.agent_id_, "env-agent-456") << "Agent ID should match environment variable";
    EXPECT_EQ(config.log.level, "error") << "Log level should match environment variable";
    EXPECT_EQ(config.collector.host, "env.collector.host") << "Collector host should match environment variable";
    EXPECT_EQ(config.collector.agent_port, 8888) << "Agent port should match environment variable";
    EXPECT_EQ(config.sampling.type, "PERCENT") << "Sampling type should match environment variable";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 25.5) << "Percent rate should match environment variable";
    EXPECT_TRUE(config.is_container) << "IsContainer should match environment variable";
    
    // Test SQL environment variable values
    EXPECT_EQ(config.sql.max_bind_args_size, 4096) << "Max bind args size should match environment variable";
    EXPECT_TRUE(config.sql.enable_sql_stats) << "SQL stats should be enabled as per environment variable";
    
    // Test HTTP environment variable values
    EXPECT_FALSE(config.http.url_stat.enable_trim_path) << "URL stat enable trim path should match environment variable";
}

// Test environment variable override YAML
TEST_F(ConfigTest, EnvironmentVariableOverrideYamlTest) {
    // Set YAML config
    set_config_string(partial_config_yaml_);
    
    // Set environment variables that should override YAML
    setenv(env::APPLICATION_NAME, "EnvOverrideApp", 1);
    setenv(env::GRPC_HOST, "env.override.host", 1);
    setenv(env::SQL_MAX_BIND_ARGS_SIZE, "8192", 1);
    setenv(env::SQL_ENABLE_SQL_STATS, "true", 1);
    
    Config config = make_config();
    
    // Environment variables should override YAML
    EXPECT_EQ(config.app_name_, "EnvOverrideApp") << "Environment variable should override YAML app name";
    EXPECT_EQ(config.collector.host, "env.override.host") << "Environment variable should override YAML collector host";
    EXPECT_EQ(config.sql.max_bind_args_size, 8192) << "Environment variable should override YAML max bind args size";
    EXPECT_TRUE(config.sql.enable_sql_stats) << "Environment variable should override YAML SQL stats setting";
    
    // YAML values should remain where no environment variable is set
    EXPECT_EQ(config.app_type_, 1301) << "App type should remain from YAML";
    EXPECT_EQ(config.collector.agent_port, 8000) << "Agent port should remain from YAML";
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
    setenv(env::CONFIG_FILE, config_file.c_str(), 1);
    
    Config config = make_config();
    
    // Values should be loaded from file
    EXPECT_EQ(config.app_name_, "PartialApp") << "App name should be loaded from file";
    EXPECT_EQ(config.app_type_, 1301) << "App type should be loaded from file";
    EXPECT_EQ(config.collector.host, "partial.host") << "Collector host should be loaded from file";
}

// Test missing configuration file
TEST_F(ConfigTest, MissingConfigurationFileTest) {
    // Set environment variable to point to non-existent file
    std::string missing_file = temp_dir_ + "/missing_config.yaml";
    setenv(env::CONFIG_FILE, missing_file.c_str(), 1);
    
    Config config = make_config();
    
    // Should use default values when file is missing
    EXPECT_EQ(config.app_name_, "") << "App name should be default when file is missing";
    EXPECT_EQ(config.app_type_, 1300) << "App type should be default when file is missing";
}

// ========== Validation Logic Tests ==========

// Test value validation and correction
TEST_F(ConfigTest, ValueValidationTest) {
    set_config_string(extreme_values_yaml_);
    Config config = make_config();
    
    // Test sampling value corrections
    EXPECT_EQ(config.sampling.counter_rate, 0) << "Negative counter rate should be corrected to 0";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 100) << "Percent rate > 100 should be corrected to 100";
    EXPECT_EQ(config.sampling.new_throughput, 0) << "Negative new throughput should be corrected to 0";
    EXPECT_EQ(config.sampling.cont_throughput, 0) << "Negative continue throughput should be corrected to 0";
    
    // Test span value corrections
    EXPECT_EQ(config.span.queue_size, 1024) << "Queue size < 1 should be corrected to default (1024)";
    EXPECT_EQ(config.span.max_event_depth, INT32_MAX) << "Max event depth -1 should be corrected to INT32_MAX";
    EXPECT_EQ(config.span.max_event_sequence, INT32_MAX) << "Max event sequence -1 should be corrected to INT32_MAX";
    EXPECT_EQ(config.span.event_chunk_size, 20) << "Event chunk size < 1 should be corrected to default (20)";
}

// Test edge case percent rates
TEST_F(ConfigTest, PercentRateEdgeCasesTest) {
    // Test very small percent rate
    std::string small_percent_yaml = R"(
Sampling:
  PercentRate: 0.005
)";
    set_config_string(small_percent_yaml);
    Config config = make_config();
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 0.01) << "Very small percent rate should be corrected to minimum (0.01)";
    
    // Test negative percent rate
    std::string negative_percent_yaml = R"(
Sampling:
  PercentRate: -5.0
)";
    set_config_string(negative_percent_yaml);
    config = make_config();
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 0) << "Negative percent rate should be corrected to 0";
}

// ========== Error Handling Tests ==========

// Test invalid YAML handling
TEST_F(ConfigTest, InvalidYamlHandlingTest) {
    set_config_string(invalid_yaml_);
    Config config = make_config();
    
    // Should fallback to default values when YAML is invalid
    EXPECT_EQ(config.app_name_, "") << "App name should be default when YAML is invalid";
    EXPECT_EQ(config.app_type_, 1300) << "App type should be default when YAML is invalid";
    EXPECT_EQ(config.log.level, "info") << "Log level should be default when YAML is invalid";
}

// ========== Configuration String Generation Tests ==========

// Test configuration to string conversion
TEST_F(ConfigTest, ConfigurationToStringTest) {
    set_config_string(complete_config_yaml_);
    Config config = make_config();
    
    std::string config_string = to_config_string(config);
    
    // Check that generated string contains expected values
    EXPECT_TRUE(config_string.find("ApplicationName: MyTestApp") != std::string::npos) 
        << "Config string should contain application name";
    EXPECT_TRUE(config_string.find("ApplicationType: 1300") != std::string::npos) 
        << "Config string should contain application type";
    EXPECT_TRUE(config_string.find("GrpcHost: test.collector.host") != std::string::npos) 
        << "Config string should contain GRPC host";
    EXPECT_TRUE(config_string.find("Type: PERCENT") != std::string::npos) 
        << "Config string should contain sampling type";
    EXPECT_TRUE(config_string.find("Level: debug") != std::string::npos) 
        << "Config string should contain log level";
}

// Test round-trip configuration (string -> config -> string)
TEST_F(ConfigTest, ConfigurationRoundTripTest) {
    set_config_string(complete_config_yaml_);
    Config config = make_config();
    
    std::string generated_config_string = to_config_string(config);
    
    // Use generated string as new config
    set_config_string(generated_config_string);
    Config config2 = make_config();
    
    // Both configs should have same values
    EXPECT_EQ(config.app_name_, config2.app_name_) << "App name should match after round-trip";
    EXPECT_EQ(config.app_type_, config2.app_type_) << "App type should match after round-trip";
    EXPECT_EQ(config.collector.host, config2.collector.host) << "Collector host should match after round-trip";
    EXPECT_EQ(config.sampling.type, config2.sampling.type) << "Sampling type should match after round-trip";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, config2.sampling.percent_rate) << "Percent rate should match after round-trip";
    EXPECT_EQ(config.http.server.status_errors.size(), config2.http.server.status_errors.size()) 
        << "Status errors count should match after round-trip";
}

// Test empty configuration string generation
TEST_F(ConfigTest, EmptyConfigurationStringTest) {
    set_config_string("");
    Config config = make_config();
    
    std::string config_string = to_config_string(config);
    
    // Should contain default values
    EXPECT_TRUE(config_string.find("ApplicationType: 1300") != std::string::npos) 
        << "Config string should contain default application type";
    EXPECT_TRUE(config_string.find("Level: info") != std::string::npos) 
        << "Config string should contain default log level";
    EXPECT_TRUE(config_string.find("Type: COUNTER") != std::string::npos) 
        << "Config string should contain default sampling type";
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
    setenv(env::CONFIG_FILE, config_file.c_str(), 1);
    setenv(env::APPLICATION_NAME, "OverriddenApp", 1); // Should override file
    setenv(env::LOG_LEVEL, "warn", 1); // Should override file
    
    Config config = make_config();
    
    // Environment variables should override file values
    EXPECT_EQ(config.app_name_, "OverriddenApp") << "Environment variable should override file app name";
    EXPECT_EQ(config.log.level, "warn") << "Environment variable should override file log level";
    
    // File values should be used where no environment variable exists
    EXPECT_EQ(config.app_type_, 1300) << "App type should come from file";
    EXPECT_EQ(config.collector.host, "test.collector.host") << "Collector host should come from file";
    EXPECT_EQ(config.agent_id_, "test-agent-123") << "Agent ID should come from file";
}

// ========== Exception Handling Tests ==========

// Test type conversion exception handling
TEST_F(ConfigTest, TypeConversionExceptionHandlingTest) {
    // YAML with invalid type conversions
    std::string invalid_type_yaml = R"(
ApplicationName: ValidString
ApplicationType: "not_a_number"  # Should be int, will use default
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
    
    Config config = make_config();
    
    std::string captured_output = testing::internal::GetCapturedStderr();
    
    // Valid conversions should work
    EXPECT_EQ(config.app_name_, "ValidString") << "Valid string should be parsed correctly";
    
    // Invalid conversions should use default values
    EXPECT_EQ(config.app_type_, 1300) << "Invalid int should use default value";
    EXPECT_TRUE(config.enable) << "Invalid bool should use default value (true)";
    EXPECT_EQ(config.collector.agent_port, 9991) << "Invalid port should use default value";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 100.0) << "Invalid double should use default value";
    EXPECT_EQ(config.http.server.status_errors.size(), 1) << "Invalid array should use default value";
    EXPECT_EQ(config.http.server.status_errors[0], "5xx") << "Default array should contain '5xx'";
    
    // Note: We can't easily check stderr output in this test environment without additional setup
    // but the error messages should be logged for invalid conversions
}

// Test mixed valid and invalid configurations
TEST_F(ConfigTest, MixedValidInvalidConfigurationTest) {
    std::string mixed_yaml = R"(
ApplicationName: ValidApp
ApplicationType: 1305            # Valid int
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
    Config config = make_config();
    
    // Valid values should be parsed correctly
    EXPECT_EQ(config.app_name_, "ValidApp") << "Valid app name should be parsed";
    EXPECT_EQ(config.app_type_, 1305) << "Valid app type should be parsed";
    EXPECT_TRUE(config.enable) << "Valid enable should be parsed";
    EXPECT_EQ(config.collector.host, "valid.host.com") << "Valid host should be parsed";
    EXPECT_EQ(config.collector.span_port, 9999) << "Valid span port should be parsed";
    EXPECT_EQ(config.sampling.type, "PERCENT") << "Valid sampling type should be parsed";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 50.5) << "Valid percent rate should be parsed";
    EXPECT_EQ(config.span.queue_size, 2048) << "Valid queue size should be parsed";
    
    // Invalid values should use defaults
    EXPECT_EQ(config.collector.agent_port, 9991) << "Invalid agent port should use default";
    EXPECT_EQ(config.sampling.counter_rate, 1) << "Invalid counter rate should use default";
    EXPECT_EQ(config.span.max_event_depth, 64) << "Invalid max event depth should use default";
}

// ========== Environment Variable Validation Tests ==========

// Test environment variable validation for invalid values
TEST_F(ConfigTest, EnvironmentVariableValidationTest) {
    // Set invalid environment variables
    setenv(env::ENABLE, "invalid_bool", 1);                  // Invalid bool
    setenv(env::APPLICATION_TYPE, "not_a_number", 1);        // Invalid int
    setenv(env::GRPC_AGENT_PORT, "invalid_port", 1);         // Invalid int
    setenv(env::SAMPLING_PERCENT_RATE, "not_a_double", 1);   // Invalid double
    setenv(env::STAT_ENABLE, "maybe", 1);                    // Invalid bool
    setenv(env::SPAN_QUEUE_SIZE, "abc", 1);                  // Invalid int
    
    Config config = make_config();
    
    // All invalid values should use defaults
    EXPECT_TRUE(config.enable) << "Invalid bool should use default value (true)";
    EXPECT_EQ(config.app_type_, 1300) << "Invalid int should use default value";
    EXPECT_EQ(config.collector.agent_port, 9991) << "Invalid port should use default value";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 100.0) << "Invalid double should use default value";
    EXPECT_TRUE(config.stat.enable) << "Invalid bool should use default value (true)";
    EXPECT_EQ(config.span.queue_size, 1024) << "Invalid int should use default value";
}

// Test environment variable validation for valid values
TEST_F(ConfigTest, EnvironmentVariableValidValuesTest) {
    // Set valid environment variables
    setenv(env::ENABLE, "false", 1);                  // Valid bool
    setenv(env::APPLICATION_TYPE, "1500", 1);         // Valid int
    setenv(env::GRPC_AGENT_PORT, "8080", 1);          // Valid int
    setenv(env::SAMPLING_PERCENT_RATE, "75.5", 1);    // Valid double
    setenv(env::STAT_ENABLE, "1", 1);                 // Valid bool
    setenv(env::SPAN_QUEUE_SIZE, "2048", 1);          // Valid int
    
    Config config = make_config();
    
    // All valid values should be parsed correctly
    EXPECT_FALSE(config.enable) << "Valid bool should be parsed correctly";
    EXPECT_EQ(config.app_type_, 1500) << "Valid int should be parsed correctly";
    EXPECT_EQ(config.collector.agent_port, 8080) << "Valid port should be parsed correctly";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 75.5) << "Valid double should be parsed correctly";
    EXPECT_TRUE(config.stat.enable) << "Valid bool should be parsed correctly";
    EXPECT_EQ(config.span.queue_size, 2048) << "Valid int should be parsed correctly";
}

// Test environment variable validation for boolean edge cases
TEST_F(ConfigTest, EnvironmentVariableBooleanEdgeCasesTest) {
    // Test various valid boolean representations
    setenv(env::ENABLE, "TRUE", 1);
    setenv(env::STAT_ENABLE, "False", 1);
    setenv(env::IS_CONTAINER, "yes", 1);
    setenv(env::HTTP_COLLECT_URL_STAT, "NO", 1);
    
    Config config = make_config();
    
    EXPECT_TRUE(config.enable) << "TRUE should be parsed as true";
    EXPECT_FALSE(config.stat.enable) << "False should be parsed as false";
    EXPECT_TRUE(config.is_container) << "yes should be parsed as true";
    EXPECT_FALSE(config.http.url_stat.enable) << "NO should be parsed as false";
}

// Test environment variable validation for negative values
TEST_F(ConfigTest, EnvironmentVariableNegativeValuesTest) {
    // Set valid negative values where applicable
    setenv(env::SPAN_MAX_EVENT_DEPTH, "-1", 1);       // Valid -1 (should be processed by make_config validation)
    setenv(env::SPAN_MAX_EVENT_SEQUENCE, "-1", 1);    // Valid -1 (should be processed by make_config validation)
    
    Config config = make_config();
    
    // These should be parsed as -1 and then validated by make_config to INT32_MAX
    EXPECT_EQ(config.span.max_event_depth, INT32_MAX) << "-1 should be converted to INT32_MAX by make_config";
    EXPECT_EQ(config.span.max_event_sequence, INT32_MAX) << "-1 should be converted to INT32_MAX by make_config";
}

// ========== SQL Configuration Specific Tests ==========

// Test SQL configuration with various bind args sizes
TEST_F(ConfigTest, SqlMaxBindArgsSizeValidationTest) {
    // Test default
    Config config1 = make_config();
    EXPECT_EQ(config1.sql.max_bind_args_size, 1024) << "Default max bind args size should be 1024";
    
    // Test YAML configuration with different values
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 256
  EnableSqlStats: true
)");
    Config config2 = make_config();
    EXPECT_EQ(config2.sql.max_bind_args_size, 256) << "Max bind args size should match YAML";
    EXPECT_TRUE(config2.sql.enable_sql_stats) << "SQL stats should be enabled as per YAML";
    
    // Test environment variable override
    setenv(env::SQL_MAX_BIND_ARGS_SIZE, "16384", 1);
    Config config3 = make_config();
    EXPECT_EQ(config3.sql.max_bind_args_size, 16384) << "Environment variable should override YAML";
}

// Test SQL stats enable/disable configurations  
TEST_F(ConfigTest, SqlStatsEnableTest) {
    // Test default (disabled)
    Config config1 = make_config();
    EXPECT_FALSE(config1.sql.enable_sql_stats) << "SQL stats should be disabled by default";
    
    // Test enabling via YAML
    set_config_string(R"(
Sql:
  EnableSqlStats: true
)");
    Config config2 = make_config();
    EXPECT_TRUE(config2.sql.enable_sql_stats) << "SQL stats should be enabled as per YAML";
    
    // Test enabling via environment variable
    set_config_string("");
    setenv(env::SQL_ENABLE_SQL_STATS, "true", 1);
    Config config3 = make_config();
    EXPECT_TRUE(config3.sql.enable_sql_stats) << "SQL stats should be enabled as per environment variable";
    
    // Test disabling via environment variable  
    setenv(env::SQL_ENABLE_SQL_STATS, "false", 1);
    Config config4 = make_config();
    EXPECT_FALSE(config4.sql.enable_sql_stats) << "SQL stats should be disabled as per environment variable";
}

// Test SQL configuration edge cases
TEST_F(ConfigTest, SqlConfigurationEdgeCasesTest) {
    // Test zero bind args size
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 0
  EnableSqlStats: false
)");
    Config config1 = make_config();
    EXPECT_EQ(config1.sql.max_bind_args_size, 0) << "Zero bind args size should be allowed";
    EXPECT_FALSE(config1.sql.enable_sql_stats) << "SQL stats should be disabled";
    
    // Test very large bind args size
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 1048576
  EnableSqlStats: true
)");
    Config config2 = make_config();
    EXPECT_EQ(config2.sql.max_bind_args_size, 1048576) << "Large bind args size should be allowed";
    EXPECT_TRUE(config2.sql.enable_sql_stats) << "SQL stats should be enabled";
}

// Test SQL configuration string generation
TEST_F(ConfigTest, SqlConfigurationToStringTest) {
    set_config_string(R"(
Sql:
  MaxBindArgsSize: 2048
  EnableSqlStats: true
)");
    Config config = make_config();
    
    std::string config_string = to_config_string(config);
    
    // Check that SQL configuration is included in generated string
    EXPECT_TRUE(config_string.find("MaxBindArgsSize: 2048") != std::string::npos) 
        << "Config string should contain SQL max bind args size";
    EXPECT_TRUE(config_string.find("EnableSqlStats: true") != std::string::npos) 
        << "Config string should contain SQL stats enable setting";
}

// Test SQL configuration round-trip
TEST_F(ConfigTest, SqlConfigurationRoundTripTest) {
    const std::string sql_config = R"(
ApplicationName: "SqlTestApp"
Sql:
  MaxBindArgsSize: 3072
  EnableSqlStats: true
)";
    
    set_config_string(sql_config);
    Config config1 = make_config();
    
    std::string generated_config_string = to_config_string(config1);
    
    // Use generated string as new config
    set_config_string(generated_config_string);
    Config config2 = make_config();
    
    // SQL configs should match after round-trip
    EXPECT_EQ(config1.sql.max_bind_args_size, config2.sql.max_bind_args_size) 
        << "Max bind args size should match after round-trip";
    EXPECT_EQ(config1.sql.enable_sql_stats, config2.sql.enable_sql_stats) 
        << "SQL stats enable should match after round-trip";
}

// Test invalid SQL environment variable values
TEST_F(ConfigTest, SqlInvalidEnvironmentVariableTest) {
    // Test invalid max bind args size (should fallback to default)
    setenv(env::SQL_MAX_BIND_ARGS_SIZE, "invalid", 1);
    setenv(env::SQL_ENABLE_SQL_STATS, "invalid", 1);
    
    Config config = make_config();
    
    // Should fallback to defaults when environment variable is invalid
    EXPECT_EQ(config.sql.max_bind_args_size, 1024) << "Should use default when env var is invalid";
    EXPECT_FALSE(config.sql.enable_sql_stats) << "Should use default when env var is invalid";
}

// ========== CallStack Trace Configuration Tests ==========

// Test default callstack trace configuration
TEST_F(ConfigTest, CallstackTraceDefaultTest) {
    Config config = make_config();
    
    // Default should be false
    EXPECT_FALSE(config.enable_callstack_trace) << "CallStack trace should be disabled by default";
}

// Test enabling callstack trace via YAML
TEST_F(ConfigTest, CallstackTraceEnableViaYamlTest) {
    set_config_string(R"(
EnableCallstackTrace: true
)");
    Config config = make_config();
    
    EXPECT_TRUE(config.enable_callstack_trace) << "CallStack trace should be enabled as per YAML";
}

// Test disabling callstack trace via YAML
TEST_F(ConfigTest, CallstackTraceDisableViaYamlTest) {
    set_config_string(R"(
EnableCallstackTrace: false
)");
    Config config = make_config();
    
    EXPECT_FALSE(config.enable_callstack_trace) << "CallStack trace should be disabled as per YAML";
}

// Test enabling callstack trace via environment variable
TEST_F(ConfigTest, CallstackTraceEnableViaEnvironmentVariableTest) {
    setenv(env::ENABLE_CALLSTACK_TRACE, "true", 1);
    
    Config config = make_config();
    
    EXPECT_TRUE(config.enable_callstack_trace) << "CallStack trace should be enabled as per environment variable";
}

// Test disabling callstack trace via environment variable
TEST_F(ConfigTest, CallstackTraceDisableViaEnvironmentVariableTest) {
    setenv(env::ENABLE_CALLSTACK_TRACE, "false", 1);
    
    Config config = make_config();
    
    EXPECT_FALSE(config.enable_callstack_trace) << "CallStack trace should be disabled as per environment variable";
}

// Test environment variable overrides YAML for callstack trace
TEST_F(ConfigTest, CallstackTraceEnvironmentVariableOverrideYamlTest) {
    // Set YAML to disable
    set_config_string(R"(
EnableCallstackTrace: false
)");
    
    // Set environment variable to enable (should override YAML)
    setenv(env::ENABLE_CALLSTACK_TRACE, "true", 1);
    
    Config config = make_config();
    
    EXPECT_TRUE(config.enable_callstack_trace) << "Environment variable should override YAML for callstack trace";
}

// Test environment variable overrides YAML (opposite case)
TEST_F(ConfigTest, CallstackTraceEnvironmentVariableOverrideYamlOppositeTest) {
    // Set YAML to enable
    set_config_string(R"(
EnableCallstackTrace: true
)");
    
    // Set environment variable to disable (should override YAML)
    setenv(env::ENABLE_CALLSTACK_TRACE, "false", 1);
    
    Config config = make_config();
    
    EXPECT_FALSE(config.enable_callstack_trace) << "Environment variable should override YAML for callstack trace";
}

// Test callstack trace in complete configuration
TEST_F(ConfigTest, CallstackTraceInCompleteConfigurationTest) {
    const std::string complete_config = R"(
ApplicationName: "CallstackTestApp"
ApplicationType: 1300
EnableCallstackTrace: true

Log:
  Level: "debug"

Collector:
  GrpcHost: "test.host"
  GrpcAgentPort: 9000
)";
    
    set_config_string(complete_config);
    Config config = make_config();
    
    EXPECT_EQ(config.app_name_, "CallstackTestApp") << "App name should match";
    EXPECT_TRUE(config.enable_callstack_trace) << "CallStack trace should be enabled";
    EXPECT_EQ(config.log.level, "debug") << "Other config values should also be loaded";
}

// Test callstack trace configuration string generation
TEST_F(ConfigTest, CallstackTraceConfigurationToStringTest) {
    set_config_string(R"(
EnableCallstackTrace: true
)");
    Config config = make_config();
    
    std::string config_string = to_config_string(config);
    
    // Check that callstack trace configuration is included in generated string
    EXPECT_TRUE(config_string.find("EnableCallstackTrace: true") != std::string::npos) 
        << "Config string should contain EnableCallstackTrace setting";
}

// Test callstack trace configuration string generation when disabled
TEST_F(ConfigTest, CallstackTraceConfigurationToStringDisabledTest) {
    set_config_string(R"(
EnableCallstackTrace: false
)");
    Config config = make_config();
    
    std::string config_string = to_config_string(config);
    
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
    Config config1 = make_config();
    
    EXPECT_TRUE(config1.enable_callstack_trace) << "Initial config should have callstack trace enabled";
    
    std::string generated_config_string = to_config_string(config1);
    
    // Use generated string as new config
    set_config_string(generated_config_string);
    Config config2 = make_config();
    
    // CallStack trace config should match after round-trip
    EXPECT_EQ(config1.enable_callstack_trace, config2.enable_callstack_trace) 
        << "CallStack trace setting should match after round-trip";
    EXPECT_TRUE(config2.enable_callstack_trace) << "CallStack trace should still be enabled after round-trip";
}

// Test invalid callstack trace environment variable value
TEST_F(ConfigTest, CallstackTraceInvalidEnvironmentVariableTest) {
    // Test invalid boolean value (should fallback to default)
    setenv(env::ENABLE_CALLSTACK_TRACE, "invalid_bool", 1);
    
    Config config = make_config();
    
    // Should fallback to default (false) when environment variable is invalid
    EXPECT_FALSE(config.enable_callstack_trace) << "Should use default (false) when env var is invalid";
}

// Test various valid boolean representations for callstack trace
TEST_F(ConfigTest, CallstackTraceBooleanRepresentationsTest) {
    // Test "1" as true
    setenv(env::ENABLE_CALLSTACK_TRACE, "1", 1);
    Config config1 = make_config();
    EXPECT_TRUE(config1.enable_callstack_trace) << "1 should be parsed as true";
    
    // Test "0" as false
    setenv(env::ENABLE_CALLSTACK_TRACE, "0", 1);
    Config config2 = make_config();
    EXPECT_FALSE(config2.enable_callstack_trace) << "0 should be parsed as false";
    
    // Test "TRUE" as true
    setenv(env::ENABLE_CALLSTACK_TRACE, "TRUE", 1);
    Config config3 = make_config();
    EXPECT_TRUE(config3.enable_callstack_trace) << "TRUE should be parsed as true";
    
    // Test "False" as false
    setenv(env::ENABLE_CALLSTACK_TRACE, "False", 1);
    Config config4 = make_config();
    EXPECT_FALSE(config4.enable_callstack_trace) << "False should be parsed as false";
    
    // Test "yes" as true
    setenv(env::ENABLE_CALLSTACK_TRACE, "yes", 1);
    Config config5 = make_config();
    EXPECT_TRUE(config5.enable_callstack_trace) << "yes should be parsed as true";
    
    // Test "NO" as false
    setenv(env::ENABLE_CALLSTACK_TRACE, "NO", 1);
    Config config6 = make_config();
    EXPECT_FALSE(config6.enable_callstack_trace) << "NO should be parsed as false";
}

// Test callstack trace with invalid YAML type
TEST_F(ConfigTest, CallstackTraceInvalidYamlTypeTest) {
    // YAML with invalid type for EnableCallstackTrace
    const std::string invalid_type_yaml = R"(
EnableCallstackTrace: "not_a_boolean"
)";
    
    set_config_string(invalid_type_yaml);
    Config config = make_config();
    
    // Should use default value when YAML type is invalid
    EXPECT_FALSE(config.enable_callstack_trace) << "Should use default (false) when YAML type is invalid";
}

// Test callstack trace mixed with other configurations
TEST_F(ConfigTest, CallstackTraceMixedConfigurationTest) {
    const std::string mixed_config = R"(
ApplicationName: "MixedApp"
ApplicationType: 1305
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
    Config config = make_config();
    
    // Verify all config values including callstack trace
    EXPECT_EQ(config.app_name_, "MixedApp") << "App name should match";
    EXPECT_EQ(config.app_type_, 1305) << "App type should match";
    EXPECT_TRUE(config.enable) << "Enable should be true";
    EXPECT_TRUE(config.enable_callstack_trace) << "CallStack trace should be enabled";
    EXPECT_EQ(config.log.level, "warn") << "Log level should match";
    EXPECT_EQ(config.log.max_file_size, 50) << "Log max file size should match";
    EXPECT_EQ(config.collector.host, "mixed.collector.host") << "Collector host should match";
    EXPECT_EQ(config.sampling.type, "PERCENT") << "Sampling type should match";
    EXPECT_DOUBLE_EQ(config.sampling.percent_rate, 25.0) << "Percent rate should match";
    EXPECT_EQ(config.sql.max_bind_args_size, 2048) << "SQL max bind args size should match";
    EXPECT_TRUE(config.sql.enable_sql_stats) << "SQL stats should be enabled";
}

// Test callstack trace environment variable with other environment variables
TEST_F(ConfigTest, CallstackTraceEnvironmentVariableWithOthersTest) {
    setenv(env::APPLICATION_NAME, "EnvMixedApp", 1);
    setenv(env::ENABLE_CALLSTACK_TRACE, "true", 1);
    setenv(env::SQL_ENABLE_SQL_STATS, "true", 1);
    setenv(env::LOG_LEVEL, "error", 1);
    
    Config config = make_config();
    
    // Verify all environment variables are loaded correctly
    EXPECT_EQ(config.app_name_, "EnvMixedApp") << "App name should match env var";
    EXPECT_TRUE(config.enable_callstack_trace) << "CallStack trace should be enabled via env var";
    EXPECT_TRUE(config.sql.enable_sql_stats) << "SQL stats should be enabled via env var";
    EXPECT_EQ(config.log.level, "error") << "Log level should match env var";
}

// Test callstack trace default value is included in string output
TEST_F(ConfigTest, CallstackTraceDefaultInStringOutputTest) {
    // Don't set any config, use defaults
    Config config = make_config();
    
    std::string config_string = to_config_string(config);
    
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
    Config config = make_config();
    
    EXPECT_FALSE(config.http.url_stat.enable_trim_path) << "URL stat enable trim path should be disabled via YAML";
}

// Test URL stat enable trim path via environment variable
TEST_F(ConfigTest, UrlStatEnableTrimPathViaEnvironmentVariableTest) {
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "false", 1);
    Config config = make_config();
    
    EXPECT_FALSE(config.http.url_stat.enable_trim_path) << "URL stat enable trim path should be disabled via environment variable";
}

// Test environment variable overrides YAML for enable trim path
TEST_F(ConfigTest, UrlStatEnableTrimPathEnvironmentVariableOverrideYamlTest) {
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: false
)");
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "true", 1);
    Config config = make_config();
    
    EXPECT_TRUE(config.http.url_stat.enable_trim_path) << "Environment variable should override YAML for enable trim path";
}

// Test environment variable overrides YAML (opposite case)
TEST_F(ConfigTest, UrlStatEnableTrimPathEnvironmentVariableOverrideYamlOppositeTest) {
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: true
)");
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "false", 1);
    Config config = make_config();
    
    EXPECT_FALSE(config.http.url_stat.enable_trim_path) << "Environment variable should override YAML for enable trim path (opposite)";
}

// Test invalid environment variable for enable trim path
TEST_F(ConfigTest, UrlStatEnableTrimPathInvalidEnvironmentVariableTest) {
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "invalid", 1);
    Config config = make_config();
    
    // Should use default value (true) when invalid
    EXPECT_TRUE(config.http.url_stat.enable_trim_path) << "Invalid environment variable should use default value";
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
    Config config = make_config();
    
    EXPECT_TRUE(config.http.url_stat.enable) << "URL stat should be enabled";
    EXPECT_EQ(config.http.url_stat.limit, 512) << "URL stat limit should match YAML";
    EXPECT_FALSE(config.http.url_stat.enable_trim_path) << "Enable trim path should be false";
    EXPECT_EQ(config.http.url_stat.trim_path_depth, 2) << "Path depth should match YAML";
    EXPECT_TRUE(config.http.url_stat.method_prefix) << "Method prefix should be true";
}

// Test enable trim path boolean variations
TEST_F(ConfigTest, UrlStatEnableTrimPathBooleanVariationsTest) {
    // Test "yes"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: yes
)");
    Config config1 = make_config();
    EXPECT_TRUE(config1.http.url_stat.enable_trim_path) << "yes should be parsed as true";
    
    // Test "no"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: no
)");
    Config config2 = make_config();
    EXPECT_FALSE(config2.http.url_stat.enable_trim_path) << "no should be parsed as false";
    
    // Test "TRUE"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: TRUE
)");
    Config config3 = make_config();
    EXPECT_TRUE(config3.http.url_stat.enable_trim_path) << "TRUE should be parsed as true";
    
    // Test "FALSE"
    set_config_string(R"(
Http:
  UrlStatEnableTrimPath: FALSE
)");
    Config config4 = make_config();
    EXPECT_FALSE(config4.http.url_stat.enable_trim_path) << "FALSE should be parsed as false";
}

// Test enable trim path environment variable boolean variations
TEST_F(ConfigTest, UrlStatEnableTrimPathEnvironmentVariableBooleanVariationsTest) {
    // Test "1"
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "1", 1);
    Config config1 = make_config();
    EXPECT_TRUE(config1.http.url_stat.enable_trim_path) << "1 should be parsed as true";
    
    // Test "0"
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "0", 1);
    Config config2 = make_config();
    EXPECT_FALSE(config2.http.url_stat.enable_trim_path) << "0 should be parsed as false";
    
    // Test "yes"
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "yes", 1);
    Config config3 = make_config();
    EXPECT_TRUE(config3.http.url_stat.enable_trim_path) << "yes should be parsed as true";
    
    // Test "no"
    setenv(env::HTTP_URL_STAT_ENABLE_TRIM_PATH, "no", 1);
    Config config4 = make_config();
    EXPECT_FALSE(config4.http.url_stat.enable_trim_path) << "no should be parsed as false";
}

} // namespace pinpoint
