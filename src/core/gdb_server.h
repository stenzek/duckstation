// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

// GDB server is available on all platforms except Android.
#ifndef __ANDROID__
#define ENABLE_GDB_SERVER
#endif

#ifdef ENABLE_GDB_SERVER

namespace GDBServer {

bool Initialize(u16 port);
bool HasAnyClients();
void Poll(u32 timeout_ms);
void Shutdown();

void OnSystemPaused();
void OnSystemResumed();

} // namespace GDBServer

#endif // ENABLE_GDB_SERVER
