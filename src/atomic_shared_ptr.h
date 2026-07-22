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

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

#if !defined(__cpp_lib_atomic_shared_ptr)
#include <mutex>
#include <shared_mutex>
#endif

namespace pinpoint {

    /**
     * @brief Thread-safe wrapper around std::shared_ptr.
     *
     * Unchanged values are served from a generation-validated thread-local
     * snapshot. Refreshes use std::atomic<std::shared_ptr<T>> when the C++20
     * specialization is available, and fall back to std::shared_mutex on
     * platforms such as Apple libc++ where it is still missing.
     */
    template <typename T>
    class AtomicSharedPtr {
    public:
        AtomicSharedPtr() = default;
        explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}

        AtomicSharedPtr(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr(AtomicSharedPtr&&) = delete;
        AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;

        std::shared_ptr<T> load() const {
            return load_cached_ref();
        }

        /**
         * @brief Return this thread's generation-validated snapshot by reference.
         *
         * The reference points into thread-local storage and remains valid until
         * the next load() or load_cached_ref() of this AtomicSharedPtr on the
         * same thread. Callers that retain the pointer beyond that point, or
         * hand it to another thread, must copy it into an owning shared_ptr.
         */
        const std::shared_ptr<T>& load_cached_ref() const {
            const uint64_t generation = generation_.load(std::memory_order_acquire);
            auto& cache = thread_cache();
            auto cached = cache.find(this);
            if (cached != cache.end() &&
                cached->second.instance_id == instance_id_ &&
                cached->second.generation == generation) {
                return cached->second.snapshot;
            }

            auto snapshot = load_shared_source();
            if (cached == cache.end()) {
                cached = cache.emplace(
                    this, CacheEntry{instance_id_, generation, std::move(snapshot)}).first;
            } else {
                cached->second = CacheEntry{instance_id_, generation, std::move(snapshot)};
            }
            return cached->second.snapshot;
        }

        void store(std::shared_ptr<T> desired) {
#if defined(__cpp_lib_atomic_shared_ptr)
            ptr_.store(std::move(desired));
#else
            {
                std::unique_lock lock(mutex_);
                ptr_ = std::move(desired);
            }
#endif
            // Publish the pointer first, then announce its new generation.
            // An acquire load that observes this increment can safely reuse a
            // snapshot only if it was filled for the same generation. During a
            // concurrent store a reader may still return the previous snapshot,
            // but the first load after the store completes must take the slow
            // path and refresh it from the synchronized shared source.
            generation_.fetch_add(1, std::memory_order_release);
        }

    private:
        struct CacheEntry {
            uint64_t instance_id;
            uint64_t generation;
            std::shared_ptr<T> snapshot;
        };

        using ThreadCache = std::unordered_map<const AtomicSharedPtr*, CacheEntry>;

        static ThreadCache& thread_cache() {
            // The map is per T and per thread. Entries are keyed by address to
            // avoid accumulating one entry per construction on a long-lived
            // thread, while the construction id stored in each entry prevents
            // address reuse from resurrecting an old snapshot.
            static thread_local ThreadCache cache;
            return cache;
        }

        std::shared_ptr<T> load_shared_source() const {
#if defined(__cpp_lib_atomic_shared_ptr)
            return ptr_.load();
#else
            std::shared_lock lock(mutex_);
            return ptr_;
#endif
        }

        inline static std::atomic<uint64_t> next_instance_id_{1};
        const uint64_t instance_id_{
            next_instance_id_.fetch_add(1, std::memory_order_relaxed)};
        std::atomic<uint64_t> generation_{0};

#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<T>> ptr_{std::shared_ptr<T>{}};
#else
        mutable std::shared_mutex mutex_;
        std::shared_ptr<T> ptr_;
#endif
    };

}  // namespace pinpoint
