// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "timer.h"
#include "types.h"
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include "windows_headers.h"
#else
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

namespace Common {

#ifdef _WIN32

static double s_counter_frequency;
static bool s_counter_initialized = false;

// This gets leaked... oh well.
static thread_local HANDLE s_sleep_timer;
static thread_local bool s_sleep_timer_created = false;

static HANDLE GetSleepTimer()
{
  if (s_sleep_timer_created)
    return s_sleep_timer;

  s_sleep_timer_created = true;
  s_sleep_timer = CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!s_sleep_timer)
  {
    s_sleep_timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
    if (!s_sleep_timer)
      std::fprintf(stderr, "CreateWaitableTimer() failed, falling back to Sleep()\n");
  }

  return s_sleep_timer;
}

double Timer::GetFrequency()
{
  // even if this races, it should still result in the same value..
  if (!s_counter_initialized)
  {
    LARGE_INTEGER Freq;
    QueryPerformanceFrequency(&Freq);
    s_counter_frequency = static_cast<double>(Freq.QuadPart) / 1000000000.0;
    s_counter_initialized = true;
  }

  return s_counter_frequency;
}

Timer::Value Timer::GetCurrentValue()
{
  Timer::Value ReturnValue;
  QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&ReturnValue));
  return ReturnValue;
}

double Timer::ConvertValueToNanoseconds(Timer::Value value)
{
  return (static_cast<double>(value) / GetFrequency());
}

double Timer::ConvertValueToMilliseconds(Timer::Value value)
{
  return ((static_cast<double>(value) / GetFrequency()) / 1000000.0);
}

double Timer::ConvertValueToSeconds(Timer::Value value)
{
  return ((static_cast<double>(value) / GetFrequency()) / 1000000000.0);
}

Timer::Value Timer::ConvertSecondsToValue(double s)
{
  return static_cast<Value>((s * 1000000000.0) * GetFrequency());
}

Timer::Value Timer::ConvertMillisecondsToValue(double ms)
{
  return static_cast<Value>((ms * 1000000.0) * GetFrequency());
}

Timer::Value Timer::ConvertNanosecondsToValue(double ns)
{
  return static_cast<Value>(ns * GetFrequency());
}

void Timer::SleepUntil(Value value, bool exact)
{
  if (exact)
  {
    // Even with the high-precision timer, it's not precise enough to wake us up *exactly* when we want
    // to. Dropping off the last 0.5ms and spinning for it seems enough on my system (Win11 22H2).
    const Value wake_at = value - ConvertMillisecondsToValue(0.5);
    Value current = GetCurrentValue();
    if (wake_at > current)
      SleepUntil(wake_at, false);

    // And spin off whatever time is left.
    do
    {
      current = GetCurrentValue();
    } while (current < value);
  }
  else
  {
    const s64 diff = static_cast<s64>(value - GetCurrentValue());
    if (diff <= 0)
      return;

    HANDLE timer = GetSleepTimer();
    if (timer)
    {
      const u64 one_hundred_nanos_diff = static_cast<u64>(ConvertValueToNanoseconds(diff) / 100.0);
      if (one_hundred_nanos_diff == 0)
        return;

      LARGE_INTEGER fti;
      fti.QuadPart = -static_cast<s64>(one_hundred_nanos_diff);

      if (SetWaitableTimer(timer, &fti, 0, nullptr, nullptr, FALSE))
      {
        WaitForSingleObject(timer, INFINITE);
        return;
      }
    }

    // falling back to sleep... bad.
    Sleep(static_cast<DWORD>(static_cast<u64>(diff) / 1000000));
  }
}

#else

double Timer::GetFrequency()
{
  return 1.0;
}

Timer::Value Timer::GetCurrentValue()
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

