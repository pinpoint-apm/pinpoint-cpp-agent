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

#pragma once

#include <chrono>
#include <string>
#include <optional>
#include <vector>
#include <string_view>

namespace pinpoint {

    int64_t generate_span_id();
    int64_t to_milli_seconds(const std::chrono::system_clock::time_point& tm);

    std::vector<unsigned char> generate_sql_uid(std::string_view sql);

    std::string get_host_name();
    std::string get_host_ip_addr();

    std::optional<int> stoi_(std::string_view str);
    std::optional<int64_t> stoll_(std::string_view str);
    std::optional<double> stod_(std::string_view str);
    std::optional<bool> stob_(std::string_view str);

    bool compare_string(std::string_view str1, std::string_view str2);

}
