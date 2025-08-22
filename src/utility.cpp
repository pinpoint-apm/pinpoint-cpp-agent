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
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <iostream>
#include <random>
#include <cctype>

#include "absl/strings/numbers.h"
#include "logging.h"
#include "utility.h"

namespace pinpoint {

    int64_t generate_span_id() {
        static thread_local std::mt19937_64 rand_source{std::random_device()()};
        return static_cast<int64_t>(rand_source());
    }

    int64_t to_milli_seconds(const std::chrono::system_clock::time_point &tm) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch()).count();
    }

    std::string get_host_name() {
        char host_name[80];

        if (gethostname(host_name, sizeof(host_name)) != 0) {
            return "unknown";
        }

        return {host_name};
    }

    std::string get_host_ip_addr() {
        char host_name[80];
        struct hostent *host_entry;

        if (gethostname(host_name, sizeof(host_name)) != 0) {
            return "0.0.0.0";
        }

        host_entry = gethostbyname(host_name);
        if (!host_entry) {
            return "0.0.0.0";
        }

        struct in_addr addr{};
        memcpy(&addr, host_entry->h_addr_list[0], sizeof(struct in_addr));
        return {inet_ntoa(addr)};
    }

    struct iequal
    {
        bool operator()(int c1, int c2) const
        {
            return std::toupper(c1) == std::toupper(c2);
        }
    };

    bool compare_string(std::string_view str1, std::string_view str2)
    {
        return std::equal(str1.begin(), str1.end(), str2.begin(), iequal());
    }

    int stoi_(std::string_view str) {
        int out = 0;
        if (absl::SimpleAtoi(str.data(), &out)) {
            return out;
        }
        return -1;
    }

    int64_t stoll_(std::string_view str) {
        int64_t out = 0;
        if (absl::SimpleAtoi(str.data(), &out)) {
            return out;
        }
        return -1;
    }

    double stod_(std::string_view str) {
        double out = 0;
        if (absl::SimpleAtod(str.data(), &out)) {
            return out;
        }
        return -1;
    }

    bool stob_(std::string_view str) {
        bool out = false;
        if (absl::SimpleAtob(str.data(), &out)) {
            return out;
        }
        return false;
    }

 }
