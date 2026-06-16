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

#include "../src/object_name.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <set>
#include <string>

namespace pinpoint {

namespace {

// Parse a canonical "8-4-4-4-12" UUID string into msb/lsb (big-endian layout).
Uuid uuid_from_string(const std::string& s) {
    std::string hex;
    hex.reserve(32);
    for (char c : s) {
        if (c != '-') {
            hex.push_back(c);
        }
    }
    EXPECT_EQ(hex.size(), 32u);
    Uuid uuid;
    uuid.msb = std::strtoull(hex.substr(0, 16).c_str(), nullptr, 16);
    uuid.lsb = std::strtoull(hex.substr(16, 16).c_str(), nullptr, 16);
    return uuid;
}

struct GoldenVector {
    const char* uuid;
    const char* base64;
};

// Java-verified vectors (Base64Utils.encode(UUID)). See task spec section 5.2.
const GoldenVector kGoldenVectors[] = {
    {"00000000-0000-0000-0000-000000000000", "AAAAAAAAAAAAAAAAAAAAAA"},
    {"ffffffff-ffff-ffff-ffff-ffffffffffff", "_____________________w"},
    {"12345678-90ab-cdef-1234-567890abcdef", "EjRWeJCrze8SNFZ4kKvN7w"},
    {"00112233-4455-6677-8899-aabbccddeeff", "ABEiM0RVZneImaq7zN3u_w"},
    {"0192f1a0-7e8b-7c3d-9f2e-1a2b3c4d5e6f", "AZLxoH6LfD2fLhorPE1ebw"},
    {"deadbeef-dead-beef-dead-beefdeadbeef", "3q2-796tvu_erb7v3q2-7w"},
};

} // namespace

// ---------------------------------------------------------------------------
// Base64 / UUID encoding (Java byte-compatibility)
// ---------------------------------------------------------------------------

TEST(Base64Uuid, EncodeMatchesGoldenVectors) {
    for (const auto& v : kGoldenVectors) {
        const Uuid uuid = uuid_from_string(v.uuid);
        const std::string encoded = base64_encode_uuid(uuid);
        EXPECT_EQ(encoded, v.base64) << "uuid=" << v.uuid;
        EXPECT_EQ(encoded.size(), 22u) << "uuid=" << v.uuid;
    }
}

TEST(Base64Uuid, UrlSafeAlphabetUsed) {
    // ffff... must use '_' (URL-safe) and never '+' or '/' or padding '='.
    const std::string all_ones = base64_encode_uuid(uuid_from_string(kGoldenVectors[1].uuid));
    EXPECT_NE(all_ones.find('_'), std::string::npos);
    EXPECT_EQ(all_ones.find('+'), std::string::npos);
    EXPECT_EQ(all_ones.find('/'), std::string::npos);
    EXPECT_EQ(all_ones.find('='), std::string::npos);

    // deadbeef... exercises both '-' and '_'.
    const std::string dead = base64_encode_uuid(uuid_from_string(kGoldenVectors[5].uuid));
    EXPECT_NE(dead.find('-'), std::string::npos);
    EXPECT_NE(dead.find('_'), std::string::npos);
}

// ---------------------------------------------------------------------------
// UUID v7 generation (RFC 9562)
// ---------------------------------------------------------------------------

TEST(UuidV7, VersionAndVariantBits) {
    for (int i = 0; i < 100; ++i) {
        const Uuid uuid = generate_uuid_v7();
        // version nibble (bits 12-15 of msb) must be 0x7.
        EXPECT_EQ((uuid.msb >> 12) & 0xF, 0x7u);
        // variant (top 2 bits of lsb) must be 0b10.
        EXPECT_EQ((uuid.lsb >> 62) & 0x3u, 0x2u);
    }
}

TEST(UuidV7, EncodesTo22CharsAndIsUnique) {
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        const std::string b64 = base64_encode_uuid(generate_uuid_v7());
        EXPECT_EQ(b64.size(), 22u);
        seen.insert(b64);
    }
    EXPECT_EQ(seen.size(), 100u) << "generated agent ids should be unique";
}

// ---------------------------------------------------------------------------
// Version string parsing
// ---------------------------------------------------------------------------

TEST(NameVersionParse, KnownValuesCaseInsensitive) {
    EXPECT_EQ(parse_name_version("v1"), NameVersion::kV1);
    EXPECT_EQ(parse_name_version("V1"), NameVersion::kV1);
    EXPECT_EQ(parse_name_version("v3"), NameVersion::kV3);
    EXPECT_EQ(parse_name_version("V3"), NameVersion::kV3);
    EXPECT_EQ(parse_name_version("v4"), NameVersion::kV4);
    EXPECT_EQ(parse_name_version("V4"), NameVersion::kV4);
}

TEST(NameVersionParse, UnknownAndEmptyFallBackToV3) {
    EXPECT_EQ(parse_name_version(""), NameVersion::kV3);
    EXPECT_EQ(parse_name_version("v2"), NameVersion::kV3);
    EXPECT_EQ(parse_name_version("garbage"), NameVersion::kV3);
}

// ---------------------------------------------------------------------------
// ID validation
// ---------------------------------------------------------------------------

TEST(ValidateId, CharsetAndLength) {
    EXPECT_TRUE(validate_id("abcABC123.-_", 24));
    EXPECT_FALSE(validate_id("", 24));                 // empty
    EXPECT_FALSE(validate_id("has space", 24));        // space not allowed
    EXPECT_FALSE(validate_id("slash/here", 24));       // '/' not allowed
    EXPECT_FALSE(validate_id("kor\xea\xb0\x80", 24));  // multibyte (non-ASCII) rejected
}

