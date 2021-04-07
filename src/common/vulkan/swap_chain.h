// Copyright 2016 Dolphin Emulator Project
// Copyright 2020 DuckStation Emulator Project
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include "../types.h"
#include "../window_info.h"
#include "texture.h"
#include "vulkan_loader.h"
#include <memory>
#include <vector>

namespace Vulkan {

class SwapChain
{
public:
  SwapChain(const WindowInfo& wi, VkSurfaceKHR surface, bool vsync);
  ~SwapChain();

  // Creates a vulkan-renderable surface for the specified window handle.
  static VkSurfaceKHR CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device, WindowInfo* wi);

  // Destroys a previously-created surface.
  static void DestroyVulkanSurface(VkInstance instance, WindowInfo* wi, VkSurfaceKHR surface);

  // Enumerates fullscreen modes for window info.
  struct FullscreenModeInfo
  {
    u32 width;
    u32 height;
    float refresh_rate;
  };
  static std::vector<FullscreenModeInfo>
  GetSurfaceFullscreenModes(VkInstance instance, VkPhysicalDevice physical_device, const WindowInfo& wi);

  // Create a new swap chain from a pre-existing surface.
  static std::unique_ptr<SwapChain> Create(const WindowInfo& wi, VkSurfaceKHR surface, bool vsync);

  ALWAYS_INLINE VkSurfaceKHR GetSurface() const { return m_surface; }
  ALWAYS_INLINE VkSurfaceFormatKHR GetSurfaceFormat() const { return m_surface_format; }
  ALWAYS_INLINE VkFormat GetTextureFormat() const { return m_surface_format.format; }
  ALWAYS_INLINE bool IsVSyncEnabled() const { return m_vsync_enabled; }
  ALWAYS_INLINE VkSwapchainKHR GetSwapChain() const { return m_swap_chain; }
  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_window_info; }
  ALWAYS_INLINE u32 GetWidth() const { return m_window_info.surface_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_window_info.surface_height; }
  ALWAYS_INLINE u32 GetCurrentImageIndex() const { return m_current_image; }
  ALWAYS_INLINE u32 GetImageCount() const { return static_cast<u32>(m_images.size()); }
  ALWAYS_INLINE VkImage GetCurrentImage() const { return m_images[m_current_image].image; }
  ALWAYS_INLINE const Texture& GetCurrentTexture() const { return m_images[m_current_image].texture; }
  ALWAYS_INLINE Texture& GetCurrentTexture() { return m_images[m_current_image].texture; }
  ALWAYS_INLINE VkFramebuffer GetCurrentFramebuffer() const { return m_images[m_current_image].framebuffer; }
  ALWAYS_INLINE VkRenderPass GetLoadRenderPass() const { return m_load_render_pass; }
  ALWAYS_INLINE VkRenderPass GetClearRenderPass() const { return m_clear_render_pass; }
  ALWAYS_INLINE VkSemaphore GetImageAvailableSemaphore() const { return m_image_available_semaphore; }
  ALWAYS_INLINE VkSemaphore GetRenderingFinishedSemaphore() const { return m_rendering_finished_semaphore; }
  VkResult AcquireNextImage();

  bool RecreateSurface(const WindowInfo& new_wi);
  bool ResizeSwapChain(u32 new_width = 0, u32 new_height = 0);
  bool RecreateSwapChain();

  // Change vsync enabled state. This may fail as it causes a swapchain recreation.
  bool SetVSync(bool enabled);

private:
  bool SelectSurfaceFormat();
  bool SelectPresentMode();

  bool CreateSwapChain();
  void DestroySwapChain();

  bool SetupSwapChainImages();
  void DestroySwapChainImages();

  void DestroySurface();

  bool CreateSemaphores();
  void DestroySemaphores();

  struct SwapChainImage
  {
    VkImage image;
    Texture texture;
    VkFramebuffer framebuffer;
  };

  WindowInfo m_window_info;

  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkSurfaceFormatKHR m_surface_format = {};
  VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;

  VkRenderPass m_load_render_pass = VK_NULL_HANDLE;
  VkRenderPass m_clear_render_pass = VK_NULL_HANDLE;

  VkSemaphore m_image_available_semaphore = VK_NULL_HANDLE;
  VkSemaphore m_rendering_finished_semaphore = VK_NULL_HANDLE;

  VkSwapchainKHR m_swap_chain = VK_NULL_HANDLE;
  std::vector<SwapChainImage> m_images;
  u32 m_current_image = 0;
  bool m_vsync_enabled = false;
};

} // namespace Vulkan
