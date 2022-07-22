#include "jit_code_buffer.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/platform.h"
#include <algorithm>
Log_SetChannel(JitCodeBuffer);

#if defined(_WIN32)
#include "common/windows_headers.h"
#else
#include <errno.h>
#include <sys/mman.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
// pthread_jit_write_protect_np()
#include <pthread.h>
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

#if defined(_WIN32)
#if !defined(_UWP)
  m_code_ptr = static_cast<u8*>(VirtualAlloc(nullptr, m_total_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
  m_code_ptr = static_cast<u8*>(
    VirtualAlloc2FromApp(GetCurrentProcess(), nullptr, m_total_size, MEM_COMMIT, PAGE_READWRITE, nullptr, 0));
  if (m_code_ptr)
  {
    ULONG old_protection;
    if (!VirtualProtectFromApp(m_code_ptr, m_total_size, PAGE_EXECUTE_READWRITE, &old_protection))
    {
      VirtualFree(m_code_ptr, m_total_size, MEM_RELEASE);
      return false;
    }
  }
#endif
  if (!m_code_ptr)
  {
    Log_ErrorPrintf("VirtualAlloc(RWX, %u) for internal buffer failed: %u", m_total_size, GetLastError());
    return false;
  }
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__APPLE__) && defined(__aarch64__)
  // MAP_JIT and toggleable write protection is required on Apple Silicon.
  flags |= MAP_JIT;
#endif

  m_code_ptr = static_cast<u8*>(mmap(nullptr, m_total_size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0));
  if (!m_code_ptr)
  {
    Log_ErrorPrintf("mmap(RWX, %u) for internal buffer failed: %d", m_total_size, errno);
    return false;
  }
#else
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

bool JitCodeBuffer::Initialize(void* buffer, u32 size, u32 far_code_size /* = 0 */, u32 guard_size /* = 0 */)
{
  Destroy();

  if ((far_code_size > 0 && guard_size >= far_code_size) || (far_code_size + (guard_size * 2)) > size)
    return false;

#if defined(_WIN32)
  DWORD old_protect = 0;
  if (!VirtualProtect(buffer, size, PAGE_EXECUTE_READWRITE, &old_protect))
  {
    Log_ErrorPrintf("VirtualProtect(RWX) for external buffer failed: %u", GetLastError());
    return false;
  }

  if (guard_size > 0)
  {
    DWORD old_guard_protect = 0;
    u8* guard_at_end = (static_cast<u8*>(buffer) + size) - guard_size;
    if (!VirtualProtect(buffer, guard_size, PAGE_NOACCESS, &old_guard_protect) ||
        !VirtualProtect(guard_at_end, guard_size, PAGE_NOACCESS, &old_guard_protect))
    {
      Log_ErrorPrintf("VirtualProtect(NOACCESS) for guard page failed: %u", GetLastError());
      return false;
    }
  }

  m_code_ptr = static_cast<u8*>(buffer);
  m_old_protection = static_cast<u32>(old_protect);
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)
  if (mprotect(buffer, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
  {
    Log_ErrorPrintf("mprotect(RWX) for external buffer failed: %d", errno);
    return false;
  }

  if (guard_size > 0)
  {
    u8* guard_at_end = (static_cast<u8*>(buffer) + size) - guard_size;
    if (mprotect(buffer, guard_size, PROT_NONE) != 0 || mprotect(guard_at_end, guard_size, PROT_NONE) != 0)
    {
      Log_ErrorPrintf("mprotect(NONE) for guard page failed: %d", errno);
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
      Log_ErrorPrintf("Failed to free code pointer %p", m_code_ptr);
#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__HAIKU__) || defined(__FreeBSD__)
    if (munmap(m_code_ptr, m_total_size) != 0)
      Log_ErrorPrintf("Failed to free code pointer %p", m_code_ptr);
#endif
  }
  else if (m_code_ptr)
  {
#if defined(_WIN32)
    DWORD old_protect = 0;
    if (!VirtualProtect(m_code_ptr, m_total_size, m_old_protection, &old_protect))
      Log_ErrorPrintf("Failed to restore protection on %p", m_code_ptr);
#else
    if (mprotect(m_code_ptr, m_total_size, m_old_protection) != 0)
      Log_ErrorPrintf("Failed to restore protection on %p", m_code_ptr);
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

#if defined(CPU_AARCH32) || defined(CPU_AARCH64)
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

#if defined(CPU_AARCH32) || defined(CPU_AARCH64)
  // ARM instruction and data caches are not coherent, we need to flush after every block.
  FlushInstructionCache(m_free_far_code_ptr, length);
#endif

  Assert(length <= (m_far_code_size - m_far_code_used));
  m_free_far_code_ptr += length;
  m_far_code_used += length;
}

void JitCodeBuffer::Reset()
{
  WriteProtect(false);

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

  WriteProtect(true);
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

#if defined(__APPLE__) && defined(__aarch64__)

void JitCodeBuffer::WriteProtect(bool enabled)
{
  static bool initialized = false;
  static bool needs_write_protect = false;

  if (!initialized)
  {
    initialized = true;
    needs_write_protect = (pthread_jit_write_protect_supported_np() != 0);
    if (needs_write_protect)
      Log_InfoPrint("pthread_jit_write_protect_np() will be used before writing to JIT space.");
  }

  if (!needs_write_protect)
    return;

  pthread_jit_write_protect_np(enabled ? 1 : 0);
}

#endif