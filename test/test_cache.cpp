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

#include "../src/cache.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <vector>
#include <future>
#include <set>
#include <string>

namespace pinpoint {

class CacheTest : public ::testing::Test {};

namespace {
    PreparedSqlRef prepared_sql(std::string parameters,
                                std::string normalized_sql = "SELECT 0#") {
        return std::make_shared<const PreparedSql>(PreparedSql{
            std::move(parameters), std::move(normalized_sql)});
    }
}

TEST_F(CacheTest, RawSqlCacheHitSkipsGeneratorAndReturnsSameImmutableEntry) {
    RawSqlCache cache(16, 4);
    int generator_calls = 0;
    auto generator = [&] {
        ++generator_calls;
        return prepared_sql("42", "SELECT * FROM users WHERE id = 0#");
    };

    auto first = cache.get("SELECT * FROM users WHERE id = 42", generator);
    auto second = cache.get("SELECT * FROM users WHERE id = 42", generator);

    ASSERT_NE(first.value, nullptr);
    EXPECT_FALSE(first.found);
    EXPECT_TRUE(second.found);
    EXPECT_EQ(generator_calls, 1);
    EXPECT_EQ(first.value, second.value);
    EXPECT_EQ(second.value->parameters, "42");
    EXPECT_EQ(second.value->normalized_sql, "SELECT * FROM users WHERE id = 0#");
}

TEST_F(CacheTest, RawSqlCacheReferenceSurvivesEviction) {
    RawSqlCache cache(1, 1);
    auto retained = cache.get("SELECT 1", [] {
        return prepared_sql("1");
    }).value;
    std::weak_ptr<const PreparedSql> weak = retained;

    cache.get("SELECT 2", [] {
        return prepared_sql("2");
    });

    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->parameters, "1");
    EXPECT_FALSE(weak.expired());
    retained.reset();
    EXPECT_TRUE(weak.expired());
}

TEST_F(CacheTest, RawSqlCacheOversizedStatementBypassesStorage) {
    RawSqlCache cache(16, 4, 8);
    int generator_calls = 0;
    auto generator = [&] {
        ++generator_calls;
        return prepared_sql("123456789");
    };

    auto first = cache.get("SELECT 123456789", generator);
    auto second = cache.get("SELECT 123456789", generator);

    EXPECT_FALSE(first.found);
    EXPECT_FALSE(second.found);
    EXPECT_EQ(generator_calls, 2);
}

// The limit is inclusive: a statement exactly at it bypasses storage, one
// byte under it is cached. Pins the boundary shared with SqlUidCache so the
// two cannot drift apart.
TEST_F(CacheTest, RawSqlCacheLengthLimitIsInclusive) {
    RawSqlCache cache(16, 4, 10);
    auto generator = [] { return prepared_sql("1"); };

    const std::string at_limit(10, 'a');
    EXPECT_FALSE(cache.get(at_limit, generator).found);
    EXPECT_FALSE(cache.get(at_limit, generator).found)
        << "a statement at the limit must never be stored";

    const std::string under_limit(9, 'a');
    EXPECT_FALSE(cache.get(under_limit, generator).found);
    EXPECT_TRUE(cache.get(under_limit, generator).found)
        << "a statement under the limit must still be cached";
}

// kNoCacheLengthLimit restores the pre-limit behaviour (Sql.CacheLengthLimit: -1).
TEST_F(CacheTest, RawSqlCacheNoLengthLimitCachesLongStatements) {
    RawSqlCache cache(16, 4, kNoCacheLengthLimit);
    auto generator = [] { return prepared_sql("1"); };

    const std::string long_sql(3000, 'a');
    EXPECT_FALSE(cache.get(long_sql, generator).found);
    EXPECT_TRUE(cache.get(long_sql, generator).found);
}

TEST_F(CacheTest, RawSqlCacheConcurrentSameKeyPublishesOneEntry) {
    RawSqlCache cache(64, 8);
    std::atomic<int> generator_calls{0};
    std::vector<std::future<PreparedSqlRef>> futures;

    for (int i = 0; i < 16; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            return cache.get("SELECT * FROM concurrent WHERE id = 7", [&] {
                generator_calls.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
                return prepared_sql("7");
            }).value;
        }));
    }

    std::vector<PreparedSqlRef> results;
    for (auto& future : futures) {
        results.push_back(future.get());
    }

    ASSERT_NE(results.front(), nullptr);
    for (const auto& result : results) {
        EXPECT_EQ(result, results.front());
    }
    EXPECT_GE(generator_calls.load(std::memory_order_relaxed), 1);
}

// Basic functionality tests

// Test basic get operation with cache miss
TEST_F(CacheTest, BasicGetCacheMissTest) {
    IdCache cache(5);
    
    auto result = cache.get("key1");
    
    EXPECT_EQ(result.value, 1) << "First key should get ID 1";
    EXPECT_FALSE(result.found) << "First access should be cache miss (found=false)";
}

// Test cache hit after initial miss
TEST_F(CacheTest, BasicGetCacheHitTest) {
    IdCache cache(5);
    
    // First access - cache miss
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.value, 1);
    EXPECT_FALSE(result1.found);
    
    // Second access - cache hit
    auto result2 = cache.get("key1");
    EXPECT_EQ(result2.value, 1) << "Same key should return same ID";
    EXPECT_TRUE(result2.found) << "Second access should be cache hit (found=true)";
}

// Test multiple different keys get different IDs
TEST_F(CacheTest, MultipleDifferentKeysTest) {
    IdCache cache(5);
    
    auto result1 = cache.get("key1");
    auto result2 = cache.get("key2");
    auto result3 = cache.get("key3");
    
    EXPECT_EQ(result1.value, 1);
    EXPECT_EQ(result2.value, 2);
    EXPECT_EQ(result3.value, 3);
    
    EXPECT_FALSE(result1.found);
    EXPECT_FALSE(result2.found);
    EXPECT_FALSE(result3.found);
    
    // Verify all different IDs
    EXPECT_NE(result1.value, result2.value);
    EXPECT_NE(result2.value, result3.value);
    EXPECT_NE(result1.value, result3.value);
}

