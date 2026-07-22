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

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "sharded_bounded_queue.h"

namespace pinpoint {
namespace {

    TEST(ShardedBoundedQueueTest, RejectsZeroCapacity) {
        EXPECT_THROW((ShardedBoundedQueue<std::unique_ptr<int>>(0)),
                     std::invalid_argument);
    }

    TEST(ShardedBoundedQueueTest, SplitsConfiguredCapacityIntoUsefulSizedShards) {
        ShardedBoundedQueue<std::unique_ptr<int>> queue(17, 8);
        EXPECT_EQ(queue.capacity(), 17u);
        EXPECT_EQ(queue.shard_count(), 1u);

        ShardedBoundedQueue<std::unique_ptr<int>> small_queue(3, 32);
        EXPECT_EQ(small_queue.capacity(), 3u);
        EXPECT_EQ(small_queue.shard_count(), 1u);

        ShardedBoundedQueue<std::unique_ptr<int>> default_queue(1024, 32);
        EXPECT_EQ(default_queue.capacity(), 1024u);
        EXPECT_EQ(default_queue.shard_count(), 32u);
    }

    TEST(ShardedBoundedQueueTest, CapacityOneHeadDropsAndKeepsNewestValue) {
        ShardedBoundedQueue<std::unique_ptr<int>> queue(1);

        for (int value = 0; value < 10000; ++value) {
            auto item = std::make_unique<int>(value);
            queue.enqueue(item);
            EXPECT_EQ(item, nullptr);
        }

        std::unique_ptr<int> consumed;
        ASSERT_TRUE(queue.try_dequeue(consumed));
        ASSERT_NE(consumed, nullptr);
        EXPECT_EQ(*consumed, 9999);
        EXPECT_FALSE(queue.try_dequeue(consumed));
        EXPECT_EQ(queue.dropped_oldest(), 9999u);
    }

    TEST(ShardedBoundedQueueTest, OneShardIsFifoAndHeadDropsAcrossWraparound) {
        ShardedBoundedQueue<std::unique_ptr<int>> queue(3, 1);

        for (int value = 1; value <= 4; ++value) {
            auto item = std::make_unique<int>(value);
            queue.enqueue(item);
        }

        EXPECT_EQ(queue.dropped_oldest(), 1u);
        for (int expected = 2; expected <= 4; ++expected) {
            std::unique_ptr<int> item;
            ASSERT_TRUE(queue.try_dequeue(item));
            ASSERT_NE(item, nullptr);
            EXPECT_EQ(*item, expected);
        }
        std::unique_ptr<int> empty;
        EXPECT_FALSE(queue.try_dequeue(empty));
    }

    TEST(ShardedBoundedQueueTest, BatchDequeueDrainsFifoAndRespectsLimit) {
        ShardedBoundedQueue<std::unique_ptr<int>> queue(16, 1);

        for (int value = 1; value <= 10; ++value) {
            auto item = std::make_unique<int>(value);
            queue.enqueue(item);
        }

        std::vector<std::unique_ptr<int>> batch;
        EXPECT_EQ(queue.try_dequeue_batch(batch, 0), 0u);
        EXPECT_TRUE(batch.empty());

        // Appends across calls and respects max_items per call.
        ASSERT_EQ(queue.try_dequeue_batch(batch, 4), 4u);
        ASSERT_EQ(queue.try_dequeue_batch(batch, 4), 4u);
        ASSERT_EQ(queue.try_dequeue_batch(batch, 4), 2u);
        EXPECT_EQ(queue.try_dequeue_batch(batch, 4), 0u);

        ASSERT_EQ(batch.size(), 10u);
        for (int expected = 1; expected <= 10; ++expected) {
            ASSERT_NE(batch[expected - 1], nullptr);
            EXPECT_EQ(*batch[expected - 1], expected);
        }
    }

