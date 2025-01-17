// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/gpu_device.h"

#include <memory>

class Error;
class Image;
class MediaCapture;

enum class DisplayScreenshotMode : u8;

class GPUBackend;

struct GPUSettings;
struct GPUBackendUpdateDisplayCommand;
struct GPUBackendFramePresentationParameters;

class GPUPresenter final
{
public:
  GPUPresenter();
  virtual ~GPUPresenter();

  /// Main frame presenter - used both when a game is and is not running.
  static bool PresentFrame(GPUPresenter* presenter, GPUBackend* backend, bool allow_skip_present, u64 present_time);

  ALWAYS_INLINE s32 GetDisplayWidth() const { return m_display_width; }
  ALWAYS_INLINE s32 GetDisplayHeight() const { return m_display_height; }
  ALWAYS_INLINE s32 GetDisplayVRAMWidth() const { return m_display_vram_width; }
  ALWAYS_INLINE s32 GetDisplayVRAMHeight() const { return m_display_vram_height; }
  ALWAYS_INLINE s32 GetDisplayTextureViewX() const { return m_display_texture_view_x; }
  ALWAYS_INLINE s32 GetDisplayTextureViewY() const { return m_display_texture_view_y; }
  ALWAYS_INLINE s32 GetDisplayTextureViewWidth() const { return m_display_texture_view_width; }
  ALWAYS_INLINE s32 GetDisplayTextureViewHeight() const { return m_display_texture_view_height; }
  ALWAYS_INLINE GPUTexture* GetDisplayTexture() const { return m_display_texture; }
  ALWAYS_INLINE GPUTexture* GetDisplayDepthBuffer() const { return m_display_depth_buffer; }
  ALWAYS_INLINE bool HasDisplayTexture() const { return m_display_texture; }

  bool Initialize(Error* error);

  void UpdateSettings(const GPUSettings& old_settings);

  void ClearDisplay();
  void ClearDisplayTexture();
  void SetDisplayParameters(u16 display_width, u16 display_height, u16 display_origin_left, u16 display_origin_top,
                            u16 display_vram_width, u16 display_vram_height, float display_pixel_aspect_ratio);
  void SetDisplayTexture(GPUTexture* texture, GPUTexture* depth_buffer, s32 view_x, s32 view_y, s32 view_width,
                         s32 view_height);
  bool Deinterlace(u32 field);
  bool ApplyChromaSmoothing();

  /// Helper function for computing the draw rectangle in a larger window.
  void CalculateDrawRect(s32 window_width, s32 window_height, bool apply_rotation, bool apply_aspect_ratio,
                         GSVector4i* display_rect, GSVector4i* draw_rect) const;

  /// Helper function for computing screenshot bounds.
  void CalculateScreenshotSize(DisplayScreenshotMode mode, u32* width, u32* height, GSVector4i* display_rect,
                               GSVector4i* draw_rect) const;

  /// Renders the display, optionally with postprocessing to the specified image.
  bool RenderScreenshotToBuffer(u32 width, u32 height, const GSVector4i display_rect, const GSVector4i draw_rect,
                                bool postfx, Image* out_image);

  /// Sends the current frame to media capture.
  void SendDisplayToMediaCapture(MediaCapture* cap);

private:
  enum : u32
  {
    DEINTERLACE_BUFFER_COUNT = 4,
    MAX_SKIPPED_PRESENT_COUNT = 50,
  };

  static void SleepUntilPresentTime(u64 present_time);

  /// Draws the current display texture, with any post-processing.
  GPUDevice::PresentResult PresentDisplay();

  bool CompileDisplayPipelines(bool display, bool deinterlace, bool chroma_smoothing, Error* error);

  GPUDevice::PresentResult RenderDisplay(GPUTexture* target, const GSVector4i display_rect, const GSVector4i draw_rect,
                                         bool postfx);

  bool DeinterlaceSetTargetSize(u32 width, u32 height, bool preserve);
  void DestroyDeinterlaceTextures();

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
  GPUTexture* m_display_texture = nullptr;
  GPUTexture* m_display_depth_buffer = nullptr;
  s32 m_display_texture_view_x = 0;
  s32 m_display_texture_view_y = 0;
  s32 m_display_texture_view_width = 0;
  s32 m_display_texture_view_height = 0;

  u32 m_skipped_present_count = 0;
};

namespace Host {

/// Called at the end of the frame, before presentation.
void FrameDoneOnGPUThread(GPUPresenter* gpu_presenter, u32 frame_number);

} // namespace Host
