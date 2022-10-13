#pragma once
#include "common/gpu_texture.h"
#include "common/rectangle.h"
#include "common/window_info.h"
#include "types.h"
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

enum class RenderAPI : u32
{
  None,
  D3D11,
  D3D12,
  Vulkan,
  OpenGL,
  OpenGLES
};

// Interface to the frontend's renderer.
class HostDisplay
{
public:
  struct AdapterAndModeList
  {
    std::vector<std::string> adapter_names;
    std::vector<std::string> fullscreen_modes;
  };

  virtual ~HostDisplay();

  /// Returns the default/preferred API for the system.
  static RenderAPI GetPreferredAPI();

  /// Parses a fullscreen mode into its components (width * height @ refresh hz)
  static bool ParseFullscreenMode(const std::string_view& mode, u32* width, u32* height, float* refresh_rate);

  /// Converts a fullscreen mode to a string.
  static std::string GetFullscreenModeString(u32 width, u32 height, float refresh_rate);

  ALWAYS_INLINE const WindowInfo& GetWindowInfo() const { return m_window_info; }
  ALWAYS_INLINE s32 GetWindowWidth() const { return static_cast<s32>(m_window_info.surface_width); }
  ALWAYS_INLINE s32 GetWindowHeight() const { return static_cast<s32>(m_window_info.surface_height); }
  ALWAYS_INLINE float GetWindowScale() const { return m_window_info.surface_scale; }

  // Position is relative to the top-left corner of the window.
  ALWAYS_INLINE s32 GetMousePositionX() const { return m_mouse_position_x; }
  ALWAYS_INLINE s32 GetMousePositionY() const { return m_mouse_position_y; }
  ALWAYS_INLINE void SetMousePosition(s32 x, s32 y)
  {
    m_mouse_position_x = x;
    m_mouse_position_y = y;
  }

  ALWAYS_INLINE const void* GetDisplayTextureHandle() const { return m_display_texture; }
  ALWAYS_INLINE s32 GetDisplayWidth() const { return m_display_width; }
  ALWAYS_INLINE s32 GetDisplayHeight() const { return m_display_height; }
  ALWAYS_INLINE float GetDisplayAspectRatio() const { return m_display_aspect_ratio; }
  ALWAYS_INLINE bool IsGPUTimingEnabled() const { return m_gpu_timing_enabled; }

  virtual RenderAPI GetRenderAPI() const = 0;
  virtual void* GetRenderDevice() const = 0;
  virtual void* GetRenderContext() const = 0;

  virtual bool HasRenderDevice() const = 0;
  virtual bool HasRenderSurface() const = 0;

  virtual bool CreateRenderDevice(const WindowInfo& wi) = 0;
  virtual bool InitializeRenderDevice() = 0;
  virtual bool MakeRenderContextCurrent() = 0;
  virtual bool DoneRenderContextCurrent() = 0;
  virtual void DestroyRenderSurface() = 0;
  virtual bool ChangeRenderWindow(const WindowInfo& wi) = 0;
  virtual bool SupportsFullscreen() const = 0;
  virtual bool IsFullscreen() = 0;
  virtual bool SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate) = 0;
  virtual AdapterAndModeList GetAdapterAndModeList() = 0;
  virtual bool CreateResources() = 0;
  virtual void DestroyResources() = 0;

  virtual bool SetPostProcessingChain(const std::string_view& config) = 0;

  /// Call when the window size changes externally to recreate any resources.
  virtual void ResizeRenderWindow(s32 new_window_width, s32 new_window_height) = 0;

  /// Creates an abstracted RGBA8 texture. If dynamic, the texture can be updated with UpdateTexture() below.
  virtual std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                    GPUTexture::Format format, const void* data, u32 data_stride,
                                                    bool dynamic = false) = 0;
  virtual bool BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer, u32* out_pitch) = 0;
  virtual void EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height) = 0;

  virtual bool UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch);

  virtual bool DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                               u32 out_data_stride) = 0;

  /// Returns false if the window was completely occluded.
  virtual bool Render(bool skip_present) = 0;

  /// Renders the display with postprocessing to the specified image.
  virtual bool RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                GPUTexture::Format* out_format) = 0;

  virtual void SetVSync(bool enabled) = 0;

  /// ImGui context management, usually called by derived classes.
  virtual bool CreateImGuiContext() = 0;
  virtual void DestroyImGuiContext() = 0;
  virtual bool UpdateImGuiFontTexture() = 0;

  bool UsesLowerLeftOrigin() const;
  void SetDisplayMaxFPS(float max_fps);
  bool ShouldSkipDisplayingFrame();

  void ClearDisplayTexture()
  {
    m_display_texture = nullptr;
    m_display_texture_view_x = 0;
    m_display_texture_view_y = 0;
    m_display_texture_view_width = 0;
    m_display_texture_view_height = 0;
    m_display_changed = true;
  }

  void SetDisplayTexture(GPUTexture* texture, s32 view_x, s32 view_y, s32 view_width, s32 view_height)
  {
    m_display_texture = texture;
    m_display_texture_view_x = view_x;
    m_display_texture_view_y = view_y;
    m_display_texture_view_width = view_width;
    m_display_texture_view_height = view_height;
    m_display_changed = true;
  }

  void SetDisplayTextureRect(s32 view_x, s32 view_y, s32 view_width, s32 view_height)
  {
    m_display_texture_view_x = view_x;
    m_display_texture_view_y = view_y;
    m_display_texture_view_width = view_width;
    m_display_texture_view_height = view_height;
    m_display_changed = true;
  }

  void SetDisplayParameters(s32 display_width, s32 display_height, s32 active_left, s32 active_top, s32 active_width,
                            s32 active_height, float display_aspect_ratio)
  {
    m_display_width = display_width;
    m_display_height = display_height;
    m_display_active_left = active_left;
    m_display_active_top = active_top;
    m_display_active_width = active_width;
    m_display_active_height = active_height;
    m_display_aspect_ratio = display_aspect_ratio;
    m_display_changed = true;
  }

  virtual bool SupportsTextureFormat(GPUTexture::Format format) const = 0;

  virtual bool GetHostRefreshRate(float* refresh_rate);

  /// Enables/disables GPU frame timing.
  virtual bool SetGPUTimingEnabled(bool enabled);

  /// Returns the amount of GPU time utilized since the last time this method was called.
  virtual float GetAndResetAccumulatedGPUTime();

  /// Sets the software cursor to the specified texture. Ownership of the texture is transferred.
  void SetSoftwareCursor(std::unique_ptr<GPUTexture> texture, float scale = 1.0f);

  /// Sets the software cursor to the specified image.
  bool SetSoftwareCursor(const void* pixels, u32 width, u32 height, u32 stride, float scale = 1.0f);

  /// Sets the software cursor to the specified path (png image).
  bool SetSoftwareCursor(const char* path, float scale = 1.0f);

  /// Disables the software cursor.
  void ClearSoftwareCursor();

  /// Helper function for computing the draw rectangle in a larger window.
  std::tuple<s32, s32, s32, s32> CalculateDrawRect(s32 window_width, s32 window_height,
                                                   bool apply_aspect_ratio = true) const;

  /// Helper function for converting window coordinates to display coordinates.
  std::tuple<float, float> ConvertWindowCoordinatesToDisplayCoordinates(s32 window_x, s32 window_y, s32 window_width,
                                                                        s32 window_height) const;

  /// Helper function to save texture data to a PNG. If flip_y is set, the image will be flipped aka OpenGL.
  bool WriteTextureToFile(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, std::string filename,
                          bool clear_alpha = true, bool flip_y = false, u32 resize_width = 0, u32 resize_height = 0,
                          bool compress_on_thread = false);

  /// Helper function to save current display texture to PNG.
  bool WriteDisplayTextureToFile(std::string filename, bool full_resolution = true, bool apply_aspect_ratio = true,
                                 bool compress_on_thread = false);

  /// Helper function to save current display texture to a buffer.
  bool WriteDisplayTextureToBuffer(std::vector<u32>* buffer, u32 resize_width = 0, u32 resize_height = 0,
                                   bool clear_alpha = true);

  /// Helper function to save screenshot to PNG.
  bool WriteScreenshotToFile(std::string filename, bool compress_on_thread = false);

