// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <ctime>
#include <optional>

namespace Common {

inline std::optional<std::tm> LocalTime(std::time_t tvalue)
{
  std::optional<std::tm> ret;
  ret.emplace();
#ifdef _MSC_VER
  if (localtime_s(&ret.value(), &tvalue) != 0)
    ret.reset();
#else
  if (!localtime_r(&tvalue, &ret.value()))
    ret.reset();
#endif
  return ret;
}

} // namespace Common
