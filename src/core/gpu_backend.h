// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"

#include "util/gpu_device.h"

#include "common/heap_array.h"
#include "common/threading.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <tuple>

class Error;
class SmallStringBase;

class GPUFramebuffer;
class GPUPipeline;

struct Settings;
class StateWrapper;

namespace System {
struct MemorySaveState;
}

// DESIGN NOTE: Only static methods should be called on the CPU thread.
// You specifically don't have a global pointer available for this reason.

class GPUBackend
{
public:
  static GPUThreadCommand* NewClearVRAMCommand();
  static GPUThreadCommand* NewClearDisplayCommand();
  static GPUBackendUpdateDisplayCommand* NewUpdateDisplayCommand();
  static GPUBackendSubmitFrameCommand* NewSubmitFrameCommand();
  static GPUThreadCommand* NewClearCacheCommand();
  static GPUThreadCommand* NewBufferSwappedCommand();
  static GPUThreadCommand* NewUpdateResolutionScaleCommand();
  static GPUBackendReadVRAMCommand* NewReadVRAMCommand();
  static GPUBackendFillVRAMCommand* NewFillVRAMCommand();
  static GPUBackendUpdateVRAMCommand* NewUpdateVRAMCommand(u32 num_words);
  static GPUBackendCopyVRAMCommand* NewCopyVRAMCommand();
  static GPUBackendSetDrawingAreaCommand* NewSetDrawingAreaCommand();
  static GPUBackendUpdateCLUTCommand* NewUpdateCLUTCommand();
  static GPUBackendDrawPolygonCommand* NewDrawPolygonCommand(u32 num_vertices);
  static GPUBackendDrawPrecisePolygonCommand* NewDrawPrecisePolygonCommand(u32 num_vertices);
  static GPUBackendDrawRectangleCommand* NewDrawRectangleCommand();
  static GPUBackendDrawLineCommand* NewDrawLineCommand(u32 num_vertices);
  static void PushCommand(GPUThreadCommand* cmd);
  static void PushCommandAndWakeThread(GPUThreadCommand* cmd);
  static void PushCommandAndSync(GPUThreadCommand* cmd, bool spin);
  static void SyncGPUThread(bool spin);

  static bool IsUsingHardwareBackend();

  static std::unique_ptr<GPUBackend> CreateHardwareBackend();
  static std::unique_ptr<GPUBackend> CreateSoftwareBackend();

  static bool RenderScreenshotToBuffer(u32 width, u32 height, bool postfx, Image* out_image);
  static void RenderScreenshotToFile(const std::string_view path, DisplayScreenshotMode mode, u8 quality,
                                     bool compress_on_thread, bool show_osd_message);

  static bool BeginQueueFrame();
  static void WaitForOneQueuedFrame();

  static bool AllocateMemorySaveStates(std::span<System::MemorySaveState> states, Error* error);

public:
  GPUBackend();
  virtual ~GPUBackend();

  virtual bool Initialize(bool upload_vram, Error* error);

  virtual void UpdateSettings(const Settings& old_settings);

  /// Returns the current resolution scale.
  virtual u32 GetResolutionScale() const = 0;

  /// Updates the resolution scale when it's set to automatic.
  virtual void UpdateResolutionScale() = 0;

  // Graphics API state reset/restore - call when drawing the UI etc.
  // TODO: replace with "invalidate cached state"
  virtual void RestoreDeviceContext() = 0;

  /// Main command handler for GPU thread.
  void HandleCommand(const GPUThreadCommand* cmd);

  /// Draws the current display texture, with any post-processing.
  GPUDevice::PresentResult PresentDisplay();

  /// Helper function to save current display texture to PNG. Used for regtest.
  bool WriteDisplayTextureToFile(std::string filename);

  void GetStatsString(SmallStringBase& str) const;
  void GetMemoryStatsString(SmallStringBase& str) const;

  void ResetStatistics();
  void UpdateStatistics(u32 frame_count);

protected:
  enum : u32
  {
    DEINTERLACE_BUFFER_COUNT = 4,
  };

