// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "string_util.h"
#include "assert.h"
#include "bitutils.h"

#include <cctype>
#include <cstdio>
#include <memory>

#ifndef __APPLE__
#include <malloc.h> // alloca
#else
#include <alloca.h>
#endif

#ifdef _WIN32
#include "windows_headers.h"
#endif

namespace StringUtil {

template<bool swap>
static size_t DecodeUTF16Impl(const void* bytes, size_t pos, size_t size, char32_t* ch);

template<bool swap>
static std::string DecodeUTF16StringImpl(const void* bytes, size_t size);

} // namespace StringUtil

bool StringUtil::WildcardMatch(const char* subject, const char* mask, bool case_sensitive /*= true*/)
{
  if (case_sensitive)
  {
    const char* cp = nullptr;
    const char* mp = nullptr;

    while ((*subject) && (*mask != '*'))
    {
      if ((*mask != '?') && (std::tolower(*mask) != std::tolower(*subject)))
        return false;

      mask++;
      subject++;
    }

    while (*subject)
    {
      if (*mask == '*')
      {
        if (*++mask == 0)
          return true;

        mp = mask;
        cp = subject + 1;
      }
      else
      {
        if ((*mask == '?') || (std::tolower(*mask) == std::tolower(*subject)))
        {
          mask++;
          subject++;
        }
        else
        {
          mask = mp;
          subject = cp++;
        }
      }
    }

    while (*mask == '*')
    {
      mask++;
    }

    return *mask == 0;
  }
  else
  {
    const char* cp = nullptr;
    const char* mp = nullptr;

    while ((*subject) && (*mask != '*'))
    {
      if ((*mask != *subject) && (*mask != '?'))
        return false;

      mask++;
      subject++;
    }

    while (*subject)
    {
      if (*mask == '*')
      {
        if (*++mask == 0)
          return true;

        mp = mask;
        cp = subject + 1;
      }
      else
      {
        if ((*mask == *subject) || (*mask == '?'))
        {
          mask++;
          subject++;
        }
        else
        {
          mask = mp;
          subject = cp++;
        }
      }
    }

    while (*mask == '*')
    {
      mask++;
    }

    return *mask == 0;
  }
}

std::size_t StringUtil::Strlcpy(char* dst, const char* src, std::size_t size)
{
  std::size_t len = std::strlen(src);
  if (len < size)
  {
    std::memcpy(dst, src, len + 1);
  }
  else
  {
    std::memcpy(dst, src, size - 1);
    dst[size - 1] = '\0';
  }
  return len;
}

std::size_t StringUtil::Strnlen(const char* str, std::size_t max_size)
{
  const char* loc = static_cast<const char*>(std::memchr(str, 0, max_size));
  return loc ? static_cast<size_t>(loc - str) : max_size;
}

std::size_t StringUtil::Strlcpy(char* dst, const std::string_view src, std::size_t size)
{
  std::size_t len = src.length();
  if (len < size)
  {
    std::memcpy(dst, src.data(), len);
    dst[len] = '\0';
  }
  else
  {
    std::memcpy(dst, src.data(), size - 1);
    dst[size - 1] = '\0';
  }
  return len;
}

u8 StringUtil::DecodeHexDigit(char ch)
{
  if (ch >= '0' && ch <= '9')
    return static_cast<u8>(ch - '0');
  else if (ch >= 'a' && ch <= 'f')
    return static_cast<u8>(0xa + (ch - 'a'));
  else if (ch >= 'A' && ch <= 'F')
    return static_cast<u8>(0xa + (ch - 'A'));
  else
    return 0;
}

size_t StringUtil::DecodeHex(std::span<u8> dest, const std::string_view str)
{
  if ((str.length() % 2) != 0)
    return 0;

  const size_t bytes = str.length() / 2;
  if (dest.size() != bytes)
    return 0;

  for (size_t i = 0; i < bytes; i++)
  {
    std::optional<u8> byte = StringUtil::FromChars<u8>(str.substr(i * 2, 2), 16);
    if (byte.has_value())
      dest[i] = byte.value();
    else
      return i;
  }

  return bytes;
}

