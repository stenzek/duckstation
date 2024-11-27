// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_types.h"
#include "timers.h"
#include "types.h"

#include "util/gpu_device.h"
#include "util/gpu_texture.h"

#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/types.h"

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

class Error;
class Image;
class SmallStringBase;

class StateWrapper;

class GPUDevice;
class GPUTexture;
class GPUPipeline;
class MediaCapture;

namespace GPUDump {
enum class PacketType : u8;
class Recorder;
class Player;
} // namespace GPUDump
struct Settings;

namespace Threading {
class Thread;
}

class GPU
{
public:
  enum class BlitterState : u8
  {
    Idle,
    ReadingVRAM,
    WritingVRAM,
    DrawingPolyLine
  };

  enum : u32
  {
    MAX_FIFO_SIZE = 4096,
    DOT_TIMER_INDEX = 0,
    HBLANK_TIMER_INDEX = 1,
    MAX_RESOLUTION_SCALE = 32,
    DEINTERLACE_BUFFER_COUNT = 4,
    DRAWING_AREA_COORD_MASK = 1023,
  };

  enum : u16
  {
    NTSC_TICKS_PER_LINE = 3413,
    NTSC_TOTAL_LINES = 263,
    PAL_TICKS_PER_LINE = 3406,
    PAL_TOTAL_LINES = 314,
  };

  enum : u16
  {
    NTSC_HORIZONTAL_ACTIVE_START = 488,
    NTSC_HORIZONTAL_ACTIVE_END = 3288,
    NTSC_VERTICAL_ACTIVE_START = 16,
    NTSC_VERTICAL_ACTIVE_END = 256,
    PAL_HORIZONTAL_ACTIVE_START = 487,
    PAL_HORIZONTAL_ACTIVE_END = 3282,
    PAL_VERTICAL_ACTIVE_START = 20,
    PAL_VERTICAL_ACTIVE_END = 308,
  };

  // Base class constructor.
  GPU();
  virtual ~GPU();

  virtual const Threading::Thread* GetSWThread() const = 0;
  virtual bool IsHardwareRenderer() const = 0;

  virtual bool Initialize(Error* error);
  virtual void Reset(bool clear_vram);
  virtual bool DoState(StateWrapper& sw, GPUTexture** save_to_texture, bool update_display);

  // Graphics API state reset/restore - call when drawing the UI etc.
  // TODO: replace with "invalidate cached state"
  virtual void RestoreDeviceContext();

  // Render statistics debug window.
  void DrawDebugStateWindow(float scale);
  void GetStatsString(SmallStringBase& str);
  void GetMemoryStatsString(SmallStringBase& str);
  void ResetStatistics();
  void UpdateStatistics(u32 frame_count);

  void CPUClockChanged();

  // MMIO access
  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  // DMA access
  void DMARead(u32* words, u32 word_count);

  ALWAYS_INLINE bool BeginDMAWrite() const
  {
    return (m_GPUSTAT.dma_direction == GPUDMADirection::CPUtoGP0 || m_GPUSTAT.dma_direction == GPUDMADirection::FIFO);
  }
  ALWAYS_INLINE void DMAWrite(u32 address, u32 value)
  {
    m_fifo.Push((ZeroExtend64(address) << 32) | ZeroExtend64(value));
  }
  void EndDMAWrite();

  /// Writing to GPU dump.
  GPUDump::Recorder* GetGPUDump() const { return m_gpu_dump.get(); }
  bool StartRecordingGPUDump(const char* path, u32 num_frames = 1);
  void StopRecordingGPUDump();
  void WriteCurrentVideoModeToDump(GPUDump::Recorder* dump) const;
  void ProcessGPUDumpPacket(GPUDump::PacketType type, const std::span<const u32> data);

  /// Returns true if no data is being sent from VRAM to the DAC or that no portion of VRAM would be visible on screen.
  ALWAYS_INLINE bool IsDisplayDisabled() const
  {
    return m_GPUSTAT.display_disable || m_crtc_state.display_vram_width == 0 || m_crtc_state.display_vram_height == 0;
  }

