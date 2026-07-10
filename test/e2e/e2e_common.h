#pragma once

#include <cstdlib>
#include <string>

namespace it_test {

inline void set_default_env(const char* key, const char* value) {
    if (std::getenv(key) == nullptr) {
        ::setenv(key, value, 0);
    }
}

// The collector must be supplied by the CI/manual run environment.
inline void configure_agent_env(const char* application, const char* agent_id) {
    set_default_env("PINPOINT_CPP_APPLICATION_NAME", application);
    set_default_env("PINPOINT_CPP_AGENT_ID", agent_id);
    set_default_env("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true");
    set_default_env("PINPOINT_CPP_SQL_ENABLE_SQL_STATS", "true");
    set_default_env("PINPOINT_CPP_ENABLE_CALLSTACK_TRACE", "true");
}

inline std::string env_or(const char* key, const char* fallback) {
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
    return env_or("PINPOINT_CPP_COLLECTOR_HOST", "");
}

// Keep the spelling used by the standalone scenarios and the server apps
// compatible while the scripts pass per-process application/agent identities.
inline void ConfigureAgentEnvironment(const char* application,
                                      const char* agent_id) {
    configure_agent_env(application, agent_id);
}

}  // namespace it_test
