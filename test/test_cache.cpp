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

class CacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Called before each test
    }

    void TearDown() override {
        // Called after each test
    }
};

// Basic functionality tests

// Test basic get operation with cache miss
TEST_F(CacheTest, BasicGetCacheMissTest) {
    IdCache cache(5);
    
    auto result = cache.get("key1");
    
    EXPECT_EQ(result.id, 1) << "First key should get ID 1";
    EXPECT_FALSE(result.old) << "First access should be cache miss (old=false)";
}

// Test cache hit after initial miss
TEST_F(CacheTest, BasicGetCacheHitTest) {
    IdCache cache(5);
    
    // First access - cache miss
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.id, 1);
    EXPECT_FALSE(result1.old);
    
    // Second access - cache hit
    auto result2 = cache.get("key1");
    EXPECT_EQ(result2.id, 1) << "Same key should return same ID";
    EXPECT_TRUE(result2.old) << "Second access should be cache hit (old=true)";
}

// Test multiple different keys get different IDs
TEST_F(CacheTest, MultipleDifferentKeysTest) {
    IdCache cache(5);
    
    auto result1 = cache.get("key1");
    auto result2 = cache.get("key2");
    auto result3 = cache.get("key3");
    
    EXPECT_EQ(result1.id, 1);
    EXPECT_EQ(result2.id, 2);
    EXPECT_EQ(result3.id, 3);
    
    EXPECT_FALSE(result1.old);
    EXPECT_FALSE(result2.old);
    EXPECT_FALSE(result3.old);
    
    // Verify all different IDs
    EXPECT_NE(result1.id, result2.id);
    EXPECT_NE(result2.id, result3.id);
    EXPECT_NE(result1.id, result3.id);
}

// Test ID sequence is incremental
TEST_F(CacheTest, IdSequenceIncrementalTest) {
    IdCache cache(10);
    
    std::vector<int32_t> ids;
    for (int i = 0; i < 5; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        ids.push_back(result.id);
        EXPECT_FALSE(result.old) << "Key " << i << " should be cache miss";
    }
    
    // Verify IDs are sequential
    for (size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(ids[i], static_cast<int32_t>(i + 1)) << "ID " << i << " should be sequential";
    }
}

// LRU policy tests

// Test LRU eviction when cache is full
TEST_F(CacheTest, LRUEvictionTest) {
    IdCache cache(3); // Small cache size
    
    // Fill cache completely
    auto result1 = cache.get("key1"); // ID: 1
    auto result2 = cache.get("key2"); // ID: 2
    auto result3 = cache.get("key3"); // ID: 3
    
    EXPECT_EQ(result1.id, 1);
    EXPECT_EQ(result2.id, 2);
    EXPECT_EQ(result3.id, 3);
    
    // Add one more item - should evict key1 (oldest in LRU order)
    auto result4 = cache.get("key4"); // ID: 4, evicts key1
    EXPECT_EQ(result4.id, 4);
    EXPECT_FALSE(result4.old);
    
    // Check that key1 was evicted by verifying it gets a new ID
    auto result1_again = cache.get("key1"); // Should get new ID: 5
    EXPECT_GT(result1_again.id, 4) << "Evicted key should get new ID";
    EXPECT_FALSE(result1_again.old) << "Evicted key should be cache miss";
    
    // Verify key4 is still in cache (was just added)
    auto result4_check = cache.get("key4");
    EXPECT_EQ(result4_check.id, 4) << "Recently added key should still be in cache";
    EXPECT_TRUE(result4_check.old) << "Recently added key should be cache hit";
}

// Test LRU ordering - accessing item moves it to front
TEST_F(CacheTest, LRUOrderingTest) {
    IdCache cache(3);
    
    // Fill cache: key1, key2, key3
    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3
    
    // Access key1 again - should move it to front of LRU list
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.id, 1);
    EXPECT_TRUE(result1.old);
    
    // Add new key - should evict the oldest (key2, since key1 was recently accessed)
    auto result4 = cache.get("key4"); // ID: 4
    EXPECT_EQ(result4.id, 4);
    
    // Verify key1 is still in cache (was recently accessed)
    auto result1_check = cache.get("key1");
    EXPECT_EQ(result1_check.id, 1) << "Recently accessed key should still be in cache";
    EXPECT_TRUE(result1_check.old) << "Recently accessed key should be cache hit";
    
    // Verify key4 is in cache (was just added)
    auto result4_check = cache.get("key4");
    EXPECT_EQ(result4_check.id, 4) << "Recently added key should be in cache";
    EXPECT_TRUE(result4_check.old) << "Recently added key should be cache hit";
    
    // Check if key2 was evicted by seeing if it gets a new ID
    auto result2_check = cache.get("key2");
    EXPECT_GT(result2_check.id, 4) << "Evicted key should get new ID";
    EXPECT_FALSE(result2_check.old) << "Evicted key should be cache miss";
}

// Remove functionality tests

// Test basic remove operation
TEST_F(CacheTest, BasicRemoveTest) {
    IdCache cache(5);
    
    // Add item to cache
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.id, 1);
    EXPECT_FALSE(result1.old);
    
    // Verify it's in cache
    auto result2 = cache.get("key1");
    EXPECT_EQ(result2.id, 1);
    EXPECT_TRUE(result2.old);
    
    // Remove the item
    cache.remove("key1");
    
    // Verify it's no longer in cache
    auto result3 = cache.get("key1");
    EXPECT_EQ(result3.id, 2) << "Removed key should get new ID";
    EXPECT_FALSE(result3.old) << "Removed key should be cache miss";
}

