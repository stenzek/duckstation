// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team, Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR PolyForm-Strict-1.0.0)

// A reference client implementation for interfacing with PINE is available here:
// https://code.govanify.com/govanify/pine/

#pragma once

namespace PINEServer {
bool IsRunning();
bool Initialize(u16 slot);
void Shutdown();
} // namespace PINEServer
