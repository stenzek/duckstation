// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "jit_code_buffer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/memmap.h"

#include <algorithm>

Log_SetChannel(JitCodeBuffer);

#if defined(_WIN32)
#include "common/windows_headers.h"
#else
#include <errno.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#endif
#endif

JitCodeBuffer::JitCodeBuffer() = default;

JitCodeBuffer::JitCodeBuffer(u32 size, u32 far_code_size)
{
  if (!Allocate(size, far_code_size))
    Panic("Failed to allocate code space");
}

JitCodeBuffer::JitCodeBuffer(void* buffer, u32 size, u32 far_code_size, u32 guard_pages)
{
  if (!Initialize(buffer, size, far_code_size))
    Panic("Failed to initialize code space");
}

JitCodeBuffer::~JitCodeBuffer()
{
  Destroy();
}

bool JitCodeBuffer::Allocate(u32 size /* = 64 * 1024 * 1024 */, u32 far_code_size /* = 0 */)
{
  Destroy();

  m_total_size = size + far_code_size;

#ifdef CPU_ARCH_X64
  // Try to find a region in 32-bit range of ourselves.
  // Assume that the DuckStation binary will at max be 256MB. Therefore the max offset is
  // +/- 256MB + round_up_pow2(size). This'll be 512MB for the JITs.
  static const u8 base_ptr = 0;
  const u8* base =
    reinterpret_cast<const u8*>(Common::AlignDownPow2(reinterpret_cast<uintptr_t>(&base_ptr), HOST_PAGE_SIZE));
  const u32 max_displacement = 0x80000000u - Common::NextPow2(256 * 1024 * 1024 + m_total_size);
  const u8* max_address = ((base + max_displacement) < base) ?
                            reinterpret_cast<const u8*>(std::numeric_limits<uintptr_t>::max()) :
                            (base + max_displacement);
  const u8* min_address = ((base - max_displacement) > base) ? nullptr : (base - max_displacement);
  const u32 step = 64 * 1024 * 1024;
  const u32 steps = static_cast<u32>(max_address - min_address) / step;
  for (u32 offset = 0; offset < steps; offset++)
  {
    const u8* addr = max_address - (offset * step);
    VERBOSE_LOG("Trying {} (base {}, offset {}, displacement 0x{:X})", static_cast<const void*>(addr),
                static_cast<const void*>(base), offset, static_cast<ptrdiff_t>(addr - base));
    if (TryAllocateAt(addr))
      break;
  }
  if (m_code_ptr)
  {
    INFO_LOG("Allocated JIT buffer of size {} at {} (0x{:X} bytes away)", m_total_size, static_cast<void*>(m_code_ptr),
             static_cast<ptrdiff_t>(m_code_ptr - base));
  }
  else
  {
    ERROR_LOG("Failed to allocate JIT buffer in range, expect crashes.");
    if (!TryAllocateAt(nullptr))
      return false;
  }
#else
  if (!TryAllocateAt(nullptr))
    return false;
#endif

  m_free_code_ptr = m_code_ptr;
  m_code_size = size;
  m_code_used = 0;

  m_far_code_ptr = static_cast<u8*>(m_code_ptr) + size;
  m_free_far_code_ptr = m_far_code_ptr;
  m_far_code_size = far_code_size;
  m_far_code_used = 0;

  m_old_protection = 0;
  m_owns_buffer = true;
  return true;
}

bool JitCodeBuffer::TryAllocateAt(const void* addr)
{
#if defined(_WIN32)
  m_code_ptr = static_cast<u8*>(VirtualAlloc(const_cast<void*>(addr), m_total_size,
                                             addr ? (MEM_RESERVE | MEM_COMMIT) : MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  if (!m_code_ptr)
  {
    if (!addr)
      ERROR_LOG("VirtualAlloc(RWX, {}) for internal buffer failed: {}", m_total_size, GetLastError());
    return false;
  }

  return true;
#elif defined(__APPLE__) && !defined(__aarch64__)
  kern_return_t ret = mach_vm_allocate(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&addr), m_total_size,
                                       addr ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE);
  if (ret != KERN_SUCCESS)
  {
    ERROR_LOG("mach_vm_allocate() returned {}", ret);
    return false;
  }

  ret = mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(addr), m_total_size, false,
                        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
  if (ret != KERN_SUCCESS)
  {
    ERROR_LOG("mach_vm_protect() returned {}", ret);
    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(addr), m_total_size);
    return false;
  }

  m_code_ptr = static_cast<u8*>(const_cast<void*>(addr));
  return true;
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__linux__)
  // Linux does the right thing, allows us to not disturb an existing mapping.
  if (addr)
    flags |= MAP_FIXED_NOREPLACE;
