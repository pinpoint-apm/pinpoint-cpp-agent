/*
 * Copyright 2020-present NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pinpoint {

    /**
     * @brief Preallocated bounded queue with producer contention sharded N ways.
     *
     * Producer threads are assigned a stable home shard. Each shard has an
     * independent, short-held mutex and circular buffer, removing the former
     * process-wide mutex and allocation from the hot path. Values never move to
     * another shard, so FIFO ordering is retained for each producer shard.
     *
     * QueueSize remains a global logical bound. Capacity starts evenly divided
     * into quotas; an active shard may borrow the quota of an inactive shard.
     * Quota transfers are rare setup/expansion operations protected by a
     * separate mutex, while steady-state enqueue and head-drop touch only the
     * producer's shard. Cross-shard dequeue order is intentionally unspecified.
     *
     * To keep quota borrowing and every enqueue allocation-free, each shard
     * preallocates a physical ring with QueueSize cells. Physical cell storage
     * is therefore shard_count * QueueSize, even though the global logical
     * retention bound remains QueueSize.
     */
    template <typename T>
    class ShardedBoundedQueue final {
        static_assert(std::is_nothrow_default_constructible_v<T>);
        static_assert(std::is_nothrow_move_assignable_v<T>);
        static_assert(std::is_nothrow_destructible_v<T>);

    public:
        static constexpr size_t kDefaultShardCount = 32;
        static constexpr size_t kMaxShardCount = 64;
        static constexpr size_t kMinCellsPerShard = 32;

        explicit ShardedBoundedQueue(
                size_t capacity, size_t requested_shards = kDefaultShardCount)
            : capacity_(capacity),
              shard_count_(compute_shard_count(capacity, requested_shards)),
              active_shards_(0),
              borrowable_shards_(initial_bitmap(shard_count_)) {
            if (capacity_ == 0) {
                throw std::invalid_argument("ShardedBoundedQueue capacity must be positive");
            }

            shards_.reserve(shard_count_);
            base_quotas_.reserve(shard_count_);
            const size_t base_capacity = capacity_ / shard_count_;
            const size_t extra_cells = capacity_ % shard_count_;
            for (size_t shard = 0; shard < shard_count_; ++shard) {
                const size_t quota = base_capacity + (shard < extra_cells ? 1 : 0);
                // A shard can borrow every other quota when it is the only
                // active producer. Each shard therefore preallocates capacity_
                // physical cells: total physical cells are
                // shard_count_ * capacity_. This keeps all later enqueues
                // allocation-free, while the sum of logical quotas and retained
                // values remains capacity_.
                shards_.push_back(std::make_unique<Shard>(capacity_, quota));
                base_quotas_.push_back(quota);
            }
        }

        ShardedBoundedQueue(const ShardedBoundedQueue&) = delete;
        ShardedBoundedQueue& operator=(const ShardedBoundedQueue&) = delete;
        ShardedBoundedQueue(ShardedBoundedQueue&&) = delete;
        ShardedBoundedQueue& operator=(ShardedBoundedQueue&&) = delete;

        void enqueue(T& value) {
            const size_t home = thread_shard_id() % shard_count_;
            ensure_active(home);

            // Once there is no inactive quota left, enqueue and possible
            // head-drop are deliberately combined under one shard lock. This
            // is the common saturated path and matches the single-lock cost of
            // the legacy queue without its process-wide contention.
            if (borrowable_shards_.load(std::memory_order_acquire) == 0) {
                shards_[home]->enqueue_or_overwrite(value);
                return;
            }

            if (shards_[home]->try_enqueue(value)) {
                return;
            }

            // An isolated producer grows beyond its initial quota by borrowing
            // unused quota in coarse blocks. Once every shard is active (the
            // contended steady state), borrowable_shards_ is zero and this is a
            // read-only branch directly to one-lock head-drop.
            if (try_borrow_inactive_quota(home) &&
                    shards_[home]->try_enqueue(value)) {
                return;
            }

            shards_[home]->enqueue_or_overwrite(value);
        }

        bool try_dequeue(T& value) {
            return dequeue_from_active_shards(value);
        }

        /// @brief Explicit shutdown drain entry point for the quiescent queue.
        bool try_dequeue_after_stop(T& value) {
            return dequeue_from_active_shards(value);
        }

        size_t capacity() const noexcept { return capacity_; }
        size_t shard_count() const noexcept { return shard_count_; }

        uint64_t dropped_oldest() const noexcept {
            uint64_t total = 0;
            for (const auto& shard : shards_) {
                total += shard->dropped_oldest();
            }
            return total;
        }

    private:
        class alignas(64) Shard final {
        public:
            Shard(size_t physical_capacity, size_t initial_quota)
                : cells_(physical_capacity), quota_(initial_quota) {}

            bool is_active() const noexcept {
                return active_.load(std::memory_order_acquire);
            }

            void mark_active() noexcept {
                active_.store(true, std::memory_order_release);
            }

            bool try_enqueue(T& value) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (size_ == quota_) {
                    return false;
                }
                push(value);
                return true;
            }

            void enqueue_or_overwrite(T& value) {
                // Declared before the guard so the dropped value's destructor
                // runs after the lock is released: ~T of a dropped span chunk
                // is a cascade of frees, and this head-drop path runs exactly
                // when the queue is saturated — producers mapped to this shard
                // and consumer probes must not stall behind it.
                T dropped{};
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (size_ < quota_) {
                        push(value);
                        return;
                    }
                    // Every active shard is restored to at least its non-zero
                    // base quota before its first enqueue.
                    if (quota_ == 0) {
                        return;
                    }
                    // Empty the head cell before storing the new value: when
                    // quota_ < physical capacity the tail cell is empty, so
                    // the old head value would otherwise stay alive until
                    // tail_ wraps back around to its cell — retaining up to
                    // the full physical ring instead of quota_ values
                    // (take_excess_quota extracts dropped cells the same
                    // way). Exchanging head_ first also keeps the
                    // quota_ == capacity case correct, where head_ and tail_
                    // are the same cell.
                    dropped = std::exchange(cells_[head_], T{});
                    head_ = next(head_);
                    cells_[tail_] = std::move(value);
                    tail_ = next(tail_);
                    record_drop();
                }
                // dropped is destroyed here, outside the shard lock.
            }

            bool try_dequeue(T& value) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (size_ == 0) {
                    return false;
                }
                pop(value);
                return true;
            }

            size_t quota() const {
                std::lock_guard<std::mutex> lock(mutex_);
                return quota_;
            }

            size_t take_inactive_quota() {
                std::lock_guard<std::mutex> lock(mutex_);
                if (active_.load(std::memory_order_relaxed) || size_ != 0) {
                    return 0;
                }
                return std::exchange(quota_, 0);
            }

            size_t take_excess_quota(size_t base_quota, size_t requested) {
                // Declared before the guard so the dropped values are
                // destroyed after the lock is released, mirroring
                // enqueue_or_overwrite. This rebalance path is rare (runs once
                // when a shard turns active while a borrower holds its
                // cells), so the vector's one possible allocation is a fair
                // trade for keeping a batch of ~T cascades out of the
                // critical section.
                std::vector<T> dropped;
                std::lock_guard<std::mutex> lock(mutex_);
                const size_t excess = quota_ > base_quota ? quota_ - base_quota : 0;
                const size_t transferred = std::min(excess, requested);
                quota_ -= transferred;

                // A newly active shard reclaims its reserved base quota. If a
                // borrower currently uses those cells, preserve the newest data
                // by head-dropping only the excess oldest values.
                if (size_ > quota_) {
                    dropped.reserve(size_ - quota_);
                    while (size_ > quota_) {
                        dropped.push_back(std::exchange(cells_[head_], T{}));
                        head_ = next(head_);
                        --size_;
                        record_drop();
                    }
                }
                return transferred;
            }

            void add_quota(size_t amount) {
                std::lock_guard<std::mutex> lock(mutex_);
                quota_ += amount;
            }

            uint64_t dropped_oldest() const noexcept {
                return dropped_oldest_.load(std::memory_order_relaxed);
            }

        private:
            void push(T& value) noexcept {
                cells_[tail_] = std::move(value);
                tail_ = next(tail_);
                ++size_;
            }

            void pop(T& value) noexcept {
                value = std::move(cells_[head_]);
                head_ = next(head_);
                --size_;
            }

            size_t next(size_t position) const noexcept {
                ++position;
                return position == cells_.size() ? 0 : position;
            }

            void record_drop() noexcept {
                // All writers already hold mutex_, so a relaxed load/store is
                // sufficient and avoids a locked atomic read-modify-write. The
                // atomic representation lets the worker read drop snapshots
                // without taking every producer shard mutex.
                const auto current = dropped_oldest_.load(std::memory_order_relaxed);
                dropped_oldest_.store(current + 1, std::memory_order_relaxed);
            }

            std::vector<T> cells_;
            mutable std::mutex mutex_;
            std::atomic<bool> active_{false};
            size_t quota_{0};
            size_t head_{0};
            size_t tail_{0};
            size_t size_{0};
            std::atomic<uint64_t> dropped_oldest_{0};
        };

        void ensure_active(size_t home) {
            if (shards_[home]->is_active()) {
                return;
            }

            std::lock_guard<std::mutex> rebalance_lock(rebalance_mutex_);
            if (shards_[home]->is_active()) {
                return;
            }

            const uint64_t home_bit = shard_bit(home);
            borrowable_shards_.fetch_and(~home_bit, std::memory_order_relaxed);

            const size_t current_quota = shards_[home]->quota();
            size_t needed = base_quotas_[home] - std::min(current_quota, base_quotas_[home]);
            for (size_t donor = 0; donor < shard_count_ && needed > 0; ++donor) {
                if (donor == home) {
                    continue;
                }
                const size_t transferred = shards_[donor]->take_excess_quota(
                    base_quotas_[donor], needed);
                if (transferred > 0) {
                    shards_[home]->add_quota(transferred);
                    needed -= transferred;
                }
            }

            shards_[home]->mark_active();
            active_shards_.fetch_or(home_bit, std::memory_order_release);
        }

        bool try_borrow_inactive_quota(size_t home) {
            std::lock_guard<std::mutex> rebalance_lock(rebalance_mutex_);
            uint64_t candidates = borrowable_shards_.load(std::memory_order_relaxed);
            for (size_t offset = 1; offset < shard_count_; ++offset) {
                const size_t donor = (home + offset) % shard_count_;
                const uint64_t donor_bit = shard_bit(donor);
                if ((candidates & donor_bit) == 0) {
                    continue;
                }

                const size_t transferred = shards_[donor]->take_inactive_quota();
                borrowable_shards_.fetch_and(~donor_bit, std::memory_order_relaxed);
                candidates &= ~donor_bit;
                if (transferred > 0) {
                    shards_[home]->add_quota(transferred);
                    return true;
                }
            }
            return false;
        }

        bool dequeue_from_active_shards(T& value) {
            const uint64_t active = active_shards_.load(std::memory_order_acquire);
            const size_t begin = consumer_cursor_;
            for (size_t offset = 0; offset < shard_count_; ++offset) {
                const size_t shard = (begin + offset) % shard_count_;
                if ((active & shard_bit(shard)) == 0) {
                    continue;
                }
                if (shards_[shard]->try_dequeue(value)) {
                    consumer_cursor_ = (shard + 1) % shard_count_;
                    return true;
                }
            }
            return false;
        }

        static size_t thread_shard_id() noexcept {
            // One process-wide atomic increment per producer thread. Enqueues
            // after the first do not touch any global assignment state.
            static std::atomic<size_t> next_id{0};
            thread_local const size_t id = next_id.fetch_add(1, std::memory_order_relaxed);
            return id;
        }

        static size_t compute_shard_count(
                size_t capacity, size_t requested_shards) noexcept {
            // Tiny queues gain no useful contention reduction from one-cell
            // shards and would unnecessarily relax their FIFO behavior.
            const size_t useful_shards = std::max<size_t>(1, capacity / kMinCellsPerShard);
            return std::min(capacity,
                            std::min(kMaxShardCount,
                                     std::min(std::max<size_t>(1, requested_shards),
                                              useful_shards)));
        }

        static uint64_t initial_bitmap(size_t shard_count) noexcept {
            return shard_count == kMaxShardCount
                ? ~uint64_t{0}
                : (uint64_t{1} << shard_count) - 1;
        }

        static uint64_t shard_bit(size_t shard) noexcept {
            return uint64_t{1} << shard;
        }

        const size_t capacity_;
        const size_t shard_count_;
        std::vector<std::unique_ptr<Shard>> shards_;
        std::vector<size_t> base_quotas_;
        std::atomic<uint64_t> active_shards_;
        std::atomic<uint64_t> borrowable_shards_;
        std::mutex rebalance_mutex_;
        // Own cache line: the consumer writes this on every successful
        // dequeue, while producers read active_shards_/borrowable_shards_
        // above on every enqueue. Without the padding the cursor write would
        // ping-pong the line those reads ride on — the exact cross-thread
        // line sharing the per-shard design set out to remove.
        alignas(64) size_t consumer_cursor_{0};
    };

} // namespace pinpoint
