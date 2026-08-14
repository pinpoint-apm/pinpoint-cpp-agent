#pragma once

#include <cstdlib>
#include <string>

namespace it_test {

inline void SetDefaultEnv(const char* key, const char* value) {
    if (std::getenv(key) == nullptr) {
        ::setenv(key, value, 0);
    }
}

// The collector must be supplied by the CI/manual run environment.
// The agent id is always auto-generated per process; agent_name provides
// the stable, human-readable per-process label instead.
inline void ConfigureAgentEnvironment(const char* application,
                                      const char* agent_name) {
    SetDefaultEnv("PINPOINT_CPP_APPLICATION_NAME", application);
    SetDefaultEnv("PINPOINT_CPP_AGENT_NAME", agent_name);
    SetDefaultEnv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true");
    SetDefaultEnv("PINPOINT_CPP_SQL_ENABLE_SQL_STATS", "true");
    SetDefaultEnv("PINPOINT_CPP_SQL_TRACE_BIND_VALUE", "true");
    SetDefaultEnv("PINPOINT_CPP_ENABLE_CALLSTACK_TRACE", "true");
}

inline std::string EnvOr(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value == nullptr ? std::string(fallback) : std::string(value);
}

inline const char* JsonBool(bool value) { return value ? "true" : "false"; }

inline std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

inline std::string CollectorHost() {
    return EnvOr("PINPOINT_CPP_COLLECTOR_HOST", "");
}

}  // namespace it_test
