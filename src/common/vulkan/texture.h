// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once
#include "../types.h"
#include "loader.h"
#include <algorithm>
#include <memory>

namespace Vulkan {
class Texture
{
public:
  Texture();
  Texture(Texture&& move);
  Texture(const Texture&) = delete;
  ~Texture();

  Texture& operator=(Texture&& move);
  Texture& operator=(const Texture&) = delete;

  ALWAYS_INLINE bool IsValid() const { return (m_image != VK_NULL_HANDLE); }

  /// An image is considered owned/managed if we control the memory.
  ALWAYS_INLINE bool IsOwned() const { return (m_device_memory != VK_NULL_HANDLE); }

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetLevels() const { return m_levels; }
  ALWAYS_INLINE u32 GetLayers() const { return m_layers; }
  ALWAYS_INLINE u32 GetMipWidth(u32 level) const { return std::max<u32>(m_width >> level, 1u); }
  ALWAYS_INLINE u32 GetMipHeight(u32 level) const { return std::max<u32>(m_height >> level, 1u); }
  ALWAYS_INLINE VkFormat GetFormat() const { return m_format; }
  ALWAYS_INLINE VkSampleCountFlagBits GetSamples() const { return m_samples; }
  ALWAYS_INLINE VkImageLayout GetLayout() const { return m_layout; }
  ALWAYS_INLINE VkImageViewType GetViewType() const { return m_view_type; }
  ALWAYS_INLINE VkImage GetImage() const { return m_image; }
  ALWAYS_INLINE VkDeviceMemory GetDeviceMemory() const { return m_device_memory; }
  ALWAYS_INLINE VkImageView GetView() const { return m_view; }

  bool Create(u32 width, u32 height, u32 levels, u32 layers, VkFormat format, VkSampleCountFlagBits samples,
              VkImageViewType view_type, VkImageTiling tiling, VkImageUsageFlags usage);

  bool Adopt(VkImage existing_image, VkImageViewType view_type, u32 width, u32 height, u32 levels, u32 layers,
             VkFormat format, VkSampleCountFlagBits samples);

  void Destroy(bool defer = true);

  // Used when the render pass is changing the image layout, or to force it to
  // VK_IMAGE_LAYOUT_UNDEFINED, if the existing contents of the image is
  // irrelevant and will not be loaded.
  void OverrideImageLayout(VkImageLayout new_layout);

  void TransitionToLayout(VkCommandBuffer command_buffer, VkImageLayout new_layout);
  void TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, u32 start_level, u32 num_levels, u32 start_layer,
                                      u32 num_layers, VkImageLayout old_layout, VkImageLayout new_layout);

  VkFramebuffer CreateFramebuffer(VkRenderPass render_pass);

  void UpdateFromBuffer(VkCommandBuffer cmdbuf, u32 level, u32 layer, u32 x, u32 y, u32 width, u32 height,
                        VkBuffer buffer, u32 buffer_offset);

private:
  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_levels = 0;
  u32 m_layers = 0;
  VkFormat m_format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
  VkImageViewType m_view_type = VK_IMAGE_VIEW_TYPE_2D;
  VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage m_image = VK_NULL_HANDLE;
  VkDeviceMemory m_device_memory = VK_NULL_HANDLE;
  VkImageView m_view = VK_NULL_HANDLE;
};

} // namespace Vulkan
