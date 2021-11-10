#include "timestamp.h"
#include <cstring>
#include <ctime>
#include <tuple>

#if defined(_WIN32)

static void UnixTimeToSystemTime(time_t t, LPSYSTEMTIME pst);
static time_t SystemTimeToUnixTime(const SYSTEMTIME* pst);

#endif

Timestamp::Timestamp()
{
#if defined(_WIN32)
  m_value.wYear = 1970;
  m_value.wMonth = 1;
  m_value.wDayOfWeek = 0;
  m_value.wDay = 1;
  m_value.wHour = 0;
  m_value.wMinute = 0;
  m_value.wSecond = 0;
  m_value.wMilliseconds = 0;
#else
  m_value.tv_sec = 0;
  m_value.tv_usec = 0;
#endif
}

Timestamp::Timestamp(const Timestamp& copy)
{
#if defined(_WIN32)
  std::memcpy(&m_value, &copy.m_value, sizeof(m_value));
#else
  std::memcpy(&m_value, &copy.m_value, sizeof(m_value));
#endif
}

double Timestamp::DifferenceInSeconds(Timestamp& other) const
{
#if defined(_WIN32)
  FILETIME lft, rft;
  SystemTimeToFileTime(&m_value, &lft);
  SystemTimeToFileTime(&other.m_value, &rft);

  u64 lval = ((u64)lft.dwHighDateTime) << 32 | (u64)lft.dwLowDateTime;
  u64 rval = ((u64)rft.dwHighDateTime) << 32 | (u64)rft.dwLowDateTime;
  s64 diff = ((s64)lval - (s64)rval);
  return double(diff / 10000000ULL) + (double(diff % 10000000ULL) / 10000000.0);

#else
  return (double)(m_value.tv_sec - other.m_value.tv_sec) +
         (((double)(m_value.tv_usec - other.m_value.tv_usec)) / 1000000.0);
#endif
}

s64 Timestamp::DifferenceInSecondsInt(Timestamp& other) const
{
#if defined(_WIN32)
  FILETIME lft, rft;
  SystemTimeToFileTime(&m_value, &lft);
  SystemTimeToFileTime(&other.m_value, &rft);

  u64 lval = ((u64)lft.dwHighDateTime) << 32 | (u64)lft.dwLowDateTime;
  u64 rval = ((u64)rft.dwHighDateTime) << 32 | (u64)rft.dwLowDateTime;
  s64 diff = ((s64)lval - (s64)rval);
  return diff / 10000000ULL;

#else
  return static_cast<s64>(m_value.tv_sec - other.m_value.tv_sec);
#endif
}

Timestamp::UnixTimestampValue Timestamp::AsUnixTimestamp() const
{
#if defined(_WIN32)
  return (UnixTimestampValue)SystemTimeToUnixTime(&m_value);
#else
  return (UnixTimestampValue)m_value.tv_sec;
#endif
}

Timestamp::ExpandedTime Timestamp::AsExpandedTime() const
{
  ExpandedTime et;

#if defined(_WIN32)
  et.Year = m_value.wYear;
  et.Month = m_value.wMonth;
  et.DayOfMonth = m_value.wDay;
  et.DayOfWeek = m_value.wDayOfWeek;
  et.Hour = m_value.wHour;
  et.Minute = m_value.wMinute;
  et.Second = m_value.wSecond;
  et.Milliseconds = m_value.wMilliseconds;
#else
  struct tm t;
  time_t unixTime = (time_t)m_value.tv_sec;
  gmtime_r(&unixTime, &t);
  et.Year = t.tm_year + 1900;
  et.Month = t.tm_mon + 1;
  et.DayOfMonth = t.tm_mday;
  et.DayOfWeek = t.tm_wday;
  et.Hour = t.tm_hour;
  et.Minute = t.tm_min;
  et.Second = t.tm_sec;
  et.Milliseconds = m_value.tv_usec / 1000;
#endif

  return et;
}

