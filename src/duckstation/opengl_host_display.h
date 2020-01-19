#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include <SDL.h>
#include <string>
#include <memory>

class OpenGLHostDisplay final : public HostDisplay
{
public:
  OpenGLHostDisplay(SDL_Window* window);
  ~OpenGLHostDisplay();

  static std::unique_ptr<HostDisplay> Create(SDL_Window* window, bool debug_device);

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

  std::tuple<u32, u32> GetWindowSize() const override;
  void WindowResized() override;

private:
  const char* GetGLSLVersionString() const;
  std::string GetGLSLVersionHeader() const;

  bool CreateGLContext(bool debug_device);
  bool CreateImGuiContext();
  bool CreateGLResources();

  void Render();
  void RenderDisplay();

  SDL_Window* m_window = nullptr;
  SDL_GLContext m_gl_context = nullptr;
  int m_window_width = 0;
  int m_window_height = 0;

  GL::Program m_display_program;
  GLuint m_display_vao = 0;
  GLuint m_display_texture_id = 0;
  s32 m_display_offset_x = 0;
  s32 m_display_offset_y = 0;
  s32 m_display_width = 0;
  s32 m_display_height = 0;
  u32 m_display_texture_width = 0;
  u32 m_display_texture_height = 0;
  int m_display_top_margin = 0;
  float m_display_aspect_ratio = 1.0f;
  GLuint m_display_nearest_sampler = 0;
  GLuint m_display_linear_sampler = 0;

  bool m_is_gles = false;
  bool m_display_texture_changed = false;
  bool m_display_linear_filtering = false;
};