std::optional<std::vector<u8>> StringUtil::DecodeHex(const std::string_view in)
{
  std::optional<std::vector<u8>> ret;
  ret = std::vector<u8>(in.size() / 2);
  if (DecodeHex(ret.value(), in) != ret->size())
    ret.reset();
  return ret;
}

std::string StringUtil::EncodeHex(const void* data, size_t length)
{
  static constexpr auto hex_char = [](char x) { return static_cast<char>((x >= 0xA) ? ((x - 0xA) + 'a') : (x + '0')); };

  const u8* bytes = static_cast<const u8*>(data);

  std::string ret;
  ret.reserve(length * 2);
  for (size_t i = 0; i < length; i++)
  {
    ret.push_back(hex_char(bytes[i] >> 4));
    ret.push_back(hex_char(bytes[i] & 0xF));
  }
  return ret;
}

size_t StringUtil::EncodeBase64(const std::span<char> dest, const std::span<const u8> data)
{
  const size_t expected_length = EncodedBase64Length(data);
  Assert(dest.size() <= expected_length);

  static constexpr std::array<char, 64> table = {
    {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
     'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
     's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'}};

  const size_t dataLength = data.size();
  size_t dest_pos = 0;

  for (size_t i = 0; i < dataLength;)
  {
    const size_t bytes_in_sequence = std::min<size_t>(dataLength - i, 3);
    switch (bytes_in_sequence)
    {
      case 1:
        dest[dest_pos++] = table[(data[i] >> 2) & 63];
        dest[dest_pos++] = table[(data[i] & 3) << 4];
        dest[dest_pos++] = '=';
        dest[dest_pos++] = '=';
        break;

      case 2:
        dest[dest_pos++] = table[(data[i] >> 2) & 63];
        dest[dest_pos++] = table[((data[i] & 3) << 4) | ((data[i + 1] >> 4) & 15)];
        dest[dest_pos++] = table[(data[i + 1] & 15) << 2];
        dest[dest_pos++] = '=';
        break;

      case 3:
        dest[dest_pos++] = table[(data[i] >> 2) & 63];
        dest[dest_pos++] = table[((data[i] & 3) << 4) | ((data[i + 1] >> 4) & 15)];
        dest[dest_pos++] = table[((data[i + 1] & 15) << 2) | ((data[i + 2] >> 6) & 3)];
        dest[dest_pos++] = table[data[i + 2] & 63];
        break;

        DefaultCaseIsUnreachable();
    }

    i += bytes_in_sequence;
  }

  DebugAssert(dest_pos == expected_length);
  return dest_pos;
}

size_t StringUtil::DecodeBase64(const std::span<u8> data, const std::string_view str)
{
  static constexpr std::array<u8, 128> table = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 64, 64, 64, 0,  64, 64, 64, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64, 64, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64};

  const size_t str_length = str.length();
  if ((str_length % 4) != 0)
    return 0;

  size_t data_pos = 0;
  for (size_t i = 0; i < str_length;)
  {
    const u8 byte1 = table[str[i++] & 0x7F];
    const u8 byte2 = table[str[i++] & 0x7F];
    const u8 byte3 = table[str[i++] & 0x7F];
    const u8 byte4 = table[str[i++] & 0x7F];

    if (byte1 == 64 || byte2 == 64 || byte3 == 64 || byte4 == 64)
      break;

    data[data_pos++] = (byte1 << 2) | (byte2 >> 4);
    if (str[i - 2] != '=')
      data[data_pos++] = ((byte2 << 4) | (byte3 >> 2));
    if (str[i - 1] != '=')
      data[data_pos++] = ((byte3 << 6) | byte4);
  }

  return data_pos;
}

std::optional<std::vector<u8>> StringUtil::DecodeBase64(const std::string_view str)
{
  std::vector<u8> ret;
  const size_t len = DecodedBase64Length(str);
  ret.resize(len);
  if (DecodeBase64(ret, str) != len)
    ret = {};
  return ret;
}

