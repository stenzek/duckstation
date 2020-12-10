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
std::string StdStringFromFormat(const char* format, ...);
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
template<typename T>
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
      Strncasecmp("on", str.data(), str.length()) == 0 || Strncasecmp("1", str.data(), str.length()) == 0)
  {
    return true;
  }

  if (Strncasecmp("false", str.data(), str.length()) == 0 || Strncasecmp("no", str.data(), str.length()) == 0 ||
      Strncasecmp("off", str.data(), str.length()) == 0 || Strncasecmp("0", str.data(), str.length()) == 0)
  {
    return false;
  }

  return std::nullopt;
}

#ifndef _MSC_VER
/// from_chars doesn't seem to work with floats on gcc
template<>
inline std::optional<float> FromChars(const std::string_view& str, int base)
{
  float value;
  std::string temp(str);
  std::istringstream ss(temp);
  ss >> std::setbase(base) >> value;
  if (ss.fail())
    return std::nullopt;
  else
    return value;
}
#endif

/// Encode/decode hexadecimal byte buffers
std::optional<std::vector<u8>> DecodeHex(const std::string_view& str);
std::string EncodeHex(const u8* data, int length);

/// starts_with from C++20
ALWAYS_INLINE static bool StartsWith(const std::string_view& str, const char* prefix)
{
  return (str.compare(0, std::strlen(prefix), prefix) == 0);
}
ALWAYS_INLINE static bool EndsWith(const std::string_view& str, const char* suffix)
{
  const std::size_t suffix_length = std::strlen(suffix);
  return (str.length() >= suffix_length && str.compare(str.length() - suffix_length, suffix_length, suffix) == 0);
}

#ifdef WIN32

/// Converts the specified UTF-8 string to a wide string.
std::wstring UTF8StringToWideString(const std::string_view& str);
bool UTF8StringToWideString(std::wstring& dest, const std::string_view& str);

/// Converts the specified wide string to a UTF-8 string.
std::string WideStringToUTF8String(const std::wstring_view& str);
bool WideStringToUTF8String(std::string& dest, const std::wstring_view& str);

#endif

} // namespace StringUtil