#elif defined(__FreeBSD__)
  // FreeBSD achieves the same with MAP_FIXED and MAP_EXCL.
  if (addr)
    flags |= MAP_FIXED | MAP_EXCL;
#elif defined(__APPLE__)
  // On ARM64, we need to use MAP_JIT, which means we can't use MAP_FIXED.
  if (addr)
    return false;
  flags |= MAP_JIT;
#endif

  m_code_ptr =
    static_cast<u8*>(mmap(const_cast<void*>(addr), m_total_size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0));
  if (!m_code_ptr)
  {
    if (!addr)
      ERROR_LOG("mmap(RWX, {}) for internal buffer failed: {}", m_total_size, errno);

    return false;
  }
  else if (addr && m_code_ptr != addr)
  {
    if (munmap(m_code_ptr, m_total_size) != 0)
      ERROR_LOG("Failed to munmap() incorrectly hinted allocation: {}", errno);
    m_code_ptr = nullptr;
    return false;
  }

  return true;
#else
  return false;
#endif
}

bool JitCodeBuffer::Initialize(void* buffer, u32 size, u32 far_code_size /* = 0 */, u32 guard_size /* = 0 */)
{
  Destroy();

  if ((far_code_size > 0 && guard_size >= far_code_size) || (far_code_size + (guard_size * 2)) > size)
    return false;

#if defined(_WIN32)
  DWORD old_protect = 0;
  if (!VirtualProtect(buffer, size, PAGE_EXECUTE_READWRITE, &old_protect))
  {
    ERROR_LOG("VirtualProtect(RWX) for external buffer failed: {}", GetLastError());
    return false;
  }

  if (guard_size > 0)
  {
    DWORD old_guard_protect = 0;
    u8* guard_at_end = (static_cast<u8*>(buffer) + size) - guard_size;
    if (!VirtualProtect(buffer, guard_size, PAGE_NOACCESS, &old_guard_protect) ||
        !VirtualProtect(guard_at_end, guard_size, PAGE_NOACCESS, &old_guard_protect))
    {
      ERROR_LOG("VirtualProtect(NOACCESS) for guard page failed: {}", GetLastError());
      return false;
    }
  }

  m_code_ptr = static_cast<u8*>(buffer);
  m_old_protection = static_cast<u32>(old_protect);
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)
  if (mprotect(buffer, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
  {
    ERROR_LOG("mprotect(RWX) for external buffer failed: {}", errno);
    return false;
  }

  if (guard_size > 0)
  {
    u8* guard_at_end = (static_cast<u8*>(buffer) + size) - guard_size;
    if (mprotect(buffer, guard_size, PROT_NONE) != 0 || mprotect(guard_at_end, guard_size, PROT_NONE) != 0)
    {
      ERROR_LOG("mprotect(NONE) for guard page failed: {}", errno);
      return false;
    }
  }

  // reasonable default?
  m_code_ptr = static_cast<u8*>(buffer);
  m_old_protection = PROT_READ | PROT_WRITE;
#else
  m_code_ptr = nullptr;
#endif

  if (!m_code_ptr)
    return false;

  m_total_size = size;
  m_free_code_ptr = m_code_ptr + guard_size;
  m_code_size = size - far_code_size - (guard_size * 2);
  m_code_used = 0;

  m_far_code_ptr = static_cast<u8*>(m_code_ptr) + m_code_size;
  m_free_far_code_ptr = m_far_code_ptr;
  m_far_code_size = far_code_size - guard_size;
  m_far_code_used = 0;

  m_guard_size = guard_size;
  m_owns_buffer = false;
  return true;
}

void JitCodeBuffer::Destroy()
{
  if (m_owns_buffer)
  {
#if defined(_WIN32)
    if (!VirtualFree(m_code_ptr, 0, MEM_RELEASE))
      ERROR_LOG("Failed to free code pointer {}", static_cast<void*>(m_code_ptr));
#elif defined(__APPLE__) && !defined(__aarch64__)
    const kern_return_t res =
      mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(m_code_ptr), m_total_size);
    if (res != KERN_SUCCESS)
      ERROR_LOG("mach_vm_deallocate() failed: {}", res);
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)
    if (munmap(m_code_ptr, m_total_size) != 0)
      ERROR_LOG("Failed to free code pointer {}", static_cast<void*>(m_code_ptr));
#endif
  }
  else if (m_code_ptr)
  {
#if defined(_WIN32)
    DWORD old_protect = 0;
    if (!VirtualProtect(m_code_ptr, m_total_size, m_old_protection, &old_protect))
      ERROR_LOG("Failed to restore protection on {}", static_cast<void*>(m_code_ptr));
#else
    if (mprotect(m_code_ptr, m_total_size, m_old_protection) != 0)
      ERROR_LOG("Failed to restore protection on {}", static_cast<void*>(m_code_ptr));
#endif
  }

  m_code_ptr = nullptr;
  m_free_code_ptr = nullptr;
  m_code_size = 0;
  m_code_reserve_size = 0;
  m_code_used = 0;
  m_far_code_ptr = nullptr;
  m_free_far_code_ptr = nullptr;
  m_far_code_size = 0;
  m_far_code_used = 0;
  m_total_size = 0;
  m_guard_size = 0;
  m_old_protection = 0;
  m_owns_buffer = false;
}

