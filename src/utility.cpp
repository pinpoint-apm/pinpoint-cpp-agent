/*
 * Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <array>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <random>
#include <cctype>
#include <sys/socket.h>

#include "absl/strings/numbers.h"
#include "utility.h"
#include "MurmurHash3.h"

namespace pinpoint {

    // Constants
    namespace {
        constexpr size_t kHostNameMaxLength = 256;  // RFC 1035
        constexpr size_t kMurmurHashOutputSize = 16;  // 128 bits
        constexpr uint32_t kMurmurHashSeed = 0;
    }

    int64_t generate_span_id() {
        static thread_local std::mt19937_64 rand_source{std::random_device()()};
        return static_cast<int64_t>(rand_source());
    }

    int64_t to_milli_seconds(const std::chrono::system_clock::time_point &tm) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch()).count();
    }

    std::string get_host_name() {
        std::array<char, kHostNameMaxLength> host_name{};

        if (gethostname(host_name.data(), host_name.size()) != 0) {
            return "unknown";
        }

        // Ensure null-termination
        host_name[kHostNameMaxLength - 1] = '\0';
        return {host_name.data()};
    }

    std::string get_host_ip_addr() {
        std::array<char, kHostNameMaxLength> host_name{};

        if (gethostname(host_name.data(), host_name.size()) != 0) {
            return "0.0.0.0";
        }

        host_name[kHostNameMaxLength - 1] = '\0';

        // Use getaddrinfo instead of deprecated gethostbyname
        struct addrinfo hints{};
        struct addrinfo* result = nullptr;

        hints.ai_family = AF_INET;  // IPv4
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host_name.data(), nullptr, &hints, &result) != 0 || !result) {
            return "0.0.0.0";
        }

        // Thread-safe conversion using inet_ntop instead of inet_ntoa
        std::array<char, INET_ADDRSTRLEN> ip_str{};
        auto* sockaddr_ipv4 = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        
        if (inet_ntop(AF_INET, &(sockaddr_ipv4->sin_addr), ip_str.data(), ip_str.size()) == nullptr) {
            freeaddrinfo(result);
            return "0.0.0.0";
        }

        std::string ip_address{ip_str.data()};
        freeaddrinfo(result);
        return ip_address;
    }

    namespace {
        // Helper for case-insensitive character comparison
        struct char_iequal {
            bool operator()(unsigned char c1, unsigned char c2) const {
                // Use unsigned char to avoid UB with std::toupper
                return std::toupper(c1) == std::toupper(c2);
            }
        };
    }

    bool compare_string(std::string_view str1, std::string_view str2) {
        // Check length first for early return and safety
        if (str1.size() != str2.size()) {
            return false;
        }

        // Safe comparison with length check
        return std::equal(str1.begin(), str1.end(), str2.begin(), char_iequal());
    }

    namespace {
        // Template helper for safe string conversions
        template<typename T, typename ConversionFunc>
        std::optional<T> safe_string_convert(std::string_view str, ConversionFunc&& func) {
            // absl::SimpleAtoi family expects null-terminated strings
            // string_view doesn't guarantee this, so we create a temporary string
            // if the view doesn't point to a null-terminated buffer
            
            // Check if string_view points to null-terminated data
            const bool is_null_terminated = (str.data()[str.size()] == '\0');
            
            T result{};
            bool success = false;
            
            if (is_null_terminated) {
                success = func(str.data(), &result);
            } else {
                // Create temporary null-terminated string
                std::string temp(str);
                success = func(temp.c_str(), &result);
            }
            
            return success ? std::optional<T>(result) : std::nullopt;
        }
    }

    std::optional<int> stoi_(std::string_view str) {
        return safe_string_convert<int>(str, [](const char* s, int* out) {
            return absl::SimpleAtoi(s, out);
        });
    }

    std::optional<int64_t> stoll_(std::string_view str) {
        return safe_string_convert<int64_t>(str, [](const char* s, int64_t* out) {
            return absl::SimpleAtoi(s, out);
        });
    }

    std::optional<double> stod_(std::string_view str) {
        return safe_string_convert<double>(str, [](const char* s, double* out) {
            return absl::SimpleAtod(s, out);
        });
    }

    std::optional<bool> stob_(std::string_view str) {
        return safe_string_convert<bool>(str, [](const char* s, bool* out) {
            return absl::SimpleAtob(s, out);
        });
    }

    std::vector<unsigned char> generate_sql_uid(std::string_view sql) {
        // MurmurHash3_x64_128 produces 16 bytes (128 bits) of output
        std::vector<unsigned char> result(kMurmurHashOutputSize);
        MurmurHash3_x64_128(sql.data(), static_cast<int>(sql.length()), kMurmurHashSeed, result.data());
        return result;
    }

 }
