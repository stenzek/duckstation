// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/types.h"

class JitCodeBuffer
{
public:
  JitCodeBuffer();
  JitCodeBuffer(u32 size, u32 far_code_size);
  JitCodeBuffer(void* buffer, u32 size, u32 far_code_size, u32 guard_size);
  ~JitCodeBuffer();

  bool IsValid() const { return (m_code_ptr != nullptr); }

  bool Allocate(u32 size = 64 * 1024 * 1024, u32 far_code_size = 0);
  bool Initialize(void* buffer, u32 size, u32 far_code_size = 0, u32 guard_size = 0);
  void Destroy();
  void Reset();

  ALWAYS_INLINE u8* GetCodePointer() const { return m_code_ptr; }
  ALWAYS_INLINE u32 GetTotalSize() const { return m_total_size; }
  ALWAYS_INLINE float GetUsedPct() const
  {
    return (static_cast<float>(m_code_used) / static_cast<float>(m_code_size)) * 100.0f;
  }
  ALWAYS_INLINE float GetFarUsedPct() const
  {
    return (static_cast<float>(m_far_code_used) / static_cast<float>(m_far_code_size)) * 100.0f;
  }
  ALWAYS_INLINE u32 GetTotalUsed() const { return m_code_used + m_far_code_used; }

  ALWAYS_INLINE u8* GetFreeCodePointer() const { return m_free_code_ptr; }
  ALWAYS_INLINE u32 GetFreeCodeSpace() const { return static_cast<u32>(m_code_size - m_code_used); }
  void ReserveCode(u32 size);
  void CommitCode(u32 length);

  ALWAYS_INLINE u8* GetFreeFarCodePointer() const { return m_free_far_code_ptr; }
  ALWAYS_INLINE u32 GetFreeFarCodeSpace() const { return static_cast<u32>(m_far_code_size - m_far_code_used); }
  void CommitFarCode(u32 length);

  /// Adjusts the free code pointer to the specified alignment, padding with bytes.
  /// Assumes alignment is a power-of-two.
  void Align(u32 alignment, u8 padding_value);

  /// Flushes the instruction cache on the host for the specified range.
  static void FlushInstructionCache(void* address, u32 size);

private:
  bool TryAllocateAt(const void* addr);

  u8* m_code_ptr = nullptr;
  u8* m_free_code_ptr = nullptr;
  u32 m_code_size = 0;
  u32 m_code_reserve_size = 0;
  u32 m_code_used = 0;

  u8* m_far_code_ptr = nullptr;
  u8* m_free_far_code_ptr = nullptr;
  u32 m_far_code_size = 0;
  u32 m_far_code_used = 0;

  u32 m_total_size = 0;
  u32 m_guard_size = 0;
  u32 m_old_protection = 0;
  bool m_owns_buffer = false;
};