std::string StringUtil::EncodeBase64(const std::span<u8> data)
{
  std::string ret;
  ret.resize(EncodedBase64Length(data));
  ret.resize(EncodeBase64(ret, data));
  return ret;
}

std::string_view StringUtil::StripWhitespace(const std::string_view str)
{
  std::string_view::size_type start = 0;
  while (start < str.size() && StringUtil::IsWhitespace(str[start]))
    start++;
  if (start == str.size())
    return {};

  std::string_view::size_type end = str.size() - 1;
  while (end > start && StringUtil::IsWhitespace(str[end]))
    end--;

  return str.substr(start, end - start + 1);
}

void StringUtil::StripWhitespace(std::string* str)
{
  {
    const char* cstr = str->c_str();
    std::string_view::size_type start = 0;
    while (start < str->size() && StringUtil::IsWhitespace(cstr[start]))
      start++;
    if (start != 0)
      str->erase(0, start);
  }

  {
    const char* cstr = str->c_str();
    std::string_view::size_type start = str->size();
    while (start > 0 && StringUtil::IsWhitespace(cstr[start - 1]))
      start--;
    if (start != str->size())
      str->erase(start);
  }
}

std::vector<std::string_view> StringUtil::SplitString(const std::string_view str, char delimiter,
                                                      bool skip_empty /*= true*/)
{
  std::vector<std::string_view> res;
  std::string_view::size_type last_pos = 0;
  std::string_view::size_type pos;
  while (last_pos < str.size() && (pos = str.find(delimiter, last_pos)) != std::string_view::npos)
  {
    std::string_view part(StripWhitespace(str.substr(last_pos, pos - last_pos)));
    if (!skip_empty || !part.empty())
      res.push_back(std::move(part));

    last_pos = pos + 1;
  }

  if (last_pos < str.size())
  {
    std::string_view part(StripWhitespace(str.substr(last_pos)));
    if (!skip_empty || !part.empty())
      res.push_back(std::move(part));
  }

  return res;
}

std::vector<std::string> StringUtil::SplitNewString(const std::string_view str, char delimiter,
                                                    bool skip_empty /*= true*/)
{
  std::vector<std::string> res;
  std::string_view::size_type last_pos = 0;
  std::string_view::size_type pos;
  while (last_pos < str.size() && (pos = str.find(delimiter, last_pos)) != std::string_view::npos)
  {
    std::string_view part(StripWhitespace(str.substr(last_pos, pos - last_pos)));
    if (!skip_empty || !part.empty())
      res.emplace_back(part);

    last_pos = pos + 1;
  }

  if (last_pos < str.size())
  {
    std::string_view part(StripWhitespace(str.substr(last_pos)));
    if (!skip_empty || !part.empty())
      res.emplace_back(part);
  }

  return res;
}

std::string StringUtil::ReplaceAll(const std::string_view subject, const std::string_view search,
                                   const std::string_view replacement)
{
  std::string ret(subject);
  ReplaceAll(&ret, search, replacement);
  return ret;
}

void StringUtil::ReplaceAll(std::string* subject, const std::string_view search, const std::string_view replacement)
{
  if (!subject->empty())
  {
    std::string::size_type start_pos = 0;
    while ((start_pos = subject->find(search, start_pos)) != std::string::npos)
    {
      subject->replace(start_pos, search.length(), replacement);
      start_pos += replacement.length();
    }
  }
}

std::string StringUtil::ReplaceAll(const std::string_view subject, const char search, const char replacement)
{
  std::string ret(subject);
  ReplaceAll(&ret, search, replacement);
  return ret;
}

void StringUtil::ReplaceAll(std::string* subject, const char search, const char replacement)
{
  for (size_t i = 0; i < subject->length(); i++)
  {
    const char ch = (*subject)[i];
    (*subject)[i] = (ch == search) ? replacement : ch;
  }
}

bool StringUtil::ParseAssignmentString(const std::string_view str, std::string_view* key, std::string_view* value)
{
  const std::string_view::size_type pos = str.find('=');
  if (pos == std::string_view::npos)
  {
    *key = std::string_view();
    *value = std::string_view();
    return false;
  }

  *key = StripWhitespace(str.substr(0, pos));
  if (pos != (str.size() - 1))
    *value = StripWhitespace(str.substr(pos + 1));
  else
    *value = std::string_view();

  return true;
}

