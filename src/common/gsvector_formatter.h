// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gsvector.h"
#include "small_string.h"

#include "fmt/format.h"

template<>
struct fmt::formatter<GSVector4i> : formatter<std::string_view>
{
  auto format(const GSVector4i& rc, format_context& ctx) const
  {
    const TinyString str =
      TinyString::from_format("{},{} => {},{} ({}x{})", rc.left, rc.top, rc.right, rc.bottom, rc.width(), rc.height());

    return fmt::formatter<std::string_view>::format(str.view(), ctx);
  }
};
