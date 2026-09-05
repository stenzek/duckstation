// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

/**
 * Provides a map template which doesn't require heap allocations for lookups.
 */

#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace detail {
struct transparent_string_hash
{
  using is_transparent = void;

  static std::size_t operator()(const std::string_view& v) { return std::hash<std::string_view>{}(v); }
  static std::size_t operator()(const std::string& s) { return std::hash<std::string>{}(s); }
  static std::size_t operator()(const char* s) { return operator()(std::string_view(s)); }
};

struct transparent_string_equal
{
  using is_transparent = void;

  static bool operator()(const std::string& lhs, const std::string_view& rhs) { return lhs == rhs; }
  static bool operator()(const std::string& lhs, const std::string& rhs) { return lhs == rhs; }
  static bool operator()(const std::string& lhs, const char* rhs) { return lhs == rhs; }
  static bool operator()(const std::string_view& lhs, const std::string& rhs) { return lhs == rhs; }
  static bool operator()(const char* lhs, const std::string& rhs) { return lhs == rhs; }
};

struct transparent_string_less
{
  using is_transparent = void;

  static bool operator()(const std::string& lhs, const std::string_view& rhs) { return lhs < rhs; }
  static bool operator()(const std::string& lhs, const std::string& rhs) { return lhs < rhs; }
  static bool operator()(const std::string& lhs, const char* rhs) { return lhs < rhs; }
  static bool operator()(const std::string_view& lhs, const std::string& rhs) { return lhs < rhs; }
  static bool operator()(const char* lhs, const std::string& rhs) { return lhs < rhs; }
};
} // namespace detail

template<typename ValueType>
using StringMap = std::map<std::string, ValueType, detail::transparent_string_less>;
template<typename ValueType>
using StringMultiMap = std::multimap<std::string, ValueType, detail::transparent_string_less>;
using StringSet = std::set<std::string, detail::transparent_string_less>;
using StringMultiSet = std::multiset<std::string, detail::transparent_string_less>;

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
