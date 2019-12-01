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

  std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                    bool dynamic) override;
  void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                     u32 data_stride) override;

  void SetDisplayTexture(void* texture, s32 offset_x, s32 offset_y, s32 width, s32 height, u32 texture_width,
                         u32 texture_height, float aspect_ratio) override;
  void SetDisplayLinearFiltering(bool enabled) override;
  void SetDisplayTopMargin(int height) override;

  void SetVSync(bool enabled) override;

  void Render() override;

  std::tuple<u32, u32> GetWindowSize() const override;
  void WindowResized() override;

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
  GLuint m_display_texture_id = 0;
  s32 m_display_offset_x = 0;
  s32 m_display_offset_y = 0;
  s32 m_display_width = 0;
  s32 m_display_height = 0;
  u32 m_display_texture_width = 0;
  u32 m_display_texture_height = 0;
  int m_display_top_margin = 0;
  float m_display_aspect_ratio = 1.0f;

  bool m_display_texture_changed = false;
  bool m_display_linear_filtering = false;
};
