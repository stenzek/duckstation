#pragma once
#include "../gpu_texture.h"
#include "loader.h"
#include <algorithm>
#include <memory>

namespace Vulkan {

class Texture final : public GPUTexture
{
public:
  Texture();
  Texture(Texture&& move);
  Texture(const Texture&) = delete;
  ~Texture();

  Texture& operator=(Texture&& move);
  Texture& operator=(const Texture&) = delete;

  static VkFormat GetVkFormat(Format format);
  static Format LookupBaseFormat(VkFormat vformat);

  bool IsValid() const override;

  /// An image is considered owned/managed if we control the memory.
  ALWAYS_INLINE bool IsOwned() const { return (m_allocation != VK_NULL_HANDLE); }

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetLevels() const { return m_levels; }
  ALWAYS_INLINE u32 GetLayers() const { return m_layers; }
  
  ALWAYS_INLINE VkFormat GetVkFormat() const { return GetVkFormat(m_format); }
  ALWAYS_INLINE VkSampleCountFlagBits GetVkSamples() const { return static_cast<VkSampleCountFlagBits>(m_samples); }
  ALWAYS_INLINE VkImageLayout GetLayout() const { return m_layout; }
  ALWAYS_INLINE VkImageViewType GetViewType() const { return m_view_type; }
  ALWAYS_INLINE VkImage GetImage() const { return m_image; }
  ALWAYS_INLINE VmaAllocation GetAllocation() const { return m_allocation; }
  ALWAYS_INLINE VkImageView GetView() const { return m_view; }

  bool Create(u32 width, u32 height, u32 levels, u32 layers, VkFormat format, VkSampleCountFlagBits samples,
              VkImageViewType view_type, VkImageTiling tiling, VkImageUsageFlags usage, bool dedicated_memory = false,
              const VkComponentMapping* swizzle = nullptr);

  bool Adopt(VkImage existing_image, VkImageViewType view_type, u32 width, u32 height, u32 levels, u32 layers,
             VkFormat format, VkSampleCountFlagBits samples, const VkComponentMapping* swizzle = nullptr);

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
                        VkBuffer buffer, u32 buffer_offset, u32 row_length);

  u32 CalcUpdatePitch(u32 width) const;
  u32 CalcUpdateRowLength(u32 pitch) const;
  bool BeginUpdate(u32 width, u32 height, void** out_buffer, u32* out_pitch);
  void EndUpdate(u32 x, u32 y, u32 width, u32 height, u32 level, u32 layer);
  bool Update(u32 x, u32 y, u32 width, u32 height, u32 level, u32 layer, const void* data, u32 data_pitch);

private:
  VkImageViewType m_view_type = VK_IMAGE_VIEW_TYPE_2D;
  VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage m_image = VK_NULL_HANDLE;
  VmaAllocation m_allocation = VK_NULL_HANDLE;
  VkImageView m_view = VK_NULL_HANDLE;
};

} // namespace Vulkan
