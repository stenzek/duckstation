#include "texture.h"
#include "../align.h"
#include "../assert.h"
#include "../log.h"
#include "../string_util.h"
#include "context.h"
#include "util.h"
#include <algorithm>
Log_SetChannel(Texture);

static constexpr std::array<VkFormat, static_cast<u32>(GPUTexture::Format::Count)> s_vk_mapping = {
  {VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16,
   VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_R8_UNORM, VK_FORMAT_D16_UNORM}};

static constexpr VkComponentMapping s_identity_swizzle{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

Vulkan::Texture::Texture() = default;

Vulkan::Texture::Texture(Texture&& move)
  : m_view_type(move.m_view_type), m_layout(move.m_layout), m_image(move.m_image), m_allocation(move.m_allocation),
    m_view(move.m_view)
{
  m_width = move.m_width;
  m_height = move.m_height;
  m_layers = move.m_layers;
  m_levels = move.m_levels;
  m_samples = move.m_samples;

  move.ClearBaseProperties();
  move.m_view_type = VK_IMAGE_VIEW_TYPE_2D;
  move.m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  move.m_image = VK_NULL_HANDLE;
  move.m_allocation = VK_NULL_HANDLE;
  move.m_view = VK_NULL_HANDLE;
}

Vulkan::Texture::~Texture()
{
  if (IsValid())
    Destroy(true);
}

VkFormat Vulkan::Texture::GetVkFormat(Format format)
{
  return s_vk_mapping[static_cast<u8>(format)];
}

GPUTexture::Format Vulkan::Texture::LookupBaseFormat(VkFormat vformat)
{
  for (u32 i = 0; i < static_cast<u32>(s_vk_mapping.size()); i++)
  {
    if (s_vk_mapping[i] == vformat)
      return static_cast<Format>(i);
  }
  return GPUTexture::Format::Unknown;
}

bool Vulkan::Texture::IsValid() const
{
  return (m_image != VK_NULL_HANDLE);
}

Vulkan::Texture& Vulkan::Texture::operator=(Texture&& move)
{
  if (IsValid())
    Destroy(true);

  std::swap(m_width, move.m_width);
  std::swap(m_height, move.m_height);
  std::swap(m_levels, move.m_levels);
  std::swap(m_layers, move.m_layers);
  std::swap(m_format, move.m_format);
  std::swap(m_samples, move.m_samples);
  std::swap(m_view_type, move.m_view_type);
  std::swap(m_layout, move.m_layout);
  std::swap(m_image, move.m_image);
  std::swap(m_allocation, move.m_allocation);
  std::swap(m_view, move.m_view);

  return *this;
}

bool Vulkan::Texture::Create(u32 width, u32 height, u32 levels, u32 layers, VkFormat format,
                             VkSampleCountFlagBits samples, VkImageViewType view_type, VkImageTiling tiling,
                             VkImageUsageFlags usage, bool dedicated_memory /* = false */,
                             const VkComponentMapping* swizzle /* = nullptr */)
{
  const VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                        nullptr,
                                        0,
                                        VK_IMAGE_TYPE_2D,
                                        format,
                                        {width, height, 1},
                                        levels,
                                        layers,
                                        samples,
                                        tiling,
                                        usage,
                                        VK_SHARING_MODE_EXCLUSIVE,
                                        0,
                                        nullptr,
                                        VK_IMAGE_LAYOUT_UNDEFINED};

  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
  aci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  if (dedicated_memory)
    aci.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkResult res = vmaCreateImage(g_vulkan_context->GetAllocator(), &image_info, &aci, &image, &allocation, nullptr);
  if (res != VK_SUCCESS && dedicated_memory)
  {
    // try without dedicated memory
    aci.flags &= ~VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    res = vmaCreateImage(g_vulkan_context->GetAllocator(), &image_info, &aci, &image, &allocation, nullptr);
  }
  if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY)
  {
    Log_WarningPrintf("Failed to allocate device memory for %ux%u texture", width, height);
    return false;
  }
  else if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateImage failed: ");
    return false;
  }

  const VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                           nullptr,
                                           0,
                                           image,
                                           view_type,
                                           format,
                                           swizzle ? *swizzle : s_identity_swizzle,
                                           {Util::IsDepthFormat(format) ?
                                              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) :
                                              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                                            0, levels, 0, layers}};

  VkImageView view = VK_NULL_HANDLE;
  res = vkCreateImageView(g_vulkan_context->GetDevice(), &view_info, nullptr, &view);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateImageView failed: ");
    vmaDestroyImage(g_vulkan_context->GetAllocator(), image, allocation);
    return false;
  }

  if (IsValid())
    Destroy(true);

  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_levels = static_cast<u8>(levels);
  m_layers = static_cast<u8>(layers);
  m_samples = static_cast<u8>(samples);
  m_format = LookupBaseFormat(format);
  m_view_type = view_type;
  m_image = image;
  m_allocation = allocation;
  m_view = view;
  return true;
}

