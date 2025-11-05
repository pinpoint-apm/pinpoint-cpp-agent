# Pinpoint C++ Agent - Configuration Guide

This document describes the configuration options available for the Pinpoint C++ Agent.

## Table of Contents
- [Configuration Methods](#configuration-methods)
- [Agent Configuration](#agent-configuration)
- [Logging Configuration](#logging-configuration)
- [Collector Configuration](#collector-configuration)
- [Stat Configuration](#stat-configuration)
- [Sampling Configuration](#sampling-configuration)
- [Span Configuration](#span-configuration)
- [HTTP Configuration](#http-configuration)
- [SQL Configuration](#sql-configuration)
- [Advanced Configuration](#advanced-configuration)
- [Configuration Examples](#configuration-examples)

## Configuration Methods

The Pinpoint C++ Agent supports three configuration methods, with the following priority order:

1. **Environment Variables** (Highest priority)
2. **YAML Configuration File**
3. **Default Values** (Lowest priority)

### Method 1: YAML Configuration File

Create a `pinpoint-config.yaml` file:

```yaml
ApplicationName: "MyApplication"
ApplicationType: 1300
AgentId: "my-agent-001"
AgentName: "MyAgent"
Enable: true

Log:
  Level: "info"
  FilePath: "/var/log/pinpoint/agent.log"
  MaxFileSize: 10

Collector:
  GrpcHost: "localhost"
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992

Stat:
  Enable: true
  BatchCount: 6
  BatchInterval: 5000

Sampling:
  Type: "COUNTING"
  CounterRate: 1
  PercentRate: 100
  NewThroughput: 0
  ContinueThroughput: 0

Span:
  QueueSize: 1024
  MaxEventDepth: 64
  MaxEventSequence: 5000
  EventChunkSize: 20

Http:
  CollectUrlStat: true
  UrlStatLimit: 1024
  UrlStatPathDepth: 1
  UrlStatMethodPrefix: false
  
  Server:
    StatusCodeErrors: ["5xx"]
    ExcludeUrl: []
    ExcludeMethod: []
    RecordRequestHeader: ["Content-Type", "User-Agent"]
    RecordRequestCookie: []
    RecordResponseHeader: ["Content-Type"]
  
  Client:
    RecordRequestHeader: ["Content-Type"]
    RecordRequestCookie: []
    RecordResponseHeader: ["Content-Type"]

Sql:
  MaxBindArgsSize: 1024
  EnableSqlStats: false

IsContainer: false
EnableCallstackTrace: false
```

Set the configuration file path:

```cpp
#include "pinpoint/tracer.h"

int main() {
    pinpoint::SetConfigFilePath("/path/to/pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();
    // ...
}
```

### Method 2: Environment Variables

Configure using environment variables:

```bash
export PINPOINT_CPP_APPLICATION_NAME="MyApplication"
export PINPOINT_CPP_AGENT_ID="my-agent-001"
export PINPOINT_CPP_GRPC_HOST="localhost"
export PINPOINT_CPP_LOG_LEVEL="info"
# ... other variables
```

Or set via `setenv()` in your application:

```cpp
#include <cstdlib>

int main() {
    setenv("PINPOINT_CPP_APPLICATION_NAME", "MyApplication", 1);
    setenv("PINPOINT_CPP_GRPC_HOST", "localhost", 1);
    
    auto agent = pinpoint::CreateAgent();
    // ...
}
```

### Method 3: Configuration String

Pass configuration directly as a YAML string:

```cpp
#include "pinpoint/tracer.h"

int main() {
    std::string config = R"(
        ApplicationName: "MyApplication"
        Collector:
          GrpcHost: "localhost"
        Sampling:
          Type: "PERCENT"
          PercentRate: 10
    )";
    
    pinpoint::SetConfigString(config);
    auto agent = pinpoint::CreateAgent();
    // ...
}
```

## Agent Configuration

Basic agent settings.

### ApplicationName

- **Description**: Name of the application being monitored
- **Type**: String
- **Default**: `""` (empty string)
- **YAML Key**: `ApplicationName`
- **Environment Variable**: `PINPOINT_CPP_APPLICATION_NAME`
- **Required**: Yes

**Example:**
```yaml
ApplicationName: "OrderService"
```

```bash
export PINPOINT_CPP_APPLICATION_NAME="OrderService"
```

### ApplicationType

- **Description**: Type code for the application (used for service topology)
- **Type**: Integer
- **Default**: `1300` (C++ application)
- **YAML Key**: `ApplicationType`
- **Environment Variable**: `PINPOINT_CPP_APPLICATION_TYPE`

**Common Types:**
- `1300`: C++ Application (default)
- `1400`: Custom type

**Example:**
```yaml
ApplicationType: 1300
```

### AgentId

- **Description**: Unique identifier for the agent instance
- **Type**: String
- **Default**: Auto-generated (hostname + random string)
- **YAML Key**: `AgentId`
- **Environment Variable**: `PINPOINT_CPP_AGENT_ID`

**Example:**
```yaml
AgentId: "order-service-001"
```

**Auto-generation Format**: `{hostname}-{random}` (e.g., `server01-a3b4c`)

### AgentName

- **Description**: Friendly name for the agent
- **Type**: String
- **Default**: `""` (empty string)
- **YAML Key**: `AgentName`
- **Environment Variable**: `PINPOINT_CPP_AGENT_NAME`

**Example:**
```yaml
AgentName: "Production Order Service Agent"
```

### Enable

- **Description**: Enable or disable the agent
- **Type**: Boolean
- **Default**: `true`
- **YAML Key**: `Enable`
- **Environment Variable**: `PINPOINT_CPP_ENABLE`

**Example:**
```yaml
Enable: false  # Disable agent
```

## Logging Configuration

Control agent logging behavior.

### Log.Level

- **Description**: Logging level
- **Type**: String
- **Default**: `"info"`
- **YAML Key**: `Log.Level` or `LogLevel`
- **Environment Variable**: `PINPOINT_CPP_LOG_LEVEL`

**Valid Values:**
- `"trace"`: Most verbose
- `"debug"`: Debug information
- `"info"`: Informational messages (default)
- `"warn"`: Warnings only
- `"error"`: Errors only

**Example:**
```yaml
Log:
  Level: "debug"
```

```bash
export PINPOINT_CPP_LOG_LEVEL="debug"
```

### Log.FilePath

- **Description**: Path to log file (if empty, logs to stdout)
- **Type**: String
- **Default**: `""` (stdout)
- **YAML Key**: `Log.FilePath`
- **Environment Variable**: `PINPOINT_CPP_LOG_FILE_PATH`

**Example:**
```yaml
Log:
  FilePath: "/var/log/pinpoint/agent.log"
```

### Log.MaxFileSize

- **Description**: Maximum log file size in MB before rotation
- **Type**: Integer
- **Default**: `10` (MB)
- **YAML Key**: `Log.MaxFileSize`
- **Environment Variable**: `PINPOINT_CPP_LOG_MAX_FILE_SIZE`

**Example:**
```yaml
Log:
  MaxFileSize: 50  # 50 MB
```

## Collector Configuration

Configure connection to Pinpoint Collector.

### Collector.GrpcHost

- **Description**: Hostname or IP address of the Pinpoint Collector
- **Type**: String
- **Default**: `""` (empty string)
- **YAML Key**: `Collector.GrpcHost`
- **Environment Variable**: `PINPOINT_CPP_GRPC_HOST`
- **Required**: Yes

**Example:**
```yaml
Collector:
  GrpcHost: "pinpoint-collector.example.com"
```

### Collector.GrpcAgentPort

- **Description**: gRPC port for agent metadata
- **Type**: Integer
- **Default**: `9991`
- **YAML Key**: `Collector.GrpcAgentPort`
- **Environment Variable**: `PINPOINT_CPP_GRPC_AGENT_PORT`

**Example:**
```yaml
Collector:
  GrpcAgentPort: 9991
```

### Collector.GrpcSpanPort

- **Description**: gRPC port for span data
- **Type**: Integer
- **Default**: `9993`
- **YAML Key**: `Collector.GrpcSpanPort`
- **Environment Variable**: `PINPOINT_CPP_GRPC_SPAN_PORT`

**Example:**
```yaml
Collector:
  GrpcSpanPort: 9993
```

### Collector.GrpcStatPort

- **Description**: gRPC port for statistics data
- **Type**: Integer
- **Default**: `9992`
- **YAML Key**: `Collector.GrpcStatPort`
- **Environment Variable**: `PINPOINT_CPP_GRPC_STAT_PORT`

**Example:**
```yaml
Collector:
  GrpcStatPort: 9992
```

## Stat Configuration

Configure statistics collection.

### Stat.Enable

- **Description**: Enable statistics collection
- **Type**: Boolean
- **Default**: `true`
- **YAML Key**: `Stat.Enable`
- **Environment Variable**: `PINPOINT_CPP_STAT_ENABLE`

**Example:**
```yaml
Stat:
  Enable: true
```

### Stat.BatchCount

- **Description**: Number of stat batches to collect before sending
- **Type**: Integer
- **Default**: `6`
- **YAML Key**: `Stat.BatchCount`
- **Environment Variable**: `PINPOINT_CPP_STAT_BATCH_COUNT`

**Example:**
```yaml
Stat:
  BatchCount: 10
```

### Stat.BatchInterval

- **Description**: Interval between stat collections in milliseconds
- **Type**: Integer
- **Default**: `5000` (5 seconds)
- **YAML Key**: `Stat.BatchInterval`
- **Environment Variable**: `PINPOINT_CPP_STAT_BATCH_INTERVAL`

**Example:**
```yaml
Stat:
  BatchInterval: 10000  # 10 seconds
```

## Sampling Configuration

Configure transaction sampling strategies.

### Sampling.Type

- **Description**: Sampling strategy type
- **Type**: String
- **Default**: `"COUNTING"`
- **YAML Key**: `Sampling.Type`
- **Environment Variable**: `PINPOINT_CPP_SAMPLING_TYPE`

**Valid Values:**
- `"COUNTING"`: Sample every Nth transaction
- `"PERCENT"`: Sample by percentage
- `"THROUGHPUT"`: Adaptive throughput-based sampling

**Example:**
```yaml
Sampling:
  Type: "PERCENT"
```

### Sampling.CounterRate

- **Description**: Sample 1 out of every N transactions (for COUNTING type)
- **Type**: Integer
- **Default**: `1` (sample all)
- **YAML Key**: `Sampling.CounterRate`
- **Environment Variable**: `PINPOINT_CPP_SAMPLING_COUNTER_RATE`

**Example:**
```yaml
Sampling:
  Type: "COUNTING"
  CounterRate: 10  # Sample 1 out of every 10 transactions
```

### Sampling.PercentRate

- **Description**: Percentage of transactions to sample (for PERCENT type)
- **Type**: Double
- **Default**: `100` (sample all)
- **Range**: `0.01` to `100`
- **YAML Key**: `Sampling.PercentRate`
- **Environment Variable**: `PINPOINT_CPP_SAMPLING_PERCENT_RATE`

**Example:**
```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 10.0  # Sample 10% of transactions
```

### Sampling.NewThroughput

- **Description**: Target throughput for new transactions per second (for THROUGHPUT type)
- **Type**: Integer
- **Default**: `0` (disabled)
- **YAML Key**: `Sampling.NewThroughput`
- **Environment Variable**: `PINPOINT_CPP_SAMPLING_NEW_THROUGHPUT`

**Example:**
```yaml
Sampling:
  Type: "THROUGHPUT"
  NewThroughput: 100  # Target 100 new transactions/sec
```

### Sampling.ContinueThroughput

- **Description**: Target throughput for continuing transactions per second
- **Type**: Integer
- **Default**: `0` (disabled)
- **YAML Key**: `Sampling.ContinueThroughput`
- **Environment Variable**: `PINPOINT_CPP_SAMPLING_CONTINUE_THROUGHPUT`

**Example:**
```yaml
Sampling:
  ContinueThroughput: 200  # Target 200 continuing transactions/sec
```

## Span Configuration

Configure span collection behavior.

### Span.QueueSize

- **Description**: Size of the span queue
- **Type**: Integer
- **Default**: `1024`
- **YAML Key**: `Span.QueueSize`
- **Environment Variable**: `PINPOINT_CPP_SPAN_QUEUE_SIZE`
- **Minimum**: `1`

**Example:**
```yaml
Span:
  QueueSize: 2048
```

### Span.MaxEventDepth

- **Description**: Maximum depth of nested span events
- **Type**: Integer
- **Default**: `64`
- **YAML Key**: `Span.MaxEventDepth`
- **Environment Variable**: `PINPOINT_CPP_SPAN_MAX_EVENT_DEPTH`
- **Special**: `-1` for unlimited
- **Minimum**: `2`

**Example:**
```yaml
Span:
  MaxEventDepth: 128
```

### Span.MaxEventSequence

- **Description**: Maximum number of span events per span
- **Type**: Integer
- **Default**: `5000`
- **YAML Key**: `Span.MaxEventSequence`
- **Environment Variable**: `PINPOINT_CPP_SPAN_MAX_EVENT_SEQUENCE`
- **Special**: `-1` for unlimited
- **Minimum**: `4`

**Example:**
```yaml
Span:
  MaxEventSequence: 10000
```

### Span.EventChunkSize

- **Description**: Number of span events per chunk for transmission
- **Type**: Integer
- **Default**: `20`
- **YAML Key**: `Span.EventChunkSize`
- **Environment Variable**: `PINPOINT_CPP_SPAN_EVENT_CHUNK_SIZE`
- **Minimum**: `1`

**Example:**
```yaml
Span:
  EventChunkSize: 50
```

## HTTP Configuration

Configure HTTP request/response tracing.

### Http.CollectUrlStat

- **Description**: Enable URL statistics collection
- **Type**: Boolean
- **Default**: `false`
- **YAML Key**: `Http.CollectUrlStat`
- **Environment Variable**: `PINPOINT_CPP_HTTP_COLLECT_URL_STAT`

**Example:**
```yaml
Http:
  CollectUrlStat: true
```

### Http.UrlStatLimit

- **Description**: Maximum number of unique URLs to track
- **Type**: Integer
- **Default**: `1024`
- **YAML Key**: `Http.UrlStatLimit`
- **Environment Variable**: `PINPOINT_CPP_HTTP_URL_STAT_LIMIT`

**Example:**
```yaml
Http:
  UrlStatLimit: 2048
```

### Http.UrlStatPathDepth

- **Description**: URL path depth for normalization
- **Type**: Integer
- **Default**: `1`
- **YAML Key**: `Http.UrlStatPathDepth`
- **Environment Variable**: `PINPOINT_CPP_HTTP_URL_STAT_PATH_DEPTH`

**Example:**
```yaml
Http:
  UrlStatPathDepth: 2  # /api/users -> /api/*
```

### Http.UrlStatMethodPrefix

- **Description**: Include HTTP method in URL stat key
- **Type**: Boolean
- **Default**: `false`
- **YAML Key**: `Http.UrlStatMethodPrefix`
- **Environment Variable**: `PINPOINT_CPP_HTTP_URL_STAT_METHOD_PREFIX`

**Example:**
```yaml
Http:
  UrlStatMethodPrefix: true  # GET:/api/users, POST:/api/users
```

### Http.Server.StatusCodeErrors

- **Description**: HTTP status codes to treat as errors
- **Type**: String Array
- **Default**: `["5xx"]`
- **YAML Key**: `Http.Server.StatusCodeErrors`
- **Environment Variable**: `PINPOINT_CPP_HTTP_SERVER_STATUS_CODE_ERRORS` (comma-separated)

**Example:**
```yaml
Http:
  Server:
    StatusCodeErrors: ["4xx", "5xx"]
```

```bash
export PINPOINT_CPP_HTTP_SERVER_STATUS_CODE_ERRORS="4xx,5xx"
```

### Http.Server.ExcludeUrl

- **Description**: URL patterns to exclude from tracing
- **Type**: String Array
- **Default**: `[]`
- **YAML Key**: `Http.Server.ExcludeUrl`
- **Environment Variable**: `PINPOINT_CPP_HTTP_SERVER_EXCLUDE_URL` (comma-separated)

**Example:**
```yaml
Http:
  Server:
    ExcludeUrl: ["/health", "/metrics", "/actuator/*"]
```

### Http.Server.ExcludeMethod

- **Description**: HTTP methods to exclude from tracing
- **Type**: String Array
- **Default**: `[]`
- **YAML Key**: `Http.Server.ExcludeMethod`
- **Environment Variable**: `PINPOINT_CPP_HTTP_SERVER_EXCLUDE_METHOD` (comma-separated)

**Example:**
```yaml
Http:
  Server:
    ExcludeMethod: ["OPTIONS", "HEAD"]
```

### Http.Server.RecordRequestHeader

- **Description**: Request headers to record
- **Type**: String Array
- **Default**: `[]`
- **YAML Key**: `Http.Server.RecordRequestHeader`
- **Environment Variable**: `PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER` (comma-separated)

**Example:**
```yaml
Http:
  Server:
    RecordRequestHeader: ["Content-Type", "User-Agent", "Referer"]
```

### Http.Server.RecordRequestCookie

- **Description**: Request cookies to record
- **Type**: String Array
- **Default**: `[]`
- **YAML Key**: `Http.Server.RecordRequestCookie`
- **Environment Variable**: `PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_COOKIE` (comma-separated)

**Example:**
```yaml
Http:
  Server:
    RecordRequestCookie: ["session_id"]
```

### Http.Server.RecordResponseHeader

- **Description**: Response headers to record
- **Type**: String Array
- **Default**: `[]`
- **YAML Key**: `Http.Server.RecordResponseHeader`
- **Environment Variable**: `PINPOINT_CPP_HTTP_SERVER_RECORD_RESPONSE_HEADER` (comma-separated)

**Example:**
```yaml
Http:
  Server:
    RecordResponseHeader: ["Content-Type", "Content-Length"]
```

### Http.Client Configuration

Similar to server configuration but for HTTP client requests:

- `Http.Client.RecordRequestHeader`
- `Http.Client.RecordRequestCookie`
- `Http.Client.RecordResponseHeader`

**Example:**
```yaml
Http:
  Client:
    RecordRequestHeader: ["Content-Type", "Authorization"]
    RecordResponseHeader: ["Content-Type"]
```

## SQL Configuration

Configure SQL query tracing.

### Sql.MaxBindArgsSize

- **Description**: Maximum size of SQL bind arguments to record (in bytes)
- **Type**: Integer
- **Default**: `1024`
- **YAML Key**: `Sql.MaxBindArgsSize`
- **Environment Variable**: `PINPOINT_CPP_SQL_MAX_BIND_ARGS_SIZE`

**Example:**
```yaml
Sql:
  MaxBindArgsSize: 2048
```

### Sql.EnableSqlStats

- **Description**: Enable SQL statistics collection
- **Type**: Boolean
- **Default**: `false`
- **YAML Key**: `Sql.EnableSqlStats`
- **Environment Variable**: `PINPOINT_CPP_SQL_ENABLE_SQL_STATS`

**Example:**
```yaml
Sql:
  EnableSqlStats: true
```

## Advanced Configuration

### IsContainer

- **Description**: Indicate if running in a container environment
- **Type**: Boolean
- **Default**: Auto-detected (checks for `/.dockerenv` or `KUBERNETES_SERVICE_HOST`)
- **YAML Key**: `IsContainer`
- **Environment Variable**: `PINPOINT_CPP_IS_CONTAINER`

**Example:**
```yaml
IsContainer: true
```

### EnableCallstackTrace

- **Description**: Enable call stack trace collection on errors
- **Type**: Boolean
- **Default**: `false`
- **YAML Key**: `EnableCallstackTrace`
- **Environment Variable**: `PINPOINT_CPP_ENABLE_CALLSTACK_TRACE`

**Example:**
```yaml
EnableCallstackTrace: true
```

## Configuration Examples

### Example 1: Development Configuration

Full sampling, debug logging, local collector:

```yaml
ApplicationName: "MyApp-Dev"
Enable: true

Log:
  Level: "debug"
  FilePath: ""  # Log to stdout

Collector:
  GrpcHost: "localhost"
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992

Sampling:
  Type: "COUNTING"
  CounterRate: 1  # Sample all transactions

Http:
  CollectUrlStat: true
  Server:
    RecordRequestHeader: ["*"]  # Record all headers
    RecordResponseHeader: ["*"]

Sql:
  EnableSqlStats: true

EnableCallstackTrace: true
```

### Example 2: Production Configuration

Optimized for production with sampling:

```yaml
ApplicationName: "MyApp-Prod"
AgentId: "prod-server-01"
Enable: true

Log:
  Level: "warn"
  FilePath: "/var/log/pinpoint/agent.log"
  MaxFileSize: 50

Collector:
  GrpcHost: "pinpoint-collector.prod.example.com"

Sampling:
  Type: "PERCENT"
  PercentRate: 10.0  # Sample 10% of transactions

Stat:
  Enable: true
  BatchCount: 10
  BatchInterval: 10000  # 10 seconds

Span:
  QueueSize: 2048
  MaxEventDepth: 32
  MaxEventSequence: 1000

Http:
  CollectUrlStat: true
  UrlStatLimit: 5000
  Server:
    StatusCodeErrors: ["5xx"]
    ExcludeUrl: ["/health", "/metrics"]
    RecordRequestHeader: ["Content-Type", "User-Agent"]
    RecordResponseHeader: ["Content-Type"]

EnableCallstackTrace: false
```

### Example 3: High-Traffic Configuration

Optimized for high-traffic scenarios:

```yaml
ApplicationName: "HighTrafficApp"
Enable: true

Log:
  Level: "error"  # Minimal logging

Collector:
  GrpcHost: "collector.example.com"

Sampling:
  Type: "THROUGHPUT"
  NewThroughput: 500  # Limit to 500 new transactions/sec
  ContinueThroughput: 1000  # Limit to 1000 continuing transactions/sec

Stat:
  Enable: true
  BatchCount: 20
  BatchInterval: 5000

Span:
  QueueSize: 4096
  MaxEventDepth: 16  # Limit depth
  MaxEventSequence: 500  # Limit events per span
  EventChunkSize: 100

Http:
  CollectUrlStat: false  # Disable to reduce overhead
  Server:
    StatusCodeErrors: ["5xx"]
    ExcludeUrl: ["/health", "/ping"]
    ExcludeMethod: ["OPTIONS", "HEAD"]
    RecordRequestHeader: []  # Don't record headers
    RecordResponseHeader: []

Sql:
  MaxBindArgsSize: 512
  EnableSqlStats: false

EnableCallstackTrace: false
```

### Example 4: Environment Variable Configuration

```bash
#!/bin/bash

# Basic configuration
export PINPOINT_CPP_APPLICATION_NAME="MyApp"
export PINPOINT_CPP_AGENT_ID="app-server-01"
export PINPOINT_CPP_ENABLE="true"

# Collector
export PINPOINT_CPP_GRPC_HOST="pinpoint-collector"

# Logging
export PINPOINT_CPP_LOG_LEVEL="info"
export PINPOINT_CPP_LOG_FILE_PATH="/var/log/pinpoint/agent.log"

# Sampling
export PINPOINT_CPP_SAMPLING_TYPE="PERCENT"
export PINPOINT_CPP_SAMPLING_PERCENT_RATE="25.0"

# HTTP
export PINPOINT_CPP_HTTP_COLLECT_URL_STAT="true"
export PINPOINT_CPP_HTTP_SERVER_STATUS_CODE_ERRORS="4xx,5xx"
export PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER="Content-Type,User-Agent"

# SQL
export PINPOINT_CPP_SQL_ENABLE_SQL_STATS="true"

# Run application
./my_application
```

## Configuration Best Practices

### 1. Development Environment
- Use `CounterRate: 1` to sample all transactions
- Set `Log.Level: "debug"` for detailed information
- Enable `EnableCallstackTrace: true` for better debugging
- Record all headers for troubleshooting

### 2. Production Environment
- Use percentage or throughput-based sampling
- Set `Log.Level: "warn"` or `"error"`
- Carefully select which headers to record
- Exclude health check and monitoring endpoints
- Monitor queue sizes and adjust if needed

### 3. High-Traffic Scenarios
- Use throughput-based sampling
- Increase queue sizes
- Limit max event depth and sequence
- Disable URL statistics if not needed
- Exclude unnecessary HTTP methods

### 4. Container Deployments
- Set `IsContainer: true` explicitly if auto-detection fails
- Use environment variables for configuration
- Ensure unique `AgentId` per container instance
- Consider using hostname in `AgentId`

### 5. Security Considerations
- Never record sensitive headers (Authorization, Cookie, etc.) unless absolutely necessary
- Limit SQL bind argument sizes
- Be cautious with recording cookies
- Review recorded headers regularly

## Troubleshooting

### Agent Not Connecting

Check collector configuration:
```yaml
Collector:
  GrpcHost: "correct-hostname"  # Verify hostname
  GrpcAgentPort: 9991  # Verify port
```

Verify with logs:
```yaml
Log:
  Level: "debug"  # Enable debug logging
```

### High Memory Usage

Reduce queue sizes and limits:
```yaml
Span:
  QueueSize: 512  # Reduce from default 1024
  MaxEventSequence: 1000  # Reduce from default 5000

Http:
  UrlStatLimit: 512  # Reduce from default 1024
```

### Performance Impact

Reduce sampling rate:
```yaml
Sampling:
  Type: "PERCENT"
  PercentRate: 5.0  # Sample only 5%
```

Disable unnecessary features:
```yaml
Http:
  CollectUrlStat: false
  
Sql:
  EnableSqlStats: false
  
Stat:
  Enable: false
```

### Missing Transactions

Increase sampling:
```yaml
Sampling:
  CounterRate: 1  # Sample all
```

Check excluded URLs:
```yaml
Http:
  Server:
    ExcludeUrl: []  # Remove exclusions temporarily
```

## Related Documentation

- [Quick Start Guide](quick_start.md)
- [Instrumentation Guide](instrument.md)
- [Code Examples](examples.md)

## Support

For configuration issues:
- GitHub: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
- Documentation: [Pinpoint APM](https://pinpoint-apm.github.io/pinpoint/)

## License

Apache License 2.0 - See [LICENSE](../LICENSE) for details.

