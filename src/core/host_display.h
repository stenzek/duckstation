#pragma once
#include "common/rectangle.h"
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

  /// Call when the window size changes externally to recreate any resources.
  virtual void WindowResized(s32 new_window_width, s32 new_window_height);

  /// Creates an abstracted RGBA8 texture. If dynamic, the texture can be updated with UpdateTexture() below.
  virtual std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                            bool dynamic = false) = 0;
  virtual void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                             u32 data_stride) = 0;

  virtual void Render() = 0;

  virtual void SetVSync(bool enabled) = 0;

  const s32 GetDisplayTopMargin() const { return m_display_top_margin; }

  void ClearDisplayTexture()
  {
    m_display_texture_handle = nullptr;
    m_display_texture_width = 0;
    m_display_texture_height = 0;
    m_display_texture_view_x = 0;
    m_display_texture_view_y = 0;
    m_display_texture_view_width = 0;
    m_display_texture_view_height = 0;
    m_display_changed = true;
  }

  void SetDisplayTexture(void* texture_handle, s32 texture_width, s32 texture_height, s32 view_x, s32 view_y,
                         s32 view_width, s32 view_height)
  {
    m_display_texture_handle = texture_handle;
    m_display_texture_width = texture_width;
    m_display_texture_height = texture_height;
    m_display_texture_view_x = view_x;
    m_display_texture_view_y = view_y;
    m_display_texture_view_width = view_width;
    m_display_texture_view_height = view_height;
    m_display_changed = true;
  }

  void SetDisplayParameters(s32 display_width, s32 display_height, const Common::Rectangle<s32>& display_area,
                            float pixel_aspect_ratio)
  {
    m_display_width = display_width;
    m_display_height = display_height;
    m_display_area = display_area;
    m_display_aspect_ratio = pixel_aspect_ratio;
    m_display_changed = true;
  }

  void SetDisplayLinearFiltering(bool enabled) { m_display_linear_filtering = enabled; }
  void SetDisplayTopMargin(s32 height) { m_display_top_margin = height; }

  // Helper function for computing the draw rectangle in a larger window.
  std::tuple<s32, s32, s32, s32> CalculateDrawRect() const;

protected:
  s32 m_window_width = 0;
  s32 m_window_height = 0;

  s32 m_display_width = 0;
  s32 m_display_height = 0;
  Common::Rectangle<s32> m_display_area{};
  float m_display_aspect_ratio = 1.0f;

  void* m_display_texture_handle = nullptr;
  s32 m_display_texture_width = 0;
  s32 m_display_texture_height = 0;
  s32 m_display_texture_view_x = 0;
  s32 m_display_texture_view_y = 0;
  s32 m_display_texture_view_width = 0;
  s32 m_display_texture_view_height = 0;

  s32 m_display_top_margin = 0;

  bool m_display_linear_filtering = false;
  bool m_display_changed = false;
};
