// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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

template<typename ValueType>
using StringMap = std::map<std::string, ValueType, detail::transparent_string_less>;
template<typename ValueType>
using StringMultiMap = std::multimap<std::string, ValueType, detail::transparent_string_less>;
using StringSet = std::set<std::string, detail::transparent_string_less>;
using StringMultiSet = std::multiset<std::string, detail::transparent_string_less>;

#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201811L
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

template<typename ValueType>
using PreferUnorderedStringMap = UnorderedStringMap<ValueType>;
template<typename ValueType>
using PreferUnorderedStringMultimap = UnorderedStringMultimap<ValueType>;
using PreferUnorderedStringSet = UnorderedStringSet;
using PreferUnorderedStringMultiSet = UnorderedStringMultiSet;
#else

#pragma message "__cpp_lib_generic_unordered_lookup is missing, performance will be slower."

// GCC 10 doesn't support generic_unordered_lookup...
template<typename ValueType>
using PreferUnorderedStringMap = StringMap<ValueType>;
template<typename ValueType>
using PreferUnorderedStringMultimap = StringMultiMap<ValueType>;
using PreferUnorderedStringSet = StringSet;
using PreferUnorderedStringMultiSet = StringMultiSet;

#endif
