// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <map>
#include <string>

#if defined(_WIN32)

// eww :/ but better than including windows.h
enum class PageProtect : u32
{
  NoAccess = 0x01,         // PAGE_NOACCESS
  ReadOnly = 0x02,         // PAGE_READONLY
  ReadWrite = 0x04,        // PAGE_READWRITE
  ReadExecute = 0x20,      // PAGE_EXECUTE_READ
  ReadWriteExecute = 0x40, // PAGE_EXECUTE_READWRITE
};

#elif defined(__APPLE__)

#include <mach/mach_vm.h>

enum class PageProtect : u32
{
  NoAccess = VM_PROT_NONE,
  ReadOnly = VM_PROT_READ,
  ReadWrite = VM_PROT_READ | VM_PROT_WRITE,
  ReadExecute = VM_PROT_READ | VM_PROT_EXECUTE,
  ReadWriteExecute = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
};

#else

#include <sys/mman.h>

enum class PageProtect : u32
{
  NoAccess = PROT_NONE,
  ReadOnly = PROT_READ,
  ReadWrite = PROT_READ | PROT_WRITE,
  ReadExecute = PROT_READ | PROT_EXEC,
  ReadWriteExecute = PROT_READ | PROT_WRITE | PROT_EXEC,
};

#endif

class Error;

namespace MemMap {
std::string GetFileMappingName(const char* prefix);
void* CreateSharedMemory(const char* name, size_t size, Error* error);
void DeleteSharedMemory(const char* name);
void DestroySharedMemory(void* ptr);
void* MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, PageProtect mode);
void UnmapSharedMemory(void* baseaddr, size_t size);
bool MemProtect(void* baseaddr, size_t size, PageProtect mode);

/// Returns the base address for the current process.
const void* GetBaseAddress();

/// Allocates RWX memory in branch range from the base address.
void* AllocateJITMemory(size_t size);

/// Releases RWX memory.
void ReleaseJITMemory(void* ptr, size_t size);

/// Flushes the instruction cache on the host for the specified range.
/// Only needed outside of X86, X86 has coherent D/I cache.
#if !defined(CPU_ARCH_ARM32) && !defined(CPU_ARCH_ARM64) && !defined(CPU_ARCH_RISCV64)
// clang-format off
ALWAYS_INLINE static void FlushInstructionCache(void* address, size_t size) { }
// clang-format on
#else
void FlushInstructionCache(void* address, size_t size);
#endif

/// JIT write protect for Apple Silicon. Needs to be called prior to writing to any RWX pages.
#if !defined(__APPLE__) || !defined(__aarch64__)
// clang-format off
ALWAYS_INLINE static void BeginCodeWrite() { }
ALWAYS_INLINE static void EndCodeWrite() { }
// clang-format on
#else
void BeginCodeWrite();
void EndCodeWrite();
#endif
} // namespace MemMap

class SharedMemoryMappingArea
{
public:
  SharedMemoryMappingArea();
  ~SharedMemoryMappingArea();

  ALWAYS_INLINE size_t GetSize() const { return m_size; }
  ALWAYS_INLINE size_t GetNumPages() const { return m_num_pages; }

  ALWAYS_INLINE u8* BasePointer() const { return m_base_ptr; }
  ALWAYS_INLINE u8* OffsetPointer(size_t offset) const { return m_base_ptr + offset; }
  ALWAYS_INLINE u8* PagePointer(size_t page) const { return m_base_ptr + HOST_PAGE_SIZE * page; }

  bool Create(size_t size);
  void Destroy();

  u8* Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, PageProtect mode);
  bool Unmap(void* map_base, size_t map_size);

private:
  u8* m_base_ptr = nullptr;
  size_t m_size = 0;
  size_t m_num_pages = 0;
  size_t m_num_mappings = 0;

#ifdef _WIN32
  using PlaceholderMap = std::map<size_t, size_t>;

  PlaceholderMap::iterator FindPlaceholder(size_t page);

  PlaceholderMap m_placeholder_ranges;
#endif
};
