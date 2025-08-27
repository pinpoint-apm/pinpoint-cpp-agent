# Pinpoint C++ Agent

The C++ SDK Library for [Pinpoint](https://github.com/pinpoint-apm/pinpoint).

Pinpoint C++ Agent enables you to monitor C++ applications using Pinpoint.
Developers can instrument C++ applications using the APIs provided in this library.

## Requirements
* Pinpoint 2.4.0+
* C++17 compiler
* bazel or cmake + vcpkg

## Getting Started
### pinpoint-config.yaml
```yaml
ApplicationName: "MyAppName"
Collector:
  GrpcHost: "my.collector.host"
```

### example.cpp
```cpp
#include "pinpoint/tracer.h"

int main() {
    // create agent
    pinpoint::SetConfigFilePath("pinpoint-config.yaml");
    auto agent = pinpoint::CreateAgent();

    // create span
    auto span = agent->NewSpan("C++ Server", "/foo");
    span->SetRemoteAddress("remote_addr");
    span->SetEndPoint("end_point");

    // create span event
    auto se = span->NewSpanEvent("foo");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    span->EndSpanEvent();

    // finish span and send span to server
    span->EndSpan();

    std::this_thread::sleep_for(std::chrono::seconds(600));
    agent->Shutdown();
    return 0;
}
```
## Contributing

We are looking forward to your contributions via pull requests.
For tips on contributing code fixes or enhancements, please see the [contributing guide](CONTRIBUTING.md).
To report bugs, please create an Issue on the GitHub repository. 

## License

Pinpoint is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for full license text.
