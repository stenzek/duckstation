// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"
#include <charconv>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iomanip>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fast_float/fast_float.h"

// Older versions of libstdc++ are missing support for from_chars() with floats, and was only recently
// merged in libc++. So, just fall back to stringstream (yuck!) on everywhere except MSVC.
#if !defined(_MSC_VER)
#include <locale>
#include <sstream>
#ifdef __APPLE__
#include <Availability.h>
#endif
#endif

namespace StringUtil {

/// Checks if a wildcard matches a search string.
bool WildcardMatch(const char* subject, const char* mask, bool case_sensitive = true);

/// Safe version of strlcpy.
std::size_t Strlcpy(char* dst, const char* src, std::size_t size);

/// Strlcpy from string_view.
std::size_t Strlcpy(char* dst, const std::string_view src, std::size_t size);

/// Bounds checked version of strlen.
std::size_t Strnlen(const char* str, std::size_t max_size);

/// Platform-independent strcasecmp
static inline int Strcasecmp(const char* s1, const char* s2)
{
#ifdef _MSC_VER
  return _stricmp(s1, s2);
#else
  return strcasecmp(s1, s2);
#endif
}

/// Platform-independent strcasecmp
static inline int Strncasecmp(const char* s1, const char* s2, std::size_t n)
{
#ifdef _MSC_VER
  return _strnicmp(s1, s2, n);
#else
  return strncasecmp(s1, s2, n);
#endif
}

// Case-insensitive equality of string views.
static inline bool EqualNoCase(std::string_view s1, std::string_view s2)
{
  const size_t s1_len = s1.length();
  const size_t s2_len = s2.length();
  if (s1_len != s2_len)
    return false;
  else if (s1_len == 0)
    return true;

  return (Strncasecmp(s1.data(), s2.data(), s1_len) == 0);
}
static inline int CompareNoCase(std::string_view s1, std::string_view s2)
{
  const size_t s1_len = s1.length();
  const size_t s2_len = s2.length();
  const size_t compare_len = std::min(s1_len, s2_len);
  const int compare_res = (compare_len > 0) ? Strncasecmp(s1.data(), s2.data(), compare_len) : 0;
  return (compare_len != 0) ? compare_res : ((s1_len < s2_len) ? -1 : ((s1_len > s2_len) ? 1 : 0));
}

/// Wrapper around std::from_chars
template<typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
inline std::optional<T> FromChars(const std::string_view str, int base = 10)
{
  T value;

  const std::from_chars_result result = std::from_chars(str.data(), str.data() + str.length(), value, base);
  if (result.ec != std::errc())
    return std::nullopt;

  return value;
}
template<typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
inline std::optional<T> FromChars(const std::string_view str, int base, std::string_view* endptr)
{
  T value;

  const char* ptr = str.data();
  const char* end = ptr + str.length();
  const std::from_chars_result result = std::from_chars(ptr, end, value, base);
  if (result.ec != std::errc())
    return std::nullopt;

  if (endptr)
  {
    const size_t remaining_len = end - result.ptr;
    *endptr = (remaining_len > 0) ? std::string_view(result.ptr, remaining_len) : std::string_view();
  }

  return value;
}

template<typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
inline std::optional<T> FromCharsWithOptionalBase(std::string_view str, std::string_view* endptr = nullptr)
{
  int base = 10;
  if (str.starts_with("0x"))
  {
    base = 16;
    str = str.substr(2);
  }
  else if (str.starts_with("0b"))
  {
    base = 2;
    str = str.substr(1);
  }
  else if (str.starts_with("0") && str.length() > 1)
  {
    base = 8;
    str = str.substr(1);
  }

  if (endptr)
    return FromChars<T>(str, base, endptr);
  else
    return FromChars<T>(str, base);
}

template<typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
inline std::optional<T> FromChars(const std::string_view str)
{
  T value;

  const fast_float::from_chars_result result = fast_float::from_chars(str.data(), str.data() + str.length(), value);
  if (result.ec != std::errc())
    return std::nullopt;

  return value;
}
template<typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
inline std::optional<T> FromChars(const std::string_view str, std::string_view* endptr)
{
  T value;

  const char* ptr = str.data();
  const char* end = ptr + str.length();
  const fast_float::from_chars_result result = fast_float::from_chars(ptr, end, value);
  if (result.ec != std::errc())
    return std::nullopt;

  if (endptr)
  {
    const size_t remaining_len = end - result.ptr;
    *endptr = (remaining_len > 0) ? std::string_view(result.ptr, remaining_len) : std::string_view();
  }

  return value;
}

/// Wrapper around std::to_chars
template<typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
inline std::string ToChars(T value, int base = 10)
{
  // to_chars() requires macOS 10.15+.
#if !defined(__APPLE__) || MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_15
  constexpr size_t MAX_SIZE = 32;
  char buf[MAX_SIZE];
  std::string ret;

  const std::to_chars_result result = std::to_chars(buf, buf + MAX_SIZE, value, base);
  if (result.ec == std::errc())
    ret.append(buf, result.ptr - buf);

  return ret;
#else
  std::ostringstream ss;
  ss.imbue(std::locale::classic());
  ss << std::setbase(base) << value;
  return ss.str();
#endif
}

template<typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
inline std::string ToChars(T value)
{
  // No to_chars() in older versions of libstdc++/libc++.
#ifdef _MSC_VER
  constexpr size_t MAX_SIZE = 64;
  char buf[MAX_SIZE];
  std::string ret;
  const std::to_chars_result result = std::to_chars(buf, buf + MAX_SIZE, value);
  if (result.ec == std::errc())
    ret.append(buf, result.ptr - buf);
  return ret;
#else
  std::ostringstream ss;
  ss.imbue(std::locale::classic());
  ss << value;
  return ss.str();
#endif
}

/// Explicit override for booleans
template<>
inline std::optional<bool> FromChars(const std::string_view str, int base)
{
  if (Strncasecmp("true", str.data(), str.length()) == 0 || Strncasecmp("yes", str.data(), str.length()) == 0 ||
      Strncasecmp("on", str.data(), str.length()) == 0 || Strncasecmp("1", str.data(), str.length()) == 0 ||
      Strncasecmp("enabled", str.data(), str.length()) == 0)
  {
    return true;
  }

  if (Strncasecmp("false", str.data(), str.length()) == 0 || Strncasecmp("no", str.data(), str.length()) == 0 ||
      Strncasecmp("off", str.data(), str.length()) == 0 || Strncasecmp("0", str.data(), str.length()) == 0 ||
      Strncasecmp("disabled", str.data(), str.length()) == 0)
  {
    return false;
  }

  return std::nullopt;
}

template<>
inline std::string ToChars(bool value, int base)
{
  return std::string(value ? "true" : "false");
}

/// Encode/decode hexadecimal byte buffers
u8 DecodeHexDigit(char ch);
std::optional<std::vector<u8>> DecodeHex(const std::string_view str);
std::string EncodeHex(const void* data, size_t length);
template<typename T>
ALWAYS_INLINE static std::string EncodeHex(const std::span<const T> data)
{
  return EncodeHex(data.data(), data.size_bytes());
}

/// Returns true if the character is a hexadecimal digit.
template<typename T>
ALWAYS_INLINE static bool IsHexDigit(T ch)
{
  return ((ch >= static_cast<T>('a') && ch <= static_cast<T>('f')) ||
          (ch >= static_cast<T>('A') && ch <= static_cast<T>('F')) ||
          (ch >= static_cast<T>('0') && ch <= static_cast<T>('9')));
}

/// StartsWith/EndsWith variants which aren't case sensitive.
ALWAYS_INLINE static bool StartsWithNoCase(const std::string_view str, const std::string_view prefix)
{
  return (!str.empty() && Strncasecmp(str.data(), prefix.data(), prefix.length()) == 0);
}
ALWAYS_INLINE static bool EndsWithNoCase(const std::string_view str, const std::string_view suffix)
{
  const std::size_t suffix_length = suffix.length();
  return (str.length() >= suffix_length &&
          Strncasecmp(str.data() + (str.length() - suffix_length), suffix.data(), suffix_length) == 0);
}

/// Strip whitespace from the start/end of the string.
std::string_view StripWhitespace(const std::string_view str);
void StripWhitespace(std::string* str);

/// Splits a string based on a single character delimiter.
[[nodiscard]] std::vector<std::string_view> SplitString(const std::string_view str, char delimiter,
                                                        bool skip_empty = true);
[[nodiscard]] std::vector<std::string> SplitNewString(const std::string_view str, char delimiter,
                                                      bool skip_empty = true);

/// Joins a string together using the specified delimiter.
template<typename T>
static inline std::string JoinString(const T& start, const T& end, char delimiter)
{
  std::string ret;
  for (auto it = start; it != end; ++it)
  {
    if (it != start)
      ret += delimiter;
    ret.append(*it);
  }
  return ret;
}
template<typename T>
static inline std::string JoinString(const T& start, const T& end, const std::string_view delimiter)
{
  std::string ret;
  for (auto it = start; it != end; ++it)
  {
    if (it != start)
      ret.append(delimiter);
    ret.append(*it);
  }
  return ret;
}

/// Replaces all instances of search in subject with replacement.
[[nodiscard]] std::string ReplaceAll(const std::string_view subject, const std::string_view search,
                                     const std::string_view replacement);
void ReplaceAll(std::string* subject, const std::string_view search, const std::string_view replacement);
[[nodiscard]] std::string ReplaceAll(const std::string_view subject, const char search, const char replacement);
void ReplaceAll(std::string* subject, const char search, const char replacement);

/// Parses an assignment string (Key = Value) into its two components.
bool ParseAssignmentString(const std::string_view str, std::string_view* key, std::string_view* value);

/// Unicode replacement character.
static constexpr char32_t UNICODE_REPLACEMENT_CHARACTER = 0xFFFD;

/// Appends a UTF-16/UTF-32 codepoint to a UTF-8 string.
void EncodeAndAppendUTF8(std::string& s, char32_t ch);

/// Decodes UTF-8 to a single codepoint, updating the position parameter.
/// Returns the number of bytes the codepoint took in the original string.
size_t DecodeUTF8(const void* bytes, size_t length, char32_t* ch);
size_t DecodeUTF8(const std::string_view str, size_t offset, char32_t* ch);
size_t DecodeUTF8(const std::string& str, size_t offset, char32_t* ch);

// Replaces the end of a string with ellipsis if it exceeds the specified length.
std::string Ellipsise(const std::string_view str, u32 max_length, const char* ellipsis = "...");
void EllipsiseInPlace(std::string& str, u32 max_length, const char* ellipsis = "...");

/// Searches for the specified byte pattern in the given memory span. Wildcards (i.e. ??) are supported.
std::optional<size_t> BytePatternSearch(const std::span<const u8> bytes, const std::string_view pattern);

/// Strided memcpy/memcmp.
ALWAYS_INLINE static void StrideMemCpy(void* dst, std::size_t dst_stride, const void* src, std::size_t src_stride,
                                       std::size_t copy_size, std::size_t count)
{
  if (src_stride == dst_stride && src_stride == copy_size)
  {
    std::memcpy(dst, src, src_stride * count);
    return;
  }

  const u8* src_ptr = static_cast<const u8*>(src);
  u8* dst_ptr = static_cast<u8*>(dst);
  for (std::size_t i = 0; i < count; i++)
  {
    std::memcpy(dst_ptr, src_ptr, copy_size);
    src_ptr += src_stride;
    dst_ptr += dst_stride;
  }
}

ALWAYS_INLINE static int StrideMemCmp(const void* p1, std::size_t p1_stride, const void* p2, std::size_t p2_stride,
                                      std::size_t copy_size, std::size_t count)
{
  if (p1_stride == p2_stride && p1_stride == copy_size)
    return std::memcmp(p1, p2, p1_stride * count);

  const u8* p1_ptr = static_cast<const u8*>(p1);
  const u8* p2_ptr = static_cast<const u8*>(p2);
  for (std::size_t i = 0; i < count; i++)
  {
    int result = std::memcmp(p1_ptr, p2_ptr, copy_size);
    if (result != 0)
      return result;
    p2_ptr += p2_stride;
    p1_ptr += p1_stride;
  }

  return 0;
}

#ifdef _WIN32

/// Converts the specified UTF-8 string to a wide string.
std::wstring UTF8StringToWideString(const std::string_view str);
bool UTF8StringToWideString(std::wstring& dest, const std::string_view str);

/// Converts the specified wide string to a UTF-8 string.
std::string WideStringToUTF8String(const std::wstring_view str);
bool WideStringToUTF8String(std::string& dest, const std::wstring_view str);

#endif

} // namespace StringUtil
