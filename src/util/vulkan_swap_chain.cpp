// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "vulkan_swap_chain.h"
#include "vulkan_builders.h"
#include "vulkan_device.h"

#include "common/assert.h"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cmath>

#if defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#endif

#if defined(VK_USE_PLATFORM_METAL_EXT)
#include "util/metal_layer.h"
#endif

Log_SetChannel(VulkanDevice);

static VkFormat GetLinearFormat(VkFormat format)
{
  switch (format)
  {
    case VK_FORMAT_R8_SRGB:
      return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_SRGB:
      return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8B8_SRGB:
      return VK_FORMAT_R8G8B8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8_SRGB:
      return VK_FORMAT_B8G8R8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
      return VK_FORMAT_B8G8R8A8_UNORM;
    default:
      return format;
  }
}

static const char* PresentModeToString(VkPresentModeKHR mode)
{
  switch (mode)
  {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
      return "VK_PRESENT_MODE_IMMEDIATE_KHR";

    case VK_PRESENT_MODE_MAILBOX_KHR:
      return "VK_PRESENT_MODE_MAILBOX_KHR";

    case VK_PRESENT_MODE_FIFO_KHR:
      return "VK_PRESENT_MODE_FIFO_KHR";

    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
      return "VK_PRESENT_MODE_FIFO_RELAXED_KHR";

    case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR:
      return "VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR";

    case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR:
      return "VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR";

    default:
      return "UNKNOWN_VK_PRESENT_MODE";
  }
}

static VkPresentModeKHR GetPreferredPresentModeForVsyncMode(DisplaySyncMode mode)
{
  static constexpr std::array<VkPresentModeKHR, static_cast<size_t>(DisplaySyncMode::Count)> modes = {{
    VK_PRESENT_MODE_IMMEDIATE_KHR,    // Disabled
    VK_PRESENT_MODE_FIFO_KHR,         // VSync
    VK_PRESENT_MODE_FIFO_RELAXED_KHR, // VSyncRelaxed
    VK_PRESENT_MODE_IMMEDIATE_KHR,    // VRR ??
  }};

  return modes[static_cast<size_t>(mode)];
}

VulkanSwapChain::VulkanSwapChain(const WindowInfo& wi, VkSurfaceKHR surface, VkPresentModeKHR requested_present_mode,
                                 std::optional<bool> exclusive_fullscreen_control)
  : m_window_info(wi), m_surface(surface), m_requested_present_mode(requested_present_mode),
    m_exclusive_fullscreen_control(exclusive_fullscreen_control)
{
}

VulkanSwapChain::~VulkanSwapChain()
{
  DestroySwapChainImages();
  DestroySwapChain();
  DestroySurface();
}

VkSurfaceKHR VulkanSwapChain::CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device, WindowInfo* wi)
{
#if defined(VK_USE_PLATFORM_WIN32_KHR)
  if (wi->type == WindowInfo::Type::Win32)
  {
    VkWin32SurfaceCreateInfoKHR surface_create_info = {
      VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, // VkStructureType               sType
      nullptr,                                         // const void*                   pNext
      0,                                               // VkWin32SurfaceCreateFlagsKHR  flags
      nullptr,                                         // HINSTANCE                     hinstance
      reinterpret_cast<HWND>(wi->window_handle)        // HWND                          hwnd
    };

    VkSurfaceKHR surface;
    VkResult res = vkCreateWin32SurfaceKHR(instance, &surface_create_info, nullptr, &surface);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateWin32SurfaceKHR failed: ");
      return VK_NULL_HANDLE;
    }

    return surface;
  }
#endif

