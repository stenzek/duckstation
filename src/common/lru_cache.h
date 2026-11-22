// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "heterogeneous_containers.h"
#include "types.h"
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>

template<class V>
struct LRUCacheDefaultDeleter
{
  static void operator()(V&& value) noexcept
  {
    if constexpr (std::is_pointer_v<V>)
      std::default_delete<std::remove_pointer_t<V>>{}(value);
  }
};

template<class V>
struct LRUCacheDefaultWeigher
{
  static constexpr std::size_t operator()(const V&) noexcept { return 0; }
};

// When a weigher is used, values must only be replaced through Insert(), not through Lookup() or Apply().
template<class K, class V, class Deleter = LRUCacheDefaultDeleter<V>, class Weigher = LRUCacheDefaultWeigher<V>>
class LRUCache
{
  using CounterType = std::uint64_t;

  struct Item
  {
    V value;
    CounterType last_access;
  };

  using MapType = std::conditional_t<std::is_same_v<K, std::string>, StringMap<Item>, std::map<K, Item>>;

public:
  LRUCache(std::size_t max_capacity = 16, bool manual_evict = false, Deleter deleter = Deleter())
    : m_max_capacity(max_capacity), m_deleter(std::move(deleter)), m_manual_evict(manual_evict)
  {
  }
  ~LRUCache()
  {
    Clear();
  }

  std::size_t GetSize() const { return m_items.size(); }
  std::size_t GetMaxCapacity() const { return m_max_capacity; }
  u64 GetTotalWeight() const { return m_total_weight; }
  u64 GetMaxWeight() const { return m_max_weight; }

  void Clear()
  {
    for (auto it = m_items.rbegin(); it != m_items.rend(); ++it)
      m_deleter(std::move(it->second.value));
    m_items.clear();
    m_total_weight = 0;
  }

  void SetMaxCapacity(std::size_t capacity)
  {
    m_max_capacity = capacity;
    if (!m_manual_evict)
      ManualEvict();
  }

  void SetMaxWeight(u64 weight)
  {
    m_max_weight = weight;
    if (!m_manual_evict)
      ManualEvict();
  }

  template<typename KeyT>
  V* Lookup(const KeyT& key)
  {
    auto iter = m_items.find(key);
    if (iter == m_items.end())
      return nullptr;

    iter->second.last_access = ++m_last_counter;
    return &iter->second.value;
  }

  V* Insert(K key, V value)
  {
    auto iter = m_items.find(key);
    if (iter != m_items.end())
    {
      m_total_weight = m_total_weight - m_weigher(iter->second.value) + m_weigher(value);
      m_deleter(std::move(iter->second.value));
      iter->second.value = std::move(value);
      iter->second.last_access = ++m_last_counter;

      // the replaced item is now the most recently used, so it is only evicted if it's the last item
      if (!m_manual_evict)
      {
        while (m_total_weight > m_max_weight && m_items.size() > 1)
          Evict(1);
      }

      return &iter->second.value;
    }
    else
    {
      const u64 weight = m_weigher(value);
      ShrinkForNewItem(weight);
      m_total_weight += weight;

      Item it;
      it.last_access = ++m_last_counter;
      it.value = std::move(value);
      auto ip = m_items.emplace(std::move(key), std::move(it));
      return &ip.first->second.value;
    }
  }

  void Evict(std::size_t count = 1)
  {
    while (!m_items.empty() && count > 0)
    {
      typename MapType::iterator lowest = m_items.end();
      for (auto iter = m_items.begin(); iter != m_items.end(); ++iter)
      {
        if (lowest == m_items.end() || iter->second.last_access < lowest->second.last_access)
          lowest = iter;
      }
      m_total_weight -= m_weigher(lowest->second.value);
      m_deleter(std::move(lowest->second.value));
      m_items.erase(lowest);
      count--;
    }
  }

  template<typename Pred>
  std::size_t RemoveMatchingItems(const Pred& pred)
  {
    std::size_t removed_count = 0;
    for (auto iter = m_items.begin(); iter != m_items.end();)
    {
      if (pred(iter->first))
      {
        m_total_weight -= m_weigher(iter->second.value);
        m_deleter(std::move(iter->second.value));
        iter = m_items.erase(iter);
        removed_count++;
      }
      else
      {
        ++iter;
      }
    }
    return removed_count;
  }

  template<typename KeyT>
  bool Remove(const KeyT& key)
  {
    auto iter = m_items.find(key);
    if (iter == m_items.end())
      return false;
    m_total_weight -= m_weigher(iter->second.value);
    m_deleter(std::move(iter->second.value));
    m_items.erase(iter);
    return true;
  }
  void SetManualEvict(bool block)
  {
    m_manual_evict = block;
    if (!m_manual_evict)
      ManualEvict();
  }
  void ManualEvict()
  {
    // evict if we went over
    if (m_items.size() > m_max_capacity)
      Evict(m_items.size() - m_max_capacity);
    while (m_total_weight > m_max_weight && !m_items.empty())
      Evict(1);
  }

  template<typename F>
  void Apply(const F& func)
  {
    for (auto& [key, value] : m_items)
      func(const_cast<const K&>(key), value.value);
  }

private:
  void ShrinkForNewItem(u64 new_weight)
  {
    if (m_manual_evict)
      return;

    if (m_items.size() >= m_max_capacity)
      Evict(m_items.size() - (m_max_capacity - 1));
    while ((m_total_weight + new_weight) > m_max_weight && !m_items.empty())
      Evict(1);
  }

  MapType m_items;
  CounterType m_last_counter = 0;
  std::size_t m_max_capacity = 0;
  u64 m_total_weight = 0;
  u64 m_max_weight = std::numeric_limits<u64>::max();
  NO_UNIQUE_ADDRESS Deleter m_deleter;
  NO_UNIQUE_ADDRESS Weigher m_weigher;
  bool m_manual_evict = false;
};
