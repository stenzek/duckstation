// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once
#include "../types.h"
#include "loader.h"
#include <memory>

namespace Vulkan {

class StagingBuffer
{
public:
  enum class Type
  {
    Upload,
    Readback,
    Mutable
  };

  StagingBuffer();
  StagingBuffer(StagingBuffer&& move);
  StagingBuffer(const StagingBuffer&) = delete;
  virtual ~StagingBuffer();

  StagingBuffer& operator=(StagingBuffer&& move);
  StagingBuffer& operator=(const StagingBuffer&) = delete;

  ALWAYS_INLINE Type GetType() const { return m_type; }
  ALWAYS_INLINE VkDeviceSize GetSize() const { return m_size; }
  ALWAYS_INLINE VkBuffer GetBuffer() const { return m_buffer; }
  ALWAYS_INLINE bool IsMapped() const { return m_map_pointer != nullptr; }
  ALWAYS_INLINE const char* GetMapPointer() const { return m_map_pointer; }
  ALWAYS_INLINE char* GetMapPointer() { return m_map_pointer; }
  ALWAYS_INLINE VkDeviceSize GetMapOffset() const { return m_map_offset; }
  ALWAYS_INLINE VkDeviceSize GetMapSize() const { return m_map_size; }
  ALWAYS_INLINE bool IsValid() const { return (m_buffer != VK_NULL_HANDLE); }
  ALWAYS_INLINE bool IsCoherent() const { return m_coherent; }

  bool Map(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
  void Unmap();

  // Upload part 1: Prepare from device read from the CPU side
  void FlushCPUCache(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

  // Upload part 2: Prepare for device read from the GPU side
  // Implicit when submitting the command buffer, so rarely needed.
  void InvalidateGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits dst_access_flags,
                          VkPipelineStageFlagBits dst_pipeline_stage, VkDeviceSize offset = 0,
                          VkDeviceSize size = VK_WHOLE_SIZE);

  // Readback part 0: Prepare for GPU usage (if necessary)
  void PrepareForGPUWrite(VkCommandBuffer command_buffer, VkAccessFlagBits dst_access_flags,
                          VkPipelineStageFlagBits dst_pipeline_stage, VkDeviceSize offset = 0,
                          VkDeviceSize size = VK_WHOLE_SIZE);

  // Readback part 1: Prepare for host readback from the GPU side
  void FlushGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits src_access_flags,
                     VkPipelineStageFlagBits src_pipeline_stage, VkDeviceSize offset = 0,
                     VkDeviceSize size = VK_WHOLE_SIZE);

  // Readback part 2: Prepare for host readback from the CPU side
  void InvalidateCPUCache(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

  // offset is from the start of the buffer, not from the map offset
  void Read(VkDeviceSize offset, void* data, size_t size, bool invalidate_caches = true);
  void Write(VkDeviceSize offset, const void* data, size_t size, bool invalidate_caches = true);

  // Creates the optimal format of image copy.
  bool Create(Type type, VkDeviceSize size, VkBufferUsageFlags usage);

  void Destroy(bool defer = true);

  // Allocates the resources needed to create a staging buffer.
  static bool AllocateBuffer(Type type, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* out_buffer,
                             VkDeviceMemory* out_memory, bool* out_coherent);

protected:
  Type m_type = Type::Upload;
  VkBuffer m_buffer = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  VkDeviceSize m_size = 0;
  bool m_coherent = false;

  char* m_map_pointer = nullptr;
  VkDeviceSize m_map_offset = 0;
  VkDeviceSize m_map_size = 0;
};
} // namespace Vulkan
