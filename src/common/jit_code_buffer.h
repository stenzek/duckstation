#pragma once
#include "types.h"

class JitCodeBuffer
{
public:
  JitCodeBuffer(size_t size = 64 * 1024 * 1024);
  ~JitCodeBuffer();

  void* GetFreeCodePointer() const { return m_free_code_ptr; }
  size_t GetFreeCodeSpace() const { return (m_code_size - m_code_used); }
  void CommitCode(size_t length);
  void Reset();

  /// Adjusts the free code pointer to the specified alignment, padding with bytes.
  /// Assumes alignment is a power-of-two.
  void Align(u32 alignment, u8 padding_value);

private:
  void* m_code_ptr;
  void* m_free_code_ptr;
  size_t m_code_size;
  size_t m_code_used;
};

