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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace pinpoint {

    struct ActiveSpanShard;

    /**
     * @brief Intrusive registration node for the active-request histogram.
     *
     * Embedded in SpanImpl and UnsampledSpan, so registering with AgentStats
     * (addActiveSpan/dropActiveSpan) links/unlinks this node in a shard's
     * doubly-linked list instead of inserting into a map — no heap allocation
     * on the per-request hot path. The agent is a guest in the host's process
     * and cannot choose its allocator, so a malloc/free pair per request has
     * unpredictable cost across hosts.
     *
     * Lifetime contract: a linked node must never outlive its owning span, or
     * it leaves dangling pointers in the shard list. Owners enforce this with a
     * destructor backstop calling dropActiveSpan only while still linked — one
     * atomic load after a normal EndSpan, and it avoids touching a non-owning
     * AgentService once the node no longer needs the registry.
     */
    struct ActiveSpanNode {
        // List linkage; guarded by shard_->mutex_ while linked.
        ActiveSpanNode* prev_{nullptr};
        ActiveSpanNode* next_{nullptr};
        // Span start time (epoch ms); written before linking, read by the
        // stats snapshot under the shard lock.
        int64_t start_time_{0};
        // Shard this node links into. Written before the release store on
        // linked_, so a dropper that observed linked_ == true without the
        // lock also sees the shard pointer it needs to take that lock.
        ActiveSpanShard* shard_{nullptr};
        // Fast-path guard: lets dropActiveSpan return without touching the
        // shard mutex when there is nothing to unlink (async spans never
        // link; destructor backstops run after EndSpan already unlinked).
        std::atomic<bool> linked_{false};

        ActiveSpanNode() = default;
        ActiveSpanNode(const ActiveSpanNode&) = delete;
        ActiveSpanNode& operator=(const ActiveSpanNode&) = delete;

        bool isLinked() const noexcept {
            return linked_.load(std::memory_order_acquire);
        }
    };

    // Own cache line per shard, like AgentStats::ResponseTimeShard: every
    // request touches a shard twice — link at span start, unlink at EndSpan —
    // and the shard is picked by the random span id, so without the padding
    // two threads holding *different* shard mutexes would still fight over
    // one straddled line, giving back half of what the sharding bought.
    struct alignas(64) ActiveSpanShard {
        std::mutex mutex_;
        ActiveSpanNode* head_{nullptr};
    };

    /**
     * @brief Sharded registry of in-flight spans, backing the active-request
     * histogram AgentStats sends with each agent stat.
     *
     * Header-only so benchmark/active_span_benchmark.cpp measures the exact
     * production code. AgentStats owns one instance and delegates
     * addActiveSpan/dropActiveSpan/collectActiveRequests to it.
     */
    class ActiveSpanRegistry {
    public:
        void add(ActiveSpanNode& node, int64_t span_id, int64_t start_time) {
            // A span registers exactly once (SpanImpl::extractContext or the
            // UnsampledSpan constructor). Re-linking a linked node would
            // corrupt the shard list, so degrade a contract violation to a
            // no-op — the same tolerance the old map's try_emplace used to
            // give a duplicate id.
            if (node.linked_.load(std::memory_order_relaxed)) {
                return;
            }
            auto& shard = shardOf(span_id);
            node.start_time_ = start_time;
            node.shard_ = &shard;
            std::lock_guard<std::mutex> lock(shard.mutex_);
            node.prev_ = nullptr;
            node.next_ = shard.head_;
            if (shard.head_ != nullptr) {
                shard.head_->prev_ = &node;
            }
            shard.head_ = &node;
            // Release pairs with the acquire in drop's unlocked check: a
            // dropper that sees true must also see shard_, which it reads
            // before taking the shard lock.
            node.linked_.store(true, std::memory_order_release);
        }

        void drop(ActiveSpanNode& node) {
            // Unlocked fast path: nothing to unlink for async spans (never
            // linked) and for the destructor backstop after EndSpan already
            // dropped the node — one atomic load, no shard mutex.
            if (!node.linked_.load(std::memory_order_acquire)) {
                return;
            }
            auto* shard = node.shard_;
            std::lock_guard<std::mutex> lock(shard->mutex_);
            // Re-check under the lock: EndSpan's drop and the destructor
            // backstop can race; the loser must no-op, not unlink twice.
            if (!node.linked_.load(std::memory_order_relaxed)) {
                return;
            }
            if (node.prev_ != nullptr) {
                node.prev_->next_ = node.next_;
            } else {
                shard->head_ = node.next_;
            }
            if (node.next_ != nullptr) {
                node.next_->prev_ = node.prev_;
            }
            node.prev_ = nullptr;
            node.next_ = nullptr;
            node.linked_.store(false, std::memory_order_release);
        }

        /// @brief Buckets every linked span's age at @p sample_time_ms into
        /// the Pinpoint active-request histogram (<1s, <3s, <5s, >=5s).
        void collect(int32_t buckets[4], int64_t sample_time_ms) {
            buckets[0] = 0;
            buckets[1] = 0;
            buckets[2] = 0;
            buckets[3] = 0;

            for (auto& shard : shards_) {
                std::lock_guard<std::mutex> lock(shard.mutex_);
                for (const auto* node = shard.head_; node != nullptr; node = node->next_) {
                    auto active_time = sample_time_ms - node->start_time_;
                    if (active_time < 1000) {
                        buckets[0]++;
                    } else if (active_time < 3000) {
                        buckets[1]++;
                    } else if (active_time < 5000) {
                        buckets[2]++;
                    } else {
                        buckets[3]++;
                    }
                }
            }
        }

    private:
        static constexpr size_t kShardCount = 64;

        ActiveSpanShard& shardOf(int64_t span_id) {
            const auto shard_index = std::hash<int64_t>{}(span_id) % shards_.size();
            return shards_[shard_index];
        }

        std::array<ActiveSpanShard, kShardCount> shards_;
    };

}  // namespace pinpoint
