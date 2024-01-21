// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "vulkan_texture.h"
#include "vulkan_builders.h"
#include "vulkan_device.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

Log_SetChannel(VulkanDevice);

static constexpr const VkComponentMapping s_identity_swizzle{
  VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
  VK_COMPONENT_SWIZZLE_IDENTITY};

static VkImageLayout GetVkImageLayout(VulkanTexture::Layout layout)
{
  static constexpr std::array<VkImageLayout, static_cast<u32>(VulkanTexture::Layout::Count)> s_vk_layout_mapping = {{
    VK_IMAGE_LAYOUT_UNDEFINED,                        // Undefined
    VK_IMAGE_LAYOUT_PREINITIALIZED,                   // Preinitialized
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,         // ColorAttachment
    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, // DepthStencilAttachment
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,         // ShaderReadOnly
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,             // ClearDst
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,             // TransferSrc
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,             // TransferDst
    VK_IMAGE_LAYOUT_GENERAL,                          // TransferSelf
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,                  // PresentSrc
    VK_IMAGE_LAYOUT_GENERAL,                          // FeedbackLoop
    VK_IMAGE_LAYOUT_GENERAL,                          // ReadWriteImage
    VK_IMAGE_LAYOUT_GENERAL,                          // ComputeReadWriteImage
    VK_IMAGE_LAYOUT_GENERAL,                          // General
  }};
  return (layout == VulkanTexture::Layout::FeedbackLoop && VulkanDevice::GetInstance().UseFeedbackLoopLayout()) ?
           VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT :
           s_vk_layout_mapping[static_cast<u32>(layout)];
}

static VkAccessFlagBits GetFeedbackLoopInputAccessBits()
{
  return VulkanDevice::GetInstance().UseFeedbackLoopLayout() ? VK_ACCESS_SHADER_READ_BIT :
                                                               VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
}

VulkanTexture::VulkanTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                             VkImage image, VmaAllocation allocation, VkImageView view, VkFormat vk_format)
  : GPUTexture(static_cast<u16>(width), static_cast<u16>(height), static_cast<u8>(layers), static_cast<u8>(levels),
               static_cast<u8>(samples), type, format),
    m_image(image), m_allocation(allocation), m_view(view), m_vk_format(vk_format)
{
}

VulkanTexture::~VulkanTexture()
{
  Destroy(true);
}

std::unique_ptr<VulkanTexture> VulkanTexture::Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                     Type type, Format format, VkFormat vk_format)
{
  if (!ValidateConfig(width, height, layers, levels, samples, type, format))
    return {};

  VulkanDevice& dev = VulkanDevice::GetInstance();

  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           nullptr,
                           0,
                           VK_IMAGE_TYPE_2D,
                           vk_format,
                           {width, height, 1u},
                           levels,
                           layers,
                           static_cast<VkSampleCountFlagBits>(samples),
                           VK_IMAGE_TILING_OPTIMAL,
                           0u,
                           VK_SHARING_MODE_EXCLUSIVE,
                           0,
                           nullptr,
                           VK_IMAGE_LAYOUT_UNDEFINED};

  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
  aci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               nullptr,
                               0,
                               VK_NULL_HANDLE,
                               VK_IMAGE_VIEW_TYPE_2D,
                               vk_format,
                               s_identity_swizzle,
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<u32>(levels), 0, 1}};

  // TODO: Don't need the feedback loop stuff yet.
  switch (type)
  {
    case Type::Texture:
    case Type::DynamicTexture:
    {
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    break;

    case Type::RenderTarget:
    {
      DebugAssert(levels == 1);
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  (dev.UseFeedbackLoopLayout() ? VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT :
                                                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    }
    break;

    case Type::DepthStencil:
    {
      DebugAssert(levels == 1);
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  (dev.UseFeedbackLoopLayout() ? VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0);
      vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    break;

    case Type::RWTexture:
    {
      DebugAssert(levels == 1);
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    break;

    default:
      return {};
  }

  // Use dedicated allocations for typical RT size
  if ((type == Type::RenderTarget || type == Type::DepthStencil) && width >= 512 && height >= 448)
    aci.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkResult res = vmaCreateImage(dev.GetAllocator(), &ici, &aci, &image, &allocation, nullptr);
  if (aci.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT && res != VK_SUCCESS)
  {
    // try without dedicated allocation
    aci.flags &= ~VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    res = vmaCreateImage(dev.GetAllocator(), &ici, &aci, &image, &allocation, nullptr);
  }
  if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY)
  {
    Log_ErrorPrintf("Failed to allocate device memory for %ux%u texture", width, height);
    return {};
  }
  else if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateImage failed: ");
    return {};
  }

  VkImageView view = VK_NULL_HANDLE;
  vci.image = image;
  res = vkCreateImageView(dev.GetVulkanDevice(), &vci, nullptr, &view);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateImageView failed: ");
    vmaDestroyImage(dev.GetAllocator(), image, allocation);
    return {};
  }

  return std::unique_ptr<VulkanTexture>(
    new VulkanTexture(width, height, layers, levels, samples, type, format, image, allocation, view, vk_format));
}