  virtual void ReadVRAM(u32 x, u32 y, u32 width, u32 height) = 0;
  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params) = 0;
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data,
                          GPUBackendCommandParameters params) = 0;
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                        GPUBackendCommandParameters params) = 0;

  virtual void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) = 0;
  virtual void DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd) = 0;
  virtual void DrawSprite(const GPUBackendDrawRectangleCommand* cmd) = 0;
  virtual void DrawLine(const GPUBackendDrawLineCommand* cmd) = 0;

  virtual void DrawingAreaChanged() = 0;
  virtual void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit) = 0;
  virtual void ClearCache() = 0;
  virtual void OnBufferSwapped() = 0;
  virtual void ClearVRAM() = 0;

  virtual void UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd) = 0;

  virtual void LoadState(const GPUBackendLoadStateCommand* cmd) = 0;

  virtual bool AllocateMemorySaveState(System::MemorySaveState& mss, Error* error) = 0;
  virtual void DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss) = 0;

  /// Ensures all pending draws are flushed to the host GPU.
  virtual void FlushRender() = 0;

  /// Helper function for computing the draw rectangle in a larger window.
  void CalculateDrawRect(s32 window_width, s32 window_height, bool apply_rotation, bool apply_aspect_ratio,
                         GSVector4i* display_rect, GSVector4i* draw_rect) const;

  /// Helper function for computing screenshot bounds.
  void CalculateScreenshotSize(DisplayScreenshotMode mode, u32* width, u32* height, GSVector4i* display_rect,
                               GSVector4i* draw_rect) const;

  /// Renders the display, optionally with postprocessing to the specified image.
  void HandleRenderScreenshotToBuffer(const GPUThreadRenderScreenshotToBufferCommand* cmd);
  void HandleRenderScreenshotToFile(const GPUThreadRenderScreenshotToFileCommand* cmd);

  /// Renders the display, optionally with postprocessing to the specified image.
  bool RenderScreenshotToBuffer(u32 width, u32 height, const GSVector4i display_rect, const GSVector4i draw_rect,
                                bool postfx, Image* out_image);

  bool CompileDisplayPipelines(bool display, bool deinterlace, bool chroma_smoothing, Error* error);

  void HandleUpdateDisplayCommand(const GPUBackendUpdateDisplayCommand* cmd);
  void HandleSubmitFrameCommand(const GPUBackendFramePresentationParameters* cmd);
  void HandleAllocateMemorySaveStatesCommand(const GPUBackendAllocateMemoryStatesCommand* cmd);

  void ClearDisplay();
  void ClearDisplayTexture();
  void SetDisplayTexture(GPUTexture* texture, GPUTexture* depth_buffer, s32 view_x, s32 view_y, s32 view_width,
                         s32 view_height);

  GPUDevice::PresentResult RenderDisplay(GPUTexture* target, const GSVector4i display_rect, const GSVector4i draw_rect,
                                         bool postfx);

  /// Sends the current frame to media capture.
  void SendDisplayToMediaCapture(MediaCapture* cap);

  bool Deinterlace(u32 field, u32 line_skip);
  bool DeinterlaceExtractField(u32 dst_bufidx, GPUTexture* src, u32 x, u32 y, u32 width, u32 height, u32 line_skip);
  bool DeinterlaceSetTargetSize(u32 width, u32 height, bool preserve);
  void DestroyDeinterlaceTextures();
  bool ApplyChromaSmoothing();

  s32 m_display_width = 0;
  s32 m_display_height = 0;
  s32 m_display_origin_left = 0;
  s32 m_display_origin_top = 0;
  s32 m_display_vram_width = 0;
  s32 m_display_vram_height = 0;
  float m_display_pixel_aspect_ratio = 1.0f;

  u32 m_current_deinterlace_buffer = 0;
  std::unique_ptr<GPUPipeline> m_deinterlace_pipeline;
  std::unique_ptr<GPUPipeline> m_deinterlace_extract_pipeline;
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
};

namespace Host {

/// Called at the end of the frame, before presentation.
void FrameDoneOnGPUThread(GPUBackend* gpu_backend, u32 frame_number);

} // namespace Host
