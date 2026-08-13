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

To disable tracing without removing the agent from your code, set `Enable` to
`false` — in the config file, or as `PINPOINT_CPP_ENABLE=false` in the
environment:

```yaml
Enable: false
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
pinpoint::StartAgent(options);
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

The agent logs configuration, gRPC, and span collection activity. By default it
writes to stdout/stderr; file logging rotates automatically. Levels are `trace`,
`debug`, `info` (default), `warn`, `error` — see the
[Configuration Guide](config.md) for the keys and their environment variables.

```yaml
Log:
  Level: "debug"
  FilePath: "/var/log/pinpoint/agent.log"
  MaxFileSize: 10  # MB — rotates when this size is reached

EnableCallstackTrace: true   # stack traces on recorded errors
```

---

## Common Issues

### Agent Not Starting

**Symptoms:** the application runs but no data appears in the Pinpoint UI.

**Diagnosis:** read the agent log — see [Verifying Agent Startup](#verifying-agent-startup)
for what each entry means and why `Enable()` right after `StartAgent()` diagnoses
nothing. An **empty** agent log next to a `false` return points at `Enable: false`
rather than a broken configuration; check that first.

**Solutions:**

1. **Check the required fields** — `ApplicationName` and `Collector.Host` must be set.
2. **Verify collector connectivity** — see [Cannot Connect to Collector](#cannot-connect-to-collector).
3. **Check permissions** — the log directory must be writable.
4. **Check the *resolved* configuration** — with `Log.Level: "debug"` the agent
   logs the configuration it actually resolved, after the file, environment
   variables and defaults have been merged. This is the fastest way to catch a
   setting that never took effect: a typo'd YAML key, a stale `PINPOINT_CPP_*`
   variable overriding the file, or a value clamped into range.

### No Data in the Pinpoint UI

The agent starts successfully but no traces appear.

1. **Check sampling** — set `Sampling.Type: COUNTER` and
   `Sampling.CounterRate: 1` together to sample every transaction.
2. **Verify spans are ended** — `EndSpan()` must run on every code path; a span
   released without it is never sent.
3. **Check excluded URLs and methods** — a filter match produces a noop span, so
   the transaction disappears entirely. Temporarily clear both:

   ```yaml
   Http:
     Server:
       ExcludeUrl: []
       ExcludeMethod: []
   ```

   `ExcludeMethod` only applies when the span is created with the
   `NewSpan(operation, rpc_point, method, reader)` overload.
4. **Check `ApplicationName`** is set.
5. **Wait for collection** — data may take 5–10 seconds to appear.
6. **Check the collector side** — the agent log can show a clean `AgentInfo sent`
   and spans still be rejected downstream. Review the Pinpoint **collector** logs
   before assuming the agent is at fault.

### Incomplete Traces

1. **Raise the limits** — `Span.MaxEventDepth` / `Span.MaxEventSequence`
   (`-1` = unlimited).
2. **Check event ending** — every `NewSpanEvent()` needs a matching `EndEvent()`
   on the returned event.

### High Memory Usage

1. **Always end spans** — an unended span holds its buffered events and
   exceptions for as long as it lives. Use the RAII guard shown in
   [API Contracts §2](api_contracts.md#2-end-exactly-once-and-record-before-ending).
2. **Reduce buffer sizes and collection limits:**

   ```yaml
   Span:
     QueueSize: 512            # Reduce from default 1024
     MaxEventDepth: 32         # Limit depth
     MaxEventSequence: 500     # Reduce from default 5000

   Http:
     UrlStatLimit: 512         # Reduce from default 1024
   ```

### High CPU Usage or Slow Responses

1. **Reduce sampling**, or cap it by throughput:

   ```yaml
   Sampling:
     Type: "PERCENT"
     PercentRate: 10.0         # Sample only 10%
     NewThroughput: 100        # Or: accept all, cap at 100 new tx/sec
     ContinueThroughput: 200
   ```

2. **Disable what you do not read** — `Http.CollectUrlStat`, `Sql.EnableSqlStats`,
   `Sql.TraceBindValue`, `Stat.Enable`.
3. **Reduce the work per transaction** — trace only the paths that matter, prefer
   fewer coarser span events, and cut the volume and size of annotations and
   recorded headers (`HEADERS-ALL` is a debugging setting, not a production one).
4. **Skip expensive collection on unsampled spans** — guard it with
   `span->IsSampled()`.

### Cannot Connect to Collector

Logs show connection or gRPC errors.

1. Verify `Collector.Host` and the three ports (`AgentPort` 9991, `StatPort`
   9992, `SpanPort` 9993) point at a running, healthy collector.
2. Test connectivity to each port, e.g. `telnet <host> 9991`, `nslookup <host>`.
3. Allow gRPC ports 9991–9993 through firewalls and network policies
   (e.g. Kubernetes).

### Missing Distributed Tracing

1. **Verify context propagation** — both inject and extract must be implemented:

   ```cpp
   // Server: Extract context
   HttpHeaderReader reader(req.headers);
   auto span = agent->NewSpan("Service", "/endpoint", reader);

   // Client: Inject context through the span event for the outbound call
   auto se = span->NewSpanEvent("outgoing-call");
   HttpHeaderReaderWriter writer(headers);
   se->InjectContext(writer);
   ```

2. **Check the headers** are present on the inbound request
   (`req.get_header_value("Pinpoint-TraceID")`), and that no gateway or proxy
   strips or rewrites them.
3. **Check your reader's header lookup is case-insensitive** — the agent asks
   your `TraceContextReader` for the canonical spellings (`Pinpoint-TraceID`,
   `Pinpoint-Sampled`, ...), but proxies and HTTP/2 clients routinely re-case
   them (`pinpoint-traceid`). A reader backed by a case-sensitive container
   silently finds nothing and every request looks like a new transaction.
   `httplib::Headers` is case-insensitive by default, which is why
   [`example/http_trace_context.h`](../example/http_trace_context.h) works as
   written; a plain `std::map<std::string, std::string>` does **not** — normalise
   the key inside your `Get()`.

---

## Getting Help

Enable `Log.Level: "debug"`, then open an issue at
[pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
with the agent version, your sanitized configuration, the relevant log lines,
and minimal reproduction steps.
