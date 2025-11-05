# Pinpoint C++ Agent - Troubleshooting Guide

This guide helps you diagnose and resolve common issues with the Pinpoint C++ Agent.

## Table of Contents
- [Disabling the Agent](#disabling-the-agent)
- [Logging](#logging)
- [Common Issues](#common-issues)
- [Performance Issues](#performance-issues)
- [Connection Issues](#connection-issues)
- [Data Collection Issues](#data-collection-issues)
- [Getting Help](#getting-help)

## Disabling the Agent

If the agent causes disruptions or problems to a production application, you can disable the agent.

### Config Option

You can disable the agent by setting the config option **'Enable'** to false.

**Option 1: YAML Configuration File**

```yaml
Enable: false
```

**Option 2: Environment Variable**

```bash
export PINPOINT_CPP_ENABLE="false"
```

Or in your application:

```cpp
#include <cstdlib>

int main() {
    setenv("PINPOINT_CPP_ENABLE", "false", 1);
    
    auto agent = pinpoint::CreateAgent();
    // Agent will be disabled
    
    // Your application code
    
    return 0;
}
```

For more information, refer to the [Configuration Guide](config.md#enable).

### Shutdown Function

You can stop the agent by calling the `Agent::Shutdown()` function, and there's no need to restart your application.

After `Agent::Shutdown()` is called, the agent will stop collecting tracing data and sending it to the collector.

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

You can write code to start and stop the agent dynamically using HTTP endpoints:

```cpp
#include "pinpoint/tracer.h"
#include "3rd_party/httplib.h"
#include <mutex>

// Global agent instance
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
    
    // Your other handlers here
    
    std::cout << "Control server running on port 8000" << std::endl;
    std::cout << "POST /agent/new - Start agent" << std::endl;
    std::cout << "POST /agent/shutdown - Stop agent" << std::endl;
    std::cout << "GET /agent/status - Check agent status" << std::endl;
    
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

## Logging

Pinpoint C++ Agent outputs logs related to agent operation (configuration, gRPC, span collection, etc.).

### Log Output

By default, logs are written to **stdout/stderr**. You can configure file-based logging:

```yaml
Log:
  Level: "info"
  FilePath: "/var/log/pinpoint/agent.log"
  MaxFileSize: 10  # MB
```

### Log Levels

You can use the config option **LogLevel** to increase the granularity of the agent's logging.

**Available Log Levels** (from most to least verbose):
- `trace` - Very detailed debugging information
- `debug` - Debugging information
- `info` - Informational messages (default)
- `warn` - Warning messages
- `error` - Error messages only

**Setting Log Level:**

```yaml
Log:
  Level: "debug"
```

Or via environment variable:

```bash
export PINPOINT_CPP_LOG_LEVEL="debug"
```

For more information, refer to the [Configuration Guide](config.md#logging-configuration).

### Viewing Logs

**Console Output:**
```bash
# Run your application and view logs
./my_application 2>&1 | tee app.log
```

**File Output:**
```bash
# Tail the log file
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

## Common Issues

### Issue 1: Agent Not Starting

**Symptoms:**
- Application runs but no data appears in Pinpoint UI
- Agent initialization fails

**Diagnosis:**

```cpp
auto agent = pinpoint::CreateAgent();
if (!agent->Enable()) {
    std::cerr << "Agent failed to start" << std::endl;
    // Check logs for details
}
```

**Solutions:**

1. **Check Configuration:**
   ```yaml
   ApplicationName: "MyApp"  # Required
   Collector:
     GrpcHost: "localhost"   # Required
   ```

2. **Verify Collector Connection:**
   ```bash
   # Test connectivity
   telnet pinpoint-collector 9991
   
   # Check DNS resolution
   nslookup pinpoint-collector
   ```

3. **Check Permissions:**
   ```bash
   # Ensure log directory is writable
   ls -la /var/log/pinpoint/
   ```

4. **Review Logs:**
   ```bash
   # Look for error messages
   grep -i error /var/log/pinpoint/agent.log
   ```

### Issue 2: No Data in Pinpoint UI

**Symptoms:**
- Agent starts successfully but no traces appear

**Solutions:**

1. **Check Sampling Configuration:**
   ```yaml
   Sampling:
     Type: "COUNTING"
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

3. **Check Application Name:**
   ```yaml
   ApplicationName: "MyApp"  # Must be set
   ```

4. **Wait for Collection:**
   - Data may take 5-10 seconds to appear
   - Check Stat collection interval

### Issue 3: Memory Leaks

**Symptoms:**
- Application memory usage grows over time

**Solutions:**

1. **Always End Spans:**
   ```cpp
   // Use RAII pattern
   class SpanGuard {
   public:
       SpanGuard(pinpoint::SpanPtr span) : span_(span) {}
       ~SpanGuard() { if (span_) span_->EndSpan(); }
   private:
       pinpoint::SpanPtr span_;
   };
   ```

2. **Check Queue Sizes:**
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
- Application CPU usage is higher than expected

**Solutions:**

1. **Reduce Sampling:**
   ```yaml
   Sampling:
     Type: "PERCENT"
     PercentRate: 10.0  # Sample only 10%
   ```

2. **Disable Features:**
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

## Performance Issues

### High Memory Usage

**Check Current Usage:**
```cpp
// Monitor span queue size
// Reduce QueueSize if memory is an issue
```

**Optimize Configuration:**
```yaml
Span:
  QueueSize: 512            # Reduce from default 1024
  MaxEventSequence: 500     # Reduce from default 5000

Http:
  UrlStatLimit: 512         # Reduce from default 1024
```

### Slow Application Response

**Check Sampling:**
```yaml
Sampling:
  Type: "THROUGHPUT"
  NewThroughput: 100        # Limit sampling rate
  ContinueThroughput: 200
```

**Avoid Expensive Operations:**
```cpp
auto span = agent->NewSpan("Service", "/endpoint");

// Check if sampled before expensive operations
if (span->IsSampled()) {
    // Only collect detailed data if sampled
    collectDetailedMetrics();
}

span->EndSpan();
```

## Connection Issues

### Cannot Connect to Collector

**Symptoms:**
- Logs show connection errors
- gRPC errors in logs

**Diagnosis:**

1. **Check Collector Address:**
   ```yaml
   Collector:
     GrpcHost: "pinpoint-collector.example.com"  # Correct address?
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
   # Verify collector is running
   curl http://pinpoint-collector:8080/health
   ```

**Solutions:**

1. Update collector address
2. Configure firewall rules
3. Check network policies (in Kubernetes)
4. Verify collector is running and healthy

### gRPC Connection Timeout

**Configuration:**
```yaml
Collector:
  GrpcHost: "pinpoint-collector"
  # Add timeout settings if available
```

**Network Diagnosis:**
```bash
# Check network latency
ping pinpoint-collector

# Trace route
traceroute pinpoint-collector

# Check MTU issues
ping -M do -s 1472 pinpoint-collector
```

## Data Collection Issues

### Missing Spans

**Check:**

1. **Sampling Rate:**
   ```yaml
   Sampling:
     CounterRate: 1  # Temporarily sample all
   ```

2. **Span Ending:**
   ```cpp
   // Always end spans
   span->EndSpan();
   ```

3. **Excluded URLs:**
   ```yaml
   Http:
     Server:
       ExcludeUrl: []  # Remove exclusions temporarily
   ```

### Incomplete Traces

**Solutions:**

1. **Increase Limits:**
   ```yaml
   Span:
     MaxEventDepth: -1      # Unlimited
     MaxEventSequence: -1   # Unlimited
   ```

2. **Check Event Ending:**
   ```cpp
   // Always end span events
   span->NewSpanEvent("operation");
   // ... do work ...
   span->EndSpanEvent();  // Must call this!
   ```

### Missing Distributed Tracing

**Verify Context Propagation:**

```cpp
// Server: Extract context
HttpTraceContextReader reader(req.headers);
auto span = agent->NewSpan("Service", "/endpoint", reader);

// Client: Inject context
HttpTraceContextWriter writer(headers);
span->InjectContext(writer);
```

**Check Headers:**
```cpp
// Verify Pinpoint headers are present
std::cout << "Trace ID: " 
          << req.get_header_value("Pinpoint-TraceID") << std::endl;
```

## Getting Help

### Before Asking for Help

1. **Enable Debug Logging:**
   ```yaml
   Log:
     Level: "debug"
   ```

2. **Collect Information:**
   - Agent version
   - Configuration (sanitized)
   - Error messages from logs
   - Steps to reproduce

3. **Check Documentation:**
   - [Quick Start Guide](quick_start.md)
   - [Configuration Guide](config.md)
   - [Instrumentation Guide](instrument.md)

### Reporting Issues

When reporting issues on [GitHub](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues), include:

1. **Environment:**
   - OS and version
   - C++ compiler and version
   - Pinpoint collector version
   - Agent version

2. **Configuration:**
   ```yaml
   # Your sanitized configuration
   ApplicationName: "MyApp"
   Log:
     Level: "debug"
   # ... other relevant settings
   ```

3. **Error Messages:**
   ```
   # Relevant log entries
   [ERROR] Failed to connect to collector: ...
   ```

4. **Reproduction Steps:**
   - How to reproduce the issue
   - Expected behavior
   - Actual behavior

5. **Code Sample:**
   ```cpp
   // Minimal code that reproduces the issue
   ```

### Community Resources

- **GitHub Issues**: [pinpoint-cpp-agent/issues](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
- **Pinpoint Documentation**: [pinpoint-apm.github.io](https://pinpoint-apm.github.io/pinpoint/)
- **Main Pinpoint Project**: [github.com/pinpoint-apm/pinpoint](https://github.com/pinpoint-apm/pinpoint)

### Useful Debugging Commands

```bash
# Check agent logs
tail -f /var/log/pinpoint/agent.log | grep -i error

# Monitor memory usage
watch -n 1 'ps aux | grep my_application'

# Check network connections
netstat -an | grep 999[1-3]

# Verify collector connectivity
curl -v telnet://pinpoint-collector:9991

# Check application performance
top -p $(pidof my_application)

# Monitor file descriptors
lsof -p $(pidof my_application) | wc -l
```

## Related Documentation

- [Quick Start Guide](quick_start.md) - Getting started with Pinpoint C++ Agent
- [Configuration Guide](config.md) - Detailed configuration reference
- [Instrumentation Guide](instrument.md) - How to instrument your application
- [Code Examples](examples.md) - Practical code examples

## License

Apache License 2.0 - See [LICENSE](../LICENSE) for details.

---

**Reference**: This guide is based on the [Pinpoint Go Agent Troubleshooting Guide](https://github.com/pinpoint-apm/pinpoint-go-agent/blob/main/doc/troubleshooting.md).

