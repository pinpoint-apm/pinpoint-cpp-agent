# Pinpoint C++ Agent - Configuration Guide

This document is a consolidated reference for all configuration options available in the Pinpoint C++ Agent (`pinpoint-cpp-agent`). Every option is listed once, in the table for its section, with its YAML key, environment variable, type and default.

---

## Configuration Methods & Precedence

The agent merges configuration from three sources. **Later sources override earlier ones:**

1. **Default Values** (lowest priority) — built-in defaults.
2. **YAML Configuration File** — a config file path or an inline YAML string.
3. **Environment Variables** (highest priority) — `PINPOINT_CPP_*` variables applied last.

Values are normalised (clamped into range) after the merge.

> **Environment variables are read only while building the initial configuration** (before the agent exists). Once the agent is running, a [hot reload](#configuration-hot-reload) rebuilds the config from the file **without re-reading environment variables** — so a value provided only via an env var is fixed for the agent's lifetime and cannot change at runtime.

### Method 1: YAML Configuration File

Create a `pinpoint-config.yaml` file and set its path:

```cpp
pinpoint::AgentOptions options;
options.config_file_path = "/path/to/pinpoint-config.yaml";
pinpoint::StartAgent(options);
```

Or set the path via environment variable:

```bash
export PINPOINT_CPP_CONFIG_FILE="/path/to/pinpoint-config.yaml"
```

### Method 2: Environment Variables

```bash
export PINPOINT_CPP_APPLICATION_NAME="MyApplication"
export PINPOINT_CPP_COLLECTOR_HOST="localhost"
export PINPOINT_CPP_LOG_LEVEL="info"
```

Or programmatically, before `StartAgent()`:

```cpp
setenv("PINPOINT_CPP_APPLICATION_NAME", "MyApplication", 1);
setenv("PINPOINT_CPP_COLLECTOR_HOST", "localhost", 1);
```

List-typed options accept **comma-separated values** in an environment variable:

```bash
export PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER="Content-Type,User-Agent,X-Request-Id"
```

#### Customizing the environment variable prefix

All environment variable names use the `PINPOINT_CPP` prefix by default. Set `AgentOptions::env_prefix` to change it — the agent then reads `<prefix>_<suffix>` (e.g. `MYAPP_APPLICATION_NAME`). An empty prefix selects the default.

```cpp
pinpoint::AgentOptions options;
options.env_prefix = "MYAPP";   // read MYAPP_* instead of PINPOINT_CPP_*
```

### Method 3: Configuration String (Inline YAML)

```cpp
pinpoint::AgentOptions options;
options.config_yaml = R"(
    ApplicationName: "MyApplication"
    Collector:
      Host: "localhost"
    Sampling:
      Type: "PERCENT"
      PercentRate: 10
)";
pinpoint::StartAgent(options);
```

---

## Agent Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `ApplicationName` | `PINPOINT_CPP_APPLICATION_NAME` | string | `""` | **Required.** Name of the monitored application. Max 24 chars for `UidVersion: v1`, otherwise max 254 chars. |
| `AgentName` | `PINPOINT_CPP_AGENT_NAME` | string | `""` | Optional human-readable label (max 255 chars; 254 for v4). Falls back to the agent id when omitted. |
| `UidVersion` | `PINPOINT_CPP_UID_VERSION` | string | `v3` | Agent self-identity (ObjectName) version: `v1`, `v3`, or `v4` (case-insensitive; unknown/empty → `v3`). See [Identity Versions](#identity-versions). |
| `ServiceName` | `PINPOINT_CPP_SERVICE_NAME` | string | `""` | Only used for `UidVersion: v4` (max 254 chars); an unset value resolves to `DEFAULT` at startup. Unused for v1/v3. |
| `ApiKey` | `PINPOINT_CPP_API_KEY` | string | `""` | **Required for `UidVersion: v4`**. Unused for v1/v3. Never logged in plaintext. |
| `Enable` | `PINPOINT_CPP_ENABLE` | bool | `true` | Set `false` to disable tracing without code changes. **`StartAgent()` then returns `false`** and installs no agent — that is the success path for a deliberate disable, not a failure. See [Disabling the Agent](trouble_shooting.md#disabling-the-agent). |

> **Note:** The Pinpoint service type (formerly the `ApplicationType` YAML key) is no longer a configuration option. It is passed in code as `AgentOptions::app_type` and defaults to `APP_TYPE_CPP` (`1300`).

> **Agent id:** the agent id is not configurable. It is always auto-generated at startup as a 22-char URL-safe Base64 UUIDv7, so every process — including sibling pre-fork workers — gets a unique id. Use `AgentName` for a stable, human-readable label; it need not be unique. See the [Pre-fork Integration Guide](prefork.md).

### Identity Versions

`UidVersion` selects how the agent identifies itself to the Collector (mirrors the Java agent's `pinpoint.modules.uid.version`):

| | v1 | v3 (default) | v4 |
|---|---|---|---|
| `ApplicationName` max length | 24 | 254 | 254 |
| Agent id | auto Base64(UUIDv7) | same as v1 | auto Base64(UUIDv7) |
| `ServiceName` | not used | not used | used (falls back to `DEFAULT`) |
| `ApiKey` | not used | not used | **required** |
| gRPC `protocol.version` header | 100 | 100 | 400 |
| gRPC `servicename` / `apikey` headers | not sent | not sent | sent |

v1 and v3 are identical on the wire (both `protocol.version=100`); they differ only in the `ApplicationName` length limit. A missing/invalid required value (e.g. `ApplicationName`, or `ApiKey` for v4, or an invalid `ServiceName`) aborts agent startup (the agent degrades to a no-op).

> **Note:** because the agent id is generated afresh on every startup, it changes across restarts — matching the Java agent's v4 behavior. Use `AgentName` for a stable label.

---

## Logging Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Log.Level` | `PINPOINT_CPP_LOG_LEVEL` | string | `"info"` | `trace`, `debug`, `info`, `warn`, `error` |
| `Log.FilePath` | `PINPOINT_CPP_LOG_FILE_PATH` | string | `""` | Empty = stdout/stderr. Non-empty enables file logging with rotation. Supports the per-worker placeholder `%pid%`, which expands to the process id. |
| `Log.MaxFileSize` | `PINPOINT_CPP_LOG_MAX_FILE_SIZE` | int | `10` | Max log file size in MB before rotation. |

`LogLevel` is accepted as a legacy top-level YAML alias for `Log.Level`. Prefer `Log.Level`; when both are present, `Log.Level` wins.

> **Multi-process hosts:** the built-in size rotation is not safe when several
> worker processes share one log file — use the `%pid%` placeholder to give
> each worker its own file (e.g.
> `FilePath: "/var/log/pinpoint/agent-%pid%.log"`). See the
> [Pre-fork Integration Guide](prefork.md).

---

## Collector Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Collector.Host` | `PINPOINT_CPP_COLLECTOR_HOST` | string | `""` | **Required.** Pinpoint Collector hostname or IP. |
| `Collector.AgentPort` | `PINPOINT_CPP_COLLECTOR_AGENT_PORT` | int | `9991` | gRPC port for agent metadata. Valid range: `1`-`65535`. |
| `Collector.SpanPort` | `PINPOINT_CPP_COLLECTOR_SPAN_PORT` | int | `9993` | gRPC port for span data. Valid range: `1`-`65535`. |
| `Collector.StatPort` | `PINPOINT_CPP_COLLECTOR_STAT_PORT` | int | `9992` | gRPC port for statistics data. Valid range: `1`-`65535`. |
| `Collector.SpanBatch.Size` | `PINPOINT_CPP_SPAN_BATCH_SIZE` | int | `20` | Min `1`. Max spans collected per send batch. |
| `Collector.SpanBatch.FlushIntervalMs` | `PINPOINT_CPP_SPAN_BATCH_FLUSH_INTERVAL_MS` | int | `1000` | Min `1`. Span batch flush interval in milliseconds. |
| `Collector.SpanBatch.CollectDeadlineMs` | `PINPOINT_CPP_SPAN_BATCH_COLLECT_DEADLINE_MS` | int | `500` | Min `0`. Deadline for collecting a batch before send. |
| `Collector.SpanBatch.MaxConcurrentRequests` | `PINPOINT_CPP_SPAN_BATCH_MAX_CONCURRENT_REQUESTS` | int | `10` | Min `1`. Max concurrent span-send requests. |
| `Collector.AgentInfo.RefreshIntervalMs` | `PINPOINT_CPP_AGENT_INFO_REFRESH_INTERVAL_MS` | int | `86400000` | AgentInfo refresh interval in milliseconds. |
| `Collector.AgentInfo.SendRetryIntervalMs` | `PINPOINT_CPP_AGENT_INFO_SEND_RETRY_INTERVAL_MS` | int | `3000` | Retry interval for sending AgentInfo. |
| `Collector.AgentInfo.MaxTryPerAttempt` | `PINPOINT_CPP_AGENT_INFO_MAX_TRY_PER_ATTEMPT` | int | `3` | Max send attempts per AgentInfo refresh. |

> **Deprecated aliases.** The earlier keys `Collector.GrpcHost`, `Collector.GrpcAgentPort`, `Collector.GrpcSpanPort`, `Collector.GrpcStatPort` (env `PINPOINT_CPP_GRPC_HOST`, `PINPOINT_CPP_GRPC_AGENT_PORT`, `PINPOINT_CPP_GRPC_SPAN_PORT`, `PINPOINT_CPP_GRPC_STAT_PORT`) are still honored as a fallback for backward compatibility, but are deprecated. Prefer the `Collector.Host` / `Collector.*Port` keys above; when both are set, the preferred key wins.

---

## gRPC Transport Configuration

The C++ agent exposes Java-agent-style gRPC transport options under `Grpc`. Defaults use plaintext channels, 30s/60s keepalive, and 4MiB message limits. gRPC request deadlines are not configurable: request-style calls use a fixed 5000ms deadline, and the stat stream has no deadline.

### TLS

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Collector.Grpc.SslEnable` | `PINPOINT_CPP_GRPC_SSL_ENABLE` | bool | `false` | Enables TLS credentials for all gRPC channels. |
| `Collector.Grpc.TrustCertFilePath` | `PINPOINT_CPP_GRPC_SSL_TRUST_CERT_FILE_PATH` | string | `""` | PEM trust certificate path used by TLS credentials. |
| `Collector.Grpc.RootCertFilePath` | `PINPOINT_CPP_GRPC_SSL_ROOT_CERT_FILE_PATH` | string | `""` | Alias for gRPC root certificate path. `TrustCertFilePath` takes precedence when both are set. |

### Channel Options

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Collector.Grpc.KeepAliveTimeMs` | `PINPOINT_CPP_GRPC_KEEPALIVE_TIME_MS` | int | `30000` | Maps to `GRPC_ARG_KEEPALIVE_TIME_MS`. |
| `Collector.Grpc.KeepAliveTimeoutMs` | `PINPOINT_CPP_GRPC_KEEPALIVE_TIMEOUT_MS` | int | `60000` | Maps to `GRPC_ARG_KEEPALIVE_TIMEOUT_MS`. |
| `Collector.Grpc.KeepAlivePermitWithoutCalls` | `PINPOINT_CPP_GRPC_KEEPALIVE_PERMIT_WITHOUT_CALLS` | bool | `false` | Maps to `GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS`. |
| `Collector.Grpc.MaxSendMessageSize` | `PINPOINT_CPP_GRPC_MAX_SEND_MESSAGE_SIZE` | int | `4194304` | Maps to `GRPC_ARG_MAX_SEND_MESSAGE_LENGTH`. `-1` means unlimited. |
| `Collector.Grpc.MaxReceiveMessageSize` | `PINPOINT_CPP_GRPC_MAX_RECEIVE_MESSAGE_SIZE` | int | `4194304` | Maps to `GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH`. `-1` means unlimited. |
| `Collector.Grpc.SenderQueueSize` | `PINPOINT_CPP_GRPC_SENDER_QUEUE_SIZE` | int | `1000` | Valid range: `1`-`65536`. Applied to metadata queue. Span still uses `Span.QueueSize`; agent/stat have no separate C++ sender queue. |

The same `Grpc` channel options are applied to the agent, metadata, span, and stat gRPC channels. Java-specific name resolver providers, custom interceptor injection, Netty channel type, channelz reporter wiring, retry/hedging service config, flow-control window, and write-buffer watermarks do not have a direct equivalent in the current C++ agent implementation.

---

## Stat Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Stat.Enable` | `PINPOINT_CPP_STAT_ENABLE` | bool | `true` | Enable/disable system statistics collection. |
| `Stat.BatchCount` | `PINPOINT_CPP_STAT_BATCH_COUNT` | int | `6` | Number of stat batches collected before sending. Valid range: `1`-`100`. |
| `Stat.BatchInterval` | `PINPOINT_CPP_STAT_BATCH_INTERVAL` | int | `5000` | Interval between collections in milliseconds. Valid range: `1000`-`60000`. |

---

## Sampling Configuration

| YAML Key | Environment Variable | Type | Default | Range / Notes |
|---|---|---|---|---|
| `Sampling.Type` | `PINPOINT_CPP_SAMPLING_TYPE` | string | `"COUNTER"` | `"COUNTER"` or `"PERCENT"` (case-insensitive). Values other than `PERCENT` use counter sampling. |
| `Sampling.CounterRate` | `PINPOINT_CPP_SAMPLING_COUNTER_RATE` | int | `1` | Sample 1/N transactions. `0` = disable. |
| `Sampling.PercentRate` | `PINPOINT_CPP_SAMPLING_PERCENT_RATE` | double | `100` | Negative values become `0`; positive values below `0.01` become `0.01`; values above `100` become `100`. |
| `Sampling.NewThroughput` | `PINPOINT_CPP_SAMPLING_NEW_THROUGHPUT` | int | `0` | Target TPS for new transactions. `0` = unlimited. |
| `Sampling.ContinueThroughput` | `PINPOINT_CPP_SAMPLING_CONTINUE_THROUGHPUT` | int | `0` | Target TPS for continuing transactions. `0` = unlimited. |

Throughput limiting is not a separate `Sampling.Type`; it is enabled automatically when `NewThroughput` or `ContinueThroughput` is greater than `0`. How the samplers behave, and how the decision propagates across services, is described in [Instrumentation Guide §12](instrument.md#12-sampling-policy).

> Out-of-range values are automatically normalised (clamped) by the agent during `make_config()`.

---

## Span Configuration

| YAML Key | Environment Variable | Type | Default | Range / Notes |
|---|---|---|---|---|
| `Span.QueueSize` | `PINPOINT_CPP_SPAN_QUEUE_SIZE` | int | `1024` | Valid range: `1`-`65536`. |
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
| `Http.UrlStatLimit` | `PINPOINT_CPP_HTTP_URL_STAT_LIMIT` | int | `1024` | Max unique URL stat keys to track. `0` records none; negative values fall back to the default. |
| `Http.UrlStatQueueSize` | `PINPOINT_CPP_HTTP_URL_STAT_QUEUE_SIZE` | int | `1024` | Max URL stat records buffered while waiting for aggregation; records beyond it are dropped. Valid range `1`–`65536`; out-of-range values fall back to the default. |
| `Http.UrlStatEnableTrimPath` | `PINPOINT_CPP_HTTP_URL_STAT_ENABLE_TRIM_PATH` | bool | `true` | Enable URL path trimming for normalisation. |
| `Http.UrlStatTrimPathDepth` | `PINPOINT_CPP_HTTP_URL_STAT_TRIM_PATH_DEPTH` | int | `1` | Number of leading path segments kept during normalisation; a trimmed path gets a `*` suffix (depth `1`: `/api/users` → `/api/*`; depth `2`: `/api/v1/users` → `/api/v1/*`). A path with no more segments than the depth is kept as-is (depth `2`: `/api/users` → `/api/users`). Values below `1` are treated as `1`. Requires `UrlStatEnableTrimPath: true`. |
| `Http.UrlStatMethodPrefix` | `PINPOINT_CPP_HTTP_URL_STAT_METHOD_PREFIX` | bool | `false` | Prefix URL stat key with the HTTP method and a space (e.g., `GET /api/users`). |

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

Exclusion patterns, `HEADERS-ALL`, and the wildcard rules for `ExcludeUrl` are documented in [Instrumentation Guide §13](instrument.md#13-http-filtering-and-header-recording).

---

## SQL Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `Sql.MaxBindArgsSize` | `PINPOINT_CPP_SQL_MAX_BIND_ARGS_SIZE` | int | `1024` | Max bytes of SQL bind arguments to record. Bind values are recorded only when this is greater than `0`; negative values are clamped to `0`. An argument that would exceed a positive limit is omitted and the value ends with `...(N)`. |
| `Sql.EnableSqlStats` | `PINPOINT_CPP_SQL_ENABLE_SQL_STATS` | bool | `false` | Aggregate execution counts even for unsampled traces. |
| `Sql.EnableRawSqlCache` | `PINPOINT_CPP_SQL_ENABLE_RAW_SQL_CACHE` | bool | `true` | Cache normalized SQL and bind parameters by raw SQL text to avoid repeated normalization. |
| `Sql.TraceBindValue` | `PINPOINT_CPP_SQL_TRACE_BIND_VALUE` | bool | `true` | Record SQL bind values in span-event annotations. |

---

## Advanced Configuration

| YAML Key | Environment Variable | Type | Default | Notes |
|---|---|---|---|---|
| `IsContainer` | `PINPOINT_CPP_IS_CONTAINER` | bool | auto-detected | Checks `/.dockerenv` or `KUBERNETES_SERVICE_HOST`. Set explicitly if auto-detection fails. |
| `EnableCallstackTrace` | `PINPOINT_CPP_ENABLE_CALLSTACK_TRACE` | bool | `false` | Capture stack trace when recording errors. |
| `EnableConfigFileWatcher` | `PINPOINT_CPP_ENABLE_CONFIG_FILE_WATCHER` | bool | `false` | Watch the YAML config file and hot-reload changes at runtime. See [Configuration Hot Reload](#configuration-hot-reload). Non-reloadable: evaluated once at agent start. |

---

## Configuration Hot Reload

With `EnableConfigFileWatcher: true` (or `PINPOINT_CPP_ENABLE_CONFIG_FILE_WATCHER=true`;
default `false`) the agent hot-reloads a subset of options from the YAML config
file **without restarting the application**. A background thread compares the
file's last-write timestamp once per second; on a change the file is re-read,
validated and applied. Environment variables are **not** re-read.

Requirements: the watcher toggle must be on (it is evaluated once at agent start
and cannot itself be reloaded), and a config file path must be set — via
`AgentOptions::config_file_path` or `PINPOINT_CPP_CONFIG_FILE` — and exist at
startup. Inline YAML strings are not watched.

### Reloadable vs. Non-Reloadable Options

Options that define the agent's identity or its gRPC connection targets are
**non-reloadable** — changing them requires an application restart.

| Category | Options | Reloadable? |
|---|---|---|
| Agent identity | `ApplicationName`, `AgentName`, `UidVersion`, `ServiceName`, `ApiKey` | No |
| Collector / gRPC connection | `Collector.Host`, `Collector.AgentPort`, `Collector.SpanPort`, `Collector.StatPort`, `Collector.Grpc.*` | No |
| Config-file watcher | `EnableConfigFileWatcher` | No |
| Sampling | `Sampling.*` (Type, CounterRate, PercentRate, NewThroughput, ContinueThroughput) | **Yes** |
| HTTP filters | `Http.Server.ExcludeUrl`, `Http.Server.ExcludeMethod` | **Yes** |
| HTTP status errors | `Http.Server.StatusCodeErrors` | **Yes** |
| HTTP header recording | `Http.Server.RecordRequest/ResponseHeader`, `RecordRequestCookie`, `Http.Client.*` | **Yes** |
| SQL tracing | `Sql.MaxBindArgsSize`, `Sql.EnableSqlStats`, `Sql.EnableRawSqlCache`, `Sql.TraceBindValue` | **Yes** |

The reload is **always applied**: a change to a non-reloadable field is ignored —
the running value is kept — and logged as a warning, while reloadable changes in
the same file still take effect.

### What Is Rebuilt

Each component is rebuilt **only if its backing configuration changed**, so
untouched components keep running as-is with their accumulated state (such as
throughput-sampler counters): the sampler (`Sampling.*`), the URL filter
(`ExcludeUrl`), the method filter (`ExcludeMethod`), the status error codes
(`StatusCodeErrors`), and the header recorders (any server- or client-side
header/cookie list).

Rebuilt components are published together in a **single atomic swap**, so
in-flight requests can never observe a half-applied reload. Each span snapshots
the configuration once at creation, so per-span options (e.g. `Span.MaxEventDepth`,
`Span.EventChunkSize`, `Sql.EnableSqlStats`, `Sql.TraceBindValue`) take effect for
spans **created after** the reload; an already-open span keeps its values.

A successful reload logs `agent config reloaded`; a warning is logged instead if
the file cannot be parsed or changed a non-reloadable field.

---

## Configuration Examples

### Development

Full sampling, debug logging, local collector.

```yaml
ApplicationName: "MyApp-Dev"

Log:
  Level: "debug"
  FilePath: ""  # stdout

Collector:
  Host: "localhost"

Sampling:
  Type: "COUNTER"
  CounterRate: 1  # sample all

Http:
  CollectUrlStat: true
  Server:
    RecordRequestHeader: ["HEADERS-ALL"]
    RecordResponseHeader: ["HEADERS-ALL"]

Sql:
  EnableSqlStats: true
  TraceBindValue: true

EnableCallstackTrace: true
```

### Production

Throughput-capped percentage sampling, reduced logging, selective header recording.

```yaml
ApplicationName: "MyApp-Prod"
AgentName: "prod-server-01"

Log:
  Level: "warn"
  FilePath: "/var/log/pinpoint/agent.log"
  MaxFileSize: 50

Collector:
  Host: "pinpoint-collector.prod.example.com"

Sampling:
  Type: "PERCENT"
  PercentRate: 10.0
  NewThroughput: 500        # omit both for unlimited
  ContinueThroughput: 1000

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
    ExcludeMethod: ["OPTIONS", "HEAD"]
    RecordRequestHeader: ["Content-Type", "User-Agent"]
    RecordResponseHeader: ["Content-Type"]

Sql:
  MaxBindArgsSize: 512
  TraceBindValue: false

EnableCallstackTrace: false
```

---

## Best Practices

The two examples above are the starting points for development and production.
Beyond them:

### High-Traffic
- Prefer explicit `NewThroughput` / `ContinueThroughput` TPS caps.
- Increase `Span.QueueSize`; decrease `MaxEventDepth` and `MaxEventSequence`.
- Disable `CollectUrlStat` and `EnableSqlStats` if not needed.

### Container Deployments
- Set `IsContainer: true` explicitly if auto-detection fails.
- Use environment variables for configuration.
- The agent id is auto-generated per process, so containers never collide; set a distinct `AgentName` (e.g., hostname or pod name) for a recognizable label.

### Security
- Never record sensitive headers unless absolutely necessary.
- Disable `Sql.TraceBindValue` when bind values are not required for diagnostics.
- Limit `Sql.MaxBindArgsSize` to avoid capturing large payloads.
- Audit recorded headers and cookies regularly.

---

## Symptom → Key Index

Diagnosis lives in the [Troubleshooting Guide](trouble_shooting.md). What
belongs here is the reverse index — which key to reach for once you know the
symptom:

| Symptom | Keys to change |
|---|---|
| Agent never connects | `Collector.Host`, `Collector.*Port` ([Collector](#collector-configuration)) |
| Nothing is traced at all | `Enable` — `false` disables the agent and makes `StartAgent()` return `false` ([Agent](#agent-configuration)) |
| Transactions missing | `Sampling.CounterRate: 1` to sample all; clear `Http.Server.ExcludeUrl` / `ExcludeMethod` ([Sampling](#sampling-configuration), [HTTP](#http-configuration)) |
| Memory too high | Lower `Span.QueueSize`, `Span.MaxEventSequence`, `Http.UrlStatLimit` ([Span](#span-configuration), [HTTP](#http-configuration)) |
| CPU / latency overhead | Lower `Sampling.PercentRate` or set `NewThroughput` / `ContinueThroughput`; turn off `Http.CollectUrlStat`, `Sql.EnableSqlStats`, `Stat.Enable` |
| Traces truncated | Raise `Span.MaxEventDepth` / `MaxEventSequence` (`-1` = unlimited) ([Span](#span-configuration)) |
| A setting seems ignored | Set `Log.Level: "debug"` — the agent logs the configuration it **resolved**, after file, env vars and clamping. An env var set only in the environment silently wins over the file ([Precedence](#configuration-methods--precedence)) |

Sizing guidance for each of these is in [Best Practices](#best-practices) above.
