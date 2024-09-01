// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "vulkan_loader.h"

#include "common/types.h"

#include <deque>
#include <memory>

class VulkanStreamBuffer
{
public:
  VulkanStreamBuffer();
  VulkanStreamBuffer(VulkanStreamBuffer&& move);
  VulkanStreamBuffer(const VulkanStreamBuffer&) = delete;
  ~VulkanStreamBuffer();

  VulkanStreamBuffer& operator=(VulkanStreamBuffer&& move);
  VulkanStreamBuffer& operator=(const VulkanStreamBuffer&) = delete;

  ALWAYS_INLINE bool IsValid() const { return (m_buffer != VK_NULL_HANDLE); }
  ALWAYS_INLINE VkBuffer GetBuffer() const { return m_buffer; }
  ALWAYS_INLINE const VkBuffer* GetBufferPtr() const { return &m_buffer; }
  ALWAYS_INLINE u8* GetHostPointer() const { return m_host_pointer; }
  ALWAYS_INLINE u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
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

  u32 m_size = 0;
  u32 m_current_offset = 0;
  u32 m_current_space = 0;
  u32 m_current_gpu_position = 0;

  VmaAllocation m_allocation = VK_NULL_HANDLE;
  VkBuffer m_buffer = VK_NULL_HANDLE;
  u8* m_host_pointer = nullptr;

  // List of fences and the corresponding positions in the buffer
  std::deque<std::pair<u64, u32>> m_tracked_fences;
};
