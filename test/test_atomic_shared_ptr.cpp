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

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#include "atomic_shared_ptr.h"

namespace pinpoint {
namespace {

TEST(AtomicSharedPtrTest, StoreIsObservedByReaderOnItsNextLoad) {
    AtomicSharedPtr<const int> value(std::make_shared<const int>(1));
    std::atomic<bool> reader_ready{false};
    std::atomic<bool> store_complete{false};
    std::atomic<int> before_store{0};
    std::atomic<int> after_store{0};

    std::thread reader([&] {
        before_store.store(*value.load_cached_ref(), std::memory_order_relaxed);
        reader_ready.store(true, std::memory_order_release);
        while (!store_complete.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        after_store.store(*value.load_cached_ref(), std::memory_order_relaxed);
    });

    while (!reader_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    value.store(std::make_shared<const int>(2));
    store_complete.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(before_store.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(after_store.load(std::memory_order_relaxed), 2);
}

TEST(AtomicSharedPtrTest, SameTypeInstancesHaveIndependentThreadCaches) {
    AtomicSharedPtr<const int> first(std::make_shared<const int>(11));
    AtomicSharedPtr<const int> second(std::make_shared<const int>(22));

    EXPECT_EQ(*first.load_cached_ref(), 11);
    EXPECT_EQ(*second.load_cached_ref(), 22);

    second.store(std::make_shared<const int>(23));
    EXPECT_EQ(*second.load_cached_ref(), 23);
    EXPECT_EQ(*first.load(), 11);  // The original by-value API stays additive.
}

struct CheckedSnapshot {
    uint64_t sequence;
    uint64_t inverse;
};

TEST(AtomicSharedPtrTest, ConcurrentReadersAndStoresSeeWholeSnapshots) {
    constexpr uint64_t kStoreCount = 5000;
    constexpr size_t kReaderCount = 8;
    AtomicSharedPtr<const CheckedSnapshot> value(
        std::make_shared<const CheckedSnapshot>(CheckedSnapshot{0, ~uint64_t{0}}));
    std::atomic<bool> start{false};
    std::atomic<bool> stores_done{false};
    std::atomic<uint64_t> errors{0};
    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);

    for (size_t i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stores_done.load(std::memory_order_acquire)) {
                const auto& snapshot = value.load_cached_ref();
                if (snapshot->inverse != ~snapshot->sequence) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }

            const auto final_snapshot = value.load();
            if (final_snapshot->sequence != kStoreCount ||
                final_snapshot->inverse != ~kStoreCount) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (uint64_t i = 1; i <= kStoreCount; ++i) {
            value.store(std::make_shared<const CheckedSnapshot>(
                CheckedSnapshot{i, ~i}));
        }
        stores_done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(errors.load(std::memory_order_relaxed), 0u);
}

TEST(AtomicSharedPtrTest, ReusedAddressDoesNotResurrectCachedSnapshot) {
    using Holder = AtomicSharedPtr<const int>;
    std::aligned_storage<sizeof(Holder), alignof(Holder)>::type storage;

    auto* first = ::new (static_cast<void*>(&storage))
        Holder(std::make_shared<const int>(31));
    EXPECT_EQ(*first->load_cached_ref(), 31);
    first->~Holder();

    auto* second = ::new (static_cast<void*>(&storage))
        Holder(std::make_shared<const int>(47));
    EXPECT_EQ(*second->load_cached_ref(), 47);
    second->~Holder();
}

}  // namespace
}  // namespace pinpoint