#if defined(VK_USE_PLATFORM_METAL_EXT)
  if (wi->type == WindowInfo::Type::MacOS)
  {
    // TODO: FIXME
    if (!wi->surface_handle && !CocoaTools::CreateMetalLayer(wi))
      return VK_NULL_HANDLE;

    VkMetalSurfaceCreateInfoEXT surface_create_info = {VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, nullptr, 0,
                                                       static_cast<const CAMetalLayer*>(wi->surface_handle)};

    VkSurfaceKHR surface;
    VkResult res = vkCreateMetalSurfaceEXT(instance, &surface_create_info, nullptr, &surface);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateMetalSurfaceEXT failed: ");
      return VK_NULL_HANDLE;
    }

    return surface;
  }
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
  if (wi->type == WindowInfo::Type::Android)
  {
    VkAndroidSurfaceCreateInfoKHR surface_create_info = {
      VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,  // VkStructureType                sType
      nullptr,                                            // const void*                    pNext
      0,                                                  // VkAndroidSurfaceCreateFlagsKHR flags
      reinterpret_cast<ANativeWindow*>(wi->window_handle) // ANativeWindow* window
    };

    VkSurfaceKHR surface;
    VkResult res = vkCreateAndroidSurfaceKHR(instance, &surface_create_info, nullptr, &surface);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateAndroidSurfaceKHR failed: ");
      return VK_NULL_HANDLE;
    }

    return surface;
  }
#endif

#if defined(VK_USE_PLATFORM_XLIB_KHR)
  if (wi->type == WindowInfo::Type::X11)
  {
    VkXlibSurfaceCreateInfoKHR surface_create_info = {
      VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, // VkStructureType               sType
      nullptr,                                        // const void*                   pNext
      0,                                              // VkXlibSurfaceCreateFlagsKHR   flags
      static_cast<Display*>(wi->display_connection),  // Display*                      dpy
      reinterpret_cast<Window>(wi->window_handle)     // Window                        window
    };

    VkSurfaceKHR surface;
    VkResult res = vkCreateXlibSurfaceKHR(instance, &surface_create_info, nullptr, &surface);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateXlibSurfaceKHR failed: ");
      return VK_NULL_HANDLE;
    }

    return surface;
  }
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
  if (wi->type == WindowInfo::Type::Wayland)
  {
    VkWaylandSurfaceCreateInfoKHR surface_create_info = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0,
                                                         static_cast<struct wl_display*>(wi->display_connection),
                                                         static_cast<struct wl_surface*>(wi->window_handle)};

    VkSurfaceKHR surface;
    VkResult res = vkCreateWaylandSurfaceKHR(instance, &surface_create_info, nullptr, &surface);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateWaylandSurfaceEXT failed: ");
      return VK_NULL_HANDLE;
    }

    return surface;
  }
#endif

  return VK_NULL_HANDLE;
}

void VulkanSwapChain::DestroyVulkanSurface(VkInstance instance, WindowInfo* wi, VkSurfaceKHR surface)
{
  vkDestroySurfaceKHR(VulkanDevice::GetInstance().GetVulkanInstance(), surface, nullptr);

#if defined(__APPLE__)
  if (wi->type == WindowInfo::Type::MacOS && wi->surface_handle)
    CocoaTools::DestroyMetalLayer(wi);
#endif
}

std::unique_ptr<VulkanSwapChain> VulkanSwapChain::Create(const WindowInfo& wi, VkSurfaceKHR surface,
                                                         DisplaySyncMode sync_mode,
                                                         std::optional<bool> exclusive_fullscreen_control)
{
  const VkPresentModeKHR requested_mode = GetPreferredPresentModeForVsyncMode(sync_mode);
  std::unique_ptr<VulkanSwapChain> swap_chain =
    std::unique_ptr<VulkanSwapChain>(new VulkanSwapChain(wi, surface, requested_mode, exclusive_fullscreen_control));
  if (!swap_chain->CreateSwapChain())
    return nullptr;

  return swap_chain;
}

