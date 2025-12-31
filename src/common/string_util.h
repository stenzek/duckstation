// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace StringUtil {

/// Returns the given character in uppercase.
ALWAYS_INLINE char ToLower(char ch)
{
  return ch + ((static_cast<u8>(ch) >= 'A' && static_cast<u8>(ch) <= 'Z') ? ('a' - 'A') : 0);
}
ALWAYS_INLINE char ToUpper(char ch)
{
  return ch - ((static_cast<u8>(ch) >= 'a' && static_cast<u8>(ch) <= 'z') ? ('a' - 'A') : 0);
}

/// Checks if a wildcard matches a search string.
bool WildcardMatch(const char* subject, const char* mask, bool case_sensitive = true);

/// Safe version of strlcpy.
std::size_t Strlcpy(char* dst, const char* src, std::size_t size);

/// Strlcpy from string_view.
std::size_t Strlcpy(char* dst, const std::string_view src, std::size_t size);

/// Bounds checked version of strlen.
std::size_t Strnlen(const char* str, std::size_t max_size);

/// Platform-independent strcasecmp
int Strcasecmp(const char* s1, const char* s2);

/// Platform-independent strcasecmp
int Strncasecmp(const char* s1, const char* s2, std::size_t n);

// Case-insensitive equality of string views.
bool EqualNoCase(std::string_view s1, std::string_view s2);
int CompareNoCase(std::string_view s1, std::string_view s2);
bool ContainsNoCase(std::string_view s1, std::string_view s2);

/// Constexpr version of strcmp, suitable for use in static_assert.
inline constexpr int ConstexprCompare(const char* s1, const char* s2)
{
  const size_t len1 = std::char_traits<char>::length(s1);
  const size_t len2 = std::char_traits<char>::length(s2);
  const size_t clen = std::min(len1, len2);
  if (const int res = std::char_traits<char>::compare(s1, s2, clen); res != 0)
    return res;

  return (len1 < len2) ? -1 : ((len1 > len2) ? 1 : 0);
}

/// Wrapper around std::from_chars
template<typename T>
  requires std::is_integral_v<T>
std::optional<T> FromChars(const std::string_view str, const int base = 10);

template<typename T>
  requires std::is_integral_v<T>
std::optional<T> FromChars(const std::string_view str, const int base, std::string_view* const endptr);

template<typename T>
  requires std::is_integral_v<T>
std::optional<T> FromCharsWithOptionalBase(const std::string_view str, std::string_view* const endptr = nullptr);

template<typename T>
  requires std::is_floating_point_v<T>
std::optional<T> FromChars(const std::string_view str);

template<typename T>
  requires std::is_floating_point_v<T>
std::optional<T> FromChars(const std::string_view str, std::string_view* const endptr);

/// Wrapper around std::to_chars
template<typename T>
  requires std::is_integral_v<T>
std::string ToChars(const T value, const int base = 10);

template<typename T>
  requires std::is_floating_point_v<T>
std::string ToChars(const T value);

/// Returns true if the given character is whitespace.
ALWAYS_INLINE bool IsWhitespace(char ch)
{
  return ((ch >= 0x09 && ch <= 0x0D) || // horizontal tab, line feed, vertical tab, form feed, carriage return
          ch == 0x20);                  // space
}

/// Removes control characters from the given string.
std::string StripControlCharacters(std::string_view str);

/// Encode/decode hexadecimal byte buffers
u8 DecodeHexDigit(char ch);
size_t DecodeHex(std::span<u8> dest, const std::string_view str);
std::optional<std::vector<u8>> DecodeHex(const std::string_view str);
std::string EncodeHex(const void* data, size_t length);
template<typename T>
ALWAYS_INLINE std::string EncodeHex(const std::span<const T> data)
{
  return EncodeHex(data.data(), data.size_bytes());
}

