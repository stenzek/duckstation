// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "util/gpu_device.h"

#include <memory>
#include <string>
#include <vector>

class Error;
class Image;
class MediaCapture;
class SettingsInterface;

enum class DisplayScreenshotMode : u8;

class GPUBackend;

struct GPUSettings;
struct GPUBackendUpdateDisplayCommand;
struct GPUBackendFramePresentationParameters;

namespace PostProcessing {
class Chain;
}

class ALIGN_TO_CACHE_LINE GPUPresenter final
{
public:
  GPUPresenter();
  virtual ~GPUPresenter();

  ALWAYS_INLINE s32 GetDisplayWidth() const { return m_display_width; }
  ALWAYS_INLINE s32 GetDisplayHeight() const { return m_display_height; }
  ALWAYS_INLINE s32 GetDisplayVRAMWidth() const { return m_display_vram_width; }
  ALWAYS_INLINE s32 GetDisplayVRAMHeight() const { return m_display_vram_height; }
  ALWAYS_INLINE s32 GetDisplayTextureViewX() const { return m_display_texture_view_x; }
  ALWAYS_INLINE s32 GetDisplayTextureViewY() const { return m_display_texture_view_y; }
  ALWAYS_INLINE s32 GetDisplayTextureViewWidth() const { return m_display_texture_view_width; }
  ALWAYS_INLINE s32 GetDisplayTextureViewHeight() const { return m_display_texture_view_height; }
  ALWAYS_INLINE GPUTexture* GetDisplayTexture() const { return m_display_texture; }
  ALWAYS_INLINE bool HasDisplayTexture() const { return m_display_texture; }
  ALWAYS_INLINE bool HasBorderOverlay() const { return static_cast<bool>(m_border_overlay_texture); }

  bool Initialize(Error* error);

  bool UpdateSettings(const GPUSettings& old_settings, Error* error);
  bool UpdatePostProcessingSettings(bool force_reload, Error* error);

  void ClearDisplay();
  void ClearDisplayTexture();
  void SetDisplayParameters(u16 display_width, u16 display_height, u16 display_origin_left, u16 display_origin_top,
                            u16 display_vram_width, u16 display_vram_height, float display_pixel_aspect_ratio,
                            bool display_24bit);
  void SetDisplayTexture(GPUTexture* texture, s32 view_x, s32 view_y, s32 view_width, s32 view_height);
  bool Deinterlace(u32 field);
  bool ApplyChromaSmoothing();

  /// Helper function for computing the draw rectangle in a larger window.
  void CalculateDrawRect(s32 window_width, s32 window_height, bool apply_aspect_ratio, bool integer_scale,
                         bool apply_alignment, GSVector4i* display_rect, GSVector4i* draw_rect) const;

  /// Helper function for computing screenshot bounds.
  GSVector2i CalculateScreenshotSize(DisplayScreenshotMode mode) const;

  /// Renders the display, optionally with postprocessing to the specified image.
  bool RenderScreenshotToBuffer(u32 width, u32 height, bool postfx, bool apply_aspect_ratio, Image* out_image,
                                Error* error);

  /// Sends the current frame to media capture.
  void SendDisplayToMediaCapture(MediaCapture* cap);

  /// Main frame presenter - used both when a game is and is not running.
  static bool PresentFrame(GPUPresenter* presenter, GPUBackend* backend, bool allow_skip_present, u64 present_time);

  /// Returns a list of border overlay presets.
  static std::vector<std::string> EnumerateBorderOverlayPresets();

  /// Returns the settings interface to use for loading post-processing shader configuration.
  /// Assumes the settings lock is being held.
  static SettingsInterface& GetPostProcessingSettingsInterface(const char* section);

  /// Toggles post-processing. Only callable from the CPU thread.
  static void TogglePostProcessing();

  /// Reloads post-processing settings. Only callable from the CPU thread.
  static void ReloadPostProcessingSettings(bool display, bool internal, bool reload_shaders);