  /// Returns true if scanout should be interlaced.
  ALWAYS_INLINE bool IsInterlacedDisplayEnabled() const
  {
    return (!m_force_progressive_scan && m_GPUSTAT.vertical_interlace);
  }

  /// Returns true if interlaced rendering is enabled and force progressive scan is disabled.
  ALWAYS_INLINE bool IsInterlacedRenderingEnabled() const
  {
    return (!m_force_progressive_scan && m_GPUSTAT.SkipDrawingToActiveField());
  }

  /// Returns true if we're in PAL mode, otherwise false if NTSC.
  ALWAYS_INLINE bool IsInPALMode() const { return m_GPUSTAT.pal_mode; }

  /// Returns the number of pending GPU ticks.
  TickCount GetPendingCRTCTicks() const;
  TickCount GetPendingCommandTicks() const;
  TickCount GetRemainingCommandTicks() const;

  /// Returns true if enough ticks have passed for the raster to be on the next line.
  bool IsCRTCScanlinePending() const;

  /// Returns true if a raster scanline or command execution is pending.
  bool IsCommandCompletionPending() const;

  /// Synchronizes the CRTC, updating the hblank timer.
  void SynchronizeCRTC();

  /// Recompile shaders/recreate framebuffers when needed.
  virtual void UpdateSettings(const Settings& old_settings);

  /// Returns the current resolution scale.
  virtual u32 GetResolutionScale() const;

  /// Updates the resolution scale when it's set to automatic.
  virtual void UpdateResolutionScale();

  /// Returns the full display resolution of the GPU, including padding.
  std::tuple<u32, u32> GetFullDisplayResolution() const;

  float ComputeHorizontalFrequency() const;
  float ComputeVerticalFrequency() const;
  float ComputeDisplayAspectRatio() const;
  float ComputeSourceAspectRatio() const;

  /// Computes aspect ratio correction, i.e. the scale to apply to the source aspect ratio to preserve
  /// the original pixel aspect ratio regardless of how much cropping has been applied.
  float ComputeAspectRatioCorrection() const;

  /// Applies the pixel aspect ratio to a given size, preserving the larger dimension.
  void ApplyPixelAspectRatioToSize(float* width, float* height) const;

  static std::unique_ptr<GPU> CreateHardwareRenderer();
  static std::unique_ptr<GPU> CreateSoftwareRenderer();

  // Converts window coordinates into horizontal ticks and scanlines. Returns false if out of range. Used for lightguns.
  void ConvertScreenCoordinatesToDisplayCoordinates(float window_x, float window_y, float* display_x,
                                                    float* display_y) const;
  bool ConvertDisplayCoordinatesToBeamTicksAndLines(float display_x, float display_y, float x_scale, u32* out_tick,
                                                    u32* out_line) const;

  // Returns the current beam position.
  void GetBeamPosition(u32* out_ticks, u32* out_line);

  // Returns the number of system clock ticks until the specified tick/line.
  TickCount GetSystemTicksUntilTicksAndLine(u32 ticks, u32 line);

  // Returns the number of visible lines.
  ALWAYS_INLINE u16 GetCRTCActiveStartLine() const { return m_crtc_state.vertical_display_start; }
  ALWAYS_INLINE u16 GetCRTCActiveEndLine() const { return m_crtc_state.vertical_display_end; }

  // Returns the video clock frequency.
  TickCount GetCRTCFrequency() const;
  ALWAYS_INLINE u16 GetCRTCDotClockDivider() const { return m_crtc_state.dot_clock_divider; }
  ALWAYS_INLINE s32 GetCRTCDisplayWidth() const { return m_crtc_state.display_width; }
  ALWAYS_INLINE s32 GetCRTCDisplayHeight() const { return m_crtc_state.display_height; }

  // Ticks for hblank/vblank.
  void CRTCTickEvent(TickCount ticks);
  void CommandTickEvent(TickCount ticks);
  void FrameDoneEvent(TickCount ticks);

  // Dumps raw VRAM to a file.
  bool DumpVRAMToFile(const char* filename);

  // Ensures all buffered vertices are drawn.
  virtual void FlushRender() = 0;

