#pragma once
#include "core/host_display.h"
#include "frontend-common/vulkan_host_display.h"
#include <SDL.h>
#include <string_view>

class SDLVulkanHostDisplay final : public HostDisplay
{
public:
  SDLVulkanHostDisplay(SDL_Window* window);
  ~SDLVulkanHostDisplay();

  static std::unique_ptr<HostDisplay> Create(SDL_Window* window, std::string_view shader_cache_directory,
                                             bool debug_device);

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  void WindowResized(s32 new_window_width, s32 new_window_height) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic = false) override;

  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;

  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  void SetVSync(bool enabled) override;

  void Render() override;

private:
  bool Initialize(std::string_view shader_cache_directory, bool debug_device);

  SDL_Window* m_window = nullptr;
  FrontendCommon::VulkanHostDisplay m_display;
};