    TEST(ShardedBoundedQueueTest, BatchDequeuePreservesPerProducerOrderAcrossShards) {
        struct Item {
            size_t producer;
            size_t sequence;
        };

        constexpr size_t kProducerCount = 4;
        // Matches each shard's base quota so no producer ever borrows or
        // head-drops: every enqueued item must come back out.
        constexpr size_t kItemsPerProducer = 64;
        ShardedBoundedQueue<std::unique_ptr<Item>> queue(
            kProducerCount * kItemsPerProducer, kProducerCount);

        std::vector<std::thread> producers;
        for (size_t producer = 0; producer < kProducerCount; ++producer) {
            producers.emplace_back([&, producer] {
                for (size_t sequence = 0; sequence < kItemsPerProducer; ++sequence) {
                    auto item = std::make_unique<Item>(Item{producer, sequence});
                    queue.enqueue(item);
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }
        ASSERT_EQ(queue.dropped_oldest(), 0u);

        // A batch size that is not a multiple of any shard's backlog, so the
        // drain rotates across shards and resumes shards mid-run.
        std::vector<std::unique_ptr<Item>> batch;
        std::vector<size_t> next_sequence(kProducerCount, 0);
        size_t total = 0;
        while (queue.try_dequeue_batch(batch, 24) > 0) {
            for (auto& item : batch) {
                ASSERT_NE(item, nullptr);
                ASSERT_LT(item->producer, kProducerCount);
                EXPECT_EQ(item->sequence, next_sequence[item->producer]);
                ++next_sequence[item->producer];
                ++total;
            }
            batch.clear();
        }
        EXPECT_EQ(total, kProducerCount * kItemsPerProducer);

        std::unique_ptr<Item> leftover;
        EXPECT_FALSE(queue.try_dequeue(leftover));
    }

    TEST(ShardedBoundedQueueTest, ConcurrentProducersPreservePerProducerOrder) {
        struct Item {
            size_t producer;
            size_t sequence;
        };

        constexpr size_t kProducerCount = 8;
        constexpr size_t kItemsPerProducer = 4000;
        constexpr size_t kTotalItems = kProducerCount * kItemsPerProducer;
        ShardedBoundedQueue<std::unique_ptr<Item>> queue(kTotalItems, kProducerCount);

        std::atomic<bool> start{false};
        std::atomic<size_t> finished{0};
        std::vector<std::thread> producers;
        producers.reserve(kProducerCount);
        for (size_t producer = 0; producer < kProducerCount; ++producer) {
            producers.emplace_back([&, producer] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (size_t sequence = 0; sequence < kItemsPerProducer; ++sequence) {
                    auto item = std::make_unique<Item>(Item{producer, sequence});
                    queue.enqueue(item);
                }
                finished.fetch_add(1, std::memory_order_release);
            });
        }

        start.store(true, std::memory_order_release);
        std::vector<size_t> next_sequence(kProducerCount, 0);
        size_t consumed = 0;
        while (finished.load(std::memory_order_acquire) < kProducerCount ||
               consumed < kTotalItems) {
            std::unique_ptr<Item> item;
            if (!queue.try_dequeue(item)) {
                std::this_thread::yield();
                continue;
            }
            ASSERT_EQ(item->sequence, next_sequence[item->producer]);
            ++next_sequence[item->producer];
            ++consumed;
        }

        for (auto& producer : producers) {
            producer.join();
        }

        EXPECT_EQ(queue.dropped_oldest(), 0u);
        EXPECT_EQ(consumed, kTotalItems);
        for (const auto sequence : next_sequence) {
            EXPECT_EQ(sequence, kItemsPerProducer);
        }
    }

    TEST(ShardedBoundedQueueTest, ConcurrentOverflowAccountsForEveryItem) {
        constexpr size_t kProducerCount = 16;
        constexpr size_t kItemsPerProducer = 2000;
        constexpr size_t kTotalItems = kProducerCount * kItemsPerProducer;
        constexpr size_t kCapacity = 257;
        ShardedBoundedQueue<std::unique_ptr<size_t>> queue(kCapacity, kProducerCount);

        std::atomic<bool> start{false};
        std::vector<std::thread> producers;
        producers.reserve(kProducerCount);
        for (size_t producer = 0; producer < kProducerCount; ++producer) {
            producers.emplace_back([&, producer] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (size_t sequence = 0; sequence < kItemsPerProducer; ++sequence) {
                    auto item = std::make_unique<size_t>(
                        producer * kItemsPerProducer + sequence);
                    queue.enqueue(item);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& producer : producers) {
            producer.join();
        }

        std::vector<bool> seen(kTotalItems, false);
        size_t retained = 0;
        std::unique_ptr<size_t> item;
        while (queue.try_dequeue(item)) {
            ASSERT_LT(*item, kTotalItems);
            EXPECT_FALSE(seen[*item]);
            seen[*item] = true;
            ++retained;
        }

        EXPECT_EQ(retained, kCapacity);
        EXPECT_EQ(retained + queue.dropped_oldest(), kTotalItems);
    }

    TEST(ShardedBoundedQueueTest, ShardHeadDropRetainsEachProducersNewestValues) {
        struct Item {
            size_t producer;
            size_t sequence;
        };

        constexpr size_t kProducerCount = 2;
        constexpr size_t kItemsPerProducer = 100;
        constexpr size_t kQuotaPerProducer = 32;
        ShardedBoundedQueue<std::unique_ptr<Item>> queue(64, kProducerCount);

        std::atomic<size_t> first_items_enqueued{0};
        std::vector<std::thread> producers;
        for (size_t producer = 0; producer < kProducerCount; ++producer) {
            producers.emplace_back([&, producer] {
                auto first = std::make_unique<Item>(Item{producer, 0});
                queue.enqueue(first);
                first_items_enqueued.fetch_add(1, std::memory_order_release);
                while (first_items_enqueued.load(std::memory_order_acquire) <
                       kProducerCount) {
                    std::this_thread::yield();
                }
                for (size_t sequence = 1; sequence < kItemsPerProducer; ++sequence) {
                    auto item = std::make_unique<Item>(Item{producer, sequence});
                    queue.enqueue(item);
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }

        std::vector<std::vector<size_t>> retained(kProducerCount);
        std::unique_ptr<Item> item;
        while (queue.try_dequeue(item)) {
            ASSERT_NE(item, nullptr);
            retained[item->producer].push_back(item->sequence);
        }

        for (const auto& sequences : retained) {
            ASSERT_EQ(sequences.size(), kQuotaPerProducer);
            for (size_t offset = 0; offset < kQuotaPerProducer; ++offset) {
                EXPECT_EQ(sequences[offset],
                          kItemsPerProducer - kQuotaPerProducer + offset);
            }
        }
        EXPECT_EQ(queue.dropped_oldest(),
                  kProducerCount * (kItemsPerProducer - kQuotaPerProducer));
    }

    TEST(ShardedBoundedQueueTest, LateProducerReclaimsReservedQuotaWithinGlobalCapacity) {
        struct Item {
            size_t producer;
            size_t sequence;
        };

        ShardedBoundedQueue<std::unique_ptr<Item>> queue(64, 2);

        std::thread first([&] {
            for (size_t sequence = 0; sequence < 64; ++sequence) {
                auto item = std::make_unique<Item>(Item{0, sequence});
                queue.enqueue(item);
            }
        });
        first.join();

        // The first producer borrowed both 32-cell quotas. Activating a second
        // shard reclaims its reservation and head-drops the first producer's
        // oldest 32 items before admitting its own 32.
        std::thread late([&] {
            for (size_t sequence = 0; sequence < 32; ++sequence) {
                auto item = std::make_unique<Item>(Item{1, sequence});
                queue.enqueue(item);
            }
        });
        late.join();

        std::vector<std::vector<size_t>> retained(2);
        std::unique_ptr<Item> item;
        while (queue.try_dequeue(item)) {
            ASSERT_NE(item, nullptr);
            retained[item->producer].push_back(item->sequence);
        }

        ASSERT_EQ(retained[0].size(), 32u);
        ASSERT_EQ(retained[1].size(), 32u);
        for (size_t offset = 0; offset < 32; ++offset) {
            EXPECT_EQ(retained[0][offset], offset + 32);
            EXPECT_EQ(retained[1][offset], offset);
        }
        EXPECT_EQ(queue.dropped_oldest(), 32u);
    }

    TEST(ShardedBoundedQueueTest, QuotaReclaimUsesNoThrowOperationsAfterScratchAllocation) {
        // The queue's contract only requires no-throw default construction and
        // move assignment. A throwing move constructor catches accidental
        // push_back/exchange use after quota_ has been committed.
        struct Value {
            Value() noexcept = default;
            explicit Value(int value) noexcept : value(value) {}
            Value(const Value&) = delete;
            Value& operator=(const Value&) = delete;
            Value(Value&&) {
                throw std::runtime_error("move construction must not run");
            }
            Value& operator=(Value&& other) noexcept {
                value = std::exchange(other.value, -1);
                return *this;
            }
            ~Value() noexcept = default;

            int value{-1};
        };

        ShardedBoundedQueue<Value> queue(64, 2);
        std::thread first([&] {
            for (int value = 0; value < 64; ++value) {
                Value item(value);
                queue.enqueue(item);
            }
        });
        first.join();

        std::exception_ptr reclaim_failure;
        std::thread late([&] {
            try {
                Value item(64);
                queue.enqueue(item);
            } catch (...) {
                reclaim_failure = std::current_exception();
            }
        });
        late.join();

        EXPECT_EQ(reclaim_failure, nullptr);
        EXPECT_EQ(queue.dropped_oldest(), 32u);

        size_t retained = 0;
        Value item;
        while (queue.try_dequeue(item)) {
            ++retained;
        }
        EXPECT_EQ(retained, 33u);
    }

    TEST(ShardedBoundedQueueTest, HeadDropDestroysOverwrittenValuesPromptly) {
        // Regression: the saturated overwrite path used to advance head_ past
        // the dropped oldest value without destroying it, so up to the full
        // physical ring (shard_count * capacity cells) of already-dropped
        // values stayed alive instead of the documented `capacity` bound.
        constexpr size_t kProducerCount = 2;
        constexpr size_t kItemsPerProducer = 200;
        constexpr size_t kCapacity = 64;

        std::atomic<size_t> alive{0};
        ShardedBoundedQueue<std::shared_ptr<int>> queue(kCapacity, kProducerCount);

        std::atomic<size_t> first_items_enqueued{0};
        std::vector<std::thread> producers;
        for (size_t producer = 0; producer < kProducerCount; ++producer) {
            producers.emplace_back([&] {
                const auto make_item = [&alive] {
                    alive.fetch_add(1, std::memory_order_relaxed);
                    return std::shared_ptr<int>(new int(0), [&alive](int* p) {
                        alive.fetch_sub(1, std::memory_order_relaxed);
                        delete p;
                    });
                };
                // Barrier after the first item so both shards are active (and
                // the overwrite path is taken) for the bulk of the enqueues.
                auto first = make_item();
                queue.enqueue(first);
                first_items_enqueued.fetch_add(1, std::memory_order_release);
                while (first_items_enqueued.load(std::memory_order_acquire) <
                       kProducerCount) {
                    std::this_thread::yield();
                }
                for (size_t sequence = 1; sequence < kItemsPerProducer; ++sequence) {
                    auto item = make_item();
                    queue.enqueue(item);
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }

        EXPECT_LE(alive.load(), kCapacity)
            << "dropped values must be destroyed when overwritten, not retained "
               "until the ring wraps back around";

        size_t dequeued = 0;
        std::shared_ptr<int> item;
        while (queue.try_dequeue(item)) {
            ASSERT_NE(item, nullptr);
            item.reset();
            ++dequeued;
        }
        EXPECT_LE(dequeued, kCapacity);
        EXPECT_EQ(alive.load(), 0u)
            << "every value must be destroyed once dequeued or overwritten";
    }

} // namespace
} // namespace pinpoint
