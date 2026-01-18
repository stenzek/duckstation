// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "vulkan_swap_chain.h"
#include "vulkan_builders.h"
#include "vulkan_device.h"
#include "vulkan_loader.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"

#ifdef __APPLE__
#include "common/cocoa_tools.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>

#ifdef ENABLE_SDL
#include <SDL3/SDL_vulkan.h>
#endif

LOG_CHANNEL(GPUDevice);

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

VulkanSwapChain::VulkanSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode, bool allow_present_throttle,
                                 std::optional<bool> exclusive_fullscreen_control)
  : GPUSwapChain(wi, vsync_mode, allow_present_throttle), m_exclusive_fullscreen_control(exclusive_fullscreen_control)
{
}

VulkanSwapChain::~VulkanSwapChain()
{
  Destroy(VulkanDevice::GetInstance(), true);
}

bool VulkanSwapChain::CreateSurface(VkPhysicalDevice physical_device, Error* error)
{
#if defined(VK_USE_PLATFORM_WIN32_KHR)
  if (m_window_info.type == WindowInfoType::Win32)
  {
    const VkWin32SurfaceCreateInfoKHR surface_create_info = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                                             .pNext = nullptr,
                                                             .flags = 0,
                                                             .hinstance = NULL,
                                                             .hwnd = static_cast<HWND>(m_window_info.window_handle)};
    const VkResult res =
      vkCreateWin32SurfaceKHR(VulkanLoader::GetVulkanInstance(), &surface_create_info, nullptr, &m_surface);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateWin32SurfaceKHR() failed: ", res);
      return false;
    }

    return true;
  }
#endif

#if defined(VK_USE_PLATFORM_METAL_EXT)
  if (m_window_info.type == WindowInfoType::MacOS)
  {
    m_metal_layer = CocoaTools::CreateMetalLayer(m_window_info.window_handle, error);
    if (!m_metal_layer)
      return false;

    const VkMetalSurfaceCreateInfoEXT surface_create_info = {.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                                                             .pNext = nullptr,
                                                             .flags = 0,
                                                             .pLayer = static_cast<const CAMetalLayer*>(m_metal_layer)};
    const VkResult res =
      vkCreateMetalSurfaceEXT(VulkanLoader::GetVulkanInstance(), &surface_create_info, nullptr, &m_surface);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateMetalSurfaceEXT failed: ", res);
      return false;
    }

    return true;
  }
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
  if (m_window_info.type == WindowInfoType::Android)
  {
    const VkAndroidSurfaceCreateInfoKHR surface_create_info = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .window = static_cast<ANativeWindow*>(m_window_info.window_handle)};
    const VkResult res =
      vkCreateAndroidSurfaceKHR(VulkanLoader::GetVulkanInstance(), &surface_create_info, nullptr, &m_surface);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateAndroidSurfaceKHR failed: ", res);
      return false;
    }

    return true;
  }
#endif

#if defined(VK_USE_PLATFORM_XCB_KHR)
  if (m_window_info.type == WindowInfoType::XCB)
  {
    const VkXcbSurfaceCreateInfoKHR surface_create_info = {
      .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .connection = static_cast<xcb_connection_t*>(m_window_info.display_connection),
      .window = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(m_window_info.window_handle))};
    const VkResult res =
      vkCreateXcbSurfaceKHR(VulkanLoader::GetVulkanInstance(), &surface_create_info, nullptr, &m_surface);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateXcbSurfaceKHR failed: ", res);
      return false;
    }

    return true;
  }
#endif

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
  if (m_window_info.type == WindowInfoType::Wayland)
  {
    const VkWaylandSurfaceCreateInfoKHR surface_create_info = {
      .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
      .pNext = nullptr,
      .flags = 0,
      .display = static_cast<struct wl_display*>(m_window_info.display_connection),
      .surface = static_cast<struct wl_surface*>(m_window_info.window_handle)};
    VkResult res =
      vkCreateWaylandSurfaceKHR(VulkanLoader::GetVulkanInstance(), &surface_create_info, nullptr, &m_surface);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateWaylandSurfaceEXT failed: ", res);
      return false;
    }

    return true;
  }
#endif

