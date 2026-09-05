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
#include <chrono>
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
     * Immutable result of normalizing one raw SQL statement. RawSqlCache
     * returns shared ownership so annotations can refer to the extracted
     * parameters without copying them, even after the cache entry itself is
     * evicted.
     *
     * The normalized text is retained, and deliberately NOT the id/uid: an
     * entry that cached the identity would have to be invalidated whenever a
     * metadata send failed, and no reverse index maps a normalized statement
     * back to the raw variants that produced it — so invalidation could only
     * be done wholesale. Resolving the identity per use instead costs one
     * shared-lock lookup in the id/uid cache and keeps one failed send from
     * touching any other entry. Matches the Go agent's raw cache.
     */
    struct PreparedSql {
        std::string parameters;
        std::string normalized_sql;
    };

    using PreparedSqlRef = std::shared_ptr<const PreparedSql>;

    /// @brief A prepared statement plus the identity resolved for this use.
    struct PreparedSqlResult {
        PreparedSqlRef sql;
        SqlIdentity identity;
    };

    /**
     * @brief Generic LRU cache result.
     *
     * @tparam ValueType Type of the cached value.
     */
    template<typename ValueType>
    struct LruCacheResult {
        ValueType value;
        bool found;  // true if entry already existed in cache
    };

    /// @brief Result returned from `IdCache::get`.
    using CacheResult = LruCacheResult<int32_t>;

    /**
     * @brief Optional write-time expiry for a cache.
     *
     * A `ttl` of zero (the default) disables expiry, which is what every cache
     * but the SQL UID one wants: their entries only need to outlive the
     * collector-side records they publish. `clock` is injectable so tests can
     * cross a multi-day ttl without sleeping. steady_clock, not system_clock:
     * an NTP step backwards must not resurrect an entry that already expired.
     */
    struct CacheExpiry {
        using Clock = std::chrono::steady_clock;

        Clock::duration ttl{Clock::duration::zero()};
        // Read once per lookup while ttl is enabled. The indirect call is paid
        // only by the cache that opts into expiry.
        std::function<Clock::time_point()> clock{&Clock::now};
    };

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

    /**
     * Hash-carrying twins of the string/api cache keys: the sharded caches
     * hash a key once per lookup — to pick the shard — and hand that same hash
     * to the shard's unordered_map through these keys (pass-through hasher),
     * so the key bytes are never scanned a second time.
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
        using Hash = HashedStringCacheKeyHash;
        using Equal = HashedStringCacheKeyEqual;

        static StoredKey store(LookupKey key) {
            return HashedStringCacheStoredKey{std::string(key.str), key.hash};
        }

        static LookupKey map_key(const StoredKey& key) noexcept {
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
        using Hash = HashedApiCacheKeyHash;
        using Equal = HashedApiCacheKeyEqual;

        static StoredKey store(LookupKey key) {
            return HashedApiCacheStoredKey{std::string(key.api_str), key.api_type, key.hash};
        }

        static LookupKey map_key(const StoredKey& key) noexcept {
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

    /// @brief Result returned from `SqlUidCache::get`. SqlUid is a fixed
    ///        16-byte array, so copying one out on a hit is allocation-free.
    using SqlUidCacheResult = LruCacheResult<SqlUid>;

    /**
     * @brief Thread-safe LRU cache implementation template.
     *
     * Combines a std::list (LRU ordering) with a std::unordered_map keyed by a
     * non-owning map key from KeyTraits (average O(1) lookup, no key storage
     * allocated on a hit). Guarded by a std::shared_mutex, so lookups run
     * concurrently under a shared lock. LRU reordering is lazy: below max_size
     * nothing can be evicted so ordering is irrelevant, and once full an entry
     * is promoted only after aging past half the capacity's churn (see get()),
     * keeping most hits shared-lock reads instead of serializing on the
     * exclusive lock the splice needs.
     *
     * @tparam ValueType Type of values stored in the cache.
     * @tparam KeyTraits Converts lookup keys into owned storage and map keys.
     */
    template<typename ValueType, typename KeyTraits>
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
            // Expiry deadline stamped at insert, and re-stamped when an
            // expired entry is refreshed. Written under the exclusive lock
            // only, like `value` beside it, so a plain member suffices — the
            // shared lock readers below cannot overlap that write.
            CacheExpiry::Clock::time_point expires_at;

            Node(typename KeyTraits::StoredKey&& k, ValueType&& v, uint64_t seq,
                 CacheExpiry::Clock::time_point deadline)
                : key(std::move(k)), value(std::move(v)), last_promoted(seq),
                  expires_at(deadline) {}
        };

    public:
        using LookupKey = typename KeyTraits::LookupKey;

        // max_size is clamped to >= 1: with a capacity of 0 the eviction in
        // insert_or_promote() would erase the just-inserted front node and
        // then return a reference into the freed list node — use-after-free.
        explicit LruCacheImpl(size_t max_size, CacheExpiry expiry = {})
            : max_size_(max_size > 0 ? max_size : 1),
              promote_age_threshold_(max_size_ / 2 > 0 ? max_size_ / 2 : 1),
              expiry_(std::move(expiry)) {
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
         * @brief Retrieves or creates a cache entry (no allocation on hit).
         *
         * Below capacity nothing can be evicted, so the splice is skipped and a
         * hit is a pure shared-lock read. Once full, promoting on every hit
         * would serialize all lookups on the exclusive lock the splice needs,
         * so promotion is aged: an entry promoted within the last
         * promote_age_threshold_ operations is still far from the eviction end
         * and stays a shared-lock read. On a miss the generator runs OUTSIDE
         * any lock, so an expensive one does not serialize other lookups.
         */
        template<typename Generator>
        LruCacheResult<ValueType> get(LookupKey key, Generator&& generator) {
            bool hit_while_full = false;
            {
                // Fast path: a shared lock lets concurrent hits proceed in parallel.
                std::shared_lock<std::shared_mutex> lock(mutex_);
                const auto it = cache_map_.find(key);
                // An expired entry is skipped, not erased: erasing needs the
                // exclusive lock and would serialize the hot path. It is
                // refreshed on the write path below (see insert_or_promote),
                // or evicted in LRU order like any other cold entry.
                if (it != cache_map_.end() && !expired(*it->second)) {
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
                const auto it = cache_map_.find(key);
                if (it != cache_map_.end() && !expired(*it->second)) {
                    cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
                    it->second->last_promoted.store(next_op_seq(), std::memory_order_relaxed);
                    return LruCacheResult<ValueType>{it->second->value, true};
                }
            }

            // Cache miss: run the (potentially expensive) generator WITHOUT holding
            // the lock. For SqlUidCache this is a MurmurHash over the full SQL text,
            // which would otherwise serialize every concurrent lookup.
            auto new_value = generator();

            // Stage the new node outside the lock too: the list-node allocation
            // and KeyTraits::store()'s key copy (for RawSqlCache the full raw
            // SQL, up to 64 KB) would otherwise stall every concurrent
            // shared-lock hit behind malloc + memcpy. Taking the op sequence
            // here can reorder slightly against promotions racing under the
            // lock, perturbing an age estimate by one step — the same benign
            // race the shared-lock age check tolerates. `staged` doubles as the
            // graveyard: a node discarded on a lost insert race and the evicted
            // victim are spliced back onto it, so they also die after unlock.
            std::list<Node> staged;
            staged.emplace_front(KeyTraits::store(key), std::move(new_value),
                                 next_op_seq(), next_deadline());

            std::unique_lock<std::shared_mutex> lock(mutex_);
            return insert_or_promote(staged);
        }

        /**
         * @brief Removes an entry, but only while it still holds @p expected.
         *
         * Removal is a release of a value whose metadata never reached the
         * collector, and it can land arbitrarily late — after retries, a retry
         * delay, or a queue drop. By then the entry may have been evicted and
         * a fresh one inserted under the same key, whose metadata is perfectly
         * healthy; erasing that one would burn a new id and re-publish for
         * nothing. Comparing the value first confines the release to the entry
         * that actually failed.
         *
         * @param key The key to remove.
         * @param expected Value the entry must still hold to be removed.
         */
        void remove(LookupKey key, const ValueType& expected) {
            // Declared before the lock so the removed node (key storage and
            // value) is destroyed after the exclusive section, not while
            // shared-lock readers wait behind the free.
            std::list<Node> removed;
            std::unique_lock<std::shared_mutex> lock(mutex_);

            const auto it = cache_map_.find(key);
            if (it != cache_map_.end() && it->second->value == expected) {
                removed.splice(removed.begin(), cache_list_, it->second);
                cache_map_.erase(it);
            }
        }

    private:
        /**
         * @brief Inserts the staged node, or promotes an existing entry.
         *        Assumes the caller holds the lock.
         *
         * get() pre-builds the candidate node and its owned key bytes in
         * `staged`, outside the lock; this critical section only relinks list
         * pointers and updates the map (try_emplace may allocate a map node,
         * but pre-reserved buckets avoid a rehash and no key string is copied).
         * Because the generator ran unlocked, another thread may have inserted
         * the same key meanwhile — try_emplace detects that, and our node stays
         * in `staged` while the existing entry is returned (found = true). An
         * evicted victim is spliced onto `staged` too, so both die after unlock.
         *
         * Hashes the key once: try_emplace checks and inserts in one operation.
         * The map key views the node's owned storage, which splice never moves.
         */
        LruCacheResult<ValueType> insert_or_promote(std::list<Node>& staged) {
            const auto list_it = staged.begin();

            // No rollback needed if this throws: the candidate still lives in
            // `staged`, and neither the map nor the list has changed.
            const auto inserted =
                cache_map_.try_emplace(KeyTraits::map_key(list_it->key), list_it);

            if (!inserted.second) {
                // The key is already present: either an identical key was
                // inserted concurrently (we lost the race), or get() routed an
                // expired entry here. Leave our node in staged and promote the
                // existing entry to most-recently-used, reusing the op sequence
                // the candidate was stamped with.
                const auto existing = inserted.first->second;
                cache_list_.splice(cache_list_.begin(), cache_list_, existing);
                existing->last_promoted.store(
                    list_it->last_promoted.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                if (expired(*existing)) {
                    // The one place an expired entry is actually retired: take
                    // the freshly generated value and restart its ttl. Reported
                    // as a miss, which is what makes the caller re-publish the
                    // metadata the collector may since have aged out.
                    existing->value = std::move(list_it->value);
                    existing->expires_at = list_it->expires_at;
                    return LruCacheResult<ValueType>{existing->value, false};
                }
                return LruCacheResult<ValueType>{existing->value, true};
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

        bool has_ttl() const noexcept {
            return expiry_.ttl > CacheExpiry::Clock::duration::zero();
        }

        bool expired(const Node& node) const {
            return has_ttl() && expiry_.clock() >= node.expires_at;
        }

        CacheExpiry::Clock::time_point next_deadline() const {
            return has_ttl() ? expiry_.clock() + expiry_.ttl
                             : CacheExpiry::Clock::time_point::max();
        }

        using MapType = std::unordered_map<LookupKey,
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
        const CacheExpiry expiry_{};
        mutable std::shared_mutex mutex_{};
    };

    // Shared default for every sharded cache below. 16 shards spread unrelated
    // hot keys across independent shared_mutex instances, while a 1024-entry
    // cache still leaves each shard a useful 64-entry LRU slice.
    inline constexpr size_t kDefaultCacheShardCount = 16;

    // Key length at or above which the SQL caches below bypass storage
    // entirely (see Config::sql.cache_length_limit / Sql.CacheLengthLimit).
    // Bounds their memory by entries x limit instead of by the largest
    // statement ever seen. kNoCacheLengthLimit disables the bypass.
    inline constexpr size_t kNoCacheLengthLimit = static_cast<size_t>(-1);
    inline constexpr size_t kDefaultCacheLengthLimit = 2048;

    /**
     * @brief Hash-sharded front over N independent LruCacheImpl instances.
     *
     * Hits on one LruCacheImpl share its read lock, but every hit still
     * acquires the same shared_mutex and may wait behind a writer. Splitting
     * the key space over independent shards reduces that contention point for
     * unrelated hot keys.
     *
     * The key is hashed ONCE per operation: the shard index derives from the
     * hash (see shard_for()), and the same hash rides into the shard's map via
     * the hash-carrying key (InnerTraits' pass-through hasher).
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
        explicit ShardedLruCache(size_t max_size, size_t shard_count,
                                 CacheExpiry expiry = {}) {
            const size_t entry_count = max_size > 0 ? max_size : 1;
            const size_t requested_shards = shard_count > 0 ? shard_count : 1;
            const size_t actual_shards = std::min(requested_shards, entry_count);
            const size_t base_size = entry_count / actual_shards;
            const size_t remainder = entry_count % actual_shards;

            shards_.reserve(actual_shards);
            for (size_t i = 0; i < actual_shards; ++i) {
                const size_t capacity = base_size + (i < remainder ? 1 : 0);
                shards_.emplace_back(std::make_unique<Shard>(capacity, expiry));
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
         * @brief Removes an entry that still holds @p expected (see
         *        LruCacheImpl::remove for why the value is compared).
         *
         * Derives the shard from the same hash as get(): removal must hit the
         * shard that owns the entry, or a stale one keeps serving its old value
         * after an invalidation — for the id caches, breaking metadata
         * re-registration with the collector.
         */
        void remove(LookupKey key, const ValueType& expected) {
            const size_t hash = ShardTraits::hash(key);
            shard_for(hash).remove(ShardTraits::with_hash(key, hash), expected);
        }

        size_t shardCount() const noexcept {
            return shards_.size();
        }

    private:
        using ShardCache = LruCacheImpl<ValueType, typename ShardTraits::InnerTraits>;

        // Shards live behind unique_ptr: LruCacheImpl is immovable (it owns a
        // shared_mutex), so the vector could not hold it by value.
        struct Shard {
            Shard(size_t capacity, const CacheExpiry& expiry)
                : cache(capacity, expiry) {}
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

    using RawSqlCacheResult = LruCacheResult<PreparedSqlRef>;

    /**
     * Front cache keyed by raw SQL, holding the normalization result only (see
     * PreparedSql). The cache is sharded so unrelated hot SQL statements do not
     * contend on one shared_mutex; the shard selection, capacity split, and
     * hash-once contract live in ShardedLruCache.
     *
     * It has no remove(): entries carry no collector state, so nothing here
     * ever needs invalidating.
     */
    class RawSqlCache {
    public:
        explicit RawSqlCache(size_t max_size,
                             size_t shard_count = kDefaultCacheShardCount,
                             size_t cache_length_limit = kDefaultCacheLengthLimit)
            : cache_(max_size, shard_count),
              cache_length_limit_(cache_length_limit) {}

        RawSqlCache(const RawSqlCache&) = delete;
        RawSqlCache& operator=(const RawSqlCache&) = delete;
        RawSqlCache(RawSqlCache&&) = delete;
        RawSqlCache& operator=(RawSqlCache&&) = delete;

        template<typename Generator>
        RawSqlCacheResult get(std::string_view raw_sql, Generator&& generator) {
            // Long statements remain correct but bypass the cache so one
            // pathological key cannot pin an excessive amount of memory. Only
            // normalization is repaid here; the caller resolves the id/uid
            // through the inner cache either way, which keeps its own entry.
            if (raw_sql.size() >= cache_length_limit_) {
                return RawSqlCacheResult{generator(), false};
            }
            return cache_.get(raw_sql, std::forward<Generator>(generator));
        }

    private:
        ShardedLruCache<PreparedSqlRef, StringCacheShardTraits> cache_;
        const size_t cache_length_limit_;
    };

    /**
     * @brief LRU cache that assigns numeric identifiers to frequently used keys.
     *
     * Used for API, SQL and error metadata to keep gRPC payloads small. Every
     * sampled span and named span event resolves its api id here, so the store
     * is sharded (see ShardedLruCache): a single LruCacheImpl would put every
     * request thread's shared-lock traffic on one shared_mutex cache line.
     *
     * The id sequence is deliberately ONE atomic shared by all shards: the
     * collector keys metadata by id, so two shards handing the same id to
     * different keys would corrupt the mapping. Only touched on a miss.
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
         * @brief Evicts a cached key that still maps to @p expected_id.
         *
         * Routes to the shard get() uses, so the next get() is a guaranteed
         * miss that mints a fresh id — the miss path is what re-enqueues the
         * metadata to the collector after a connection reset. The id is
         * compared first so a late release cannot evict a re-inserted entry
         * that was never the one that failed (see LruCacheImpl::remove).
         */
        void remove(LookupKey key, int32_t expected_id) {
            cache_.remove(key, expected_id);
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
     * Sharded like IdCacheImpl: it is hit once per SQL statement. UIDs are
     * content hashes of the key, so no cross-shard state is needed for them to
     * stay consistent.
     *
     * Unlike the id caches this one expires (see CacheExpiry and
     * Config::sql.cache_expire_hours). A cache hit suppresses re-publication of
     * the UID metadata, so an entry that never expires means the collector's
     * SqlUidMetaData row is written exactly once — and that row has a TTL of
     * its own (180 days). A process outliving it would keep emitting UIDs whose
     * SQL text the collector no longer has, and the UI would show empty SQL
     * until a restart. Expiring on write, as Java does at 168 hours
     * (profiler.jdbc.sqlcacheexpirehours), re-publishes in time to refresh it.
     */
    class SqlUidCache {
    public:
        explicit SqlUidCache(size_t max_size,
                             size_t shard_count = kDefaultCacheShardCount,
                             size_t cache_length_limit = kDefaultCacheLengthLimit,
                             CacheExpiry expiry = {})
            : cache_(max_size, shard_count, std::move(expiry)),
              cache_length_limit_(cache_length_limit) {}
        ~SqlUidCache() = default;

        // Delete copy and move operations
        SqlUidCache(const SqlUidCache&) = delete;
        SqlUidCache& operator=(const SqlUidCache&) = delete;
        SqlUidCache(SqlUidCache&&) = delete;
        SqlUidCache& operator=(SqlUidCache&&) = delete;

        /**
         * @brief Looks up or inserts an SQL UID entry.
         *
         * Statements at or above the length limit are never stored: the UID is
         * a content hash, so recomputing it is free of side effects and yields
         * the same bytes. `found` stays false for them, which makes the caller
         * re-send the UID metadata on every use — the same trade Java's
         * UidCache bypass makes.
         *
         * @param key Normalized SQL string (no allocation on cache hit).
         * @return Cache result containing UID bytes and whether the entry existed.
         */
        SqlUidCacheResult get(std::string_view key) {
            if (key.size() >= cache_length_limit_) {
                return SqlUidCacheResult{generate_sql_uid(key), false};
            }
            return cache_.get(key, [&key]() {
                return generate_sql_uid(key);
            });
        }

        /**
         * @brief Removes a cached SQL UID entry still holding @p expected_uid
         *        (see LruCacheImpl::remove for why the value is compared).
         *
         * @param key Normalized SQL string.
         * @param expected_uid UID the entry must still hold.
         */
        void remove(std::string_view key, const SqlUid& expected_uid) {
            cache_.remove(key, expected_uid);
        }

        size_t shardCount() const noexcept {
            return cache_.shardCount();
        }

    private:
        ShardedLruCache<SqlUid, StringCacheShardTraits> cache_;
        const size_t cache_length_limit_;
    };

} // namespace pinpoint
