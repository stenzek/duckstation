// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"
#include "gpu_texture.h"
#include "vulkan_loader.h"
#include "vulkan_stream_buffer.h"

#include <limits>
#include <memory>

class VulkanDevice;

class VulkanTexture final : public GPUTexture
{
public:
  enum class Layout : u32
  {
    Undefined,
    Preinitialized,
    ColorAttachment,
    DepthStencilAttachment,
    ShaderReadOnly,
    ClearDst,
    TransferSrc,
    TransferDst,
    TransferSelf,
    PresentSrc,
    FeedbackLoop,
    ReadWriteImage,
    ComputeReadWriteImage,
    General,
    Count
  };

  ~VulkanTexture() override;

  static std::unique_ptr<VulkanTexture> Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type,
                                               Format format, VkFormat vk_format);
  void Destroy(bool defer);

  ALWAYS_INLINE VkImage GetImage() const { return m_image; }
  ALWAYS_INLINE VkImageView GetView() const { return m_view; }
  ALWAYS_INLINE Layout GetLayout() const { return m_layout; }
  ALWAYS_INLINE VkFormat GetVkFormat() const { return m_vk_format; }

  VkImageLayout GetVkLayout() const;
  VkClearColorValue GetClearColorValue() const;
  VkClearDepthStencilValue GetClearDepthValue() const;

  bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0, u32 level = 0) override;
  bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) override;
  void Unmap() override;
  void MakeReadyForSampling() override;

  void SetDebugName(const std::string_view& name) override;

  void TransitionToLayout(Layout layout);
  void CommitClear();
  void CommitClear(VkCommandBuffer cmdbuf);

  // Used when the render pass is changing the image layout, or to force it to
  // VK_IMAGE_LAYOUT_UNDEFINED, if the existing contents of the image is
  // irrelevant and will not be loaded.
  void OverrideImageLayout(Layout new_layout);

  void TransitionToLayout(VkCommandBuffer command_buffer, Layout new_layout);
  void TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, u32 start_layer, u32 num_layers, u32 start_level,
                                      u32 num_levels, Layout old_layout, Layout new_layout);

  static void TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, VkImage image, Type type, u32 start_layer,
                                             u32 num_layers, u32 start_level, u32 num_levels, Layout old_layout,
                                             Layout new_layout);

  // Call when the texture is bound to the pipeline, or read from in a copy.
  ALWAYS_INLINE void SetUseFenceCounter(u64 counter) { m_use_fence_counter = counter; }

  VkDescriptorSet GetDescriptorSetWithSampler(VkSampler sampler);

private:
  VulkanTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format, VkImage image,
                VmaAllocation allocation, VkImageView view, VkFormat vk_format);

  VkCommandBuffer GetCommandBufferForUpdate();
  void CopyTextureDataForUpload(void* dst, const void* src, u32 width, u32 height, u32 pitch, u32 upload_pitch) const;
  VkBuffer AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 width, u32 height) const;
  void UpdateFromBuffer(VkCommandBuffer cmdbuf, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level, u32 pitch,
                        VkBuffer buffer, u32 buffer_offset);

  VkImage m_image = VK_NULL_HANDLE;
  VmaAllocation m_allocation = VK_NULL_HANDLE;
  VkImageView m_view = VK_NULL_HANDLE;
  VkFormat m_vk_format = VK_FORMAT_UNDEFINED;
  Layout m_layout = Layout::Undefined;

  // Contains the fence counter when the texture was last used.
  // When this matches the current fence counter, the texture was used this command buffer.
  u64 m_use_fence_counter = 0;

  // Single-bind-point descriptor/sampler pairs.
  std::vector<std::pair<VkSampler, VkDescriptorSet>> m_descriptor_sets;

  u16 m_map_x = 0;
  u16 m_map_y = 0;
  u16 m_map_width = 0;
  u16 m_map_height = 0;
  u8 m_map_layer = 0;
  u8 m_map_level = 0;
};

class VulkanSampler final : public GPUSampler
{
  friend VulkanDevice;

public:
  ~VulkanSampler() override;

  ALWAYS_INLINE VkSampler GetSampler() const { return m_sampler; }

  void SetDebugName(const std::string_view& name) override;

private:
  VulkanSampler(VkSampler sampler);

  VkSampler m_sampler;
};

class VulkanTextureBuffer final : public GPUTextureBuffer
{
  friend VulkanDevice;

public:
  VulkanTextureBuffer(Format format, u32 size_in_elements);
  ~VulkanTextureBuffer() override;

  ALWAYS_INLINE VkBuffer GetBuffer() const { return m_buffer.GetBuffer(); }
  ALWAYS_INLINE VkDescriptorSet GetDescriptorSet() const { return m_descriptor_set; }

  bool CreateBuffer(bool ssbo);
  void Destroy(bool defer);

  // Inherited via GPUTextureBuffer
  void* Map(u32 required_elements) override;
  void Unmap(u32 used_elements) override;

  void SetDebugName(const std::string_view& name) override;

private:
  VulkanStreamBuffer m_buffer;
  VkBufferView m_buffer_view = VK_NULL_HANDLE;
  VkDescriptorSet m_descriptor_set = VK_NULL_HANDLE;
};

class VulkanDownloadTexture final : public GPUDownloadTexture
{
public:
  ~VulkanDownloadTexture() override;

  static std::unique_ptr<VulkanDownloadTexture> Create(u32 width, u32 height, GPUTexture::Format format, void* memory,
                                                       size_t memory_size, u32 memory_stride);

  void CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height,
                       u32 src_layer, u32 src_level, bool use_transfer_pitch) override;

  bool Map(u32 x, u32 y, u32 width, u32 height) override;
  void Unmap() override;

  void Flush() override;

  void SetDebugName(std::string_view name) override;

private:
  VulkanDownloadTexture(u32 width, u32 height, GPUTexture::Format format, VmaAllocation allocation,
                        VkDeviceMemory memory, VkBuffer buffer, VkDeviceSize memory_offset, VkDeviceSize buffer_size,
                        const u8* map_ptr, u32 map_pitch);

  VmaAllocation m_allocation = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  VkBuffer m_buffer = VK_NULL_HANDLE;

  u64 m_copy_fence_counter = 0;
  VkDeviceSize m_memory_offset = 0;
  VkDeviceSize m_buffer_size = 0;

  bool m_needs_cache_invalidate = false;
};