std::optional<VkSurfaceFormatKHR> VulkanSwapChain::SelectSurfaceFormat(VkSurfaceKHR surface)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  u32 format_count;
  VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(dev.GetVulkanPhysicalDevice(), surface, &format_count, nullptr);
  if (res != VK_SUCCESS || format_count == 0)
  {
    LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: ");
    return std::nullopt;
  }

  std::vector<VkSurfaceFormatKHR> surface_formats(format_count);
  res =
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev.GetVulkanPhysicalDevice(), surface, &format_count, surface_formats.data());
  Assert(res == VK_SUCCESS);

  // If there is a single undefined surface format, the device doesn't care, so we'll just use RGBA
  const auto has_format = [&surface_formats](VkFormat fmt) {
    return std::any_of(surface_formats.begin(), surface_formats.end(), [fmt](const VkSurfaceFormatKHR& sf) {
      return (sf.format == fmt || GetLinearFormat(sf.format) == fmt);
    });
  };
  if (has_format(VK_FORMAT_UNDEFINED))
    return VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

  // Prefer 8-bit formats.
  for (VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16,
                          VK_FORMAT_R5G5B5A1_UNORM_PACK16})
  {
    if (has_format(format))
      return VkSurfaceFormatKHR{format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  }

  Log_ErrorPrintf("Failed to find a suitable format for swap chain buffers. Available formats were:");
  for (const VkSurfaceFormatKHR& sf : surface_formats)
    Log_ErrorPrintf("  %u", static_cast<unsigned>(sf.format));

  return std::nullopt;
}

std::optional<VkPresentModeKHR> VulkanSwapChain::SelectPresentMode(VkSurfaceKHR surface,
                                                                   VkPresentModeKHR requested_mode)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  VkResult res;
  u32 mode_count;
  res = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.GetVulkanPhysicalDevice(), surface, &mode_count, nullptr);
  if (res != VK_SUCCESS || mode_count == 0)
  {
    LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: ");
    return std::nullopt;
  }

  std::vector<VkPresentModeKHR> present_modes(mode_count);
  res = vkGetPhysicalDeviceSurfacePresentModesKHR(dev.GetVulkanPhysicalDevice(), surface, &mode_count,
                                                  present_modes.data());
  Assert(res == VK_SUCCESS);

  // Checks if a particular mode is supported, if it is, returns that mode.
  auto CheckForMode = [&present_modes](VkPresentModeKHR check_mode) {
    auto it = std::find_if(present_modes.begin(), present_modes.end(),
                           [check_mode](VkPresentModeKHR mode) { return check_mode == mode; });
    return it != present_modes.end();
  };

  // Use preferred mode if available.
  VkPresentModeKHR selected_mode;
  if (CheckForMode(requested_mode))
  {
    selected_mode = requested_mode;
  }
  else if (requested_mode != VK_PRESENT_MODE_FIFO_KHR && CheckForMode(VK_PRESENT_MODE_MAILBOX_KHR))
  {
    // Prefer mailbox over fifo for adaptive vsync/no-vsync.
    selected_mode = VK_PRESENT_MODE_MAILBOX_KHR;
  }
  else if (requested_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR && CheckForMode(VK_PRESENT_MODE_FIFO_KHR))
  {
    // Fallback to FIFO if we're using any kind of vsync.
    // This should never fail, FIFO is mandated.
    selected_mode = VK_PRESENT_MODE_FIFO_KHR;
  }
  else
  {
    // Fall back to whatever is available.
    selected_mode = present_modes[0];
  }

  Log_DevPrintf("(SwapChain) Preferred present mode: %s, selected: %s", PresentModeToString(requested_mode),
                PresentModeToString(selected_mode));

  return selected_mode;
}

bool VulkanSwapChain::CreateSwapChain()
{
  VulkanDevice& dev = VulkanDevice::GetInstance();

  // Select swap chain format and present mode
  std::optional<VkSurfaceFormatKHR> surface_format = SelectSurfaceFormat(m_surface);
  std::optional<VkPresentModeKHR> present_mode = SelectPresentMode(m_surface, m_requested_present_mode);
  if (!surface_format.has_value() || !present_mode.has_value())
    return false;

  // Look up surface properties to determine image count and dimensions
  VkSurfaceCapabilitiesKHR surface_capabilities;
  VkResult res =
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.GetVulkanPhysicalDevice(), m_surface, &surface_capabilities);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: ");
    return false;
  }

  // Select number of images in swap chain, we prefer one buffer in the background to work on
  u32 image_count = std::max(surface_capabilities.minImageCount + 1u, 2u);

  // maxImageCount can be zero, in which case there isn't an upper limit on the number of buffers.
  if (surface_capabilities.maxImageCount > 0)
    image_count = std::min(image_count, surface_capabilities.maxImageCount);

  // Determine the dimensions of the swap chain. Values of -1 indicate the size we specify here
  // determines window size? Android sometimes lags updating currentExtent, so don't use it.
  VkExtent2D size = surface_capabilities.currentExtent;