TEST(ValidateId, LengthBoundary) {
    EXPECT_TRUE(validate_id(std::string(24, 'a'), 24));
    EXPECT_FALSE(validate_id(std::string(25, 'a'), 24));
    EXPECT_TRUE(validate_id(std::string(254, 'a'), 254));
    EXPECT_FALSE(validate_id(std::string(255, 'a'), 254));
}

// ---------------------------------------------------------------------------
// resolve_object_name : v1 / v3 / v4
// ---------------------------------------------------------------------------

ObjectNameInput baseInput() {
    ObjectNameInput in;
    in.application_name = "test-app";
    return in;
}

TEST(ResolveV1V3, ProvidedAgentIdUsed) {
    ObjectNameInput in = baseInput();
    in.agent_id = "my-agent-id";
    const auto obj = resolve_object_name(NameVersion::kV3, in);
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(obj->version, 1);
    EXPECT_EQ(obj->agent_id, "my-agent-id");
    EXPECT_EQ(obj->application_name, "test-app");
    EXPECT_EQ(obj->protocol_version(), 100);
    EXPECT_FALSE(obj->is_v4());
}

TEST(ResolveV1V3, MissingAgentIdAutoGeneratedBase64) {
    const auto obj = resolve_object_name(NameVersion::kV3, baseInput());
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(obj->agent_id.size(), 22u); // base64(UUID)
}

TEST(ResolveV1V3, AgentNameFallsBackToAgentId) {
    ObjectNameInput in = baseInput();
    in.agent_id = "agent-1";
    const auto obj = resolve_object_name(NameVersion::kV3, in);
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(obj->agent_name, "agent-1");
}

TEST(ResolveV1V3, MissingApplicationNameFails) {
    ObjectNameInput in;
    in.agent_id = "agent-1";
    EXPECT_FALSE(resolve_object_name(NameVersion::kV1, in).has_value());
    EXPECT_FALSE(resolve_object_name(NameVersion::kV3, in).has_value());
}

TEST(ResolveV1V3, ApplicationNameLengthLimitDiffers) {
    // 25-char application name: invalid for v1 (max 24), valid for v3 (max 254).
    ObjectNameInput in = baseInput();
    in.agent_id = "agent-1";
    in.application_name = std::string(25, 'a');

    EXPECT_FALSE(resolve_object_name(NameVersion::kV1, in).has_value());

    const auto v3 = resolve_object_name(NameVersion::kV3, in);
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(v3->application_name, std::string(25, 'a'));

    // 255-char application name: invalid even for v3 (max 254).
    in.application_name = std::string(255, 'a');
    EXPECT_FALSE(resolve_object_name(NameVersion::kV3, in).has_value());
}

TEST(ResolveV4, HappyPath) {
    ObjectNameInput in;
    in.application_name = "test-app";
    in.service_name = "test-service";
    in.api_key = "secret-key";
    in.agent_name = "my-agent";

    const auto obj = resolve_object_name(NameVersion::kV4, in);
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(obj->version, 4);
    EXPECT_TRUE(obj->is_v4());
    EXPECT_EQ(obj->protocol_version(), 400);
    EXPECT_EQ(obj->agent_id.size(), 22u);
    EXPECT_EQ(obj->agent_name, "my-agent");
    EXPECT_EQ(obj->service_name, "test-service");
    EXPECT_EQ(obj->api_key, "secret-key");
    // agent_id must be base64 of the generated UUID.
    EXPECT_EQ(base64_encode_uuid(obj->agent_uuid), obj->agent_id);
}

TEST(ResolveV4, AgentIdInputIgnoredAndRegenerated) {
    ObjectNameInput in;
    in.agent_id = "user-supplied-id";
    in.application_name = "test-app";
    in.service_name = "test-service";
    in.api_key = "secret-key";

    const auto a = resolve_object_name(NameVersion::kV4, in);
    const auto b = resolve_object_name(NameVersion::kV4, in);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE(a->agent_id, "user-supplied-id");
    EXPECT_NE(a->agent_id, b->agent_id); // fresh UUID each resolve
}

TEST(ResolveV4, AgentNameDefaultsToBase64AgentId) {
    ObjectNameInput in;
    in.application_name = "test-app";
    in.service_name = "test-service";
    in.api_key = "secret-key";
    // no agent_name provided

    const auto obj = resolve_object_name(NameVersion::kV4, in);
    ASSERT_TRUE(obj.has_value());
    EXPECT_EQ(obj->agent_name, obj->agent_id);
}

TEST(ResolveV4, MissingServiceNameFails) {
    ObjectNameInput in;
    in.application_name = "test-app";
    in.api_key = "secret-key";
    EXPECT_FALSE(resolve_object_name(NameVersion::kV4, in).has_value());
}

TEST(ResolveV4, MissingApiKeyFails) {
    ObjectNameInput in;
    in.application_name = "test-app";
    in.service_name = "test-service";
    EXPECT_FALSE(resolve_object_name(NameVersion::kV4, in).has_value());
}

TEST(ResolveV4, MissingApplicationNameFails) {
    ObjectNameInput in;
    in.service_name = "test-service";
    in.api_key = "secret-key";
    EXPECT_FALSE(resolve_object_name(NameVersion::kV4, in).has_value());
}

TEST(ResolveV4, ToStringMasksApiKey) {
    ObjectNameInput in;
    in.application_name = "test-app";
    in.service_name = "test-service";
    in.api_key = "super-secret-key";

    const auto obj = resolve_object_name(NameVersion::kV4, in);
    ASSERT_TRUE(obj.has_value());
    const std::string s = obj->to_string();
    EXPECT_EQ(s.find("super-secret-key"), std::string::npos);
    EXPECT_NE(s.find("****"), std::string::npos);
}

} // namespace pinpoint