void Timestamp::SetNow()
{
#if defined(_WIN32)
  GetSystemTime(&m_value);
#else
  gettimeofday(&m_value, NULL);
#endif
}

void Timestamp::SetUnixTimestamp(UnixTimestampValue value)
{
#if defined(_WIN32)
  UnixTimeToSystemTime((time_t)value, &m_value);
#else
  m_value.tv_sec = (time_t)value;
  m_value.tv_usec = 0;
#endif
}

void Timestamp::SetExpandedTime(const ExpandedTime& value)
{
#if defined(_WIN32)
  // bit of a hacky way to fill in the missing fields
  SYSTEMTIME st;
  st.wYear = (WORD)value.Year;
  st.wMonth = (WORD)value.Month;
  st.wDay = (WORD)value.DayOfMonth;
  st.wDayOfWeek = (WORD)0;
  st.wHour = (WORD)value.Hour;
  st.wMinute = (WORD)value.Minute;
  st.wSecond = (WORD)value.Second;
  st.wMilliseconds = (WORD)value.Milliseconds;
  FILETIME ft;
  SystemTimeToFileTime(&st, &ft);
  FileTimeToSystemTime(&ft, &m_value);
#else
  struct tm t;
  std::memset(&t, 0, sizeof(t));
  t.tm_sec = value.Second;
  t.tm_min = value.Minute;
  t.tm_hour = value.Hour;
  t.tm_mday = value.DayOfMonth;
  t.tm_mon = value.Month - 1;
  t.tm_year = value.Year - 1900;
  time_t unixTime = mktime(&t);
  SetUnixTimestamp((UnixTimestampValue)unixTime);
#endif
}

String Timestamp::ToString(const char* format) const
{
  SmallString destination;
  ToString(destination, format);
  return String(destination);
}

void Timestamp::ToString(String& destination, const char* format) const
{
  time_t unixTime = (time_t)AsUnixTimestamp();
  tm localTime;

#if defined(_WIN32)
  localtime_s(&localTime, &unixTime);
#else
  localtime_r(&unixTime, &localTime);
#endif

  char buffer[256];
  strftime(buffer, countof(buffer) - 1, format, &localTime);
  buffer[countof(buffer) - 1] = 0;

  destination.Clear();
  destination.AppendString(buffer);
}

Timestamp Timestamp::Now()
{
  Timestamp t;
  t.SetNow();
  return t;
}

Timestamp Timestamp::FromUnixTimestamp(UnixTimestampValue value)
{
  Timestamp t;
  t.SetUnixTimestamp(value);
  return t;
}

Timestamp Timestamp::FromExpandedTime(const ExpandedTime& value)
{
  Timestamp t;
  t.SetExpandedTime(value);
  return t;
}

bool Timestamp::operator==(const Timestamp& other) const
{
#if defined(_WIN32)
  return std::memcmp(&m_value, &other.m_value, sizeof(m_value)) == 0;
#else
  return std::memcmp(&m_value, &other.m_value, sizeof(m_value)) == 0;
#endif
}

bool Timestamp::operator!=(const Timestamp& other) const
{
  return !operator==(other);
}

bool Timestamp::operator<(const Timestamp& other) const
{
#if defined(_WIN32)
  return std::tie(m_value.wYear, m_value.wMonth, m_value.wDay, m_value.wHour, m_value.wMinute, m_value.wSecond,
                  m_value.wMilliseconds) < std::tie(other.m_value.wYear, other.m_value.wMonth, other.m_value.wDay,
                                                    other.m_value.wHour, other.m_value.wMinute, other.m_value.wSecond,
                                                    other.m_value.wMilliseconds);
#else
  return std::tie(m_value.tv_sec, m_value.tv_usec) < std::tie(other.m_value.tv_sec, other.m_value.tv_usec);
#endif
}

