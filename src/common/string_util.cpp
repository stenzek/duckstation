#include "string_util.h"
#include <cstdio>

namespace StringUtil {

std::string StdStringFromFormat(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string ret = StdStringFromFormatV(format, ap);
  va_end(ap);
  return ret;
}

std::string StdStringFromFormatV(const char* format, std::va_list ap)
{
#ifdef WIN32
  int len = _vscprintf(format, ap);
#else
  int len = std::vsnprintf(nullptr, 0, format, ap);
#endif

  std::string ret;
  ret.resize(len);
  std::vsnprintf(ret.data(), ret.size() + 1, format, ap);
  return ret;
}

bool WildcardMatch(const char* subject, const char* mask, bool case_sensitive /*= true*/)
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

std::size_t Strlcpy(char* dst, const char* src, std::size_t size)
{
  std::size_t len = std::strlen(src);
  if (len < size)
  {
    std::memcpy(dst, src, len + 1);
  }
  else
  {
    std::memcpy(dst, src, size - 1);
    dst[size] = '\0';
  }
  return len;
}

} // namespace StringUtil