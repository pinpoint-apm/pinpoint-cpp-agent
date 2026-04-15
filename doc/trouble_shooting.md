# Pinpoint C++ Agent - Troubleshooting Guide

This guide helps you diagnose and resolve common issues with the Pinpoint C++ Agent.

---

## Table of Contents

- [Disabling the Agent](#disabling-the-agent)
- [Logging](#logging)
- [Common Issues](#common-issues)
- [Performance Issues](#performance-issues)
- [Connection Issues](#connection-issues)
- [Data Collection Issues](#data-collection-issues)
- [Getting Help](#getting-help)

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

    auto agent = pinpoint::CreateAgent();
    // Agent will be disabled — no tracing data is collected

    // Your application code

    return 0;
}
```

For more information, refer to the [Configuration Guide](config.md).

### Shutdown Function

You can stop the agent at runtime by calling `Agent::Shutdown()`. There is no need to restart your application. After `Shutdown()` is called, the agent stops collecting tracing data and sending it to the collector.

```cpp
#include "pinpoint/tracer.h"

int main() {
    auto agent = pinpoint::CreateAgent();

    // Your application code

    // Stop the agent when needed
    agent->Shutdown();

    return 0;
}
```

### Dynamic Control Example

You can start and stop the agent dynamically using HTTP endpoints. This is useful for production environments where you want to enable or disable tracing without restarting the application.

```cpp
#include "pinpoint/tracer.h"
#include "3rd_party/httplib.h"
#include <mutex>

std::mutex agent_mutex;
pinpoint::AgentPtr global_agent = nullptr;

void handle_new_agent(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(agent_mutex);

    try {
        if (global_agent) {
            res.status = 400;
            res.set_content("{\"error\": \"Agent already running\"}", "application/json");
            return;
        }

        pinpoint::SetConfigFilePath("/path/to/pinpoint-config.yaml");
        global_agent = pinpoint::CreateAgent();

        if (global_agent && global_agent->Enable()) {
            res.status = 200;
            res.set_content("{\"message\": \"New Pinpoint C++ Agent - success\"}",
                            "application/json");
        } else {
            res.status = 500;
            res.set_content("{\"error\": \"New Pinpoint C++ Agent - fail\"}",
                            "application/json");
            global_agent = nullptr;
        }
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}",
                        "application/json");
        global_agent = nullptr;
    }
}

void handle_shutdown(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(agent_mutex);

    if (global_agent) {
        global_agent->Shutdown();
        global_agent = nullptr;
        res.status = 200;
        res.set_content("{\"message\": \"Shutdown Pinpoint C++ Agent\"}",
                        "application/json");
    } else {
        res.status = 400;
        res.set_content("{\"error\": \"Agent not running\"}", "application/json");
    }
}

void handle_status(const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(agent_mutex);

    bool running = (global_agent != nullptr);
    res.status = 200;
    res.set_content("{\"running\": " + std::string(running ? "true" : "false") + "}",
                    "application/json");
}

int main() {
    httplib::Server server;

    server.Post("/agent/new", handle_new_agent);
    server.Post("/agent/shutdown", handle_shutdown);
    server.Get("/agent/status", handle_status);

    std::cout << "Control server running on port 8000" << std::endl;
    std::cout << "POST /agent/new      - Start agent" << std::endl;
    std::cout << "POST /agent/shutdown - Stop agent" << std::endl;
    std::cout << "GET  /agent/status   - Check agent status" << std::endl;

    server.listen("0.0.0.0", 8000);

    return 0;
}
```

Usage:

```bash
# Check agent status
curl http://localhost:8000/agent/status

# Start the agent
curl -X POST http://localhost:8000/agent/new

# Stop the agent
curl -X POST http://localhost:8000/agent/shutdown
```

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

# View recent logs
tail -n 100 /var/log/pinpoint/agent.log

# Search for errors
grep "ERROR" /var/log/pinpoint/agent.log
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

**Diagnosis:**

```cpp
auto agent = pinpoint::CreateAgent();
if (!agent->Enable()) {
    std::cerr << "Agent failed to start" << std::endl;
    // Check logs for details
}
```

**Solutions:**

1. **Check Configuration** — ensure required fields are set:

   ```yaml
   ApplicationName: "MyApp"  # Required
   Collector:
     GrpcHost: "localhost"   # Required
   ```

2. **Verify Collector Connection:**

   ```bash
   telnet pinpoint-collector 9991
   nslookup pinpoint-collector
   ```

3. **Check Permissions** — ensure the log directory is writable:

   ```bash
   ls -la /var/log/pinpoint/
   ```

4. **Review Logs:**

   ```bash
   grep -i error /var/log/pinpoint/agent.log
   ```

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

2. **Verify Spans Are Ended:**

   ```cpp
   void handleRequest() {
       auto agent = pinpoint::GlobalAgent();
       auto span = agent->NewSpan("Service", "/endpoint");

       // Process request

       span->EndSpan();  // Make sure this is called!
   }
   ```

3. **Check Application Name** — `ApplicationName` must be set in configuration.

4. **Wait for Collection** — data may take 5–10 seconds to appear depending on the stat collection interval.

### Issue 3: Memory Leaks

**Symptoms:**
- Application memory usage grows over time.

**Solutions:**

1. **Always End Spans** — use the RAII pattern to guarantee cleanup:

   ```cpp
   class SpanGuard {
   public:
       explicit SpanGuard(pinpoint::SpanPtr span) : span_(std::move(span)) {}
       ~SpanGuard() { if (span_) span_->EndSpan(); }
   private:
       pinpoint::SpanPtr span_;
   };
   ```

2. **Reduce Queue Sizes:**

   ```yaml
   Span:
     QueueSize: 1024  # Reduce if necessary
   ```

3. **Limit Event Collection:**

   ```yaml
   Span:
     MaxEventDepth: 32      # Limit depth
     MaxEventSequence: 1000 # Limit events per span
   ```

### Issue 4: High CPU Usage

**Symptoms:**
- Application CPU usage is higher than expected.

**Solutions:**

1. **Reduce Sampling:**

   ```yaml
   Sampling:
     Type: "PERCENT"
     PercentRate: 10.0  # Sample only 10%
   ```

2. **Disable Unnecessary Features:**

   ```yaml
   Http:
     CollectUrlStat: false

   Sql:
     EnableSqlStats: false

   Stat:
     Enable: false
   ```

3. **Optimize Span Collection:**

   ```yaml
   Span:
     MaxEventDepth: 16
     EventChunkSize: 50
   ```

---

## Performance Issues

### High Memory Usage

Reduce buffer sizes and collection limits:

```yaml
Span:
  QueueSize: 512            # Reduce from default 1024
  MaxEventSequence: 500     # Reduce from default 5000

Http:
  UrlStatLimit: 512         # Reduce from default 1024
```

### Slow Application Response

Use throughput-based sampling to limit overhead:

```yaml
Sampling:
  Type: "THROUGHPUT"
  NewThroughput: 100        # Limit new transaction sampling
  ContinueThroughput: 200
```

Avoid expensive operations on unsampled spans:

```cpp
auto span = agent->NewSpan("Service", "/endpoint");

// Check if sampled before expensive operations
if (span->IsSampled()) {
    collectDetailedMetrics();
}

span->EndSpan();
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
     GrpcHost: "pinpoint-collector.example.com"
     GrpcAgentPort: 9991
     GrpcSpanPort: 9993
     GrpcStatPort: 9992
   ```

2. **Test Network Connectivity:**

   ```bash
   # Test connection to collector
   telnet pinpoint-collector.example.com 9991

   # Check firewall rules
   sudo iptables -L

   # Test DNS resolution
   nslookup pinpoint-collector.example.com
   ```

3. **Check Collector Status:**

   ```bash
   curl http://pinpoint-collector:8080/health
   ```

**Solutions:**

1. Update collector address to the correct hostname/IP.
2. Configure firewall rules to allow traffic on gRPC ports (9991–9993).
3. Check network policies (e.g., in Kubernetes).
4. Verify the collector is running and healthy.

### gRPC Connection Timeout

Run network diagnostics:

```bash
# Check network latency
ping pinpoint-collector

# Trace route
traceroute pinpoint-collector

# Check MTU issues
ping -M do -s 1472 pinpoint-collector
```

---

## Data Collection Issues

### Missing Spans

1. **Check Sampling Rate** — temporarily set to sample all:

   ```yaml
   Sampling:
     CounterRate: 1
   ```

2. **Verify Span Ending** — ensure `EndSpan()` is called on every code path.

3. **Check Excluded URLs** — temporarily remove exclusions:

   ```yaml
   Http:
     Server:
       ExcludeUrl: []
   ```

### Incomplete Traces

1. **Increase Limits:**

   ```yaml
   Span:
     MaxEventDepth: -1      # Unlimited
     MaxEventSequence: -1   # Unlimited
   ```

2. **Check Event Ending** — ensure every `NewSpanEvent()` has a matching `EndSpanEvent()`:

   ```cpp
   span->NewSpanEvent("operation");
   // ... do work ...
   span->EndSpanEvent();  // Must call this!
   ```

### Missing Distributed Tracing

1. **Verify Context Propagation** — both inject and extract must be implemented:

   ```cpp
   // Server: Extract context
   HttpTraceContextReader reader(req.headers);
   auto span = agent->NewSpan("Service", "/endpoint", reader);

   // Client: Inject context
   HttpTraceContextWriter writer(headers);
   span->InjectContext(writer);
   ```

2. **Check Headers** — verify Pinpoint headers are present in the request:

   ```cpp
   std::cout << "Trace ID: "
             << req.get_header_value("Pinpoint-TraceID") << std::endl;
   ```

3. **Check for Header Stripping** — gateways or proxies may strip or rewrite Pinpoint headers.

---

## Getting Help

### Before Asking for Help

1. **Enable Debug Logging:**

   ```yaml
   Log:
     Level: "debug"
   ```

2. **Collect Information:**
   - Agent version
   - Configuration (sanitized — remove secrets)
   - Error messages from logs
   - Steps to reproduce

3. **Check Documentation:**
   - [Quick Start Guide](quick_start.md)
   - [Configuration Guide](config.md)
   - [Instrumentation Guide](instrument.md)

### Reporting Issues

When reporting issues on [GitHub](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues), include:

1. **Environment** — OS and version, C++ compiler and version, Pinpoint collector version, agent version.

2. **Configuration** (sanitized):

   ```yaml
   ApplicationName: "MyApp"
   Log:
     Level: "debug"
   # ... other relevant settings
   ```

3. **Error Messages:**

   ```text
   [ERROR] Failed to connect to collector: ...
   ```

4. **Reproduction Steps** — how to reproduce the issue, expected behavior, actual behavior.

5. **Code Sample** — minimal code that reproduces the issue.

### Community Resources

- **GitHub Issues**: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
- **Pinpoint Documentation**: [pinpoint-apm.github.io](https://pinpoint-apm.github.io/pinpoint/)
- **Main Pinpoint Project**: [github.com/pinpoint-apm/pinpoint](https://github.com/pinpoint-apm/pinpoint)

### Useful Debugging Commands

```bash
# Check agent logs
tail -f /var/log/pinpoint/agent.log | grep -i error

# Monitor memory usage
watch -n 1 'ps aux | grep my_application'

# Check network connections to collector ports
netstat -an | grep 999[1-3]

# Verify collector connectivity
curl -v telnet://pinpoint-collector:9991

# Check application performance
top -p $(pidof my_application)

# Monitor file descriptors
lsof -p $(pidof my_application) | wc -l
```

---

## Related Documentation

- [Quick Start Guide](quick_start.md) — Getting started with Pinpoint C++ Agent
- [Configuration Guide](config.md) — Detailed configuration reference
- [Instrumentation Guide](instrument.md) — How to instrument your application
- Examples: see the `example/` directory in the repository

---

*Apache License 2.0 — See [LICENSE](../LICENSE) for details.*
