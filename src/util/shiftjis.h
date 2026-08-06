// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <string>
#include <string_view>

// Converts ICU-compatible Shift_JIS (ICU canonical converter ibm-943_P15A-2003, also exposed as windows-932) to UTF-8.
std::string ConvertShiftJISToUTF8(std::string_view str);