  /// Helper function for computing the draw rectangle in a larger window.
  void CalculateDrawRect(s32 window_width, s32 window_height, bool apply_rotation, bool apply_aspect_ratio,
                         GSVector4i* display_rect, GSVector4i* draw_rect) const;

  /// Helper function for computing screenshot bounds.
  void CalculateScreenshotSize(DisplayScreenshotMode mode, u32* width, u32* height, GSVector4i* display_rect,
                               GSVector4i* draw_rect) const;

  /// Helper function to save current display texture to PNG.
  bool WriteDisplayTextureToFile(std::string path);

  /// Renders the display, optionally with postprocessing to the specified image.
  bool RenderScreenshotToBuffer(u32 width, u32 height, const GSVector4i display_rect, const GSVector4i draw_rect,
                                bool postfx, Image* out_image);

  /// Helper function to save screenshot to PNG.
  bool RenderScreenshotToFile(std::string path, DisplayScreenshotMode mode, u8 quality, bool compress_on_thread,
                              bool show_osd_message);

  /// Draws the current display texture, with any post-processing.
  GPUDevice::PresentResult PresentDisplay();

  /// Sends the current frame to media capture.
  bool SendDisplayToMediaCapture(MediaCapture* cap);

  /// Reads the CLUT from the specified coordinates, accounting for wrap-around.
  static void ReadCLUT(u16* dest, GPUTexturePaletteReg reg, bool clut_is_8bit);

protected:
  TickCount CRTCTicksToSystemTicks(TickCount crtc_ticks, TickCount fractional_ticks) const;
  TickCount SystemTicksToCRTCTicks(TickCount sysclk_ticks, TickCount* fractional_ticks) const;

  // The GPU internally appears to run at 2x the system clock.
  ALWAYS_INLINE static constexpr TickCount GPUTicksToSystemTicks(TickCount gpu_ticks)
  {
    return std::max<TickCount>((gpu_ticks + 1) >> 1, 1);
  }
  ALWAYS_INLINE static constexpr TickCount SystemTicksToGPUTicks(TickCount sysclk_ticks) { return sysclk_ticks << 1; }

  static constexpr std::tuple<u8, u8> UnpackTexcoord(u16 texcoord)
  {
    return std::make_tuple(static_cast<u8>(texcoord), static_cast<u8>(texcoord >> 8));
  }

  static constexpr std::tuple<u8, u8, u8> UnpackColorRGB24(u32 rgb24)
  {
    return std::make_tuple(static_cast<u8>(rgb24), static_cast<u8>(rgb24 >> 8), static_cast<u8>(rgb24 >> 16));
  }

  static bool DumpVRAMToFile(const char* filename, u32 width, u32 height, u32 stride, const void* buffer,
                             bool remove_alpha);

  void SoftReset();
  void ClearDisplay();

  // Sets dots per scanline
  void UpdateCRTCConfig();
  void UpdateCRTCDisplayParameters();

  // Update ticks for this execution slice
  void UpdateCRTCTickEvent();
  void UpdateCommandTickEvent();

  // Updates dynamic bits in GPUSTAT (ready to send VRAM/ready to receive DMA)
  void UpdateDMARequest();
  void UpdateGPUIdle();

  /// Returns 0 if the currently-displayed field is on odd lines (1,3,5,...) or 1 if even (2,4,6,...).
  ALWAYS_INLINE u32 GetInterlacedDisplayField() const { return ZeroExtend32(m_crtc_state.interlaced_field); }

  /// Returns 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  ALWAYS_INLINE u32 GetActiveLineLSB() const { return ZeroExtend32(m_crtc_state.active_line_lsb); }

  /// Updates drawing area that's suitablef or clamping.
  void SetClampedDrawingArea();

  /// Sets/decodes GP0(E1h) (set draw mode).
  void SetDrawMode(u16 bits);

  /// Sets/decodes polygon/rectangle texture palette value.
  void SetTexturePalette(u16 bits);

  /// Sets/decodes texture window bits.
  void SetTextureWindow(u32 value);

  u32 ReadGPUREAD();
  void FinishVRAMWrite();