void VulkanTexture::Destroy(bool defer)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  dev.UnbindTexture(this);
  if (defer)
  {
    for (auto& it : m_descriptor_sets)
      dev.DeferPersistentDescriptorSetDestruction(it.second);
  }
  else
  {
    for (auto& it : m_descriptor_sets)
      dev.FreePersistentDescriptorSet(it.second);
  }
  m_descriptor_sets.clear();

  if (m_view != VK_NULL_HANDLE)
  {
    if (defer)
      VulkanDevice::GetInstance().DeferImageViewDestruction(m_view);
    else
      vkDestroyImageView(VulkanDevice::GetInstance().GetVulkanDevice(), m_view, nullptr);
    m_view = VK_NULL_HANDLE;
  }

  // If we don't have device memory allocated, the image is not owned by us (e.g. swapchain)
  if (m_allocation != VK_NULL_HANDLE)
  {
    if (defer)
      VulkanDevice::GetInstance().DeferImageDestruction(m_image, m_allocation);
    else
      vmaDestroyImage(VulkanDevice::GetInstance().GetAllocator(), m_image, m_allocation);
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
  }
}

VkImageLayout VulkanTexture::GetVkLayout() const
{
  return GetVkImageLayout(m_layout);
}

VkCommandBuffer VulkanTexture::GetCommandBufferForUpdate()
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  if ((m_type != Type::Texture && m_type != Type::DynamicTexture) ||
      m_use_fence_counter == dev.GetCurrentFenceCounter())
  {
    // Console.WriteLn("Texture update within frame, can't use do beforehand");
    if (dev.InRenderPass())
      dev.EndRenderPass();
    return dev.GetCurrentCommandBuffer();
  }

  return dev.GetCurrentInitCommandBuffer();
}

void VulkanTexture::CopyTextureDataForUpload(void* dst, const void* src, u32 width, u32 height, u32 pitch,
                                             u32 upload_pitch) const
{
  StringUtil::StrideMemCpy(dst, upload_pitch, src, pitch, GetPixelSize() * width, height);
}

VkBuffer VulkanTexture::AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 width,
                                                    u32 height) const
{
  const u32 size = upload_pitch * height;
  const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  nullptr,
                                  0,
                                  static_cast<VkDeviceSize>(size),
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE,
                                  0,
                                  nullptr};

  // Don't worry about setting the coherent bit for this upload, the main reason we had
  // that set in StreamBuffer was for MoltenVK, which would upload the whole buffer on
  // smaller uploads, but we're writing to the whole thing anyway.
  VmaAllocationCreateInfo aci = {};
  aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
  aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

  VmaAllocationInfo ai;
  VkBuffer buffer;
  VmaAllocation allocation;
  VkResult res = vmaCreateBuffer(VulkanDevice::GetInstance().GetAllocator(), &bci, &aci, &buffer, &allocation, &ai);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "(AllocateUploadStagingBuffer) vmaCreateBuffer() failed: ");
    return VK_NULL_HANDLE;
  }

  // Immediately queue it for freeing after the command buffer finishes, since it's only needed for the copy.
  VulkanDevice::GetInstance().DeferBufferDestruction(buffer, allocation);

  // And write the data.
  CopyTextureDataForUpload(ai.pMappedData, data, width, height, pitch, upload_pitch);
  vmaFlushAllocation(VulkanDevice::GetInstance().GetAllocator(), allocation, 0, size);
  return buffer;
}

