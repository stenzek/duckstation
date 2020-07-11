// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#include "staging_buffer.h"
#include "../assert.h"
#include "context.h"
#include "util.h"

namespace Vulkan {
StagingBuffer::StagingBuffer() = default;

StagingBuffer::StagingBuffer(StagingBuffer&& move)
  : m_type(move.m_type), m_buffer(move.m_buffer), m_memory(move.m_memory), m_size(move.m_size),
    m_coherent(move.m_coherent), m_map_pointer(move.m_map_pointer), m_map_offset(move.m_map_offset),
    m_map_size(move.m_map_size)
{
  move.m_type = Type::Upload;
  move.m_buffer = VK_NULL_HANDLE;
  move.m_memory = VK_NULL_HANDLE;
  move.m_size = 0;
  move.m_coherent = false;
  move.m_map_pointer = nullptr;
  move.m_map_offset = 0;
  move.m_map_size = 0;
}

StagingBuffer::~StagingBuffer()
{
  if (IsValid())
    Destroy(true);
}

StagingBuffer& StagingBuffer::operator=(StagingBuffer&& move)
{
  if (IsValid())
    Destroy(true);

  std::swap(m_type, move.m_type);
  std::swap(m_buffer, move.m_buffer);
  std::swap(m_memory, move.m_memory);
  std::swap(m_size, move.m_size);
  std::swap(m_coherent, move.m_coherent);
  std::swap(m_map_pointer, move.m_map_pointer);
  std::swap(m_map_offset, move.m_map_offset);
  std::swap(m_map_size, move.m_map_size);
  return *this;
}

bool StagingBuffer::Map(VkDeviceSize offset, VkDeviceSize size)
{
  m_map_offset = offset;
  if (size == VK_WHOLE_SIZE)
    m_map_size = m_size - offset;
  else
    m_map_size = size;

  Assert(!m_map_pointer);
  Assert(m_map_offset + m_map_size <= m_size);

  void* map_pointer;
  VkResult res = vkMapMemory(g_vulkan_context->GetDevice(), m_memory, m_map_offset, m_map_size, 0, &map_pointer);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkMapMemory failed: ");
    return false;
  }

  m_map_pointer = reinterpret_cast<char*>(map_pointer);
  return true;
}

void StagingBuffer::Unmap()
{
  Assert(m_map_pointer);

  vkUnmapMemory(g_vulkan_context->GetDevice(), m_memory);
  m_map_pointer = nullptr;
  m_map_offset = 0;
  m_map_size = 0;
}

void StagingBuffer::FlushCPUCache(VkDeviceSize offset, VkDeviceSize size)
{
  Assert(offset >= m_map_offset);
  if (m_coherent || !IsMapped())
    return;

  VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, m_memory, offset - m_map_offset, size};
  vkFlushMappedMemoryRanges(g_vulkan_context->GetDevice(), 1, &range);
}

void StagingBuffer::InvalidateGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits dest_access_flags,
                                       VkPipelineStageFlagBits dest_pipeline_stage, VkDeviceSize offset,
                                       VkDeviceSize size)
{
  if (m_coherent)
    return;

  Assert((offset + size) <= m_size || (offset < m_size && size == VK_WHOLE_SIZE));
  Util::BufferMemoryBarrier(command_buffer, m_buffer, VK_ACCESS_HOST_WRITE_BIT, dest_access_flags, offset, size,
                            VK_PIPELINE_STAGE_HOST_BIT, dest_pipeline_stage);
}

void StagingBuffer::PrepareForGPUWrite(VkCommandBuffer command_buffer, VkAccessFlagBits dst_access_flags,
                                       VkPipelineStageFlagBits dst_pipeline_stage, VkDeviceSize offset,
                                       VkDeviceSize size)
{
  if (m_coherent)
    return;

  Assert((offset + size) <= m_size || (offset < m_size && size == VK_WHOLE_SIZE));
  Util::BufferMemoryBarrier(command_buffer, m_buffer, 0, dst_access_flags, offset, size,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst_pipeline_stage);
}

void StagingBuffer::FlushGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits src_access_flags,
                                  VkPipelineStageFlagBits src_pipeline_stage, VkDeviceSize offset, VkDeviceSize size)
{
  if (m_coherent)
    return;

  Assert((offset + size) <= m_size || (offset < m_size && size == VK_WHOLE_SIZE));
  Util::BufferMemoryBarrier(command_buffer, m_buffer, src_access_flags, VK_ACCESS_HOST_READ_BIT, offset, size,
                            src_pipeline_stage, VK_PIPELINE_STAGE_HOST_BIT);
}