void StringUtil::EncodeAndAppendUTF8(std::string& s, char32_t ch)
{
  if (ch <= 0x7F) [[likely]]
  {
    s.push_back(static_cast<char>(static_cast<u8>(ch)));
  }
  else if (ch <= 0x07FF)
  {
    s.push_back(static_cast<char>(static_cast<u8>(0xc0 | static_cast<u8>((ch >> 6) & 0x1f))));
    s.push_back(static_cast<char>(static_cast<u8>(0x80 | static_cast<u8>((ch & 0x3f)))));
  }
  else if (ch <= 0xFFFF)
  {
    s.push_back(static_cast<char>(static_cast<u8>(0xe0 | static_cast<u8>(((ch >> 12) & 0x0f)))));
    s.push_back(static_cast<char>(static_cast<u8>(0x80 | static_cast<u8>(((ch >> 6) & 0x3f)))));
    s.push_back(static_cast<char>(static_cast<u8>(0x80 | static_cast<u8>((ch & 0x3f)))));
  }
  else if (ch <= 0x10FFFF)
  {
    s.push_back(static_cast<char>(static_cast<u8>(0xf0 | static_cast<u8>(((ch >> 18) & 0x07)))));
    s.push_back(static_cast<char>(static_cast<u8>(0x80 | static_cast<u8>(((ch >> 12) & 0x3f)))));
    s.push_back(static_cast<char>(static_cast<u8>(0x80 | static_cast<u8>(((ch >> 6) & 0x3f)))));
    s.push_back(static_cast<char>(static_cast<u8>(0x80 | static_cast<u8>((ch & 0x3f)))));
  }
  else
  {
    s.push_back(static_cast<char>(0xefu));
    s.push_back(static_cast<char>(0xbfu));
    s.push_back(static_cast<char>(0xbdu));
  }
}

size_t StringUtil::GetEncodedUTF8Length(char32_t ch)
{
  if (ch <= 0x7F) [[likely]]
    return 1;
  else if (ch <= 0x07FF)
    return 2;
  else if (ch <= 0xFFFF)
    return 3;
  else if (ch <= 0x10FFFF)
    return 4;
  else
    return 3;
}

size_t StringUtil::EncodeAndAppendUTF8(void* utf8, size_t pos, size_t size, char32_t ch)
{
  u8* utf8_bytes = static_cast<u8*>(utf8) + pos;
  if (ch <= 0x7F) [[likely]]
  {
    if (pos == size) [[unlikely]]
      return 0;

    utf8_bytes[0] = static_cast<u8>(ch);
    return 1;
  }
  else if (ch <= 0x07FF)
  {
    if ((pos + 1) >= size) [[unlikely]]
      return 0;

    utf8_bytes[0] = static_cast<u8>(0xc0 | static_cast<u8>((ch >> 6) & 0x1f));
    utf8_bytes[1] = static_cast<u8>(0x80 | static_cast<u8>((ch & 0x3f)));
    return 2;
  }
  else if (ch <= 0xFFFF)
  {
    if ((pos + 3) >= size) [[unlikely]]
      return 0;

    utf8_bytes[0] = static_cast<u8>(0xe0 | static_cast<u8>(((ch >> 12) & 0x0f)));
    utf8_bytes[1] = static_cast<u8>(0x80 | static_cast<u8>(((ch >> 6) & 0x3f)));
    utf8_bytes[2] = static_cast<u8>(0x80 | static_cast<u8>((ch & 0x3f)));
    return 3;
  }
  else if (ch <= 0x10FFFF)
  {
    if ((pos + 4) >= size) [[unlikely]]
      return 0;

    utf8_bytes[0] = static_cast<u8>(0xf0 | static_cast<u8>(((ch >> 18) & 0x07)));
    utf8_bytes[1] = static_cast<u8>(0x80 | static_cast<u8>(((ch >> 12) & 0x3f)));
    utf8_bytes[2] = static_cast<u8>(0x80 | static_cast<u8>(((ch >> 6) & 0x3f)));
    utf8_bytes[3] = static_cast<u8>(0x80 | static_cast<u8>((ch & 0x3f)));
    return 4;
  }
  else
  {
    if ((pos + 3) >= size) [[unlikely]]
      return 0;

    utf8_bytes[0] = 0xefu;
    utf8_bytes[1] = 0xbfu;
    utf8_bytes[2] = 0xbdu;
    return 3;
  }
}

