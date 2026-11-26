// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

namespace GDBServer {

bool Initialize(u16 port);
bool HasAnyClients();
void Shutdown();

void OnSystemPaused();
void OnSystemResumed();

} // namespace GDBServer
