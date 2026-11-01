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

/// Sets up the lookup tables for the current RAM size and PIO device, call on system initialization.
/// The allocation is kept between systems, and only reallocated if the size changes.
void Initialize();

/// Updates the lookup tables after the RAM size or PIO device changes, and flushes the code cache.
void ReallocateLUTs();

/// Flushes the code cache, forcing all blocks to be recompiled.
void Reset();

/// Free all non-persistent resources for the code cache.
void Shutdown();

/// Invalidates all blocks which are in the range of the specified code page.
void InvalidateBlocksWithPageIndex(u32 page_index);

/// Invalidates all blocks in the cache.
void InvalidateAllRAMBlocks();

/// Allows code in a RAM range without blocks to be compiled again, after it becomes mapped.
void ResetRAMDispatchRange(PhysicalMemoryAddress start, PhysicalMemoryAddress end);

} // namespace CPU::CodeCache
