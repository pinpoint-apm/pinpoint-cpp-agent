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
#include <vector>

#include "atomic_shared_ptr.h"

namespace pinpoint {
namespace {

// The generation/TLS-cache behavior under test is the opt-in policy.
template <typename T>
using CachedAtomicSharedPtr = AtomicSharedPtr<T, SnapshotCache::ThreadCached>;

TEST(AtomicSharedPtrTest, StoreIsObservedByReaderOnItsNextLoad) {
    CachedAtomicSharedPtr<const int> value(std::make_shared<const int>(1));
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
    CachedAtomicSharedPtr<const int> first(std::make_shared<const int>(11));
    CachedAtomicSharedPtr<const int> second(std::make_shared<const int>(22));

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
    CachedAtomicSharedPtr<const CheckedSnapshot> value(
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

// The default policy must not leave a strong reference behind in TLS: the
// global agent holder relies on this so ~AgentImpl can never run from
// thread/process teardown (see global_agent() in agent.cpp).
TEST(AtomicSharedPtrTest, UncachedLoadDoesNotPinSnapshotInThreadLocalStorage) {
    auto initial = std::make_shared<const int>(5);
    AtomicSharedPtr<const int> value(initial);

    EXPECT_EQ(*value.load(), 5);
    value.store(std::make_shared<const int>(6));

    // Only the local `initial` still refers to the first snapshot.
    EXPECT_EQ(initial.use_count(), 1);
}

// Pins the documented load_cached_ref() contract: the reference names this
// thread's cache slot, so a store alone leaves it untouched and the next load
// of the same holder on this thread rebinds it to the fresh snapshot.
TEST(AtomicSharedPtrTest, CachedRefRebindsToNewSnapshotOnNextLoad) {
    CachedAtomicSharedPtr<const int> value(std::make_shared<const int>(1));

    const auto& ref = value.load_cached_ref();
    EXPECT_EQ(*ref, 1);

    value.store(std::make_shared<const int>(2));
    EXPECT_EQ(*ref, 1);

    (void)value.load_cached_ref();
    EXPECT_EQ(*ref, 2);
}

TEST(AtomicSharedPtrTest, ThreadCachedLoadPinsSnapshotUntilNextLoad) {
    auto initial = std::make_shared<const int>(5);
    CachedAtomicSharedPtr<const int> value(initial);

    EXPECT_EQ(*value.load_cached_ref(), 5);
    value.store(std::make_shared<const int>(6));

    // The holder released the first snapshot but this thread's cache entry
    // still pins it until the next load refreshes the entry.
    EXPECT_EQ(initial.use_count(), 2);
    EXPECT_EQ(*value.load_cached_ref(), 6);
    EXPECT_EQ(initial.use_count(), 1);
}

TEST(AtomicSharedPtrTest, ReusedAddressDoesNotResurrectCachedSnapshot) {
    using Holder = CachedAtomicSharedPtr<const int>;
    alignas(Holder) unsigned char storage[sizeof(Holder)];

    auto* first = ::new (static_cast<void*>(storage))
        Holder(std::make_shared<const int>(31));
    EXPECT_EQ(*first->load_cached_ref(), 31);
    first->~Holder();

    auto* second = ::new (static_cast<void*>(storage))
        Holder(std::make_shared<const int>(47));
    EXPECT_EQ(*second->load_cached_ref(), 47);
    second->~Holder();
}

// State for ThreadCachedLoadDuringTlsTeardownStaysSafe: the host-object
// destructor runs during thread TLS teardown, where gtest assertions are
// awkward, so it reports what it observed through these globals instead.
CachedAtomicSharedPtr<const int>* g_teardown_holder = nullptr;
std::atomic<int> g_teardown_observed{0};

struct TeardownHostTlsObject {
    void touch() const {}
    ~TeardownHostTlsObject() {
        if (g_teardown_holder != nullptr) {
            g_teardown_observed.store(*g_teardown_holder->load_cached_ref(),
                                      std::memory_order_relaxed);
        }
    }
};

TEST(AtomicSharedPtrTest, ThreadCachedLoadDuringTlsTeardownStaysSafe) {
    CachedAtomicSharedPtr<const int> value(std::make_shared<const int>(42));
    g_teardown_holder = &value;
    g_teardown_observed.store(0, std::memory_order_relaxed);

    std::thread host_thread([&value] {
        // Constructed before the holder's first load on this thread, so TLS
        // teardown (reverse construction order) destroys it after the
        // cache's reclaim guard: its destructor re-enters load_cached_ref()
        // once the thread cache map is already reclaimed — the host pattern
        // (a thread_local connection wrapper recording a final span at
        // thread exit) the teardown-safe slot in thread_cache() exists for.
        // That post-teardown load deliberately leaks one small map.
        static thread_local TeardownHostTlsObject host_object;
        host_object.touch();
        EXPECT_EQ(*value.load_cached_ref(), 42);
    });
    host_thread.join();

    EXPECT_EQ(g_teardown_observed.load(std::memory_order_relaxed), 42)
        << "a load from a host thread_local destructor must still observe the value";
    g_teardown_holder = nullptr;
}

}  // namespace
}  // namespace pinpoint