size_t StringUtil::DecodeUTF8(const void* bytes, size_t length, char32_t* ch)
{
  const u8* s = reinterpret_cast<const u8*>(bytes);
  if (s[0] < 0x80) [[likely]]
  {
    *ch = s[0];
    return 1;
  }
  else if ((s[0] & 0xe0) == 0xc0)
  {
    if (length < 2) [[unlikely]]
      goto invalid;

    *ch = static_cast<char32_t>((static_cast<u32>(s[0] & 0x1f) << 6) | (static_cast<u32>(s[1] & 0x3f) << 0));
    return 2;
  }
  else if ((s[0] & 0xf0) == 0xe0)
  {
    if (length < 3) [[unlikely]]
      goto invalid;

    *ch = static_cast<char32_t>((static_cast<u32>(s[0] & 0x0f) << 12) | (static_cast<u32>(s[1] & 0x3f) << 6) |
                                (static_cast<u32>(s[2] & 0x3f) << 0));
    return 3;
  }
  else if ((s[0] & 0xf8) == 0xf0 && (s[0] <= 0xf4))
  {
    if (length < 4) [[unlikely]]
      goto invalid;

    *ch = static_cast<char32_t>((static_cast<u32>(s[0] & 0x07) << 18) | (static_cast<u32>(s[1] & 0x3f) << 12) |
                                (static_cast<u32>(s[2] & 0x3f) << 6) | (static_cast<u32>(s[3] & 0x3f) << 0));
    return 4;
  }

invalid:
  *ch = UNICODE_REPLACEMENT_CHARACTER; // unicode replacement character
  return 1;
}

size_t StringUtil::EncodeAndAppendUTF16(void* utf16, size_t pos, size_t size, char32_t codepoint)
{
  u8* const utf16_bytes = std::assume_aligned<sizeof(u16)>(static_cast<u8*>(utf16)) + (pos * sizeof(u16));
  if (codepoint <= 0xFFFF) [[likely]]
  {
    if (pos == size) [[unlikely]]
      return 0;

    // surrogates are invalid
    const u16 codepoint16 =
      static_cast<u16>((codepoint >= 0xD800 && codepoint <= 0xDFFF) ? UNICODE_REPLACEMENT_CHARACTER : codepoint);
    std::memcpy(utf16_bytes, &codepoint16, sizeof(codepoint16));
    return 1;
  }
  else if (codepoint <= 0x10FFFF)
  {
    if ((pos + 1) >= size) [[unlikely]]
      return 0;

    codepoint -= 0x010000;

    const u16 low = static_cast<u16>(((static_cast<u32>(codepoint) >> 10) & 0x3FFu) + 0xD800);
    const u16 high = static_cast<u16>((static_cast<u32>(codepoint) & 0x3FFu) + 0xDC00);
    std::memcpy(utf16_bytes, &low, sizeof(high));
    std::memcpy(utf16_bytes + sizeof(u16), &high, sizeof(high));
    return 2;
  }
  else
  {
    // unrepresentable
    constexpr u16 value = static_cast<u16>(UNICODE_REPLACEMENT_CHARACTER);
    std::memcpy(utf16_bytes, &value, sizeof(value));
    return 1;
  }
}

