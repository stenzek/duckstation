#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include <EGL/egl.h>
#include <android/native_window.h>
#include <glad.h>
#include <memory>
#include <string>

class AndroidGLESHostDisplay final : public HostDisplay
{
public:
  AndroidGLESHostDisplay(ANativeWindow* window);
  ~AndroidGLESHostDisplay();

  static std::unique_ptr<HostDisplay> Create(ANativeWindow* window);

  RenderAPI GetRenderAPI() const override;
  void* GetRenderDevice() const override;
  void* GetRenderContext() const override;
  void* GetRenderWindow() const override;

  void ChangeRenderWindow(void* new_window) override;
  void WindowResized(s32 new_window_width, s32 new_window_height) override;

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

  bool CreateSurface();
  void DestroySurface();

  bool CreateGLContext();
  bool CreateImGuiContext();
  bool CreateGLResources();

  void RenderDisplay();

  ANativeWindow* m_window = nullptr;
  int m_window_width = 0;
  int m_window_height = 0;

  EGLDisplay m_egl_display = EGL_NO_DISPLAY;
  EGLSurface m_egl_surface = EGL_NO_SURFACE;
  EGLContext m_egl_context = EGL_NO_CONTEXT;
  EGLConfig m_egl_config = {};

  GL::Program m_display_program;
};
