// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/gpu_device.h"

#include "gpu_thread_commands.h"

#include <memory>

class Error;
class SmallStringBase;

class GPUFramebuffer;
class GPUPipeline;

struct GPUSettings;
class StateWrapper;

class GPUPresenter;

namespace System {
struct MemorySaveState;
}

// DESIGN NOTE: Only static methods should be called on the CPU thread.
// You specifically don't have a global pointer available for this reason.

class ALIGN_TO_CACHE_LINE GPUBackend
{
public:
  static GPUThreadCommand* NewClearVRAMCommand();
  static GPUThreadCommand* NewClearDisplayCommand();
  static GPUBackendUpdateDisplayCommand* NewUpdateDisplayCommand();
  static GPUBackendSubmitFrameCommand* NewSubmitFrameCommand();
  static GPUThreadCommand* NewClearCacheCommand();
  static GPUThreadCommand* NewBufferSwappedCommand();
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
  static GPUBackendDrawPreciseLineCommand* NewDrawPreciseLineCommand(u32 num_vertices);
  static void PushCommand(GPUThreadCommand* cmd);
  static void PushCommandAndWakeThread(GPUThreadCommand* cmd);
  static void PushCommandAndSync(GPUThreadCommand* cmd, bool spin);
  static void SyncGPUThread(bool spin);

  static bool IsUsingHardwareBackend();

  static std::unique_ptr<GPUBackend> CreateHardwareBackend(GPUPresenter& presenter);
  static std::unique_ptr<GPUBackend> CreateSoftwareBackend(GPUPresenter& presenter);
  static std::unique_ptr<GPUBackend> CreateNullBackend(GPUPresenter& presenter);

  static bool RenderScreenshotToBuffer(u32 width, u32 height, bool postfx, bool apply_aspect_ratio, Image* out_image,
                                       Error* error);
  static void RenderScreenshotToFile(const std::string_view path, DisplayScreenshotMode mode, u8 quality,
                                     bool show_osd_message);

  static bool BeginQueueFrame();
  static void WaitForOneQueuedFrame();
  static u32 GetQueuedFrameCount();

  static bool AllocateMemorySaveStates(std::span<System::MemorySaveState> states, Error* error);

public:
  GPUBackend(GPUPresenter& presenter);
  virtual ~GPUBackend();

  ALWAYS_INLINE const GPUPresenter& GetPresenter() const { return m_presenter; }
  ALWAYS_INLINE GPUPresenter& GetPresenter() { return m_presenter; }

  virtual bool Initialize(bool upload_vram, Error* error);

  virtual bool UpdateSettings(const GPUSettings& old_settings, Error* error);
  virtual void UpdatePostProcessingSettings(bool force_reload);

  /// Returns the current resolution scale.
  virtual u32 GetResolutionScale() const = 0;

  // Graphics API state reset/restore - call when drawing the UI etc.
  // TODO: replace with "invalidate cached state"
  virtual void RestoreDeviceContext() = 0;

  /// Ensures all pending draws are flushed to the host GPU.
  virtual void FlushRender() = 0;

  /// Main command handler for GPU thread.
  void HandleCommand(const GPUThreadCommand* cmd);

  void GetStatsString(SmallStringBase& str) const;
  void GetMemoryStatsString(SmallStringBase& str) const;

  void ResetStatistics();
  void UpdateStatistics(u32 frame_count);

  /// Screen-aligned vertex type for various draw types.
  struct ScreenVertex
  {
    float x;
    float y;
    float u;
    float v;

    ALWAYS_INLINE void Set(const GSVector2& xy, const GSVector2& uv)
    {
      GSVector4::store<false>(this, GSVector4::xyxy(xy, uv));
    }
  };

  static void SetScreenQuadInputLayout(GPUPipeline::GraphicsConfig& config);
  static GSVector4 GetScreenQuadClipSpaceCoordinates(const GSVector4i bounds, const GSVector2i rt_size);

  static void DrawScreenQuad(const GSVector4i bounds, const GSVector2i rt_size, const GSVector4 uv_bounds,
                             const void* push_constants, u32 push_constants_size);

protected:
  enum : u32
  {
    DEINTERLACE_BUFFER_COUNT = 4,
  };

  struct Counters
  {
    u32 num_reads;
    u32 num_writes;
    u32 num_copies;
    u32 num_vertices;
    u32 num_primitives;
    u32 num_depth_buffer_clears;
  };

  struct Stats : Counters
  {
    size_t host_buffer_streamed;
    u32 host_num_draws;
    u32 host_num_barriers;
    u32 host_num_render_passes;
    u32 host_num_copies;
    u32 host_num_downloads;
    u32 host_num_uploads;
  };

  virtual void ReadVRAM(u32 x, u32 y, u32 width, u32 height) = 0;
  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, bool interlaced_rendering,
                        u8 interlaced_display_field) = 0;
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) = 0;
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask,
                        bool check_mask) = 0;

  virtual void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) = 0;
  virtual void DrawPrecisePolygon(const GPUBackendDrawPrecisePolygonCommand* cmd) = 0;
  virtual void DrawSprite(const GPUBackendDrawRectangleCommand* cmd) = 0;
  virtual void DrawLine(const GPUBackendDrawLineCommand* cmd) = 0;
  virtual void DrawPreciseLine(const GPUBackendDrawPreciseLineCommand* cmd) = 0;

  virtual void DrawingAreaChanged() = 0;
  virtual void ClearCache() = 0;
  virtual void OnBufferSwapped() = 0;
  virtual void ClearVRAM() = 0;

  virtual void UpdateDisplay(const GPUBackendUpdateDisplayCommand* cmd) = 0;

  virtual void LoadState(const GPUBackendLoadStateCommand* cmd) = 0;

  virtual bool AllocateMemorySaveState(System::MemorySaveState& mss, Error* error) = 0;
  virtual void DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss) = 0;

  void HandleUpdateDisplayCommand(const GPUBackendUpdateDisplayCommand* cmd);
  void HandleSubmitFrameCommand(const GPUBackendFramePresentationParameters* cmd);

  GPUPresenter& m_presenter;
  GSVector4i m_clamped_drawing_area = {};

  static Counters s_counters;
  static Stats s_stats;

private:
  static void ReleaseQueuedFrame();
};

namespace Host {

/// Called at the end of the frame, before presentation.
void FrameDoneOnGPUThread(GPUBackend* gpu_backend, u32 frame_number);

} // namespace Host
