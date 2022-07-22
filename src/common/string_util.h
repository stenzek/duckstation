#pragma once
#include "types.h"
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__has_include) && __has_include(<charconv>)
#include <charconv>
#ifndef _MSC_VER
#include <sstream>
#endif
#else
#include <sstream>
#endif

namespace StringUtil {

/// Constructs a std::string from a format string.
std::string StdStringFromFormat(const char* format, ...) printflike(1, 2);
std::string StdStringFromFormatV(const char* format, std::va_list ap);

/// Checks if a wildcard matches a search string.
bool WildcardMatch(const char* subject, const char* mask, bool case_sensitive = true);

/// Safe version of strlcpy.
std::size_t Strlcpy(char* dst, const char* src, std::size_t size);

/// Strlcpy from string_view.
std::size_t Strlcpy(char* dst, const std::string_view& src, std::size_t size);

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

/// Wrapper arond std::from_chars
template<typename T, std::enable_if_t<std::is_integral<T>::value, bool> = true>
inline std::optional<T> FromChars(const std::string_view& str, int base = 10)
{
  T value;

#if defined(__has_include) && __has_include(<charconv>)
  const std::from_chars_result result = std::from_chars(str.data(), str.data() + str.length(), value, base);
  if (result.ec != std::errc())
    return std::nullopt;
#else
  std::string temp(str);
  std::istringstream ss(temp);
  ss >> std::setbase(base) >> value;
  if (ss.fail())
    return std::nullopt;
#endif

  return value;
}

template<typename T, std::enable_if_t<std::is_floating_point<T>::value, bool> = true>
inline std::optional<T> FromChars(const std::string_view& str)
{
  T value;

#if defined(__has_include) && __has_include(<charconv>) && defined(_MSC_VER)
  const std::from_chars_result result = std::from_chars(str.data(), str.data() + str.length(), value);
  if (result.ec != std::errc())
    return std::nullopt;
#else
  /// libstdc++ does not support from_chars with floats yet
  std::string temp(str);
  std::istringstream ss(temp);
  ss >> value;
  if (ss.fail())
    return std::nullopt;
#endif

  return value;
}

/// Explicit override for booleans
template<>
inline std::optional<bool> FromChars(const std::string_view& str, int base)
{
  if (Strncasecmp("true", str.data(), str.length()) == 0 || Strncasecmp("yes", str.data(), str.length()) == 0 ||
      Strncasecmp("on", str.data(), str.length()) == 0 || Strncasecmp("1", str.data(), str.length()) == 0 ||
      Strncasecmp("enabled", str.data(), str.length()) == 0 || Strncasecmp("1", str.data(), str.length()) == 0)
  {
    return true;
  }

  if (Strncasecmp("false", str.data(), str.length()) == 0 || Strncasecmp("no", str.data(), str.length()) == 0 ||
      Strncasecmp("off", str.data(), str.length()) == 0 || Strncasecmp("0", str.data(), str.length()) == 0 ||
      Strncasecmp("disabled", str.data(), str.length()) == 0 || Strncasecmp("0", str.data(), str.length()) == 0)
  {
    return false;
  }

  return std::nullopt;
}

/// Encode/decode hexadecimal byte buffers
std::optional<std::vector<u8>> DecodeHex(const std::string_view& str);
std::string EncodeHex(const u8* data, int length);

/// starts_with from C++20
ALWAYS_INLINE static bool StartsWith(const std::string_view& str, const std::string_view& prefix)
{
  return (str.compare(0, prefix.length(), prefix) == 0);
}
ALWAYS_INLINE static bool EndsWith(const std::string_view& str, const std::string_view& suffix)
{
  const std::size_t suffix_length = suffix.length();
  return (str.length() >= suffix_length && str.compare(str.length() - suffix_length, suffix_length, suffix) == 0);
}

/// StartsWith/EndsWith variants which aren't case sensitive.
ALWAYS_INLINE static bool StartsWithNoCase(const std::string_view& str, const std::string_view& prefix)
{
  return (!str.empty() && Strncasecmp(str.data(), prefix.data(), prefix.length()) == 0);
}
ALWAYS_INLINE static bool EndsWithNoCase(const std::string_view& str, const std::string_view& suffix)
{
  const std::size_t suffix_length = suffix.length();
  return (str.length() >= suffix_length &&
          Strncasecmp(str.data() + (str.length() - suffix_length), suffix.data(), suffix_length) == 0);
}

/// Strip whitespace from the start/end of the string.
std::string_view StripWhitespace(const std::string_view& str);
void StripWhitespace(std::string* str);

/// Splits a string based on a single character delimiter.
std::vector<std::string_view> SplitString(const std::string_view& str, char delimiter, bool skip_empty = true);

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
static inline std::string JoinString(const T& start, const T& end, const std::string_view& delimiter)
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
std::string ReplaceAll(const std::string_view& subject, const std::string_view& search,
                       const std::string_view& replacement);
void ReplaceAll(std::string* subject, const std::string_view& search, const std::string_view& replacement);

/// Parses an assignment string (Key = Value) into its two components.
bool ParseAssignmentString(const std::string_view& str, std::string_view* key, std::string_view* value);

/// Appends a UTF-16/UTF-32 codepoint to a UTF-8 string.
void AppendUTF16CharacterToUTF8(std::string& s, u16 ch);

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
std::wstring UTF8StringToWideString(const std::string_view& str);
bool UTF8StringToWideString(std::wstring& dest, const std::string_view& str);

/// Converts the specified wide string to a UTF-8 string.
std::string WideStringToUTF8String(const std::wstring_view& str);
bool WideStringToUTF8String(std::string& dest, const std::wstring_view& str);

#endif

} // namespace StringUtil
