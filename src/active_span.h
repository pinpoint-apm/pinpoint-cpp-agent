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
        // Nodes currently linked into head_'s list. Per shard, not one
        // registry-wide atomic, so the increment at span start lands on the
        // cache line this request already owns. Atomic because abandon()
        // adjusts it without the mutex, and size() sums the shards unlocked.
        std::atomic<size_t> count_{0};
    };

    /**
     * @brief Sharded registry of in-flight spans, backing the active-request
     * histogram AgentStats sends with each agent stat.
     *
     * Header-only so benchmark/active_span_benchmark.cpp measures the exact
     * production code. AgentStats owns one instance and delegates
     * addActiveSpan/dropActiveSpan/collectActiveRequests to it.
     *
     * There is deliberately no size cap. Here the nodes are owned by the spans and only
     * linked into the shard lists: the registry unlinking one on its own
     * would race the owner's drop and break the linked_ handshake (add's
     * release / drop's acquire). Refusing an add is not an option either —
     * drop would then unlink a node that was never linked. The registry only
     * counts (see size()); the owner (AgentStats) turns a count above
     * kActiveSpanWarnThreshold into a rate-limited warning.
     */
    class ActiveSpanRegistry {
    public:
        static constexpr size_t kShardCount = 64;

        /**
         * @brief Links @p node into the shard picked by @p span_id.
         *
         * @return The number of nodes in that shard after linking, or 0 when
         *         the call was a no-op. The caller can use it as a cheap
         *         overflow trigger: when every shard holds at most T/kShardCount
         *         nodes the registry holds at most T, so a total above T
         *         implies some shard's count exceeds T/kShardCount — only then
         *         is the 64-load size() worth computing.
         */
        size_t add(ActiveSpanNode& node, int64_t span_id, int64_t start_time) {
            // A span registers once. Re-linking a node would corrupt its shard,
            // so a duplicate registration is a no-op.
            if (node.linked_.load(std::memory_order_relaxed)) {
                return 0;
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
            const auto count = shard.count_.fetch_add(1, std::memory_order_relaxed) + 1;
            // Release pairs with the acquire in drop's unlocked check: a
            // dropper that sees true must also see shard_, which it reads
            // before taking the shard lock.
            node.linked_.store(true, std::memory_order_release);
            return count;
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
            shard->count_.fetch_sub(1, std::memory_order_relaxed);
            node.linked_.store(false, std::memory_order_release);
        }

        /**
         * @brief Forgets @p node without touching a shard.
         *
         * For a caller that has determined this registry's shards are not
         * safe to lock — the registry belongs to another process, so a shard
         * mutex may be inherited locked (see AgentStats' fork guard). The
         * list itself is deliberately left alone: it is the owning process's
         * copy that matters, and this process must not walk it. Clearing
         * `linked_` keeps the destructor backstop from retrying the drop.
         *
         * The shard counter is this process's copy-on-write copy, so it is
         * safe to adjust without the mutex and must be: it was inherited
         * counting this node, and nothing else will ever subtract it. Only
         * the call that flips `linked_` subtracts, so a repeated abandon (or
         * one on a node that never linked) leaves the count alone.
         */
        static void abandon(ActiveSpanNode& node) noexcept {
            if (node.linked_.exchange(false, std::memory_order_acq_rel)) {
                node.shard_->count_.fetch_sub(1, std::memory_order_relaxed);
            }
        }

        /// @brief Nodes currently linked, summed over the shards without
        /// locking — a snapshot that concurrent add/drop may already have
        /// moved past. kShardCount relaxed loads, so callers on the request
        /// path should gate it on the trigger add() returns.
        size_t size() const noexcept {
            size_t total = 0;
            for (const auto& shard : shards_) {
                total += shard.count_.load(std::memory_order_relaxed);
            }
            return total;
        }

        /// @brief Buckets every linked span's age at @p sample_time_ms into
        /// the Pinpoint active-request histogram (<=1s, <=3s, <=5s, >5s).
        void collect(int32_t buckets[4], int64_t sample_time_ms) {
            buckets[0] = 0;
            buckets[1] = 0;
            buckets[2] = 0;
            buckets[3] = 0;

            for (auto& shard : shards_) {
                std::lock_guard<std::mutex> lock(shard.mutex_);
                for (const auto* node = shard.head_; node != nullptr; node = node->next_) {
                    auto active_time = sample_time_ms - node->start_time_;
                    // Inclusive upper bounds: a span at exactly 1000ms is
                    // still "fast".
                    if (active_time <= 1000) {
                        buckets[0]++;
                    } else if (active_time <= 3000) {
                        buckets[1]++;
                    } else if (active_time <= 5000) {
                        buckets[2]++;
                    } else {
                        buckets[3]++;
                    }
                }
            }
        }

    private:
        ActiveSpanShard& shardOf(int64_t span_id) {
            const auto shard_index = std::hash<int64_t>{}(span_id) % shards_.size();
            return shards_[shard_index];
        }

        std::array<ActiveSpanShard, kShardCount> shards_;
    };

}  // namespace pinpoint