bool Vulkan::Texture::Adopt(VkImage existing_image, VkImageViewType view_type, u32 width, u32 height, u32 levels,
                            u32 layers, VkFormat format, VkSampleCountFlagBits samples,
                            const VkComponentMapping* swizzle /* = nullptr */)
{
  // Only need to create the image view, this is mainly for swap chains.
  const VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                           nullptr,
                                           0,
                                           existing_image,
                                           view_type,
                                           format,
                                           swizzle ? *swizzle : s_identity_swizzle,
                                           {Util::IsDepthFormat(format) ?
                                              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) :
                                              static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                                            0, levels, 0, layers}};

  // Memory is managed by the owner of the image.
  VkImageView view = VK_NULL_HANDLE;
  VkResult res = vkCreateImageView(g_vulkan_context->GetDevice(), &view_info, nullptr, &view);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateImageView failed: ");
    return false;
  }

  if (IsValid())
    Destroy(true);

  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_levels = static_cast<u8>(levels);
  m_layers = static_cast<u8>(layers);
  m_format = LookupBaseFormat(format);
  m_samples = static_cast<u8>(samples);
  m_view_type = view_type;
  m_image = existing_image;
  m_view = view;
  return true;
}

void Vulkan::Texture::Destroy(bool defer /* = true */)
{
  if (m_view != VK_NULL_HANDLE)
  {
    if (defer)
      g_vulkan_context->DeferImageViewDestruction(m_view);
    else
      vkDestroyImageView(g_vulkan_context->GetDevice(), m_view, nullptr);
    m_view = VK_NULL_HANDLE;
  }

  // If we don't have device memory allocated, the image is not owned by us (e.g. swapchain)
  if (m_allocation != VK_NULL_HANDLE)
  {
    Assert(m_image != VK_NULL_HANDLE);
    if (defer)
      g_vulkan_context->DeferImageDestruction(m_image, m_allocation);
    else
      vmaDestroyImage(g_vulkan_context->GetAllocator(), m_image, m_allocation);
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
  }

  ClearBaseProperties();
  m_samples = VK_SAMPLE_COUNT_1_BIT;
  m_view_type = VK_IMAGE_VIEW_TYPE_2D;
  m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void Vulkan::Texture::OverrideImageLayout(VkImageLayout new_layout)
{
  m_layout = new_layout;
}

void Vulkan::Texture::TransitionToLayout(VkCommandBuffer command_buffer, VkImageLayout new_layout)
{
  if (m_layout == new_layout)
    return;
  const Vulkan::Util::DebugScope debugScope(command_buffer, "Texture::TransitionToLayout: %s",
                                            Vulkan::Util::VkImageLayoutToString(new_layout));

  TransitionSubresourcesToLayout(command_buffer, 0, m_levels, 0, m_layers, m_layout, new_layout);

  m_layout = new_layout;
}

void Vulkan::Texture::TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, u32 start_level, u32 num_levels,
                                                     u32 start_layer, u32 num_layers, VkImageLayout old_layout,
                                                     VkImageLayout new_layout)
{
  const Vulkan::Util::DebugScope debugScope(
    command_buffer, "Texture::TransitionSubresourcesToLayout: Lvl:[%u,%u) Lyr:[%u,%u) %s -> %s", start_level,
    start_level + num_levels, start_layer, start_layer + num_layers, Vulkan::Util::VkImageLayoutToString(old_layout),
    Vulkan::Util::VkImageLayoutToString(new_layout));

  VkImageMemoryBarrier barrier = {
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, // VkStructureType            sType
    nullptr,                                // const void*                pNext
    0,                                      // VkAccessFlags              srcAccessMask
    0,                                      // VkAccessFlags              dstAccessMask
    old_layout,                             // VkImageLayout              oldLayout
    new_layout,                             // VkImageLayout              newLayout
    VK_QUEUE_FAMILY_IGNORED,                // uint32_t                   srcQueueFamilyIndex
    VK_QUEUE_FAMILY_IGNORED,                // uint32_t                   dstQueueFamilyIndex
    m_image,                                // VkImage                    image
    {static_cast<VkImageAspectFlags>(IsDepthFormat(m_format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
     start_level, num_levels, start_layer, num_layers} // VkImageSubresourceRange    subresourceRange
  };

  // srcStageMask -> Stages that must complete before the barrier
  // dstStageMask -> Stages that must wait for after the barrier before beginning
  VkPipelineStageFlags srcStageMask, dstStageMask;
  switch (old_layout)
  {
    case VK_IMAGE_LAYOUT_UNDEFINED:
      // Layout undefined therefore contents undefined, and we don't care what happens to it.
      barrier.srcAccessMask = 0;
      srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;

    case VK_IMAGE_LAYOUT_PREINITIALIZED:
      // Image has been pre-initialized by the host, so ensure all writes have completed.
      barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_HOST_BIT;
      break;

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      // Image was being used as a color attachment, so ensure all writes have completed.
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      break;

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      // Image was being used as a depthstencil attachment, so ensure all writes have completed.
      barrier.srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      break;

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      // Image was being used as a shader resource, make sure all reads have finished.
      barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      // Image was being used as a copy source, ensure all reads have finished.
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      // Image was being used as a copy destination, ensure all writes have finished.
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    default:
      srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;
  }

  switch (new_layout)
  {
    case VK_IMAGE_LAYOUT_UNDEFINED:
      barrier.dstAccessMask = 0;
      dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;

    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      break;

    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      barrier.dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      break;

    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      break;

    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      break;

    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
      break;

    default:
      dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      break;
  }
  vkCmdPipelineBarrier(command_buffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkFramebuffer Vulkan::Texture::CreateFramebuffer(VkRenderPass render_pass)
{
  const VkFramebufferCreateInfo ci = {
    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0u, render_pass, 1, &m_view, m_width, m_height, m_layers};
  VkFramebuffer fb = VK_NULL_HANDLE;
  VkResult res = vkCreateFramebuffer(g_vulkan_context->GetDevice(), &ci, nullptr, &fb);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateFramebuffer() failed: ");
    return VK_NULL_HANDLE;
  }

  return fb;
}

void Vulkan::Texture::UpdateFromBuffer(VkCommandBuffer cmdbuf, u32 level, u32 layer, u32 x, u32 y, u32 width,
                                       u32 height, VkBuffer buffer, u32 buffer_offset, u32 row_length)
{
  const VkImageLayout old_layout = m_layout;
  if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    TransitionSubresourcesToLayout(cmdbuf, level, 1, layer, 1, old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  const VkBufferImageCopy bic = {static_cast<VkDeviceSize>(buffer_offset),
                                 row_length,
                                 height,
                                 {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1u},
                                 {static_cast<int32_t>(x), static_cast<int32_t>(y), 0},
                                 {width, height, 1u}};

  vkCmdCopyBufferToImage(cmdbuf, buffer, m_image, m_layout, 1, &bic);

  if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    TransitionSubresourcesToLayout(cmdbuf, level, 1, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, old_layout);
}

u32 Vulkan::Texture::CalcUpdatePitch(u32 width) const
{
  return Common::AlignUp(width * GetPixelSize(), g_vulkan_context->GetBufferCopyRowPitchAlignment());
}

u32 Vulkan::Texture::CalcUpdateRowLength(u32 pitch) const
{
  return pitch / GetPixelSize();
}

bool Vulkan::Texture::BeginUpdate(u32 width, u32 height, void** out_buffer, u32* out_pitch)
{
  const u32 pitch = CalcUpdatePitch(width);
  const u32 required_size = pitch * height;
  StreamBuffer& buffer = g_vulkan_context->GetTextureUploadBuffer();
  if (required_size > buffer.GetCurrentSize())
    return false;

  // TODO: allocate temporary buffer if this fails...
  if (!buffer.ReserveMemory(required_size, g_vulkan_context->GetBufferCopyOffsetAlignment()))
  {
    g_vulkan_context->ExecuteCommandBuffer(false);
    if (!buffer.ReserveMemory(required_size, g_vulkan_context->GetBufferCopyOffsetAlignment()))
      return false;
  }

  *out_buffer = buffer.GetCurrentHostPointer();
  *out_pitch = pitch;
  return true;
}

void Vulkan::Texture::EndUpdate(u32 x, u32 y, u32 width, u32 height, u32 level, u32 layer)
{
  const u32 pitch = CalcUpdatePitch(width);
  const u32 required_size = pitch * height;

  StreamBuffer& buffer = g_vulkan_context->GetTextureUploadBuffer();
  const u32 buffer_offset = buffer.GetCurrentOffset();
  buffer.CommitMemory(required_size);

  UpdateFromBuffer(g_vulkan_context->GetCurrentCommandBuffer(), level, layer, x, y, width, height, buffer.GetBuffer(),
                   buffer_offset, CalcUpdateRowLength(pitch));
}

bool Vulkan::Texture::Update(u32 x, u32 y, u32 width, u32 height, u32 level, u32 layer, const void* data,
                             u32 data_pitch)
{
  const u32 pitch = CalcUpdatePitch(width);
  const u32 row_length = CalcUpdateRowLength(pitch);
  const u32 required_size = pitch * height;
  StreamBuffer& sbuffer = g_vulkan_context->GetTextureUploadBuffer();

  // If the texture is larger than half our streaming buffer size, use a separate buffer.
  // Otherwise allocation will either fail, or require lots of cmdbuffer submissions.
  if (required_size > (g_vulkan_context->GetTextureUploadBuffer().GetCurrentSize() / 2))
  {
    const u32 size = data_pitch * height;
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
    VkResult res = vmaCreateBuffer(g_vulkan_context->GetAllocator(), &bci, &aci, &buffer, &allocation, &ai);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vmaCreateBuffer() failed: ");
      return VK_NULL_HANDLE;
    }

    // Immediately queue it for freeing after the command buffer finishes, since it's only needed for the copy.
    g_vulkan_context->DeferBufferDestruction(buffer, allocation);

    StringUtil::StrideMemCpy(ai.pMappedData, pitch, data, data_pitch, std::min(data_pitch, pitch), height);
    vmaFlushAllocation(g_vulkan_context->GetAllocator(), allocation, 0, size);

    UpdateFromBuffer(g_vulkan_context->GetCurrentCommandBuffer(), level, layer, x, y, width, height, buffer, 0,
                     row_length);
    return true;
  }
  else
  {
    if (!sbuffer.ReserveMemory(required_size, g_vulkan_context->GetBufferCopyOffsetAlignment()))
    {
      g_vulkan_context->ExecuteCommandBuffer(false);
      if (!sbuffer.ReserveMemory(required_size, g_vulkan_context->GetBufferCopyOffsetAlignment()))
      {
        Log_ErrorPrintf("Failed to reserve texture upload memory (%u bytes).", required_size);
        return false;
      }
    }

    const u32 buffer_offset = sbuffer.GetCurrentOffset();
    StringUtil::StrideMemCpy(sbuffer.GetCurrentHostPointer(), pitch, data, data_pitch, std::min(data_pitch, pitch),
                             height);
    sbuffer.CommitMemory(required_size);

    UpdateFromBuffer(g_vulkan_context->GetCurrentCommandBuffer(), level, layer, x, y, width, height,
                     sbuffer.GetBuffer(), buffer_offset, row_length);
    return true;
  }
}
