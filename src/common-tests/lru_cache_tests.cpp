// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/lru_cache.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct TrackedValue
{
  TrackedValue(int value_, int* destruction_count_) : value(value_), destruction_count(destruction_count_) {}
  ~TrackedValue() { (*destruction_count)++; }

  int value;
  int* destruction_count;
};

struct PoolDeleter
{
  void operator()(std::unique_ptr<TrackedValue>&& value) const { pool->push_back(std::move(value)); }

  std::vector<std::unique_ptr<TrackedValue>>* pool;
};
} // namespace

TEST(LRUCache, InsertLookupClearAndCapacityAccessors)
{
  LRUCache<int, int> cache(2);
  EXPECT_EQ(cache.GetSize(), 0u);
  EXPECT_EQ(cache.GetMaxCapacity(), 2u);
  EXPECT_EQ(cache.Lookup(1), nullptr);

  int* value = cache.Insert(1, 10);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 10);
  EXPECT_EQ(cache.GetSize(), 1u);

  value = cache.Lookup(1);
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(*value, 10);

  cache.Clear();
  EXPECT_EQ(cache.GetSize(), 0u);
  EXPECT_EQ(cache.Lookup(1), nullptr);
}

TEST(LRUCache, LookupUpdatesLeastRecentlyUsedOrder)
{
  LRUCache<int, int> cache(2);
  cache.Insert(1, 10);
  cache.Insert(2, 20);
  ASSERT_NE(cache.Lookup(1), nullptr);

  cache.Insert(3, 30);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_NE(cache.Lookup(3), nullptr);
}

TEST(LRUCache, LookupDistinguishesMissingRawPointerFromCachedNullPointer)
{
  LRUCache<int, int*> cache(1);
  EXPECT_EQ(cache.Lookup(1), nullptr);

  int** inserted_value = cache.Insert(1, nullptr);
  ASSERT_NE(inserted_value, nullptr);
  EXPECT_EQ(*inserted_value, nullptr);

  int** cached_value = cache.Lookup(1);
  ASSERT_NE(cached_value, nullptr);
  EXPECT_EQ(*cached_value, nullptr);
}

TEST(LRUCache, SetMaxCapacityEvictsLeastRecentlyUsedItems)
{
  LRUCache<int, int> cache(3);
  cache.Insert(1, 10);
  cache.Insert(2, 20);
  cache.Insert(3, 30);
  ASSERT_NE(cache.Lookup(1), nullptr);

  cache.SetMaxCapacity(2);
  EXPECT_EQ(cache.GetMaxCapacity(), 2u);
  EXPECT_EQ(cache.GetSize(), 2u);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_NE(cache.Lookup(3), nullptr);

  cache.SetMaxCapacity(0);
  EXPECT_EQ(cache.GetMaxCapacity(), 0u);
  EXPECT_EQ(cache.GetSize(), 0u);
}

TEST(LRUCache, EvictRemovesRequestedNumberOfItems)
{
  LRUCache<int, int> cache(4);
  cache.Insert(1, 10);
  cache.Insert(2, 20);
  cache.Insert(3, 30);
  cache.Insert(4, 40);
  ASSERT_NE(cache.Lookup(1), nullptr);

  cache.Evict(0);
  EXPECT_EQ(cache.GetSize(), 4u);

  cache.Evict(2);
  EXPECT_EQ(cache.GetSize(), 2u);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_EQ(cache.Lookup(3), nullptr);
  EXPECT_NE(cache.Lookup(4), nullptr);

  cache.Evict(10);
  EXPECT_EQ(cache.GetSize(), 0u);
  cache.Evict();
  EXPECT_EQ(cache.GetSize(), 0u);
}

TEST(LRUCache, RemoveAndRemoveMatchingItems)
{
  LRUCache<int, int> cache(5);
  for (int i = 1; i <= 5; i++)
    cache.Insert(i, i * 10);

  EXPECT_FALSE(cache.Remove(6));
  EXPECT_TRUE(cache.Remove(1));
  EXPECT_FALSE(cache.Remove(1));
  EXPECT_EQ(cache.RemoveMatchingItems([](int key) { return (key % 2) == 0; }), 2u);
  EXPECT_EQ(cache.RemoveMatchingItems([](int) { return false; }), 0u);

  EXPECT_EQ(cache.GetSize(), 2u);
  EXPECT_NE(cache.Lookup(3), nullptr);
  EXPECT_NE(cache.Lookup(5), nullptr);
}

