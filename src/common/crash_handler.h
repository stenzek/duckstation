// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "types.h"
#include <string_view>

#ifndef _WIN32
#include <signal.h>
#endif

namespace CrashHandler {
bool Install();
void SetWriteDirectory(std::string_view dump_directory);
void WriteDumpForCaller();

#ifndef _WIN32
// Allow crash handler to be invoked from a signal.
void CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx);
#endif

} // namespace CrashHandler
