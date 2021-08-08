// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#include "texture.h"
#include "../assert.h"
#include "context.h"
#include "util.h"
#include <algorithm>

namespace Vulkan {
Texture::Texture() = default;

Texture::Texture(Texture&& move)
  : m_width(move.m_width), m_height(move.m_height), m_levels(move.m_levels), m_layers(move.m_layers),
    m_format(move.m_format), m_samples(move.m_samples), m_view_type(move.m_view_type), m_layout(move.m_layout),
    m_image(move.m_image), m_device_memory(move.m_device_memory), m_view(move.m_view)
{
  move.m_width = 0;
  move.m_height = 0;
  move.m_levels = 0;
  move.m_layers = 0;
  move.m_format = VK_FORMAT_UNDEFINED;
  move.m_samples = VK_SAMPLE_COUNT_1_BIT;
  move.m_view_type = VK_IMAGE_VIEW_TYPE_2D;
  move.m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  move.m_image = VK_NULL_HANDLE;
  move.m_device_memory = VK_NULL_HANDLE;
  move.m_view = VK_NULL_HANDLE;
}

Texture::~Texture()
{
  if (IsValid())
    Destroy(true);
}

Vulkan::Texture& Texture::operator=(Texture&& move)
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
  std::swap(m_device_memory, move.m_device_memory);
  std::swap(m_view, move.m_view);

  return *this;
}

bool Texture::Create(u32 width, u32 height, u32 levels, u32 layers, VkFormat format, VkSampleCountFlagBits samples,
                     VkImageViewType view_type, VkImageTiling tiling, VkImageUsageFlags usage)
{
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
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

  VkImage image = VK_NULL_HANDLE;
  VkResult res = vkCreateImage(g_vulkan_context->GetDevice(), &image_info, nullptr, &image);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateImage failed: ");
    return false;
  }

  // Allocate memory to back this texture, we want device local memory in this case
  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(g_vulkan_context->GetDevice(), image, &memory_requirements);

  VkMemoryAllocateInfo memory_info = {
    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, memory_requirements.size,
    g_vulkan_context->GetMemoryType(memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};

  VkDeviceMemory device_memory;
  res = vkAllocateMemory(g_vulkan_context->GetDevice(), &memory_info, nullptr, &device_memory);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkAllocateMemory failed: ");
    vkDestroyImage(g_vulkan_context->GetDevice(), image, nullptr);
    return false;
  }

  res = vkBindImageMemory(g_vulkan_context->GetDevice(), image, device_memory, 0);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkBindImageMemory failed: ");
    vkDestroyImage(g_vulkan_context->GetDevice(), image, nullptr);
    vkFreeMemory(g_vulkan_context->GetDevice(), device_memory, nullptr);
    return false;
  }

  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     nullptr,
                                     0,
                                     image,
                                     view_type,
                                     format,
                                     {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                     {Util::IsDepthFormat(format) ?
                                        static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) :
                                        static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                                      0, levels, 0, layers}};

  VkImageView view = VK_NULL_HANDLE;
  res = vkCreateImageView(g_vulkan_context->GetDevice(), &view_info, nullptr, &view);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateImageView failed: ");
    vkDestroyImage(g_vulkan_context->GetDevice(), image, nullptr);
    vkFreeMemory(g_vulkan_context->GetDevice(), device_memory, nullptr);
    return false;
  }

  if (IsValid())
    Destroy(true);

  m_width = width;
  m_height = height;
  m_levels = levels;
  m_layers = layers;
  m_format = format;
  m_samples = samples;
  m_view_type = view_type;
  m_image = image;
  m_device_memory = device_memory;
  m_view = view;
  return true;
}

bool Texture::Adopt(VkImage existing_image, VkImageViewType view_type, u32 width, u32 height, u32 levels, u32 layers,
                    VkFormat format, VkSampleCountFlagBits samples)
{
  // Only need to create the image view, this is mainly for swap chains.
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     nullptr,
                                     0,
                                     existing_image,
                                     view_type,
                                     format,
                                     {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
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

  m_width = width;
  m_height = height;
  m_levels = levels;
  m_layers = layers;
  m_format = format;
  m_samples = samples;
  m_view_type = view_type;
  m_image = existing_image;
  m_view = view;
  return true;
}

void Texture::Destroy(bool defer /* = true */)
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
  if (m_device_memory != VK_NULL_HANDLE)
  {
    DebugAssert(m_image != VK_NULL_HANDLE);
    if (defer)
      g_vulkan_context->DeferImageDestruction(m_image);
    else
      vkDestroyImage(g_vulkan_context->GetDevice(), m_image, nullptr);
    m_image = VK_NULL_HANDLE;

    if (defer)
      g_vulkan_context->DeferDeviceMemoryDestruction(m_device_memory);
    else
      vkFreeMemory(g_vulkan_context->GetDevice(), m_device_memory, nullptr);
    m_device_memory = VK_NULL_HANDLE;
  }

  m_width = 0;
  m_height = 0;
  m_levels = 0;
  m_layers = 0;
  m_format = VK_FORMAT_UNDEFINED;
  m_samples = VK_SAMPLE_COUNT_1_BIT;
  m_view_type = VK_IMAGE_VIEW_TYPE_2D;
  m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  m_image = VK_NULL_HANDLE;
  m_device_memory = VK_NULL_HANDLE;
  m_view = VK_NULL_HANDLE;
}

void Texture::OverrideImageLayout(VkImageLayout new_layout)
{
  m_layout = new_layout;
}

void Texture::TransitionToLayout(VkCommandBuffer command_buffer, VkImageLayout new_layout)
{
  if (m_layout == new_layout)
    return;
  const Vulkan::Util::DebugScope debugScope(command_buffer, "Texture::TransitionToLayout: %s",
                                            Vulkan::Util::VkImageLayoutToString(new_layout));

  TransitionSubresourcesToLayout(command_buffer, 0, m_levels, 0, m_layers, m_layout, new_layout);

  m_layout = new_layout;
}

void Texture::TransitionSubresourcesToLayout(VkCommandBuffer command_buffer, u32 start_level, u32 num_levels,
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
    {static_cast<VkImageAspectFlags>(Util::IsDepthFormat(m_format) ? VK_IMAGE_ASPECT_DEPTH_BIT :
                                                                     VK_IMAGE_ASPECT_COLOR_BIT),
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

VkFramebuffer Texture::CreateFramebuffer(VkRenderPass render_pass)
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

void Texture::UpdateFromBuffer(VkCommandBuffer cmdbuf, u32 level, u32 layer, u32 x, u32 y, u32 width, u32 height,
                               VkBuffer buffer, u32 buffer_offset)
{
  const VkImageLayout old_layout = m_layout;
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "Texture::UpdateFromBuffer: Lvl:%u Lyr:%u {%u,%u} %ux%u", level,
                                            layer, x, y, width, height);
  TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  const VkBufferImageCopy bic = {static_cast<VkDeviceSize>(buffer_offset),
                                 width,
                                 height,
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                                 {static_cast<int32_t>(x), static_cast<int32_t>(y), 0},
                                 {width, height, 1u}};

  vkCmdCopyBufferToImage(cmdbuf, buffer, m_image, m_layout, 1, &bic);

  TransitionToLayout(cmdbuf, old_layout);
}

} // namespace Vulkan
