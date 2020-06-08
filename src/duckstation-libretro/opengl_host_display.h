#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "libretro.h"
#include <string>
#include <memory>

class OpenGLHostDisplay final : public HostDisplay
{
public:
  OpenGLHostDisplay(bool is_gles);
  ~OpenGLHostDisplay();

  static bool RequestHardwareRendererContext(retro_hw_render_callback* cb);

  static std::unique_ptr<HostDisplay> Create(bool debug_device);

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;
  bool DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;

  void SetVSync(bool enabled) override;

  void Render() override;

private:
  const char* GetGLSLVersionString() const;
  std::string GetGLSLVersionHeader() const;

  bool GetGLContext(bool debug_device);
  bool CreateGLResources();

  void RenderDisplay();

  GL::Program m_display_program;
  GLuint m_display_vao = 0;
  GLuint m_display_nearest_sampler = 0;
  GLuint m_display_linear_sampler = 0;

  s32 m_last_display_width = -1;
  s32 m_last_display_height = -1;

  retro_hw_render_callback m_render_callback = {};
  bool m_is_gles = false;
};