TEST_F(CacheTest, IdSequenceIncrementalTest) {
    IdCache cache(10);
    
    std::vector<int32_t> ids;
    for (int i = 0; i < 5; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        ids.push_back(result.value);
        EXPECT_FALSE(result.found) << "Key " << i << " should be cache miss";
    }
    
    // Verify IDs are sequential
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(ids[i], static_cast<int32_t>(i + 1)) << "ID " << i << " should be sequential";
    }
}

// LRU policy tests

// Test LRU eviction when cache is full
//
// This and the other LRU-order tests pin shard_count=1: with more shards each
// shard evicts within its own capacity slice, so a global eviction order is
// no longer observable. shard_count=1 is also the degenerate configuration
// that must behave exactly like the pre-sharding cache.
TEST_F(CacheTest, LRUEvictionTest) {
    IdCache cache(3, 1); // Small cache size, single shard
    
    // Fill cache completely
    auto result1 = cache.get("key1"); // ID: 1
    auto result2 = cache.get("key2"); // ID: 2
    auto result3 = cache.get("key3"); // ID: 3
    
    EXPECT_EQ(result1.value, 1);
    EXPECT_EQ(result2.value, 2);
    EXPECT_EQ(result3.value, 3);
    
    // Add one more item - should evict key1 (oldest in LRU order)
    auto result4 = cache.get("key4"); // ID: 4, evicts key1
    EXPECT_EQ(result4.value, 4);
    EXPECT_FALSE(result4.found);
    
    // Check that key1 was evicted by verifying it gets a new ID
    auto result1_again = cache.get("key1"); // Should get new ID: 5
    EXPECT_GT(result1_again.value, 4) << "Evicted key should get new ID";
    EXPECT_FALSE(result1_again.found) << "Evicted key should be cache miss";
    
    // Verify key4 is still in cache (was just added)
    auto result4_check = cache.get("key4");
    EXPECT_EQ(result4_check.value, 4) << "Recently added key should still be in cache";
    EXPECT_TRUE(result4_check.found) << "Recently added key should be cache hit";
}

// Test LRU ordering - accessing item moves it to front
TEST_F(CacheTest, LRUOrderingTest) {
    IdCache cache(3, 1);
    
    // Fill cache: key1, key2, key3
    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3
    
    // Access key1 again - should move it to front of LRU list
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.value, 1);
    EXPECT_TRUE(result1.found);
    
    // Add new key - should evict the oldest (key2, since key1 was recently accessed)
    auto result4 = cache.get("key4"); // ID: 4
    EXPECT_EQ(result4.value, 4);
    
    // Verify key1 is still in cache (was recently accessed)
    auto result1_check = cache.get("key1");
    EXPECT_EQ(result1_check.value, 1) << "Recently accessed key should still be in cache";
    EXPECT_TRUE(result1_check.found) << "Recently accessed key should be cache hit";
    
    // Verify key4 is in cache (was just added)
    auto result4_check = cache.get("key4");
    EXPECT_EQ(result4_check.value, 4) << "Recently added key should be in cache";
    EXPECT_TRUE(result4_check.found) << "Recently added key should be cache hit";
    
    // Check if key2 was evicted by seeing if it gets a new ID
    auto result2_check = cache.get("key2");
    EXPECT_GT(result2_check.value, 4) << "Evicted key should get new ID";
    EXPECT_FALSE(result2_check.found) << "Evicted key should be cache miss";
}

// Test aged promotion: a full-cache hit on a recently inserted entry skips the
// splice (pure shared-lock read), while a hit on an aged entry promotes it so
// it survives subsequent evictions
TEST_F(CacheTest, AgedPromotionTest) {
    IdCache cache(4, 1);  // single shard; promotion age threshold = 2 ops

    cache.get("key1"); // op 1
    cache.get("key2"); // op 2
    cache.get("key3"); // op 3
    cache.get("key4"); // op 4, cache now full

    // key4 is age 0 (< threshold): hit returns without promotion
    auto r4 = cache.get("key4");
    EXPECT_TRUE(r4.found);
    EXPECT_EQ(r4.value, 4);

    // key1 is age 3 (>= threshold): hit promotes it to MRU (op 5)
    auto r1 = cache.get("key1");
    EXPECT_TRUE(r1.found);
    EXPECT_EQ(r1.value, 1);

    // Two inserts evict the two oldest entries: key2, then key3
    cache.get("key5"); // op 6, evicts key2
    cache.get("key6"); // op 7, evicts key3

    // key1 survived thanks to the promotion; key2 and key3 are gone
    auto r1_check = cache.get("key1");
    EXPECT_TRUE(r1_check.found) << "Promoted aged entry should survive eviction";
    EXPECT_EQ(r1_check.value, 1);
    EXPECT_FALSE(cache.get("key2").found) << "Oldest unpromoted entry should be evicted";
    EXPECT_FALSE(cache.get("key3").found) << "Second-oldest unpromoted entry should be evicted";
}

