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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "utility.h"

namespace pinpoint {

    enum class SqlMetaMode : uint8_t {
        Id,
        Uid,
    };

    using SqlIdentity = std::variant<int32_t, SqlUid>;

    /**
     * Immutable result of preparing one raw SQL statement. RawSqlCache returns
     * shared ownership so annotations can refer to the extracted parameters
     * without copying them, even after the cache entry itself is evicted.
     */
    struct PreparedSql {
        std::string normalized_sql;
        std::string parameters;
        SqlIdentity identity;
    };

    using PreparedSqlRef = std::shared_ptr<const PreparedSql>;

    /**
     * @brief Generic LRU cache result structure.
     *
     * @tparam ValueType Type of the cached value.
     */
    template<typename ValueType>
    struct LruCacheResult {
        ValueType value;
        bool found;  // true if entry already existed in cache
    };

    /**
     * @brief Result returned from `IdCache::get`.
     *
     * Type alias for LruCacheResult<int32_t>.
     * Use `.value` to access the ID and `.found` to check if it existed in cache.
     */
    using CacheResult = LruCacheResult<int32_t>;

    struct ApiCacheKey {
        std::string_view api_str;
        int32_t api_type;
    };

    struct ApiCacheKeyHash {
        size_t operator()(const ApiCacheKey& key) const noexcept {
            size_t seed = std::hash<std::string_view>{}(key.api_str);
            seed ^= std::hash<int32_t>{}(key.api_type) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    struct StringCacheKeyTraits {
        using LookupKey = std::string_view;
        using StoredKey = std::string;
        using MapKey = std::string_view;
        using Hash = std::hash<MapKey>;
        using Equal = std::equal_to<MapKey>;

        static MapKey lookup_key(LookupKey key) noexcept {
            return key;
        }

        static StoredKey store(LookupKey key) {
            return std::string(key);
        }

        static MapKey map_key(const StoredKey& key) noexcept {
            return std::string_view(key);
        }
    };

    /**
     * Hash-carrying twins of the string/api cache keys, mirroring
     * RawSqlCacheKey/RawSqlCacheStoredKey below: the sharded caches hash a key
     * once per lookup — to pick the shard — and hand that same hash to the
     * shard's unordered_map through these keys (pass-through hasher), so the
     * key bytes are never scanned a second time.
     */
    struct HashedStringCacheKey {
        std::string_view str;
        size_t hash;
    };

    struct HashedStringCacheStoredKey {
        std::string str;
        size_t hash;
    };

    struct HashedStringCacheKeyHash {
        size_t operator()(const HashedStringCacheKey& key) const noexcept {
            return key.hash;
        }
    };

    struct HashedStringCacheKeyEqual {
        bool operator()(const HashedStringCacheKey& lhs, const HashedStringCacheKey& rhs) const noexcept {
            return lhs.str == rhs.str;
        }
    };

    struct HashedStringCacheKeyTraits {
        using LookupKey = HashedStringCacheKey;
        using StoredKey = HashedStringCacheStoredKey;
        using MapKey = HashedStringCacheKey;
        using Hash = HashedStringCacheKeyHash;
        using Equal = HashedStringCacheKeyEqual;

        static MapKey lookup_key(LookupKey key) noexcept {
            return key;
        }

        static StoredKey store(LookupKey key) {
            return HashedStringCacheStoredKey{std::string(key.str), key.hash};
        }

        static MapKey map_key(const StoredKey& key) noexcept {
            return HashedStringCacheKey{key.str, key.hash};
        }
    };

    struct HashedApiCacheKey {
        std::string_view api_str;
        int32_t api_type;
        size_t hash;
    };

    struct HashedApiCacheStoredKey {
        std::string api_str;
        int32_t api_type;
        size_t hash;
    };

    struct HashedApiCacheKeyHash {
        size_t operator()(const HashedApiCacheKey& key) const noexcept {
            return key.hash;
        }
    };

    struct HashedApiCacheKeyEqual {
        bool operator()(const HashedApiCacheKey& lhs, const HashedApiCacheKey& rhs) const noexcept {
            return lhs.api_type == rhs.api_type && lhs.api_str == rhs.api_str;
        }
    };

    struct HashedApiCacheKeyTraits {
        using LookupKey = HashedApiCacheKey;
        using StoredKey = HashedApiCacheStoredKey;
        using MapKey = HashedApiCacheKey;
        using Hash = HashedApiCacheKeyHash;
        using Equal = HashedApiCacheKeyEqual;

        static MapKey lookup_key(LookupKey key) noexcept {
            return key;
        }

        static StoredKey store(LookupKey key) {
            return HashedApiCacheStoredKey{std::string(key.api_str), key.api_type, key.hash};
        }

        static MapKey map_key(const StoredKey& key) noexcept {
            return HashedApiCacheKey{key.api_str, key.api_type, key.hash};
        }
    };

    /**
     * ShardTraits implementations for ShardedLruCache (see its contract):
     * they define the caller-facing key, how to hash it once, and how to
     * attach that hash for the shard's LruCacheImpl.
     */
    struct StringCacheShardTraits {
        using LookupKey = std::string_view;
        using InnerTraits = HashedStringCacheKeyTraits;

        static size_t hash(LookupKey key) noexcept {
            return std::hash<std::string_view>{}(key);
        }

        static HashedStringCacheKey with_hash(LookupKey key, size_t hash) noexcept {
            return HashedStringCacheKey{key, hash};
        }
    };

    struct ApiCacheShardTraits {
        using LookupKey = ApiCacheKey;
        using InnerTraits = HashedApiCacheKeyTraits;

        static size_t hash(LookupKey key) noexcept {
            // Reuses ApiCacheKeyHash so the (string, type) combining logic
            // has a single definition.
            return ApiCacheKeyHash{}(key);
        }

        static HashedApiCacheKey with_hash(LookupKey key, size_t hash) noexcept {
            return HashedApiCacheKey{key.api_str, key.api_type, hash};
        }
    };

    /**
     * @brief Result returned from `SqlUidCache::get`.
     *
     * Type alias for LruCacheResult<SqlUid>. SqlUid is a fixed 16-byte array, so
     * copying a result out of the cache on a hit is allocation-free.
     * Use `.value` to access the UID and `.found` to check if it existed in cache.
     */
    using SqlUidCacheResult = LruCacheResult<SqlUid>;

    /**
     * @brief Thread-safe LRU cache implementation template.
     *
     * Combines a std::list (LRU ordering) with a std::unordered_map keyed by
     * a non-owning map key from KeyTraits (average O(1) lookup without
     * allocating key storage on a hit). Copying the cached ValueType still has
     * that type's normal copy cost. Access is guarded by a std::shared_mutex:
     * lookups take a shared lock so ordinary cache hits can run concurrently.
     * LRU reordering is performed lazily — while the cache is below max_size
     * no eviction can occur, so ordering is irrelevant; once full, an entry is
     * promoted only after it has aged past half the capacity's churn (see
     * get()), so most hits remain shared-lock reads instead of serializing on
     * the exclusive lock the splice needs.
     *
     * @tparam ValueType Type of values stored in the cache.
     * @tparam KeyTraits Converts lookup keys into owned storage and map keys.
     */
    template<typename ValueType, typename KeyTraits = StringCacheKeyTraits>
    class LruCacheImpl {
    private:
        // Declared before the member functions: insert_or_promote() names
        // Node in its parameter list, which — unlike function bodies — is
        // not complete-class context, so a later declaration would not be
        // found there (and could resolve to an unrelated outer Node type).
        struct Node {
            typename KeyTraits::StoredKey key;
            ValueType value;
            // Op sequence at insert / last promotion. Written under the exclusive
            // lock but read under shared locks, hence atomic (relaxed suffices —
            // a stale read only mis-estimates the age; see get()).
            std::atomic<uint64_t> last_promoted;

            Node(typename KeyTraits::StoredKey&& k, ValueType&& v, uint64_t seq)
                : key(std::move(k)), value(std::move(v)), last_promoted(seq) {}
        };

    public:
        using LookupKey = typename KeyTraits::LookupKey;

        // max_size is clamped to >= 1: with a capacity of 0 the eviction in
        // insert_or_promote() would erase the just-inserted front node and
        // then return a reference into the freed list node — use-after-free.
        explicit LruCacheImpl(size_t max_size)
            : max_size_(max_size > 0 ? max_size : 1),
              promote_age_threshold_(max_size_ / 2 > 0 ? max_size_ / 2 : 1) {
            // Reserve buckets up front so the map never rehashes while warming
            // up to capacity. +1 covers the transient over-capacity entry that
            // exists between insertion and eviction inside insert_or_promote().
            cache_map_.reserve(max_size_ + 1);
        }
        ~LruCacheImpl() = default;

        // Delete copy and move operations (mutex is not movable)
        LruCacheImpl(const LruCacheImpl&) = delete;
        LruCacheImpl& operator=(const LruCacheImpl&) = delete;
        LruCacheImpl(LruCacheImpl&&) = delete;
        LruCacheImpl& operator=(LruCacheImpl&&) = delete;

        /**
         * @brief Retrieves or creates a cache entry.
         *
         * Lookups take a shared lock, so cache hits run concurrently. While the
         * cache has not reached capacity no entry can be evicted, so LRU ordering
         * is irrelevant and the splice is skipped entirely — a hit is then a pure
         * shared-lock read. Once full, promoting on every hit would serialize all
         * lookups on the exclusive lock the splice needs, so promotion is aged:
         * an entry promoted within the last promote_age_threshold_ insert/promote
         * operations is still far from the eviction end and its hit stays a pure
         * shared-lock read; only entries past that age pay for the splice. On a
         * miss the generator runs OUTSIDE any lock, so an expensive generator
         * does not serialize other threads' lookups.
         *
         * @param key The key to look up (no allocation on hit).
         * @param generator Function to generate a new value if key not found.
         * @return Result containing the value and whether it was found.
         */
        template<typename Generator>
        LruCacheResult<ValueType> get(LookupKey key, Generator&& generator) {
            const auto map_key = KeyTraits::lookup_key(key);
            bool hit_while_full = false;
            {
                // Fast path: a shared lock lets concurrent hits proceed in parallel.
                std::shared_lock<std::shared_mutex> lock(mutex_);
                const auto it = cache_map_.find(map_key);
                if (it != cache_map_.end()) {
                    if (cache_map_.size() < max_size_) {
                        // Below capacity: nothing can be evicted, so LRU order does
                        // not matter — skip the splice and keep this a pure read.
                        return LruCacheResult<ValueType>{it->second->value, true};
                    }
                    // A stale op_seq_ read races benignly with concurrent
                    // promotions: worst case the age is overestimated and the
                    // entry takes one unnecessary trip through the slow path.
                    const auto age = op_seq_.load(std::memory_order_relaxed) -
                                     it->second->last_promoted.load(std::memory_order_relaxed);
                    if (age < promote_age_threshold_) {
                        // Promoted recently: still in the newer half of the LRU
                        // list, so eviction is not imminent — skip the splice.
                        return LruCacheResult<ValueType>{it->second->value, true};
                    }
                    hit_while_full = true;
                }
            }

            if (hit_while_full) {
                // Cache is full and the entry has aged: promote it so it survives
                // the next evictions. Splicing mutates the list and needs an
                // exclusive lock (a shared lock cannot be upgraded). The entry may
                // have been evicted between the two locks, so re-resolve; if it is
                // gone, fall through to regenerate it below.
                std::unique_lock<std::shared_mutex> lock(mutex_);
                const auto it = cache_map_.find(map_key);
                if (it != cache_map_.end()) {
                    cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
                    it->second->last_promoted.store(next_op_seq(), std::memory_order_relaxed);
                    return LruCacheResult<ValueType>{it->second->value, true};
                }
            }

            // Cache miss: run the (potentially expensive) generator WITHOUT holding
            // the lock. For SqlUidCache this is a MurmurHash over the full SQL text,
            // which would otherwise serialize every concurrent lookup.
            auto new_value = generator();

            // Stage the new node outside the lock as well: the list-node
            // allocation and KeyTraits::store()'s key copy (for RawSqlCache
            // the full raw SQL text, up to 64 KB) would otherwise stall every
            // concurrent shared-lock hit behind malloc + memcpy. Taking the
            // op sequence here (relaxed, like every op_seq_ use) can reorder
            // it slightly against promotions racing under the lock; that only
            // perturbs an entry's age estimate by a step — the same benign
            // race the shared-lock age check above already tolerates.
            // `staged` doubles as the graveyard: the node discarded on a lost
            // insert race and the evicted victim are spliced back onto it, so
            // their destruction also runs after the lock is released.
            std::list<Node> staged;
            staged.emplace_front(KeyTraits::store(key), std::move(new_value),
                                 next_op_seq());

            std::unique_lock<std::shared_mutex> lock(mutex_);
            return insert_or_promote(staged);
        }

        /**
         * @brief Removes an entry from the cache.
         *
         * @param key The key to remove.
         */
        void remove(LookupKey key) {
            // Declared before the lock so the removed node (key storage and
            // value) is destroyed after the exclusive section, not while
            // shared-lock readers wait behind the free.
            std::list<Node> removed;
            std::unique_lock<std::shared_mutex> lock(mutex_);

            const auto it = cache_map_.find(KeyTraits::lookup_key(key));
            if (it != cache_map_.end()) {
                removed.splice(removed.begin(), cache_list_, it->second);
                cache_map_.erase(it);
            }
        }

    private:
        /**
         * @brief Inserts the staged node, or promotes an existing entry.
         *
         * The candidate list node and its owned key bytes are pre-built by
         * get() in `staged`, outside the lock. This critical section relinks
         * list pointers and updates the map; a successful try_emplace may
         * allocate an unordered_map node, but the pre-reserved buckets avoid
         * a rehash and no full key string is copied. Node destruction is
         * deferred until after unlock. Because the generator ran outside the
         * lock, another thread may have inserted the same key in the meantime.
         * We detect that race via try_emplace and, if so, leave our node in
         * `staged` and return the existing entry (found = true). An evicted
         * victim is also spliced onto `staged` for deferred destruction.
         *
         * Hashes the key once: try_emplace performs the existence check and
         * the insert in a single map operation. The map key is a view into
         * the node's owned key storage, which splice never relocates.
         * Assumes the lock is already held by the caller.
         *
         * @param staged Single-node list holding the candidate; receives the
         *               discarded or evicted node, if any.
         */
        LruCacheResult<ValueType> insert_or_promote(std::list<Node>& staged) {
            const auto list_it = staged.begin();

            // No rollback needed if this throws: the candidate still lives in
            // `staged`, and neither the map nor the list has changed.
            const auto inserted =
                cache_map_.try_emplace(KeyTraits::map_key(list_it->key), list_it);

            if (!inserted.second) {
                // Lost the race: an identical key was inserted concurrently.
                // Leave our node in staged and promote the existing entry to
                // most-recently-used, reusing the op sequence the candidate
                // was stamped with.
                cache_list_.splice(cache_list_.begin(), cache_list_, inserted.first->second);
                inserted.first->second->last_promoted.store(
                    list_it->last_promoted.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                return LruCacheResult<ValueType>{inserted.first->second->value, true};
            }

            // Adopt the staged node at the MRU end: an O(1) pointer relink.
            cache_list_.splice(cache_list_.begin(), staged, list_it);

            // Evict the least recently used entry if over capacity. Only the
            // map erase happens here; the node itself — its key string and
            // value, possibly the last PreparedSqlRef — moves to staged and
            // is freed by the caller after unlock.
            if (cache_map_.size() > max_size_) {
                const auto victim = std::prev(cache_list_.end());
                cache_map_.erase(KeyTraits::map_key(victim->key));
                staged.splice(staged.end(), cache_list_, victim);
            }
            return LruCacheResult<ValueType>{list_it->value, false};
        }

        uint64_t next_op_seq() noexcept {
            return op_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        using MapType = std::unordered_map<typename KeyTraits::MapKey,
                                          typename std::list<Node>::iterator,
                                          typename KeyTraits::Hash,
                                          typename KeyTraits::Equal>;
        std::list<Node> cache_list_{};
        MapType cache_map_{};
        const size_t max_size_{};
        // A full-cache hit is promoted only once the entry has aged past this
        // many insert/promotion operations; younger entries are still far from
        // the eviction end, so their hits stay pure shared-lock reads.
        const uint64_t promote_age_threshold_{};
        // Counts inserts and promotions; entry age is measured against it.
        std::atomic<uint64_t> op_seq_{0};
        mutable std::shared_mutex mutex_{};
    };

    // Shared default for every sharded cache below. 16 shards spread unrelated
    // hot keys across independent shared_mutex instances, while a 1024-entry
    // cache still leaves each shard a useful 64-entry LRU slice.
    inline constexpr size_t kDefaultCacheShardCount = 16;

    /**
     * @brief Hash-sharded front over N independent LruCacheImpl instances.
     *
     * Ordinary hits on one LruCacheImpl can share its read lock, but every hit
     * still acquires and releases the same shared_mutex and may wait behind a
     * writer. Splitting the key space over independent shards reduces that
     * shared synchronization point for unrelated hot keys.
     *
     * The key is hashed ONCE per operation: the shard index derives from the
     * hash (see shard_for()), and the same hash rides into the shard's
     * unordered_map via the hash-carrying key (InnerTraits' pass-through
     * hasher), so the key bytes are never scanned twice.
     *
     * ShardTraits contract:
     *   LookupKey               caller-facing key type
     *   InnerTraits             KeyTraits for the per-shard LruCacheImpl, whose
     *                           LookupKey carries a precomputed hash
     *   size_t hash(LookupKey)  full key hash, computed once per operation
     *   InnerTraits::LookupKey with_hash(LookupKey, size_t)
     *                           attaches the hash for the shard's map
     *
     * @tparam ValueType   Type of values stored in the cache.
     * @tparam ShardTraits See contract above.
     */
    template<typename ValueType, typename ShardTraits>
    class ShardedLruCache {
    public:
        using LookupKey = typename ShardTraits::LookupKey;

        // Total capacity stays max_size: it is split across shards (remainder
        // spread over the first shards), so a hot shard evicts within its own
        // slice — the accepted trade-off for removing the shared lock line.
        // The shard count is clamped to the entry count so no shard ends up
        // with capacity 0.
        explicit ShardedLruCache(size_t max_size, size_t shard_count) {
            const size_t entry_count = max_size > 0 ? max_size : 1;
            const size_t requested_shards = shard_count > 0 ? shard_count : 1;
            const size_t actual_shards = std::min(requested_shards, entry_count);
            const size_t base_size = entry_count / actual_shards;
            const size_t remainder = entry_count % actual_shards;

            shards_.reserve(actual_shards);
            for (size_t i = 0; i < actual_shards; ++i) {
                const size_t capacity = base_size + (i < remainder ? 1 : 0);
                shards_.emplace_back(std::make_unique<Shard>(capacity));
            }
        }
        ~ShardedLruCache() = default;

        // Delete copy and move operations (shards hold mutexes)
        ShardedLruCache(const ShardedLruCache&) = delete;
        ShardedLruCache& operator=(const ShardedLruCache&) = delete;
        ShardedLruCache(ShardedLruCache&&) = delete;
        ShardedLruCache& operator=(ShardedLruCache&&) = delete;

        /**
         * @brief Retrieves or creates a cache entry in the key's shard.
         *
         * @param key The key to look up (hashed once, no allocation on hit).
         * @param generator Function to generate a new value if key not found.
         * @return Result containing the value and whether it was found.
         */
        template<typename Generator>
        LruCacheResult<ValueType> get(LookupKey key, Generator&& generator) {
            const size_t hash = ShardTraits::hash(key);
            return shard_for(hash).get(ShardTraits::with_hash(key, hash),
                                       std::forward<Generator>(generator));
        }

        /**
         * @brief Removes an entry from the cache.
         *
         * Derives the shard from the same hash as get(): removal must evict
         * from the shard that owns the entry, or a stale entry would keep
         * serving its old value after an invalidation (for the id caches that
         * would break metadata re-registration with the collector).
         *
         * @param key The key to remove.
         */
        void remove(LookupKey key) {
            const size_t hash = ShardTraits::hash(key);
            shard_for(hash).remove(ShardTraits::with_hash(key, hash));
        }

        size_t shardCount() const noexcept {
            return shards_.size();
        }

    private:
        using ShardCache = LruCacheImpl<ValueType, typename ShardTraits::InnerTraits>;

        // Shards live behind unique_ptr: LruCacheImpl is immovable (it owns a
        // shared_mutex), so the vector could not hold it by value.
        struct Shard {
            explicit Shard(size_t capacity) : cache(capacity) {}
            ShardCache cache;
        };

        ShardCache& shard_for(size_t hash) noexcept {
            // Pick the shard from a remixed value, not `hash % size`: the
            // shard's unordered_map is fed this same `hash` (pass-through
            // hasher), so selecting the shard from the low bits would leave
            // every key in a shard sharing those bits. Harmless with prime
            // bucket counts (libstdc++/libc++) but 16x chain length with
            // power-of-two buckets (MSVC). The multiply spreads high entropy
            // into the bits the modulo consumes.
            const size_t shard_idx =
                (hash * 0x9E3779B97F4A7C15ull >> 32) % shards_.size();
            return shards_[shard_idx]->cache;
        }

        std::vector<std::unique_ptr<Shard>> shards_;
    };

    struct RawSqlCacheKey {
        std::string_view raw_sql;
        uint64_t metadata_epoch;
        size_t hash;
    };

    struct RawSqlCacheStoredKey {
        std::string raw_sql;
        uint64_t metadata_epoch;
        size_t hash;
    };

    struct RawSqlCacheKeyHash {
        size_t operator()(const RawSqlCacheKey& key) const noexcept {
            return key.hash;
        }
    };

    struct RawSqlCacheKeyEqual {
        bool operator()(const RawSqlCacheKey& lhs, const RawSqlCacheKey& rhs) const noexcept {
            return lhs.metadata_epoch == rhs.metadata_epoch && lhs.raw_sql == rhs.raw_sql;
        }
    };

    struct RawSqlCacheKeyTraits {
        using LookupKey = RawSqlCacheKey;
        using StoredKey = RawSqlCacheStoredKey;
        using MapKey = RawSqlCacheKey;
        using Hash = RawSqlCacheKeyHash;
        using Equal = RawSqlCacheKeyEqual;

        static MapKey lookup_key(LookupKey key) noexcept {
            return key;
        }

        static StoredKey store(LookupKey key) {
            return RawSqlCacheStoredKey{
                std::string(key.raw_sql), key.metadata_epoch, key.hash};
        }

        static MapKey map_key(const StoredKey& key) noexcept {
            return RawSqlCacheKey{
                key.raw_sql, key.metadata_epoch, key.hash};
        }
    };

    struct RawSqlCacheShardTraits {
        // Raw SQL lookups key on (text, metadata epoch): advancing the epoch
        // makes every entry cached under the old one unreachable (see
        // RawSqlCacheKeyEqual and removeCacheSql in agent.cpp).
        struct LookupKey {
            std::string_view raw_sql;
            uint64_t metadata_epoch;
        };
        using InnerTraits = RawSqlCacheKeyTraits;

        static size_t hash(LookupKey key) noexcept {
            size_t hash = std::hash<std::string_view>{}(key.raw_sql);
            hash ^= std::hash<uint64_t>{}(key.metadata_epoch) + 0x9e3779b9 +
                    (hash << 6) + (hash >> 2);
            return hash;
        }

        static RawSqlCacheKey with_hash(LookupKey key, size_t hash) noexcept {
            return RawSqlCacheKey{key.raw_sql, key.metadata_epoch, hash};
        }
    };

    using RawSqlCacheResult = LruCacheResult<PreparedSqlRef>;

    /**
     * Front cache keyed by raw SQL. The cache is sharded so unrelated hot SQL
     * statements do not contend on one shared_mutex; the shard selection,
     * capacity split, and hash-once contract live in ShardedLruCache.
     */
    class RawSqlCache {
    public:
        explicit RawSqlCache(size_t max_size,
                             size_t shard_count = kDefaultCacheShardCount,
                             size_t max_cacheable_length = 64 * 1024)
            : cache_(max_size, shard_count),
              max_cacheable_length_(max_cacheable_length) {}

        RawSqlCache(const RawSqlCache&) = delete;
        RawSqlCache& operator=(const RawSqlCache&) = delete;
        RawSqlCache(RawSqlCache&&) = delete;
        RawSqlCache& operator=(RawSqlCache&&) = delete;

        template<typename Generator>
        RawSqlCacheResult get(std::string_view raw_sql,
                              uint64_t metadata_epoch,
                              Generator&& generator) {
            // Very large statements remain correct but bypass the cache so one
            // pathological key cannot pin an excessive amount of memory.
            if (raw_sql.size() > max_cacheable_length_) {
                return RawSqlCacheResult{generator(), false};
            }
            return cache_.get(
                RawSqlCacheShardTraits::LookupKey{raw_sql, metadata_epoch},
                std::forward<Generator>(generator));
        }

        size_t shardCount() const noexcept {
            return cache_.shardCount();
        }

    private:
        ShardedLruCache<PreparedSqlRef, RawSqlCacheShardTraits> cache_;
        const size_t max_cacheable_length_;
    };

    /**
     * @brief LRU cache that assigns numeric identifiers to frequently used keys.
     *
     * The cache is used for API, SQL, and error metadata to minimize payload
     * sizes when sending data over gRPC. Every sampled span and every named
     * span event resolves its api id here, so the store is sharded (see
     * ShardedLruCache): a single LruCacheImpl would put every request thread's
     * shared-lock acquire/release on one shared_mutex cache line.
     *
     * The id sequence is deliberately ONE atomic shared by all shards, not a
     * per-shard counter: the collector keys metadata by id, so two shards
     * handing the same id to different keys would corrupt the mapping. The
     * counter is only touched on a miss, so it is not a hit-path hot spot.
     */
    template<typename ShardTraits>
    class IdCacheImpl {
    public:
        using LookupKey = typename ShardTraits::LookupKey;

        explicit IdCacheImpl(size_t max_size,
                             size_t shard_count = kDefaultCacheShardCount)
            : cache_(max_size, shard_count) {}
        ~IdCacheImpl() = default;

        // Delete copy and move operations
        IdCacheImpl(const IdCacheImpl&) = delete;
        IdCacheImpl& operator=(const IdCacheImpl&) = delete;
        IdCacheImpl(IdCacheImpl&&) = delete;
        IdCacheImpl& operator=(IdCacheImpl&&) = delete;

        /**
         * @brief Looks up or inserts a key identifier.
         *
         * @param key Key to cache (no allocation on cache hit).
         * @return CacheResult containing the identifier and whether the entry already existed.
         */
        CacheResult get(LookupKey key) {
            return cache_.get(key, [this]() {
                return ++id_sequence_;
            });
        }

        /**
         * @brief Evicts a cached key from the cache.
         *
         * Routes to the shard get() uses for the key, so the next get() is a
         * guaranteed miss that mints a fresh id — the miss path is what
         * re-enqueues the metadata to the collector after a connection reset.
         *
         * @param key Entry to remove.
         */
        void remove(LookupKey key) {
            cache_.remove(key);
        }

        size_t shardCount() const noexcept {
            return cache_.shardCount();
        }

    private:
        ShardedLruCache<int32_t, ShardTraits> cache_;
        std::atomic<int32_t> id_sequence_{0};
    };

    using IdCache = IdCacheImpl<StringCacheShardTraits>;
    using ApiIdCache = IdCacheImpl<ApiCacheShardTraits>;

    /**
     * @brief LRU cache that assigns binary UIDs to normalized SQL statements.
     *
     * Sharded like IdCacheImpl: when the raw front cache is disabled this is
     * hit once per SQL statement. UIDs are content hashes of the key, so no
     * cross-shard state is needed for them to stay consistent.
     */
    class SqlUidCache {
    public:
        explicit SqlUidCache(size_t max_size,
                             size_t shard_count = kDefaultCacheShardCount)
            : cache_(max_size, shard_count) {}
        ~SqlUidCache() = default;

        // Delete copy and move operations
        SqlUidCache(const SqlUidCache&) = delete;
        SqlUidCache& operator=(const SqlUidCache&) = delete;
        SqlUidCache(SqlUidCache&&) = delete;
        SqlUidCache& operator=(SqlUidCache&&) = delete;

        /**
         * @brief Looks up or inserts an SQL UID entry.
         *
         * @param key Normalized SQL string (no allocation on cache hit).
         * @return Cache result containing UID bytes and whether the entry existed.
         */
        SqlUidCacheResult get(std::string_view key);

        /**
         * @brief Removes a cached SQL UID entry.
         *
         * @param key Normalized SQL string.
         */
        void remove(std::string_view key) {
            cache_.remove(key);
        }

        size_t shardCount() const noexcept {
            return cache_.shardCount();
        }

    private:
        ShardedLruCache<SqlUid, StringCacheShardTraits> cache_;
    };

} // namespace pinpoint
