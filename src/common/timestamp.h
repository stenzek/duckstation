#pragma once
#include "types.h"
#include "string.h"

#if defined(WIN32)
#include "windows_headers.h"
#else
#include <sys/time.h>
#endif

class Timestamp
{
public:
  using UnixTimestampValue = u64;
  struct ExpandedTime
  {
    u32 Year;         // 0-...
    u32 Month;        // 1-12
    u32 DayOfMonth;   // 1-31
    u32 DayOfWeek;    // 0-6, starting at Sunday
    u32 Hour;         // 0-23
    u32 Minute;       // 0-59
    u32 Second;       // 0-59
    u32 Milliseconds; // 0-999
  };

public:
  Timestamp();
  Timestamp(const Timestamp& copy);

  // readers
  UnixTimestampValue AsUnixTimestamp() const;
  ExpandedTime AsExpandedTime() const;

  // calculators
  double DifferenceInSeconds(Timestamp& other) const;
  s64 DifferenceInSecondsInt(Timestamp& other) const;

  // setters
  void SetNow();
  void SetUnixTimestamp(UnixTimestampValue value);
  void SetExpandedTime(const ExpandedTime& value);

  // string conversion
  String ToString(const char* format) const;
  void ToString(String& destination, const char* format) const;

  // creators
  static Timestamp Now();
  static Timestamp FromUnixTimestamp(UnixTimestampValue value);
  static Timestamp FromExpandedTime(const ExpandedTime& value);

// windows-specific
#ifdef WIN32
  FILETIME AsFileTime();
  void SetWindowsFileTime(const FILETIME* pFileTime);
  static Timestamp FromWindowsFileTime(const FILETIME* pFileTime);
#endif

  // operators
  bool operator==(const Timestamp& other) const;
  bool operator!=(const Timestamp& other) const;
  bool operator<(const Timestamp& other) const;
  bool operator<=(const Timestamp& other) const;
  bool operator>(const Timestamp& other) const;
  bool operator>=(const Timestamp& other) const;
  Timestamp& operator=(const Timestamp& other);

private:
#if defined(WIN32)
  SYSTEMTIME m_value;
#else
  struct timeval m_value;
#endif
};
