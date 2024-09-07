// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "vulkan_loader.h"
#include "vulkan_texture.h"
#include "window_info.h"

#include "common/types.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

class VulkanSwapChain
{
public:
  // We don't actually need +1 semaphores, or, more than one really.
  // But, the validation layer gets cranky if we don't fence wait before the next image acquire.
  // So, add an additional semaphore to ensure that we're never acquiring before fence waiting.
  static constexpr u32 NUM_SEMAPHORES = 4; // Should be command buffers + 1

  ~VulkanSwapChain();

  // Creates a vulkan-renderable surface for the specified window handle.
  static VkSurfaceKHR CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device, WindowInfo* wi);

  // Destroys a previously-created surface.
  static void DestroyVulkanSurface(VkInstance instance, WindowInfo* wi, VkSurfaceKHR surface);

  // Create a new swap chain from a pre-existing surface.
  static std::unique_ptr<VulkanSwapChain> Create(const WindowInfo& wi, VkSurfaceKHR surface,
                                                 VkPresentModeKHR present_mode,
                                                 std::optional<bool> exclusive_fullscreen_control);

  // Determines present mode to use.
  static bool SelectPresentMode(VkSurfaceKHR surface, GPUVSyncMode* vsync_mode, VkPresentModeKHR* present_mode);

  ALWAYS_INLINE VkSurfaceKHR GetSurface() const { return m_surface; }
  ALWAYS_INLINE VkSwapchainKHR GetSwapChain() const { return m_swap_chain; }
  ALWAYS_INLINE const VkSwapchainKHR* GetSwapChainPtr() const { return &m_swap_chain; }
  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_window_info; }
  ALWAYS_INLINE u32 GetWidth() const { return m_window_info.surface_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_window_info.surface_height; }
  ALWAYS_INLINE float GetScale() const { return m_window_info.surface_scale; }
  ALWAYS_INLINE u32 GetCurrentImageIndex() const { return m_current_image; }
  ALWAYS_INLINE const u32* GetCurrentImageIndexPtr() const { return &m_current_image; }
  ALWAYS_INLINE u32 GetImageCount() const { return static_cast<u32>(m_images.size()); }
  ALWAYS_INLINE VkFormat GetImageFormat() const { return m_format; }
  ALWAYS_INLINE VkImage GetCurrentImage() const { return m_images[m_current_image].image; }
  ALWAYS_INLINE VkImageView GetCurrentImageView() const { return m_images[m_current_image].view; }
  ALWAYS_INLINE VkFramebuffer GetCurrentFramebuffer() const { return m_images[m_current_image].framebuffer; }
  ALWAYS_INLINE VkSemaphore GetImageAvailableSemaphore() const
  {
    return m_semaphores[m_current_semaphore].available_semaphore;
  }
  ALWAYS_INLINE const VkSemaphore* GetImageAvailableSemaphorePtr() const
  {
    return &m_semaphores[m_current_semaphore].available_semaphore;
  }
  ALWAYS_INLINE VkSemaphore GetRenderingFinishedSemaphore() const
  {
    return m_semaphores[m_current_semaphore].rendering_finished_semaphore;
  }
  ALWAYS_INLINE const VkSemaphore* GetRenderingFinishedSemaphorePtr() const
  {
    return &m_semaphores[m_current_semaphore].rendering_finished_semaphore;
  }

  VkResult AcquireNextImage();
  void ReleaseCurrentImage();
  void ResetImageAcquireResult();

  bool RecreateSurface(const WindowInfo& new_wi);
  bool ResizeSwapChain(u32 new_width = 0, u32 new_height = 0, float new_scale = 1.0f);

  // Change vsync enabled state. This may fail as it causes a swapchain recreation.
  bool SetPresentMode(VkPresentModeKHR present_mode);

private:
  VulkanSwapChain(const WindowInfo& wi, VkSurfaceKHR surface, VkPresentModeKHR present_mode,
                  std::optional<bool> exclusive_fullscreen_control);

  static std::optional<VkSurfaceFormatKHR> SelectSurfaceFormat(VkSurfaceKHR surface);

  bool CreateSwapChain();
  void DestroySwapChain();

  void DestroySwapChainImages();

  void DestroySurface();

  struct Image
  {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
  };

  struct ImageSemaphores
  {
    VkSemaphore available_semaphore;
    VkSemaphore rendering_finished_semaphore;
  };

  WindowInfo m_window_info;

  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkSwapchainKHR m_swap_chain = VK_NULL_HANDLE;

  std::vector<Image> m_images;
  std::array<ImageSemaphores, NUM_SEMAPHORES> m_semaphores = {};

  u32 m_current_image = 0;
  u32 m_current_semaphore = 0;

  VkFormat m_format = VK_FORMAT_UNDEFINED;
  VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;

  std::optional<VkResult> m_image_acquire_result;
  std::optional<bool> m_exclusive_fullscreen_control;
};