template<bool swap>
size_t StringUtil::DecodeUTF16Impl(const void* bytes, size_t pos, size_t size, char32_t* ch)
{
  const u8* const utf16_bytes = std::assume_aligned<sizeof(u16)>(static_cast<const u8*>(bytes)) + pos * sizeof(u16);

  u16 high;
  std::memcpy(&high, utf16_bytes, sizeof(high));
  if constexpr (swap)
    high = ByteSwap(high);

  // High surrogate?
  if (high >= 0xD800 && high <= 0xDBFF) [[unlikely]]
  {
    if ((size - pos) < 2) [[unlikely]]
    {
      // Missing low surrogate.
      *ch = UNICODE_REPLACEMENT_CHARACTER;
      return 1;
    }

    u16 low;
    std::memcpy(&low, utf16_bytes + sizeof(u16), sizeof(low));
    if constexpr (swap)
      low = ByteSwap(low);

    if (low >= 0xDC00 && low <= 0xDFFF) [[likely]]
    {
      *ch = static_cast<char32_t>(((static_cast<u32>(high) - 0xD800u) << 10) + ((static_cast<u32>(low) - 0xDC00)) +
                                  0x10000u);
      return 2;
    }
    else
    {
      // Invalid high surrogate.
      *ch = UNICODE_REPLACEMENT_CHARACTER;
      return 2;
    }
  }
  else
  {
    // Single 16-bit value.
    *ch = static_cast<char32_t>(high);
    return 1;
  }
}

template<bool swap>
std::string StringUtil::DecodeUTF16StringImpl(const void* bytes, size_t size)
{
  std::string dest;
  dest.reserve(size);

  const size_t u16_size = size / 2;
  for (size_t pos = 0; pos < u16_size;)
  {
    char32_t codepoint;
    const size_t byte_len = DecodeUTF16Impl<swap>(bytes, pos, u16_size, &codepoint);
    StringUtil::EncodeAndAppendUTF8(dest, codepoint);
    pos += byte_len;
  }

  return dest;
}

size_t StringUtil::DecodeUTF16(const void* bytes, size_t pos, size_t size, char32_t* codepoint)
{
  return DecodeUTF16Impl<false>(bytes, pos, size, codepoint);
}

size_t StringUtil::DecodeUTF16BE(const void* bytes, size_t pos, size_t size, char32_t* codepoint)
{
  return DecodeUTF16Impl<true>(bytes, pos, size, codepoint);
}

std::string StringUtil::DecodeUTF16String(const void* bytes, size_t size)
{
  return DecodeUTF16StringImpl<false>(bytes, size);
}

std::string StringUtil::DecodeUTF16BEString(const void* bytes, size_t size)
{
  return DecodeUTF16StringImpl<true>(bytes, size);
}

std::string StringUtil::Ellipsise(const std::string_view str, u32 max_length, const char* ellipsis /*= "..."*/)
{
  std::string ret;
  ret.reserve(max_length);

  const u32 str_length = static_cast<u32>(str.length());
  const u32 ellipsis_len = static_cast<u32>(std::strlen(ellipsis));
  DebugAssert(ellipsis_len > 0 && ellipsis_len <= max_length);

  if (str_length > max_length)
  {
    const u32 copy_size = std::min(str_length, max_length - ellipsis_len);
    if (copy_size > 0)
      ret.append(str.data(), copy_size);
    if (copy_size != str_length)
      ret.append(ellipsis);
  }
  else
  {
    ret.append(str);
  }

  return ret;
}

void StringUtil::EllipsiseInPlace(std::string& str, u32 max_length, const char* ellipsis /*= "..."*/)
{
  const u32 str_length = static_cast<u32>(str.length());
  const u32 ellipsis_len = static_cast<u32>(std::strlen(ellipsis));
  DebugAssert(ellipsis_len > 0 && ellipsis_len <= max_length);

  if (str_length > max_length)
  {
    const u32 keep_size = std::min(static_cast<u32>(str.length()), max_length - ellipsis_len);
    if (keep_size != str_length)
      str.erase(keep_size);

    str.append(ellipsis);
  }
}

