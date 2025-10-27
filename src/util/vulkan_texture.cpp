// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "vulkan_texture.h"
#include "vulkan_builders.h"
#include "vulkan_device.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/log.h"

LOG_CHANNEL(GPUDevice);

static constexpr const VkComponentMapping s_identity_swizzle{
  VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
  VK_COMPONENT_SWIZZLE_IDENTITY};

static VkImageLayout GetVkImageLayout(VulkanTexture::Layout layout)
{
  // TODO: Wrong for depth textures in feedback loop
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
  return (layout == VulkanTexture::Layout::FeedbackLoop &&
          VulkanDevice::GetInstance().GetOptionalExtensions().vk_khr_dynamic_rendering_local_read) ?
           VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR :
           s_vk_layout_mapping[static_cast<u32>(layout)];
}

VulkanTexture::VulkanTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                             Flags flags, VkImage image, VmaAllocation allocation, VkImageView view, VkFormat vk_format)
  : GPUTexture(static_cast<u16>(width), static_cast<u16>(height), static_cast<u8>(layers), static_cast<u8>(levels),
               static_cast<u8>(samples), type, format, flags),
    m_image(image), m_allocation(allocation), m_view(view), m_vk_format(vk_format)
{
}

VulkanTexture::~VulkanTexture()
{
  Destroy(true);
}