#ifndef __ANDROID__
  if (size.width == UINT32_MAX)
#endif
  {
    size.width = m_window_info.surface_width;
    size.height = m_window_info.surface_height;
  }
  size.width =
    std::clamp(size.width, surface_capabilities.minImageExtent.width, surface_capabilities.maxImageExtent.width);
  size.height =
    std::clamp(size.height, surface_capabilities.minImageExtent.height, surface_capabilities.maxImageExtent.height);

  // Prefer identity transform if possible
  VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  if (!(surface_capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
    transform = surface_capabilities.currentTransform;

  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
  {
    // If we only support pre-multiplied/post-multiplied... :/
    if (surface_capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
      alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  }

  // Select swap chain flags, we only need a colour attachment
  VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if ((surface_capabilities.supportedUsageFlags & image_usage) != image_usage)
  {
    Log_ErrorPrintf("Vulkan: Swap chain does not support usage as color attachment");
    return false;
  }

  // Store the old/current swap chain when recreating for resize
  // Old swap chain is destroyed regardless of whether the create call succeeds
  VkSwapchainKHR old_swap_chain = m_swap_chain;
  m_swap_chain = VK_NULL_HANDLE;

  // Now we can actually create the swap chain
  VkSwapchainCreateInfoKHR swap_chain_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                              nullptr,
                                              0,
                                              m_surface,
                                              image_count,
                                              surface_format->format,
                                              surface_format->colorSpace,
                                              size,
                                              1u,
                                              image_usage,
                                              VK_SHARING_MODE_EXCLUSIVE,
                                              0,
                                              nullptr,
                                              transform,
                                              alpha,
                                              present_mode.value(),
                                              VK_TRUE,
                                              old_swap_chain};
  std::array<uint32_t, 2> indices = {{
    dev.GetGraphicsQueueFamilyIndex(),
    dev.GetPresentQueueFamilyIndex(),
  }};
  if (dev.GetGraphicsQueueFamilyIndex() != dev.GetPresentQueueFamilyIndex())
  {
    swap_chain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    swap_chain_info.queueFamilyIndexCount = 2;
    swap_chain_info.pQueueFamilyIndices = indices.data();
  }

#ifdef _WIN32
  VkSurfaceFullScreenExclusiveInfoEXT exclusive_info = {VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
                                                        nullptr, VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT};
  VkSurfaceFullScreenExclusiveWin32InfoEXT exclusive_win32_info = {
    VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT, nullptr, NULL};
  if (m_exclusive_fullscreen_control.has_value())
  {
    if (dev.GetOptionalExtensions().vk_ext_full_screen_exclusive)
    {
      exclusive_info.fullScreenExclusive =
        (m_exclusive_fullscreen_control.value() ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT :
                                                  VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT);

      exclusive_win32_info.hmonitor =
        MonitorFromWindow(reinterpret_cast<HWND>(m_window_info.window_handle), MONITOR_DEFAULTTONEAREST);
      if (!exclusive_win32_info.hmonitor)
        Log_ErrorPrintf("MonitorFromWindow() for exclusive fullscreen exclusive override failed.");

      Vulkan::AddPointerToChain(&swap_chain_info, &exclusive_info);
      Vulkan::AddPointerToChain(&swap_chain_info, &exclusive_win32_info);
    }
    else
    {
      Log_ErrorPrintf("Exclusive fullscreen control requested, but VK_EXT_full_screen_exclusive is not supported.");
    }
  }
#else
  if (m_exclusive_fullscreen_control.has_value())
    Log_ErrorPrintf("Exclusive fullscreen control requested, but is not supported on this platform.");
#endif

  res = vkCreateSwapchainKHR(dev.GetVulkanDevice(), &swap_chain_info, nullptr, &m_swap_chain);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSwapchainKHR failed: ");
    return false;
  }

  // Now destroy the old swap chain, since it's been recreated.
  // We can do this immediately since all work should have been completed before calling resize.
  if (old_swap_chain != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(dev.GetVulkanDevice(), old_swap_chain, nullptr);

  m_format = surface_format->format;
  m_window_info.surface_width = std::max(1u, size.width);
  m_window_info.surface_height = std::max(1u, size.height);
  m_window_info.surface_format = VulkanDevice::GetFormatForVkFormat(surface_format->format);
  m_actual_present_mode = present_mode.value();
  if (m_window_info.surface_format == GPUTexture::Format::Unknown)
  {
    Log_ErrorPrintf("Unknown Vulkan surface format %u", static_cast<u32>(surface_format->format));
    return false;
  }

  // Get and create images.
  Assert(m_images.empty());

  res = vkGetSwapchainImagesKHR(dev.GetVulkanDevice(), m_swap_chain, &image_count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetSwapchainImagesKHR failed: ");
    return false;
  }

  std::vector<VkImage> images(image_count);
  res = vkGetSwapchainImagesKHR(dev.GetVulkanDevice(), m_swap_chain, &image_count, images.data());
  Assert(res == VK_SUCCESS);

  VkRenderPass render_pass = dev.GetSwapChainRenderPass(m_window_info.surface_format, VK_ATTACHMENT_LOAD_OP_CLEAR);
  if (render_pass == VK_NULL_HANDLE)
    return false;

  Vulkan::FramebufferBuilder fbb;
  m_images.reserve(image_count);
  m_current_image = 0;
  for (u32 i = 0; i < image_count; i++)
  {
    Image image = {};
    image.image = images[i];

    const VkImageViewCreateInfo view_info = {
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      nullptr,
      0,
      images[i],
      VK_IMAGE_VIEW_TYPE_2D,
      m_format,
      {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
       VK_COMPONENT_SWIZZLE_IDENTITY},
      {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
    };
    if ((res = vkCreateImageView(dev.GetVulkanDevice(), &view_info, nullptr, &image.view)) != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateImageView() failed: ");
      return false;
    }

    fbb.AddAttachment(image.view);
    fbb.SetRenderPass(render_pass);
    fbb.SetSize(size.width, size.height, 1);
    if ((image.framebuffer = fbb.Create(dev.GetVulkanDevice())) == VK_NULL_HANDLE)
    {
      vkDestroyImageView(dev.GetVulkanDevice(), image.view, nullptr);
      return false;
    }

    m_images.push_back(image);
  }

  // We don't actually need +1 semaphores, or, more than one really.
  // But, the validation layer gets cranky if we don't fence wait before the next image acquire.
  // So, add an additional semaphore to ensure that we're never acquiring before fence waiting.
  const u32 semaphore_count = image_count + 1;
  m_semaphores.reserve(semaphore_count);
  m_current_semaphore = 0;
  for (u32 i = 0; i < semaphore_count; i++)
  {
    ImageSemaphores sema;

    const VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
    res = vkCreateSemaphore(dev.GetVulkanDevice(), &semaphore_info, nullptr, &sema.available_semaphore);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateSemaphore failed: ");
      return false;
    }

    res = vkCreateSemaphore(dev.GetVulkanDevice(), &semaphore_info, nullptr, &sema.rendering_finished_semaphore);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateSemaphore failed: ");
      vkDestroySemaphore(dev.GetVulkanDevice(), sema.available_semaphore, nullptr);
      return false;
    }

    m_semaphores.push_back(sema);
  }

  return true;
}