void StagingBuffer::InvalidateCPUCache(VkDeviceSize offset, VkDeviceSize size)
{
  Assert(offset >= m_map_offset);
  if (m_coherent || !IsMapped())
    return;

  VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, m_memory, offset - m_map_offset, size};
  vkInvalidateMappedMemoryRanges(g_vulkan_context->GetDevice(), 1, &range);
}

void StagingBuffer::Read(VkDeviceSize offset, void* data, size_t size, bool invalidate_caches)
{
  Assert((offset + size) <= m_size);
  Assert(offset >= m_map_offset && size <= (m_map_size + (offset - m_map_offset)));
  if (invalidate_caches)
    InvalidateCPUCache(offset, size);

  memcpy(data, m_map_pointer + (offset - m_map_offset), size);
}

void StagingBuffer::Write(VkDeviceSize offset, const void* data, size_t size, bool invalidate_caches)
{
  Assert((offset + size) <= m_size);
  Assert(offset >= m_map_offset && size <= (m_map_size + (offset - m_map_offset)));

  memcpy(m_map_pointer + (offset - m_map_offset), data, size);
  if (invalidate_caches)
    FlushCPUCache(offset, size);
}

bool StagingBuffer::AllocateBuffer(Type type, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* out_buffer,
                                   VkDeviceMemory* out_memory, bool* out_coherent)
{
  VkBufferCreateInfo buffer_create_info = {
    VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, // VkStructureType        sType
    nullptr,                              // const void*            pNext
    0,                                    // VkBufferCreateFlags    flags
    size,                                 // VkDeviceSize           size
    usage,                                // VkBufferUsageFlags     usage
    VK_SHARING_MODE_EXCLUSIVE,            // VkSharingMode          sharingMode
    0,                                    // uint32_t               queueFamilyIndexCount
    nullptr                               // const uint32_t*        pQueueFamilyIndices
  };
  VkResult res = vkCreateBuffer(g_vulkan_context->GetDevice(), &buffer_create_info, nullptr, out_buffer);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateBuffer failed: ");
    return false;
  }

  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(g_vulkan_context->GetDevice(), *out_buffer, &requirements);

  u32 type_index;
  if (type == Type::Upload)
    type_index = g_vulkan_context->GetUploadMemoryType(requirements.memoryTypeBits, out_coherent);
  else
    type_index = g_vulkan_context->GetReadbackMemoryType(requirements.memoryTypeBits, out_coherent);

  VkMemoryAllocateInfo memory_allocate_info = {
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, // VkStructureType    sType
    nullptr,                                // const void*        pNext
    requirements.size,                      // VkDeviceSize       allocationSize
    type_index                              // uint32_t           memoryTypeIndex
  };
  res = vkAllocateMemory(g_vulkan_context->GetDevice(), &memory_allocate_info, nullptr, out_memory);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkAllocateMemory failed: ");
    vkDestroyBuffer(g_vulkan_context->GetDevice(), *out_buffer, nullptr);
    return false;
  }

  res = vkBindBufferMemory(g_vulkan_context->GetDevice(), *out_buffer, *out_memory, 0);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkBindBufferMemory failed: ");
    vkDestroyBuffer(g_vulkan_context->GetDevice(), *out_buffer, nullptr);
    vkFreeMemory(g_vulkan_context->GetDevice(), *out_memory, nullptr);
    return false;
  }

  return true;
}

bool StagingBuffer::Create(Type type, VkDeviceSize size, VkBufferUsageFlags usage)
{
  if (!AllocateBuffer(type, size, usage, &m_buffer, &m_memory, &m_coherent))
    return false;

  m_type = type;
  m_size = size;
  return true;
}

void StagingBuffer::Destroy(bool defer /* = true */)
{
  if (!IsValid())
    return;

  // Unmap before destroying
  if (m_map_pointer)
    Unmap();

  if (defer)
    g_vulkan_context->DeferBufferDestruction(m_buffer);
  else
    vkDestroyBuffer(g_vulkan_context->GetDevice(), m_buffer, nullptr);

  if (defer)
    g_vulkan_context->DeferDeviceMemoryDestruction(m_memory);
  else
    vkFreeMemory(g_vulkan_context->GetDevice(), m_memory, nullptr);

  m_type = Type::Upload;
  m_buffer = VK_NULL_HANDLE;
  m_memory = VK_NULL_HANDLE;
  m_size = 0;
  m_coherent = false;
  m_map_pointer = nullptr;
  m_map_offset = 0;
  m_map_size = 0;
}

} // namespace Vulkan
