#pragma once
#include "common/vulkan/texture.h"
#include "frontend-common/vulkan_host_display.h"
#include "libretro.h"

#define HAVE_VULKAN
#include "libretro_vulkan.h"

class LibretroVulkanHostDisplay final : public FrontendCommon::VulkanHostDisplay
{
public:
  LibretroVulkanHostDisplay();
  ~LibretroVulkanHostDisplay();

  static bool RequestHardwareRendererContext(retro_hw_render_callback* cb);

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                          bool threaded_presentation) override;
  void DestroyRenderDevice() override;

  void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  bool ChangeRenderWindow(const WindowInfo& new_wi) override;

  void SetVSync(bool enabled) override;

  bool Render() override;

protected:
  bool CreateResources() override;
  void DestroyResources() override;
  VkRenderPass GetRenderPassForDisplay() const override;

private:
  static constexpr VkFormat FRAMEBUFFER_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

  bool CheckFramebufferSize(u32 width, u32 height);

  retro_hw_render_interface_vulkan* m_ri = nullptr;

  Vulkan::Texture m_frame_texture;
  retro_vulkan_image m_frame_view = {};
  VkFramebuffer m_frame_framebuffer = VK_NULL_HANDLE;
  VkRenderPass m_frame_render_pass = VK_NULL_HANDLE;
};
