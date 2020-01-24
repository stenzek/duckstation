#pragma once
#include "types.h"
#include <memory>
#include <tuple>

// An abstracted RGBA8 texture.
class HostDisplayTexture
{
public:
  virtual ~HostDisplayTexture();

  virtual void* GetHandle() const = 0;
  virtual u32 GetWidth() const = 0;
  virtual u32 GetHeight() const = 0;
};

// Interface to the frontend's renderer.
class HostDisplay
{
public:
  enum class RenderAPI
  {
    None,
    D3D11,
    OpenGL,
    OpenGLES
  };

  virtual ~HostDisplay();

  virtual RenderAPI GetRenderAPI() const = 0;
  virtual void* GetRenderDevice() const = 0;
  virtual void* GetRenderContext() const = 0;
  virtual void* GetRenderWindow() const = 0;

  /// Switches the render window, recreating the surface.
  virtual void ChangeRenderWindow(void* new_window) = 0;

  /// Creates an abstracted RGBA8 texture. If dynamic, the texture can be updated with UpdateTexture() below.
  virtual std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                            bool dynamic = false) = 0;
  virtual void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                             u32 data_stride) = 0;

  virtual void Render() = 0;

  virtual void SetVSync(bool enabled) = 0;

  virtual std::tuple<u32, u32> GetWindowSize() const = 0;
  virtual void WindowResized() = 0;

  const s32 GetDisplayTopMargin() const { return m_display_top_margin; }

  void SetDisplayTexture(void* texture_handle, s32 offset_x, s32 offset_y, s32 width, s32 height, u32 texture_width,
                         u32 texture_height, float aspect_ratio)
  {
    m_display_texture_handle = texture_handle;
    m_display_offset_x = offset_x;
    m_display_offset_y = offset_y;
    m_display_width = width;
    m_display_height = height;
    m_display_texture_width = texture_width;
    m_display_texture_height = texture_height;
    m_display_aspect_ratio = aspect_ratio;
    m_display_texture_changed = true;
  }

  void SetDisplayLinearFiltering(bool enabled) { m_display_linear_filtering = enabled; }
  void SetDisplayTopMargin(s32 height) { m_display_top_margin = height; }

  // Helper function for computing the draw rectangle in a larger window.
  static std::tuple<int, int, int, int> CalculateDrawRect(int window_width, int window_height, float display_ratio);

protected:
  void* m_display_texture_handle = nullptr;
  s32 m_display_offset_x = 0;
  s32 m_display_offset_y = 0;
  s32 m_display_width = 0;
  s32 m_display_height = 0;
  u32 m_display_texture_width = 0;
  u32 m_display_texture_height = 0;
  s32 m_display_top_margin = 0;
  float m_display_aspect_ratio = 1.0f;

  bool m_display_texture_changed = false;
  bool m_display_linear_filtering = false;
};
