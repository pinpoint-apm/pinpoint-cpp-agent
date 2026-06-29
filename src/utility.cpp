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

    static std::string resolve_host_ip_addr() {
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

    std::string get_host_ip_addr() {
        // getaddrinfo can block for seconds when the hostname is not
        // resolvable (e.g. no DNS); the address is stable for the process
        // lifetime, so resolve once and reuse.
        static const std::string cached_ip_addr = resolve_host_ip_addr();
        return cached_ip_addr;
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
        if (str1.size() != str2.size()) {
            return false;
        }

        return std::equal(str1.begin(), str1.end(), str2.begin(), char_iequal());
    }

    std::optional<int> stoi_(std::string_view str) {
        int result{};
        return absl::SimpleAtoi(str, &result) ? std::optional<int>(result) : std::nullopt;
    }

    std::optional<int64_t> stoll_(std::string_view str) {
        int64_t result{};
        return absl::SimpleAtoi(str, &result) ? std::optional<int64_t>(result) : std::nullopt;
    }

    std::optional<double> stod_(std::string_view str) {
        double result{};
        return absl::SimpleAtod(str, &result) ? std::optional<double>(result) : std::nullopt;
    }

    std::optional<bool> stob_(std::string_view str) {
        bool result{};
        return absl::SimpleAtob(str, &result) ? std::optional<bool>(result) : std::nullopt;
    }

    SqlUid generate_sql_uid(std::string_view sql) {
        // MurmurHash3_x64_128 produces 16 bytes (128 bits) of output
        static_assert(SqlUid{}.size() == kMurmurHashOutputSize,
                      "SqlUid size must match MurmurHash3_x64_128 output");
        SqlUid result{};
        MurmurHash3_x64_128(sql.data(), static_cast<int>(sql.length()), kMurmurHashSeed, result.data());
        return result;
    }

 }
