// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#include "staging_texture.h"
#include "../assert.h"
#include "context.h"
#include "util.h"

namespace Vulkan {

StagingTexture::StagingTexture() = default;

StagingTexture::StagingTexture(StagingTexture&& move)
  : m_staging_buffer(std::move(move.m_staging_buffer)), m_flush_fence_counter(move.m_flush_fence_counter),
    m_width(move.m_width), m_height(move.m_height), m_texel_size(move.m_texel_size), m_map_stride(move.m_map_stride)
{
  move.m_flush_fence_counter = 0;
  move.m_width = 0;
  move.m_height = 0;
  move.m_texel_size = 0;
  move.m_map_stride = 0;
}

StagingTexture& StagingTexture::operator=(StagingTexture&& move)
{
  if (IsValid())
    Destroy(true);

  std::swap(m_staging_buffer, move.m_staging_buffer);
  std::swap(m_flush_fence_counter, move.m_flush_fence_counter);
  std::swap(m_width, move.m_width);
  std::swap(m_height, move.m_height);
  std::swap(m_texel_size, move.m_texel_size);
  std::swap(m_map_stride, move.m_map_stride);
  return *this;
}

StagingTexture::~StagingTexture()
{
  if (IsValid())
    Destroy(true);
}

bool StagingTexture::Create(StagingBuffer::Type type, VkFormat format, u32 width, u32 height)
{
  const u32 texel_size = Util::GetTexelSize(format);
  const u32 map_stride = texel_size * width;
  const u32 buffer_size = map_stride * height;

  VkBufferUsageFlags usage_flags;
  switch (type)
  {
    case StagingBuffer::Type::Readback:
      usage_flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      break;
    case StagingBuffer::Type::Upload:
      usage_flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      break;
    case StagingBuffer::Type::Mutable:
    default:
      usage_flags = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      break;
  }

  StagingBuffer new_buffer;
  if (!new_buffer.Create(type, buffer_size, usage_flags) || !new_buffer.Map())
    return false;

  if (IsValid())
    Destroy(true);

  m_staging_buffer = std::move(new_buffer);
  m_width = width;
  m_height = height;
  m_texel_size = texel_size;
  m_map_stride = map_stride;
  return true;
}

void StagingTexture::Destroy(bool defer /* = true */)
{
  if (!IsValid())
    return;

  m_staging_buffer.Destroy(defer);
  m_flush_fence_counter = 0;
  m_width = 0;
  m_height = 0;
  m_texel_size = 0;
  m_map_stride = 0;
}

void StagingTexture::CopyFromTexture(VkCommandBuffer command_buffer, Texture& src_texture, u32 src_x, u32 src_y,
                                     u32 src_layer, u32 src_level, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  Assert(m_staging_buffer.GetType() == StagingBuffer::Type::Readback ||
         m_staging_buffer.GetType() == StagingBuffer::Type::Mutable);
  Assert((src_x + width) <= src_texture.GetWidth() && (src_y + height) <= src_texture.GetHeight());
  Assert((dst_x + width) <= m_width && (dst_y + height) <= m_height);

  const Vulkan::Util::DebugScope debugScope(command_buffer,
                                            "StagingTexture::CopyFromTexture: {%u,%u} Lyr:%u Lvl:%u {%u,%u} %ux%u",
                                            src_x, src_y, src_layer, src_level, dst_x, dst_y, width, height);

  VkImageLayout old_layout = src_texture.GetLayout();
  src_texture.TransitionToLayout(command_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

  // Issue the image->buffer copy, but delay it for now.
  VkBufferImageCopy image_copy = {};
  const VkImageAspectFlags aspect =
    Util ::IsDepthFormat(src_texture.GetFormat()) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  image_copy.bufferOffset = static_cast<VkDeviceSize>(dst_y * m_map_stride + dst_x * m_texel_size);
  image_copy.bufferRowLength = m_width;
  image_copy.bufferImageHeight = 0;
  image_copy.imageSubresource = {aspect, src_level, src_layer, 1};
  image_copy.imageOffset = {static_cast<int32_t>(src_x), static_cast<int32_t>(src_y), 0};
  image_copy.imageExtent = {width, height, 1u};
  vkCmdCopyImageToBuffer(command_buffer, src_texture.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         m_staging_buffer.GetBuffer(), 1, &image_copy);

  // Restore old source texture layout.
  src_texture.TransitionToLayout(command_buffer, old_layout);
}

void StagingTexture::CopyFromTexture(Texture& src_texture, u32 src_x, u32 src_y, u32 src_layer, u32 src_level,
                                     u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetCurrentCommandBuffer(),
                                            "StagingTexture::CopyFromTexture: {%u,%u} Lyr:%u Lvl:%u {%u,%u} %ux%u",
                                            src_x, src_y, src_layer, src_level, dst_x, dst_y, width, height);
  CopyFromTexture(g_vulkan_context->GetCurrentCommandBuffer(), src_texture, src_x, src_y, src_layer, src_level, dst_x,
                  dst_y, width, height);

  m_needs_flush = true;
  m_flush_fence_counter = g_vulkan_context->GetCurrentFenceCounter();
}

void StagingTexture::CopyToTexture(VkCommandBuffer command_buffer, u32 src_x, u32 src_y, Texture& dst_texture,
                                   u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, u32 width, u32 height)
{
  Assert(m_staging_buffer.GetType() == StagingBuffer::Type::Upload ||
         m_staging_buffer.GetType() == StagingBuffer::Type::Mutable);
  Assert((dst_x + width) <= dst_texture.GetWidth() && (dst_y + height) <= dst_texture.GetHeight());
  Assert((src_x + width) <= m_width && (src_y + height) <= m_height);

  // Flush caches before copying.
  m_staging_buffer.FlushCPUCache();

  VkImageLayout old_layout = dst_texture.GetLayout();
  dst_texture.TransitionToLayout(command_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  // Issue the image->buffer copy, but delay it for now.
  VkBufferImageCopy image_copy = {};
  image_copy.bufferOffset = static_cast<VkDeviceSize>(src_y * m_map_stride + src_x * m_texel_size);
  image_copy.bufferRowLength = m_width;
  image_copy.bufferImageHeight = 0;
  image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_level, dst_layer, 1};
  image_copy.imageOffset = {static_cast<int32_t>(dst_x), static_cast<int32_t>(dst_y), 0};
  image_copy.imageExtent = {width, height, 1u};
  vkCmdCopyBufferToImage(command_buffer, m_staging_buffer.GetBuffer(), dst_texture.GetImage(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);

  // Restore old source texture layout.
  dst_texture.TransitionToLayout(command_buffer, old_layout);
}

void StagingTexture::CopyToTexture(u32 src_x, u32 src_y, Texture& dst_texture, u32 dst_x, u32 dst_y, u32 dst_layer,
                                   u32 dst_level, u32 width, u32 height)
{
  const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetCurrentCommandBuffer(),
                                            "StagingTexture::CopyToTexture: {%u,%u} | {%u,%u} Lyr:%u Lvl:%u %ux%u",
                                            src_x, src_y, dst_x, dst_y, dst_layer, dst_level, width, height);
  CopyToTexture(g_vulkan_context->GetCurrentCommandBuffer(), src_x, src_y, dst_texture, dst_x, dst_y, dst_layer,
                dst_level, width, height);

  m_needs_flush = true;
  m_flush_fence_counter = g_vulkan_context->GetCurrentFenceCounter();
}

void StagingTexture::Flush()
{
  if (!m_needs_flush)
    return;

  // Is this copy in the current command buffer?
  if (g_vulkan_context->GetCurrentFenceCounter() == m_flush_fence_counter)
  {
    // Execute the command buffer and wait for it to finish.
    g_vulkan_context->ExecuteCommandBuffer(true);
  }
  else
  {
    // Wait for the GPU to finish with it.
    g_vulkan_context->WaitForFenceCounter(m_flush_fence_counter);
  }

  // For readback textures, invalidate the CPU cache as there is new data there.
  if (m_staging_buffer.GetType() == StagingBuffer::Type::Readback ||
      m_staging_buffer.GetType() == StagingBuffer::Type::Mutable)
  {
    m_staging_buffer.InvalidateCPUCache();
  }

  m_needs_flush = false;
}

void StagingTexture::ReadTexels(u32 src_x, u32 src_y, u32 width, u32 height, void* out_ptr, u32 out_stride)
{
  Assert(m_staging_buffer.GetType() != StagingBuffer::Type::Upload);
  Assert((src_x + width) <= m_width && (src_y + height) <= m_height);
  PrepareForAccess();

  // Offset pointer to point to start of region being copied out.
  const char* current_ptr = m_staging_buffer.GetMapPointer();
  current_ptr += src_y * m_map_stride;
  current_ptr += src_x * m_texel_size;

  // Optimal path: same dimensions, same stride.
  if (src_x == 0 && width == m_width && m_map_stride == out_stride)
  {
    std::memcpy(out_ptr, current_ptr, m_map_stride * height);
    return;
  }

  size_t copy_size = std::min<u32>(width * m_texel_size, m_map_stride);
  char* dst_ptr = reinterpret_cast<char*>(out_ptr);
  for (u32 row = 0; row < height; row++)
  {
    std::memcpy(dst_ptr, current_ptr, copy_size);
    current_ptr += m_map_stride;
    dst_ptr += out_stride;
  }
}

void StagingTexture::ReadTexel(u32 x, u32 y, void* out_ptr)
{
  Assert(m_staging_buffer.GetType() != StagingBuffer::Type::Upload);
  Assert(x < m_width && y < m_height);
  PrepareForAccess();

  const char* src_ptr = GetMappedPointer() + y * GetMappedStride() + x * m_texel_size;
  std::memcpy(out_ptr, src_ptr, m_texel_size);
}

void StagingTexture::WriteTexels(u32 dst_x, u32 dst_y, u32 width, u32 height, const void* in_ptr, u32 in_stride)
{
  Assert(m_staging_buffer.GetType() != StagingBuffer::Type::Readback);
  Assert((dst_x + width) <= m_width && (dst_y + height) <= m_height);
  PrepareForAccess();

  // Offset pointer to point to start of region being copied to.
  char* current_ptr = GetMappedPointer();
  current_ptr += dst_y * m_map_stride;
  current_ptr += dst_x * m_texel_size;

  // Optimal path: same dimensions, same stride.
  if (dst_x == 0 && width == m_width && m_map_stride == in_stride)
  {
    std::memcpy(current_ptr, in_ptr, m_map_stride * height);
    return;
  }

  size_t copy_size = std::min<u32>(width * m_texel_size, m_map_stride);
  const char* src_ptr = reinterpret_cast<const char*>(in_ptr);
  for (u32 row = 0; row < height; row++)
  {
    std::memcpy(current_ptr, src_ptr, copy_size);
    current_ptr += m_map_stride;
    src_ptr += in_stride;
  }
}

void StagingTexture::WriteTexel(u32 x, u32 y, const void* in_ptr)
{
  Assert(x < m_width && y < m_height);
  PrepareForAccess();

  char* dest_ptr = GetMappedPointer() + y * m_map_stride + x * m_texel_size;
  std::memcpy(dest_ptr, in_ptr, m_texel_size);
}

void StagingTexture::PrepareForAccess()
{
  Assert(IsMapped());
  if (m_needs_flush)
    Flush();
}

} // namespace Vulkan