#if defined(ENABLE_SDL)
  if (m_window_info.type == WindowInfoType::SDL)
  {
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(m_window_info.window_handle),
                                  VulkanLoader::GetVulkanInstance(), nullptr, &m_surface))
    {
      Error::SetStringFmt(error, "SDL_Vulkan_CreateSurface() failed: {}", SDL_GetError());
      return false;
    }

    return true;
  }
#endif

  Error::SetStringFmt(error, "Unhandled window type: {}", static_cast<unsigned>(m_window_info.type));
  return false;
}

void VulkanSwapChain::DestroySurface()
{
  if (m_surface != VK_NULL_HANDLE)
  {
    vkDestroySurfaceKHR(VulkanLoader::GetVulkanInstance(), m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
  }

#if defined(__APPLE__)
  if (m_metal_layer)
  {
    CocoaTools::DestroyMetalLayer(m_window_info.window_handle, m_metal_layer);
    m_metal_layer = nullptr;
  }
#endif
}

std::optional<VkSurfaceFormatKHR> VulkanSwapChain::SelectSurfaceFormat(VkPhysicalDevice physdev, Error* error)
{
  u32 format_count;
  VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, m_surface, &format_count, nullptr);
  if (res != VK_SUCCESS || format_count == 0)
  {
    Vulkan::SetErrorObject(error, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: ", res);
    return std::nullopt;
  }

  std::vector<VkSurfaceFormatKHR> surface_formats(format_count);
  res = vkGetPhysicalDeviceSurfaceFormatsKHR(physdev, m_surface, &format_count, surface_formats.data());
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

  SmallString errormsg("Failed to find a suitable format for swap chain buffers. Available formats were:");
  for (const VkSurfaceFormatKHR& sf : surface_formats)
    errormsg.append_format(" {}", static_cast<unsigned>(sf.format));
  Error::SetStringView(error, errormsg);
  return std::nullopt;
}

std::optional<VkPresentModeKHR> VulkanSwapChain::SelectPresentMode(VkPhysicalDevice physdev, GPUVSyncMode& vsync_mode,
                                                                   Error* error)
{

  VkResult res;
  u32 mode_count;
  res = vkGetPhysicalDeviceSurfacePresentModesKHR(physdev, m_surface, &mode_count, nullptr);
  if (res != VK_SUCCESS || mode_count == 0)
  {
    Vulkan::SetErrorObject(error, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: ", res);
    return std::nullopt;
  }

  std::vector<VkPresentModeKHR> present_modes(mode_count);
  res = vkGetPhysicalDeviceSurfacePresentModesKHR(physdev, m_surface, &mode_count, present_modes.data());
  Assert(res == VK_SUCCESS);

  // Checks if a particular mode is supported, if it is, returns that mode.
  const auto CheckForMode = [&present_modes](VkPresentModeKHR check_mode) {
    auto it = std::find_if(present_modes.begin(), present_modes.end(),
                           [check_mode](VkPresentModeKHR mode) { return check_mode == mode; });
    return it != present_modes.end();
  };

  switch (vsync_mode)
  {
    case GPUVSyncMode::Disabled:
    {
      // Prefer immediate > mailbox > fifo.
      if (CheckForMode(VK_PRESENT_MODE_IMMEDIATE_KHR))
      {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
      }
      else if (CheckForMode(VK_PRESENT_MODE_MAILBOX_KHR))
      {
        WARNING_LOG("Immediate not supported for vsync-disabled, using mailbox.");
        return VK_PRESENT_MODE_MAILBOX_KHR;
      }
      else
      {
        WARNING_LOG("Mailbox not supported for vsync-disabled, using FIFO.");
        vsync_mode = GPUVSyncMode::FIFO;
        return VK_PRESENT_MODE_FIFO_KHR;
      }
    }
    break;

    case GPUVSyncMode::FIFO:
    {
      // FIFO is always available.
      return VK_PRESENT_MODE_FIFO_KHR;
    }
    break;

    case GPUVSyncMode::Mailbox:
    {
      // Mailbox > fifo.
      if (CheckForMode(VK_PRESENT_MODE_MAILBOX_KHR))
      {
        return VK_PRESENT_MODE_MAILBOX_KHR;
      }
      else
      {
        WARNING_LOG("Mailbox not supported for vsync-mailbox, using FIFO.");
        vsync_mode = GPUVSyncMode::FIFO;
        return VK_PRESENT_MODE_FIFO_KHR;
      }
    }
    break;

      DefaultCaseIsUnreachable()
  }
}

bool VulkanSwapChain::CreateSwapChain(VulkanDevice& dev, Error* error)
{
  const VkPhysicalDevice physdev = dev.GetVulkanPhysicalDevice();

  // Select swap chain format
  std::optional<VkSurfaceFormatKHR> surface_format = SelectSurfaceFormat(physdev, error);
  if (!surface_format.has_value())
    return false;

  const std::optional<VkPresentModeKHR> present_mode = SelectPresentMode(physdev, m_vsync_mode, error);
  if (!present_mode.has_value())
    return false;

  // Look up surface properties to determine image count and dimensions
  VkSurfaceCapabilities2KHR surface_caps = {
    .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = nullptr, .surfaceCapabilities = {}};
  VkResult res = VK_NOT_READY;

  // The present mode can alter the number of images required. Use VK_KHR_get_surface_capabilities2 to confirm it.
  const VulkanLoader::OptionalExtensions& optional_extensions = VulkanLoader::GetOptionalExtensions();
  if (optional_extensions.vk_khr_get_surface_capabilities2 && optional_extensions.vk_khr_surface_maintenance1)
  {
    VkPhysicalDeviceSurfaceInfo2KHR dsi = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .pNext = nullptr, .surface = m_surface};
    VkSurfacePresentModeKHR dsi_pm = {
      .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT, .pNext = nullptr, .presentMode = present_mode.value()};
    Vulkan::AddPointerToChain(&dsi, &dsi_pm);
    res = vkGetPhysicalDeviceSurfaceCapabilities2KHR(physdev, &dsi, &surface_caps);
    if (res != VK_SUCCESS)
      LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceCapabilities2KHR() failed: ");
  }

  if (res != VK_SUCCESS)
  {
    DEV_LOG("VK_EXT_surface_maintenance1 not supported, image count may be sub-optimal.");

    res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physdev, m_surface, &surface_caps.surfaceCapabilities);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: ", res);
      return false;
    }
  }

  // Select number of images in swap chain, we prefer one buffer in the background to work on in triple-buffered mode.
  // maxImageCount can be zero, in which case there isn't an upper limit on the number of buffers.
  u32 image_count = std::clamp<u32>(
    (present_mode.value() == VK_PRESENT_MODE_MAILBOX_KHR) ? 3 : 2, surface_caps.surfaceCapabilities.minImageCount,
    (surface_caps.surfaceCapabilities.maxImageCount == 0) ? std::numeric_limits<u32>::max() :
                                                            surface_caps.surfaceCapabilities.maxImageCount);
  DEV_LOG("Creating a swap chain with {} images in present mode {}", image_count,
          PresentModeToString(present_mode.value()));

  // Determine the dimensions of the swap chain. Values of -1 indicate the size we specify here
  // determines window size? Android sometimes lags updating currentExtent, so don't use it.
  // We want to avoid the system-level downsampling with fractional scaling on MacOS too.
  VkExtent2D size = surface_caps.surfaceCapabilities.currentExtent;
