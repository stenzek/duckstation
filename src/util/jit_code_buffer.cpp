// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "jit_code_buffer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/memmap.h"

#include <algorithm>
#include <cstring>

JitCodeBuffer::JitCodeBuffer() = default;

JitCodeBuffer::~JitCodeBuffer() = default;

void JitCodeBuffer::Reset(void* ptr, u32 size, u32 far_code_size /* = 0 */)
{
  Assert(far_code_size < size);

  m_total_size = size;
  m_code_ptr = static_cast<u8*>(ptr);
  m_free_code_ptr = m_code_ptr;
  m_code_size = size - far_code_size;
  m_code_used = 0;

  m_far_code_size = far_code_size;
  m_far_code_ptr = (far_code_size > 0) ? (static_cast<u8*>(m_code_ptr) + m_code_size) : nullptr;
  m_free_far_code_ptr = m_far_code_ptr;
  m_far_code_used = 0;

  MemMap::BeginCodeWrite();

  std::memset(m_code_ptr, 0, m_total_size);
  MemMap::FlushInstructionCache(m_code_ptr, m_total_size);

  MemMap::EndCodeWrite();
}

void JitCodeBuffer::CommitCode(u32 length)
{
  if (length == 0)
    return;

  MemMap::FlushInstructionCache(m_free_code_ptr, length);

  Assert(length <= (m_code_size - m_code_used));
  m_free_code_ptr += length;
  m_code_used += length;
}

void JitCodeBuffer::CommitFarCode(u32 length)
{
  if (length == 0)
    return;

  MemMap::FlushInstructionCache(m_free_far_code_ptr, length);

  Assert(length <= (m_far_code_size - m_far_code_used));
  m_free_far_code_ptr += length;
  m_far_code_used += length;
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
