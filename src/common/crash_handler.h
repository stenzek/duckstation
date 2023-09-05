// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "types.h"
#include <string_view>

namespace CrashHandler {
bool Install();
void SetWriteDirectory(const std::string_view& dump_directory);
void WriteDumpForCaller();
void Uninstall();
} // namespace CrashHandler