#if defined(__ANDROID__) && !defined(__APPLE__)
  if (size.width == UINT32_MAX)
#endif
  {
    size.width = m_window_info.surface_width;
    size.height = m_window_info.surface_height;
  }
  size.width = std::clamp(size.width, surface_caps.surfaceCapabilities.minImageExtent.width,
                          surface_caps.surfaceCapabilities.maxImageExtent.width);
  size.height = std::clamp(size.height, surface_caps.surfaceCapabilities.minImageExtent.height,
                           surface_caps.surfaceCapabilities.maxImageExtent.height);

  // Prefer identity transform if possible
  VkExtent2D window_size = size;
  WindowInfo::PreRotation window_prerotation = WindowInfo::PreRotation::Identity;
  VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  switch (surface_caps.surfaceCapabilities.currentTransform)
  {
    case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
      break;

    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
      transform = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
      window_prerotation = WindowInfo::PreRotation::Rotate90Clockwise;
      std::swap(size.width, size.height);
      DEV_LOG("Using VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR pretransform.");
      break;

    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
      transform = VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
      window_prerotation = WindowInfo::PreRotation::Rotate180Clockwise;
      DEV_LOG("Using VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR pretransform.");
      break;

    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
      transform = VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
      window_prerotation = WindowInfo::PreRotation::Rotate270Clockwise;
      std::swap(size.width, size.height);
      DEV_LOG("Using VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR pretransform.");
      break;

    default:
    {
      if (!(surface_caps.surfaceCapabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
      {
        WARNING_LOG("Unhandled surface transform 0x{:X}, identity unsupported.",
                    static_cast<u32>(surface_caps.surfaceCapabilities.supportedTransforms));
        transform = surface_caps.surfaceCapabilities.currentTransform;
      }
      else
      {
        WARNING_LOG("Unhandled surface transform 0x{:X}",
                    static_cast<u32>(surface_caps.surfaceCapabilities.supportedTransforms));
      }
    }
    break;
  }

  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(surface_caps.surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
  {
    // If we only support pre-multiplied/post-multiplied... :/
    if (surface_caps.surfaceCapabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
      alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  }

  // Select swap chain flags, we only need a colour attachment
  VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if ((surface_caps.surfaceCapabilities.supportedUsageFlags & image_usage) != image_usage)
  {
    Error::SetStringView(error, "Swap chain does not support usage as color attachment");
    return false;
  }

  // Store the old/current swap chain when recreating for resize
  // Old swap chain is destroyed regardless of whether the create call succeeds
  VkSwapchainKHR old_swap_chain = m_swap_chain;
  m_swap_chain = VK_NULL_HANDLE;

  // Now we can actually create the swap chain
  VkSwapchainCreateInfoKHR swap_chain_info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                              .pNext = nullptr,
                                              .flags = 0,
                                              .surface = m_surface,
                                              .minImageCount = image_count,
                                              .imageFormat = surface_format->format,
                                              .imageColorSpace = surface_format->colorSpace,
                                              .imageExtent = size,
                                              .imageArrayLayers = 1u,
                                              .imageUsage = image_usage,
                                              .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                              .queueFamilyIndexCount = 0,
                                              .pQueueFamilyIndices = nullptr,
                                              .preTransform = transform,
                                              .compositeAlpha = alpha,
                                              .presentMode = present_mode.value(),
                                              .clipped = VK_TRUE,
                                              .oldSwapchain = old_swap_chain};
  const std::array<u32, 2> queue_indices = {{
    dev.GetGraphicsQueueFamilyIndex(),
    dev.GetPresentQueueFamilyIndex(),
  }};
  if (dev.GetGraphicsQueueFamilyIndex() != dev.GetPresentQueueFamilyIndex())
  {
    swap_chain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    swap_chain_info.queueFamilyIndexCount = 2;
    swap_chain_info.pQueueFamilyIndices = queue_indices.data();
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
        ERROR_LOG("MonitorFromWindow() for exclusive fullscreen exclusive override failed.");

      Vulkan::AddPointerToChain(&swap_chain_info, &exclusive_info);
      Vulkan::AddPointerToChain(&swap_chain_info, &exclusive_win32_info);
    }
    else
    {
      ERROR_LOG("Exclusive fullscreen control requested, but VK_EXT_full_screen_exclusive is not supported.");
    }
  }
#else
  if (m_exclusive_fullscreen_control.has_value())
    ERROR_LOG("Exclusive fullscreen control requested, but is not supported on this platform.");
#endif

  const VkDevice vkdev = dev.GetVulkanDevice();
  res = vkCreateSwapchainKHR(vkdev, &swap_chain_info, nullptr, &m_swap_chain);
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkCreateSwapchainKHR failed: ", res);
    return false;
  }

  // Now destroy the old swap chain, since it's been recreated.
  // We can do this immediately since all work should have been completed before calling resize.
  if (old_swap_chain != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(vkdev, old_swap_chain, nullptr);

  if (window_size.width > std::numeric_limits<u16>::max() || window_size.height > std::numeric_limits<u16>::max())
  {
    Error::SetStringFmt(error, "Invalid swap chain dimensions: {}x{}", window_size.width, window_size.height);
    return false;
  }

  m_present_mode = present_mode.value();
  m_window_info.surface_width = static_cast<u16>(window_size.width);
  m_window_info.surface_height = static_cast<u16>(window_size.height);
  m_window_info.surface_format = VulkanDevice::GetFormatForVkFormat(surface_format->format);
  m_window_info.surface_prerotation = window_prerotation;
  if (m_window_info.surface_format == GPUTextureFormat::Unknown)
  {
    Error::SetStringFmt(error, "Unknown surface format {}", static_cast<u32>(surface_format->format));
    return false;
  }

  return true;
}

bool VulkanSwapChain::CreateSwapChainImages(VulkanDevice& dev, Error* error)
{
  const VkDevice vkdev = dev.GetVulkanDevice();

  // Get and create images.
  Assert(m_images.empty());

  u32 image_count;
  VkResult res = vkGetSwapchainImagesKHR(vkdev, m_swap_chain, &image_count, nullptr);
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkGetSwapchainImagesKHR failed: ", res);
    return false;
  }

  std::vector<VkImage> images(image_count);
  res = vkGetSwapchainImagesKHR(vkdev, m_swap_chain, &image_count, images.data());
  Assert(res == VK_SUCCESS);

  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!dev.GetOptionalExtensions().vk_khr_dynamic_rendering)
  {
    render_pass = dev.GetSwapChainRenderPass(m_window_info.surface_format, VK_ATTACHMENT_LOAD_OP_CLEAR);
    if (render_pass == VK_NULL_HANDLE)
    {
      Error::SetStringFmt(error, "Failed to get render pass for format {}",
                          GPUTexture::GetFormatName(m_window_info.surface_format));
      return false;
    }
  }

  const VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};

  const u32 fb_width = GetPostRotatedWidth();
  const u32 fb_height = GetPostRotatedHeight();
  m_images.reserve(image_count);
  m_current_image = 0;
  m_current_image_acquire_semaphore = (NUM_IMAGE_ACQUIRE_SEMAPHORES - 1);
  for (u32 i = 0; i < image_count; i++)
  {
    Image& image = m_images.emplace_back();
    image.image = images[i];
    image.framebuffer = VK_NULL_HANDLE;
    image.present_semaphore = VK_NULL_HANDLE;

    const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .image = images[i],
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VulkanDevice::TEXTURE_FORMAT_MAPPING[static_cast<u8>(m_window_info.surface_format)],
      .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY},
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u},
    };
    if ((res = vkCreateImageView(vkdev, &view_info, nullptr, &image.view)) != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateImageView() failed: ", res);
      m_images.pop_back();
      return false;
    }

    if (!dev.GetOptionalExtensions().vk_khr_dynamic_rendering)
    {
      Vulkan::FramebufferBuilder fbb;
      fbb.AddAttachment(image.view);
      fbb.SetRenderPass(render_pass);
      fbb.SetSize(fb_width, fb_height, 1);
      if ((image.framebuffer = fbb.Create(vkdev)) == VK_NULL_HANDLE)
      {
        Error::SetStringView(error, "Failed to create swap chain image framebuffer.");
        vkDestroyImageView(vkdev, image.view, nullptr);
        m_images.pop_back();
        return false;
      }
    }

    res = vkCreateSemaphore(vkdev, &semaphore_info, nullptr, &image.present_semaphore);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateSemaphore failed: ", res);
      return false;
    }
  }

  for (u32 i = 0; i < NUM_IMAGE_ACQUIRE_SEMAPHORES; i++)
  {
    res = vkCreateSemaphore(vkdev, &semaphore_info, nullptr, &m_image_acquire_semaphores[i]);
    if (res != VK_SUCCESS)
    {
      Vulkan::SetErrorObject(error, "vkCreateSemaphore failed: ", res);
      return false;
    }
  }

  return true;
}

