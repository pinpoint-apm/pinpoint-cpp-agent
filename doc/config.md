# Pinpoint C++ Agent - Configuration Guide

This document is a consolidated reference for all configuration options available in the Pinpoint C++ Agent (`pinpoint-cpp-agent`).

---

## Table of Contents

- [Configuration Methods & Precedence](#configuration-methods--precedence)
- [Agent Configuration](#agent-configuration)
- [Logging Configuration](#logging-configuration)
- [Collector Configuration](#collector-configuration)
- [gRPC Transport Configuration](#grpc-transport-configuration)
- [Stat Configuration](#stat-configuration)
- [Sampling Configuration](#sampling-configuration)
- [Span Configuration](#span-configuration)
- [HTTP Configuration](#http-configuration)
- [SQL Configuration](#sql-configuration)
- [Advanced Configuration](#advanced-configuration)
- [Configuration Hot Reload](#configuration-hot-reload)
- [Configuration Examples](#configuration-examples)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)

---

## Configuration Methods & Precedence

The agent merges configuration from three sources. **Later sources override earlier ones:**

1. **Default Values** (lowest priority) — built-in defaults applied by `init_config`.
2. **YAML Configuration File** — loaded via `PINPOINT_CPP_CONFIG_FILE` env var, `read_config_from_file()`, or `set_config_string()`.
3. **Environment Variables** (highest priority) — `PINPOINT_CPP_*` variables applied last by `load_env_config`.

During startup (`make_config`), the agent loads defaults → reads the optional config file → parses YAML → applies environment overrides → normalises values → initialises logging.

### Method 1: YAML Configuration File

Create a `pinpoint-config.yaml` file and set its path:

```cpp
#include "pinpoint/tracer.h"

int main() {
    pinpoint::SetConfigFilePath("/path/to/pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();
    // ...
}
```

Or set the path via environment variable:

```bash
export PINPOINT_CPP_CONFIG_FILE="/path/to/pinpoint-config.yaml"
```

### Method 2: Environment Variables

```bash
export PINPOINT_CPP_APPLICATION_NAME="MyApplication"
export PINPOINT_CPP_AGENT_ID="my-agent-001"
export PINPOINT_CPP_GRPC_HOST="localhost"
export PINPOINT_CPP_LOG_LEVEL="info"
```

Or programmatically:

```cpp
#include <cstdlib>

int main() {
    setenv("PINPOINT_CPP_APPLICATION_NAME", "MyApplication", 1);
    setenv("PINPOINT_CPP_GRPC_HOST", "localhost", 1);
    auto agent = pinpoint::CreateAgent();
    // ...
}
```

### Method 3: Configuration String (Inline YAML)

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

---

## Agent Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `ApplicationName` | `PINPOINT_CPP_APPLICATION_NAME` | string | `""` | **Required.** Name of the monitored application. Max 24 chars for `UidVersion: v1`, otherwise max 254 chars. |
| `ApplicationType` | `PINPOINT_CPP_APPLICATION_TYPE` | int | `1300` | Pinpoint service type code. `1300` = C++ App. |
| `AgentId` | `PINPOINT_CPP_AGENT_ID` | string | auto-generated | Allowed chars `[a-zA-Z0-9._-]`, max 24 chars. Auto-generated as a 22-char URL-safe Base64 UUIDv7 when empty/invalid. **Ignored for `UidVersion: v4`** (a fresh UUIDv7 is always generated). |
| `AgentName` | `PINPOINT_CPP_AGENT_NAME` | string | `""` | Optional human-readable label (max 255 chars; 254 for v4). Falls back to `AgentId` when omitted. |
| `UidVersion` | `PINPOINT_CPP_UID_VERSION` | string | `v3` | Agent self-identity (ObjectName) version: `v1`, `v3`, or `v4` (case-insensitive; unknown/empty → `v3`). See [Identity Versions](#identity-versions). |
| `ServiceName` | `PINPOINT_CPP_SERVICE_NAME` | string | `""` | **Required for `UidVersion: v4`** (max 254 chars). Unused for v1/v3. |
| `ApiKey` | `PINPOINT_CPP_API_KEY` | string | `""` | **Required for `UidVersion: v4`**. Unused for v1/v3. Never logged in plaintext. |
| `Enable` | `PINPOINT_CPP_ENABLE` | bool | `true` | Set `false` to disable tracing without code changes. |

### Identity Versions

`UidVersion` selects how the agent identifies itself to the Collector (mirrors the Java agent's `pinpoint.modules.uid.version`):

| | v1 | v3 (default) | v4 |
|---|---|---|---|
| `ApplicationName` max length | 24 | 254 | 254 |
| `AgentId` | user value, else auto Base64(UUIDv7) | same as v1 | always auto Base64(UUIDv7); input ignored |
| `ServiceName` / `ApiKey` | not used | not used | **required** |
| gRPC `protocol.version` header | 100 | 100 | 400 |
| gRPC `servicename` / `apikey` headers | not sent | not sent | sent |

v1 and v3 are identical on the wire (both `protocol.version=100`); they differ only in the `ApplicationName` length limit. A missing/invalid required value (e.g. `ApplicationName`, or `ServiceName`/`ApiKey` for v4) aborts agent startup (the agent degrades to a no-op).

> **Note (v4):** because v4 generates a new `AgentId` on every startup, the agent's id changes across restarts — matching the Java agent's behavior.

---

## Logging Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Log.Level` | `PINPOINT_CPP_LOG_LEVEL` | string | `"info"` | `trace`, `debug`, `info`, `warn`, `error` |
| `Log.FilePath` | `PINPOINT_CPP_LOG_FILE_PATH` | string | `""` | Empty = stdout/stderr. Non-empty enables file logging with rotation. |
| `Log.MaxFileSize` | `PINPOINT_CPP_LOG_MAX_FILE_SIZE` | int | `10` | Max log file size in MB before rotation. |

---

## Collector Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Collector.GrpcHost` | `PINPOINT_CPP_GRPC_HOST` | string | `""` | **Required.** Pinpoint Collector hostname or IP. |
| `Collector.GrpcAgentPort` | `PINPOINT_CPP_GRPC_AGENT_PORT` | int | `9991` | gRPC port for agent metadata. |
| `Collector.GrpcSpanPort` | `PINPOINT_CPP_GRPC_SPAN_PORT` | int | `9993` | gRPC port for span data. |
| `Collector.GrpcStatPort` | `PINPOINT_CPP_GRPC_STAT_PORT` | int | `9992` | gRPC port for statistics data. |

---

## gRPC Transport Configuration

The C++ agent exposes Java-agent-style gRPC transport options under `Grpc`. Defaults use plaintext channels, 30s/60s keepalive, and 4MiB message limits. gRPC request deadlines are not configurable: request-style calls use a fixed 5000ms deadline, and the stat stream has no deadline.

### TLS

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Grpc.Ssl.TrustCertFilePath` | `PINPOINT_CPP_GRPC_SSL_TRUST_CERT_FILE_PATH` | string | `""` | PEM trust certificate path used by TLS credentials. |
| `Grpc.Ssl.RootCertFilePath` | `PINPOINT_CPP_GRPC_SSL_ROOT_CERT_FILE_PATH` | string | `""` | Alias for gRPC root certificate path. `TrustCertFilePath` takes precedence when both are set. |

### Channel Options

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Grpc.SslEnable` | `PINPOINT_CPP_GRPC_SSL_ENABLE` | bool | `false` | Enables TLS credentials for all gRPC channels. |
| `Grpc.KeepAliveTimeMs` | `PINPOINT_CPP_GRPC_KEEPALIVE_TIME_MS` | int | `30000` | Maps to `GRPC_ARG_KEEPALIVE_TIME_MS`. |
| `Grpc.KeepAliveTimeoutMs` | `PINPOINT_CPP_GRPC_KEEPALIVE_TIMEOUT_MS` | int | `60000` | Maps to `GRPC_ARG_KEEPALIVE_TIMEOUT_MS`. |
| `Grpc.KeepAlivePermitWithoutCalls` | `PINPOINT_CPP_GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS` | bool | `false` | Maps to `GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS`. |
| `Grpc.MaxSendMessageSize` | `PINPOINT_CPP_GRPC_MAX_SEND_MESSAGE_SIZE` | int | `4194304` | Maps to `GRPC_ARG_MAX_SEND_MESSAGE_LENGTH`. `-1` means unlimited. |
| `Grpc.MaxReceiveMessageSize` | `PINPOINT_CPP_GRPC_MAX_RECEIVE_MESSAGE_SIZE` | int | `4194304` | Maps to `GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH`. `-1` means unlimited. |
| `Grpc.SenderQueueSize` | `PINPOINT_CPP_GRPC_SENDER_QUEUE_SIZE` | int | `1000` | Applied to metadata queue. Span still uses `Span.QueueSize`; agent/stat have no separate C++ sender queue. |
| `Grpc.ChannelExecutorQueueSize` | `PINPOINT_CPP_GRPC_CHANNEL_EXECUTOR_QUEUE_SIZE` | int | `1000` | Parsed for Java config parity. The C++ gRPC API used here does not expose the same Netty executor queue. |

The same `Grpc` channel options are applied to the agent, metadata, span, and stat gRPC channels. Java-specific name resolver providers, custom interceptor injection, Netty channel type, channelz reporter wiring, retry/hedging service config, flow-control window, and write-buffer watermarks do not have a direct equivalent in the current C++ agent implementation.

```yaml
Grpc:
  Ssl:
    TrustCertFilePath: "/etc/pinpoint/collector-ca.pem"
  SslEnable: true
  SenderQueueSize: 1000
  MaxSendMessageSize: 4194304
  MaxReceiveMessageSize: 4194304
```

---

## Stat Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Stat.Enable` | `PINPOINT_CPP_STAT_ENABLE` | bool | `true` | Enable/disable system statistics collection. |
| `Stat.BatchCount` | `PINPOINT_CPP_STAT_BATCH_COUNT` | int | `6` | Number of stat batches collected before sending. |
| `Stat.BatchInterval` | `PINPOINT_CPP_STAT_BATCH_INTERVAL` | int | `5000` | Interval between collections in milliseconds. |

---

## Sampling Configuration

| YAML Key | Environment Variable | Type | Default | Range / Notes |
|---|---|---|---|---|
| `Sampling.Type` | `PINPOINT_CPP_SAMPLING_TYPE` | string | `"COUNTER"` | `"COUNTER"`, `"PERCENT"`, `"THROUGHPUT"` |
| `Sampling.CounterRate` | `PINPOINT_CPP_SAMPLING_COUNTER_RATE` | int | `1` | Sample 1/N transactions. `0` = disable. |
| `Sampling.PercentRate` | `PINPOINT_CPP_SAMPLING_PERCENT_RATE` | double | `100` | Clamped to `[0.01, 100]`. |
| `Sampling.NewThroughput` | `PINPOINT_CPP_SAMPLING_NEW_THROUGHPUT` | int | `0` | Target TPS for new transactions. `0` = unlimited. |
| `Sampling.ContinueThroughput` | `PINPOINT_CPP_SAMPLING_CONTINUE_THROUGHPUT` | int | `0` | Target TPS for continuing transactions. `0` = unlimited. |

> Out-of-range values are automatically normalised (clamped) by the agent during `make_config()`.

---

## Span Configuration

| YAML Key | Environment Variable | Type | Default | Range / Notes |
|---|---|---|---|---|
| `Span.QueueSize` | `PINPOINT_CPP_SPAN_QUEUE_SIZE` | int | `1024` | Min `1`. |
| `Span.MaxEventDepth` | `PINPOINT_CPP_SPAN_MAX_EVENT_DEPTH` | int | `64` | Min `2`. `-1` = unlimited. |
| `Span.MaxEventSequence` | `PINPOINT_CPP_SPAN_MAX_EVENT_SEQUENCE` | int | `5000` | Min `4`. `-1` = unlimited. |
| `Span.EventChunkSize` | `PINPOINT_CPP_SPAN_EVENT_CHUNK_SIZE` | int | `20` | Min `1`. Events per transmission chunk. |

> Negative or invalid values are coerced to safe defaults during `make_config()`.

---

## HTTP Configuration

### URL Statistics

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Http.CollectUrlStat` | `PINPOINT_CPP_HTTP_COLLECT_URL_STAT` | bool | `false` | Enable URL statistics collection. |
| `Http.UrlStatLimit` | `PINPOINT_CPP_HTTP_URL_STAT_LIMIT` | int | `1024` | Max unique URLs to track. |
| `Http.UrlStatEnableTrimPath` | `PINPOINT_CPP_HTTP_URL_STAT_ENABLE_TRIM_PATH` | bool | `true` | Enable URL path trimming for normalisation. |
| `Http.UrlStatTrimPathDepth` | `PINPOINT_CPP_HTTP_URL_STAT_TRIM_PATH_DEPTH` | int | `1` | URL path depth for normalisation (e.g., depth 2: `/api/users` → `/api/*`). Requires `UrlStatEnableTrimPath: true`. |
| `Http.UrlStatMethodPrefix` | `PINPOINT_CPP_HTTP_URL_STAT_METHOD_PREFIX` | bool | `false` | Prefix URL stat key with HTTP method (e.g., `GET:/api/users`). |

### Server-side Tracing

| YAML Key | Environment Variable | Type | Default |
|---|---|---|---|
| `Http.Server.StatusCodeErrors` | `PINPOINT_CPP_HTTP_SERVER_STATUS_CODE_ERRORS` | list&lt;string&gt; | `["5xx"]` |
| `Http.Server.ExcludeUrl` | `PINPOINT_CPP_HTTP_SERVER_EXCLUDE_URL` | list&lt;string&gt; | `[]` |
| `Http.Server.ExcludeMethod` | `PINPOINT_CPP_HTTP_SERVER_EXCLUDE_METHOD` | list&lt;string&gt; | `[]` |
| `Http.Server.RecordRequestHeader` | `PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER` | list&lt;string&gt; | `[]` |
| `Http.Server.RecordRequestCookie` | `PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_COOKIE` | list&lt;string&gt; | `[]` |
| `Http.Server.RecordResponseHeader` | `PINPOINT_CPP_HTTP_SERVER_RECORD_RESPONSE_HEADER` | list&lt;string&gt; | `[]` |

### Client-side Tracing

| YAML Key | Environment Variable | Type | Default |
|---|---|---|---|
| `Http.Client.RecordRequestHeader` | `PINPOINT_CPP_HTTP_CLIENT_RECORD_REQUEST_HEADER` | list&lt;string&gt; | `[]` |
| `Http.Client.RecordRequestCookie` | `PINPOINT_CPP_HTTP_CLIENT_RECORD_REQUEST_COOKIE` | list&lt;string&gt; | `[]` |
| `Http.Client.RecordResponseHeader` | `PINPOINT_CPP_HTTP_CLIENT_RECORD_RESPONSE_HEADER` | list&lt;string&gt; | `[]` |

> Environment variables for list types accept **comma-separated values**:
> ```bash
> export PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER="Content-Type,User-Agent,X-Request-Id"
> ```

---

## SQL Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Sql.MaxBindArgsSize` | `PINPOINT_CPP_SQL_MAX_BIND_ARGS_SIZE` | int | `1024` | Max bytes of SQL bind arguments to record. |
| `Sql.EnableSqlStats` | `PINPOINT_CPP_SQL_ENABLE_SQL_STATS` | bool | `false` | Aggregate execution counts even for unsampled traces. |

---

## Advanced Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `IsContainer` | `PINPOINT_CPP_IS_CONTAINER` | bool | auto-detected | Checks `/.dockerenv` or `KUBERNETES_SERVICE_HOST`. Set explicitly if auto-detection fails. |
| `EnableCallstackTrace` | `PINPOINT_CPP_ENABLE_CALLSTACK_TRACE` | bool | `false` | Capture stack trace when recording errors. |

---

## Configuration Hot Reload

The agent supports hot-reloading a subset of configuration options from the YAML configuration file **without restarting the application**. When the agent starts with a valid config file path, a background file-watcher thread monitors the file for changes (polling every 1 second). When a modification is detected the agent automatically re-reads the file, validates the new configuration, and applies it.

### How It Works

1. The file-watcher compares the file's last-write timestamp once per second.
2. When a change is detected, the file is re-read and parsed into a new `Config` object.
3. The new config passes the same validation (`check()`) as the initial config.
4. An **identity check** (`isReloadable`) ensures that immutable fields have not been changed (see below).
5. If validation passes, the agent atomically swaps the internal configuration and rebuilds the affected components.

### Reloadable vs. Non-Reloadable Options

Not all configuration options can be changed at runtime. Options that define the agent's identity or gRPC connection targets are **non-reloadable** — changing them requires an application restart.

| Category | Options | Reloadable? |
|---|---|---|
| Agent identity | `ApplicationName`, `ApplicationType`, `AgentId`, `AgentName`, `UidVersion`, `ServiceName`, `ApiKey` | No |
| Collector connection | `Collector.GrpcHost`, `GrpcAgentPort`, `GrpcSpanPort`, `GrpcStatPort` | No |
| Sampling | `Sampling.*` (Type, CounterRate, PercentRate, NewThroughput, ContinueThroughput) | **Yes** |
| HTTP filters | `Http.Server.ExcludeUrl`, `Http.Server.ExcludeMethod` | **Yes** |
| HTTP status errors | `Http.Server.StatusCodeErrors` | **Yes** |
| HTTP header recording | `Http.Server.RecordRequest/ResponseHeader`, `RecordRequestCookie`, `Http.Client.*` | **Yes** |

If the new config file changes any non-reloadable field, the reload is silently skipped and the agent continues with the previous configuration.

### Components Rebuilt on Reload

When a reload is accepted, the following internal components are rebuilt from the new configuration:

- **Sampler** — sampling strategy and rates are updated.
- **HTTP URL filter** — the URL exclusion list is replaced.
- **HTTP method filter** — the method exclusion list is replaced.
- **HTTP status error codes** — the error-code set is replaced.
- **HTTP header recorders** — server-side and client-side header recording rules are replaced.

All swaps are performed atomically (thread-safe), so in-flight requests are not affected.

### Requirements

- A **YAML configuration file path** must be set (via `SetConfigFilePath()` or `PINPOINT_CPP_CONFIG_FILE`). Hot reload does not apply to environment variables or inline config strings.
- The config file must exist at startup; the watcher is not started otherwise.

### Example: Changing Sampling Rate at Runtime

Initial config file:

```yaml
ApplicationName: "MyApp"
Collector:
  GrpcHost: "collector.example.com"
Sampling:
  Type: "PERCENT"
  PercentRate: 100
```

To reduce sampling to 10% without restart, edit the file in place:

```yaml
ApplicationName: "MyApp"
Collector:
  GrpcHost: "collector.example.com"
Sampling:
  Type: "PERCENT"
  PercentRate: 10
```

The agent detects the change within ~1 second and applies the new sampling rate. A log message is not emitted on success, but a warning is logged if the file cannot be parsed.

---

## Configuration Examples

### Example 1: Development

Full sampling, debug logging, local collector.

```yaml
ApplicationName: "MyApp-Dev"
Enable: true

Log:
  Level: "debug"
  FilePath: ""  # stdout

Collector:
  GrpcHost: "localhost"
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992

Sampling:
  Type: "COUNTER"
  CounterRate: 1  # sample all

Http:
  CollectUrlStat: true
  Server:
    RecordRequestHeader: ["*"]
    RecordResponseHeader: ["*"]

Sql:
  EnableSqlStats: true

EnableCallstackTrace: true
```

### Example 2: Production

Percentage sampling, reduced logging, selective header recording.

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
  PercentRate: 10.0

Stat:
  Enable: true
  BatchCount: 10
  BatchInterval: 10000

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

### Example 3: High-Traffic

Throughput-based sampling, large queues, minimal overhead.

```yaml
ApplicationName: "HighTrafficApp"
Enable: true

Log:
  Level: "error"

Collector:
  GrpcHost: "collector.example.com"

Sampling:
  Type: "THROUGHPUT"
  NewThroughput: 500
  ContinueThroughput: 1000

Stat:
  Enable: true
  BatchCount: 20
  BatchInterval: 5000

Span:
  QueueSize: 4096
  MaxEventDepth: 16
  MaxEventSequence: 500
  EventChunkSize: 100

Http:
  CollectUrlStat: false
  Server:
    StatusCodeErrors: ["5xx"]
    ExcludeUrl: ["/health", "/ping"]
    ExcludeMethod: ["OPTIONS", "HEAD"]
    RecordRequestHeader: []
    RecordResponseHeader: []

Sql:
  MaxBindArgsSize: 512
  EnableSqlStats: false

EnableCallstackTrace: false
```

### Example 4: Environment Variables Only

```bash
#!/bin/bash

# Agent
export PINPOINT_CPP_APPLICATION_NAME="MyApp"
export PINPOINT_CPP_AGENT_ID="app-server-01"
export PINPOINT_CPP_ENABLE="true"

# Collector
export PINPOINT_CPP_GRPC_HOST="pinpoint-collector"
export PINPOINT_CPP_GRPC_AGENT_PORT="9991"
export PINPOINT_CPP_GRPC_SPAN_PORT="9993"
export PINPOINT_CPP_GRPC_STAT_PORT="9992"

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

./my_application
```

---

## Best Practices

### Development
- `CounterRate: 1` — sample every transaction.
- `Log.Level: "debug"` — verbose output for diagnostics.
- `EnableCallstackTrace: true` — capture stack on errors.
- Record all headers (`["*"]`) for troubleshooting (non-production only).

### Production
- Use `PERCENT` or `THROUGHPUT` sampling to control overhead.
- `Log.Level: "warn"` or `"error"`.
- Exclude health-check / monitoring endpoints via `ExcludeUrl`.
- Record only the headers you need; avoid sensitive ones (`Authorization`, `Cookie`).

### High-Traffic
- Prefer `THROUGHPUT` sampling with explicit TPS caps.
- Increase `Span.QueueSize`; decrease `MaxEventDepth` and `MaxEventSequence`.
- Disable `CollectUrlStat` and `EnableSqlStats` if not needed.

### Container Deployments
- Set `IsContainer: true` explicitly if auto-detection fails.
- Use environment variables for configuration.
- Ensure unique `AgentId` per container (e.g., include hostname or pod name).

### Security
- Never record sensitive headers unless absolutely necessary.
- Limit `Sql.MaxBindArgsSize` to avoid capturing large payloads.
- Audit recorded headers and cookies regularly.

---

## Troubleshooting

### Agent Not Connecting
1. Verify `Collector.GrpcHost` and port values match the collector cluster.
2. Set `Log.Level: "debug"` and review startup logs — `make_config()` prints the resolved configuration.

### High Memory Usage
- Reduce `Span.QueueSize` (e.g., `512`).
- Lower `Span.MaxEventSequence` (e.g., `1000`).
- Reduce `Http.UrlStatLimit` (e.g., `512`).

### Performance Impact
- Lower sampling rate: `PercentRate: 5.0`.
- Disable `CollectUrlStat`, `EnableSqlStats`, and/or `Stat.Enable`.

### Missing Transactions
- Set `Sampling.CounterRate: 1` to sample all.
- Temporarily clear `Http.Server.ExcludeUrl` and `ExcludeMethod`.

---

## Full YAML Reference

Below is a complete YAML config with all keys and their default values:

```yaml
ApplicationName: ""
ApplicationType: 1300
AgentId: ""          # auto-generated if empty
AgentName: ""
Enable: true
IsContainer: false   # auto-detected if not set

Log:
  Level: "info"
  FilePath: ""       # empty = stdout/stderr
  MaxFileSize: 10

Collector:
  GrpcHost: ""
  GrpcAgentPort: 9991
  GrpcSpanPort: 9993
  GrpcStatPort: 9992

Grpc:
  Ssl:
    TrustCertFilePath: ""
    RootCertFilePath: ""
  SslEnable: false
  KeepAliveTimeMs: 30000
  KeepAliveTimeoutMs: 60000
  KeepAlivePermitWithoutCalls: false
  MaxSendMessageSize: 4194304
  MaxReceiveMessageSize: 4194304
  SenderQueueSize: 1000
  ChannelExecutorQueueSize: 1000

Stat:
  Enable: true
  BatchCount: 6
  BatchInterval: 5000

Sampling:
  Type: "COUNTER"
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
  CollectUrlStat: false
  UrlStatLimit: 1024
  UrlStatEnableTrimPath: true
  UrlStatTrimPathDepth: 1
  UrlStatMethodPrefix: false
  Server:
    StatusCodeErrors: ["5xx"]
    ExcludeUrl: []
    ExcludeMethod: []
    RecordRequestHeader: []
    RecordRequestCookie: []
    RecordResponseHeader: []
  Client:
    RecordRequestHeader: []
    RecordRequestCookie: []
    RecordResponseHeader: []

Sql:
  MaxBindArgsSize: 1024
  EnableSqlStats: false

EnableCallstackTrace: false
```

---

## Related Documentation

- [Quick Start Guide](quick_start.md)
- [Instrumentation Guide](instrumentation.md)
- Code examples: see the `example/` directory in the repository
- GitHub: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
- Pinpoint APM Docs: [https://pinpoint-apm.github.io/pinpoint/](https://pinpoint-apm.github.io/pinpoint/)

---

*Apache License 2.0 — See [LICENSE](../LICENSE) for details.*