TEST(LRUCache, ManualEvictionCanTemporarilyExceedCapacity)
{
  LRUCache<int, int> cache(2, true);
  cache.Insert(1, 10);
  cache.Insert(2, 20);
  cache.Insert(3, 30);
  EXPECT_EQ(cache.GetSize(), 3u);

  ASSERT_NE(cache.Lookup(1), nullptr);
  cache.ManualEvict();
  EXPECT_EQ(cache.GetSize(), 2u);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.Lookup(2), nullptr);
  EXPECT_NE(cache.Lookup(3), nullptr);

  cache.SetManualEvict(true);
  cache.Insert(4, 40);
  EXPECT_EQ(cache.GetSize(), 3u);
  ASSERT_NE(cache.Lookup(1), nullptr);
  cache.SetManualEvict(false);
  EXPECT_EQ(cache.GetSize(), 2u);
  EXPECT_NE(cache.Lookup(1), nullptr);
  EXPECT_EQ(cache.Lookup(3), nullptr);
  EXPECT_NE(cache.Lookup(4), nullptr);
}

TEST(LRUCache, ApplyVisitsAndCanModifyEveryItem)
{
  LRUCache<int, int> cache(3);
  cache.Insert(3, 30);
  cache.Insert(1, 10);
  cache.Insert(2, 20);

  std::vector<int> visited_keys;
  cache.Apply([&visited_keys](const int& key, int& value) {
    visited_keys.push_back(key);
    value += key;
  });

  EXPECT_EQ(visited_keys, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(*cache.Lookup(1), 11);
  EXPECT_EQ(*cache.Lookup(2), 22);
  EXPECT_EQ(*cache.Lookup(3), 33);
}

TEST(LRUCache, StringKeysSupportHeterogeneousLookupAndRemoval)
{
  LRUCache<std::string, int> cache(2);
  cache.Insert("first", 1);
  cache.Insert("second", 2);

  const std::string_view first_key = "first";
  ASSERT_NE(cache.Lookup(first_key), nullptr);
  EXPECT_EQ(*cache.Lookup(first_key), 1);
  EXPECT_TRUE(cache.Remove(first_key));
  EXPECT_EQ(cache.Lookup(first_key), nullptr);
  EXPECT_EQ(cache.GetSize(), 1u);
}

TEST(LRUCache, DefaultDeleterDeletesRawPointers)
{
  int destruction_count = 0;
  {
    LRUCache<int, TrackedValue*> cache(1);
    cache.Insert(1, new TrackedValue(1, &destruction_count));
    cache.Insert(2, new TrackedValue(2, &destruction_count));
    EXPECT_EQ(destruction_count, 1);

    EXPECT_TRUE(cache.Remove(2));
    EXPECT_EQ(destruction_count, 2);
  }
  EXPECT_EQ(destruction_count, 2);
}

TEST(LRUCache, DefaultDeleterAllowsUniquePointersToDestroyNormally)
{
  int destruction_count = 0;
  {
    LRUCache<int, std::unique_ptr<TrackedValue>> cache(1);
    cache.Insert(1, std::make_unique<TrackedValue>(1, &destruction_count));
    cache.Insert(1, std::make_unique<TrackedValue>(2, &destruction_count));

    EXPECT_EQ(cache.GetSize(), 1u);
    EXPECT_EQ(destruction_count, 1);
  }
  EXPECT_EQ(destruction_count, 2);
}

TEST(LRUCache, CustomDeleterCanPoolUniquePointers)
{
  int destruction_count = 0;
  std::vector<std::unique_ptr<TrackedValue>> pool;
  {
    LRUCache<int, std::unique_ptr<TrackedValue>, PoolDeleter> cache(2, false, PoolDeleter{&pool});
    cache.Insert(1, std::make_unique<TrackedValue>(1, &destruction_count));
    cache.Insert(2, std::make_unique<TrackedValue>(2, &destruction_count));
    cache.Insert(1, std::make_unique<TrackedValue>(3, &destruction_count));

    ASSERT_EQ(pool.size(), 1u);
    EXPECT_EQ(pool.front()->value, 1);
    EXPECT_NE(cache.Lookup(2), nullptr);
    EXPECT_EQ(destruction_count, 0);

    cache.Evict();
    cache.Clear();
    EXPECT_EQ(pool.size(), 3u);
    EXPECT_EQ(destruction_count, 0);
  }

  pool.clear();
  EXPECT_EQ(destruction_count, 3);
}
