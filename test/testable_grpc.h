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
#include <memory>
#include <thread>
#include <utility>

#include <gmock/gmock.h>

#include "../src/config.h"
#include "../src/grpc.h"
#include "v1/Service_mock.grpc.pb.h"

namespace pinpoint {

// Polls pred until it holds or the timeout elapses; returns whether it held.
template <typename Pred>
bool wait_for_condition(Pred&& pred, std::chrono::milliseconds timeout,
                        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return true;
}

// Testable gRPC clients: real channels are never contacted (readyChannel() is
// false and create_stub() keeps the injected mock stub), yet registerAgent()
// reports success so Start() flips the agent to "enabled". The mock stubs
// carry no expectations — with the channel never ready, the workers never
// exercise them.

class TestableGrpcAgent : public GrpcAgent {
public:
    explicit TestableGrpcAgent(std::shared_ptr<const Config> config)
        : GrpcAgent(std::move(config)) {}

    void injectMockStubs() {
        set_agent_stub(std::make_unique<::testing::NiceMock<v1::MockAgentStub>>());
    }

    bool readyChannel() override { return false; }

    // Skip real build_agent_info (which does slow DNS resolution).
    GrpcRequestStatus registerAgent() override { return SEND_OK; }

protected:
    void create_stub(const std::shared_ptr<grpc::Channel>&) override {}
};

class TestableGrpcMetadata : public GrpcMetadata {
public:
    explicit TestableGrpcMetadata(std::shared_ptr<const Config> config)
        : GrpcMetadata(std::move(config)) {}

    void injectMockStubs() {
        set_meta_stub(std::make_unique<::testing::NiceMock<v1::MockMetadataStub>>());
    }

    bool readyChannel() override { return false; }

protected:
    void create_stub(const std::shared_ptr<grpc::Channel>&) override {}
};

class TestableGrpcSpan : public GrpcSpan {
public:
    explicit TestableGrpcSpan(std::shared_ptr<const Config> config)
        : GrpcSpan(std::move(config)) {}

    void injectMockStubs() {
        set_span_stub(std::make_unique<::testing::NiceMock<v1::MockSpanStub>>());
    }

    bool readyChannel() override { return false; }

protected:
    void create_stub(const std::shared_ptr<grpc::Channel>&) override {}
};

class TestableGrpcStats : public GrpcStats {
public:
    explicit TestableGrpcStats(std::shared_ptr<const Config> config)
        : GrpcStats(std::move(config)) {}

    void injectMockStubs() {
        set_stats_stub(std::make_unique<::testing::NiceMock<v1::MockStatStub>>());
    }

    bool readyChannel() override { return false; }

protected:
    void create_stub(const std::shared_ptr<grpc::Channel>&) override {}
};

// The full client set for AgentImpl construction, stubs already injected.
struct TestableGrpcClients {
    std::unique_ptr<TestableGrpcAgent> agent;
    std::unique_ptr<TestableGrpcMetadata> metadata;
    std::unique_ptr<TestableGrpcSpan> span;
    std::unique_ptr<TestableGrpcStats> stats;
};

inline TestableGrpcClients make_testable_grpc_clients(
        const std::shared_ptr<const Config>& config) {
    TestableGrpcClients clients{
        std::make_unique<TestableGrpcAgent>(config),
        std::make_unique<TestableGrpcMetadata>(config),
        std::make_unique<TestableGrpcSpan>(config),
        std::make_unique<TestableGrpcStats>(config)};
    clients.agent->injectMockStubs();
    clients.metadata->injectMockStubs();
    clients.span->injectMockStubs();
    clients.stats->injectMockStubs();
    return clients;
}

} // namespace pinpoint