std::unique_ptr<VulkanTexture> VulkanTexture::Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                     Type type, Format format, Flags flags, VkFormat vk_format,
                                                     Error* error)
{
  if (!ValidateConfig(width, height, layers, levels, samples, type, format, flags, error))
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

  switch (type)
  {
    case Type::Texture:
    {
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    break;

    case Type::RenderTarget:
    {
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    break;

    case Type::DepthStencil:
    {
      DebugAssert(levels == 1);
      ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    break;

      DefaultCaseIsUnreachable();
  }

  if ((flags & Flags::AllowBindAsImage) != Flags::None)
  {
    DebugAssert(levels == 1);
    ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
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
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vmaCreateImage failed: ", res);
    return {};
  }

  VkImageView view = VK_NULL_HANDLE;
  vci.image = image;
  res = vkCreateImageView(dev.GetVulkanDevice(), &vci, nullptr, &view);
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkCreateImageView failed: ", res);
    vmaDestroyImage(dev.GetAllocator(), image, allocation);
    return {};
  }

  return std::unique_ptr<VulkanTexture>(
    new VulkanTexture(width, height, layers, levels, samples, type, format, flags, image, allocation, view, vk_format));
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

VkClearColorValue VulkanTexture::GetClearColorValue() const
{
  VkClearColorValue ccv;
  std::memcpy(ccv.float32, GetUNormClearColor().data(), sizeof(ccv.float32));
  return ccv;
}

VkClearDepthStencilValue VulkanTexture::GetClearDepthValue() const
{
  return VkClearDepthStencilValue{m_clear_value.depth, 0u};
}

VkCommandBuffer VulkanTexture::GetCommandBufferForUpdate()
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (m_type != Type::Texture || m_use_fence_counter == dev.GetCurrentFenceCounter())
  {
    // DEV_LOG("Texture update within frame, can't use do beforehand");
    if (dev.InRenderPass())
      dev.EndRenderPass();
    return dev.GetCurrentCommandBuffer();
  }

  return dev.GetCurrentInitCommandBuffer();
}

VkBuffer VulkanTexture::AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 width,
                                                    u32 height, u32 buffer_size) const
{
  const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  nullptr,
                                  0,
                                  static_cast<VkDeviceSize>(buffer_size),
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
  CopyTextureDataForUpload(width, height, m_format, ai.pMappedData, upload_pitch, data, pitch);
  vmaFlushAllocation(VulkanDevice::GetInstance().GetAllocator(), allocation, 0, buffer_size);
  return buffer;
}

void VulkanTexture::UpdateFromBuffer(VkCommandBuffer cmdbuf, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level,
                                     u32 pitch, VkBuffer buffer, u32 buffer_offset)
{
  const Layout old_layout = m_layout;
  if (old_layout != Layout::TransferDst)
    TransitionSubresourcesToLayout(cmdbuf, layer, 1, level, 1, old_layout, Layout::TransferDst);

  const u32 row_length = CalcUploadRowLengthFromPitch(pitch);

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

  const u32 upload_pitch =
    Common::AlignUpPow2(CalcUploadPitch(width), VulkanDevice::GetInstance().GetBufferCopyRowPitchAlignment());
  const u32 required_size = CalcUploadSize(height, upload_pitch);
  VulkanDevice& dev = VulkanDevice::GetInstance();
  VulkanStreamBuffer& sbuffer = dev.GetTextureUploadBuffer();

  // If the texture is larger than half our streaming buffer size, use a separate buffer.
  // Otherwise allocation will either fail, or require lots of cmdbuffer submissions.
  VkBuffer buffer;
  u32 buffer_offset;
  if (required_size > (sbuffer.GetCurrentSize() / 2))
  {
    buffer_offset = 0;
    buffer = AllocateUploadStagingBuffer(data, pitch, upload_pitch, width, height, required_size);
    if (buffer == VK_NULL_HANDLE)
      return false;
  }
  else
  {
    if (!sbuffer.ReserveMemory(required_size, dev.GetBufferCopyOffsetAlignment())) [[unlikely]]
    {
      dev.SubmitCommandBuffer(false, TinyString::from_format("Needs {} bytes in texture upload buffer", required_size));
      if (!sbuffer.ReserveMemory(required_size, dev.GetBufferCopyOffsetAlignment())) [[unlikely]]
      {
        ERROR_LOG("Failed to reserve texture upload memory ({} bytes).", required_size);
        return false;
      }
    }

    buffer = sbuffer.GetBuffer();
    buffer_offset = sbuffer.GetCurrentOffset();
    CopyTextureDataForUpload(width, height, m_format, sbuffer.GetCurrentHostPointer(), upload_pitch, data, pitch);
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
  const u32 aligned_pitch = Common::AlignUpPow2(CalcUploadPitch(width), dev.GetBufferCopyRowPitchAlignment());
  const u32 req_size = CalcUploadSize(height, aligned_pitch);
  VulkanStreamBuffer& buffer = dev.GetTextureUploadBuffer();
  if (req_size >= (buffer.GetCurrentSize() / 2))
    return false;

  if (!buffer.ReserveMemory(req_size, dev.GetBufferCopyOffsetAlignment())) [[unlikely]]
  {
    dev.SubmitCommandBuffer(false, TinyString::from_format("Needs {} bytes in texture upload buffer", req_size));
    if (!buffer.ReserveMemory(req_size, dev.GetBufferCopyOffsetAlignment())) [[unlikely]]
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
  const u32 aligned_pitch = Common::AlignUpPow2(CalcUploadPitch(m_width), dev.GetBufferCopyRowPitchAlignment());
  const u32 req_size = CalcUploadSize(m_map_height, aligned_pitch);
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

#ifdef ENABLE_GPU_OBJECT_NAMES

void VulkanTexture::SetDebugName(std::string_view name)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  Vulkan::SetObjectName(dev.GetVulkanDevice(), m_image, name);
  Vulkan::SetObjectName(dev.GetVulkanDevice(), m_view, name);
}

#endif

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
                                 VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) :
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
                                 VK_ACCESS_INPUT_ATTACHMENT_READ_BIT) :
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

void VulkanTexture::GenerateMipmaps()
{
  DebugAssert(HasFlag(Flags::AllowGenerateMipmaps));

  const VkCommandBuffer cmdbuf = GetCommandBufferForUpdate();

  if (m_layout == Layout::Undefined)
    TransitionToLayout(cmdbuf, Layout::TransferSrc);

  for (u32 layer = 0; layer < m_layers; layer++)
  {
    for (u32 dst_level = 1; dst_level < m_levels; dst_level++)
    {
      const u32 src_level = dst_level - 1;
      const u32 src_width = std::max<u32>(m_width >> src_level, 1u);
      const u32 src_height = std::max<u32>(m_height >> src_level, 1u);
      const u32 dst_width = std::max<u32>(m_width >> dst_level, 1u);
      const u32 dst_height = std::max<u32>(m_height >> dst_level, 1u);

      TransitionSubresourcesToLayout(cmdbuf, layer, 1, src_level, 1, m_layout, Layout::TransferSrc);
      TransitionSubresourcesToLayout(cmdbuf, layer, 1, dst_level, 1, m_layout, Layout::TransferDst);

      const VkImageBlit blit = {
        {VK_IMAGE_ASPECT_COLOR_BIT, src_level, 0u, 1u},                              // srcSubresource
        {{0, 0, 0}, {static_cast<s32>(src_width), static_cast<s32>(src_height), 1}}, // srcOffsets
        {VK_IMAGE_ASPECT_COLOR_BIT, dst_level, 0u, 1u},                              // dstSubresource
        {{0, 0, 0}, {static_cast<s32>(dst_width), static_cast<s32>(dst_height), 1}}  // dstOffsets
      };

      vkCmdBlitImage(cmdbuf, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

      TransitionSubresourcesToLayout(cmdbuf, layer, 1, src_level, 1, Layout::TransferSrc, m_layout);
      TransitionSubresourcesToLayout(cmdbuf, layer, 1, dst_level, 1, Layout::TransferDst, m_layout);
    }
  }
}

std::unique_ptr<GPUTexture> VulkanDevice::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                        GPUTexture::Type type, GPUTexture::Format format,
                                                        GPUTexture::Flags flags, const void* data /* = nullptr */,
                                                        u32 data_stride /* = 0 */, Error* error /* = nullptr */)
{
  const VkFormat vk_format = VulkanDevice::TEXTURE_FORMAT_MAPPING[static_cast<u8>(format)];
  std::unique_ptr<VulkanTexture> tex =
    VulkanTexture::Create(width, height, layers, levels, samples, type, format, flags, vk_format, error);
  if (tex && data)
    tex->Update(0, 0, width, height, data, data_stride);

  return tex;
}

VulkanSampler::VulkanSampler(VkSampler sampler) : m_sampler(sampler)
{
}

VulkanSampler::~VulkanSampler()
{
  VulkanDevice::GetInstance().DeferSamplerDestruction(m_sampler);
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void VulkanSampler::SetDebugName(std::string_view name)
{
  Vulkan::SetObjectName(VulkanDevice::GetInstance().GetVulkanDevice(), m_sampler, name);
}

#endif

std::unique_ptr<GPUSampler> VulkanDevice::CreateSampler(const GPUSampler::Config& config, Error* error /* = nullptr */)
{
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
      ERROR_LOG("Unsupported border color: {:08X}", config.border_color.GetValue());
      return {};
    }

    ci.borderColor = border_color_mapping[i].vk_color;
  }

  VkSampler sampler = VK_NULL_HANDLE;
  VkResult res = vkCreateSampler(m_device, &ci, nullptr, &sampler);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSampler() failed: ");
    Vulkan::SetErrorObject(error, "vkCreateSampler() failed: ", res);
    return {};
  }

  return std::unique_ptr<GPUSampler>(new VulkanSampler(sampler));
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

