#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "frontend-common/opengl_host_display.h"
#include "libretro.h"
#include <memory>
#include <string>

class LibretroOpenGLHostDisplay final : public FrontendCommon::OpenGLHostDisplay
{
public:
  LibretroOpenGLHostDisplay();
  ~LibretroOpenGLHostDisplay();

  static bool RequestHardwareRendererContext(retro_hw_render_callback* cb, bool prefer_gles);

  RenderAPI GetRenderAPI() const override;

  bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                          bool threaded_presentation) override;
  void DestroyRenderDevice() override;

  void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) override;
  bool ChangeRenderWindow(const WindowInfo& new_wi) override;

  void SetVSync(bool enabled) override;

  bool Render() override;

private:
  bool m_is_gles = false;
};
