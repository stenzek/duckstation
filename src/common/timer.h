// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include <cstdint>

class Timer
{
public:
  using Value = std::uint64_t;

  Timer();

  static double GetFrequency();
  static Value GetCurrentValue();

  static double ConvertValueToSeconds(Value value);
  static double ConvertValueToMilliseconds(Value value);
  static double ConvertValueToNanoseconds(Value value);
  static Value ConvertSecondsToValue(double s);
  static Value ConvertMillisecondsToValue(double s);
  static Value ConvertNanosecondsToValue(double ns);
  static void BusyWait(std::uint64_t ns);
  static void HybridSleep(std::uint64_t ns, std::uint64_t min_sleep_time = UINT64_C(2000000));
  static void NanoSleep(std::uint64_t ns);
  static void SleepUntil(Value value, bool exact);

  void Reset();
  void ResetTo(Value value) { m_tvStartValue = value; }

  Value GetStartValue() const { return m_tvStartValue; }

  double GetTimeSeconds() const;
  double GetTimeMilliseconds() const;
  double GetTimeNanoseconds() const;

  double GetTimeSecondsAndReset();
  double GetTimeMillisecondsAndReset();
  double GetTimeNanosecondsAndReset();

  bool ResetIfSecondsPassed(double s);
  bool ResetIfMillisecondsPassed(double s);
  bool ResetIfNanosecondsPassed(double s);

private:
  Value m_tvStartValue;
};