void Timer::SleepUntil(Value value, bool exact)
{
  if (exact)
  {
    static constexpr Value min_sleep_time = static_cast<Value>(0.5 * 1000000);
    const Value wake_at = value - min_sleep_time;
    Value current = GetCurrentValue();
    if (wake_at > current)
      SleepUntil(wake_at, false);

    // And spin off whatever time is left.
    do
    {
      current = GetCurrentValue();
    } while (current < value);
  }
  else
  {
    // Apple doesn't have TIMER_ABSTIME, so fall back to nanosleep in such a case.
#ifdef __APPLE__
    for (;;)
    {
      const Value current_time = GetCurrentValue();
      if (value <= current_time)
        return;

      const Value diff = value - current_time;
      struct timespec ts;
      ts.tv_sec = diff / UINT64_C(1000000000);
      ts.tv_nsec = diff % UINT64_C(1000000000);

      // nanosleep() can return EINTR if interrupted by a signal.
      if (nanosleep(&ts, nullptr) == EINTR)
        continue;
      else
        break;
    }
#else
    struct timespec ts;
    ts.tv_sec = value / UINT64_C(1000000000);
    ts.tv_nsec = value % UINT64_C(1000000000);

    for (;;)
    {
      // clock_nanosleep() can return EINTR if interrupted by a signal.
      if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR)
        continue;
      else
        break;
    }
#endif
  }
}

#endif

Timer::Timer()
{
  Reset();
}

void Timer::Reset()
{
  m_tvStartValue = GetCurrentValue();
}

double Timer::GetTimeSeconds() const
{
  return ConvertValueToSeconds(GetCurrentValue() - m_tvStartValue);
}

double Timer::GetTimeMilliseconds() const
{
  return ConvertValueToMilliseconds(GetCurrentValue() - m_tvStartValue);
}

double Timer::GetTimeNanoseconds() const
{
  return ConvertValueToNanoseconds(GetCurrentValue() - m_tvStartValue);
}

double Timer::GetTimeSecondsAndReset()
{
  const Value value = GetCurrentValue();
  const double ret = ConvertValueToSeconds(value - m_tvStartValue);
  m_tvStartValue = value;
  return ret;
}

double Timer::GetTimeMillisecondsAndReset()
{
  const Value value = GetCurrentValue();
  const double ret = ConvertValueToMilliseconds(value - m_tvStartValue);
  m_tvStartValue = value;
  return ret;
}

double Timer::GetTimeNanosecondsAndReset()
{
  const Value value = GetCurrentValue();
  const double ret = ConvertValueToNanoseconds(value - m_tvStartValue);
  m_tvStartValue = value;
  return ret;
}

bool Timer::ResetIfSecondsPassed(double s)
{
  const Value value = GetCurrentValue();
  const double ret = ConvertValueToSeconds(value - m_tvStartValue);
  if (ret < s)
    return false;

  m_tvStartValue = value;
  return true;
}

bool Timer::ResetIfMillisecondsPassed(double s)
{
  const Value value = GetCurrentValue();
  const double ret = ConvertValueToMilliseconds(value - m_tvStartValue);
  if (ret < s)
    return false;

  m_tvStartValue = value;
  return true;
}

bool Timer::ResetIfNanosecondsPassed(double s)
{
  const Value value = GetCurrentValue();
  const double ret = ConvertValueToNanoseconds(value - m_tvStartValue);
  if (ret < s)
    return false;

  m_tvStartValue = value;
  return true;
}

void Timer::BusyWait(std::uint64_t ns)
{
  const Value start = GetCurrentValue();
  const Value end = start + ConvertNanosecondsToValue(static_cast<double>(ns));
  if (end < start)
  {
    // overflow, unlikely
    while (GetCurrentValue() > end)
      ;
  }

  while (GetCurrentValue() < end)
    ;
}

void Timer::HybridSleep(std::uint64_t ns, std::uint64_t min_sleep_time)
{
  const std::uint64_t start = GetCurrentValue();
  const std::uint64_t end = start + ConvertNanosecondsToValue(static_cast<double>(ns));
  if (end < start)
  {
    // overflow, unlikely
    while (GetCurrentValue() > end)
      ;
  }

  std::uint64_t current = GetCurrentValue();
  while (current < end)
  {
    const std::uint64_t remaining = end - current;
    if (remaining >= min_sleep_time)
      NanoSleep(min_sleep_time);

    current = GetCurrentValue();
  }
}

void Timer::NanoSleep(std::uint64_t ns)
{
#if defined(_WIN32)
  HANDLE timer = GetSleepTimer();
  if (timer)
  {
    LARGE_INTEGER due_time;
    due_time.QuadPart = -static_cast<std::int64_t>(static_cast<std::uint64_t>(ns) / 100u);
    if (SetWaitableTimer(timer, &due_time, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer, INFINITE);
    else
      std::fprintf(stderr, "SetWaitableTimer() failed: %08X\n", static_cast<unsigned>(GetLastError()));
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