std::optional<size_t> StringUtil::BytePatternSearch(const std::span<const u8> bytes, const std::string_view pattern)
{
  // Parse the pattern into a bytemask.
  size_t pattern_length = 0;
  bool hinibble = true;
  for (size_t i = 0; i < pattern.size(); i++)
  {
    if ((pattern[i] >= '0' && pattern[i] <= '9') || (pattern[i] >= 'a' && pattern[i] <= 'f') ||
        (pattern[i] >= 'A' && pattern[i] <= 'F') || pattern[i] == '?')
    {
      hinibble ^= true;
      if (hinibble)
        pattern_length++;
    }
    else if (pattern[i] == ' ' || pattern[i] == '\r' || pattern[i] == '\n')
    {
      continue;
    }
    else
    {
      break;
    }
  }
  if (pattern_length == 0)
    return std::nullopt;

  const bool allocate_on_heap = (pattern_length >= 512);
  u8* match_bytes = allocate_on_heap ? new u8[pattern_length * 2] : static_cast<u8*>(alloca(pattern_length * 2));
  u8* match_masks = match_bytes + pattern_length;

  hinibble = true;
  u8 match_byte = 0;
  u8 match_mask = 0;
  size_t match_len = 0;
  for (size_t i = 0; i < pattern.size(); i++)
  {
    u8 nibble = 0, nibble_mask = 0xF;
    if (pattern[i] >= '0' && pattern[i] <= '9')
      nibble = pattern[i] - '0';
    else if (pattern[i] >= 'a' && pattern[i] <= 'f')
      nibble = pattern[i] - 'a' + 0xa;
    else if (pattern[i] >= 'A' && pattern[i] <= 'F')
      nibble = pattern[i] - 'A' + 0xa;
    else if (pattern[i] == '?')
      nibble_mask = 0;
    else if (pattern[i] == ' ' || pattern[i] == '\r' || pattern[i] == '\n')
      continue;
    else
      break;

    hinibble ^= true;
    if (hinibble)
    {
      match_bytes[match_len] = nibble | (match_byte << 4);
      match_masks[match_len] = nibble_mask | (match_mask << 4);
      match_len++;
    }
    else
    {
      match_byte = nibble;
      match_mask = nibble_mask;
    }
  }

  DebugAssert(match_len == pattern_length);

  std::optional<size_t> ret;
  const size_t max_search_offset = bytes.size() - pattern_length;
  for (size_t offset = 0; offset < max_search_offset; offset++)
  {
    const u8* start = bytes.data() + offset;
    for (size_t match_offset = 0;;)
    {
      if ((start[match_offset] & match_masks[match_offset]) != match_bytes[match_offset])
        break;

      match_offset++;
      if (match_offset == pattern_length)
      {
        // found it!
        ret = offset;
      }
    }
  }

  if (allocate_on_heap)
    delete[] match_bytes;

  return ret;
}

size_t StringUtil::DecodeUTF8(const std::string_view str, size_t offset, char32_t* ch)
{
  return DecodeUTF8(str.data() + offset, str.length() - offset, ch);
}

size_t StringUtil::DecodeUTF8(const std::string& str, size_t offset, char32_t* ch)
{
  return DecodeUTF8(str.data() + offset, str.length() - offset, ch);
}

#ifdef _WIN32

std::wstring StringUtil::UTF8StringToWideString(const std::string_view str)
{
  std::wstring ret;
  if (!UTF8StringToWideString(ret, str))
    return {};

  return ret;
}

bool StringUtil::UTF8StringToWideString(std::wstring& dest, const std::string_view str)
{
  int wlen = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0);
  if (wlen < 0)
    return false;

  dest.resize(wlen);
  if (wlen > 0 && MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), dest.data(), wlen) < 0)
    return false;

  return true;
}

std::string StringUtil::WideStringToUTF8String(const std::wstring_view str)
{
  std::string ret;
  if (!WideStringToUTF8String(ret, str))
    return {};

  return ret;
}

bool StringUtil::WideStringToUTF8String(std::string& dest, const std::wstring_view str)
{
  int mblen = WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0, nullptr, nullptr);
  if (mblen < 0)
    return false;

  dest.resize(mblen);
  if (mblen > 0 && WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), dest.data(), mblen,
                                       nullptr, nullptr) < 0)
  {
    return false;
  }

  return true;
}

#endif