// Guards the aged-promotion optimization against a silent revert to strict LRU.
// AgedPromotionTest above only checks that a young full-cache hit returns without
// an immediate reorder AND that an aged hit survives — both of which also hold
// under strict "promote on every full-cache hit". The distinguishing behavior is
// that a hit on a still-young entry does NOT protect it: because the splice is
// skipped, later insertions evict it in its original LRU position. A revert to
// strict LRU would promote it on the hit and let it survive, failing this test.
TEST_F(CacheTest, AgedPromotionYoungHitDoesNotProtectEntryTest) {
    IdCache cache(4, 1);  // single shard; promote_age_threshold_ = 4/2 = 2 ops

    cache.get("key1"); // op 1
    cache.get("key2"); // op 2
    cache.get("key3"); // op 3
    cache.get("key4"); // op 4, cache now full; order MRU..LRU: key4,key3,key2,key1

    // Hit key3 while it is young: age = op_seq(4) - key3.last_promoted(3) = 1,
    // which is < threshold(2), so the hit is a pure read and does NOT promote it.
    auto r3 = cache.get("key3");
    EXPECT_TRUE(r3.found) << "the hit itself still resolves from cache";
    EXPECT_EQ(r3.value, 3);

    // Three inserts evict the three LRU entries in order: key1, key2, then key3.
    // Because the hit above did not promote key3, it is still in eviction range.
    cache.get("key5"); // op 5, evicts key1
    cache.get("key6"); // op 6, evicts key2
    cache.get("key7"); // op 7, evicts key3

    EXPECT_FALSE(cache.get("key3").found)
        << "a young (unpromoted) entry must remain evictable; under strict LRU the "
           "earlier hit would have promoted key3 and it would have survived";
    // key4, hit by nobody but inserted last of the originals, was also not
    // promoted, so it too is gone by now; key5/key6 survive as the newest.
    EXPECT_TRUE(cache.get("key6").found) << "recent insert should still be cached";
}

// A capacity of 0 is clamped to 1 (see LruCacheImpl ctor). Without the clamp,
// insert_or_promote() would erase the just-inserted front node and then return a
// reference into the freed list node — a use-after-free. This exercises every
// cache flavor at capacity 0; under a sanitizer it also catches a dropped clamp.
TEST_F(CacheTest, ZeroCapacityIsClampedToOneTest) {
    IdCache id_cache(0);
    auto a = id_cache.get("a");
    EXPECT_FALSE(a.found) << "first lookup is a miss";
    EXPECT_NE(a.value, 0) << "a valid id is still assigned at capacity 0";
    // A second distinct key evicts the first (capacity is effectively 1) but must
    // not crash or corrupt the cache.
    auto b = id_cache.get("b");
    EXPECT_FALSE(b.found);
    auto a_again = id_cache.get("a");
    EXPECT_FALSE(a_again.found) << "capacity-1 cache evicted the first key";

    ApiIdCache api_cache(0);
    auto p = api_cache.get(ApiCacheKey{"op", 100});
    EXPECT_FALSE(p.found);
    EXPECT_NE(p.value, 0);
    EXPECT_FALSE(api_cache.get(ApiCacheKey{"op", 200}).found);

    SqlUidCache uid_cache(0);
    auto u = uid_cache.get("SELECT 1");
    EXPECT_EQ(u.value.size(), 16u);
    EXPECT_FALSE(u.found);
    EXPECT_FALSE(uid_cache.get("SELECT 2").found);
}

// Remove functionality tests

// Test basic remove operation
TEST_F(CacheTest, BasicRemoveTest) {
    IdCache cache(5);
    
    // Add item to cache
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.value, 1);
    EXPECT_FALSE(result1.found);
    
    auto result2 = cache.get("key1");
    EXPECT_EQ(result2.value, 1);
    EXPECT_TRUE(result2.found);
    
    // Remove the item
    cache.remove("key1", result2.value);
    
    // Verify it's no longer in cache
    auto result3 = cache.get("key1");
    EXPECT_EQ(result3.value, 2) << "Removed key should get new ID";
    EXPECT_FALSE(result3.found) << "Removed key should be cache miss";
}

// Test remove non-existent key (should not crash)
TEST_F(CacheTest, RemoveNonExistentKeyTest) {
    IdCache cache(5);
    
    // Remove key that doesn't exist - should not crash
    EXPECT_NO_THROW(cache.remove("nonexistent", 1));
    
    // Cache should still work normally
    auto result = cache.get("key1");
    EXPECT_EQ(result.value, 1);
    EXPECT_FALSE(result.found);
}

// Test remove from middle of cache
TEST_F(CacheTest, RemoveFromMiddleTest) {
    IdCache cache(5, 1);
    
    // Add multiple items
    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3
    
    // Remove middle item
    cache.remove("key2", 2);
    
    // Verify key1 and key3 are still accessible
    auto result1 = cache.get("key1");
    auto result3 = cache.get("key3");
    EXPECT_EQ(result1.value, 1);
    EXPECT_EQ(result3.value, 3);
    EXPECT_TRUE(result1.found);
    EXPECT_TRUE(result3.found);
    
    // Verify key2 is no longer in cache
    auto result2 = cache.get("key2");
    EXPECT_EQ(result2.value, 4); // New ID
    EXPECT_FALSE(result2.found);
}

// Edge case tests

TEST_F(CacheTest, CacheSize1Test) {
    IdCache cache(1);
    
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.value, 1);
    EXPECT_FALSE(result1.found);
    
    // Add second item - should evict first
    auto result2 = cache.get("key2");
    EXPECT_EQ(result2.value, 2);
    EXPECT_FALSE(result2.found);
    
    // First item should be evicted
    auto result1_again = cache.get("key1");
    EXPECT_EQ(result1_again.value, 3);
    EXPECT_FALSE(result1_again.found);
}

TEST_F(CacheTest, EmptyStringKeyTest) {
    IdCache cache(5);
    
    auto result1 = cache.get("");
    EXPECT_EQ(result1.value, 1);
    EXPECT_FALSE(result1.found);
    
    auto result2 = cache.get("");
    EXPECT_EQ(result2.value, 1);
    EXPECT_TRUE(result2.found);
}

// Test very long key
TEST_F(CacheTest, LongKeyTest) {
    IdCache cache(5);
    
    std::string long_key(1000, 'a'); // 1000 character key
    
    auto result1 = cache.get(long_key);
    EXPECT_EQ(result1.value, 1);
    EXPECT_FALSE(result1.found);
    
    auto result2 = cache.get(long_key);
    EXPECT_EQ(result2.value, 1);
    EXPECT_TRUE(result2.found);
}