void VulkanTexture::UpdateFromBuffer(VkCommandBuffer cmdbuf, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level,
                                     u32 pitch, VkBuffer buffer, u32 buffer_offset)
{
  const Layout old_layout = m_layout;
  if (old_layout != Layout::TransferDst)
    TransitionSubresourcesToLayout(cmdbuf, layer, 1, level, 1, old_layout, Layout::TransferDst);

  const u32 row_length = pitch / GetPixelSize();

  const VkBufferImageCopy bic = {static_cast<VkDeviceSize>(buffer_offset),
                                 row_length,
                                 height,
                                 {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<u32>(level), 0u, 1u},
                                 {static_cast<s32>(x), static_cast<s32>(y), 0},
                                 {width, height, 1u}};

  vkCmdCopyBufferToImage(cmdbuf, buffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);

  if (old_layout != Layout::TransferDst)
    TransitionSubresourcesToLayout(cmdbuf, layer, 1, level, 1, Layout::TransferDst, old_layout);
}

bool VulkanTexture::Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer, u32 level)
{
  DebugAssert(layer < m_layers && level < m_levels);
  DebugAssert((x + width) <= GetMipWidth(level) && (y + height) <= GetMipHeight(level));

  const u32 upload_pitch = Common::AlignUpPow2(pitch, VulkanDevice::GetInstance().GetBufferCopyRowPitchAlignment());
  const u32 required_size = height * upload_pitch;
  VulkanDevice& dev = VulkanDevice::GetInstance();
  VulkanStreamBuffer& sbuffer = dev.GetTextureUploadBuffer();

  // If the texture is larger than half our streaming buffer size, use a separate buffer.
  // Otherwise allocation will either fail, or require lots of cmdbuffer submissions.
  VkBuffer buffer;
  u32 buffer_offset;
  if (required_size > (sbuffer.GetCurrentSize() / 2))
  {
    buffer_offset = 0;
    buffer = AllocateUploadStagingBuffer(data, pitch, upload_pitch, width, height);
    if (buffer == VK_NULL_HANDLE)
      return false;
  }
  else
  {
    if (!sbuffer.ReserveMemory(required_size, dev.GetBufferCopyOffsetAlignment()))
    {
      dev.SubmitCommandBuffer(false, "While waiting for %u bytes in texture upload buffer", required_size);
      if (!sbuffer.ReserveMemory(required_size, dev.GetBufferCopyOffsetAlignment()))
      {
        Log_ErrorPrintf("Failed to reserve texture upload memory (%u bytes).", required_size);
        return false;
      }
    }

    buffer = sbuffer.GetBuffer();
    buffer_offset = sbuffer.GetCurrentOffset();
    CopyTextureDataForUpload(sbuffer.GetCurrentHostPointer(), data, width, height, pitch, upload_pitch);
    sbuffer.CommitMemory(required_size);
  }

  GPUDevice::GetStatistics().buffer_streamed += required_size;
  GPUDevice::GetStatistics().num_uploads++;

  const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();

  // if we're an rt and have been cleared, and the full rect isn't being uploaded, do the clear
  if (m_type == Type::RenderTarget)
  {
    if (m_state == State::Cleared && (x != 0 || y != 0 || width != m_width || height != m_height))
      CommitClear(cmdbuf);
    else
      m_state = State::Dirty;
  }

  // first time the texture is used? don't leave it undefined
  if (m_layout == Layout::Undefined)
    TransitionToLayout(cmdbuf, Layout::TransferDst);

  UpdateFromBuffer(cmdbuf, x, y, width, height, layer, level, upload_pitch, buffer, buffer_offset);
  TransitionToLayout(cmdbuf, Layout::ShaderReadOnly);
  return true;
}

bool VulkanTexture::Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level)
{
  // TODO: linear textures for dynamic?
  if ((x + width) > GetMipWidth(level) || (y + height) > GetMipHeight(level) || layer > m_layers || level > m_levels)
  {
    return false;
  }

  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (m_state == GPUTexture::State::Cleared && (x != 0 || y != 0 || width != m_width || height != m_height))
    CommitClear(GetCommandBufferForUpdate());

  // see note in Update() for the reason why.
  const u32 aligned_pitch = Common::AlignUpPow2(width * GetPixelSize(), dev.GetBufferCopyRowPitchAlignment());
  const u32 req_size = height * aligned_pitch;
  VulkanStreamBuffer& buffer = dev.GetTextureUploadBuffer();
  if (req_size >= (buffer.GetCurrentSize() / 2))
    return false;

  if (!buffer.ReserveMemory(req_size, dev.GetBufferCopyOffsetAlignment()))
  {
    dev.SubmitCommandBuffer(false, "While waiting for %u bytes in texture upload buffer", req_size);
    if (!buffer.ReserveMemory(req_size, dev.GetBufferCopyOffsetAlignment()))
      Panic("Failed to reserve texture upload memory");
  }

  // map for writing
  *map = buffer.GetCurrentHostPointer();
  *map_stride = aligned_pitch;
  m_map_x = static_cast<u16>(x);
  m_map_y = static_cast<u16>(y);
  m_map_width = static_cast<u16>(width);
  m_map_height = static_cast<u16>(height);
  m_map_layer = static_cast<u8>(layer);
  m_map_level = static_cast<u8>(level);
  m_state = GPUTexture::State::Dirty;
  return true;
}

