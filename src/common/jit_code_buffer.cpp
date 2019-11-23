#include "jit_code_buffer.h"
#include "YBaseLib/Assert.h"

#if defined(Y_PLATFORM_WINDOWS)
#include "YBaseLib/Windows/WindowsHeaders.h"
#elif defined(Y_PLATFORM_LINUX) || defined(Y_PLATFORM_ANDROID)
#include <sys/mman.h>
#endif

JitCodeBuffer::JitCodeBuffer(size_t size /* = 64 * 1024 * 1024 */, size_t far_code_size /* = 0 */)
{
  m_total_size = size + far_code_size;

#if defined(Y_PLATFORM_WINDOWS)
  m_code_ptr = VirtualAlloc(nullptr, m_total_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#elif defined(Y_PLATFORM_LINUX) || defined(Y_PLATFORM_ANDROID)
  m_code_ptr = mmap(nullptr, m_total_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
  m_code_ptr = nullptr;
#endif
  m_free_code_ptr = m_code_ptr;
  m_code_size = size;
  m_code_used = 0;

  m_far_code_ptr = static_cast<u8*>(m_code_ptr) + size;
  m_far_code_size = far_code_size;
  m_far_code_used = 0;

  if (!m_code_ptr)
    Panic("Failed to allocate code space.");
}

JitCodeBuffer::~JitCodeBuffer()
{
#if defined(Y_PLATFORM_WINDOWS)
  VirtualFree(m_code_ptr, m_total_size, MEM_RELEASE);
#elif defined(Y_PLATFORM_LINUX) || defined(Y_PLATFORM_ANDROID)
  munmap(m_code_ptr, m_total_size);
#endif
}

void JitCodeBuffer::CommitCode(size_t length)
{
  Assert(length <= (m_code_size - m_code_used));
  m_free_code_ptr = reinterpret_cast<u8*>(m_free_code_ptr) + length;
  m_code_used += length;
}

void JitCodeBuffer::CommitFarCode(size_t length)
{
  Assert(length <= (m_far_code_size - m_far_code_used));
  m_free_far_code_ptr = reinterpret_cast<u8*>(m_free_far_code_ptr) + length;
  m_far_code_used += length;
}

void JitCodeBuffer::Reset()
{
#if defined(Y_PLATFORM_WINDOWS)
  FlushInstructionCache(GetCurrentProcess(), m_code_ptr, m_total_size);
#elif defined(Y_PLATFORM_LINUX) || defined(Y_PLATFORM_ANDROID)
// TODO
#endif

  m_free_code_ptr = m_code_ptr;
  m_code_used = 0;

  m_free_far_code_ptr = m_far_code_ptr;
  m_far_code_used = 0;
}

void JitCodeBuffer::Align(u32 alignment, u8 padding_value)
{
  DebugAssert(Common::IsPow2(alignment));
  const size_t num_padding_bytes =
    std::min(static_cast<size_t>(Common::AlignUpPow2(reinterpret_cast<uintptr_t>(m_free_code_ptr), alignment) -
                                 reinterpret_cast<uintptr_t>(m_free_code_ptr)),
             GetFreeCodeSpace());
  std::memset(m_free_code_ptr, padding_value, num_padding_bytes);
  m_free_code_ptr = reinterpret_cast<char*>(m_free_code_ptr) + num_padding_bytes;
  m_code_used += num_padding_bytes;
}