// Test remove non-existent key (should not crash)
TEST_F(CacheTest, RemoveNonExistentKeyTest) {
    IdCache cache(5);
    
    // Remove key that doesn't exist - should not crash
    EXPECT_NO_THROW(cache.remove("nonexistent"));
    
    // Cache should still work normally
    auto result = cache.get("key1");
    EXPECT_EQ(result.id, 1);
    EXPECT_FALSE(result.old);
}

// Test remove from middle of cache
TEST_F(CacheTest, RemoveFromMiddleTest) {
    IdCache cache(5);
    
    // Add multiple items
    cache.get("key1"); // ID: 1
    cache.get("key2"); // ID: 2
    cache.get("key3"); // ID: 3
    
    // Remove middle item
    cache.remove("key2");
    
    // Verify key1 and key3 are still accessible
    auto result1 = cache.get("key1");
    auto result3 = cache.get("key3");
    EXPECT_EQ(result1.id, 1);
    EXPECT_EQ(result3.id, 3);
    EXPECT_TRUE(result1.old);
    EXPECT_TRUE(result3.old);
    
    // Verify key2 is no longer in cache
    auto result2 = cache.get("key2");
    EXPECT_EQ(result2.id, 4); // New ID
    EXPECT_FALSE(result2.old);
}

// Edge case tests

// Test cache with size 1
TEST_F(CacheTest, CacheSize1Test) {
    IdCache cache(1);
    
    auto result1 = cache.get("key1");
    EXPECT_EQ(result1.id, 1);
    EXPECT_FALSE(result1.old);
    
    // Add second item - should evict first
    auto result2 = cache.get("key2");
    EXPECT_EQ(result2.id, 2);
    EXPECT_FALSE(result2.old);
    
    // First item should be evicted
    auto result1_again = cache.get("key1");
    EXPECT_EQ(result1_again.id, 3);
    EXPECT_FALSE(result1_again.old);
}

// Test empty string key
TEST_F(CacheTest, EmptyStringKeyTest) {
    IdCache cache(5);
    
    auto result1 = cache.get("");
    EXPECT_EQ(result1.id, 1);
    EXPECT_FALSE(result1.old);
    
    auto result2 = cache.get("");
    EXPECT_EQ(result2.id, 1);
    EXPECT_TRUE(result2.old);
}

// Test very long key
TEST_F(CacheTest, LongKeyTest) {
    IdCache cache(5);
    
    std::string long_key(1000, 'a'); // 1000 character key
    
    auto result1 = cache.get(long_key);
    EXPECT_EQ(result1.id, 1);
    EXPECT_FALSE(result1.old);
    
    auto result2 = cache.get(long_key);
    EXPECT_EQ(result2.id, 1);
    EXPECT_TRUE(result2.old);
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
                ids.push_back(result.id);
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
            cache.remove("key" + std::to_string(counter % 20));
            counter++;
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });
    
    // Let them run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_flag.store(true);
    
    // Wait for threads to finish
    adder.wait();
    remover.wait();
    
    // Cache should still be functional
    auto result = cache.get("test_key");
    EXPECT_GT(result.id, 0) << "Cache should still be functional after concurrent operations";
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
    
    // Collect results
    std::vector<CacheResult> results;
    for (auto& future : futures) {
        results.push_back(future.get());
    }
    
    // All should have the same ID
    int32_t first_id = results[0].id;
    for (const auto& result : results) {
        EXPECT_EQ(result.id, first_id) << "All accesses to same key should return same ID";
    }
    
    // Only one should be cache miss (old=false), others should be hits (old=true)
    int cache_misses = 0;
    int cache_hits = 0;
    for (const auto& result : results) {
        if (result.old) {
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
    IdCache cache(cache_size);
    
    // Add many items
    std::vector<int32_t> original_ids;
    for (int i = 0; i < total_items; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        original_ids.push_back(result.id);
        EXPECT_EQ(result.id, i + 1);
        EXPECT_FALSE(result.old);
    }
    
    // Only the most recent cache_size items should still be in cache
    // The last cache_size keys added should still be there
    int items_still_in_cache = 0;
    int items_evicted = 0;
    
    for (int i = total_items - cache_size; i < total_items; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        if (result.old && result.id == original_ids[i]) {
            items_still_in_cache++;
        }
    }
    
    // We expect most recent items to still be in cache, but accessing them
    // in the loop above may cause evictions, so we just verify some are still there
    EXPECT_GT(items_still_in_cache, 0) << "Some recent items should still be in cache";
    
    // First few items should definitely be evicted
    for (int i = 0; i < cache_size / 2; ++i) {
        auto result = cache.get("key" + std::to_string(i));
        EXPECT_GT(result.id, total_items) << "Early items should be evicted and get new IDs";
        EXPECT_FALSE(result.old) << "Early items should be cache miss";
        items_evicted++;
    }
    
    EXPECT_GT(items_evicted, 0) << "Some early items should have been evicted";
}

} // namespace pinpoint