void VulkanTexture::Unmap()
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  VulkanStreamBuffer& sb = dev.GetTextureUploadBuffer();
  const u32 aligned_pitch = Common::AlignUpPow2(m_map_width * GetPixelSize(), dev.GetBufferCopyRowPitchAlignment());
  const u32 req_size = m_map_height * aligned_pitch;
  const u32 offset = sb.GetCurrentOffset();
  sb.CommitMemory(req_size);

  GPUDevice::GetStatistics().buffer_streamed += req_size;
  GPUDevice::GetStatistics().num_uploads++;

  // first time the texture is used? don't leave it undefined
  const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();
  if (m_layout == Layout::Undefined)
    TransitionToLayout(cmdbuf, Layout::TransferDst);

  UpdateFromBuffer(cmdbuf, m_map_x, m_map_y, m_map_width, m_map_height, m_map_layer, m_map_level, aligned_pitch,
                   sb.GetBuffer(), offset);
  TransitionToLayout(cmdbuf, Layout::ShaderReadOnly);

  m_map_x = 0;
  m_map_y = 0;
  m_map_width = 0;
  m_map_height = 0;
  m_map_layer = 0;
  m_map_level = 0;
}

void VulkanTexture::CommitClear()
{
  if (m_state != GPUTexture::State::Cleared)
    return;

  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (dev.InRenderPass())
    dev.EndRenderPass();

  CommitClear(dev.GetCurrentCommandBuffer());
}

void VulkanTexture::CommitClear(VkCommandBuffer cmdbuf)
{
  TransitionToLayout(cmdbuf, Layout::ClearDst);

  if (IsDepthStencil())
  {
    const VkClearDepthStencilValue cv = {m_clear_value.depth, 0u};
    const VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u};
    vkCmdClearDepthStencilImage(cmdbuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &srr);
  }
  else
  {
    alignas(16) VkClearColorValue cv;
    std::memcpy(cv.float32, GetUNormClearColor().data(), sizeof(cv.float32));
    const VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdClearColorImage(cmdbuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &srr);
  }

  SetState(GPUTexture::State::Dirty);
}

void VulkanTexture::OverrideImageLayout(Layout new_layout)
{
  m_layout = new_layout;
}

void VulkanTexture::SetDebugName(const std::string_view& name)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  Vulkan::SetObjectName(dev.GetVulkanDevice(), m_image, name);
  Vulkan::SetObjectName(dev.GetVulkanDevice(), m_view, name);
}

void VulkanTexture::TransitionToLayout(Layout layout)
{
  TransitionToLayout(VulkanDevice::GetInstance().GetCurrentCommandBuffer(), layout);
}

void VulkanTexture::TransitionToLayout(VkCommandBuffer command_buffer, Layout new_layout)
{
  // Need a barrier inbetween multiple self transfers.
  if (m_layout == new_layout && new_layout != Layout::TransferSelf)
    return;

  TransitionSubresourcesToLayout(command_buffer, 0, m_layers, 0, m_levels, m_layout, new_layout);

  m_layout = new_layout;
}

void VulkanTexture::TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, u32 start_layer, u32 num_layers,
                                                   u32 start_level, u32 num_levels, Layout old_layout,
                                                   Layout new_layout)
{
  TransitionSubresourcesToLayout(command_buffer, m_image, m_type, start_layer, num_layers, start_level, num_levels,
                                 old_layout, new_layout);
}

