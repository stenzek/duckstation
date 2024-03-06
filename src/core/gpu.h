// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "gpu_types.h"
#include "timers.h"
#include "types.h"

#include "util/gpu_texture.h"

#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "common/rectangle.h"
#include "common/types.h"

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

class SmallStringBase;

class StateWrapper;

class GPUDevice;
class GPUTexture;
class GPUPipeline;

struct Settings;
class TimingEvent;

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

  enum class DMADirection : u32
  {
    Off = 0,
    FIFO = 1,
    CPUtoGP0 = 2,
    GPUREADtoCPU = 3
  };

  enum : u32
  {
    MAX_FIFO_SIZE = 4096,
    DOT_TIMER_INDEX = 0,
    HBLANK_TIMER_INDEX = 1,
    MAX_RESOLUTION_SCALE = 32,
  };

  enum : u16
  {
    NTSC_TICKS_PER_LINE = 3413,
    NTSC_HSYNC_TICKS = 200,
    NTSC_TOTAL_LINES = 263,
    PAL_TICKS_PER_LINE = 3406,
    PAL_HSYNC_TICKS = 200, // actually one more on odd lines
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

  virtual bool Initialize();
  virtual void Reset(bool clear_vram);
  virtual bool DoState(StateWrapper& sw, GPUTexture** save_to_texture, bool update_display);

  // Graphics API state reset/restore - call when drawing the UI etc.
  // TODO: replace with "invalidate cached state"
  virtual void RestoreDeviceContext();

  // Render statistics debug window.
  void DrawDebugStateWindow();
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

  ALWAYS_INLINE bool BeginDMAWrite() const { return (m_GPUSTAT.dma_direction == DMADirection::CPUtoGP0); }
  ALWAYS_INLINE void DMAWrite(u32 address, u32 value)
  {
    m_fifo.Push((ZeroExtend64(address) << 32) | ZeroExtend64(value));
  }
  void EndDMAWrite();

  /// Returns true if no data is being sent from VRAM to the DAC or that no portion of VRAM would be visible on screen.
  ALWAYS_INLINE bool IsDisplayDisabled() const
  {
    return m_GPUSTAT.display_disable || m_crtc_state.display_vram_width == 0 || m_crtc_state.display_vram_height == 0;
  }

  /// Returns true if scanout should be interlaced.
  ALWAYS_INLINE bool IsInterlacedDisplayEnabled() const
  {
    return (!m_force_progressive_scan) && m_GPUSTAT.vertical_interlace;
  }

  /// Returns true if interlaced rendering is enabled and force progressive scan is disabled.
  ALWAYS_INLINE bool IsInterlacedRenderingEnabled() const
  {
    return (!m_force_progressive_scan) && m_GPUSTAT.SkipDrawingToActiveField();
  }

  /// Returns true if we're in PAL mode, otherwise false if NTSC.
  ALWAYS_INLINE bool IsInPALMode() const { return m_GPUSTAT.pal_mode; }

  /// Returns the number of pending GPU ticks.
  TickCount GetPendingCRTCTicks() const;
  TickCount GetPendingCommandTicks() const;

  /// Returns true if enough ticks have passed for the raster to be on the next line.
  bool IsCRTCScanlinePending() const;

  /// Returns true if a raster scanline or command execution is pending.
  bool IsCommandCompletionPending() const;

  /// Synchronizes the CRTC, updating the hblank timer.
  void SynchronizeCRTC();

  /// Recompile shaders/recreate framebuffers when needed.
  virtual void UpdateSettings(const Settings& old_settings);

  /// Updates the resolution scale when it's set to automatic.
  virtual void UpdateResolutionScale();

  /// Returns the effective display resolution of the GPU.
  virtual std::tuple<u32, u32> GetEffectiveDisplayResolution(bool scaled = true);

  /// Returns the full display resolution of the GPU, including padding.
  virtual std::tuple<u32, u32> GetFullDisplayResolution(bool scaled = true);

  float ComputeHorizontalFrequency() const;
  float ComputeVerticalFrequency() const;
  float ComputeDisplayAspectRatio() const;

  static std::unique_ptr<GPU> CreateHardwareRenderer();
  static std::unique_ptr<GPU> CreateSoftwareRenderer();

  // Converts window coordinates into horizontal ticks and scanlines. Returns false if out of range. Used for lightguns.
  void ConvertScreenCoordinatesToDisplayCoordinates(float window_x, float window_y, float* display_x,
                                                    float* display_y) const;
  bool ConvertDisplayCoordinatesToBeamTicksAndLines(float display_x, float display_y, float x_scale, u32* out_tick,
                                                    u32* out_line) const;

  // Returns the video clock frequency.
  TickCount GetCRTCFrequency() const;

  // Dumps raw VRAM to a file.
  bool DumpVRAMToFile(const char* filename);

  // Ensures all buffered vertices are drawn.
  virtual void FlushRender();

  ALWAYS_INLINE const void* GetDisplayTextureHandle() const { return m_display_texture; }
  ALWAYS_INLINE s32 GetDisplayWidth() const { return m_display_width; }
  ALWAYS_INLINE s32 GetDisplayHeight() const { return m_display_height; }
  ALWAYS_INLINE float GetDisplayAspectRatio() const { return m_display_aspect_ratio; }
  ALWAYS_INLINE bool HasDisplayTexture() const { return static_cast<bool>(m_display_texture); }

  /// Helper function for computing the draw rectangle in a larger window.
  Common::Rectangle<s32> CalculateDrawRect(s32 window_width, s32 window_height, bool apply_aspect_ratio = true) const;

  /// Helper function to save current display texture to PNG.
  bool WriteDisplayTextureToFile(std::string filename, bool compress_on_thread = false);

  /// Renders the display, optionally with postprocessing to the specified image.
  bool RenderScreenshotToBuffer(u32 width, u32 height, const Common::Rectangle<s32>& draw_rect, bool postfx,
                                std::vector<u32>* out_pixels, u32* out_stride, GPUTexture::Format* out_format);

  /// Helper function to save screenshot to PNG.
  bool RenderScreenshotToFile(std::string filename, DisplayScreenshotMode mode, u8 quality, bool compress_on_thread);

  /// Draws the current display texture, with any post-processing.
  bool PresentDisplay();

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

  // Sets dots per scanline
  void UpdateCRTCConfig();
  void UpdateCRTCDisplayParameters();

  // Update ticks for this execution slice
  void UpdateCRTCTickEvent();
  void UpdateCommandTickEvent();

  // Updates dynamic bits in GPUSTAT (ready to send VRAM/ready to receive DMA)
  void UpdateDMARequest();
  void UpdateGPUIdle();

  // Ticks for hblank/vblank.
  void CRTCTickEvent(TickCount ticks);
  void CommandTickEvent(TickCount ticks);

  /// Returns 0 if the currently-displayed field is on odd lines (1,3,5,...) or 1 if even (2,4,6,...).
  ALWAYS_INLINE u32 GetInterlacedDisplayField() const { return ZeroExtend32(m_crtc_state.interlaced_field); }

  /// Returns 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  ALWAYS_INLINE u32 GetActiveLineLSB() const { return ZeroExtend32(m_crtc_state.active_line_lsb); }

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

  /// Returns true if the drawing area is valid (i.e. left <= right, top <= bottom).
  ALWAYS_INLINE bool IsDrawingAreaIsValid() const { return m_drawing_area.Valid(); }

  /// Clamps the specified coordinates to the drawing area.
  ALWAYS_INLINE void ClampCoordinatesToDrawingArea(s32* x, s32* y)
  {
    const s32 x_value = *x;
    if (x_value < static_cast<s32>(m_drawing_area.left))
      *x = m_drawing_area.left;
    else if (x_value >= static_cast<s32>(m_drawing_area.right))
      *x = m_drawing_area.right - 1;

    const s32 y_value = *y;
    if (y_value < static_cast<s32>(m_drawing_area.top))
      *y = m_drawing_area.top;
    else if (y_value >= static_cast<s32>(m_drawing_area.bottom))
      *y = m_drawing_area.bottom - 1;
  }

  void AddCommandTicks(TickCount ticks);

  void WriteGP1(u32 value);
  void EndCommand();
  void ExecuteCommands();
  void HandleGetGPUInfoCommand(u32 value);

  // Rendering in the backend
  virtual void ReadVRAM(u32 x, u32 y, u32 width, u32 height);
  virtual void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color);
  virtual void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask);
  virtual void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height);
  virtual void DispatchRenderCommand();
  virtual void ClearDisplay();
  virtual void UpdateDisplay();
  virtual void DrawRendererStats();

  ALWAYS_INLINE void AddDrawTriangleTicks(s32 x1, s32 y1, s32 x2, s32 y2, s32 x3, s32 y3, bool shaded, bool textured,
                                          bool semitransparent)
  {
    // This will not produce the correct results for triangles which are partially outside the clip area.
    // However, usually it'll undershoot not overshoot. If we wanted to make this more accurate, we'd need to intersect
    // the edges with the clip rectangle.
    ClampCoordinatesToDrawingArea(&x1, &y1);
    ClampCoordinatesToDrawingArea(&x2, &y2);
    ClampCoordinatesToDrawingArea(&x3, &y3);

    TickCount pixels = std::abs((x1 * y2 + x2 * y3 + x3 * y1 - x1 * y3 - x2 * y1 - x3 * y2) / 2);
    if (textured)
      pixels += pixels;
    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      pixels += (pixels + 1) / 2;
    if (m_GPUSTAT.SkipDrawingToActiveField())
      pixels /= 2;

    AddCommandTicks(pixels);
  }
  ALWAYS_INLINE void AddDrawRectangleTicks(u32 width, u32 height, bool textured, bool semitransparent)
  {
    u32 ticks_per_row = width;
    if (textured)
      ticks_per_row += width;
    if (semitransparent || m_GPUSTAT.check_mask_before_draw)
      ticks_per_row += (width + 1u) / 2u;
    if (m_GPUSTAT.SkipDrawingToActiveField())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(ticks_per_row * height);
  }
  ALWAYS_INLINE void AddDrawLineTicks(u32 width, u32 height, bool shaded)
  {
    if (m_GPUSTAT.SkipDrawingToActiveField())
      height = std::max<u32>(height / 2, 1u);

    AddCommandTicks(std::max(width, height));
  }

  std::unique_ptr<TimingEvent> m_crtc_tick_event;
  std::unique_ptr<TimingEvent> m_command_tick_event;

  union GPUSTAT
  {
    u32 bits;
    BitField<u32, u8, 0, 4> texture_page_x_base;
    BitField<u32, u8, 4, 1> texture_page_y_base;
    BitField<u32, GPUTransparencyMode, 5, 2> semi_transparency_mode;
    BitField<u32, GPUTextureMode, 7, 2> texture_color_mode;
    BitField<u32, bool, 9, 1> dither_enable;
    BitField<u32, bool, 10, 1> draw_to_displayed_field;
    BitField<u32, bool, 11, 1> set_mask_while_drawing;
    BitField<u32, bool, 12, 1> check_mask_before_draw;
    BitField<u32, u8, 13, 1> interlaced_field;
    BitField<u32, bool, 14, 1> reverse_flag;
    BitField<u32, bool, 15, 1> texture_disable;
    BitField<u32, u8, 16, 1> horizontal_resolution_2;
    BitField<u32, u8, 17, 2> horizontal_resolution_1;
    BitField<u32, bool, 19, 1> vertical_resolution;
    BitField<u32, bool, 20, 1> pal_mode;
    BitField<u32, bool, 21, 1> display_area_color_depth_24;
    BitField<u32, bool, 22, 1> vertical_interlace;
    BitField<u32, bool, 23, 1> display_disable;
    BitField<u32, bool, 24, 1> interrupt_request;
    BitField<u32, bool, 25, 1> dma_data_request;
    BitField<u32, bool, 26, 1> gpu_idle;
    BitField<u32, bool, 27, 1> ready_to_send_vram;
    BitField<u32, bool, 28, 1> ready_to_recieve_dma;
    BitField<u32, DMADirection, 29, 2> dma_direction;
    BitField<u32, bool, 31, 1> display_line_lsb;

    ALWAYS_INLINE bool IsMaskingEnabled() const
    {
      static constexpr u32 MASK = ((1 << 11) | (1 << 12));
      return ((bits & MASK) != 0);
    }
    ALWAYS_INLINE bool SkipDrawingToActiveField() const
    {
      static constexpr u32 MASK = (1 << 19) | (1 << 22) | (1 << 10);
      static constexpr u32 ACTIVE = (1 << 19) | (1 << 22);
      return ((bits & MASK) == ACTIVE);
    }
    ALWAYS_INLINE bool InInterleaved480iMode() const
    {
      static constexpr u32 ACTIVE = (1 << 19) | (1 << 22);
      return ((bits & ACTIVE) == ACTIVE);
    }

    // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
    ALWAYS_INLINE u16 GetMaskAND() const
    {
      // return check_mask_before_draw ? 0x8000 : 0x0000;
      return Truncate16((bits << 3) & 0x8000);
    }
    ALWAYS_INLINE u16 GetMaskOR() const
    {
      // return set_mask_while_drawing ? 0x8000 : 0x0000;
      return Truncate16((bits << 4) & 0x8000);
    }
  } m_GPUSTAT = {};

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
    bool texture_window_changed;

    ALWAYS_INLINE bool IsTexturePageChanged() const { return texture_page_changed; }
    ALWAYS_INLINE void SetTexturePageChanged() { texture_page_changed = true; }
    ALWAYS_INLINE void ClearTexturePageChangedFlag() { texture_page_changed = false; }

    ALWAYS_INLINE bool IsTextureWindowChanged() const { return texture_window_changed; }
    ALWAYS_INLINE void SetTextureWindowChanged() { texture_window_changed = true; }
    ALWAYS_INLINE void ClearTextureWindowChangedFlag() { texture_window_changed = false; }
  } m_draw_mode = {};

  Common::Rectangle<u32> m_drawing_area{0, 0, VRAM_WIDTH, VRAM_HEIGHT};

  struct DrawingOffset
  {
    s32 x;
    s32 y;
  } m_drawing_offset = {};

  bool m_console_is_pal = false;
  bool m_set_texture_disable_mask = false;
  bool m_drawing_area_changed = false;
  bool m_force_progressive_scan = false;
  bool m_force_ntsc_timings = false;

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

    u16 horizontal_total;
    u16 horizontal_sync_start; // <- not currently saved to state, so we don't have to bump the version
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
  } m_crtc_state = {};

  BlitterState m_blitter_state = BlitterState::Idle;
  u32 m_command_total_words = 0;
  TickCount m_pending_command_ticks = 0;

  /// GPUREAD value for non-VRAM-reads.
  u32 m_GPUREAD_latch = 0;

  /// True if currently executing/syncing.
  bool m_syncing = false;
  bool m_fifo_pushed = false;

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

  ALWAYS_INLINE u32 FifoPop() { return Truncate32(m_fifo.Pop()); }
  ALWAYS_INLINE u32 FifoPeek() { return Truncate32(m_fifo.Peek()); }
  ALWAYS_INLINE u32 FifoPeek(u32 i) { return Truncate32(m_fifo.Peek(i)); }

  TickCount m_max_run_ahead = 128;
  u32 m_fifo_size = 128;

  void ClearDisplayTexture();
  void SetDisplayTexture(GPUTexture* texture, s32 view_x, s32 view_y, s32 view_width, s32 view_height);
  void SetDisplayTextureRect(s32 view_x, s32 view_y, s32 view_width, s32 view_height);
  void SetDisplayParameters(s32 display_width, s32 display_height, s32 active_left, s32 active_top, s32 active_width,
                            s32 active_height, float display_aspect_ratio);

  Common::Rectangle<float> CalculateDrawRect(s32 window_width, s32 window_height, float* out_left_padding,
                                             float* out_top_padding, float* out_scale, float* out_x_scale,
                                             bool apply_aspect_ratio = true) const;

  bool RenderDisplay(GPUTexture* target, const Common::Rectangle<s32>& draw_rect, bool postfx);

  s32 m_display_width = 0;
  s32 m_display_height = 0;
  s32 m_display_active_left = 0;
  s32 m_display_active_top = 0;
  s32 m_display_active_width = 0;
  s32 m_display_active_height = 0;
  float m_display_aspect_ratio = 1.0f;

  std::unique_ptr<GPUPipeline> m_display_pipeline;
  GPUTexture* m_display_texture = nullptr;
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
    u32 host_num_render_passes;
    u32 host_num_copies;
    u32 host_num_downloads;
    u32 host_num_uploads;
  };

  Counters m_counters = {};
  Stats m_stats = {};

private:
  bool CompileDisplayPipeline();

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
