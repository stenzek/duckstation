#include "timer.h"

#ifdef WIN32
#include "windows_headers.h"
#else
#include <sys/time.h>
#include <time.h>
#endif

namespace Common {

#ifdef WIN32

static double s_counter_frequency;
static bool s_counter_initialized = false;

Timer::Value Timer::GetValue()
{
  // even if this races, it should still result in the same value..
  if (!s_counter_initialized)
  {
    LARGE_INTEGER Freq;
    QueryPerformanceFrequency(&Freq);
    s_counter_frequency = static_cast<double>(Freq.QuadPart) / 1000000000.0;
    s_counter_initialized = true;
  }

  Timer::Value ReturnValue;
  QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&ReturnValue));
  return ReturnValue;
}

double Timer::ConvertValueToNanoseconds(Timer::Value value)
{
  return (static_cast<double>(value) / s_counter_frequency);
}

double Timer::ConvertValueToMilliseconds(Timer::Value value)
{
  return ((static_cast<double>(value) / s_counter_frequency) / 1000000.0);
}

double Timer::ConvertValueToSeconds(Timer::Value value)
{
  return ((static_cast<double>(value) / s_counter_frequency) / 1000000000.0);
}

#else

#if 1 // using clock_gettime()

Timer::Value Timer::GetValue()
{
  struct timespec tv;
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return ((Value)tv.tv_nsec + (Value)tv.tv_sec * 1000000000);
}

double Timer::ConvertValueToNanoseconds(Timer::Value value)
{
  return static_cast<double>(value);
}

double Timer::ConvertValueToMilliseconds(Timer::Value value)
{
  return (static_cast<double>(value) / 1000000.0);
}

double Timer::ConvertValueToSeconds(Timer::Value value)
{
  return (static_cast<double>(value) / 1000000000.0);
}

#else // using gettimeofday()

Timer::Value Timer::GetValue()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return ((Value)tv.tv_usec) + ((Value)tv.tv_sec * (Value)1000000);
}

double Timer::ConvertValueToNanoseconds(Timer::Value value)
{
  return ((double)value * 1000.0);
}

double Timer::ConvertValueToMilliseconds(Timer::Value value)
{
  return ((double)value / 1000.0);
}

double Timer::ConvertValueToSeconds(Timer::Value value)
{
  return ((double)value / 1000000.0);
}

#endif

#endif

Timer::Timer()
{
  Reset();
}

void Timer::Reset()
{
  m_tvStartValue = GetValue();
}

double Timer::GetTimeSeconds() const
{
  return ConvertValueToSeconds(GetValue() - m_tvStartValue);
}

double Timer::GetTimeMilliseconds() const
{
  return ConvertValueToMilliseconds(GetValue() - m_tvStartValue);
}

double Timer::GetTimeNanoseconds() const
{
  return ConvertValueToNanoseconds(GetValue() - m_tvStartValue);
}

} // namespace Common