void JitCodeBuffer::ReserveCode(u32 size)
{
  Assert(m_code_used == 0);
  Assert(size < m_code_size);

  m_code_reserve_size += size;
  m_free_code_ptr += size;
  m_code_size -= size;
}

void JitCodeBuffer::CommitCode(u32 length)
{
  if (length == 0)
    return;

#if defined(CPU_ARCH_ARM32) || defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_RISCV64)
  // ARM instruction and data caches are not coherent, we need to flush after every block.
  FlushInstructionCache(m_free_code_ptr, length);
#endif

  Assert(length <= (m_code_size - m_code_used));
  m_free_code_ptr += length;
  m_code_used += length;
}

void JitCodeBuffer::CommitFarCode(u32 length)
{
  if (length == 0)
    return;

#if defined(CPU_ARCH_ARM32) || defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_RISCV64)
  // ARM instruction and data caches are not coherent, we need to flush after every block.
  FlushInstructionCache(m_free_far_code_ptr, length);
#endif

  Assert(length <= (m_far_code_size - m_far_code_used));
  m_free_far_code_ptr += length;
  m_far_code_used += length;
}

void JitCodeBuffer::Reset()
{
  MemMap::BeginCodeWrite();

  m_free_code_ptr = m_code_ptr + m_guard_size + m_code_reserve_size;
  m_code_used = 0;
  std::memset(m_free_code_ptr, 0, m_code_size);
  FlushInstructionCache(m_free_code_ptr, m_code_size);

  if (m_far_code_size > 0)
  {
    m_free_far_code_ptr = m_far_code_ptr;
    m_far_code_used = 0;
    std::memset(m_free_far_code_ptr, 0, m_far_code_size);
    FlushInstructionCache(m_free_far_code_ptr, m_far_code_size);
  }

  MemMap::EndCodeWrite();
}

void JitCodeBuffer::Align(u32 alignment, u8 padding_value)
{
  DebugAssert(Common::IsPow2(alignment));
  const u32 num_padding_bytes =
    std::min(static_cast<u32>(Common::AlignUpPow2(reinterpret_cast<uintptr_t>(m_free_code_ptr), alignment) -
                              reinterpret_cast<uintptr_t>(m_free_code_ptr)),
             GetFreeCodeSpace());
  std::memset(m_free_code_ptr, padding_value, num_padding_bytes);
  m_free_code_ptr += num_padding_bytes;
  m_code_used += num_padding_bytes;
}

void JitCodeBuffer::FlushInstructionCache(void* address, u32 size)
{
#if defined(_WIN32)
  ::FlushInstructionCache(GetCurrentProcess(), address, size);
#elif defined(__GNUC__) || defined(__clang__)
  __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address) + size);
#else
#error Unknown platform.
#endif
}