// Thread safety tests

// Test concurrent get operations
TEST_F(CacheTest, ConcurrentGetTest) {
    IdCache cache(100);
    const int num_threads = 10;
    const int operations_per_thread = 100;
    
    std::vector<std::future<std::vector<int32_t>>> futures;
    
    // Launch multiple threads performing get operations
    for (int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&cache, i]() {
            std::vector<int32_t> ids;
            for (int j = 0; j < operations_per_thread; ++j) {
                std::string key = "thread" + std::to_string(i) + "_key" + std::to_string(j);
                auto result = cache.get(key);
                ids.push_back(result.value);
            }
            return ids;
        }));
    }
    
    // Collect all IDs
    std::set<int32_t> all_ids;
    for (auto& future : futures) {
        auto ids = future.get();
        for (auto id : ids) {
            all_ids.insert(id);
        }
    }
    
    // All IDs should be unique (since all keys are unique)
    int expected_count = num_threads * operations_per_thread;
    EXPECT_EQ(all_ids.size(), expected_count) << "All IDs should be unique";
    
    // IDs should be in range [1, expected_count]
    EXPECT_EQ(*all_ids.begin(), 1) << "Smallest ID should be 1";
    EXPECT_EQ(*all_ids.rbegin(), expected_count) << "Largest ID should be " << expected_count;
}