#ifdef ENABLE_GPU_OBJECT_NAMES

void VulkanTextureBuffer::SetDebugName(std::string_view name)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  Vulkan::SetObjectName(dev.GetVulkanDevice(), m_buffer.GetBuffer(), name);
  if (m_buffer_view != VK_NULL_HANDLE)
    Vulkan::SetObjectName(dev.GetVulkanDevice(), m_buffer_view, name);
}

#endif

std::unique_ptr<GPUTextureBuffer> VulkanDevice::CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                    u32 size_in_elements, Error* error)
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
    Error::SetStringView(error, "Failed to allocate persistent descriptor set for texture buffer.");
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
      ERROR_LOG("Failed to create buffer view for texture buffer.");
      tb->Destroy(false);
      return {};
    }

    dsub.AddBufferViewDescriptorWrite(tb->m_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                      tb->m_buffer_view);
  }
  dsub.Update(m_device, false);

  return tb;
}

VulkanDownloadTexture::VulkanDownloadTexture(u32 width, u32 height, GPUTexture::Format format, VmaAllocation allocation,
                                             VkDeviceMemory memory, VkBuffer buffer, VkDeviceSize memory_offset,
                                             const u8* map_ptr, u32 map_pitch)
  : GPUDownloadTexture(width, height, format, (memory != VK_NULL_HANDLE)), m_allocation(allocation), m_memory(memory),
    m_buffer(buffer), m_memory_offset(memory_offset)
{
  m_map_pointer = map_ptr;
  m_current_pitch = map_pitch;
}