protected:
  ALWAYS_INLINE bool HasSoftwareCursor() const { return static_cast<bool>(m_cursor_texture); }
  ALWAYS_INLINE bool HasDisplayTexture() const { return (m_display_texture != nullptr); }

  bool IsUsingLinearFiltering() const;

  void CalculateDrawRect(s32 window_width, s32 window_height, float* out_left, float* out_top, float* out_width,
                         float* out_height, float* out_left_padding, float* out_top_padding, float* out_scale,
                         float* out_x_scale, bool apply_aspect_ratio = true) const;

  std::tuple<s32, s32, s32, s32> CalculateSoftwareCursorDrawRect() const;
  std::tuple<s32, s32, s32, s32> CalculateSoftwareCursorDrawRect(s32 cursor_x, s32 cursor_y) const;

  WindowInfo m_window_info;

  u64 m_last_frame_displayed_time = 0;

  s32 m_mouse_position_x = 0;
  s32 m_mouse_position_y = 0;

  s32 m_display_width = 0;
  s32 m_display_height = 0;
  s32 m_display_active_left = 0;
  s32 m_display_active_top = 0;
  s32 m_display_active_width = 0;
  s32 m_display_active_height = 0;
  float m_display_aspect_ratio = 1.0f;
  float m_display_frame_interval = 0.0f;

  GPUTexture* m_display_texture = nullptr;
  s32 m_display_texture_view_x = 0;
  s32 m_display_texture_view_y = 0;
  s32 m_display_texture_view_width = 0;
  s32 m_display_texture_view_height = 0;

  std::unique_ptr<GPUTexture> m_cursor_texture;
  float m_cursor_texture_scale = 1.0f;

  bool m_display_changed = false;
  bool m_gpu_timing_enabled = false;
};

/// Returns a pointer to the current host display abstraction. Assumes AcquireHostDisplay() has been caled.
extern std::unique_ptr<HostDisplay> g_host_display;

namespace Host {
std::unique_ptr<HostDisplay> CreateDisplayForAPI(RenderAPI api);

/// Creates the host display. This may create a new window. The API used depends on the current configuration.
bool AcquireHostDisplay(RenderAPI api);

/// Destroys the host display. This may close the display window.
void ReleaseHostDisplay();

/// Returns false if the window was completely occluded. If frame_skip is set, the frame won't be
/// displayed, but the GPU command queue will still be flushed.
// bool BeginPresentFrame(bool frame_skip);

/// Presents the frame to the display, and renders OSD elements.
// void EndPresentFrame();

/// Provided by the host; renders the display.
void RenderDisplay(bool skip_present);
void InvalidateDisplay();
} // namespace Host
