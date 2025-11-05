# Pinpoint C++ Agent - HTTP Filtering and Header Recording

This document explains the HTTP filtering and header recording features in the Pinpoint C++ Agent, based on the analysis of `http.h`, `http.cpp`, `agent.cpp`, and `config.cpp`.

## Table of Contents
- [Overview](#overview)
- [HTTP URL Filtering](#http-url-filtering)
- [HTTP Method Filtering](#http-method-filtering)
- [HTTP Header Recording](#http-header-recording)
- [Configuration](#configuration)
- [Implementation Details](#implementation-details)
- [Examples](#examples)

## Overview

The Pinpoint C++ Agent provides powerful HTTP filtering and header recording capabilities to:

- **Reduce Overhead**: Exclude monitoring of specific URLs or HTTP methods
- **Privacy Protection**: Avoid collecting sensitive endpoints
- **Selective Monitoring**: Focus on important endpoints
- **Debug Support**: Record request/response headers for troubleshooting

### Key Features

1. **URL Filtering**: Exclude URLs matching Ant-style patterns
2. **Method Filtering**: Exclude specific HTTP methods (GET, POST, etc.)
3. **Header Recording**: Selectively record HTTP headers
4. **Server/Client Support**: Different configurations for server and client sides

## HTTP URL Filtering

### Purpose

URL filtering allows you to exclude certain URL patterns from being traced. This is useful for:

- Health check endpoints (`/health`, `/ping`)
- Static resources (`/static/*`, `*.css`, `*.js`)
- Admin endpoints
- Frequently called but unimportant endpoints

### Pattern Matching

The agent uses **Ant-style path patterns** with support for wildcards:

- `*` - Matches any sequence of characters within a single path segment
- `**` - Matches any sequence of characters across multiple path segments

### Implementation

**Source**: `http.cpp:82-133`

```cpp
class HttpUrlFilter {
public:
    HttpUrlFilter(const std::vector<std::string>& cfg) : cfg_(cfg) {
        for (auto& iter : cfg_) {
            auto p = convert_to_regex(iter);
            pattern_.emplace_back(p);
        }
    }

    bool isFiltered(std::string_view url) const {
        for (auto& iter : pattern_) {
            if (std::regex_match(url.data(), iter)) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> cfg_;
    std::vector<std::regex> pattern_;
    
    static std::string convert_to_regex(std::string_view antPath);
};
```

### Conversion Algorithm

Ant-style patterns are converted to regular expressions:

**Ant Pattern → Regex**:
- `/api/*/users` → `^/api/[^/]*/users$`
- `/api/**/users` → `^/api/.*/users$`
- `/health` → `^/health$`
- `*.css` → `^.*\.css$`

**Key Logic** (`http.cpp:98-126`):
```cpp
std::string HttpUrlFilter::convert_to_regex(std::string_view antPath) {
    std::stringstream buf;
    buf << '^';  // Start anchor
    
    bool after_start = false;
    for (char c : antPath) {
        if (after_start) {
            if (c == '*') {
                buf << ".*";  // ** = match any characters including /
            } else {
                buf << "[^/]*";  // * = match any characters except /
                write_char(buf, c);
            }
            after_start = false;
        } else {
            if (c == '*') {
                after_start = true;
            } else {
                write_char(buf, c);  // Escape special regex characters
            }
        }
    }
    if (after_start) {
        buf << "[^/]*";
    }
    
    buf << '$';  // End anchor
    return buf.str();
}
```

### Usage in NewSpan()

**Source**: `agent.cpp:212-214`

```cpp
SpanPtr AgentImpl::NewSpan(std::string_view operation, std::string_view rpc_point,
                           std::string_view method, TraceContextReader& reader) {
    // ...
    if (http_url_filter_ && http_url_filter_->isFiltered(rpc_point)) {
        return noopSpan();  // URL is excluded
    }
    // ...
}
```

When a URL matches an exclusion pattern, a `noopSpan()` is returned, which performs no tracing operations.

## HTTP Method Filtering

### Purpose

Method filtering allows you to exclude specific HTTP methods from being traced:

- `OPTIONS` - CORS preflight requests
- `HEAD` - Metadata requests
- `TRACE` - Debugging method
- Any other HTTP method

### Implementation

**Source**: `http.cpp:135-144`

```cpp
class HttpMethodFilter {
public:
    HttpMethodFilter(const std::vector<std::string>& cfg) : cfg_(cfg) {}

    bool isFiltered(std::string_view method) const {
        for (auto& iter : cfg_) {
            if (compare_string(method, iter)) {  // Case-insensitive comparison
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> cfg_;
};
```

### Usage in NewSpan()

**Source**: `agent.cpp:215-217`

```cpp
if (!method.empty() && http_method_filter_ && http_method_filter_->isFiltered(method)) {
    return noopSpan();  // Method is excluded
}
```

### Characteristics

- **Case-insensitive**: `GET`, `get`, `Get` are all treated the same
- **Simple matching**: Exact string comparison (no pattern matching)
- **Early exit**: Returns immediately if method is excluded

## HTTP Header Recording

### Purpose

Header recording allows you to capture HTTP headers for analysis:

- **Request headers**: `User-Agent`, `Referer`, `Content-Type`, etc.
- **Response headers**: `Content-Type`, `Content-Length`, `Cache-Control`, etc.
- **Cookies**: Request/response cookies

### Implementation

**Source**: `http.cpp:57-80`

```cpp
class HttpHeaderRecorder {
public:
    HttpHeaderRecorder(int anno_key, std::vector<std::string>& cfg) 
        : anno_key_(anno_key), cfg_(cfg) {
        if (cfg_.size() == 1 && compare_string(cfg_[0], "HEADERS-ALL")) {
            dump_all_headers_ = true;
        }
    }

    void recordHeader(const HeaderReader& header, AnnotationPtr annotation) {
        if (cfg_.empty()) {
            return;
        }

        if (dump_all_headers_) {
            // Record all headers
            header.ForEach([this, annotation](std::string_view key, std::string_view val) {
                annotation->AppendStringString(anno_key_, key, val);
                return true;
            });
        } else {
            // Record specific headers
            for (auto iter = cfg_.begin(); iter != cfg_.end(); ++iter) {
                if (const auto v = header.Get(*iter); v.has_value()) {
                    annotation->AppendStringString(anno_key_, *iter, v.value());
                }
            }
        }
    }

private:
    int anno_key_;
    std::vector<std::string> cfg_;
    bool dump_all_headers_ = false;
};
```

### Recording Modes

**1. Specific Headers**
```yaml
Http:
  Server:
    RecordRequestHeader: ["User-Agent", "Referer", "Content-Type"]
```

Only specified headers are recorded.

**2. All Headers**
```yaml
Http:
  Server:
    RecordRequestHeader: ["HEADERS-ALL"]
```

All headers are recorded (use with caution).

### Server vs Client

The agent maintains separate header recorders for server and client:

**Server Side** (`agent.cpp:73-88`):
```cpp
// Server header recorders
http_srv_header_recorder_[HTTP_REQUEST] = nullptr;
http_srv_header_recorder_[HTTP_RESPONSE] = nullptr;
http_srv_header_recorder_[HTTP_COOKIE] = nullptr;

if (!config_.http.server.rec_request_header.empty()) {
    http_srv_header_recorder_[HTTP_REQUEST] =
        std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_REQUEST_HEADER, 
                                             config_.http.server.rec_request_header);
}
// ... similar for response and cookie
```

**Client Side** (`agent.cpp:91-106`):
```cpp
// Client header recorders
http_cli_header_recorder_[HTTP_REQUEST] = nullptr;
http_cli_header_recorder_[HTTP_RESPONSE] = nullptr;
http_cli_header_recorder_[HTTP_COOKIE] = nullptr;

if (!config_.http.client.rec_request_header.empty()) {
    http_cli_header_recorder_[HTTP_REQUEST] =
        std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_REQUEST_HEADER, 
                                             config_.http.client.rec_request_header);
}
// ... similar for response and cookie
```

### Recording Usage

**Source**: `agent.cpp:410-426`

```cpp
void AgentImpl::recordServerHeader(const HeaderType which, 
                                   HeaderReader& reader, 
                                   const AnnotationPtr& annotation) const {
    if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
        return;
    }
    if (enabled_ && http_srv_header_recorder_[which]) {
        http_srv_header_recorder_[which]->recordHeader(reader, annotation);
    }
}

void AgentImpl::recordClientHeader(const HeaderType which, 
                                   HeaderReader& reader, 
                                   const AnnotationPtr& annotation) const {
    if (which < HTTP_REQUEST || which > HTTP_COOKIE) {
        return;
    }
    if (enabled_ && http_cli_header_recorder_[which]) {
        http_cli_header_recorder_[which]->recordHeader(reader, annotation);
    }
}
```

## Configuration

### YAML Configuration

```yaml
Http:
  Server:
    # URL exclusion patterns (Ant-style)
    ExcludeUrl:
      - "/health"
      - "/ping"
      - "/actuator/**"
      - "/static/**"
      - "*.css"
      - "*.js"
      - "*.png"
    
    # HTTP method exclusion
    ExcludeMethod:
      - "OPTIONS"
      - "HEAD"
      - "TRACE"
    
    # Request header recording
    RecordRequestHeader:
      - "User-Agent"
      - "Referer"
      - "Content-Type"
    
    # Request cookie recording
    RecordRequestCookie:
      - "session"
      - "token"
    
    # Response header recording
    RecordResponseHeader:
      - "Content-Type"
      - "Content-Length"
  
  Client:
    # Client-side header recording
    RecordRequestHeader:
      - "User-Agent"
    
    RecordRequestCookie: []
    
    RecordResponseHeader:
      - "Content-Type"
```

### Environment Variables

```bash
# URL exclusion
export PINPOINT_CPP_HTTP_SERVER_EXCLUDE_URL="/health,/ping,/actuator/**"

# Method exclusion
export PINPOINT_CPP_HTTP_SERVER_EXCLUDE_METHOD="OPTIONS,HEAD"

# Server request headers
export PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_HEADER="User-Agent,Referer"

# Server request cookies
export PINPOINT_CPP_HTTP_SERVER_RECORD_REQUEST_COOKIE="session,token"

# Server response headers
export PINPOINT_CPP_HTTP_SERVER_RECORD_RESPONSE_HEADER="Content-Type"

# Client request headers
export PINPOINT_CPP_HTTP_CLIENT_RECORD_REQUEST_HEADER="User-Agent"

# Client request cookies
export PINPOINT_CPP_HTTP_CLIENT_RECORD_REQUEST_COOKIE=""

# Client response headers
export PINPOINT_CPP_HTTP_CLIENT_RECORD_RESPONSE_HEADER="Content-Type"
```

**Note**: Use comma-separated values for lists.

### Default Values

**Source**: `config.cpp:65-73`

```cpp
void init_config(Config& config) {
    // ...
    config.http.server.status_errors = {"5xx"};
    config.http.server.rec_request_header = {};  // Empty (no recording)
    config.http.server.rec_request_cookie = {};  // Empty
    config.http.server.rec_response_header = {}; // Empty
    config.http.client.rec_request_header = {};  // Empty
    config.http.client.rec_request_cookie = {};  // Empty
    config.http.client.rec_response_header = {}; // Empty
    // ...
}
```

By default, **no headers are recorded** and **no URLs/methods are excluded**.

## Implementation Details

### Initialization Order

**Source**: `agent.cpp:63-88`

1. **URL Filter**: Created if `exclude_url` is not empty
2. **Method Filter**: Created if `exclude_method` is not empty
3. **Status Error Checker**: Created if `status_errors` is not empty
4. **Header Recorders**: Created for each non-empty header configuration

```cpp
AgentImpl::AgentImpl(const Config& options) : config_(options) {
    // ...
    
    // Initialize filters
    if (!config_.http.server.exclude_url.empty()) {
        http_url_filter_ = std::make_unique<HttpUrlFilter>(config_.http.server.exclude_url);
    }
    if (!config_.http.server.exclude_method.empty()) {
        http_method_filter_ = std::make_unique<HttpMethodFilter>(config_.http.server.exclude_method);
    }
    if (!config_.http.server.status_errors.empty()) {
        http_status_errors_ = std::make_unique<HttpStatusErrors>(config_.http.server.status_errors);
    }
    
    // Initialize server header recorders
    if (!config_.http.server.rec_request_header.empty()) {
        http_srv_header_recorder_[HTTP_REQUEST] = 
            std::make_unique<HttpHeaderRecorder>(ANNOTATION_HTTP_REQUEST_HEADER, 
                                                 config_.http.server.rec_request_header);
    }
    // ... similar for other header types
}
```

### Performance Considerations

**URL Filtering**:
- Regex patterns are compiled once during initialization
- Matching is done using `std::regex_match` (O(n) where n is URL length)
- For multiple patterns, all are checked until a match is found

**Method Filtering**:
- Simple string comparison (O(m) where m is number of excluded methods)
- Case-insensitive comparison using `compare_string` helper

**Header Recording**:
- Only configured headers are recorded (selective)
- `HEADERS-ALL` mode iterates over all headers using `ForEach`
- Headers are appended to annotations as string-string pairs

### Special Characters in URL Patterns

**Source**: `http.cpp:128-133`

```cpp
void HttpUrlFilter::write_char(std::stringstream& buf, char c) {
    if (c == '.' || c == '+' || c == '^' || c == '[' || c == ']' || 
        c == '{' || c == '}') {
        buf << '\\';  // Escape regex special characters
    }
    buf << c;
}
```

These characters are automatically escaped when converting Ant patterns to regex:
- `.` → `\.`
- `+` → `\+`
- `^` → `\^`
- `[` → `\[`
- `]` → `\]`
- `{` → `\{`
- `}` → `\}`

## Examples

### Example 1: Exclude Health Checks

**Configuration**:
```yaml
Http:
  Server:
    ExcludeUrl:
      - "/health"
      - "/healthz"
      - "/ping"
```

**Result**:
- `GET /health` → No span created (noopSpan)
- `GET /api/users` → Span created normally

### Example 2: Exclude Static Resources

**Configuration**:
```yaml
Http:
  Server:
    ExcludeUrl:
      - "/static/**"
      - "*.css"
      - "*.js"
      - "*.png"
      - "*.jpg"
```

**Result**:
- `GET /static/css/style.css` → Excluded by `/static/**`
- `GET /assets/app.js` → Excluded by `*.js`
- `GET /images/logo.png` → Excluded by `*.png`
- `GET /api/data` → Traced normally

### Example 3: Exclude Specific Methods

**Configuration**:
```yaml
Http:
  Server:
    ExcludeMethod:
      - "OPTIONS"
      - "HEAD"
```

**Result**:
- `OPTIONS /api/users` → No span created
- `HEAD /api/users` → No span created
- `GET /api/users` → Span created
- `POST /api/users` → Span created

### Example 4: Record Specific Headers

**Configuration**:
```yaml
Http:
  Server:
    RecordRequestHeader:
      - "User-Agent"
      - "Referer"
      - "X-Request-ID"
    RecordResponseHeader:
      - "Content-Type"
      - "X-Response-Time"
```

**Application Code**:
```cpp
auto agent = pinpoint::GlobalAgent();
auto span = agent->NewSpan("MyService", "/api/users", "GET", trace_reader);

// Record request headers
HttpHeaderReader request_headers(req);
agent->recordServerHeader(pinpoint::HTTP_REQUEST, request_headers, span);

// ... process request ...

// Record response headers
HttpHeaderReader response_headers(res);
agent->recordServerHeader(pinpoint::HTTP_RESPONSE, response_headers, span);

span->EndSpan();
```

**Result in Pinpoint UI**:
- Request headers: `User-Agent`, `Referer`, `X-Request-ID` visible
- Response headers: `Content-Type`, `X-Response-Time` visible

### Example 5: Record All Headers (Debug Mode)

**Configuration**:
```yaml
Http:
  Server:
    RecordRequestHeader: ["HEADERS-ALL"]
    RecordResponseHeader: ["HEADERS-ALL"]
```

**⚠️ Warning**: This will record **all** HTTP headers, including potentially sensitive ones. Use only for debugging and never in production.

### Example 6: Combined Filtering

**Configuration**:
```yaml
Http:
  Server:
    ExcludeUrl:
      - "/health"
      - "/static/**"
    ExcludeMethod:
      - "OPTIONS"
    RecordRequestHeader:
      - "User-Agent"
```

**Behavior**:
```
Request: OPTIONS /api/users
→ Excluded by method filter

Request: GET /health
→ Excluded by URL filter

Request: GET /static/app.js
→ Excluded by URL filter

Request: GET /api/users
→ Traced with User-Agent header recorded
```

### Example 7: Ant Pattern Matching Examples

| Pattern | Matches | Doesn't Match |
|---------|---------|---------------|
| `/api/*` | `/api/users`<br>`/api/products` | `/api/users/123`<br>`/api` |
| `/api/**` | `/api/users`<br>`/api/users/123`<br>`/api/v1/users` | `/apis/users` |
| `/api/*/users` | `/api/v1/users`<br>`/api/v2/users` | `/api/users`<br>`/api/v1/v2/users` |
| `*.js` | `app.js`<br>`bundle.js` | `app.min.js` (wrong!)<br>Use `*.*.js` or `**.js` |
| `/admin/**` | `/admin/users`<br>`/admin/config/system` | `/administrator` |

## Best Practices

### URL Filtering

1. **Health Checks**: Always exclude health check endpoints
   ```yaml
   ExcludeUrl: ["/health", "/ping", "/healthz"]
   ```

2. **Static Resources**: Exclude static files to reduce overhead
   ```yaml
   ExcludeUrl: ["/static/**", "*.css", "*.js", "**.png", "**.jpg"]
   ```

3. **Sensitive Endpoints**: Exclude admin or internal endpoints
   ```yaml
   ExcludeUrl: ["/admin/**", "/internal/**"]
   ```

4. **Pattern Order**: More specific patterns first (though not required)
   ```yaml
   ExcludeUrl:
     - "/api/internal/**"  # Specific
     - "/api/**"            # General (this would never match if first is matched)
   ```

### Method Filtering

1. **CORS**: Exclude OPTIONS for CORS preflight
   ```yaml
   ExcludeMethod: ["OPTIONS"]
   ```

2. **Metadata**: Exclude HEAD if only checking existence
   ```yaml
   ExcludeMethod: ["HEAD", "OPTIONS"]
   ```

### Header Recording

1. **Minimal Recording**: Only record necessary headers
   ```yaml
   RecordRequestHeader: ["User-Agent"]
   ```

2. **Avoid Sensitive Data**: Never record:
   - `Authorization` headers
   - `Cookie` headers (use RecordRequestCookie selectively)
   - `X-API-Key` or similar secrets

3. **Debug Mode**: Use `HEADERS-ALL` only temporarily
   ```yaml
   # Development only!
   RecordRequestHeader: ["HEADERS-ALL"]
   ```

4. **Privacy Compliance**: Consider GDPR/privacy laws
   - Don't record IP addresses in headers
   - Don't record user identifiers without consent
   - Sanitize headers in production

5. **Performance**: Limit the number of recorded headers
   ```yaml
   # Good: 3-5 headers
   RecordRequestHeader: ["User-Agent", "Referer", "Content-Type"]
   
   # Bad: Too many
   RecordRequestHeader: ["Header1", "Header2", ... "Header20"]
   ```

## Troubleshooting

### No Spans Created

**Symptom**: No traces appear in Pinpoint UI

**Possible Causes**:
1. URL is excluded by filter
   - Check `ExcludeUrl` configuration
   - Test URL against patterns

2. Method is excluded by filter
   - Check `ExcludeMethod` configuration

3. Sampling rate is 0
   - Check sampling configuration

**Solution**:
```bash
# Enable debug logging
export PINPOINT_CPP_LOG_LEVEL=debug

# Check logs for "URL is filtered" or "Method is filtered" messages
```

### Headers Not Recorded

**Symptom**: Expected headers missing in Pinpoint UI

**Possible Causes**:
1. Header names misspelled in configuration
   - Header names are case-sensitive in some implementations
   
2. Headers not present in request/response
   - Verify headers exist using browser dev tools or curl

3. Empty configuration
   - Default is empty (no headers recorded)

**Solution**:
```yaml
# Temporarily use HEADERS-ALL to see all available headers
Http:
  Server:
    RecordRequestHeader: ["HEADERS-ALL"]
```

Then check which headers are actually available and configure accordingly.

### Pattern Not Matching

**Symptom**: URL should be excluded but isn't

**Possible Causes**:
1. Incorrect Ant pattern syntax
   - `*` vs `**` confusion
   - Missing slashes

2. Query parameters included in URL
   - Pattern: `/api/*` might not match `/api/users?id=1`
   - Solution: Use `/api/**` or normalize URLs before matching

**Debug**:
```cpp
// Test pattern matching
HttpUrlFilter filter({"/api/**"});
bool is_filtered = filter.isFiltered("/api/users");  // Should be true
```

## Related Documentation

- [Configuration Guide](config.md#http-configuration) - Complete HTTP configuration options
- [Instrumentation Guide](instrument.md#http-request-tracing) - HTTP request tracing patterns
- [Examples](examples.md#http-server-examples) - Complete HTTP server examples

## References

**Source Files**:
- `src/http.h` - HTTP filter and recorder interfaces
- `src/http.cpp` - Implementation of filters and recorders
- `src/agent.cpp:63-106` - Agent initialization with HTTP components
- `src/agent.cpp:207-241` - NewSpan with URL/method filtering
- `src/agent.cpp:410-426` - Header recording methods
- `src/config.cpp` - Configuration loading

## License

Apache License 2.0 - See [LICENSE](../LICENSE) for details.

