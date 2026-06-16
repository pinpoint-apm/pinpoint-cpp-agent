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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pinpoint {

    /**
     * @brief Agent self-identity version, mirroring Java's NameVersion
     *        (config key "pinpoint.modules.uid.version").
     *
     * v1 and v3 both produce an ObjectName with version 1 (protocol.version 100);
     * they differ only in the applicationName length limit (24 vs 254).
     * v4 produces an ObjectName with version 4 (protocol.version 400).
     */
    enum class NameVersion { kV1, kV3, kV4 };

    /**
     * @brief Parse the uid version string. Case-insensitive. Unknown/empty -> v3.
     */
    NameVersion parse_name_version(std::string_view value);

    namespace object_name {
        // Length limits, identical to Java PinpointConstants. Lengths are measured
        // in bytes; since the allowed charset is ASCII-only ([a-zA-Z0-9._-]), byte
        // length always equals character count for any valid id.
        constexpr size_t AGENT_ID_MAX_LEN = 24;
        constexpr size_t AGENT_NAME_MAX_LEN = 255;
        constexpr size_t SERVICE_NAME_MAX_LEN = 254;
        constexpr size_t APPLICATION_NAME_MAX_LEN_V1 = 24;
        constexpr size_t APPLICATION_NAME_MAX_LEN_V3 = SERVICE_NAME_MAX_LEN; // 254
        constexpr size_t AGENT_NAME_MAX_LEN_V4 = SERVICE_NAME_MAX_LEN;       // 254

        // ObjectName versions (Java ObjectName.VERSION_V1 / VERSION_V4).
        constexpr int VERSION_V1 = 1;
        constexpr int VERSION_V4 = 4;

        // protocol.version gRPC header wire values (Java ProtocolVersion: V1=1_00, V4=4_00).
        constexpr int PROTOCOL_VERSION_V1 = 100;
        constexpr int PROTOCOL_VERSION_V4 = 400;
    }

    /**
     * @brief 128-bit UUID stored as most/least significant bits (big-endian layout).
     */
    struct Uuid {
        uint64_t msb = 0;
        uint64_t lsb = 0;
    };

    /**
     * @brief Generate an RFC 9562 UUID version 7 (Unix epoch ms based, time-ordered).
     *        Compatible with Java's fasterxml TimeBasedEpochGenerator.
     */
    Uuid generate_uuid_v7();

    /**
     * @brief Encode a UUID into a 22-character URL-and-filename-safe Base64 string
     *        (RFC 4648 section 5, padding removed). Byte-compatible with Java's
     *        Base64Utils.encode(UUID): msb big-endian into bytes[0..7], lsb into
     *        bytes[8..15], then URL-safe base64 (absl::WebSafeBase64Escape).
     */
    std::string base64_encode_uuid(const Uuid& uuid);

    /**
     * @brief Validate an id, matching Java IdValidateUtils.validateId.
     *        Allowed characters: [a-zA-Z0-9], '.', '-', '_'. Empty fails.
     *        Length is the byte length and must be in (0, max_len].
     */
    bool validate_id(std::string_view value, size_t max_len);

    /**
     * @brief Raw identity inputs collected from config (env over yaml already merged).
     */
    struct ObjectNameInput {
        std::string agent_id;
        std::string agent_name;
        std::string application_name;
        std::string service_name;
        std::string api_key;
    };

    /**
     * @brief Resolved agent self-identity.
     *
     * version is 1 for v1/v3 and 4 for v4. service_name / api_key / agent_uuid are
     * only populated for v4. api_key is never exposed by to_string().
     */
    struct ObjectName {
        int version = object_name::VERSION_V1;
        std::string agent_id;
        std::string agent_name;
        std::string application_name;
        std::string service_name;
        std::string api_key;
        Uuid agent_uuid{};

        bool is_v4() const { return version == object_name::VERSION_V4; }
        int protocol_version() const {
            return is_v4() ? object_name::PROTOCOL_VERSION_V4 : object_name::PROTOCOL_VERSION_V1;
        }
        std::string to_string() const; // masks api_key
    };

    /**
     * @brief Resolve the agent identity for the given version.
     *
     * Returns std::nullopt when a required value is missing or invalid (the caller
     * must abort agent startup). Errors are logged. Successful resolution logs the
     * resolved (non-secret) values.
     */
    std::optional<ObjectName> resolve_object_name(NameVersion version, const ObjectNameInput& input);

} // namespace pinpoint