VulkanDownloadTexture::~VulkanDownloadTexture()
{
  if (m_allocation != VK_NULL_HANDLE)
  {
    // Buffer was created mapped, no need to manually unmap.
    VulkanDevice::GetInstance().DeferBufferDestruction(m_buffer, m_allocation);
  }
  else
  {
    // imported
    DebugAssert(m_is_imported && m_memory != VK_NULL_HANDLE);
    VulkanDevice::GetInstance().DeferBufferDestruction(m_buffer, m_memory);
  }
}

std::unique_ptr<VulkanDownloadTexture> VulkanDownloadTexture::Create(u32 width, u32 height, GPUTexture::Format format,
                                                                     void* memory, size_t memory_size,
                                                                     u32 memory_stride, Error* error)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkDeviceMemory dev_memory = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize memory_offset = 0;
  const u8* map_ptr = nullptr;
  u32 map_pitch = 0;
  u32 buffer_size = 0;

  // not importing memory?
  if (!memory)
  {
    map_pitch = Common::AlignUpPow2(GPUTexture::CalcUploadPitch(format, width), dev.GetBufferCopyRowPitchAlignment());
    buffer_size = height * map_pitch;

    const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    nullptr,
                                    0u,
                                    buffer_size,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE,
                                    0u,
                                    nullptr};

    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    aci.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

    VmaAllocationInfo ai = {};
    VkResult res = vmaCreateBuffer(VulkanDevice::GetInstance().GetAllocator(), &bci, &aci, &buffer, &allocation, &ai);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vmaCreateBuffer() failed: ", res);
      return {};
    }

    DebugAssert(ai.pMappedData);
    map_ptr = static_cast<u8*>(ai.pMappedData);
  }
  else
  {
    map_pitch = memory_stride;
    buffer_size = height * map_pitch;
    Assert(buffer_size <= memory_size);

    if (!dev.TryImportHostMemory(memory, memory_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dev_memory, &buffer,
                                 &memory_offset, error))
    {
      return {};
    }

    map_ptr = static_cast<u8*>(memory);
  }

  return std::unique_ptr<VulkanDownloadTexture>(new VulkanDownloadTexture(width, height, format, allocation, dev_memory,
                                                                          buffer, memory_offset, map_ptr, map_pitch));
}

