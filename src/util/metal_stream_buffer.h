// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>

#ifndef __OBJC__
#error This file needs to be compiled with Objective C++.
#endif

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

#include <deque>
#include <memory>

class MetalStreamBuffer
{
public:
  MetalStreamBuffer();
  MetalStreamBuffer(MetalStreamBuffer&& move) = delete;
  MetalStreamBuffer(const MetalStreamBuffer&) = delete;
  ~MetalStreamBuffer();

  MetalStreamBuffer& operator=(MetalStreamBuffer&& move) = delete;
  MetalStreamBuffer& operator=(const MetalStreamBuffer&) = delete;

  ALWAYS_INLINE bool IsValid() const { return (m_buffer != nil); }
  ALWAYS_INLINE id<MTLBuffer> GetBuffer() const { return m_buffer; }
  ALWAYS_INLINE u8* GetHostPointer() const { return m_host_pointer; }
  ALWAYS_INLINE u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
  ALWAYS_INLINE u32 GetCurrentSize() const { return m_size; }
  ALWAYS_INLINE u32 GetCurrentSpace() const { return m_current_space; }
  ALWAYS_INLINE u32 GetCurrentOffset() const { return m_current_offset; }

  bool Create(id<MTLDevice> device, u32 size);
  void Destroy();

  bool ReserveMemory(u32 num_bytes, u32 alignment);
  void CommitMemory(u32 final_num_bytes);

private:
  bool AllocateBuffer(u32 size);
  void UpdateCurrentFencePosition();
  void UpdateGPUPosition();

  // Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
  bool WaitForClearSpace(u32 num_bytes);

  u32 m_size = 0;
  u32 m_current_offset = 0;
  u32 m_current_space = 0;
  u32 m_current_gpu_position = 0;

  id<MTLBuffer> m_buffer = nil;
  u8* m_host_pointer = nullptr;

  // List of fences and the corresponding positions in the buffer
  std::deque<std::pair<u64, u32>> m_tracked_fences;
};
