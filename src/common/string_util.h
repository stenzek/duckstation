#pragma once
#include <cstddef>
#include <cstdarg>
#include <cstring>
#include <string>

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

} // namespace StringUtil