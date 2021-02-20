#pragma once
#include <cstdint>
#include <map>

template<class K, class V>
class LRUCache
{
  using CounterType = std::uint64_t;

  struct Item
  {
    V value;
    CounterType last_access;
  };

  using MapType = std::map<K, Item>;

public:
  LRUCache(std::size_t max_capacity = 16) : m_max_capacity(max_capacity) {}
  ~LRUCache() = default;

  std::size_t GetSize() const { return m_items.size(); }
  std::size_t GetMaxCapacity() const { return m_max_capacity; }

  void Clear() { m_items.clear(); }

  void SetMaxCapacity(std::size_t capacity)
  {
    m_max_capacity = capacity;
    if (m_items.size() > m_max_capacity)
      Evict(m_items.size() - m_max_capacity);
  }

  V* Lookup(const K& key)
  {
    auto iter = m_items.find(key);
    if (iter == m_items.end())
      return nullptr;

    iter->second.last_access = ++m_last_counter;
    return &iter->second.value;
  }

  V* Insert(const K& key, V value)
  {
    ShrinkForNewItem();

    auto iter = m_items.find(key);
    if (iter != m_items.end())
    {
      iter->second.value = std::move(value);
      iter->second.last_access = ++m_last_counter;
      return &iter->second.value;
    }
    else
    {
      Item it;
      it.last_access = ++m_last_counter;
      it.value = std::move(value);
      auto ip = m_items.emplace(key, std::move(it));
      return &ip.first->second.value;
    }
  }

  void Evict(std::size_t count = 1)
  {
    while (m_items.size() >= count)
    {
      typename MapType::iterator lowest = m_items.end();
      for (auto iter = m_items.begin(); iter != m_items.end(); ++iter)
      {
        if (lowest == m_items.end() || iter->second.last_access < lowest->second.last_access)
          lowest = iter;
      }
      m_items.erase(lowest);
    }
  }

  bool Remove(const K& key)
  {
    auto iter = m_items.find(key);
    if (iter == m_items.end())
      return false;

    m_items.erase(iter);
    return true;
  }

private:
  void ShrinkForNewItem()
  {
    if (m_items.size() < m_max_capacity)
      return;

    Evict(m_items.size() - (m_max_capacity - 1));
  }

  MapType m_items;
  CounterType m_last_counter = 0;
  std::size_t m_max_capacity = 0;
};