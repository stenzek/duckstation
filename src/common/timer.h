#pragma once
#include "types.h"
#include <cstdint>

namespace Common {

class Timer
{
public:
  using Value = u64;

  Timer();

  static Value GetValue();
  static double ConvertValueToSeconds(Value value);
  static double ConvertValueToMilliseconds(Value value);
  static double ConvertValueToNanoseconds(Value value);

  void Reset();

  double GetTimeSeconds() const;
  double GetTimeMilliseconds() const;
  double GetTimeNanoseconds() const;

private:
  Value m_tvStartValue;
};

} // namespace Common