void VulkanTexture::TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, VkImage image, Type type,
                                                   u32 start_layer, u32 num_layers, u32 start_level, u32 num_levels,
                                                   Layout old_layout, Layout new_layout)
{
  VkImageAspectFlags aspect;
  if (type == Type::DepthStencil)
  {
    // TODO: detect stencil
    aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
  }
  else
  {
    aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  }

  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  nullptr,
                                  0,
                                  0,
                                  GetVkImageLayout(old_layout),
                                  GetVkImageLayout(new_layout),
                                  VK_QUEUE_FAMILY_IGNORED,
                                  VK_QUEUE_FAMILY_IGNORED,
                                  image,
                                  {aspect, start_level, num_levels, start_layer, num_layers}};

  // srcStageMask -> Stages that must complete before the barrier
  // dstStageMask -> Stages that must wait for after the barrier before beginning
  VkPipelineStageFlags srcStageMask, dstStageMask;
  switch (old_layout)
  {
    case Layout::Undefined:
      // Layout undefined therefore contents undefined, and we don't care what happens to it.
      barrier.srcAccessMask = 0;
      srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;

    case Layout::Preinitialized:
      // Image has been pre-initialized by the host, so ensure all writes have completed.
      barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_HOST_BIT;
      break;

    case Layout::ColorAttachment:
      // Image was being used as a color attachment, so ensure all writes have completed.
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      break;

    case Layout::DepthStencilAttachment:
      // Image was being used as a depthstencil attachment, so ensure all writes have completed.
      barrier.srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      break;

    case Layout::ShaderReadOnly:
      // Image was being used as a shader resource, make sure all reads have finished.
      barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;

    case Layout::ClearDst:
      // Image was being used as a clear destination, ensure all writes have finished.
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::TransferSrc:
      // Image was being used as a copy source, ensure all reads have finished.
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::TransferDst:
      // Image was being used as a copy destination, ensure all writes have finished.
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::TransferSelf:
      // Image was being used as a copy source and destination, ensure all reads and writes have finished.
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::FeedbackLoop:
      barrier.srcAccessMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
                                (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 GetFeedbackLoopInputAccessBits()) :
                                (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
      srcStageMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
                       (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) :
                       (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      break;

    case Layout::ReadWriteImage:
      barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;

    case Layout::ComputeReadWriteImage:
      barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      break;

    case Layout::General:
    default:
      srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;
  }

  switch (new_layout)
  {
    case Layout::Undefined:
      barrier.dstAccessMask = 0;
      dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;

    case Layout::ColorAttachment:
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      break;

    case Layout::DepthStencilAttachment:
      barrier.dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      break;

    case Layout::ShaderReadOnly:
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;

    case Layout::ClearDst:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::TransferSrc:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::TransferDst:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::TransferSelf:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case Layout::PresentSrc:
      srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;

    case Layout::FeedbackLoop:
      barrier.dstAccessMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
                                (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 GetFeedbackLoopInputAccessBits()) :
                                (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
      dstStageMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ?
                       (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) :
                       (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      break;

    case Layout::ReadWriteImage:
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;

    case Layout::ComputeReadWriteImage:
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      break;

    case Layout::General:
    default:
      dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      break;
  }
  vkCmdPipelineBarrier(command_buffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkDescriptorSet VulkanTexture::GetDescriptorSetWithSampler(VkSampler sampler)
{
  for (const auto& it : m_descriptor_sets)
  {
    if (it.first == sampler)
      return it.second;
  }

  VulkanDevice& dev = VulkanDevice::GetInstance();
  VkDescriptorSet ds = dev.AllocatePersistentDescriptorSet(dev.m_single_texture_ds_layout);
  if (ds == VK_NULL_HANDLE)
    Panic("Failed to allocate persistent descriptor set.");

  Vulkan::DescriptorSetUpdateBuilder dsub;
  dsub.AddCombinedImageSamplerDescriptorWrite(ds, 0, m_view, sampler);
  dsub.Update(dev.GetVulkanDevice(), false);
  m_descriptor_sets.emplace_back(sampler, ds);
  return ds;
}

void VulkanTexture::MakeReadyForSampling()
{
  if (m_layout == Layout::ShaderReadOnly)
    return;

  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (dev.InRenderPass())
    dev.EndRenderPass();

  TransitionToLayout(Layout::ShaderReadOnly);
}

std::unique_ptr<GPUTexture> VulkanDevice::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                        GPUTexture::Type type, GPUTexture::Format format,
                                                        const void* data /* = nullptr */, u32 data_stride /* = 0 */)
{
  const VkFormat vk_format = VulkanDevice::TEXTURE_FORMAT_MAPPING[static_cast<u8>(format)];
  std::unique_ptr<VulkanTexture> tex =
    VulkanTexture::Create(width, height, layers, levels, samples, type, format, vk_format);
  if (tex && data)
    tex->Update(0, 0, width, height, data, data_stride);

  return tex;
}

bool VulkanDevice::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                   u32 out_data_stride)
{
  VulkanTexture* T = static_cast<VulkanTexture*>(texture);
  T->CommitClear();

  const u32 pitch = Common::AlignUp(width * T->GetPixelSize(), GetBufferCopyRowPitchAlignment());
  const u32 size = pitch * height;
  const u32 level = 0;
  if (!CheckDownloadBufferSize(size))
  {
    Log_ErrorPrintf("Can't read back %ux%u", width, height);
    return false;
  }

  s_stats.num_downloads++;

  if (InRenderPass())
    EndRenderPass();

  const VkCommandBuffer cmdbuf = GetCurrentCommandBuffer();

  VulkanTexture::Layout old_layout = T->GetLayout();
  if (old_layout != VulkanTexture::Layout::TransferSrc)
    T->TransitionSubresourcesToLayout(cmdbuf, 0, 1, 0, 1, old_layout, VulkanTexture::Layout::TransferSrc);

  VkBufferImageCopy image_copy = {};
  const VkImageAspectFlags aspect = T->IsDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  image_copy.bufferOffset = 0;
  image_copy.bufferRowLength = pitch / T->GetPixelSize();
  image_copy.bufferImageHeight = 0;
  image_copy.imageSubresource = {aspect, level, 0u, 1u};
  image_copy.imageOffset = {static_cast<s32>(x), static_cast<s32>(y), 0};
  image_copy.imageExtent = {width, height, 1u};

  // do the copy
  vkCmdCopyImageToBuffer(cmdbuf, T->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_download_buffer, 1,
                         &image_copy);

  // flush gpu cache
  const VkBufferMemoryBarrier buffer_info = {
    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, // VkStructureType    sType
    nullptr,                                 // const void*        pNext
    VK_ACCESS_TRANSFER_WRITE_BIT,            // VkAccessFlags      srcAccessMask
    VK_ACCESS_HOST_READ_BIT,                 // VkAccessFlags      dstAccessMask
    VK_QUEUE_FAMILY_IGNORED,                 // uint32_t           srcQueueFamilyIndex
    VK_QUEUE_FAMILY_IGNORED,                 // uint32_t           dstQueueFamilyIndex
    m_download_buffer,                       // VkBuffer           buffer
    0,                                       // VkDeviceSize       offset
    size                                     // VkDeviceSize       size
  };
  vkCmdPipelineBarrier(cmdbuf, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &buffer_info,
                       0, nullptr);

  if (old_layout != VulkanTexture::Layout::TransferSrc)
    T->TransitionSubresourcesToLayout(cmdbuf, 0, 1, 0, 1, VulkanTexture::Layout::TransferSrc, old_layout);

  SubmitCommandBuffer(true);

  // invalidate cpu cache before reading
  VkResult res = vmaInvalidateAllocation(m_allocator, m_download_buffer_allocation, 0, size);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vmaInvalidateAllocation() failed, readback may be incorrect: ");

  StringUtil::StrideMemCpy(out_data, out_data_stride, m_download_buffer_map, pitch, width * T->GetPixelSize(), height);
  return true;
}

bool VulkanDevice::CheckDownloadBufferSize(u32 required_size)
{
  if (m_download_buffer_size >= required_size)
    return true;

  DestroyDownloadBuffer();

  // Adreno has slow coherent cached reads.
  const bool is_adreno = (m_device_properties.vendorID == 0x5143 ||
                          m_device_driver_properties.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY);

  const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  nullptr,
                                  0u,
                                  required_size,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE,
                                  0u,
                                  nullptr};

  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
  aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
  aci.preferredFlags = is_adreno ? (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) :
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

  VmaAllocationInfo ai = {};
  VkResult res = vmaCreateBuffer(m_allocator, &bci, &aci, &m_download_buffer, &m_download_buffer_allocation, &ai);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateBuffer() failed: ");
    return false;
  }

  m_download_buffer_map = static_cast<u8*>(ai.pMappedData);
  return true;
}

void VulkanDevice::DestroyDownloadBuffer()
{
  if (m_download_buffer == VK_NULL_HANDLE)
    return;

  vmaDestroyBuffer(m_allocator, m_download_buffer, m_download_buffer_allocation);

  // unmapped as part of the buffer destroy
  m_download_buffer = VK_NULL_HANDLE;
  m_download_buffer_allocation = VK_NULL_HANDLE;
  m_download_buffer_map = nullptr;
  m_download_buffer_size = 0;
}

VulkanSampler::VulkanSampler(VkSampler sampler) : m_sampler(sampler)
{
}

VulkanSampler::~VulkanSampler()
{
  // Cleaned up by main class.
}

void VulkanSampler::SetDebugName(const std::string_view& name)
{
  Vulkan::SetObjectName(VulkanDevice::GetInstance().GetVulkanDevice(), m_sampler, name);
}

VkSampler VulkanDevice::GetSampler(const GPUSampler::Config& config)
{
  const auto it = m_sampler_map.find(config.key);
  if (it != m_sampler_map.end())
    return it->second;

  static constexpr std::array<VkSamplerAddressMode, static_cast<u8>(GPUSampler::AddressMode::MaxCount)> ta = {{
    VK_SAMPLER_ADDRESS_MODE_REPEAT,          // Repeat
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   // ClampToEdge
    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, // ClampToBorder
    VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // MirrorRepeat
  }};
  static constexpr std::array<VkFilter, static_cast<u8>(GPUSampler::Filter::MaxCount)> min_mag_filters = {{
    VK_FILTER_NEAREST, // Nearest
    VK_FILTER_LINEAR,  // Linear
  }};
  static constexpr std::array<VkSamplerMipmapMode, static_cast<u8>(GPUSampler::Filter::MaxCount)> mip_filters = {{
    VK_SAMPLER_MIPMAP_MODE_NEAREST, // Nearest
    VK_SAMPLER_MIPMAP_MODE_LINEAR,  // Linear
  }};
  struct BorderColorMapping
  {
    u32 color;
    VkBorderColor vk_color;
  };
  static constexpr BorderColorMapping border_color_mapping[] = {
    {0x00000000u, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK},
    {0xFF000000u, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK},
    {0xFFFFFFFFu, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE},
  };

  // See https://www.khronos.org/registry/vulkan/specs/1.2-extensions/man/html/VkSamplerCreateInfo.html#_description
  // for the reasoning behind 0.25f here.
  VkSamplerCreateInfo ci = {
    VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    nullptr,
    0,
    min_mag_filters[static_cast<u8>(config.min_filter.GetValue())],     // min
    min_mag_filters[static_cast<u8>(config.mag_filter.GetValue())],     // mag
    mip_filters[static_cast<u8>(config.mip_filter.GetValue())],         // mip
    ta[static_cast<u8>(config.address_u.GetValue())],                   // u
    ta[static_cast<u8>(config.address_v.GetValue())],                   // v
    ta[static_cast<u8>(config.address_w.GetValue())],                   // w
    0.0f,                                                               // lod bias
    static_cast<VkBool32>(config.anisotropy > 1),                       // anisotropy enable
    static_cast<float>(config.anisotropy),                              // anisotropy
    VK_FALSE,                                                           // compare enable
    VK_COMPARE_OP_ALWAYS,                                               // compare op
    static_cast<float>(config.min_lod),                                 // min lod
    (config.max_lod == 0) ? 0.25f : static_cast<float>(config.max_lod), // max lod
    VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,                            // border
    VK_FALSE                                                            // unnormalized coordinates
  };

  if (config.address_u == GPUSampler::AddressMode::ClampToBorder ||
      config.address_v == GPUSampler::AddressMode::ClampToBorder ||
      config.address_w == GPUSampler::AddressMode::ClampToBorder)
  {
    u32 i;
    for (i = 0; i < static_cast<u32>(std::size(border_color_mapping)); i++)
    {
      if (border_color_mapping[i].color == config.border_color)
        break;
    }
    if (i == std::size(border_color_mapping))
    {
      Log_ErrorPrintf("Unsupported border color: %08X", config.border_color.GetValue());
      return {};
    }

    ci.borderColor = border_color_mapping[i].vk_color;
  }

  VkSampler sampler = VK_NULL_HANDLE;
  VkResult res = vkCreateSampler(m_device, &ci, nullptr, &sampler);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkCreateSampler() failed: ");

  m_sampler_map.emplace(config.key, sampler);
  return sampler;
}

void VulkanDevice::DestroySamplers()
{
  for (auto& it : m_sampler_map)
  {
    if (it.second != VK_NULL_HANDLE)
      vkDestroySampler(m_device, it.second, nullptr);
  }
  m_sampler_map.clear();
}

std::unique_ptr<GPUSampler> VulkanDevice::CreateSampler(const GPUSampler::Config& config)
{
  const VkSampler vsampler = GetSampler(config);
  if (vsampler == VK_NULL_HANDLE)
    return {};

  return std::unique_ptr<GPUSampler>(new VulkanSampler(vsampler));
}

VulkanTextureBuffer::VulkanTextureBuffer(Format format, u32 size_in_elements)
  : GPUTextureBuffer(format, size_in_elements)
{
}

VulkanTextureBuffer::~VulkanTextureBuffer()
{
  Destroy(true);
}

bool VulkanTextureBuffer::CreateBuffer(bool ssbo)
{
  return m_buffer.Create(ssbo ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                         GetSizeInBytes());
}

void VulkanTextureBuffer::Destroy(bool defer)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (m_buffer_view != VK_NULL_HANDLE)
  {
    if (defer)
      dev.DeferBufferViewDestruction(m_buffer_view);
    else
      vkDestroyBufferView(dev.GetVulkanDevice(), m_buffer_view, nullptr);
  }
  if (m_descriptor_set != VK_NULL_HANDLE)
  {
    if (defer)
      dev.DeferPersistentDescriptorSetDestruction(m_descriptor_set);
    else
      dev.FreePersistentDescriptorSet(m_descriptor_set);
  }
}

