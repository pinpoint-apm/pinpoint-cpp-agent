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
     * @brief Snapshot-serving policy for AtomicSharedPtr.
     *
     * Uncached (the default): every load() takes an owning copy directly from
     * the shared source and retains nothing once the caller drops it.
     *
     * ThreadCached: unchanged values are served from a generation-validated
     * thread-local snapshot, and load_cached_ref() becomes available. Each
     * cache entry owns the source snapshot through a separate, per-thread
     * control block. An owning load() therefore changes only that thread's
     * local refcount on the unchanged path instead of ping-ponging the shared
     * source control block between reader cores.
     *
     * The cache pins each reader thread's last snapshot until that thread's
     * next load of the same holder or its exit, so the pointee's destructor
     * can run during thread or process teardown, long after the holder
     * released it. Opt in only when the pointee is passive data that is safe
     * to destroy there — never for objects whose teardown must stay explicit,
     * such as the global AgentImpl holder (see global_agent() in agent.cpp).
     *
     * Retention that opt-in accepts: after a store(), threads that never load
     * this holder again keep the previous snapshot alive until they exit, and
     * a destroyed holder's per-thread entries are only reclaimed when a new
     * holder of the same T reuses its address or the thread exits. Keep
     * ThreadCached holders few, long-lived, and their snapshots bounded in
     * size, or that memory lingers for the lifetime of every reader thread.
     * A load that runs while the calling thread is already tearing down its
     * TLS (a host thread_local destructor recording a final span) stays
     * defined behavior but leaks one small replacement map for that thread —
     * see thread_cache().
     */
    enum class SnapshotCache { Uncached, ThreadCached };

    /**
     * @brief Thread-safe wrapper around std::shared_ptr.
     *
     * The shared source uses std::atomic<std::shared_ptr<T>> when the C++20
     * specialization is available, and falls back to std::shared_mutex on
     * platforms such as Apple libc++ where it is still missing. With
     * SnapshotCache::ThreadCached, unchanged values are additionally served
     * from a generation-validated thread-local snapshot (see SnapshotCache for
     * the lifetime trade-off that opt-in carries).
     */
    template <typename T, SnapshotCache Cache = SnapshotCache::Uncached>
    class AtomicSharedPtr {
    public:
        AtomicSharedPtr() = default;
        explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : ptr_(std::move(ptr)) {}

        AtomicSharedPtr(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;
        AtomicSharedPtr(AtomicSharedPtr&&) = delete;
        AtomicSharedPtr& operator=(AtomicSharedPtr&&) = delete;

        /**
         * @brief Return an owning copy of the current value.
         *
         * Under SnapshotCache::ThreadCached this also refreshes the calling
         * thread's cache entry, which keeps holding the snapshot after the
         * returned copy is dropped. Unchanged owning loads copy a localized
         * alias, so their refcount RMW is private to this reader thread unless
         * the returned owner is explicitly handed to another thread. See
         * SnapshotCache for the retention implications. Uncached retains
         * nothing once the copy is dropped.
         */
        std::shared_ptr<T> load() const {
            if constexpr (Cache == SnapshotCache::ThreadCached) {
                return load_cached_ref();
            } else {
                return load_shared_source();
            }
        }

        /**
         * @brief Return this thread's generation-validated snapshot by reference.
         *
         * The reference points into thread-local storage and remains valid until
         * the next load() or load_cached_ref() of this AtomicSharedPtr on the
         * same thread. Callers that retain the pointer beyond that point, or
         * hand it to another thread, must copy it into an owning shared_ptr.
         *
         * That invalidation includes re-entrant loads: while this reference —
         * or anything pointing into the snapshot it names — is alive, do not
         * call code that may load this holder again on the same thread. Host
         * callbacks are the trap: a re-entrant load racing a store() refreshes
         * the thread's entry and can drop the last owner of the old snapshot,
         * leaving such interior references dangling. Around any call that can
         * run host code, hold an owning copy instead (load(), or a copy of the
         * needed member) — see AgentImpl::NewSpan / recordServerHeader.
         */
        const std::shared_ptr<T>& load_cached_ref() const {
            static_assert(Cache == SnapshotCache::ThreadCached,
                          "load_cached_ref() requires SnapshotCache::ThreadCached; "
                          "an Uncached holder must hand out owning copies via load()");
            const uint64_t generation = generation_.load(std::memory_order_acquire);
            auto& cache = thread_cache();
            auto cached = cache.find(this);
            if (cached != cache.end() &&
                cached->second.instance_id == instance_id_ &&
                cached->second.generation == generation) {
                return cached->second.snapshot;
            }

            auto snapshot = localize(load_shared_source());
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
        // One of these is allocated per reader thread and observed generation.
        // Its `source` is the sole reference that reader normally contributes
        // to the shared source control block. Cache hits and owning load()
        // copies share LocalOwner's distinct control block instead, keeping
        // their frequent RMWs local to the reader core.
        struct LocalOwner {
            explicit LocalOwner(std::shared_ptr<T> value) : source(std::move(value)) {}
            std::shared_ptr<T> source;
        };

        struct CacheEntry {
            uint64_t instance_id;
            uint64_t generation;
            std::shared_ptr<T> snapshot;
        };

        using ThreadCache = std::unordered_map<const AtomicSharedPtr*, CacheEntry>;

        // Per-thread cache storage, split into a trivially-destructible slot
        // plus a separate reclaim guard so a load during thread teardown stays
        // defined behavior. A block-scope thread_local with a destructor must
        // not be passed through again once destroyed ([basic.start.term]) —
        // yet a host thread_local destructor that records a final span during
        // thread exit re-enters load_cached_ref() exactly then: TLS
        // destruction runs in reverse construction order, and a host object
        // constructed before this cache's first use is destroyed after it.
        // The slot has no destructor, so it is never "destroyed" and stays
        // valid for the whole thread lifetime; only the map it points at is
        // reclaimed, by the guard.
        struct ThreadCacheSlot {
            ThreadCache* map = nullptr;
            // Set by the guard's destructor: from then on thread_cache()
            // takes the leak path below instead of re-registering a guard
            // (impossible once TLS destructors have started).
            bool reclaimed = false;
        };

        struct ThreadCacheReclaim {
            ~ThreadCacheReclaim() {
                auto& slot = thread_cache_slot();
                delete slot.map;
                slot.map = nullptr;
                slot.reclaimed = true;
            }
        };

        static ThreadCacheSlot& thread_cache_slot() {
            // Trivially destructible and constant-initialized: safe to touch
            // at any point of the thread's lifetime, including from other
            // thread_local destructors after this thread's TLS teardown began.
            static thread_local ThreadCacheSlot slot;
            return slot;
        }

        static ThreadCache& thread_cache() {
            // The map is per T and per thread. Entries are keyed by address to
            // avoid accumulating one entry per construction on a long-lived
            // thread, while the construction id stored in each entry prevents
            // address reuse from resurrecting an old snapshot.
            auto& slot = thread_cache_slot();
            if (slot.map == nullptr) {
                slot.map = new ThreadCache();
                if (!slot.reclaimed) {
                    // Normal first use on this thread: register the guard that
                    // reclaims the map at thread exit.
                    static thread_local ThreadCacheReclaim reclaim;
                    (void)reclaim;
                } else {
                    // Teardown re-entry: the guard already ran and cannot be
                    // registered again, so this replacement map is deliberately
                    // leaked — bounded at one small map per thread that loads
                    // again during its own exit, and the pointee snapshots it
                    // pins are exactly the ones ThreadCached opts into keeping
                    // alive through teardown (see SnapshotCache).
                }
            }
            return *slot.map;
        }

        std::shared_ptr<T> load_shared_source() const {
#if defined(__cpp_lib_atomic_shared_ptr)
            return ptr_.load();
#else
            std::shared_lock lock(mutex_);
            return ptr_;
#endif
        }

        static std::shared_ptr<T> localize(std::shared_ptr<T> source) {
            if (!source) {
                return {};
            }
            auto owner = std::make_shared<LocalOwner>(std::move(source));
            auto* pointee = owner->source.get();
            // Aliasing gives this thread a new control block while preserving
            // the exact source pointer (including const qualification).
            return std::shared_ptr<T>(owner, pointee);
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
