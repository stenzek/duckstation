// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"
#include "vulkan_loader.h"
#include "vulkan_texture.h"
#include "window_info.h"

#include "common/types.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

class VulkanSwapChain final : public GPUSwapChain
{
public:
  VulkanSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode, bool allow_present_throttle,
                  std::optional<bool> exclusive_fullscreen_control);
  ~VulkanSwapChain() override;

  ALWAYS_INLINE VkSurfaceKHR GetSurface() const { return m_surface; }
  ALWAYS_INLINE VkSwapchainKHR GetSwapChain() const { return m_swap_chain; }
  ALWAYS_INLINE const VkSwapchainKHR* GetSwapChainPtr() const { return &m_swap_chain; }
  ALWAYS_INLINE u32 GetCurrentImageIndex() const { return m_current_image; }
  ALWAYS_INLINE const u32* GetCurrentImageIndexPtr() const { return &m_current_image; }
  ALWAYS_INLINE u32 GetImageCount() const { return static_cast<u32>(m_images.size()); }
  ALWAYS_INLINE VkImage GetCurrentImage() const { return m_images[m_current_image].image; }
  ALWAYS_INLINE VkImageView GetCurrentImageView() const { return m_images[m_current_image].view; }
  ALWAYS_INLINE VkFramebuffer GetCurrentFramebuffer() const { return m_images[m_current_image].framebuffer; }
  ALWAYS_INLINE VkSemaphore GetImageAcquireSemaphore() const
  {
    return m_image_acquire_semaphores[m_current_image_acquire_semaphore];
  }
  ALWAYS_INLINE const VkSemaphore* GetImageAcquireSemaphorePtr() const
  {
    return &m_image_acquire_semaphores[m_current_image_acquire_semaphore];
  }
  ALWAYS_INLINE VkSemaphore GetPresentSemaphore() const { return m_images[m_current_image].present_semaphore; }
  ALWAYS_INLINE const VkSemaphore* GetPresentSemaphorePtr() const
  {
    return &m_images[m_current_image].present_semaphore;
  }

  bool CreateSurface(VkInstance instance, VkPhysicalDevice physical_device, Error* error);
  bool CreateSwapChain(VulkanDevice& dev, Error* error);
  bool CreateSwapChainImages(VulkanDevice& dev, Error* error);
  void Destroy(VulkanDevice& dev, bool wait_for_idle);

  VkResult AcquireNextImage(bool handle_errors);
  void ReleaseCurrentImage();
  void ResetImageAcquireResult();
  bool HandleAcquireOrPresentError(VkResult& res, bool is_present_error);

  bool ResizeBuffers(u32 new_width, u32 new_height, float new_scale, Error* error) override;
  bool SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle, Error* error) override;

private:
  // We don't actually need +1 semaphores, or, more than one really.
  // But, the validation layer gets cranky if we don't fence wait before the next image acquire.
  // So, add an additional semaphore to ensure that we're never acquiring before fence waiting.
  static constexpr u32 NUM_IMAGE_ACQUIRE_SEMAPHORES = 4; // Should be command buffers + 1

  std::optional<VkSurfaceFormatKHR> SelectSurfaceFormat(VkPhysicalDevice physdev, Error* error);
  std::optional<VkPresentModeKHR> SelectPresentMode(VkPhysicalDevice physdev, GPUVSyncMode& vsync_mode, Error* error);

  void DestroySwapChainImages();
  void DestroySwapChain();
  void DestroySurface();

  // Assumes the command buffer has been flushed.
  bool RecreateSurface(VulkanDevice& dev, Error* error);
  bool RecreateSwapChain(VulkanDevice& dev, Error* error);

  struct Image
  {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkSemaphore present_semaphore;
  };

  VkSurfaceKHR m_surface = VK_NULL_HANDLE;

#ifdef __APPLE__
  // On MacOS, we need to store a pointer to the metal layer as well.
  void* m_metal_layer = nullptr;
#endif

  VkSwapchainKHR m_swap_chain = VK_NULL_HANDLE;

  std::vector<Image> m_images;
  std::array<VkSemaphore, NUM_IMAGE_ACQUIRE_SEMAPHORES> m_image_acquire_semaphores = {};

  u32 m_current_image = 0;
  u32 m_current_image_acquire_semaphore = 0;

  VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;

  std::optional<VkResult> m_image_acquire_result;
  std::optional<bool> m_exclusive_fullscreen_control;
};
