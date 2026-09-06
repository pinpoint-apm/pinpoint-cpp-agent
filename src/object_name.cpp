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

#include "object_name.h"

#include <chrono>
#include <unistd.h>

#include "absl/random/random.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"

#include "logging.h"
#include "utility.h"

namespace pinpoint {

    NameVersion parse_name_version(std::string_view value) {
        if (absl::EqualsIgnoreCase(value, "v1")) {
            return NameVersion::kV1;
        }
        if (absl::EqualsIgnoreCase(value, "v4")) {
            return NameVersion::kV4;
        }
        // "v3", unknown, or empty all fall back to v3 (Java NameVersion.getVersion).
        return NameVersion::kV3;
    }

    namespace {

        void write_be64(uint64_t value, char* dst) {
            for (int i = 0; i < 8; ++i) {
                dst[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xFF);
            }
        }

        uint64_t read_be64(const char* src) {
            uint64_t value = 0;
            for (int i = 0; i < 8; ++i) {
                value = (value << 8) | static_cast<unsigned char>(src[i]);
            }
            return value;
        }

    } // namespace

    std::string base64_encode_uuid(const Uuid& uuid) {
        // Standard UUID byte layout: msb big-endian into bytes[0..7], lsb into
        // bytes[8..15] (Java Base64Utils.encode).
        std::string bytes(16, '\0');
        write_be64(uuid.msb, &bytes[0]);
        write_be64(uuid.lsb, &bytes[8]);

        // URL-and-filename-safe alphabet ('-' '_'), matching Java's
        // Base64.getUrlEncoder(); strip the trailing padding to 22 chars.
        std::string encoded;
        absl::WebSafeBase64Escape(bytes, &encoded);
        while (!encoded.empty() && encoded.back() == '=') {
            encoded.pop_back();
        }
        return encoded;
    }

    std::optional<Uuid> base64_decode_uuid(std::string_view value) {
        std::string bytes;
        if (!absl::WebSafeBase64Unescape(value, &bytes) || bytes.size() != 16) {
            return std::nullopt;
        }
        Uuid uuid;
        uuid.msb = read_be64(&bytes[0]);
        uuid.lsb = read_be64(&bytes[8]);
        return uuid;
    }

    Uuid generate_uuid_v7() {
        const auto now_ms = static_cast<uint64_t>(
            to_milli_seconds(std::chrono::system_clock::now()));

        absl::BitGen gen;
        // Abseil's entropy pool is process-global. If it was initialized
        // before fork(), sibling children inherit the same pool state and can
        // draw identical random bits. XORing the pid into the random field
        // domain-separates those inherited streams while preserving a uniform
        // distribution for independently seeded processes.
        const uint64_t process_discriminator = static_cast<uint64_t>(getpid());
        const uint64_t rand_a = absl::Uniform<uint64_t>(gen) & 0x0FFFULL;             // 12 bits
        const uint64_t rand_b =
            (absl::Uniform<uint64_t>(gen) ^ process_discriminator)
            & 0x3FFFFFFFFFFFFFFFULL; // 62 bits

        Uuid uuid;
        // msb: 48-bit unix_ts_ms | version(0b0111) | rand_a(12 bits)
        uuid.msb = ((now_ms & 0xFFFFFFFFFFFFULL) << 16) | (0x7ULL << 12) | rand_a;
        // lsb: variant(0b10) | rand_b(62 bits)
        uuid.lsb = (0x2ULL << 62) | rand_b;
        return uuid;
    }