bool Timestamp::operator<=(const Timestamp& other) const
{
#if defined(_WIN32)
  return std::tie(m_value.wYear, m_value.wMonth, m_value.wDay, m_value.wHour, m_value.wMinute, m_value.wSecond,
                  m_value.wMilliseconds) <= std::tie(other.m_value.wYear, other.m_value.wMonth, other.m_value.wDay,
                                                     other.m_value.wHour, other.m_value.wMinute, other.m_value.wSecond,
                                                     other.m_value.wMilliseconds);
#else
  return std::tie(m_value.tv_sec, m_value.tv_usec) <= std::tie(other.m_value.tv_sec, other.m_value.tv_usec);
#endif
}

bool Timestamp::operator>(const Timestamp& other) const
{
#if defined(_WIN32)
  return std::tie(m_value.wYear, m_value.wMonth, m_value.wDay, m_value.wHour, m_value.wMinute, m_value.wSecond,
                  m_value.wMilliseconds) > std::tie(other.m_value.wYear, other.m_value.wMonth, other.m_value.wDay,
                                                    other.m_value.wHour, other.m_value.wMinute, other.m_value.wSecond,
                                                    other.m_value.wMilliseconds);
#else
  return std::tie(m_value.tv_sec, m_value.tv_usec) > std::tie(other.m_value.tv_sec, other.m_value.tv_usec);
#endif
}

bool Timestamp::operator>=(const Timestamp& other) const
{
#if defined(_WIN32)
  return std::tie(m_value.wYear, m_value.wMonth, m_value.wDay, m_value.wHour, m_value.wMinute, m_value.wSecond,
                  m_value.wMilliseconds) >= std::tie(other.m_value.wYear, other.m_value.wMonth, other.m_value.wDay,
                                                     other.m_value.wHour, other.m_value.wMinute, other.m_value.wSecond,
                                                     other.m_value.wMilliseconds);
#else
  return std::tie(m_value.tv_sec, m_value.tv_usec) >= std::tie(other.m_value.tv_sec, other.m_value.tv_usec);
#endif
}

Timestamp& Timestamp::operator=(const Timestamp& other)
{
#if defined(_WIN32)
  std::memcpy(&m_value, &other.m_value, sizeof(m_value));
#else
  std::memcpy(&m_value, &other.m_value, sizeof(m_value));
#endif

  return *this;
}

#if defined(_WIN32)

// http://support.microsoft.com/kb/167296
static void UnixTimeToFileTime(time_t t, LPFILETIME pft)
{
  LONGLONG ll;
  ll = Int32x32To64(t, 10000000ULL) + 116444736000000000ULL;
  pft->dwLowDateTime = (DWORD)ll;
  pft->dwHighDateTime = ll >> 32;
}
static void UnixTimeToSystemTime(time_t t, LPSYSTEMTIME pst)
{
  FILETIME ft;
  UnixTimeToFileTime(t, &ft);
  FileTimeToSystemTime(&ft, pst);
}
static time_t FileTimeToUnixTime(const FILETIME* pft)
{
  LONGLONG ll = ((LONGLONG)pft->dwHighDateTime) << 32 | (LONGLONG)pft->dwLowDateTime;
  ll -= 116444736000000000ULL;
  ll /= 10000000ULL;
  return (time_t)ll;
}
static time_t SystemTimeToUnixTime(const SYSTEMTIME* pst)
{
  FILETIME ft;
  SystemTimeToFileTime(pst, &ft);
  return FileTimeToUnixTime(&ft);
}

FILETIME Timestamp::AsFileTime()
{
  FILETIME ft;
  SystemTimeToFileTime(&m_value, &ft);
  return ft;
}

void Timestamp::SetWindowsFileTime(const FILETIME* pFileTime)
{
  FileTimeToSystemTime(pFileTime, &m_value);
}

Timestamp Timestamp::FromWindowsFileTime(const FILETIME* pFileTime)
{
  Timestamp ts;
  ts.SetWindowsFileTime(pFileTime);
  return ts;
}

#endif