/// Returns true if the character is a hexadecimal digit.
template<typename T>
ALWAYS_INLINE bool IsHexDigit(T ch)
{
  return ((ch >= static_cast<T>('a') && ch <= static_cast<T>('f')) ||
          (ch >= static_cast<T>('A') && ch <= static_cast<T>('F')) ||
          (ch >= static_cast<T>('0') && ch <= static_cast<T>('9')));
}

/// Returns a byte array from the provided hex string, computed at compile-time.
template<size_t Length>
inline constexpr std::array<u8, Length> ParseFixedHexString(const char str[])
{
  std::array<u8, Length> h{};
  for (int i = 0; str[i] != '\0'; i++)
  {
    u8 nibble = 0;
    char ch = str[i];
    if (ch >= '0' && ch <= '9')
      nibble = str[i] - '0';
    else if (ch >= 'a' && ch <= 'z')
      nibble = 0xA + (str[i] - 'a');
    else if (ch >= 'A' && ch <= 'Z')
      nibble = 0xA + (str[i] - 'A');

    h[i / 2] |= nibble << (((i & 1) ^ 1) * 4);
  }
  return h;
}

/// Encode/decode Base64 buffers.
inline constexpr size_t DecodedBase64Length(const std::string_view str)
{
  // Should be a multiple of 4.
  const size_t str_length = str.length();
  if ((str_length % 4) != 0)
    return 0;

  // Reverse padding.
  size_t padding = 0;
  if (str.length() >= 2)
  {
    padding += static_cast<size_t>(str[str_length - 1] == '=');
    padding += static_cast<size_t>(str[str_length - 2] == '=');
  }

  return (str_length / 4) * 3 - padding;
}
inline constexpr size_t EncodedBase64Length(const std::span<const u8> data)
{
  return ((data.size() + 2) / 3) * 4;
}
size_t DecodeBase64(const std::span<u8> data, const std::string_view str);
size_t EncodeBase64(const std::span<char> dest, const std::span<const u8> data);
std::string EncodeBase64(const std::span<u8> data);
std::optional<std::vector<u8>> DecodeBase64(const std::string_view str);

/// StartsWith/EndsWith variants which aren't case sensitive.
bool StartsWithNoCase(const std::string_view str, const std::string_view prefix);
bool EndsWithNoCase(const std::string_view str, const std::string_view suffix);

/// Returns the number of occurrences of the given character in the string.
size_t CountChar(const std::string_view str, char ch);
size_t CountCharNoCase(const std::string_view str, char ch);

/// Strip whitespace from the start/end of the string.
std::string_view StripWhitespace(const std::string_view str);
void StripWhitespace(std::string* str);

/// Splits a string based on a single character delimiter.
[[nodiscard]] std::vector<std::string_view> SplitString(const std::string_view str, char delimiter,
                                                        bool skip_empty = true);
[[nodiscard]] std::vector<std::string> SplitNewString(const std::string_view str, char delimiter,
                                                      bool skip_empty = true);

/// Returns true if the given string is found in the string list container.
template<typename T>
inline bool IsInStringList(const T& list, const std::string_view str)
{
  return std::any_of(std::begin(list), std::end(list), [&str](const auto& it) { return (str == it); });
}

/// Adds a string to a string list container. No append is performed if the string already exists.
template<typename T>
inline bool AddToStringList(T& list, const std::string_view str)
{
  if (IsInStringList(list, str))
    return false;

  list.emplace_back(str);
  return true;
}

/// Removes a string from a string list container.
template<typename T>
inline bool RemoveFromStringList(T& list, const std::string_view str)
{
  bool removed = false;
  for (auto iter = std::begin(list); iter != std::end(list);)
  {
    if (str == *iter)
    {
      iter = list.erase(iter);
      removed = true;
    }
    else
    {
      ++iter;
    }
  }

  return removed;
}

