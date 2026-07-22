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

// Fork-safe agent lifecycle (issue #94). Verifies that a "cold" agent created
// in a master process can be brought online with Start() in each forked child,
// that forked children register with distinct agent ids, and that tearing down
// an agent that was started in the master never joins dead threads in a child
// (no abort).

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "../src/agent.h"
#include "../src/grpc.h"
#include "../src/config.h"
#include "../include/pinpoint/tracer.h"
#include "v1/Service_mock.grpc.pb.h"

using ::testing::_;
using ::testing::Return;
using ::testing::NiceMock;
using ::testing::DoAll;
using ::testing::SetArgPointee;

namespace pinpoint {
namespace {

// Mock gRPC clients: real channels are never contacted (readyChannel() is
// false and create_stub() keeps the injected mock), yet registerAgent()
// reports success so Start() flips the agent to "enabled".
class ForkGrpcAgent : public GrpcAgent {
public:
    explicit ForkGrpcAgent(std::shared_ptr<const Config> cfg) : GrpcAgent(std::move(cfg)) {}
    void injectMockStubs() {
        auto stub = std::make_unique<NiceMock<v1::MockAgentStub>>();
        EXPECT_CALL(*stub, RequestAgentInfo(_, _, _)).WillRepeatedly(Return(grpc::Status::OK));
        set_agent_stub(std::move(stub));
    }
    bool readyChannel() override { return false; }
    GrpcRequestStatus registerAgent() override { return SEND_OK; }
protected:
    void create_stub() override {}
};

class ForkGrpcMetadata : public GrpcMetadata {
public:
    explicit ForkGrpcMetadata(std::shared_ptr<const Config> cfg) : GrpcMetadata(std::move(cfg)) {}
    void injectMockStubs() { set_meta_stub(std::make_unique<NiceMock<v1::MockMetadataStub>>()); }
    bool readyChannel() override { return false; }
protected:
    void create_stub() override {}
};

class ForkGrpcSpan : public GrpcSpan {
public:
    explicit ForkGrpcSpan(std::shared_ptr<const Config> cfg) : GrpcSpan(std::move(cfg)) {}
    void injectMockStubs() { set_span_stub(std::make_unique<NiceMock<v1::MockSpanStub>>()); }
    bool readyChannel() override { return false; }
protected:
    void create_stub() override {}
};

class ForkGrpcStats : public GrpcStats {
public:
    explicit ForkGrpcStats(std::shared_ptr<const Config> cfg) : GrpcStats(std::move(cfg)) {}
    void injectMockStubs() { set_stats_stub(std::make_unique<NiceMock<v1::MockStatStub>>()); }
    bool readyChannel() override { return false; }
protected:
    void create_stub() override {}
};

std::shared_ptr<Config> make_fork_config() {
    auto cfg = std::make_shared<Config>();
    cfg->enable = true;
    cfg->app_name_ = "fork-app";
    cfg->agent_id_ = "fork-agent-id";     // set directly => treated as auto id
    cfg->agent_name_ = "fork-agent-name";
    cfg->collector.host = "127.0.0.1";
    cfg->collector.agent_port = 9991;
    cfg->collector.span_port = 9993;
    cfg->collector.stat_port = 9992;
    cfg->span.queue_size = 1024;
    cfg->span.event_chunk_size = 10;
    cfg->span.max_event_depth = 32;
    cfg->stat.enable = true;
    cfg->stat.collect_interval = 5000;
    cfg->sampling.type = "counter";
    cfg->sampling.counter_rate = 1;
    return cfg;
}

// Builds a COLD agent (no Start()) with mock gRPC clients.
std::shared_ptr<AgentImpl> make_cold_agent(const std::shared_ptr<Config>& cfg) {
    auto a = std::make_unique<ForkGrpcAgent>(cfg);
    auto m = std::make_unique<ForkGrpcMetadata>(cfg);
    auto s = std::make_unique<ForkGrpcSpan>(cfg);
    auto st = std::make_unique<ForkGrpcStats>(cfg);
    a->injectMockStubs();
    m->injectMockStubs();
    s->injectMockStubs();
    st->injectMockStubs();
    return std::make_shared<AgentImpl>(cfg, std::move(a), std::move(m),
                                       std::move(s), std::move(st));
}

void wait_enabled(const std::shared_ptr<AgentImpl>& agent, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!agent->Enable() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

// A master creates a cold agent, forks, and each child brings it online with
// Start(). Each child must obtain a working (enabled) agent and register with a
// distinct agent id, and exit cleanly.
TEST(ForkLifecycleTest, ChildStartYieldsWorkingAgentWithUniqueId) {
    auto cfg = make_fork_config();
    auto agent = make_cold_agent(cfg);   // cold: no threads, no channel yet

    // The cold agent in the master holds no live workers.
    EXPECT_FALSE(agent->Enable());

    int pipes[2][2];
    ASSERT_EQ(pipe(pipes[0]), 0);
    ASSERT_EQ(pipe(pipes[1]), 0);

    pid_t pids[2];
    for (int i = 0; i < 2; ++i) {
        pid_t pid = fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            // Child: bring the inherited cold agent online in this process.
            close(pipes[i][0]);
            agent->Start();
            wait_enabled(agent);
            const bool enabled = agent->Enable();
            const std::string id = agent->getAgentId();
            // Report "<enabled>:<agent_id>" back to the parent.
            std::string msg = (enabled ? "1:" : "0:") + id;
            ssize_t rc = write(pipes[i][1], msg.c_str(), msg.size());
            (void)rc;
            close(pipes[i][1]);
            // Tearing down the freshly-started agent in this process must join
            // its own (live) workers without issue.
            agent->Shutdown();
            _exit(0);
        }
        pids[i] = pid;
        close(pipes[i][1]);
    }

    std::string reports[2];
    for (int i = 0; i < 2; ++i) {
        char buf[256];
        ssize_t n = read(pipes[i][0], buf, sizeof(buf) - 1);
        if (n < 0) n = 0;
        buf[n] = '\0';
        reports[i] = buf;
        close(pipes[i][0]);

        int status = 0;
        ASSERT_EQ(waitpid(pids[i], &status, 0), pids[i]);
        EXPECT_TRUE(WIFEXITED(status)) << "child " << i << " did not exit normally";
        EXPECT_EQ(WEXITSTATUS(status), 0) << "child " << i << " exit status";
    }

    // Both children became enabled.
    EXPECT_EQ(reports[0].substr(0, 2), "1:") << "child 0 not enabled: " << reports[0];
    EXPECT_EQ(reports[1].substr(0, 2), "1:") << "child 1 not enabled: " << reports[1];

    const std::string id0 = reports[0].substr(2);
    const std::string id1 = reports[1].substr(2);
    // Distinct across children, and regenerated away from the configured id.
    EXPECT_NE(id0, id1);
    EXPECT_NE(id0, "fork-agent-id");
    EXPECT_NE(id1, "fork-agent-id");

    agent->Shutdown();
}

// Safety net: an agent that was STARTED in the master is inherited by a child
// and torn down there. The child's worker threads do not exist, so teardown
// must abandon the handles (never join or detach) — the child must not abort.
TEST(ForkLifecycleTest, TeardownOfInheritedStartedAgentDoesNotAbort) {
    auto cfg = make_fork_config();
    auto agent = make_cold_agent(cfg);
    agent->Start();          // master now holds live workers
    wait_enabled(agent);
    ASSERT_TRUE(agent->Enable());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child inherited a "started" agent whose threads are dead here.
        // Destroying it must not join those handles (which would abort).
        agent->Shutdown();
        agent.reset();
        _exit(0);
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status)) << "child aborted while tearing down inherited agent";
    EXPECT_EQ(WEXITSTATUS(status), 0);

    agent->Shutdown();
}

// The child inherits the forking thread's TLS snapshot. A child-side store
// must bump the inherited holder's generation so its next load self-heals.
TEST(ForkLifecycleTest, ChildStoreRefreshesInheritedAtomicSharedPtrSnapshot) {
    AtomicSharedPtr<const int> value(std::make_shared<const int>(1));
    ASSERT_EQ(*value.load_cached_ref(), 1);

    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        value.store(std::make_shared<const int>(2));
        _exit(*value.load_cached_ref() == 2 ? 0 : 1);
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace pinpoint
