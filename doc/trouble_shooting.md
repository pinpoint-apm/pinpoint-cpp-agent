# Pinpoint C++ Agent - Troubleshooting Guide

This guide helps you diagnose and resolve common issues with the Pinpoint C++ Agent.

---

## Verifying Agent Startup

This section is the canonical description of the agent's startup contract; the
other guides link here rather than repeating it.

**`StartAgent()` is asynchronous.** It returns as soon as initialization is
*launched*: a background thread opens the gRPC channels, registers the agent with
the collector (retrying indefinitely until the collector accepts it) and starts
the workers. `Enable()` flips to `true` only after that registration succeeds, so
it is normally still `false` right after `StartAgent()` returns — that is not an
error, and checking it there proves nothing. Use `Enable()` only as a fast-fail
guard before creating a span, never as a startup success check.

**A `false` return is a *synchronous* configuration or setup failure** (it never
throws). Nothing is installed as the global agent, and a later `StartAgent()`
call retries from scratch. Print a message pointing at the agent log, where the
cause is recorded.

**The agent log is the only authoritative startup signal:**

| In the agent log | Meaning |
|---|---|
| `AgentInfo sent` | The agent registered with the collector. Startup succeeded. |
| `agent start failed: ...` | Configuration or setup error; the line names the cause. |
| `failed to init grpc workers: ...` | gRPC bring-up error. |
| `failed to send AgentInfo` | The collector is not reachable yet. Retried indefinitely. |
| *(nothing at all)* | A deliberate `Enable: false` returns `false` and logs nothing — see [Disabling the Agent](#disabling-the-agent). |

Set `Log.Level: "debug"` for the full picture, including the resolved
configuration. Whatever the log says, **a failed agent start never affects the
application**: every tracing call degrades to a safe no-op, the application runs
exactly as it would without the agent, and only the traces are lost.

The C API behaves identically: `pt_start_agent()` returns `0` for a synchronous
failure and `pt_agent_is_enabled()` returns `1` only after registration.

---

## Disabling the Agent

If the agent causes disruptions or problems to a production application, you can disable it without removing the agent from your code.

### Config Option

Disable the agent by setting the config option **`Enable`** to `false`.

**Option 1: YAML Configuration File**

```yaml
Enable: false
```

**Option 2: Environment Variable**

```bash
export PINPOINT_CPP_ENABLE="false"
```

Or programmatically in your application:

```cpp
#include <cstdlib>

int main() {
    setenv("PINPOINT_CPP_ENABLE", "false", 1);

    // Tracing is disabled: StartAgent() returns false, no agent is
    // installed, and no tracing data is collected. Do NOT print a
    // "failed to start" message for this return value — see the note below.
    pinpoint::StartAgent();

    // Your application code

    return 0;
}
```

> **A deliberate disable returns `false`, just like a failure.** `Enable: false`
> and a real configuration error are indistinguishable from the return value
> alone, and the disable is the one case that writes **nothing** to the agent
> log — so the usual "check the agent log" message points at an empty log. In a
> deployment that may be configured with `Enable: false`, either skip the
> `StartAgent()` call when you know tracing is off, or word the message so it
> does not read as an error (e.g. "pinpoint tracing is not active"). Everything
> else keeps working: `GlobalAgent()` hands out the noop agent and every tracing
> call is a safe no-op.

For more information, refer to the [Configuration Guide](config.md).

---

## Stopping and Resuming the Agent

`Shutdown()` stops the agent at runtime with no application restart: it flushes
pending data, joins the worker threads, and stops sending to the collector. It is
**terminal for that agent instance** — the same handle can never come back
online, `Enable()` stays `false`, and every span it returns is a noop span. The
application itself keeps running normally; it just stops being traced.

To resume tracing later, build a **fresh agent** with `StartAgent()`. No agent
pointer or lock of your own is needed: `StartAgent()` installs the started agent
as the process-wide global agent, `GlobalAgent()` hands it back from anywhere (or
the noop agent when none is installed), and both calls are thread-safe.

```cpp
// Stop tracing. The application keeps working; it is simply no longer traced.
pinpoint::GlobalAgent()->Shutdown();

// ... later: resume tracing with a NEW agent ...
if (!pinpoint::StartAgent(options)) {
    std::cerr << "failed to restart the pinpoint agent: check the agent log" << std::endl;
}
auto agent = pinpoint::GlobalAgent();
```

What each call guarantees after `Shutdown()`:

| Call | Behavior after `Shutdown()` |
|---|---|
| `agent->Enable()` | Always `false`. |
| `agent->NewSpan(...)` | Returns the shared noop span. Safe to call, records nothing. |
| `pinpoint::StartAgent(...)` | Builds a fresh agent, installs it as the global agent and returns `true`; returns `false` when the rebuild fails. |
| `pinpoint::GlobalAgent()` | Returns the noop agent until `StartAgent()` installs a new one. |

Points to keep in mind:

- **Never reuse the shut-down handle.** It stays permanently disabled; get the
  replacement handle from `StartAgent()`. The new agent registers with the
  collector in the background — watch the agent log (`AgentInfo sent`) if you
  need confirmation that it came online.
- **Drop cached agent pointers.** Any `AgentPtr` the application cached — in a
  thread-local, a service object, a C `pt_agent_t` handle — still points at the
  dead agent and keeps producing noop spans. Re-fetch with `GlobalAgent()` or the
  new handle.
- **Spans that outlive the shutdown are safe.** A span created before `Shutdown()`
  can still be ended afterwards without crashing; its data is simply dropped.
- **Each cycle registers as a new agent instance.** Every `StartAgent()`
  re-resolves the agent identity with a freshly auto-generated agent id. Set
  `AgentName` for a stable label in the Pinpoint UI across cycles.
- **Shutdown is synchronous but bounded.** It joins the worker threads while
  queued spans drain, and returns within a 3-second deadline even if a worker
  is stuck in an unresponsive RPC (stragglers finish draining on a background
  thread). Still, do not call it from a latency-sensitive path.
- **Calling `StartAgent()` while an agent is running** is safe: it leaves the
  running agent untouched and returns `true` with a warning log.

In C the same cycle is `pt_agent_shutdown()` + `pt_agent_destroy()` followed by a
new `pt_start_agent()`; see the [C API guide](instrument_c.md#3-bootstrapping-the-agent).

---

## Logging

Pinpoint C++ Agent outputs logs related to agent operation (configuration, gRPC, span collection, etc.). These logs are essential for the debugging process.

### Log Output

By default, logs are written to **stdout/stderr**. You can configure file-based logging with automatic rotation:

```yaml
Log:
  Level: "info"
  FilePath: "/var/log/pinpoint/agent.log"
  MaxFileSize: 10  # MB — rotates when this size is reached
```

### Log Levels

Use the config option **`Log.Level`** to increase the granularity of the agent's logging.

Available log levels (from most to least verbose):

- `trace` — very detailed debugging information
- `debug` — debugging information
- `info` — informational messages (default)
- `warn` — warning messages
- `error` — error messages only

**Setting Log Level via YAML:**

```yaml
Log:
  Level: "debug"
```

**Setting Log Level via Environment Variable:**

```bash
export PINPOINT_CPP_LOG_LEVEL="debug"
```

For more information, refer to the [Configuration Guide](config.md).

### Viewing Logs

**Console Output:**

```bash
# Run your application and capture logs
./my_application 2>&1 | tee app.log
```

**File Output:**

```bash
# Tail the log file in real time
tail -f /var/log/pinpoint/agent.log

# Search for errors
grep -i error /var/log/pinpoint/agent.log
```

### Debug Mode

For troubleshooting, enable debug mode to see detailed information:

```yaml
Enable: true

Log:
  Level: "debug"
  FilePath: "/tmp/pinpoint-debug.log"

# Enable call stack traces on errors
EnableCallstackTrace: true
```

---

## Common Issues

### Issue 1: Agent Not Starting

**Symptoms:**
- Application runs but no data appears in Pinpoint UI.
- Agent initialization fails.

**Diagnosis:** read the agent log — see [Verifying Agent Startup](#verifying-agent-startup)
for what each entry means and why `Enable()` right after `StartAgent()` diagnoses
nothing. An **empty** agent log next to a `false` return points at `Enable: false`
rather than a broken configuration; check that first.

**Solutions:**

1. **Check Configuration** — ensure required fields are set:

   ```yaml
   ApplicationName: "MyApp"  # Required
   Collector:
     Host: "localhost"       # Required
   ```

2. **Verify Collector Connection** — see [Connection Issues](#connection-issues).

3. **Check Permissions** — ensure the log directory is writable:

   ```bash
   ls -la /var/log/pinpoint/
   ```

4. **Check the *resolved* configuration** — with `Log.Level: "debug"` the agent
   logs the configuration it actually resolved at startup, after the file,
   environment variables and defaults have been merged. This is the fastest way
   to catch a setting that never took effect: a typo'd YAML key, a stale
   `PINPOINT_CPP_*` variable in the environment overriding the file, or a value
   clamped into range. Compare it against what you intended, not against your
   config file.

### Issue 2: No Data in Pinpoint UI

**Symptoms:**
- Agent starts successfully but no traces appear.

**Solutions:**

1. **Check Sampling Configuration:**

   ```yaml
   Sampling:
     Type: "COUNTER"
     CounterRate: 1  # Sample all transactions
   ```

2. **Verify Spans Are Ended** — `EndSpan()` must run on every code path; a span
   released without it is never sent. See
   [Missing Spans](#missing-spans) below.

3. **Check Application Name** — `ApplicationName` must be set in configuration.

4. **Wait for Collection** — data may take 5–10 seconds to appear depending on the stat collection interval.

5. **Check the collector side** — the agent log can show a clean `AgentInfo sent`
   and spans still be rejected downstream. Review the Pinpoint **collector**
   logs for errors before assuming the agent is at fault.

### Issue 3: High Memory Usage

**Symptoms:**
- Application memory usage grows over time.

**Solutions:**

1. **Always End Spans** — an unended span holds its buffered events and
   exceptions for as long as it lives. Use the RAII guard shown in
   [instrument.md §6.2](instrument.md#62-end-exactly-once-and-record-before-ending).

2. **Reduce buffer sizes and collection limits:**

   ```yaml
   Span:
     QueueSize: 512            # Reduce from default 1024
     MaxEventDepth: 32         # Limit depth
     MaxEventSequence: 500     # Reduce from default 5000

   Http:
     UrlStatLimit: 512         # Reduce from default 1024
   ```

### Issue 4: High CPU Usage or Slow Responses

**Symptoms:**
- Application CPU usage or request latency is higher than expected.

**Solutions:**

1. **Reduce sampling**, or cap it by throughput:

   ```yaml
   Sampling:
     Type: "PERCENT"
     PercentRate: 10.0         # Sample only 10%
     NewThroughput: 100        # Or: accept all, cap at 100 new tx/sec
     ContinueThroughput: 200
   ```

2. **Disable what you do not read:**

   ```yaml
   Http:
     CollectUrlStat: false

   Sql:
     EnableSqlStats: false
     TraceBindValue: false

   Stat:
     Enable: false
   ```

3. **Reduce the work per transaction** — trace only the paths that matter, prefer
   fewer coarser span events over very fine-grained ones, and cut the volume and
   size of annotations and recorded headers (`HEADERS-ALL` is a debugging
   setting, not a production one).

4. **Skip expensive collection on unsampled spans:**

   ```cpp
   if (span->IsSampled()) {
       collectDetailedMetrics();
   }
   ```

---

## Connection Issues

### Cannot Connect to Collector

**Symptoms:**
- Logs show connection errors or gRPC errors.

**Diagnosis:**

1. **Check Collector Address:**

   ```yaml
   Collector:
     Host: "pinpoint-collector.example.com"
     AgentPort: 9991
     SpanPort: 9993
     StatPort: 9992
   ```

2. **Test Network Connectivity** to each gRPC port (9991–9993), e.g.
   `telnet pinpoint-collector.example.com 9991` and `nslookup pinpoint-collector.example.com`.

**Solutions:**

1. Update collector address to the correct hostname/IP.
2. Configure firewall rules to allow traffic on gRPC ports (9991–9993).
3. Check network policies (e.g., in Kubernetes).
4. Verify the collector is running and healthy.

---

## Data Collection Issues

### Missing Spans

1. **Check Sampling Rate** — temporarily set to sample all:

   ```yaml
   Sampling:
     CounterRate: 1
   ```

2. **Verify Span Ending** — ensure `EndSpan()` is called on every code path.

3. **Check Excluded URLs and Methods** — temporarily remove both exclusions; a
   filter match produces a noop span, so the transaction disappears entirely:

   ```yaml
   Http:
     Server:
       ExcludeUrl: []
       ExcludeMethod: []
   ```

   Note that `ExcludeMethod` only applies when the span is created with the
   `NewSpan(operation, rpc_point, method, reader)` overload — without the method
   argument there is nothing to match.

### Incomplete Traces

1. **Increase Limits:**

   ```yaml
   Span:
     MaxEventDepth: -1      # Unlimited
     MaxEventSequence: -1   # Unlimited
   ```

2. **Check Event Ending** — ensure every `NewSpanEvent()` has a matching `EndEvent()` on the returned event:

   ```cpp
   auto se = span->NewSpanEvent("operation");
   // ... do work ...
   se->EndEvent();  // Must call this!
   ```

### Missing Distributed Tracing

1. **Verify Context Propagation** — both inject and extract must be implemented:

   ```cpp
   // Server: Extract context
   HttpHeaderReader reader(req.headers);
   auto span = agent->NewSpan("Service", "/endpoint", reader);

   // Client: Inject context through the span event for the outbound call
   auto se = span->NewSpanEvent("outgoing-call");
   HttpHeaderReaderWriter writer(headers);
   se->InjectContext(writer);
   ```

2. **Check Headers** — verify Pinpoint headers are present in the request:

   ```cpp
   std::cout << "Trace ID: "
             << req.get_header_value("Pinpoint-TraceID") << std::endl;
   ```

3. **Check for Header Stripping** — gateways or proxies may strip or rewrite Pinpoint headers.

4. **Check your reader's header lookup is case-insensitive** — the agent asks
   your `TraceContextReader` for the canonical spellings (`Pinpoint-TraceID`,
   `Pinpoint-Sampled`, ...), but HTTP header names are case-insensitive on the
   wire and proxies and HTTP/2 clients routinely re-case them (`pinpoint-traceid`).
   A reader backed by a case-sensitive container silently finds nothing and every
   request looks like a new transaction. `httplib::Headers` is case-insensitive by
   default, which is why
   [`example/http_trace_context.h`](../example/http_trace_context.h) works as
   written; a plain `std::map<std::string, std::string>` does **not** — normalise
   the key inside your `Get()`.

---

## Getting Help

Enable `Log.Level: "debug"`, then open an issue at
[pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
with the agent version, your sanitized configuration, the relevant log lines,
and minimal reproduction steps.
