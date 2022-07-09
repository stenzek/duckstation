/**
 * Provides a map template which doesn't require heap allocations for lookups.
 */

#pragma once

#include "types.h"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace detail {
struct transparent_string_hash
{
  using is_transparent = void;

  std::size_t operator()(const std::string_view& v) const { return std::hash<std::string_view>{}(v); }
  std::size_t operator()(const std::string& s) const { return std::hash<std::string>{}(s); }
  std::size_t operator()(const char* s) const { return operator()(std::string_view(s)); }
};

struct transparent_string_equal
{
  using is_transparent = void;

  bool operator()(const std::string& lhs, const std::string_view& rhs) const { return lhs == rhs; }
  bool operator()(const std::string& lhs, const std::string& rhs) const { return lhs == rhs; }
  bool operator()(const std::string& lhs, const char* rhs) const { return lhs == rhs; }
  bool operator()(const std::string_view& lhs, const std::string& rhs) const { return lhs == rhs; }
  bool operator()(const char* lhs, const std::string& rhs) const { return lhs == rhs; }
};

struct transparent_string_less
{
  using is_transparent = void;

  bool operator()(const std::string& lhs, const std::string_view& rhs) const { return lhs < rhs; }
  bool operator()(const std::string& lhs, const std::string& rhs) const { return lhs < rhs; }
  bool operator()(const std::string& lhs, const char* rhs) const { return lhs < rhs; }
  bool operator()(const std::string_view& lhs, const std::string& rhs) const { return lhs < rhs; }
  bool operator()(const char* lhs, const std::string& rhs) const { return lhs < rhs; }
};
} // namespace detail

// This requires C++20, so fallback to ugly heap allocations if we don't have it.
#if __cplusplus >= 202002L
template<typename ValueType>
using UnorderedStringMap =
  std::unordered_map<std::string, ValueType, detail::transparent_string_hash, detail::transparent_string_equal>;
template<typename ValueType>
using UnorderedStringMultimap =
  std::unordered_multimap<std::string, ValueType, detail::transparent_string_hash, detail::transparent_string_equal>;
using UnorderedStringSet =
  std::unordered_set<std::string, detail::transparent_string_hash, detail::transparent_string_equal>;
using UnorderedStringMultiSet =
  std::unordered_multiset<std::string, detail::transparent_string_hash, detail::transparent_string_equal>;

template<typename KeyType, typename ValueType>
ALWAYS_INLINE typename UnorderedStringMap<ValueType>::const_iterator
UnorderedStringMapFind(const UnorderedStringMap<ValueType>& map, const KeyType& key)
{
  return map.find(key);
}
template<typename KeyType, typename ValueType>
ALWAYS_INLINE typename UnorderedStringMap<ValueType>::iterator
UnorderedStringMapFind(UnorderedStringMap<ValueType>& map, const KeyType& key)
{
  return map.find(key);
}
#else
template<typename ValueType>
using UnorderedStringMap = std::unordered_map<std::string, ValueType>;
template<typename ValueType>
using UnorderedStringMultimap = std::unordered_multimap<std::string, ValueType>;
using UnorderedStringSet = std::unordered_set<std::string>;
using UnorderedStringMultiSet = std::unordered_multiset<std::string>;

template<typename KeyType, typename ValueType>
ALWAYS_INLINE typename UnorderedStringMap<ValueType>::const_iterator UnorderedStringMapFind(const UnorderedStringMap<ValueType>& map, const KeyType& key)
{
  return map.find(std::string(key));
}
template<typename KeyType, typename ValueType>
ALWAYS_INLINE typename UnorderedStringMap<ValueType>::iterator UnorderedStringMapFind(UnorderedStringMap<ValueType>& map, const KeyType& key)
{
  return map.find(std::string(key));
}
#endif

template<typename ValueType>
using StringMap = std::map<std::string, ValueType, detail::transparent_string_less>;
template<typename ValueType>
using StringMultiMap = std::multimap<std::string, ValueType, detail::transparent_string_less>;
using StringSet = std::set<std::string, detail::transparent_string_less>;
using StringMultiSet = std::multiset<std::string, detail::transparent_string_less>;
