// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <ctime>

namespace Common {

inline std::tm LocalTime(std::time_t tvalue)
{
  std::tm ttime;
#ifdef _MSC_VER
  localtime_s(&ttime, &tvalue);
#else
  localtime_r(&tvalue, &ttime);
#endif
  return ttime;
}

} // namespace Common
