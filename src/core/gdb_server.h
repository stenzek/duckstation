// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

namespace GDBServer {

bool Initialize(u16 port);
bool HasAnyClients();
void PollUntil(u64 max_poll_time);
void Shutdown();

void OnSystemPaused();
void OnSystemResumed();

} // namespace GDBServer

namespace Host {

// Called when a GDB client connects or disconnects.
void OnGDBServerActiveClientsChanged(bool has_clients);

} // namespace Host