// Test concurrent get/remove operations
TEST_F(CacheTest, ConcurrentGetRemoveTest) {
    IdCache cache(50);
    std::atomic<bool> stop_flag(false);
    
    // Thread 1: Continuously add items
    std::future<void> adder = std::async(std::launch::async, [&cache, &stop_flag]() {
        int counter = 0;
        while (!stop_flag.load()) {
            cache.get("key" + std::to_string(counter % 20));
            counter++;
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });
    
    // Thread 2: Continuously remove items
    std::future<void> remover = std::async(std::launch::async, [&cache, &stop_flag]() {
        int counter = 0;
        while (!stop_flag.load()) {
            // Read the live id back so the guarded remove actually fires;
            // when the adder thread slips in between, it no-ops instead —
            // which is the guard doing its job.
            const auto key = "key" + std::to_string(counter % 20);
            cache.remove(key, cache.get(key).value);
            counter++;
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });
    
    // Let them run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_flag.store(true);
    
    adder.wait();
    remover.wait();
    
    // Cache should still be functional
    auto result = cache.get("test_key");
    EXPECT_GT(result.value, 0) << "Cache should still be functional after concurrent operations";
}

// Test concurrent access to same key
TEST_F(CacheTest, ConcurrentSameKeyTest) {
    IdCache cache(10);
    const int num_threads = 5;
    const std::string shared_key = "shared_key";
    
    std::vector<std::future<CacheResult>> futures;
    
    // Launch multiple threads accessing the same key
    for (int i = 0; i < num_threads; ++i) {
        futures.push_back(std::async(std::launch::async, [&cache, shared_key]() {
            return cache.get(shared_key);
        }));
    }
    
    std::vector<CacheResult> results;
    for (auto& future : futures) {
        results.push_back(future.get());
    }
    
    // All should have the same ID
    int32_t first_id = results[0].value;
    for (const auto& result : results) {
        EXPECT_EQ(result.value, first_id) << "All accesses to same key should return same ID";
    }
    
    // Only one should be cache miss (found=false), others should be hits (found=true)
    int cache_misses = 0;
    int cache_hits = 0;
    for (const auto& result : results) {
        if (result.found) {
            cache_hits++;
        } else {
            cache_misses++;
        }
    }
    
    EXPECT_EQ(cache_misses, 1) << "Exactly one access should be cache miss";
    EXPECT_EQ(cache_hits, num_threads - 1) << "Other accesses should be cache hits";
}

// Performance and stress tests

// Test cache behavior with many items
TEST_F(CacheTest, ManyItemsTest) {
    const int cache_size = 10; // Use smaller cache size for clearer testing
    const int total_items = 25;
    IdCache cache(cache_size, 1);
    
    // Add many items
    std::vector<int32_t> original_ids;
    for (int i = 0; i < total_items; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        original_ids.push_back(result.value);
        EXPECT_EQ(result.value, i + 1);
        EXPECT_FALSE(result.found);
    }
    
    // Only the most recent cache_size items should still be in cache
    // The last cache_size keys added should still be there
    int items_still_in_cache = 0;
    int items_evicted = 0;
    
    for (int i = total_items - cache_size; i < total_items; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        if (result.found && result.value == original_ids[i]) {
            items_still_in_cache++;
        }
    }
    
    // We expect most recent items to still be in cache, but accessing them
    // in the loop above may cause evictions, so we just verify some are still there
    EXPECT_GT(items_still_in_cache, 0) << "Some recent items should still be in cache";
    
    // First few items should definitely be evicted
    for (int i = 0; i < cache_size / 2; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        EXPECT_GT(result.value, total_items) << "Early items should be evicted and get new IDs";
        EXPECT_FALSE(result.found) << "Early items should be cache miss";
        items_evicted++;
    }
    
    EXPECT_GT(items_evicted, 0) << "Some early items should have been evicted";
}

// Test cache at exact capacity - no eviction should occur
TEST_F(CacheTest, ExactCapacityNoEvictionTest) {
    const int capacity = 5;
    IdCache cache(capacity, 1);

    // Fill cache to exact capacity
    for (int i = 0; i < capacity; ++i) {
        cache.get("key" + std::to_string(i));
    }

    // All items should still be in cache
    for (int i = 0; i < capacity; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        EXPECT_EQ(result.value, i + 1) << "Key " << i << " should retain original ID";
        EXPECT_TRUE(result.found) << "Key " << i << " should be cache hit";
    }
}

// Test removing all items and re-adding
TEST_F(CacheTest, RemoveAllAndReuseTest) {
    IdCache cache(5, 1);

    // Add 3 items
    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3

    // Remove all
    cache.remove("key1", 1);
    cache.remove("key2", 2);
    cache.remove("key3", 3);

    // Cache should be empty but functional
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.value, 4) << "Re-added key should get new ID";
    EXPECT_FALSE(result1.found) << "Re-added key should be cache miss";

    auto result2 = cache.get("key1");
    EXPECT_EQ(result2.value, 4) << "Same key should return same ID";
    EXPECT_TRUE(result2.found) << "Should be cache hit now";
}

// Test LRU eviction chain - verify correct eviction order
TEST_F(CacheTest, LRUEvictionChainTest) {
    IdCache cache(3, 1);

    // Fill: key1(LRU), key2, key3(MRU)
    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3

    // Add key4 -> evicts key1 (oldest). Cache: key2, key3, key4
    auto r4 = cache.get("key4"); // ID: 4
    EXPECT_FALSE(r4.found);

    // Verify key1 was evicted
    auto r1 = cache.get("key1"); // miss, re-inserted -> evicts key2. Cache: key3, key4, key1
    EXPECT_FALSE(r1.found) << "key1 should have been evicted";

    // Verify key2 was evicted (it was the new LRU after key1 eviction)
    auto r2 = cache.get("key2"); // miss, re-inserted -> evicts key3. Cache: key4, key1, key2
    EXPECT_FALSE(r2.found) << "key2 should have been evicted";

    // key4, key1, key2 should now be in cache
    // Verify key1 is still in cache
    auto r1_check = cache.get("key1");
    EXPECT_TRUE(r1_check.found) << "key1 should be in cache";
}

TEST_F(CacheTest, StringViewKeyFromTemporaryTest) {
    IdCache cache(5);

    {
        std::string temp_key = "temporary_key_data";
        cache.get(temp_key);
        // temp_key goes out of scope here
    }

    // Key should still be found - internal storage must own the string
    auto result = cache.get("temporary_key_data");
    EXPECT_EQ(result.value, 1) << "Should find the same entry";
    EXPECT_TRUE(result.found) << "Should be cache hit even after source string destroyed";
}

// Test remove and re-add places item at MRU position
TEST_F(CacheTest, RemoveAndReaddLRUPositionTest) {
    IdCache cache(3, 1);

    // Fill: key1, key2, key3
    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3

    // Remove key1 and re-add it (now it's MRU)
    cache.remove("key1", 1);
    cache.get("key1"); // ID: 4, now MRU

    // Add key4 -> should evict key2 (oldest remaining)
    cache.get("key4"); // ID: 5
    auto r2 = cache.get("key2");
    EXPECT_FALSE(r2.found) << "key2 should be evicted (was LRU)";

    // key1 should still be in cache (was recently re-added)
    auto r1 = cache.get("key1");
    EXPECT_EQ(r1.value, 4);
    EXPECT_TRUE(r1.found) << "key1 should be in cache (was MRU after re-add)";
}

// Test keys with special characters and Unicode
TEST_F(CacheTest, SpecialCharacterKeysTest) {
    // Single shard: three keys must coexist, which sharding cannot guarantee
    // at this capacity (each shard would hold a single entry).
    IdCache cache(10, 1);

    std::string unicode_key = "키_한글_テスト";
    std::string special_key = "key!@#$%^&*()";
    std::string null_key = std::string("key\0with\0nulls", 14);

    auto r1 = cache.get(unicode_key);
    auto r2 = cache.get(special_key);
    auto r3 = cache.get(null_key);

    EXPECT_FALSE(r1.found);
    EXPECT_FALSE(r2.found);
    EXPECT_FALSE(r3.found);

    // All should be distinct entries
    EXPECT_NE(r1.value, r2.value);
    EXPECT_NE(r2.value, r3.value);

    // Verify cache hits
    EXPECT_TRUE(cache.get(unicode_key).found);
    EXPECT_TRUE(cache.get(special_key).found);
    EXPECT_TRUE(cache.get(null_key).found);
}

// API cache tests

TEST_F(CacheTest, ApiIdCacheKeepsTypesDistinctTest) {
    ApiIdCache cache(5, 1);

    auto result1 = cache.get(ApiCacheKey{"operation", 100});
    auto result2 = cache.get(ApiCacheKey{"operation", 200});

    EXPECT_FALSE(result1.found);
    EXPECT_FALSE(result2.found);
    EXPECT_NE(result1.value, result2.value) << "Same API string with different types should be distinct";

    auto result1_again = cache.get(ApiCacheKey{"operation", 100});
    EXPECT_EQ(result1_again.value, result1.value);
    EXPECT_TRUE(result1_again.found);
}

TEST_F(CacheTest, ApiIdCacheRemoveOnlyMatchingTypeTest) {
    ApiIdCache cache(5, 1);

    auto result1 = cache.get(ApiCacheKey{"operation", 100});
    auto result2 = cache.get(ApiCacheKey{"operation", 200});

    cache.remove(ApiCacheKey{"operation", 100}, result1.value);

    auto removed = cache.get(ApiCacheKey{"operation", 100});
    EXPECT_FALSE(removed.found);
    EXPECT_NE(removed.value, result1.value);

    auto retained = cache.get(ApiCacheKey{"operation", 200});
    EXPECT_TRUE(retained.found);
    EXPECT_EQ(retained.value, result2.value);
}

TEST_F(CacheTest, ApiIdCacheStringViewKeyFromTemporaryTest) {
    ApiIdCache cache(5);

    {
        std::string temp_key = "temporary_operation";
        cache.get(ApiCacheKey{temp_key, 100});
    }

    auto result = cache.get(ApiCacheKey{"temporary_operation", 100});
    EXPECT_EQ(result.value, 1);
    EXPECT_TRUE(result.found);
}

TEST_F(CacheTest, ApiIdCacheLRUEvictionTest) {
    ApiIdCache cache(2, 1);

    cache.get(ApiCacheKey{"operation1", 100});
    cache.get(ApiCacheKey{"operation2", 100});
    cache.get(ApiCacheKey{"operation1", 100});
    cache.get(ApiCacheKey{"operation3", 100});

    EXPECT_TRUE(cache.get(ApiCacheKey{"operation1", 100}).found);
    EXPECT_FALSE(cache.get(ApiCacheKey{"operation2", 100}).found);
}

// Sharded id cache tests
//
// The id caches shard their store (ShardedLruCache) so per-span lookups do
// not all hammer one shared_mutex cache line. These tests pin the contracts
// sharding must preserve: one stable id per key, ids unique across shards,
// remove() routing to the shard get() uses (the metadata re-registration
// path), and the total capacity bound.

TEST_F(CacheTest, ShardedIdCacheReportsShardCountTest) {
    IdCache cache(1024, 16);
    EXPECT_EQ(cache.shardCount(), 16u);

    // The shard count is clamped to the entry count so no shard ends up with
    // capacity 0.
    IdCache clamped(4, 16);
    EXPECT_EQ(clamped.shardCount(), 4u);

    IdCache single(1024, 1);
    EXPECT_EQ(single.shardCount(), 1u);
}

TEST_F(CacheTest, ShardedIdCacheSameKeyAlwaysSameIdTest) {
    IdCache cache(1024, 16);

    const auto first = cache.get("com.example.Service.method()");
    EXPECT_FALSE(first.found);

    // Same key must route to the same shard every time: any flip-flop in the
    // shard selection would show up as a spurious miss minting a new id.
    for (int i = 0; i < 64; ++i) {
        const auto again = cache.get("com.example.Service.method()");
        EXPECT_TRUE(again.found);
        EXPECT_EQ(again.value, first.value);
    }
}

TEST_F(CacheTest, ShardedIdCacheConcurrentDistinctKeysUniqueIdsTest) {
    IdCache cache(4096, 16);
    constexpr int num_threads = 8;
    constexpr int keys_per_thread = 500;

    std::vector<std::future<std::vector<int32_t>>> futures;
    for (int t = 0; t < num_threads; ++t) {
        futures.push_back(std::async(std::launch::async, [&cache, t]() {
            std::vector<int32_t> ids;
            ids.reserve(keys_per_thread);
            for (int k = 0; k < keys_per_thread; ++k) {
                ids.push_back(cache.get("thread" + std::to_string(t) +
                                        "/key" + std::to_string(k)).value);
            }
            return ids;
        }));
    }

    std::set<int32_t> all_ids;
    for (auto& future : futures) {
        for (const auto id : future.get()) {
            all_ids.insert(id);
        }
    }

    // One id sequence is shared by all shards: per-shard sequences would hand
    // the same id to different keys. Every key is distinct and requested only
    // once, so the ids must be exactly 1..N — no duplicates and no gaps.
    constexpr int expected = num_threads * keys_per_thread;
    EXPECT_EQ(all_ids.size(), static_cast<size_t>(expected)) << "duplicate ids across shards";
    EXPECT_EQ(*all_ids.begin(), 1);
    EXPECT_EQ(*all_ids.rbegin(), expected);
}

TEST_F(CacheTest, ShardedIdCacheRemoveMintsFreshIdTest) {
    // 1024/16 = 64 entries per shard >= the 33 inserts below, so no shard can
    // evict no matter how the keys distribute — keeps the test deterministic.
    IdCache cache(1024, 16);
    constexpr int key_count = 32;

    std::vector<int32_t> ids;
    for (int i = 0; i < key_count; ++i) {
        ids.push_back(cache.get("api/key" + std::to_string(i)).value);
    }

    cache.remove("api/key7", ids[7]);

    // The metadata re-registration contract: after remove(), the next get()
    // must be a miss that mints a fresh id — that miss is what re-enqueues
    // the metadata to the collector after a connection reset.
    const auto readded = cache.get("api/key7");
    EXPECT_FALSE(readded.found);
    EXPECT_EQ(readded.value, key_count + 1);

    // remove() must have routed to the shard get() uses and evicted only that
    // key: every other key still hits with its original id.
    for (int i = 0; i < key_count; ++i) {
        if (i == 7) continue;
        const auto result = cache.get("api/key" + std::to_string(i));
        EXPECT_TRUE(result.found) << "key " << i;
        EXPECT_EQ(result.value, ids[i]) << "key " << i;
    }
}

TEST_F(CacheTest, ShardedApiIdCacheRemoveRoutesToOwningShardTest) {
    // 33 total inserts <= the 64-entry per-shard slice, as above.
    ApiIdCache cache(1024, 16);
    constexpr int key_count = 16;

    std::vector<std::string> names;
    std::vector<int32_t> web_ids;
    std::vector<int32_t> default_ids;
    for (int i = 0; i < key_count; ++i) {
        names.push_back("com.example.Service.method" + std::to_string(i));
        web_ids.push_back(cache.get(ApiCacheKey{names.back(), 100}).value);
        default_ids.push_back(cache.get(ApiCacheKey{names.back(), 200}).value);
    }

    cache.remove(ApiCacheKey{names[3], 100}, web_ids[3]);

    const auto readded = cache.get(ApiCacheKey{names[3], 100});
    EXPECT_FALSE(readded.found);
    EXPECT_NE(readded.value, web_ids[3]);

    // The same name under a different type is a different key (possibly in a
    // different shard) and must be untouched, as must every other entry.
    const auto sibling = cache.get(ApiCacheKey{names[3], 200});
    EXPECT_TRUE(sibling.found);
    EXPECT_EQ(sibling.value, default_ids[3]);
    for (int i = 0; i < key_count; ++i) {
        if (i == 3) continue;
        const auto web = cache.get(ApiCacheKey{names[i], 100});
        EXPECT_TRUE(web.found) << "key " << i;
        EXPECT_EQ(web.value, web_ids[i]) << "key " << i;
    }
}

TEST_F(CacheTest, ShardedIdCacheTotalCapacityBoundedTest) {
    constexpr size_t capacity = 64;
    IdCache cache(capacity, 16);

    constexpr int key_count = 1024;
    for (int i = 0; i < key_count; ++i) {
        cache.get("key" + std::to_string(i));
    }

    // Inspect newest to oldest. Within each shard, all resident keys were
    // inserted later than its evicted keys, so this order observes every
    // resident before the first miss can mutate that shard's contents.
    size_t hits = 0;
    for (int i = key_count - 1; i >= 0; --i) {
        if (cache.get("key" + std::to_string(i)).found) {
            ++hits;
        }
    }
    EXPECT_EQ(hits, capacity);
}

TEST_F(CacheTest, ShardCountOneDegeneratesToUnshardedBehaviorTest) {
    // shard_count=1 must behave exactly like the pre-sharding cache: one LRU
    // list over the full capacity with a strict global eviction order. (The
    // LRU and aged-promotion tests above all run in this configuration.)
    IdCache cache(3, 1);
    EXPECT_EQ(cache.shardCount(), 1u);

    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3
    cache.get("key4"); // ID: 4, evicts key1

    const auto evicted = cache.get("key1");
    EXPECT_FALSE(evicted.found);
    EXPECT_EQ(evicted.value, 5);

    const auto retained = cache.get("key4");
    EXPECT_TRUE(retained.found);
    EXPECT_EQ(retained.value, 4);
}

// SqlUidCache Test Suite
//
// SqlUidCache shares the sharded LRU machinery with IdCache — both
// instantiate ShardedLruCache over StringCacheShardTraits — so the LRU
// mechanics (eviction, ordering, remove, capacity bounds, concurrency) are
// already pinned by the CacheTest suite above. This suite covers only what
// is specific to SqlUidCache: the UID is a deterministic content hash of
// the SQL text.
class SqlUidCacheTest : public ::testing::Test {};

// Test cache hit after initial miss
TEST_F(SqlUidCacheTest, BasicGetCacheHitTest) {
    SqlUidCache cache(5);
    
    std::string sql = "SELECT * FROM users WHERE id = ?";
    
    // First access - cache miss
    auto result1 = cache.get(sql);
    EXPECT_EQ(result1.value.size(), 16);
    EXPECT_FALSE(result1.found);
    
    // Second access - cache hit
    auto result2 = cache.get(sql);
    EXPECT_EQ(result2.value.size(), 16) << "UID should be 16 bytes";
    EXPECT_EQ(result1.value, result2.value) << "Same key should return same UID";
    EXPECT_TRUE(result2.found) << "Second access should be cache hit (found=true)";
}

// Test same SQL always produces same UID
TEST_F(SqlUidCacheTest, ConsistentUidGenerationTest) {
    SqlUidCache cache1(10);
    SqlUidCache cache2(10);
    
    std::string sql = "SELECT * FROM users WHERE id = ? AND status = ?";
    
    auto result1 = cache1.get(sql);
    auto result2 = cache2.get(sql);
    
    EXPECT_EQ(result1.value, result2.value) << "Same SQL should always produce same UID across different cache instances";
}

// Test UID consistency across different cache instances
TEST_F(SqlUidCacheTest, UidConsistencyTest) {
    SqlUidCache cache1(5);
    SqlUidCache cache2(5);
    SqlUidCache cache3(5);
    
    std::vector<std::string> test_sqls = {
        "SELECT * FROM users WHERE id = ?",
        "INSERT INTO logs (message) VALUES (?)",
        "UPDATE products SET price = ? WHERE id = ?",
        "DELETE FROM sessions WHERE expires < ?",
        "SELECT COUNT(*) FROM orders WHERE status = ?"
    };
    
    // Get UIDs from all cache instances for each SQL
    for (const auto& sql : test_sqls) {
        auto uid1 = cache1.get(sql).value;
        auto uid2 = cache2.get(sql).value;
        auto uid3 = cache3.get(sql).value;
        
        EXPECT_EQ(uid1.size(), 16);
        EXPECT_EQ(uid2.size(), 16);
        EXPECT_EQ(uid3.size(), 16);
        
        EXPECT_EQ(uid1, uid2) << "Same SQL should get same UID across different cache instances";
        EXPECT_EQ(uid2, uid3) << "Same SQL should get same UID across different cache instances";
        EXPECT_EQ(uid1, uid3) << "Same SQL should get same UID across different cache instances";
    }
}

// A statement at or above Sql.CacheLengthLimit is never stored, so `found`
// stays false on every use. That flag is the exact predicate
// AgentImpl::cacheSqlUid enqueues the UID metadata on, so two misses here mean
// two metadata sends — the bounded-memory-for-repeated-metadata trade Java's
// UidCache bypass makes.
TEST_F(SqlUidCacheTest, LengthLimitBypassesStorageAndReportsEveryUseAsNew) {
    SqlUidCache cache(1024, 16, 2048);

    const std::string long_sql(3000, 'a');
    const auto first = cache.get(long_sql);
    const auto second = cache.get(long_sql);

    EXPECT_FALSE(first.found);
    EXPECT_FALSE(second.found) << "the entry must not have been cached";
    EXPECT_EQ(first.value, second.value)
        << "the UID is a content hash, so bypassing keeps it stable";
    EXPECT_EQ(first.value.size(), 16u);

    // Under the limit the cache still works as before.
    const std::string short_sql(2047, 'a');
    EXPECT_FALSE(cache.get(short_sql).found);
    EXPECT_TRUE(cache.get(short_sql).found);
}

// kNoCacheLengthLimit restores the pre-limit behaviour (Sql.CacheLengthLimit: -1).
TEST_F(SqlUidCacheTest, NoLengthLimitCachesLongStatements) {
    SqlUidCache cache(1024, 16, kNoCacheLengthLimit);

    const std::string long_sql(3000, 'a');
    EXPECT_FALSE(cache.get(long_sql).found);
    EXPECT_TRUE(cache.get(long_sql).found);
}

// Sharded UID cache: remove() must route by the same hash as get() so the
// entry is really evicted; the regenerated UID is identical because UIDs are
// a content hash of the SQL, so no cross-shard state is involved.
TEST_F(SqlUidCacheTest, ShardedSqlUidCacheRemoveEvictsEntryTest) {
    SqlUidCache cache(1024, 16);
    EXPECT_EQ(cache.shardCount(), 16u);

    const std::string sql = "SELECT * FROM users WHERE id = ?";
    const auto original = cache.get(sql);
    EXPECT_FALSE(original.found);
    EXPECT_TRUE(cache.get(sql).found);

    cache.remove(sql, original.value);

    const auto readded = cache.get(sql);
    EXPECT_FALSE(readded.found) << "remove() must evict from the owning shard";
    EXPECT_EQ(readded.value, original.value);
}

// A release carrying an id the entry no longer holds must not evict it: by the
// time metadata retries are exhausted the entry may have been evicted and
// re-registered, and dropping that one costs a fresh id and a re-send.
TEST_F(CacheTest, RemoveIgnoresStaleValue) {
    IdCache cache(5);
    const auto original = cache.get("key1");

    cache.remove("key1", original.value + 100);

    const auto after = cache.get("key1");
    EXPECT_TRUE(after.found) << "a mismatched value must leave the entry in place";
    EXPECT_EQ(after.value, original.value);
}

// The race the guard exists for, played out concretely: evict, re-insert under
// a fresh id, then let the old release land.
TEST_F(CacheTest, RemoveKeepsEntryReinsertedAfterEviction) {
    IdCache cache(1, 1);
    const auto evicted = cache.get("key1");
    cache.get("key2");  // capacity is 1, so this evicts key1
    const auto reinserted = cache.get("key1");
    ASSERT_NE(evicted.value, reinserted.value);

    cache.remove("key1", evicted.value);

    const auto after = cache.get("key1");
    EXPECT_TRUE(after.found);
    EXPECT_EQ(after.value, reinserted.value);
}

namespace {
    // Hands the cache a clock the test can wind forward, so a 168-hour ttl is
    // crossed without sleeping. Shared state, not a copy: CacheExpiry is copied
    // into every shard.
    struct TestClock {
        std::shared_ptr<CacheExpiry::Clock::time_point> now =
            std::make_shared<CacheExpiry::Clock::time_point>(
                CacheExpiry::Clock::now());

        CacheExpiry operator()(CacheExpiry::Clock::duration ttl) const {
            auto handle = now;
            return CacheExpiry{ttl, [handle] { return *handle; }};
        }
        void advance(CacheExpiry::Clock::duration by) const { *now += by; }
    };
}

// The SQL UID metadata row has a server-side TTL (180 days), and a cache hit
// suppresses re-publication — so an entry that never expires eventually points
// at SQL text the collector has dropped. Expiring on write re-sends in time.
TEST_F(SqlUidCacheTest, SqlUidCacheExpiresEntryAfterTtlSoMetadataIsResent) {
    const TestClock clock;
    SqlUidCache cache(1024, 4, kNoCacheLengthLimit,
                      clock(std::chrono::hours(168)));

    const std::string sql = "SELECT * FROM users WHERE id = 0#";
    const auto first = cache.get(sql);
    EXPECT_FALSE(first.found);
    EXPECT_TRUE(cache.get(sql).found);

    clock.advance(std::chrono::hours(167));
    EXPECT_TRUE(cache.get(sql).found) << "must not expire before the ttl";

    clock.advance(std::chrono::hours(1));
    const auto expired = cache.get(sql);
    EXPECT_FALSE(expired.found) << "an expired entry must re-publish its metadata";
    EXPECT_EQ(expired.value, first.value)
        << "the UID is a content hash, so it is unchanged by expiry";
    EXPECT_TRUE(cache.get(sql).found) << "the refreshed entry restarts its ttl";

    clock.advance(std::chrono::hours(168));
    EXPECT_FALSE(cache.get(sql).found) << "and expires again one ttl later";
}

// Expiry has to work once the cache is full too: that is where get() takes the
// promotion path instead of the plain shared-lock read.
TEST_F(SqlUidCacheTest, SqlUidCacheExpiresEntriesWhileFull) {
    const TestClock clock;
    SqlUidCache cache(4, 1, kNoCacheLengthLimit, clock(std::chrono::hours(168)));

    for (int i = 0; i < 4; ++i) {
        cache.get("SELECT " + std::to_string(i));
    }
    EXPECT_TRUE(cache.get("SELECT 3").found);

    clock.advance(std::chrono::hours(169));
    EXPECT_FALSE(cache.get("SELECT 3").found);
    EXPECT_TRUE(cache.get("SELECT 3").found);
}

// Expiry is opt-in: the default leaves entries in place forever, which is what
// the api/error/sql id caches rely on (Java gives them no TTL either).
TEST_F(SqlUidCacheTest, SqlUidCacheWithoutTtlNeverExpires) {
    SqlUidCache cache(1024, 4);

    const std::string sql = "SELECT 1";
    EXPECT_FALSE(cache.get(sql).found);
    EXPECT_TRUE(cache.get(sql).found);
}

} // namespace pinpoint
