#pragma once
#include "common/window_info.h"
#include "core/host_display.h"
#include "qtdisplaywidget.h"
#include "qthostdisplay.h"
#include "frontend-common/vulkan_host_display.h"
#include <memory>

class QtHostInterface;

class VulkanHostDisplay final : public QtHostDisplay
{
public:
  VulkanHostDisplay(QtHostInterface* host_interface);
  ~VulkanHostDisplay();

  bool hasDeviceContext() const override;
  bool createDeviceContext(bool debug_device) override;
  bool initializeDeviceContext(std::string_view shader_cache_directory, bool debug_device) override;
  bool activateDeviceContext() override;
  void deactivateDeviceContext() override;
  void destroyDeviceContext() override;
  bool recreateSurface() override;
  void destroySurface();

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;
  void WindowResized(s32 new_window_width, s32 new_window_height) override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* initial_data,
                                                    u32 initial_data_stride, bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* texture_data,
                     u32 texture_data_stride) override;
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  void SetVSync(bool enabled) override;

  void Render() override;

private:
  bool createImGuiContext() override;
  void destroyImGuiContext() override;
  bool createDeviceResources() override;
  void destroyDeviceResources() override;

  FrontendCommon::VulkanHostDisplay m_vulkan_display;
};
