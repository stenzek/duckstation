// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "string_util.h"
#include "assert.h"

#include <cctype>
#include <codecvt>
#include <cstdio>
#include <sstream>

#ifndef __APPLE__
#include <malloc.h> // alloca
#else
#include <alloca.h>
#endif

#ifdef _WIN32
#include "windows_headers.h"
#endif

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

std::optional<std::vector<u8>> StringUtil::DecodeHex(const std::string_view in)
{
  std::vector<u8> data;
  data.reserve(in.size() / 2);

  for (size_t i = 0; i < in.size() / 2; i++)
  {
    std::optional<u8> byte = StringUtil::FromChars<u8>(in.substr(i * 2, 2), 16);
    if (byte.has_value())
      data.push_back(*byte);
    else
      return std::nullopt;
  }

  return {data};
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

std::string_view StringUtil::StripWhitespace(const std::string_view str)
{
  std::string_view::size_type start = 0;
  while (start < str.size() && std::isspace(str[start]))
    start++;
  if (start == str.size())
    return {};

  std::string_view::size_type end = str.size() - 1;
  while (end > start && std::isspace(str[end]))
    end--;

  return str.substr(start, end - start + 1);
}

void StringUtil::StripWhitespace(std::string* str)
{
  {
    const char* cstr = str->c_str();
    std::string_view::size_type start = 0;
    while (start < str->size() && std::isspace(cstr[start]))
      start++;
    if (start != 0)
      str->erase(0, start);
  }

  {
    const char* cstr = str->c_str();
    std::string_view::size_type start = str->size();
    while (start > 0 && std::isspace(cstr[start - 1]))
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
  if (ch <= 0x7F)
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

size_t StringUtil::DecodeUTF8(const void* bytes, size_t length, char32_t* ch)
{
  const u8* s = reinterpret_cast<const u8*>(bytes);
  if (s[0] < 0x80)
  {
    *ch = s[0];
    return 1;
  }
  else if ((s[0] & 0xe0) == 0xc0)
  {
    if (length < 2)
      goto invalid;

    *ch = static_cast<char32_t>((static_cast<u32>(s[0] & 0x1f) << 6) | (static_cast<u32>(s[1] & 0x3f) << 0));
    return 2;
  }
  else if ((s[0] & 0xf0) == 0xe0)
  {
    if (length < 3)
      goto invalid;

    *ch = static_cast<char32_t>((static_cast<u32>(s[0] & 0x0f) << 12) | (static_cast<u32>(s[1] & 0x3f) << 6) |
                                (static_cast<u32>(s[2] & 0x3f) << 0));
    return 3;
  }
  else if ((s[0] & 0xf8) == 0xf0 && (s[0] <= 0xf4))
  {
    if (length < 4)
      goto invalid;

    *ch = static_cast<char32_t>((static_cast<u32>(s[0] & 0x07) << 18) | (static_cast<u32>(s[1] & 0x3f) << 12) |
                                (static_cast<u32>(s[2] & 0x3f) << 6) | (static_cast<u32>(s[3] & 0x3f) << 0));
    return 4;
  }

invalid:
  *ch = UNICODE_REPLACEMENT_CHARACTER; // unicode replacement character
  return 1;
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
  u8* match_bytes = allocate_on_heap ? static_cast<u8*>(alloca(pattern_length * 2)) : new u8[pattern_length * 2];
  u8* match_masks = match_bytes + pattern_length;

  hinibble = true;
  u8 match_byte = 0;
  u8 match_mask = 0;
  for (size_t i = 0, match_len = 0; i < pattern.size(); i++)
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
  if (pattern_length == 0)
    return std::nullopt;

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
