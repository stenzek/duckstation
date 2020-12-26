#pragma once
#include "common/d3d11/texture.h"
#include "frontend-common/d3d11_host_display.h"
#include "libretro.h"

class LibretroD3D11HostDisplay final : public FrontendCommon::D3D11HostDisplay
{
public:
  LibretroD3D11HostDisplay();
  ~LibretroD3D11HostDisplay();

  static bool RequestHardwareRendererContext(retro_hw_render_callback* cb);

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                          bool threaded_presentation) override;

  void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  bool ChangeRenderWindow(const WindowInfo& new_wi) override;

  void SetVSync(bool enabled) override;

  bool Render() override;

protected:
  void DestroyResources() override;

private:
  bool CheckFramebufferSize(u32 width, u32 height);

  D3D11::Texture m_framebuffer;
};
