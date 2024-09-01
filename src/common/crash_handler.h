// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <string_view>

#ifndef _WIN32
#include <signal.h>
#endif

namespace CrashHandler {

/// Adds a callback to run just before the crash handler exits.
/// It's not guaranteed that this handler will actually run, because the process state could be very messed up by this
/// point. It's mainly a thing so that we can free up the shared memory object if there was one created.
using CleanupHandler = void(*)();

bool Install(CleanupHandler cleanup_handler);
void SetWriteDirectory(std::string_view dump_directory);
void WriteDumpForCaller();

#ifndef _WIN32

// Allow crash handler to be invoked from a signal.
void CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx);

#endif

} // namespace CrashHandler
