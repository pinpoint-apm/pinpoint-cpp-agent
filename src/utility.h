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

    /**
     * @brief Generates a unique span identifier.
     *
     * @return 64-bit span ID.
     */
    int64_t generate_span_id();
    /**
     * @brief Converts a system clock time point to epoch milliseconds.
     *
     * @param tm Time point to convert.
     * @return Milliseconds since epoch.
     */
    int64_t to_milli_seconds(const std::chrono::system_clock::time_point& tm);

    /**
     * @brief Produces a deterministic UID for a normalized SQL string.
     *
     * @param sql Normalized SQL string.
     * @return Byte vector uniquely identifying the SQL.
     */
    std::vector<unsigned char> generate_sql_uid(std::string_view sql);

    /// @brief Returns the host name of the running process.
    std::string get_host_name();
    /// @brief Returns the host IP address of the running process.
    std::string get_host_ip_addr();

    /// @brief Safe string-to-int conversion returning `std::nullopt` on error.
    std::optional<int> stoi_(std::string_view str);
    /// @brief Safe string-to-int64 conversion returning `std::nullopt` on error.
    std::optional<int64_t> stoll_(std::string_view str);
    /// @brief Safe string-to-double conversion returning `std::nullopt` on error.
    std::optional<double> stod_(std::string_view str);
    /// @brief Safe string-to-bool conversion returning `std::nullopt` on error.
    std::optional<bool> stob_(std::string_view str);

    /**
     * @brief Case-insensitive string comparison helper that avoids allocation.
     *
     * @param str1 First string.
     * @param str2 Second string.
     * @return `true` if both strings are equal (ignoring case).
     */
    bool compare_string(std::string_view str1, std::string_view str2);

}
