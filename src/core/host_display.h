#pragma once
#include "types.h"
#include <memory>
#include <tuple>

// An abstracted RGBA8 texture.
class HostDisplayTexture
{
public:
  virtual ~HostDisplayTexture() {}

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

  virtual ~HostDisplay() {}

  virtual RenderAPI GetRenderAPI() const = 0;
  virtual void* GetHostRenderDevice() const = 0;
  virtual void* GetHostRenderContext() const = 0;

  /// Creates an abstracted RGBA8 texture. If dynamic, the texture can be updated with UpdateTexture() below.
  virtual std::unique_ptr<HostDisplayTexture> CreateTexture(u32 width, u32 height, const void* data, u32 data_stride,
                                                            bool dynamic = false) = 0;
  virtual void UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                             u32 data_stride) = 0;

  virtual void SetDisplayTexture(void* texture_handle, s32 offset_x, s32 offset_y, s32 width, s32 height,
                                 u32 texture_width, u32 texture_height, float aspect_ratio) = 0;
  virtual void SetDisplayLinearFiltering(bool enabled) = 0;
  virtual void SetDisplayTopMargin(int height) = 0;

  virtual void Render() = 0;

  virtual void SetVSync(bool enabled) = 0;

  virtual std::tuple<u32, u32> GetWindowSize() const = 0;
  virtual void WindowResized() = 0;

  // Helper function for computing the draw rectangle in a larger window.
  static std::tuple<int, int, int, int> CalculateDrawRect(int window_width, int window_height, float display_ratio)
  {
    const float window_ratio = float(window_width) / float(window_height);
    int left, top, width, height;
    if (window_ratio >= display_ratio)
    {
      width = static_cast<int>(float(window_height) * display_ratio);
      height = static_cast<int>(window_height);
      left = (window_width - width) / 2;
      top = 0;
    }
    else
    {
      width = static_cast<int>(window_width);
      height = static_cast<int>(float(window_width) / display_ratio);
      left = 0;
      top = (window_height - height) / 2;
    }

    return std::tie(left, top, width, height);
  }
};