void VulkanDownloadTexture::CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width,
                                            u32 height, u32 src_layer, u32 src_level, bool use_transfer_pitch)
{
  VulkanTexture* const vkTex = static_cast<VulkanTexture*>(src);
  VulkanDevice& dev = VulkanDevice::GetInstance();

  DebugAssert(vkTex->GetFormat() == m_format);
  DebugAssert(src_level < vkTex->GetLevels());
  DebugAssert((src_x + width) <= src->GetMipWidth(src_level) && (src_y + height) <= src->GetMipHeight(src_level));
  DebugAssert((dst_x + width) <= m_width && (dst_y + height) <= m_height);
  DebugAssert((dst_x == 0 && dst_y == 0) || !use_transfer_pitch);
  DebugAssert(!m_is_imported || !use_transfer_pitch);

  u32 copy_offset, copy_size, copy_rows;
  if (!m_is_imported)
    m_current_pitch = GetTransferPitch(use_transfer_pitch ? width : m_width, dev.GetBufferCopyRowPitchAlignment());
  GetTransferSize(dst_x, dst_y, width, height, m_current_pitch, &copy_offset, &copy_size, &copy_rows);

  dev.GetStatistics().num_downloads++;
  if (dev.InRenderPass())
    dev.EndRenderPass();
  vkTex->CommitClear();

  const VkCommandBuffer cmdbuf = dev.GetCurrentCommandBuffer();
  GL_INS_FMT("VulkanDownloadTexture::CopyFromTexture: {{{},{}}} {}x{} => {{{},{}}}", src_x, src_y, width, height, dst_x,
             dst_y);

  VulkanTexture::Layout old_layout = vkTex->GetLayout();
  if (old_layout == VulkanTexture::Layout::Undefined)
    vkTex->TransitionToLayout(cmdbuf, VulkanTexture::Layout::TransferSrc);
  else if (old_layout != VulkanTexture::Layout::TransferSrc)
    vkTex->TransitionSubresourcesToLayout(cmdbuf, 0, 1, src_level, 1, old_layout, VulkanTexture::Layout::TransferSrc);

  VkBufferImageCopy image_copy = {};
  const VkImageAspectFlags aspect = vkTex->IsDepthStencil() ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  image_copy.bufferOffset = m_memory_offset + copy_offset;
  image_copy.bufferRowLength = GPUTexture::CalcUploadRowLengthFromPitch(m_format, m_current_pitch);
  image_copy.bufferImageHeight = 0;
  image_copy.imageSubresource = {aspect, src_level, src_layer, 1u};
  image_copy.imageOffset = {static_cast<s32>(src_x), static_cast<s32>(src_y), 0};
  image_copy.imageExtent = {width, height, 1u};

  // do the copy
  vkCmdCopyImageToBuffer(cmdbuf, vkTex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &image_copy);

  // flush gpu cache
  const VkBufferMemoryBarrier buffer_info = {
    VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, // VkStructureType    sType
    nullptr,                                 // const void*        pNext
    VK_ACCESS_TRANSFER_WRITE_BIT,            // VkAccessFlags      srcAccessMask
    VK_ACCESS_HOST_READ_BIT,                 // VkAccessFlags      dstAccessMask
    VK_QUEUE_FAMILY_IGNORED,                 // uint32_t           srcQueueFamilyIndex
    VK_QUEUE_FAMILY_IGNORED,                 // uint32_t           dstQueueFamilyIndex
    m_buffer,                                // VkBuffer           buffer
    0,                                       // VkDeviceSize       offset
    copy_size                                // VkDeviceSize       size
  };
  vkCmdPipelineBarrier(cmdbuf, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &buffer_info,
                       0, nullptr);

  if (old_layout != VulkanTexture::Layout::TransferSrc && old_layout != VulkanTexture::Layout::Undefined)
    vkTex->TransitionSubresourcesToLayout(cmdbuf, 0, 1, src_level, 1, VulkanTexture::Layout::TransferSrc, old_layout);

  m_copy_fence_counter = dev.GetCurrentFenceCounter();
  m_needs_cache_invalidate = true;
  m_needs_flush = true;
}

bool VulkanDownloadTexture::Map(u32 x, u32 y, u32 width, u32 height)
{
  // Always mapped, but we might need to invalidate the cache.
  if (m_needs_cache_invalidate)
  {
    u32 copy_offset, copy_size, copy_rows;
    GetTransferSize(x, y, width, height, m_current_pitch, &copy_offset, &copy_size, &copy_rows);
    vmaInvalidateAllocation(VulkanDevice::GetInstance().GetAllocator(), m_allocation, copy_offset,
                            m_current_pitch * copy_rows);
    m_needs_cache_invalidate = false;
  }

  return true;
}

void VulkanDownloadTexture::Unmap()
{
  // Always mapped.
}

void VulkanDownloadTexture::Flush()
{
  if (!m_needs_flush)
    return;

  m_needs_flush = false;

  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (dev.GetCompletedFenceCounter() >= m_copy_fence_counter)
    return;

  // Need to execute command buffer.
  if (dev.GetCurrentFenceCounter() == m_copy_fence_counter)
  {
    if (dev.InRenderPass())
      dev.EndRenderPass();
    dev.SubmitCommandBuffer(true);
  }
  else
  {
    dev.WaitForFenceCounter(m_copy_fence_counter);
  }
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void VulkanDownloadTexture::SetDebugName(std::string_view name)
{
  if (name.empty())
    return;

  Vulkan::SetObjectName(VulkanDevice::GetInstance().GetVulkanDevice(), m_buffer, name);
}

#endif

std::unique_ptr<GPUDownloadTexture>
VulkanDevice::CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format, Error* error /* = nullptr */)
{
  return VulkanDownloadTexture::Create(width, height, format, nullptr, 0, 0, error);
}

std::unique_ptr<GPUDownloadTexture> VulkanDevice::CreateDownloadTexture(u32 width, u32 height,
                                                                        GPUTexture::Format format, void* memory,
                                                                        size_t memory_size, u32 memory_stride,
                                                                        Error* error /* = nullptr */)
{
  return VulkanDownloadTexture::Create(width, height, format, memory, memory_size, memory_stride, error);
}