void VulkanSwapChain::Destroy(VulkanDevice& dev, bool wait_for_idle)
{
  if (!m_swap_chain && !m_surface)
    return;

  if (wait_for_idle)
  {
    if (dev.InRenderPass())
      dev.EndRenderPass();

    dev.WaitForGPUIdle();
  }

  DestroySwapChain();
  DestroySurface();
}

void VulkanSwapChain::DestroySwapChainImages()
{
  const VkDevice vkdev = VulkanDevice::GetInstance().GetVulkanDevice();
  for (const auto& it : m_images)
  {
    // don't defer view destruction, images are no longer valid
    if (it.present_semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(vkdev, it.present_semaphore, nullptr);
    if (it.framebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(vkdev, it.framebuffer, nullptr);
    vkDestroyImageView(vkdev, it.view, nullptr);
  }
  m_images.clear();

  for (auto& it : m_image_acquire_semaphores)
  {
    if (it != VK_NULL_HANDLE)
    {
      vkDestroySemaphore(vkdev, it, nullptr);
      it = VK_NULL_HANDLE;
    }
  }

  m_image_acquire_result.reset();
}

void VulkanSwapChain::DestroySwapChain()
{
  DestroySwapChainImages();

  if (m_swap_chain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(VulkanDevice::GetInstance().GetVulkanDevice(), m_swap_chain, nullptr);
    m_swap_chain = VK_NULL_HANDLE;
  }
}

VkResult VulkanSwapChain::AcquireNextImage(bool handle_errors)
{
  if (m_image_acquire_result.has_value())
  {
    if (m_image_acquire_result.value() == VK_SUCCESS || !handle_errors ||
        !HandleAcquireOrPresentError(m_image_acquire_result.value(), false))
    {
      return m_image_acquire_result.value();
    }
  }

  if (!m_swap_chain)
    return VK_ERROR_SURFACE_LOST_KHR;

  // Use a different semaphore for each image.
  m_current_image_acquire_semaphore = (m_current_image_acquire_semaphore + 1) % NUM_IMAGE_ACQUIRE_SEMAPHORES;

  VkResult res = vkAcquireNextImageKHR(VulkanDevice::GetInstance().GetVulkanDevice(), m_swap_chain, UINT64_MAX,
                                       GetImageAcquireSemaphore(), VK_NULL_HANDLE, &m_current_image);
  if (res != VK_SUCCESS && HandleAcquireOrPresentError(res, false))
  {
    res = vkAcquireNextImageKHR(VulkanDevice::GetInstance().GetVulkanDevice(), m_swap_chain, UINT64_MAX,
                                GetImageAcquireSemaphore(), VK_NULL_HANDLE, &m_current_image);
  }

  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkAcquireNextImageKHR() failed: ");

  m_image_acquire_result = res;
  return res;
}

bool VulkanSwapChain::HandleAcquireOrPresentError(VkResult& res, bool is_present_error)
{
  if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
  {
    VulkanDevice& dev = VulkanDevice::GetInstance();
    if (is_present_error)
    {
      // Older NVIDIA drivers completely lock up if there is no device idle wait prior to waiting of the command
      // buffer's fences. I'm guessing it's something due to the failed present, but regardless, it shouldn't hurt
      // anything doing this here. But don't remove it for this reason.
      vkDeviceWaitIdle(dev.GetVulkanDevice());
      dev.WaitForAllFences();
    }
    else
    {
      dev.SubmitCommandBuffer(true);
    }

    Error error;
    if (!RecreateSwapChain(dev, &error))
    {
      DestroySwapChain();
      ERROR_LOG("Failed to recreate suboptimal swapchain: {}", error.GetDescription());
      res = VK_ERROR_SURFACE_LOST_KHR;
      return false;
    }

    return true;
  }
  else if (res == VK_ERROR_SURFACE_LOST_KHR)
  {
    VulkanDevice& dev = VulkanDevice::GetInstance();
    if (is_present_error)
    {
      // See above.
      vkDeviceWaitIdle(dev.GetVulkanDevice());
      dev.WaitForAllFences();
    }
    else
    {
      dev.SubmitCommandBuffer(true);
    }

    Error error;
    if (!RecreateSurface(dev, &error))
    {
      DestroySwapChain();
      ERROR_LOG("Failed to recreate surface: {}", error.GetDescription());
      res = VK_ERROR_SURFACE_LOST_KHR;
      return false;
    }

    return true;
  }

  return false;
}

void VulkanSwapChain::ReleaseCurrentImage()
{
  if (!m_image_acquire_result.has_value())
    return;

  if ((m_image_acquire_result.value() == VK_SUCCESS || m_image_acquire_result.value() == VK_SUBOPTIMAL_KHR) &&
      VulkanDevice::GetInstance().GetOptionalExtensions().vk_khr_swapchain_maintenance1)
  {
    VulkanDevice::GetInstance().WaitForGPUIdle();

    const VkReleaseSwapchainImagesInfoKHR info = {.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,
                                                  .pNext = nullptr,
                                                  .swapchain = m_swap_chain,
                                                  .imageIndexCount = 1,
                                                  .pImageIndices = &m_current_image};
    VkResult res = vkReleaseSwapchainImagesKHR(VulkanDevice::GetInstance().GetVulkanDevice(), &info);
    if (res != VK_SUCCESS)
      LOG_VULKAN_ERROR(res, "vkReleaseSwapchainImagesKHR() failed: ");
  }

  m_image_acquire_result.reset();
}

void VulkanSwapChain::ResetImageAcquireResult()
{
  m_image_acquire_result.reset();
}

bool VulkanSwapChain::ResizeBuffers(u32 new_width, u32 new_height, Error* error)
{
  if (m_window_info.surface_width == new_width && m_window_info.surface_height == new_height)
    return true;

  VulkanDevice& dev = VulkanDevice::GetInstance();
  if (dev.InRenderPass())
    dev.EndRenderPass();
  dev.SubmitCommandBuffer(true);

  if (new_width != 0 && new_height != 0)
  {
    m_window_info.surface_width = static_cast<u16>(new_width);
    m_window_info.surface_height = static_cast<u16>(new_height);
  }

  return RecreateSwapChain(dev, error);
}

bool VulkanSwapChain::RecreateSurface(VulkanDevice& dev, Error* error)
{
  // Destroy the old swap chain, images, and surface.
  DestroySwapChain();
  DestroySurface();

  // Re-create the surface with the new native handle
  if (!CreateSurface(dev.GetVulkanPhysicalDevice(), error))
    return false;

  // The validation layers get angry at us if we don't call this before creating the swapchain.
  VkBool32 present_supported = VK_TRUE;
  VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(dev.GetVulkanPhysicalDevice(), dev.GetPresentQueueFamilyIndex(),
                                                      m_surface, &present_supported);
  if (res != VK_SUCCESS)
  {
    Vulkan::SetErrorObject(error, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ", res);
    return false;
  }
  AssertMsg(present_supported, "Recreated surface does not support presenting.");

  // Finally re-create the swap chain
  if (!CreateSwapChain(dev, error) || !CreateSwapChainImages(dev, error))
  {
    DestroySwapChain();
    return false;
  }

  return true;
}

bool VulkanSwapChain::RecreateSwapChain(VulkanDevice& dev, Error* error)
{
  ReleaseCurrentImage();
  DestroySwapChainImages();

  if (!CreateSwapChain(dev, error) || !CreateSwapChainImages(dev, error))
  {
    DestroySwapChain();
    return false;
  }

  return true;
}

bool VulkanSwapChain::SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle, Error* error)
{
  m_allow_present_throttle = allow_present_throttle;

  VulkanDevice& dev = VulkanDevice::GetInstance();
  const std::optional<VkPresentModeKHR> new_present_mode =
    SelectPresentMode(dev.GetVulkanPhysicalDevice(), mode, error);
  if (!new_present_mode.has_value())
    return false;

  // High-level mode could change without the actual backend mode changing.
  m_vsync_mode = mode;
  if (m_present_mode == new_present_mode.value())
    return true;

  if (dev.InRenderPass())
    dev.EndRenderPass();
  dev.SubmitCommandBuffer(true);

  // TODO: Use the maintenance extension to change it without recreating...
  // Recreate the swap chain with the new present mode.
  VERBOSE_LOG("Recreating swap chain to change present mode.");
  ReleaseCurrentImage();
  DestroySwapChainImages();
  if (!CreateSwapChain(dev, error) || !CreateSwapChainImages(dev, error))
  {
    DestroySwapChain();
    return false;
  }

  return true;
}