void* VulkanTextureBuffer::Map(u32 required_elements)
{
  const u32 esize = GetElementSize(m_format);
  const u32 req_size = esize * required_elements;
  if (!m_buffer.ReserveMemory(req_size, esize))
  {
    VulkanDevice::GetInstance().SubmitCommandBufferAndRestartRenderPass("out of space in texture buffer");
    if (!m_buffer.ReserveMemory(req_size, esize))
      Panic("Failed to allocate texture buffer space.");
  }

  m_current_position = m_buffer.GetCurrentOffset() / esize;
  return m_buffer.GetCurrentHostPointer();
}

void VulkanTextureBuffer::Unmap(u32 used_elements)
{
  const u32 size = GetElementSize(m_format) * used_elements;
  GPUDevice::GetStatistics().buffer_streamed += size;
  GPUDevice::GetStatistics().num_uploads++;
  m_buffer.CommitMemory(size);
}

void VulkanTextureBuffer::SetDebugName(const std::string_view& name)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  Vulkan::SetObjectName(dev.GetVulkanDevice(), m_buffer.GetBuffer(), name);
  if (m_buffer_view != VK_NULL_HANDLE)
    Vulkan::SetObjectName(dev.GetVulkanDevice(), m_buffer_view, name);
}

std::unique_ptr<GPUTextureBuffer> VulkanDevice::CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                    u32 size_in_elements)
{
  static constexpr std::array<VkFormat, static_cast<u8>(GPUTextureBuffer::Format::MaxCount)> format_mapping = {{
    VK_FORMAT_R16_UINT, // R16UI
  }};

  const bool ssbo = m_features.texture_buffers_emulated_with_ssbo;
  std::unique_ptr<VulkanTextureBuffer> tb = std::make_unique<VulkanTextureBuffer>(format, size_in_elements);
  if (!tb->CreateBuffer(ssbo))
    return {};

  tb->m_descriptor_set = AllocatePersistentDescriptorSet(m_single_texture_buffer_ds_layout);
  if (tb->m_descriptor_set == VK_NULL_HANDLE)
  {
    Log_ErrorPrintf("Failed to allocate persistent descriptor set for texture buffer.");
    tb->Destroy(false);
    return {};
  }

  Vulkan::DescriptorSetUpdateBuilder dsub;
  if (ssbo)
  {
    dsub.AddBufferDescriptorWrite(tb->m_descriptor_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, tb->GetBuffer(), 0,
                                  tb->GetSizeInBytes());
  }
  else
  {
    Vulkan::BufferViewBuilder bvb;
    bvb.Set(tb->GetBuffer(), format_mapping[static_cast<u8>(format)], 0, tb->GetSizeInBytes());
    if ((tb->m_buffer_view = bvb.Create(m_device, false)) == VK_NULL_HANDLE)
    {
      Log_ErrorPrintf("Failed to create buffer view for texture buffer.");
      tb->Destroy(false);
      return {};
    }

    dsub.AddBufferViewDescriptorWrite(tb->m_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                      tb->m_buffer_view);
  }
  dsub.Update(m_device, false);

  return tb;
}
