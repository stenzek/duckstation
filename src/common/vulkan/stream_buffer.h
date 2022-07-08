// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include "../types.h"
#include "loader.h"
#include <deque>
#include <memory>

namespace Vulkan {

class StreamBuffer
{
public:
  StreamBuffer();
  StreamBuffer(StreamBuffer&& move);
  StreamBuffer(const StreamBuffer&) = delete;
  ~StreamBuffer();

  StreamBuffer& operator=(StreamBuffer&& move);
  StreamBuffer& operator=(const StreamBuffer&) = delete;

  ALWAYS_INLINE bool IsValid() const { return (m_buffer != VK_NULL_HANDLE); }
  ALWAYS_INLINE VkBuffer GetBuffer() const { return m_buffer; }
  ALWAYS_INLINE const VkBuffer* GetBufferPointer() const { return &m_buffer; }
  ALWAYS_INLINE VkDeviceMemory GetDeviceMemory() const { return m_memory; }
  ALWAYS_INLINE void* GetHostPointer() const { return m_host_pointer; }
  ALWAYS_INLINE void* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
  ALWAYS_INLINE u32 GetCurrentSize() const { return m_size; }
  ALWAYS_INLINE u32 GetCurrentSpace() const { return m_current_space; }
  ALWAYS_INLINE u32 GetCurrentOffset() const { return m_current_offset; }

  bool Create(VkBufferUsageFlags usage, u32 size);
  void Destroy(bool defer);

  bool ReserveMemory(u32 num_bytes, u32 alignment);
  void CommitMemory(u32 final_num_bytes);

private:
  bool AllocateBuffer(VkBufferUsageFlags usage, u32 size);
  void UpdateCurrentFencePosition();
  void UpdateGPUPosition();

  // Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
  bool WaitForClearSpace(u32 num_bytes);

  VkBufferUsageFlags m_usage = 0;
  u32 m_size = 0;
  u32 m_current_offset = 0;
  u32 m_current_space = 0;
  u32 m_current_gpu_position = 0;

  VkBuffer m_buffer = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  u8* m_host_pointer = nullptr;

  // List of fences and the corresponding positions in the buffer
  std::deque<std::pair<u64, u32>> m_tracked_fences;

  bool m_coherent_mapping = false;
};

} // namespace Vulkan