  // Draws the specified bounding box with display rotation and pre-rotation.
  static void DrawScreenQuad(const GSVector4i rect, const GSVector4 uv_rect, const GSVector2i target_size,
                             const GSVector2i final_target_size, DisplayRotation uv_rotation,
                             WindowInfo::PreRotation prerotation, const void* push_constants, u32 push_constants_size);

private:
  enum : u32
  {
    DEINTERLACE_BUFFER_COUNT = 4,
    MAX_SKIPPED_PRESENT_COUNT = 50,
  };

  static void SleepUntilPresentTime(u64 present_time);

  bool CompileDisplayPipelines(bool display, bool deinterlace, bool chroma_smoothing, Error* error);

  GPUDevice::PresentResult RenderDisplay(GPUTexture* target, const GSVector2i target_size, bool postfx,
                                         bool apply_aspect_ratio);
  void DrawOverlayBorders(const GSVector2i target_size, const GSVector2i final_target_size,
                          const GSVector4i overlay_display_rect, const GSVector4i draw_rect,
                          const WindowInfo::PreRotation prerotation);
  void DrawDisplay(const GSVector2i target_size, const GSVector2i final_target_size, const GSVector4i display_rect,
                   bool dst_alpha_blend, DisplayRotation rotation, WindowInfo::PreRotation prerotation);
  GPUDevice::PresentResult ApplyDisplayPostProcess(GPUTexture* target, GPUTexture* input,
                                                   const GSVector4i display_rect, const GSVector2i postfx_size);

  bool DeinterlaceSetTargetSize(u32 width, u32 height, bool preserve);
  void DestroyDeinterlaceTextures();

  void LoadPostProcessingSettings(bool force_load);

  /// Returns true if the image path or alpha blend option has changed.
  bool LoadOverlaySettings();
  bool LoadOverlayTexture();
  bool LoadOverlayPreset(Error* error, Image* image);

  s32 m_display_width = 0;
  s32 m_display_height = 0;

  s32 m_display_origin_left = 0;
  s32 m_display_origin_top = 0;
  s32 m_display_vram_width = 0;
  s32 m_display_vram_height = 0;
  float m_display_pixel_aspect_ratio = 1.0f;

  u32 m_current_deinterlace_buffer = 0;
  std::unique_ptr<GPUPipeline> m_deinterlace_pipeline;
  std::array<std::unique_ptr<GPUTexture>, DEINTERLACE_BUFFER_COUNT> m_deinterlace_buffers;
  std::unique_ptr<GPUTexture> m_deinterlace_texture;

  std::unique_ptr<GPUPipeline> m_chroma_smoothing_pipeline;
  std::unique_ptr<GPUTexture> m_chroma_smoothing_texture;

  std::unique_ptr<GPUPipeline> m_display_pipeline;
  std::unique_ptr<GPUPipeline> m_display_24bit_pipeline;
  GPUTexture* m_display_texture = nullptr;
  s32 m_display_texture_view_x = 0;
  s32 m_display_texture_view_y = 0;
  s32 m_display_texture_view_width = 0;
  s32 m_display_texture_view_height = 0;

  u32 m_skipped_present_count = 0;
  GPUTexture::Format m_present_format = GPUTexture::Format::Unknown;
  bool m_display_texture_24bit = false;
  bool m_border_overlay_alpha_blend = false;
  bool m_border_overlay_destination_alpha_blend = false;

  std::unique_ptr<GPUPipeline> m_present_copy_pipeline;

  std::unique_ptr<PostProcessing::Chain> m_display_postfx;
  std::unique_ptr<GPUTexture> m_border_overlay_texture;

  std::unique_ptr<GPUPipeline> m_border_overlay_pipeline;
  std::unique_ptr<GPUPipeline> m_present_clear_pipeline;
  std::unique_ptr<GPUPipeline> m_display_blend_pipeline;
  std::unique_ptr<GPUPipeline> m_display_24bit_blend_pipeline;
  std::unique_ptr<GPUPipeline> m_present_copy_blend_pipeline;

  GSVector4i m_border_overlay_display_rect = GSVector4i::zero();

  // Low-traffic variables down here.
  std::string m_border_overlay_image_path;
};

namespace Host {

/// Called at the end of the frame, before presentation.
void FrameDoneOnGPUThread(GPUPresenter* gpu_presenter, u32 frame_number);

} // namespace Host