  /// Returns the number of vertices in the buffered poly-line.
  ALWAYS_INLINE u32 GetPolyLineVertexCount() const
  {
    return (static_cast<u32>(m_blit_buffer.size()) + BoolToUInt32(m_render_command.shading_enable)) >>
           BoolToUInt8(m_render_command.shading_enable);
  }

  void AddCommandTicks(TickCount ticks);

  void WriteGP1(u32 value);
  void EndCommand();
  void ExecuteCommands();
  void TryExecuteCommands();
  void HandleGetGPUInfoCommand(u32 value);
  void UpdateCLUTIfNeeded(GPUTextureMode texmode, GPUTexturePaletteReg clut);
  void InvalidateCLUT();
  bool IsCLUTValid() const;

  // Rendering in the backend
  virtual void ReadVRAM(u32 x, u32 y, u32 width, u32 height) = 0;
  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) = 0;
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask) = 0;
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) = 0;
  virtual void DispatchRenderCommand() = 0;
  virtual void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit) = 0;
  virtual void UpdateDisplay() = 0;
  virtual void DrawRendererStats();
  virtual void OnBufferSwapped();

  ALWAYS_INLINE_RELEASE void AddDrawTriangleTicks(GSVector2i v1, GSVector2i v2, GSVector2i v3, bool shaded,
                                                  bool textured, bool semitransparent)
  {
    // This will not produce the correct results for triangles which are partially outside the clip area.
    // However, usually it'll undershoot not overshoot. If we wanted to make this more accurate, we'd need to intersect
    // the edges with the clip rectangle.
    // TODO: Coordinates are exclusive, so off by one here...
    const GSVector2i clamp_min = GSVector2i::load<true>(&m_clamped_drawing_area.x);
    const GSVector2i clamp_max = GSVector2i::load<true>(&m_clamped_drawing_area.z);
    v1 = v1.sat_s32(clamp_min, clamp_max);
    v2 = v2.sat_s32(clamp_min, clamp_max);
    v3 = v3.sat_s32(clamp_min, clamp_max);

    TickCount pixels =
      std::abs((v1.x * v2.y + v2.x * v3.y + v3.x * v1.y - v1.x * v3.y - v2.x * v1.y - v3.x * v2.y) / 2);
    if (textured)
      pixels += pixels;
    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      pixels += (pixels + 1) / 2;
    if (m_GPUSTAT.SkipDrawingToActiveField())
      pixels /= 2;

    AddCommandTicks(pixels);
  }
  ALWAYS_INLINE_RELEASE void AddDrawRectangleTicks(const GSVector4i clamped_rect, bool textured, bool semitransparent)
  {
    u32 drawn_width = clamped_rect.width();
    u32 drawn_height = clamped_rect.height();

    u32 ticks_per_row = drawn_width;
    if (textured)
    {
      switch (m_draw_mode.mode_reg.texture_mode)
      {
        case GPUTextureMode::Palette4Bit:
          ticks_per_row += drawn_width;
          break;

        case GPUTextureMode::Palette8Bit:
        {
          // Texture cache reload every 2 pixels, reads in 8 bytes (assuming 4x2). Cache only reloads if the
          // draw width is greater than 32, otherwise the cache hits between rows.
          if (drawn_width >= 32)
            ticks_per_row += (drawn_width / 4) * 8;
          else
            ticks_per_row += drawn_width;
        }
        break;

        case GPUTextureMode::Direct16Bit:
        case GPUTextureMode::Reserved_Direct16Bit:
        {
          // Same as above, except with 2x2 blocks instead of 4x2.
          if (drawn_width >= 32)
            ticks_per_row += (drawn_width / 2) * 8;
          else
            ticks_per_row += drawn_width;
        }
        break;

          DefaultCaseIsUnreachable()
      }
    }

    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      ticks_per_row += (drawn_width + 1u) / 2u;
    if (m_GPUSTAT.SkipDrawingToActiveField())
      drawn_height = std::max<u32>(drawn_height / 2, 1u);

    AddCommandTicks(ticks_per_row * drawn_height);
  }
  ALWAYS_INLINE_RELEASE void AddDrawLineTicks(const GSVector4i clamped_rect, bool shaded)
  {
    u32 drawn_width = clamped_rect.width();
    u32 drawn_height = clamped_rect.height();

    if (m_GPUSTAT.SkipDrawingToActiveField())
      drawn_height = std::max<u32>(drawn_height / 2, 1u);

    AddCommandTicks(std::max(drawn_width, drawn_height));
  }

  GPUSTAT m_GPUSTAT = {};

  struct DrawMode
  {
    static constexpr u16 PALETTE_MASK = UINT16_C(0b0111111111111111);
    static constexpr u32 TEXTURE_WINDOW_MASK = UINT32_C(0b11111111111111111111);

    // original values
    GPUDrawModeReg mode_reg;
    GPUTexturePaletteReg palette_reg; // from vertex
    u32 texture_window_value;

    // decoded values
    GPUTextureWindow texture_window;
    bool texture_x_flip;
    bool texture_y_flip;
    bool texture_page_changed;

    ALWAYS_INLINE bool IsTexturePageChanged() const { return texture_page_changed; }
    ALWAYS_INLINE void SetTexturePageChanged() { texture_page_changed = true; }
    ALWAYS_INLINE void ClearTexturePageChangedFlag() { texture_page_changed = false; }
  } m_draw_mode = {};

  GPUDrawingArea m_drawing_area = {};
  GPUDrawingOffset m_drawing_offset = {};
  GSVector4i m_clamped_drawing_area = {};

  bool m_console_is_pal = false;
  bool m_set_texture_disable_mask = false;
  bool m_drawing_area_changed = false;
  bool m_force_progressive_scan = false;
  ForceVideoTimingMode m_force_frame_timings = ForceVideoTimingMode::Disabled;

  struct CRTCState
  {
    struct Regs
    {
      static constexpr u32 DISPLAY_ADDRESS_START_MASK = 0b111'11111111'11111110;
      static constexpr u32 HORIZONTAL_DISPLAY_RANGE_MASK = 0b11111111'11111111'11111111;
      static constexpr u32 VERTICAL_DISPLAY_RANGE_MASK = 0b1111'11111111'11111111;

      union
      {
        u32 display_address_start;
        BitField<u32, u16, 0, 10> X;
        BitField<u32, u16, 10, 9> Y;
      };
      union
      {
        u32 horizontal_display_range;
        BitField<u32, u16, 0, 12> X1;
        BitField<u32, u16, 12, 12> X2;
      };

      union
      {
        u32 vertical_display_range;
        BitField<u32, u16, 0, 10> Y1;
        BitField<u32, u16, 10, 10> Y2;
      };
    } regs;

    u16 dot_clock_divider;

    // Size of the simulated screen in pixels. Depending on crop mode, this may include overscan area.
    u16 display_width;
    u16 display_height;

    // Top-left corner in screen coordinates where the outputted portion of VRAM is first visible.
    u16 display_origin_left;
    u16 display_origin_top;

    // Rectangle in VRAM coordinates describing the area of VRAM that is visible on screen.
    u16 display_vram_left;
    u16 display_vram_top;
    u16 display_vram_width;
    u16 display_vram_height;

    // Visible range of the screen, in GPU ticks/lines. Clamped to lie within the active video region.
    u16 horizontal_visible_start;
    u16 horizontal_visible_end;
    u16 vertical_visible_start;
    u16 vertical_visible_end;

    u16 horizontal_display_start;
    u16 horizontal_display_end;
    u16 vertical_display_start;
    u16 vertical_display_end;

    u16 horizontal_active_start;
    u16 horizontal_active_end;

    u16 horizontal_total;
    u16 vertical_total;

    TickCount fractional_ticks;
    TickCount current_tick_in_scanline;
    u32 current_scanline;

    TickCount fractional_dot_ticks; // only used when timer0 is enabled

    bool in_hblank;
    bool in_vblank;

    u8 interlaced_field; // 0 = odd, 1 = even
    u8 interlaced_display_field;
    u8 active_line_lsb;

    ALWAYS_INLINE void UpdateHBlankFlag()
    {
      in_hblank =
        (current_tick_in_scanline < horizontal_active_start || current_tick_in_scanline >= horizontal_active_end);
    }
  } m_crtc_state = {};

  BlitterState m_blitter_state = BlitterState::Idle;
  u32 m_command_total_words = 0;
  TickCount m_pending_command_ticks = 0;

  /// GPUREAD value for non-VRAM-reads.
  u32 m_GPUREAD_latch = 0;

  // These are the bits from the palette register, but zero extended to 32-bit, so we can have an "invalid" value.
  // If an extra byte is ever not needed here for padding, the 8-bit flag could be packed into the MSB of this value.
  u32 m_current_clut_reg_bits = {};
  bool m_current_clut_is_8bit = false;

  /// True if currently executing/syncing.
  bool m_executing_commands = false;

  struct VRAMTransfer
  {
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u16 col;
    u16 row;
  } m_vram_transfer = {};

  HeapFIFOQueue<u64, MAX_FIFO_SIZE> m_fifo;
  std::vector<u32> m_blit_buffer;
  u32 m_blit_remaining_words;
  GPURenderCommand m_render_command{};

  std::unique_ptr<GPUDump::Recorder> m_gpu_dump;

  ALWAYS_INLINE u32 FifoPop() { return Truncate32(m_fifo.Pop()); }
  ALWAYS_INLINE u32 FifoPeek() { return Truncate32(m_fifo.Peek()); }
  ALWAYS_INLINE u32 FifoPeek(u32 i) { return Truncate32(m_fifo.Peek(i)); }

  TickCount m_max_run_ahead = 128;
  u32 m_fifo_size = 128;

  void ClearDisplayTexture();
  void SetDisplayTexture(GPUTexture* texture, GPUTexture* depth_texture, s32 view_x, s32 view_y, s32 view_width,
                         s32 view_height);

  GPUDevice::PresentResult RenderDisplay(GPUTexture* target, const GSVector4i display_rect, const GSVector4i draw_rect,
                                         bool postfx);

  bool Deinterlace(u32 field, u32 line_skip);
  bool DeinterlaceExtractField(u32 dst_bufidx, GPUTexture* src, u32 x, u32 y, u32 width, u32 height, u32 line_skip);
  bool DeinterlaceSetTargetSize(u32 width, u32 height, bool preserve);
  void DestroyDeinterlaceTextures();
  bool ApplyChromaSmoothing();

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

  struct Counters
  {
    u32 num_reads;
    u32 num_writes;
    u32 num_copies;
    u32 num_vertices;
    u32 num_primitives;

    // u32 num_read_texture_updates;
    // u32 num_ubo_updates;
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

  Counters m_counters = {};
  Stats m_stats = {};

private:
  bool CompileDisplayPipelines(bool display, bool deinterlace, bool chroma_smoothing, Error* error);

  using GP0CommandHandler = bool (GPU::*)();
  using GP0CommandHandlerTable = std::array<GP0CommandHandler, 256>;
  static GP0CommandHandlerTable GenerateGP0CommandHandlerTable();

  // Rendering commands, returns false if not enough data is provided
  bool HandleUnknownGP0Command();
  bool HandleNOPCommand();
  bool HandleClearCacheCommand();
  bool HandleInterruptRequestCommand();
  bool HandleSetDrawModeCommand();
  bool HandleSetTextureWindowCommand();
  bool HandleSetDrawingAreaTopLeftCommand();
  bool HandleSetDrawingAreaBottomRightCommand();
  bool HandleSetDrawingOffsetCommand();
  bool HandleSetMaskBitCommand();
  bool HandleRenderPolygonCommand();
  bool HandleRenderRectangleCommand();
  bool HandleRenderLineCommand();
  bool HandleRenderPolyLineCommand();
  bool HandleFillRectangleCommand();
  bool HandleCopyRectangleCPUToVRAMCommand();
  bool HandleCopyRectangleVRAMToCPUCommand();
  bool HandleCopyRectangleVRAMToVRAMCommand();

  static const GP0CommandHandlerTable s_GP0_command_handler_table;
};

extern std::unique_ptr<GPU> g_gpu;
extern u16 g_vram[VRAM_SIZE / sizeof(u16)];
extern u16 g_gpu_clut[GPU_CLUT_SIZE];
