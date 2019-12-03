#pragma once
#include "types.h"

class JitCodeBuffer
{
public:
  JitCodeBuffer(u32 size = 64 * 1024 * 1024, u32 far_code_size = 0);
  ~JitCodeBuffer();

  void Reset();

  u8* GetFreeCodePointer() const { return m_free_code_ptr; }
  u32 GetFreeCodeSpace() const { return static_cast<u32>(m_code_size - m_code_used); }
  void CommitCode(u32 length);

  u8* GetFreeFarCodePointer() const { return m_free_far_code_ptr; }
  u32 GetFreeFarCodeSpace() const { return static_cast<u32>(m_far_code_size - m_far_code_used); }
  void CommitFarCode(u32 length);

  /// Adjusts the free code pointer to the specified alignment, padding with bytes.
  /// Assumes alignment is a power-of-two.
  void Align(u32 alignment, u8 padding_value);

private:
  u8* m_code_ptr;
  u8* m_free_code_ptr;
  u32 m_code_size;
  u32 m_code_used;

  u8* m_far_code_ptr;
  u8* m_free_far_code_ptr;
  u32 m_far_code_size;
  u32 m_far_code_used;

  u32 m_total_size;
};

