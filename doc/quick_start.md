# Pinpoint C++ Agent - Quick Start Guide

This guide will help you get started with the Pinpoint C++ Agent for monitoring your C++ applications.

## Table of Contents
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Configuration](#configuration)
- [Basic Usage](#basic-usage)
- [Running Your First Traced Application](#running-your-first-traced-application)

## Prerequisites

Before you begin, ensure you have:

- **Pinpoint Collector**: Version 2.4.0 or higher
- **C++ Compiler**: Supporting C++17 or higher
- **Build System**: CMake 3.14+ or Bazel
- **Operating System**: Linux, macOS, or Windows

## Installation

### Using CMake

Add the pinpoint-cpp-agent to your CMakeLists.txt:

```cmake
# Add pinpoint-cpp-agent as a subdirectory or use FetchContent
include(FetchContent)
FetchContent_Declare(
  pinpoint_cpp
  GIT_REPOSITORY https://github.com/pinpoint-apm/pinpoint-cpp-agent.git
  GIT_TAG main
)
FetchContent_MakeAvailable(pinpoint_cpp)

# Link against your target
target_link_libraries(your_target PRIVATE pinpoint_cpp)
```

### Using Bazel

Add to your WORKSPACE file:

```python
http_archive(
    name = "pinpoint_cpp",
    urls = ["https://github.com/pinpoint-apm/pinpoint-cpp-agent/archive/main.zip"],
    strip_prefix = "pinpoint-cpp-agent-main",
)
```

## Configuration

### Option 1: Configuration File

Create a `pinpoint-config.yaml` file:

```yaml
ApplicationName: "MyApplication"
ApplicationNamespace: ""
AgentId: "my-agent-id"  # Optional: auto-generated if not specified

Collector:
  GrpcHost: "localhost"  # Your Pinpoint collector host
  GrpcPort: 9991         # Default gRPC port
  
Sampling:
  Rate: 100  # Sampling rate (0-100, where 100 means sample all requests)
  
Logging:
  Level: "info"  # Logging level: trace, debug, info, warn, error
```

Set the configuration file path in your application:

```cpp
#include "pinpoint/tracer.h"

int main() {
    pinpoint::SetConfigFilePath("/path/to/pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();
    
    // Your application code
    
    agent->Shutdown();
    return 0;
}
```

### Option 2: Environment Variables

You can also configure the agent using environment variables:

```bash
export PINPOINT_CPP_CONFIG_FILE="/path/to/pinpoint-config.yaml"
export PINPOINT_CPP_APPLICATION_NAME="MyApplication"
export PINPOINT_CPP_AGENT_ID="my-agent-id"
export PINPOINT_CPP_COLLECTOR_HOST="localhost"
export PINPOINT_CPP_COLLECTOR_PORT="9991"
```

### Option 3: Configuration String

Pass configuration directly as a YAML string:

```cpp
#include "pinpoint/tracer.h"

int main() {
    std::string config = R"(
        ApplicationName: "MyApplication"
        Collector:
          GrpcHost: "localhost"
    )";
    
    pinpoint::SetConfigString(config);
    auto agent = pinpoint::CreateAgent();
    
    // Your application code
    
    agent->Shutdown();
    return 0;
}
```

## Basic Usage

### 1. Initialize the Agent

At application startup, create and initialize the Pinpoint agent:

```cpp
#include "pinpoint/tracer.h"

int main() {
    // Set configuration
    pinpoint::SetConfigFilePath("pinpoint-config.yaml");
    
    // Create agent instance
    auto agent = pinpoint::CreateAgent();
    
    // Check if agent is enabled
    if (!agent->Enable()) {
        std::cerr << "Failed to enable Pinpoint agent" << std::endl;
        return 1;
    }
    
    // Your application code here
    
    // Shutdown agent before exit
    agent->Shutdown();
    return 0;
}
```

### 2. Create a Span

A **Span** represents a single operation or request in your application:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    
    // Create a new span
    auto span = agent->NewSpan("MyOperation", "/api/endpoint");
    
    // Set span properties
    span->SetRemoteAddress("192.168.1.100");
    span->SetEndPoint("localhost:8080");
    
    // Your business logic here
    
    // End the span
    span->EndSpan();
}
```

### 3. Create Span Events

**Span Events** represent sub-operations within a span:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyOperation", "/api/endpoint");
    
    // Create a span event for database operation
    auto dbEvent = span->NewSpanEvent("queryDatabase");
    dbEvent->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
    dbEvent->SetDestination("mysql-db");
    dbEvent->SetEndPoint("localhost:3306");
    
    // Execute database query
    // ...
    
    // End the span event
    span->EndSpanEvent();
    
    // End the span
    span->EndSpan();
}
```

### 4. Add Annotations

Annotations provide additional metadata about your operations:

```cpp
void handleRequest() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyOperation", "/api/endpoint");
    
    // Get annotation interface
    auto annotations = span->GetAnnotations();
    
    // Add various types of annotations
    annotations->AppendString(pinpoint::ANNOTATION_API, "getUserInfo");
    annotations->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
    
    // Your business logic here
    
    span->EndSpan();
}
```

## Running Your First Traced Application

Here's a complete minimal example:

```cpp
#include <iostream>
#include <thread>
#include <chrono>
#include "pinpoint/tracer.h"

void doWork() {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MyService", "/work");
    
    span->SetRemoteAddress("client-address");
    span->SetEndPoint("localhost:8080");
    
    // Simulate some work
    auto spanEvent = span->NewSpanEvent("processData");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    span->EndSpanEvent();
    
    // Add result annotation
    auto annotations = span->GetAnnotations();
    annotations->AppendString(pinpoint::ANNOTATION_API, "doWork");
    annotations->AppendInt(pinpoint::ANNOTATION_HTTP_STATUS_CODE, 200);
    
    span->EndSpan();
}

int main() {
    // Configure via environment or file
    setenv("PINPOINT_CPP_APPLICATION_NAME", "my-first-app", 0);
    setenv("PINPOINT_CPP_CONFIG_FILE", "/tmp/pinpoint-config.yaml", 0);
    
    // Create agent
    auto agent = pinpoint::CreateAgent();
    
    if (!agent->Enable()) {
        std::cerr << "Failed to enable agent" << std::endl;
        return 1;
    }
    
    std::cout << "Pinpoint agent started" << std::endl;
    
    // Simulate multiple requests
    for (int i = 0; i < 5; i++) {
        std::cout << "Processing request " << (i + 1) << std::endl;
        doWork();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    std::cout << "Shutting down agent..." << std::endl;
    agent->Shutdown();
    
    return 0;
}
```

### Build and Run

```bash
# Compile your application
g++ -std=c++17 -o my_app my_app.cpp -lpinpoint_cpp

# Run your application
./my_app
```

## Next Steps

Now that you have a basic understanding of the Pinpoint C++ Agent, you can:

1. **Learn Advanced Instrumentation**: Read the [Instrumentation Guide](instrument.md) for detailed information on:
   - HTTP request/response tracing
   - Database query tracing
   - Distributed tracing with context propagation
   - Error handling and exception tracking
   - Asynchronous operation tracing

2. **Explore Examples**: Check the `example/` directory for complete examples:
   - `http_server.cpp` - HTTP server instrumentation
   - `web_demo.cpp` - Web application with outgoing HTTP calls
   - `db_demo.cpp` - Database query instrumentation

3. **Configure Advanced Options**: Learn about:
   - Sampling strategies
   - Custom service types
   - URL statistics collection
   - SQL parameter binding

4. **Monitor Your Application**: Access the Pinpoint Web UI to:
   - View service maps
   - Analyze transaction traces
   - Monitor performance metrics
   - Identify bottlenecks

## Troubleshooting

### Agent Not Starting

If the agent fails to start:

1. Check that the collector host and port are correct
2. Verify network connectivity to the Pinpoint collector
3. Check application logs for error messages
4. Ensure your application name is set correctly

### No Data in Pinpoint UI

If you don't see data in Pinpoint:

1. Verify the agent is enabled: `agent->Enable()` returns `true`
2. Check sampling rate in configuration (should be > 0 for testing)
3. Ensure spans are properly ended with `EndSpan()`
4. Wait a few seconds for data to appear (there's a collection interval)
5. Check Pinpoint collector logs for errors

### Performance Impact

To minimize performance impact:

1. Use appropriate sampling rates (not 100% in production)
2. Avoid excessive annotations
3. Only trace critical paths
4. Monitor agent overhead

## Support

- **GitHub Issues**: [pinpoint-apm/pinpoint-cpp-agent](https://github.com/pinpoint-apm/pinpoint-cpp-agent/issues)
- **Pinpoint Documentation**: [Pinpoint APM](https://pinpoint-apm.github.io/pinpoint/)
- **Community**: Join the Pinpoint community for discussions

## License

Pinpoint C++ Agent is licensed under the Apache License 2.0. See [LICENSE](../LICENSE) for details.