/// Joins a string together using the specified delimiter.
template<typename T>
inline std::string JoinString(const T& start, const T& end, char delimiter)
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
inline std::string JoinString(const T& list, char delimiter)
{
  return JoinString(std::begin(list), std::end(list), delimiter);
}
template<typename T>
inline std::string JoinString(const T& start, const T& end, const std::string_view delimiter)
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
template<typename T>
inline std::string JoinString(const T& list, const std::string_view delimiter)
{
  return JoinString(std::begin(list), std::end(list), delimiter);
}

/// Replaces all instances of search in subject with replacement.
[[nodiscard]] std::string ReplaceAll(const std::string_view subject, const std::string_view search,
                                     const std::string_view replacement);
void ReplaceAll(std::string* subject, const std::string_view search, const std::string_view replacement);
[[nodiscard]] std::string ReplaceAll(const std::string_view subject, const char search, const char replacement);
void ReplaceAll(std::string* subject, const char search, const char replacement);

/// Parses an assignment string (Key = Value) into its two components.
bool ParseAssignmentString(const std::string_view str, std::string_view* key, std::string_view* value);

/// Helper for tokenizing strings.
std::optional<std::string_view> GetNextToken(std::string_view& caret, char separator);

/// Unicode replacement character.
inline constexpr char32_t UNICODE_REPLACEMENT_CHARACTER = 0xFFFD;

/// Returns the length of a UTF-8 string in codepoints.
size_t GetUTF8CharacterCount(const std::string_view str);

/// Appends a UTF-16/UTF-32 codepoint to a UTF-8 string.
void EncodeAndAppendUTF8(std::string& s, char32_t ch);
size_t EncodeAndAppendUTF8(void* utf8, size_t pos, size_t size, char32_t ch);
size_t GetEncodedUTF8Length(char32_t ch);

/// Decodes UTF-8 to a single unicode codepoint.
/// Returns the number of bytes the codepoint took in the original string.
size_t DecodeUTF8(const void* bytes, size_t length, char32_t* ch);
size_t DecodeUTF8(const std::string_view str, size_t offset, char32_t* ch);
size_t DecodeUTF8(const std::string& str, size_t offset, char32_t* ch);

/// Appends a unicode codepoint to a UTF-16 string.
size_t EncodeAndAppendUTF16(void* utf16, size_t pos, size_t size, char32_t codepoint);

/// Decodes UTF-16 to a single unicode codepoint.
/// Returns the number of 16-bit units the codepoint took in the original string.
size_t DecodeUTF16(const void* bytes, size_t pos, size_t size, char32_t* codepoint);
size_t DecodeUTF16BE(const void* bytes, size_t pos, size_t size, char32_t* codepoint);

/// Decodes a UTF-16 string to a UTF-8 string.
std::string DecodeUTF16String(const void* bytes, size_t size);
std::string DecodeUTF16BEString(const void* bytes, size_t size);

// Replaces the end of a string with ellipsis if it exceeds the specified length.
std::string Ellipsise(const std::string_view str, u32 max_length, const char* ellipsis = "...");
void EllipsiseInPlace(std::string& str, u32 max_length, const char* ellipsis = "...");

/// Searches for the specified byte pattern in the given memory span. Wildcards (i.e. ??) are supported.
std::optional<size_t> BytePatternSearch(const std::span<const u8> bytes, const std::string_view pattern);

/// Strided memcpy/memcmp.
void StrideMemCpy(void* dst, std::size_t dst_stride, const void* src, std::size_t src_stride, std::size_t copy_size,
                  std::size_t count);

int StrideMemCmp(const void* p1, std::size_t p1_stride, const void* p2, std::size_t p2_stride, std::size_t copy_size,
                 std::size_t count);

#ifdef _WIN32

/// Converts the specified UTF-8 string to a wide string.
std::wstring UTF8StringToWideString(const std::string_view str);
bool UTF8StringToWideString(std::wstring& dest, const std::string_view str);

/// Converts the specified wide string to a UTF-8 string.
std::string WideStringToUTF8String(const std::wstring_view str);
bool WideStringToUTF8String(std::string& dest, const std::wstring_view str);

#endif

} // namespace StringUtil
