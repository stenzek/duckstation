#include "timer.h"
#include <cstdio>
#include <cstdlib>

#ifdef WIN32
#include "windows_headers.h"
#else
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
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

Timer::Value Timer::ConvertSecondsToValue(double s)
{
  return static_cast<Value>((s * 1000000000.0) * s_counter_frequency);
}

Timer::Value Timer::ConvertMillisecondsToValue(double ms)
{
  return static_cast<Value>((ms * 1000000.0) * s_counter_frequency);
}

Timer::Value Timer::ConvertNanosecondsToValue(double ns)
{
  return static_cast<Value>(ns * s_counter_frequency);
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

Timer::Value Timer::ConvertSecondsToValue(double s)
{
  return static_cast<Value>(s * 1000000000.0);
}

Timer::Value Timer::ConvertMillisecondsToValue(double ms)
{
  return static_cast<Value>(ms * 1000000.0);
}

Timer::Value Timer::ConvertNanosecondsToValue(double ns)
{
  return static_cast<Value>(ns);
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

Timer::Value Timer::ConvertSecondsToValue(double s)
{
  return static_cast<Value>(ms * 1000000.0);
}

Timer::Value Timer::ConvertMillisecondsToValue(double ms)
{
  return static_cast<Value>(ms * 1000.0);
}

Timer::Value Timer::ConvertNanosecondsToValue(double ns)
{
  return static_cast<Value>(ns / 1000.0);
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

void Timer::BusyWait(std::uint64_t ns)
{
  const Value start = GetValue();
  const Value end = start + ConvertNanosecondsToValue(static_cast<double>(ns));
  if (end < start)
  {
    // overflow, unlikely
    while (GetValue() > end)
      ;
  }

  while (GetValue() < end)
    ;
}

void Timer::HybridSleep(std::uint64_t ns, std::uint64_t min_sleep_time)
{
  const std::uint64_t start = GetValue();
  const std::uint64_t end = start + ConvertNanosecondsToValue(static_cast<double>(ns));
  if (end < start)
  {
    // overflow, unlikely
    while (GetValue() > end)
      ;
  }

  std::uint64_t current = GetValue();
  while (current < end)
  {
    const std::uint64_t remaining = end - current;
    if (remaining >= min_sleep_time)
      NanoSleep(min_sleep_time);

    current = GetValue();
  }
}

void Timer::NanoSleep(std::uint64_t ns)
{
#if defined(WIN32)
  static HANDLE throttle_timer;
  static bool throttle_timer_created = false;
  if (!throttle_timer_created)
  {
    throttle_timer_created = true;
    throttle_timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
    if (throttle_timer)
      std::atexit([]() { CloseHandle(throttle_timer); });
    else
      std::fprintf(stderr, "CreateWaitableTimer() failed, falling back to Sleep()\n");
  }

  if (throttle_timer)
  {
    LARGE_INTEGER due_time;
    due_time.QuadPart = -static_cast<std::int64_t>(static_cast<std::uint64_t>(ns) / 100u);
    if (SetWaitableTimer(throttle_timer, &due_time, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(throttle_timer, INFINITE);
    else
      std::fprintf(stderr, "SetWaitableTimer() failed: %08X\n", GetLastError());
  }
  else
  {
    Sleep(static_cast<std::uint32_t>(ns / 1000000));
  }
#elif defined(__ANDROID__)
  // Round down to the next millisecond.
  usleep(static_cast<useconds_t>((ns / 1000000) * 1000));
#else
  const struct timespec ts = {0, static_cast<long>(ns)};
  nanosleep(&ts, nullptr);
#endif
}

} // namespace Common
