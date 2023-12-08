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

class GPUBackend;
struct Settings;

namespace System {
struct MemorySaveState;
}

class GPU final
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
  ~GPU();

  void Initialize();
  void Reset(bool clear_vram);
  bool DoState(StateWrapper& sw, bool update_display);
  void DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss, bool update_display);

  // Render statistics debug window.
  void DrawDebugStateWindow(float scale);

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
  void UpdateSettings(const Settings& old_settings);

  /// Returns the full display resolution of the GPU, including padding.
  std::tuple<u32, u32> GetFullDisplayResolution() const;

  /// Computes clamped drawing area.
  static GSVector4i GetClampedDrawingArea(const GPUDrawingArea& drawing_area);

  float ComputeHorizontalFrequency() const;
  float ComputeVerticalFrequency() const;
  float ComputeDisplayAspectRatio() const;
  float ComputeSourceAspectRatio() const;
  float ComputePixelAspectRatio() const;

  /// Computes aspect ratio correction, i.e. the scale to apply to the source aspect ratio to preserve
  /// the original pixel aspect ratio regardless of how much cropping has been applied.
  float ComputeAspectRatioCorrection() const;

  /// Applies the pixel aspect ratio to a given size, preserving the larger dimension.
  static void ApplyPixelAspectRatioToSize(float par, float* width, float* height);

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

  // Queues the current frame for presentation. Should only be used with runahead.
  void QueuePresentCurrentFrame();

  /// Helper function for computing the draw rectangle in a larger window.
  static void CalculateDrawRect(u32 window_width, u32 window_height, u32 crtc_display_width, u32 crtc_display_height,
                                s32 display_origin_left, s32 display_origin_top, u32 display_vram_width,
                                u32 display_vram_height, DisplayRotation rotation, float pixel_aspect_ratio,
                                bool stretch_vertically, bool integer_scale, GSVector4i* display_rect,
                                GSVector4i* draw_rect);

private:
  TickCount CRTCTicksToSystemTicks(TickCount crtc_ticks, TickCount fractional_ticks) const;
  TickCount SystemTicksToCRTCTicks(TickCount sysclk_ticks, TickCount* fractional_ticks) const;

  // The GPU internally appears to run at 2x the system clock.
  ALWAYS_INLINE static constexpr TickCount GPUTicksToSystemTicks(TickCount gpu_ticks)
  {
    return std::max<TickCount>((gpu_ticks + 1) >> 1, 1);
  }
  ALWAYS_INLINE static constexpr TickCount SystemTicksToGPUTicks(TickCount sysclk_ticks) { return sysclk_ticks << 1; }

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
  ALWAYS_INLINE u8 GetInterlacedDisplayField() const { return m_crtc_state.interlaced_field; }

  /// Returns 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  ALWAYS_INLINE u8 GetActiveLineLSB() const { return m_crtc_state.active_line_lsb; }

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

  void ReadVRAM(u16 x, u16 y, u16 width, u16 height);
  void UpdateVRAM(u16 x, u16 y, u16 width, u16 height, const void* data, bool set_mask, bool check_mask);
  void UpdateDisplay(bool submit_frame);

  void PrepareForDraw();
  void FinishPolyline();
  void FillBackendCommandParameters(GPUBackendCommand* cmd) const;
  void FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const;

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
    // TODO: Make this a command
    GPUTextureWindow texture_window;
    bool texture_x_flip;
    bool texture_y_flip;
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

private:
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