    bool validate_id(std::string_view value, size_t max_len) {
        if (value.empty() || value.size() > max_len) {
            return false;
        }
        for (const char c : value) {
            const bool ok = (c >= 'a' && c <= 'z') ||
                            (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') ||
                            c == '.' || c == '-' || c == '_';
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    std::string ObjectName::to_string() const {
        std::string s = "ObjectName{version=" + std::to_string(version) +
                        ", agentId='" + agent_id + '\'' +
                        ", agentName='" + agent_name + '\'' +
                        ", applicationName='" + application_name + '\'';
        if (is_v4()) {
            // apiKey intentionally masked.
            s += ", serviceName='" + service_name + '\'' + ", apiKey='****'";
        }
        s += '}';
        return s;
    }

    namespace {

        std::string new_base64_uid() {
            const Uuid uuid = generate_uuid_v7();
            return base64_encode_uuid(uuid);
        }

        // v1/v3 resolution -> ObjectNameV1 (version 1). The only difference between
        // v1 and v3 is the applicationName length limit.
        std::optional<ObjectName> resolve_v1_v3(const ObjectNameInput& input,
                                                size_t application_name_max_len) {
            ObjectName obj;
            obj.version = object_name::VERSION_V1;

            // agentId: auto-generated as base64(UUIDv7). A valid input id only
            // arrives on a config reload (the running agent's id) and is kept
            // so the identity stays stable.
            if (validate_id(input.agent_id, object_name::AGENT_ID_MAX_LEN)) {
                obj.agent_id = input.agent_id;
                LOG_INFO("resolved AgentId='{}'", obj.agent_id);
            } else {
                obj.agent_id = new_base64_uid();
                LOG_INFO("auto generated AgentId='{}'", obj.agent_id);
            }

            // applicationName: required.
            if (!validate_id(input.application_name, application_name_max_len)) {
                LOG_ERROR("Failed to resolve ApplicationName (required, max length {})",
                          application_name_max_len);
                return std::nullopt;
            }
            obj.application_name = input.application_name;

            // agentName: optional, falls back to agentId.
            if (validate_id(input.agent_name, object_name::AGENT_NAME_MAX_LEN)) {
                obj.agent_name = input.agent_name;
            } else {
                obj.agent_name = obj.agent_id;
            }

            return obj;
        }

        // v4 resolution -> ObjectNameV4 (version 4).
        std::optional<ObjectName> resolve_v4(const ObjectNameInput& input) {
            ObjectName obj;
            obj.version = object_name::VERSION_V4;

            // agentId: auto-generated as base64(UUIDv7). A decodable input id
            // only arrives on a config reload (the running agent's own
            // generated id) and is kept so the identity stays stable instead
            // of tripping the non-reloadable-change warning with a freshly
            // minted id. base64_decode_uuid() serves purely as validation.
            if (base64_decode_uuid(input.agent_id)) {
                obj.agent_id = input.agent_id;
                LOG_INFO("v4 resolved AgentId='{}'", obj.agent_id);
            } else {
                obj.agent_id = new_base64_uid();
                LOG_INFO("v4 auto generated AgentId='{}'", obj.agent_id);
            }

            // applicationName: required.
            if (!validate_id(input.application_name, object_name::AGENT_NAME_MAX_LEN_V4)) {
                LOG_ERROR("Failed to resolve ApplicationName (required, max length {})",
                          object_name::AGENT_NAME_MAX_LEN_V4);
                return std::nullopt;
            }
            obj.application_name = input.application_name;

            // serviceName: required, like Java (ObjectNameResolverV4 throws
            // "ServiceName not provided") and Go. No "DEFAULT" fallback: a
            // missing or mistyped name must fail startup, not register the
            // agent under the wrong service.
            if (!validate_id(input.service_name, object_name::SERVICE_NAME_MAX_LEN)) {
                LOG_ERROR("Failed to resolve ServiceName (required for uid.version=v4, max length {})",
                          object_name::SERVICE_NAME_MAX_LEN);
                return std::nullopt;
            }
            obj.service_name = input.service_name;

            // apiKey: required for v4, only checked for non-emptiness.
            if (input.api_key.empty()) {
                LOG_ERROR("Failed to resolve ApiKey (required for uid.version=v4)");
                return std::nullopt;
            }
            obj.api_key = input.api_key;

            // agentName: required, falls back to base64(agentId UUID).
            if (validate_id(input.agent_name, object_name::AGENT_NAME_MAX_LEN_V4)) {
                obj.agent_name = input.agent_name;
            } else {
                obj.agent_name = obj.agent_id; // base64 of the same UUID
            }

            return obj;
        }

    } // namespace

    std::optional<ObjectName> resolve_object_name(NameVersion version, const ObjectNameInput& input) {
        std::optional<ObjectName> result;
        switch (version) {
            case NameVersion::kV1:
                result = resolve_v1_v3(input, object_name::APPLICATION_NAME_MAX_LEN_V1);
                break;
            case NameVersion::kV4:
                result = resolve_v4(input);
                break;
            case NameVersion::kV3:
            default:
                result = resolve_v1_v3(input, object_name::APPLICATION_NAME_MAX_LEN_V3);
                break;
        }
        if (result.has_value()) {
            LOG_INFO("resolved agent identity: {}", result->to_string());
        }
        return result;
    }

} // namespace pinpoint