void VulkanSwapChain::DestroySwapChainImages()
{
  VulkanDevice& dev = VulkanDevice::GetInstance();
  for (const auto& it : m_images)
  {
    // don't defer view destruction, images are no longer valid
    vkDestroyFramebuffer(dev.GetVulkanDevice(), it.framebuffer, nullptr);
    vkDestroyImageView(dev.GetVulkanDevice(), it.view, nullptr);
  }
  m_images.clear();
  for (auto& it : m_semaphores)
  {
    vkDestroySemaphore(dev.GetVulkanDevice(), it.rendering_finished_semaphore, nullptr);
    vkDestroySemaphore(dev.GetVulkanDevice(), it.available_semaphore, nullptr);
  }
  m_semaphores.clear();

  m_image_acquire_result.reset();
}

void VulkanSwapChain::DestroySwapChain()
{
  DestroySwapChainImages();

  if (m_swap_chain == VK_NULL_HANDLE)
    return;

  vkDestroySwapchainKHR(VulkanDevice::GetInstance().GetVulkanDevice(), m_swap_chain, nullptr);
  m_swap_chain = VK_NULL_HANDLE;
  m_window_info.surface_width = 0;
  m_window_info.surface_height = 0;
}

VkResult VulkanSwapChain::AcquireNextImage()
{
  if (m_image_acquire_result.has_value())
    return m_image_acquire_result.value();

  if (!m_swap_chain)
    return VK_ERROR_SURFACE_LOST_KHR;

  // Use a different semaphore for each image.
  m_current_semaphore = (m_current_semaphore + 1) % static_cast<u32>(m_semaphores.size());

  const VkResult res =
    vkAcquireNextImageKHR(VulkanDevice::GetInstance().GetVulkanDevice(), m_swap_chain, UINT64_MAX,
                          m_semaphores[m_current_semaphore].available_semaphore, VK_NULL_HANDLE, &m_current_image);
  m_image_acquire_result = res;
  return res;
}

