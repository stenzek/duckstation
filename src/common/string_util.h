#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>

#if __cplusplus >= 201703L
#include <charconv>
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

/// Platform-independent strcasecmp
inline int Strcasecmp(const char* s1, const char* s2)
{
#ifdef _MSC_VER
  return _stricmp(s1, s2);
#else
  return strcasecmp(s1, s2);
#endif
}

/// Wrapper arond std::from_chars
template<typename T>
std::optional<T> FromChars(const std::string_view str)
{
  T value;

#if __cplusplus >= 201703L
  T value;
  const std::from_chars_result result = std::from_chars(str.data(), str.data() + str.length(), value);
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

} // namespace StringUtil
