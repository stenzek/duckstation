// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "bus.h"
#include "cpu_types.h"

namespace CPU::CodeCache {

/// Returns true if any recompiler is in use.
bool IsUsingAnyRecompiler();

/// Returns true if any recompiler and fastmem is in use.
bool IsUsingFastmem();

/// Allocates resources, call once at startup.
bool ProcessStartup();

/// Frees resources, call once at shutdown.
void ProcessShutdown();

/// Initializes resources for the system.
void Initialize();

/// Frees resources used by the system.
void Shutdown();

/// Runs the system.
[[noreturn]] void Execute();

/// Flushes the code cache, forcing all blocks to be recompiled.
void Reset();

/// Invalidates all blocks which are in the range of the specified code page.
void InvalidateBlocksWithPageIndex(u32 page_index);

/// Invalidates all blocks in the cache.
void InvalidateAllRAMBlocks();

} // namespace CPU::CodeCache