void VulkanSwapChain::ReleaseCurrentImage()
{
  m_image_acquire_result.reset();
}

bool VulkanSwapChain::ResizeSwapChain(u32 new_width, u32 new_height, float new_scale)
{
  DestroySwapChainImages();

  if (new_width != 0 && new_height != 0)
  {
    m_window_info.surface_width = new_width;
    m_window_info.surface_height = new_height;
  }

  m_window_info.surface_scale = new_scale;

  if (!CreateSwapChain())
  {
    DestroySwapChain();
    return false;
  }

  return true;
}

bool VulkanSwapChain::SetSyncMode(DisplaySyncMode mode)
{
  const VkPresentModeKHR present_mode = GetPreferredPresentModeForVsyncMode(mode);
  if (m_requested_present_mode == present_mode)
    return true;

  m_requested_present_mode = present_mode;

  // Recreate the swap chain with the new present mode.
  Log_VerbosePrintf("Recreating swap chain to change present mode.");
  DestroySwapChainImages();
  if (!CreateSwapChain())
  {
    DestroySwapChain();
    return false;
  }

  return true;
}

bool VulkanSwapChain::RecreateSurface(const WindowInfo& new_wi)
{
  VulkanDevice& dev = VulkanDevice::GetInstance();

  // Destroy the old swap chain, images, and surface.
  DestroySwapChain();
  DestroySurface();

  // Re-create the surface with the new native handle
  m_window_info = new_wi;
  m_surface = CreateVulkanSurface(dev.GetVulkanInstance(), dev.GetVulkanPhysicalDevice(), &m_window_info);
  if (m_surface == VK_NULL_HANDLE)
    return false;

  // The validation layers get angry at us if we don't call this before creating the swapchain.
  VkBool32 present_supported = VK_TRUE;
  VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(dev.GetVulkanPhysicalDevice(), dev.GetPresentQueueFamilyIndex(),
                                                      m_surface, &present_supported);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ");
    return false;
  }
  if (!present_supported)
  {
    Panic("Recreated surface does not support presenting.");
    return false;
  }

  // Finally re-create the swap chain
  if (!CreateSwapChain())
  {
    DestroySwapChain();
    return false;
  }

  return true;
}

void VulkanSwapChain::DestroySurface()
{
  if (m_surface == VK_NULL_HANDLE)
    return;

  DestroyVulkanSurface(VulkanDevice::GetInstance().GetVulkanInstance(), &m_window_info, m_surface);
  m_surface = VK_NULL_HANDLE;
}
