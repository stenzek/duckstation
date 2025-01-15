// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "bus.h"
#include "cpu_types.h"

class Error;

namespace CPU::CodeCache {

/// Returns true if any recompiler is in use.
bool IsUsingRecompiler();

/// Returns true if any recompiler and fastmem is in use.
bool IsUsingFastmem();

/// Allocates resources, call once at startup.
bool ProcessStartup(Error* error);

/// Frees resources, call once at shutdown.
void ProcessShutdown();

/// Runs the system.
[[noreturn]] void Execute();

/// Flushes the code cache, forcing all blocks to be recompiled.
void Reset();

/// Free all non-persistent resources for the code cache.
void Shutdown();

/// Invalidates all blocks which are in the range of the specified code page.
void InvalidateBlocksWithPageIndex(u32 page_index);

/// Invalidates all blocks in the cache.
void InvalidateAllRAMBlocks();

/// Invalidates blocks with the specified PC, forcing recompilation.
void InvalidateOverlappingBlocks(u32 pc, bool force_recompile);

} // namespace CPU::CodeCache
