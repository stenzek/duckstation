// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu.h"
#include "core.h"
#include "cpu_pgxp.h"
#include "dma.h"
#include "gpu_backend.h"
#include "gpu_dump.h"
#include "gpu_helpers.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_sw_rasterizer.h"
#include "host.h"
#include "interrupt_controller.h"
#include "performance_counters.h"
#include "settings.h"
#include "system.h"
#include "system_private.h"
#include "timers.h"
#include "timing_event.h"
#include "video_shadergen.h"
#include "video_thread.h"
#include "video_thread_commands.h"

#include "util/gpu_device.h"
#include "util/image.h"
#include "util/imgui_manager.h"
#include "util/media_capture.h"
#include "util/postprocessing.h"
#include "util/shadergen.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitfield.h"
#include "common/error.h"
#include "common/fifo_queue.h"
#include "common/file_system.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "IconsEmoji.h"
#include "fmt/format.h"
#include "imgui.h"

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

LOG_CHANNEL(GPU);

namespace GPU {

namespace {
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
  NTSC_OVERSCAN_HORIZONTAL_ACTIVE_START = 608,
  NTSC_OVERSCAN_HORIZONTAL_ACTIVE_END = 3168,
  NTSC_OVERSCAN_VERTICAL_ACTIVE_START = 24,
  NTSC_OVERSCAN_VERTICAL_ACTIVE_END = 248,
  PAL_HORIZONTAL_ACTIVE_START = 488,
  PAL_HORIZONTAL_ACTIVE_END = 3300,
  PAL_VERTICAL_ACTIVE_START = 20,
  PAL_VERTICAL_ACTIVE_END = 308,
  PAL_OVERSCAN_HORIZONTAL_ACTIVE_START = 628,
  PAL_OVERSCAN_HORIZONTAL_ACTIVE_END = 3188,
  PAL_OVERSCAN_VERTICAL_ACTIVE_START = 30,
  PAL_OVERSCAN_VERTICAL_ACTIVE_END = 298,
};

} // namespace

/// Returns true if no data is being sent from VRAM to the DAC or that no portion of VRAM would be visible on screen.
static bool IsDisplayDisabled();

/// Returns true if interlaced rendering is enabled and force progressive scan is disabled.
static bool IsInterlacedRenderingEnabled();

/// Returns the number of pending GPU ticks.
TickCount GetPendingCRTCTicks();
TickCount GetPendingCommandTicks();

/// Returns true if a raster scanline or command execution is pending.
static bool IsCommandCompletionPending();

static float ComputeHorizontalFrequency();
static float ComputeVerticalFrequency();

// Ticks for hblank/vblank.
static void CRTCTickEvent(void*, TickCount ticks);
static void CommandTickEvent(void*, TickCount ticks);
static void FrameDoneEvent(void*, TickCount ticks);

// The GPU internally appears to run at 2x the system clock.
// TODO: No, it just draws two pixels per clock.
ALWAYS_INLINE static constexpr TickCount GPUTicksToSystemTicks(TickCount gpu_ticks)
{
  return std::max<TickCount>((gpu_ticks + 1) >> 1, 1);
}
ALWAYS_INLINE static constexpr TickCount SystemTicksToGPUTicks(TickCount sysclk_ticks)
{
  return sysclk_ticks << 1;
}

static TickCount CRTCTicksToSystemTicks(TickCount crtc_ticks, TickCount fractional_ticks);
static TickCount SystemTicksToCRTCTicks(TickCount sysclk_ticks, TickCount* fractional_ticks);

static bool DumpVRAMToFile(std::string path, u32 width, u32 height, u32 stride, const void* buffer, bool remove_alpha,
                           Error* error = nullptr);

static void SoftReset();
static void ClearDisplay();

// Sets dots per scanline
static void UpdateCRTCConfig();
static void UpdateCRTCDisplayParameters();
static void UpdateCRTCHBlankFlag();

// Update ticks for this execution slice
static void UpdateCRTCTickEvent();
static void UpdateCommandTickEvent();
static u8 UpdateOrGetGPUBusyPct();

// Updates dynamic bits in GPUSTAT (ready to send VRAM/ready to receive DMA)
static void UpdateDMARequest();
static void UpdateGPUIdle();

/// Updates drawing area that's suitable for clamping.
static void SetClampedDrawingArea();

/// Sets/decodes GP0(E1h) (set draw mode).
static void SetDrawMode(u16 bits);

/// Sets/decodes polygon/rectangle texture palette value.
static void SetTexturePalette(u16 bits);

/// Sets/decodes texture window bits.
static void SetTextureWindow(u32 value);

static u32 ReadGPUREAD();
static void FinishVRAMWrite();

/// Returns the number of vertices in the buffered poly-line.
static u32 GetPolyLineVertexCount();

static void AddCommandTicks(TickCount ticks);

static u32 FifoPop();
static u32 FifoPeek();
static u32 FifoPeek(u32 i);

static void WriteGP1(u32 value);
static void EndCommand();
static void ExecuteCommands();
static void TryExecuteCommands();
static void HandleGetGPUInfoCommand(u32 value);
static void UpdateCLUTIfNeeded(GPUTextureMode texmode, GPUTexturePaletteReg clut);
static void InvalidateCLUT();

static void ReadVRAM(u16 x, u16 y, u16 width, u16 height);
static void UpdateVRAM(u16 x, u16 y, u16 width, u16 height, const void* data, bool set_mask, bool check_mask);

static void PrepareForDraw();
static void FinishPolyline();
static void FillDrawCommand(GPUBackendDrawCommand* RESTRICT cmd, GPURenderCommand rc);

static void AddDrawTriangleTicks(GSVector2i v1, GSVector2i v2, GSVector2i v3, bool shaded, bool textured,
                                 bool semitransparent);
static void AddDrawRectangleTicks(const GSVector4i rect, bool textured, bool semitransparent);
static void AddDrawLineTicks(const GSVector4i rect, bool shaded);

using GP0CommandHandler = bool (*)();
using GP0CommandHandlerTable = std::array<GP0CommandHandler, 256>;

// Rendering commands, returns false if not enough data is provided
static bool HandleUnknownGP0Command();
static bool HandleNOPCommand();
static bool HandleClearCacheCommand();
static bool HandleInterruptRequestCommand();
static bool HandleSetDrawModeCommand();
static bool HandleSetTextureWindowCommand();
static bool HandleSetDrawingAreaTopLeftCommand();
static bool HandleSetDrawingAreaBottomRightCommand();
static bool HandleSetDrawingOffsetCommand();
static bool HandleSetMaskBitCommand();
static bool HandleRenderPolygonCommand();
static bool HandleRenderRectangleCommand();
static bool HandleRenderLineCommand();
static bool HandleRenderPolyLineCommand();
static bool HandleFillRectangleCommand();
static bool HandleCopyRectangleCPUToVRAMCommand();
static bool HandleCopyRectangleVRAMToCPUCommand();
static bool HandleCopyRectangleVRAMToVRAMCommand();

namespace {
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
};

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

  u16 current_scanline;
  TickCount fractional_ticks;
  TickCount current_tick_in_scanline;

  TickCount fractional_dot_ticks; // only used when timer0 is enabled

  bool in_hblank;
  bool in_vblank;

  /// 0 if the currently-displayed field is on odd lines (1,3,5,...) or 1 if even (2,4,6,...)
  u8 interlaced_field;
  u8 interlaced_display_field;

  /// 0 if the currently-displayed field is on an even line in VRAM, otherwise 1.
  u8 active_line_lsb;
};

struct Locals
{
  TimingEvent crtc_tick_event{"GPU CRTC Tick", 1, 1, &GPU::CRTCTickEvent, nullptr};
  TimingEvent command_tick_event{"GPU Command Tick", 1, 1, &GPU::CommandTickEvent, nullptr};
  TimingEvent frame_done_event{"Frame Done", 1, 1, &GPU::FrameDoneEvent, nullptr};

  GPUSTATReg GPUSTAT = {};

  bool console_is_pal = false;
  bool set_texture_disable_mask = false;
  bool drawing_area_changed = false;
  bool force_progressive_scan = false;

  DrawMode draw_mode = {};

  GPUDrawingArea drawing_area = {};
  GPUDrawingOffset drawing_offset = {};

  GSVector4i clamped_drawing_area = {};

  CRTCState crtc_state = {};

  u32 command_total_words = 0;
  TickCount pending_command_ticks = 0;
  u32 active_ticks_since_last_update = 0;

  /// True if currently executing/syncing.
  bool executing_commands = false;
  BlitterState blitter_state = BlitterState::Idle;

  struct VRAMTransfer
  {
    u16 x;
    u16 y;
    u16 width;
    u16 height;
    u16 col;
    u16 row;
  } vram_transfer = {};

  // One byte free, store the GPU usage here.
  u8 last_gpu_busy_pct = 0;

  // These are the bits from the palette register, but zero extended to 32-bit, so we can have an "invalid" value.
  // If an extra byte is ever not needed here for padding, the 8-bit flag could be packed into the MSB of this value.
  bool current_clut_is_8bit = false;
  u32 current_clut_reg_bits = {};

  /// GPUREAD value for non-VRAM-reads.
  u32 GPUREAD_latch = 0;

  std::unique_ptr<GPUDump::Recorder> gpu_dump;

  HeapFIFOQueue<u64, MAX_FIFO_SIZE> fifo;
  TickCount max_run_ahead = 128;
  u32 fifo_size = 128;
  u32 blit_remaining_words;
  GPURenderCommand render_command{};
  std::vector<u32> blit_buffer;
  std::vector<u64> polyline_buffer;

  u32 cpu_to_vram_dump_id = 0;
  u32 vram_to_cpu_dump_id = 0;
};
} // namespace

ALIGN_TO_CACHE_LINE static Locals s_locals;

} // namespace GPU

// aligning VRAM to 4K is fine, since the ARM64 instructions compute 4K page aligned addresses
// or it would be, except we want to import the memory for readbacks on metal..
#ifdef DYNAMIC_HOST_PAGE_SIZE
#define VRAM_STORAGE_ALIGNMENT MIN_HOST_PAGE_SIZE
#else
#define VRAM_STORAGE_ALIGNMENT HOST_PAGE_SIZE
#endif
alignas(VRAM_STORAGE_ALIGNMENT) u16 g_vram[VRAM_SIZE / sizeof(u16)];
u16 g_gpu_clut[GPU_CLUT_SIZE];

void GPU::Initialize()
{
  if (!System::IsReplayingGPUDump())
    s_locals.crtc_tick_event.Activate();

  s_locals.force_progressive_scan = (g_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive);
  s_locals.fifo_size = g_settings.gpu_fifo_size;
  s_locals.max_run_ahead = g_settings.gpu_max_run_ahead;
  s_locals.console_is_pal = System::IsPALRegion();
  UpdateCRTCConfig();
}

void GPU::Shutdown()
{
  s_locals.command_tick_event.Deactivate();
  s_locals.crtc_tick_event.Deactivate();
  s_locals.frame_done_event.Deactivate();

  StopRecordingGPUDump();
}

void GPU::UpdateSettings(const Settings& old_settings)
{
  s_locals.force_progressive_scan = (g_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive);
  s_locals.fifo_size = g_settings.gpu_fifo_size;
  s_locals.max_run_ahead = g_settings.gpu_max_run_ahead;

  if (g_settings.gpu_force_video_timing != old_settings.gpu_force_video_timing)
  {
    s_locals.console_is_pal = System::IsPALRegion();
    UpdateCRTCConfig();
  }
  else if (g_settings.display_crop_mode != old_settings.display_crop_mode ||
           g_settings.display_active_start_offset != old_settings.display_active_start_offset ||
           g_settings.display_active_end_offset != old_settings.display_active_end_offset ||
           g_settings.display_line_start_offset != old_settings.display_line_start_offset ||
           g_settings.display_line_end_offset != old_settings.display_line_end_offset)
  {
    // Crop mode calls this, so recalculate the display area
    UpdateCRTCDisplayParameters();
  }
}

void GPU::CPUClockChanged()
{
  UpdateCRTCConfig();
}

std::pair<u32, u32> GPU::GetFullDisplayResolution()
{
  u32 width, height;
  if (IsDisplayDisabled())
  {
    width = 0;
    height = 0;
  }
  else
  {
    s32 xmin, xmax, ymin, ymax;
    if (!s_locals.GPUSTAT.pal_mode)
    {
      xmin = NTSC_HORIZONTAL_ACTIVE_START;
      xmax = NTSC_HORIZONTAL_ACTIVE_END;
      ymin = NTSC_VERTICAL_ACTIVE_START;
      ymax = NTSC_VERTICAL_ACTIVE_END;
    }
    else
    {
      xmin = PAL_HORIZONTAL_ACTIVE_START;
      xmax = PAL_HORIZONTAL_ACTIVE_END;
      ymin = PAL_VERTICAL_ACTIVE_START;
      ymax = PAL_VERTICAL_ACTIVE_END;
    }

    width = static_cast<u32>(std::max<s32>(std::clamp<s32>(s_locals.crtc_state.regs.X2, xmin, xmax) -
                                             std::clamp<s32>(s_locals.crtc_state.regs.X1, xmin, xmax),
                                           0) /
                             s_locals.crtc_state.dot_clock_divider);
    height = static_cast<u32>(std::max<s32>(std::clamp<s32>(s_locals.crtc_state.regs.Y2, ymin, ymax) -
                                              std::clamp<s32>(s_locals.crtc_state.regs.Y1, ymin, ymax),
                                            0))
             << BoolToUInt8(s_locals.GPUSTAT.vertical_interlace);
  }

  return std::make_pair(width, height);
}

void GPU::Reset(bool clear_vram)
{
  s_locals.GPUSTAT.bits = 0x14802000;
  s_locals.set_texture_disable_mask = false;
  s_locals.GPUREAD_latch = 0;
  s_locals.crtc_state.fractional_ticks = 0;
  s_locals.crtc_state.fractional_dot_ticks = 0;
  s_locals.crtc_state.current_tick_in_scanline = 0;
  s_locals.crtc_state.current_scanline = 0;
  s_locals.crtc_state.in_hblank = false;
  s_locals.crtc_state.in_vblank = false;
  s_locals.crtc_state.interlaced_field = 0;
  s_locals.crtc_state.interlaced_display_field = 0;

  // Cancel VRAM writes.
  s_locals.blitter_state = BlitterState::Idle;
  s_locals.active_ticks_since_last_update = 0;

  // Force event to reschedule itself.
  s_locals.crtc_tick_event.Deactivate();
  s_locals.command_tick_event.Deactivate();

  SoftReset();

  // Can skip the VRAM clear if it's not a hardware reset.
  if (clear_vram)
    GPUBackend::PushCommand(GPUBackend::NewClearVRAMCommand());
}

void GPU::SoftReset()
{
  if (s_locals.blitter_state == BlitterState::WritingVRAM)
    FinishVRAMWrite();

  s_locals.GPUSTAT.texture_page_x_base = 0;
  s_locals.GPUSTAT.texture_page_y_base = 0;
  s_locals.GPUSTAT.semi_transparency_mode = GPUTransparencyMode::HalfBackgroundPlusHalfForeground;
  s_locals.GPUSTAT.texture_color_mode = GPUTextureMode::Palette4Bit;
  s_locals.GPUSTAT.dither_enable = false;
  s_locals.GPUSTAT.draw_to_displayed_field = false;
  s_locals.GPUSTAT.set_mask_while_drawing = false;
  s_locals.GPUSTAT.check_mask_before_draw = false;
  s_locals.GPUSTAT.reverse_flag = false;
  s_locals.GPUSTAT.texture_disable = false;
  s_locals.GPUSTAT.horizontal_resolution_2 = 0;
  s_locals.GPUSTAT.horizontal_resolution_1 = 0;
  s_locals.GPUSTAT.vertical_resolution = false;
  s_locals.GPUSTAT.pal_mode = System::IsPALRegion();
  s_locals.GPUSTAT.display_area_color_depth_24 = false;
  s_locals.GPUSTAT.vertical_interlace = false;
  s_locals.GPUSTAT.display_disable = true;
  s_locals.GPUSTAT.dma_direction = GPUDMADirection::Off;
  s_locals.drawing_area = {};
  s_locals.drawing_area_changed = true;
  s_locals.drawing_offset = {};
  std::memset(&s_locals.crtc_state.regs, 0, sizeof(s_locals.crtc_state.regs));
  s_locals.crtc_state.regs.horizontal_display_range = 0xC60260;
  s_locals.crtc_state.regs.vertical_display_range = 0x3FC10;
  s_locals.blitter_state = BlitterState::Idle;
  s_locals.pending_command_ticks = 0;
  s_locals.command_total_words = 0;
  s_locals.vram_transfer = {};
  s_locals.fifo.Clear();
  s_locals.blit_buffer.clear();
  s_locals.blit_remaining_words = 0;
  s_locals.draw_mode.texture_window_value = 0xFFFFFFFFu;
  SetDrawMode(0);
  SetTexturePalette(0);
  SetTextureWindow(0);
  InvalidateCLUT();
  UpdateDMARequest();
  UpdateCRTCConfig();
  UpdateCommandTickEvent();
  UpdateGPUIdle();
}

bool GPU::DoState(StateWrapper& sw)
{
  if (sw.IsWriting())
  {
    // Need to ensure our copy of VRAM is good.
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  }

  sw.Do(&s_locals.GPUSTAT.bits);

  sw.Do(&s_locals.draw_mode.mode_reg.bits);
  sw.Do(&s_locals.draw_mode.palette_reg.bits);
  sw.Do(&s_locals.draw_mode.texture_window_value);

  if (sw.GetVersion() < 62) [[unlikely]]
  {
    // texture_page_x, texture_page_y, texture_palette_x, texture_palette_y
    DebugAssert(sw.IsReading());
    sw.SkipBytes(sizeof(u32) * 4);
  }

  sw.Do(&s_locals.draw_mode.texture_window.and_x);
  sw.Do(&s_locals.draw_mode.texture_window.and_y);
  sw.Do(&s_locals.draw_mode.texture_window.or_x);
  sw.Do(&s_locals.draw_mode.texture_window.or_y);
  sw.Do(&s_locals.draw_mode.texture_x_flip);
  sw.Do(&s_locals.draw_mode.texture_y_flip);

  sw.Do(&s_locals.drawing_area.left);
  sw.Do(&s_locals.drawing_area.top);
  sw.Do(&s_locals.drawing_area.right);
  sw.Do(&s_locals.drawing_area.bottom);
  sw.Do(&s_locals.drawing_offset.x);
  sw.Do(&s_locals.drawing_offset.y);
  sw.Do(&s_locals.drawing_offset.x);

  sw.Do(&s_locals.console_is_pal);
  sw.Do(&s_locals.set_texture_disable_mask);

  sw.Do(&s_locals.crtc_state.regs.display_address_start);
  sw.Do(&s_locals.crtc_state.regs.horizontal_display_range);
  sw.Do(&s_locals.crtc_state.regs.vertical_display_range);
  sw.Do(&s_locals.crtc_state.dot_clock_divider);
  sw.Do(&s_locals.crtc_state.display_width);
  sw.Do(&s_locals.crtc_state.display_height);
  sw.Do(&s_locals.crtc_state.display_origin_left);
  sw.Do(&s_locals.crtc_state.display_origin_top);
  sw.Do(&s_locals.crtc_state.display_vram_left);
  sw.Do(&s_locals.crtc_state.display_vram_top);
  sw.Do(&s_locals.crtc_state.display_vram_width);
  sw.Do(&s_locals.crtc_state.display_vram_height);
  sw.Do(&s_locals.crtc_state.horizontal_total);
  sw.Do(&s_locals.crtc_state.horizontal_visible_start);
  sw.Do(&s_locals.crtc_state.horizontal_visible_end);
  sw.Do(&s_locals.crtc_state.horizontal_display_start);
  sw.Do(&s_locals.crtc_state.horizontal_display_end);
  sw.Do(&s_locals.crtc_state.vertical_total);
  sw.Do(&s_locals.crtc_state.vertical_visible_start);
  sw.Do(&s_locals.crtc_state.vertical_visible_end);
  sw.Do(&s_locals.crtc_state.vertical_display_start);
  sw.Do(&s_locals.crtc_state.vertical_display_end);
  sw.Do(&s_locals.crtc_state.fractional_ticks);
  sw.Do(&s_locals.crtc_state.current_tick_in_scanline);
  s_locals.crtc_state.current_scanline = Truncate16(sw.DoValue(static_cast<u32>(s_locals.crtc_state.current_scanline)));
  sw.DoEx(&s_locals.crtc_state.fractional_dot_ticks, 46, 0);
  sw.Do(&s_locals.crtc_state.in_hblank);
  sw.Do(&s_locals.crtc_state.in_vblank);
  sw.Do(&s_locals.crtc_state.interlaced_field);
  sw.Do(&s_locals.crtc_state.interlaced_display_field);
  sw.Do(&s_locals.crtc_state.active_line_lsb);

  sw.Do(&s_locals.blitter_state);
  sw.Do(&s_locals.pending_command_ticks);
  sw.Do(&s_locals.command_total_words);
  sw.Do(&s_locals.GPUREAD_latch);

  u16 load_clut_data[GPU_CLUT_SIZE];
  if (sw.GetVersion() < 64) [[unlikely]]
  {
    // Clear CLUT cache and let it populate later.
    InvalidateCLUT();
    std::memset(load_clut_data, 0, sizeof(load_clut_data));
  }
  else
  {
    sw.Do(&s_locals.current_clut_reg_bits);
    sw.Do(&s_locals.current_clut_is_8bit);

    // I hate this extra copy... because I'm a moron and put it in the middle of the state data.
    sw.DoArray(sw.IsReading() ? load_clut_data : g_gpu_clut, std::size(g_gpu_clut));
  }

  sw.Do(&s_locals.vram_transfer.x);
  sw.Do(&s_locals.vram_transfer.y);
  sw.Do(&s_locals.vram_transfer.width);
  sw.Do(&s_locals.vram_transfer.height);
  sw.Do(&s_locals.vram_transfer.col);
  sw.Do(&s_locals.vram_transfer.row);

  sw.Do(&s_locals.fifo);
  sw.Do(&s_locals.blit_buffer);
  sw.Do(&s_locals.blit_remaining_words);
  sw.Do(&s_locals.render_command.bits);

  if (sw.GetVersion() < 83) [[unlikely]]
  {
    // Removed in v83
    DebugAssert(sw.IsReading());
    sw.SkipBytes(sizeof(u32) * 2);
  }

  if (!sw.DoMarker("GPU-VRAM"))
    return false;

  if (sw.IsReading())
  {
    // Need to calculate the TC data size. But skip over VRAM first, we'll grab it later.
    const size_t vram_start_pos = sw.GetPosition();
    sw.SkipBytes(VRAM_SIZE);
    u32 tc_data_size;
    if (!GPUTextureCache::GetStateSize(sw, &tc_data_size)) [[unlikely]]
      return false;

    // Now we can actually allocate FIFO storage, and push it to the GPU thread.
    GPUBackendLoadStateCommand* cmd = static_cast<GPUBackendLoadStateCommand*>(VideoThread::AllocateCommand(
      VideoThreadCommandType::LoadState, sizeof(GPUBackendLoadStateCommand) + tc_data_size));
    std::memcpy(cmd->clut_data, load_clut_data, sizeof(cmd->clut_data));
    std::memcpy(cmd->vram_data, sw.GetData() + vram_start_pos, VRAM_SIZE);
    cmd->texture_cache_state_version = sw.GetVersion();
    cmd->texture_cache_state_size = tc_data_size;
    if (tc_data_size > 0)
      std::memcpy(cmd->texture_cache_state, sw.GetData() + vram_start_pos + VRAM_SIZE, tc_data_size);
    VideoThread::PushCommand(cmd);

    s_locals.drawing_area_changed = true;
    SetClampedDrawingArea();
    UpdateDMARequest();
    UpdateCRTCConfig();
    UpdateCommandTickEvent();
  }
  else // if not memory state
  {
    // write vram
    sw.DoBytes(g_vram, VRAM_SIZE);

    // write TC data, we have to be super careful here, since we're reading GPU thread state...
    GPUTextureCache::DoState(sw, false);
  }

  return !sw.HasError();
}

void GPU::DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss)
{
  sw.Do(&s_locals.GPUSTAT.bits);

  sw.DoBytes(&s_locals.draw_mode, sizeof(s_locals.draw_mode));
  sw.DoBytes(&s_locals.drawing_area, sizeof(s_locals.drawing_area));
  sw.DoBytes(&s_locals.drawing_offset, sizeof(s_locals.drawing_offset));

  sw.Do(&s_locals.console_is_pal);
  sw.Do(&s_locals.set_texture_disable_mask);

  sw.DoBytes(&s_locals.crtc_state, sizeof(s_locals.crtc_state));

  sw.Do(&s_locals.blitter_state);
  sw.Do(&s_locals.pending_command_ticks);
  sw.Do(&s_locals.command_total_words);
  sw.Do(&s_locals.GPUREAD_latch);

  sw.Do(&s_locals.current_clut_reg_bits);
  sw.Do(&s_locals.current_clut_is_8bit);

  sw.DoBytes(&s_locals.vram_transfer, sizeof(s_locals.vram_transfer));

  sw.Do(&s_locals.fifo);
  sw.Do(&s_locals.blit_buffer);
  sw.Do(&s_locals.blit_remaining_words);
  sw.Do(&s_locals.render_command.bits);

  if (sw.IsReading())
  {
    s_locals.drawing_area_changed = true;
    SetClampedDrawingArea();
    UpdateDMARequest();
    UpdateCRTCConfig();
    UpdateCommandTickEvent();
  }

  // Push to thread.
  GPUBackendDoMemoryStateCommand* cmd = static_cast<GPUBackendDoMemoryStateCommand*>(VideoThread::AllocateCommand(
    sw.IsReading() ? VideoThreadCommandType::LoadMemoryState : VideoThreadCommandType::SaveMemoryState,
    sizeof(GPUBackendDoMemoryStateCommand)));
  cmd->memory_save_state = &mss;
  VideoThread::PushCommandAndWakeThread(cmd);
}

void GPU::UpdateDMARequest()
{
  switch (s_locals.blitter_state)
  {
    case BlitterState::Idle:
      s_locals.GPUSTAT.ready_to_send_vram = false;
      s_locals.GPUSTAT.ready_to_receive_dma =
        (s_locals.fifo.IsEmpty() || s_locals.fifo.GetSize() < s_locals.command_total_words);
      break;

    case BlitterState::WritingVRAM:
      s_locals.GPUSTAT.ready_to_send_vram = false;
      s_locals.GPUSTAT.ready_to_receive_dma = (s_locals.fifo.GetSize() < s_locals.fifo_size);
      break;

    case BlitterState::ReadingVRAM:
      s_locals.GPUSTAT.ready_to_send_vram = true;
      s_locals.GPUSTAT.ready_to_receive_dma = false;
      break;

    case BlitterState::DrawingPolyLine:
      s_locals.GPUSTAT.ready_to_send_vram = false;
      s_locals.GPUSTAT.ready_to_receive_dma = (s_locals.fifo.GetSize() < s_locals.fifo_size);
      break;

    default:
      UnreachableCode();
      break;
  }

  bool dma_request;
  switch (s_locals.GPUSTAT.dma_direction)
  {
    case GPUDMADirection::Off:
      dma_request = false;
      break;

    case GPUDMADirection::FIFO:
      dma_request = s_locals.GPUSTAT.ready_to_receive_dma;
      break;

    case GPUDMADirection::CPUtoGP0:
      dma_request = s_locals.GPUSTAT.ready_to_receive_dma;
      break;

    case GPUDMADirection::GPUREADtoCPU:
      dma_request = s_locals.GPUSTAT.ready_to_send_vram;
      break;

    default:
      dma_request = false;
      break;
  }
  s_locals.GPUSTAT.dma_data_request = dma_request;
  DMA::SetRequest(DMA::Channel::GPU, dma_request);
}

void GPU::UpdateGPUIdle()
{
  s_locals.GPUSTAT.gpu_idle =
    (s_locals.blitter_state == BlitterState::Idle && s_locals.pending_command_ticks <= 0 && s_locals.fifo.IsEmpty());
}

u32 GPU::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00:
      return ReadGPUREAD();

    case 0x04:
    {
      // code can be dependent on the odd/even bit, so update the GPU state when reading.
      // we can mitigate this slightly by only updating when the raster is actually hitting a new line
      if (IsCRTCScanlinePending())
        SynchronizeCRTC();
      if (IsCommandCompletionPending())
        s_locals.command_tick_event.InvokeEarly();

      return s_locals.GPUSTAT.bits;
    }

    default:
      ERROR_LOG("Unhandled register read: {:02X}", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void GPU::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00:
    {
      if (s_locals.gpu_dump) [[unlikely]]
        s_locals.gpu_dump->WriteGP0Packet(value);

      // FIFO can be overflowed through direct GP0 writes if the command tick event hasn't run, because
      // there's no backpressure applied to the CPU. Instead force the GPU to run and catch up.
      if (s_locals.fifo.GetSize() >= s_locals.fifo_size) [[unlikely]]
      {
        s_locals.command_tick_event.InvokeEarly();

        if (s_locals.fifo.GetSize() >= s_locals.fifo.GetCapacity()) [[unlikely]]
        {
          WARNING_LOG("GPU FIFO overflow via GP0 write, size={}", s_locals.fifo.GetSize());
          return;
        }
      }

      s_locals.fifo.Push(value);
      ExecuteCommands();
      return;
    }

    case 0x04:
    {
      if (s_locals.gpu_dump) [[unlikely]]
        s_locals.gpu_dump->WriteGP1Packet(value);

      WriteGP1(value);
      return;
    }

    default:
    {
      ERROR_LOG("Unhandled register write: {:02X} <- {:08X}", offset, value);
      return;
    }
  }
}

void GPU::DMARead(u32* words, u32 word_count)
{
  if (s_locals.GPUSTAT.dma_direction != GPUDMADirection::GPUREADtoCPU)
  {
    ERROR_LOG("Invalid DMA direction from GPU DMA read");
    std::fill_n(words, word_count, UINT32_C(0xFFFFFFFF));
    return;
  }

  for (u32 i = 0; i < word_count; i++)
    words[i] = ReadGPUREAD();
}

bool GPU::BeginDMAWrite()
{
  return (s_locals.GPUSTAT.dma_direction == GPUDMADirection::CPUtoGP0 ||
          s_locals.GPUSTAT.dma_direction == GPUDMADirection::FIFO);
}

void GPU::DMAWrite(u32 address, u32 value)
{
  s_locals.fifo.Push((ZeroExtend64(address) << 32) | ZeroExtend64(value));
}

void GPU::EndDMAWrite()
{
  ExecuteCommands();
}

/**
 * NTSC GPU clock 53.693175 MHz
 * PAL GPU clock 53.203425 MHz
 * courtesy of @ggrtk
 *
 * NTSC - sysclk * 715909 / 451584
 * PAL - sysclk * 709379 / 451584
 */

TickCount GPU::GetCRTCFrequency()
{
  return s_locals.console_is_pal ? 53203425 : 53693175;
}

TickCount GPU::CRTCTicksToSystemTicks(TickCount gpu_ticks, TickCount fractional_ticks)
{
  // convert to master clock, rounding up as we want to overshoot not undershoot
  if (!s_locals.console_is_pal)
    return static_cast<TickCount>((u64(gpu_ticks) * u64(451584) + fractional_ticks + u64(715908)) / u64(715909));
  else
    return static_cast<TickCount>((u64(gpu_ticks) * u64(451584) + fractional_ticks + u64(709378)) / u64(709379));
}

TickCount GPU::SystemTicksToCRTCTicks(TickCount sysclk_ticks, TickCount* fractional_ticks)
{
  u64 mul = u64(sysclk_ticks);
  mul *= !s_locals.console_is_pal ? u64(715909) : u64(709379);
  mul += u64(*fractional_ticks);

  const TickCount ticks = static_cast<TickCount>(mul / u64(451584));
  *fractional_ticks = static_cast<TickCount>(mul % u64(451584));
  return ticks;
}

void GPU::AddCommandTicks(TickCount ticks)
{
  s_locals.pending_command_ticks += ticks;
  s_locals.active_ticks_since_last_update += ticks;
}

void GPU::SynchronizeCRTC()
{
  s_locals.crtc_tick_event.InvokeEarly();
}

float GPU::ComputeHorizontalFrequency()
{
  const CRTCState& cs = s_locals.crtc_state;
  TickCount fractional_ticks = 0;
  return static_cast<float>(
    static_cast<double>(SystemTicksToCRTCTicks(System::GetTicksPerSecond(), &fractional_ticks)) /
    static_cast<double>(cs.horizontal_total));
}

float GPU::ComputeVerticalFrequency()
{
  const CRTCState& cs = s_locals.crtc_state;
  const TickCount ticks_per_frame = cs.horizontal_total * cs.vertical_total;
  TickCount fractional_ticks = 0;
  return static_cast<float>(
    static_cast<double>(SystemTicksToCRTCTicks(System::GetTicksPerSecond(), &fractional_ticks)) /
    static_cast<double>(ticks_per_frame));
}

float GPU::ComputePixelAspectRatio()
{
  float sar =
    (s_locals.crtc_state.display_width > 0 && s_locals.crtc_state.display_height > 0) ?
      static_cast<float>(s_locals.crtc_state.display_width) / static_cast<float>(s_locals.crtc_state.display_height) :
      1.0f;

  // Force 4:3 for 24-bit modes option.
  const DisplayAspectRatio dar_type =
    (!g_settings.display_force_4_3_for_24bit || !s_locals.GPUSTAT.display_area_color_depth_24) ?
      g_settings.display_aspect_ratio :
      DisplayAspectRatio::Auto();
  float dar = 4.0f / 3.0f;
  if (dar_type == DisplayAspectRatio::PAR1_1())
  {
    dar = sar;
  }
  else if (dar_type == DisplayAspectRatio::Stretch())
  {
    const WindowInfo& wi = VideoThread::GetRenderWindowInfo();
    if (!wi.IsSurfaceless() && wi.surface_width > 0 && wi.surface_height > 0)
    {
      // Correction is applied to the GTE for stretch to fit, that way it fills the window.
      dar = static_cast<float>(wi.surface_width) / static_cast<float>(wi.surface_height);
    }
  }
  else
  {
    sar /= ComputeAspectRatioCorrection();
    if (dar_type != DisplayAspectRatio::Auto())
    {
      dar = static_cast<float>(g_settings.display_aspect_ratio.numerator) /
            static_cast<float>(g_settings.display_aspect_ratio.denominator);
    }
  }

  return (dar / sar);
}

float GPU::ComputeAspectRatioCorrection()
{
  const CRTCState& cs = s_locals.crtc_state;
  float relative_width = static_cast<float>(cs.horizontal_visible_end - cs.horizontal_visible_start);
  float relative_height = static_cast<float>(cs.vertical_visible_end - cs.vertical_visible_start);
  if (relative_width <= 0 || relative_height <= 0 || g_settings.display_aspect_ratio == DisplayAspectRatio::PAR1_1())
    return 1.0f;

  // Apply aspect ratio correction for all borders, or overscan with altered display range.
  // That way if cropping is performed, the original aspect ratio is maintained.
  switch (g_settings.display_crop_mode)
  {
    case DisplayCropMode::Borders:
    case DisplayCropMode::None:
    {
      if (s_locals.GPUSTAT.pal_mode)
      {
        relative_width /= static_cast<float>(PAL_HORIZONTAL_ACTIVE_END - PAL_HORIZONTAL_ACTIVE_START);
        relative_height /= static_cast<float>(PAL_VERTICAL_ACTIVE_END - PAL_VERTICAL_ACTIVE_START);
      }
      else
      {
        relative_width /= static_cast<float>(NTSC_HORIZONTAL_ACTIVE_END - NTSC_HORIZONTAL_ACTIVE_START);
        relative_height /= static_cast<float>(NTSC_VERTICAL_ACTIVE_END - NTSC_VERTICAL_ACTIVE_START);
      }
    }
    break;

    case DisplayCropMode::Overscan:
    {
      if (s_locals.GPUSTAT.pal_mode)
      {
        relative_width /= static_cast<float>(PAL_OVERSCAN_HORIZONTAL_ACTIVE_END - PAL_OVERSCAN_HORIZONTAL_ACTIVE_START);
        relative_height /= static_cast<float>(PAL_OVERSCAN_VERTICAL_ACTIVE_END - PAL_OVERSCAN_VERTICAL_ACTIVE_START);
      }
      else
      {
        relative_width /=
          static_cast<float>(NTSC_OVERSCAN_HORIZONTAL_ACTIVE_END - NTSC_OVERSCAN_HORIZONTAL_ACTIVE_START);
        relative_height /= static_cast<float>(NTSC_OVERSCAN_VERTICAL_ACTIVE_END - NTSC_OVERSCAN_VERTICAL_ACTIVE_START);
      }
    }
    break;

    case DisplayCropMode::OverscanUncorrected:
    case DisplayCropMode::BordersUncorrected:
    default:
      return 1.0f;
  }

  return (relative_width / relative_height);
}

GSVector2 GPU::CalculateRenderWindowSize(DisplayFineCropMode mode, std::span<const s16, 4> amount,
                                         float pixel_aspect_ratio, const GSVector2 video_size,
                                         const GSVector2 source_size, const GSVector2 window_size)
{
  GSVector2 size = video_size;
  if (pixel_aspect_ratio < 1.0f)
  {
    // stretch height, preserve width
    size.y = size.y / pixel_aspect_ratio;
  }
  else
  {
    // stretch width, preserve height
    size.x = size.x * pixel_aspect_ratio;
  }

  if (mode != DisplayFineCropMode::None)
  {
    GSVector4 crop_amount = GSVector4(GSVector4i::loadl<false>(amount.data()).s16to32());
    switch (mode)
    {
      case DisplayFineCropMode::VideoResolution:
        break;

      case DisplayFineCropMode::InternalResolution:
        crop_amount *= GSVector4::xyxy(size / source_size);
        break;

      case DisplayFineCropMode::WindowResolution:
        crop_amount *= GSVector4::xyxy(size / window_size);
        break;

        DefaultCaseIsUnreachable();
    }

    size = (size - crop_amount.xy() - crop_amount.zw()).max(GSVector2::cxpr(1.0f));
  }

  return size;
}

void GPU::UpdateCRTCConfig()
{
  static constexpr std::array<u16, 8> dot_clock_dividers = {{10, 8, 5, 4, 7, 7, 7, 7}};
  CRTCState& cs = s_locals.crtc_state;

  cs.vertical_total = s_locals.GPUSTAT.pal_mode ? PAL_TOTAL_LINES : NTSC_TOTAL_LINES;
  cs.horizontal_total = s_locals.GPUSTAT.pal_mode ? PAL_TICKS_PER_LINE : NTSC_TICKS_PER_LINE;
  cs.horizontal_active_start = s_locals.GPUSTAT.pal_mode ? PAL_HORIZONTAL_ACTIVE_START : NTSC_HORIZONTAL_ACTIVE_START;
  cs.horizontal_active_end = s_locals.GPUSTAT.pal_mode ? PAL_HORIZONTAL_ACTIVE_END : NTSC_HORIZONTAL_ACTIVE_END;

  const u8 horizontal_resolution_index =
    s_locals.GPUSTAT.horizontal_resolution_1 | (s_locals.GPUSTAT.horizontal_resolution_2 << 2);
  cs.dot_clock_divider = dot_clock_dividers[horizontal_resolution_index];
  cs.horizontal_display_start =
    (std::min<u16>(cs.regs.X1, cs.horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  cs.horizontal_display_end =
    (std::min<u16>(cs.regs.X2, cs.horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  cs.vertical_display_start = std::min<u16>(cs.regs.Y1, cs.vertical_total);
  cs.vertical_display_end = std::min<u16>(cs.regs.Y2, cs.vertical_total);

  if (s_locals.GPUSTAT.pal_mode && g_settings.gpu_force_video_timing == ForceVideoTimingMode::NTSC)
  {
    // scale to NTSC parameters
    cs.horizontal_display_start =
      static_cast<u16>((static_cast<u32>(cs.horizontal_display_start) * NTSC_TICKS_PER_LINE) / PAL_TICKS_PER_LINE);
    cs.horizontal_display_end = static_cast<u16>(
      ((static_cast<u32>(cs.horizontal_display_end) * NTSC_TICKS_PER_LINE) + (PAL_TICKS_PER_LINE - 1)) /
      PAL_TICKS_PER_LINE);
    cs.vertical_display_start =
      static_cast<u16>((static_cast<u32>(cs.vertical_display_start) * NTSC_TOTAL_LINES) / PAL_TOTAL_LINES);
    cs.vertical_display_end = static_cast<u16>(
      ((static_cast<u32>(cs.vertical_display_end) * NTSC_TOTAL_LINES) + (PAL_TOTAL_LINES - 1)) / PAL_TOTAL_LINES);

    cs.vertical_total = NTSC_TOTAL_LINES;
    cs.current_scanline %= NTSC_TOTAL_LINES;
    cs.horizontal_total = NTSC_TICKS_PER_LINE;
    cs.current_tick_in_scanline %= NTSC_TICKS_PER_LINE;
  }
  else if (!s_locals.GPUSTAT.pal_mode && g_settings.gpu_force_video_timing == ForceVideoTimingMode::PAL)
  {
    // scale to PAL parameters
    cs.horizontal_display_start =
      static_cast<u16>((static_cast<u32>(cs.horizontal_display_start) * PAL_TICKS_PER_LINE) / NTSC_TICKS_PER_LINE);
    cs.horizontal_display_end = static_cast<u16>(
      ((static_cast<u32>(cs.horizontal_display_end) * PAL_TICKS_PER_LINE) + (NTSC_TICKS_PER_LINE - 1)) /
      NTSC_TICKS_PER_LINE);
    cs.vertical_display_start =
      static_cast<u16>((static_cast<u32>(cs.vertical_display_start) * PAL_TOTAL_LINES) / NTSC_TOTAL_LINES);
    cs.vertical_display_end = static_cast<u16>(
      ((static_cast<u32>(cs.vertical_display_end) * PAL_TOTAL_LINES) + (NTSC_TOTAL_LINES - 1)) / NTSC_TOTAL_LINES);

    cs.vertical_total = PAL_TOTAL_LINES;
    cs.current_scanline %= PAL_TOTAL_LINES;
    cs.horizontal_total = PAL_TICKS_PER_LINE;
    cs.current_tick_in_scanline %= PAL_TICKS_PER_LINE;
  }

  cs.horizontal_display_start =
    static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_display_start)));
  cs.horizontal_display_end =
    static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_display_end)));
  cs.horizontal_active_start =
    static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_active_start)));
  cs.horizontal_active_end =
    static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_active_end)));
  cs.horizontal_total = static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_total)));

  cs.current_tick_in_scanline %= cs.horizontal_total;
  UpdateCRTCHBlankFlag();

  cs.current_scanline %= cs.vertical_total;

  System::SetVideoFrameRate(ComputeVerticalFrequency());

  UpdateCRTCDisplayParameters();
  UpdateCRTCTickEvent();
}

void GPU::UpdateCRTCDisplayParameters()
{
  CRTCState& cs = s_locals.crtc_state;
  const DisplayCropMode crop_mode = g_settings.display_crop_mode;

  const u16 horizontal_total = s_locals.GPUSTAT.pal_mode ? PAL_TICKS_PER_LINE : NTSC_TICKS_PER_LINE;
  const u16 vertical_total = s_locals.GPUSTAT.pal_mode ? PAL_TOTAL_LINES : NTSC_TOTAL_LINES;
  const u16 horizontal_display_start =
    (std::min<u16>(cs.regs.X1, horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  const u16 horizontal_display_end =
    (std::min<u16>(cs.regs.X2, horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  const u16 vertical_display_start = std::min<u16>(cs.regs.Y1, vertical_total);
  const u16 vertical_display_end = std::min<u16>(cs.regs.Y2, vertical_total);
  const u16 old_horizontal_visible_start = cs.horizontal_visible_start;
  const u16 old_horizontal_visible_end = cs.horizontal_visible_end;
  const u16 old_vertical_visible_start = cs.vertical_visible_start;
  const u16 old_vertical_visible_end = cs.vertical_visible_end;

  if (s_locals.GPUSTAT.pal_mode)
  {
    // TODO: Verify PAL numbers.
    switch (crop_mode)
    {
      case DisplayCropMode::None:
        cs.horizontal_visible_start = PAL_HORIZONTAL_ACTIVE_START;
        cs.horizontal_visible_end = PAL_HORIZONTAL_ACTIVE_END;
        cs.vertical_visible_start = PAL_VERTICAL_ACTIVE_START;
        cs.vertical_visible_end = PAL_VERTICAL_ACTIVE_END;
        break;

      case DisplayCropMode::Overscan:
      case DisplayCropMode::OverscanUncorrected:
        cs.horizontal_visible_start = static_cast<u16>(std::max<s32>(
          0, static_cast<s32>(PAL_OVERSCAN_HORIZONTAL_ACTIVE_START) + g_settings.display_active_start_offset));
        cs.horizontal_visible_end = static_cast<u16>(
          std::max<s32>(cs.horizontal_visible_start,
                        static_cast<s32>(PAL_OVERSCAN_HORIZONTAL_ACTIVE_END) + g_settings.display_active_end_offset));
        cs.vertical_visible_start = static_cast<u16>(std::max<s32>(
          0, static_cast<s32>(PAL_OVERSCAN_VERTICAL_ACTIVE_START) + g_settings.display_line_start_offset));
        cs.vertical_visible_end =
          static_cast<u16>(std::max<s32>(cs.vertical_visible_start, static_cast<s32>(PAL_OVERSCAN_VERTICAL_ACTIVE_END) +
                                                                      g_settings.display_line_end_offset));
        break;

      case DisplayCropMode::Borders:
      case DisplayCropMode::BordersUncorrected:
      default:
        cs.horizontal_visible_start = horizontal_display_start;
        cs.horizontal_visible_end = horizontal_display_end;
        cs.vertical_visible_start = vertical_display_start;
        cs.vertical_visible_end = vertical_display_end;
        break;
    }
    cs.horizontal_visible_start =
      std::clamp<u16>(cs.horizontal_visible_start, PAL_HORIZONTAL_ACTIVE_START, PAL_HORIZONTAL_ACTIVE_END);
    cs.horizontal_visible_end =
      std::clamp<u16>(cs.horizontal_visible_end, cs.horizontal_visible_start, PAL_HORIZONTAL_ACTIVE_END);
    cs.vertical_visible_start =
      std::clamp<u16>(cs.vertical_visible_start, PAL_VERTICAL_ACTIVE_START, PAL_VERTICAL_ACTIVE_END);
    cs.vertical_visible_end =
      std::clamp<u16>(cs.vertical_visible_end, cs.vertical_visible_start, PAL_VERTICAL_ACTIVE_END);
  }
  else
  {
    switch (crop_mode)
    {
      case DisplayCropMode::None:
        cs.horizontal_visible_start = NTSC_HORIZONTAL_ACTIVE_START;
        cs.horizontal_visible_end = NTSC_HORIZONTAL_ACTIVE_END;
        cs.vertical_visible_start = NTSC_VERTICAL_ACTIVE_START;
        cs.vertical_visible_end = NTSC_VERTICAL_ACTIVE_END;
        break;

      case DisplayCropMode::Overscan:
      case DisplayCropMode::OverscanUncorrected:
        cs.horizontal_visible_start = static_cast<u16>(std::max<s32>(
          0, static_cast<s32>(NTSC_OVERSCAN_HORIZONTAL_ACTIVE_START) + g_settings.display_active_start_offset));
        cs.horizontal_visible_end = static_cast<u16>(
          std::max<s32>(cs.horizontal_visible_start,
                        static_cast<s32>(NTSC_OVERSCAN_HORIZONTAL_ACTIVE_END) + g_settings.display_active_end_offset));
        cs.vertical_visible_start = static_cast<u16>(std::max<s32>(
          0, static_cast<s32>(NTSC_OVERSCAN_VERTICAL_ACTIVE_START) + g_settings.display_line_start_offset));
        cs.vertical_visible_end = static_cast<u16>(
          std::max<s32>(cs.vertical_visible_start,
                        static_cast<s32>(NTSC_OVERSCAN_VERTICAL_ACTIVE_END) + g_settings.display_line_end_offset));
        break;

      case DisplayCropMode::Borders:
      case DisplayCropMode::BordersUncorrected:
      default:
        cs.horizontal_visible_start = horizontal_display_start;
        cs.horizontal_visible_end = horizontal_display_end;
        cs.vertical_visible_start = vertical_display_start;
        cs.vertical_visible_end = vertical_display_end;
        break;
    }
    cs.horizontal_visible_start =
      std::clamp<u16>(cs.horizontal_visible_start, NTSC_HORIZONTAL_ACTIVE_START, NTSC_HORIZONTAL_ACTIVE_END);
    cs.horizontal_visible_end =
      std::clamp<u16>(cs.horizontal_visible_end, cs.horizontal_visible_start, NTSC_HORIZONTAL_ACTIVE_END);
    cs.vertical_visible_start =
      std::clamp<u16>(cs.vertical_visible_start, NTSC_VERTICAL_ACTIVE_START, NTSC_VERTICAL_ACTIVE_END);
    cs.vertical_visible_end =
      std::clamp<u16>(cs.vertical_visible_end, cs.vertical_visible_start, NTSC_VERTICAL_ACTIVE_END);
  }

  // If force-progressive is enabled, we only double the height in 480i mode. This way non-interleaved 480i framebuffers
  // won't be broken when displayed.
  const u8 y_shift = BoolToUInt8(s_locals.GPUSTAT.vertical_interlace && s_locals.GPUSTAT.vertical_resolution);
  const u8 height_shift = s_locals.force_progressive_scan ? y_shift : BoolToUInt8(s_locals.GPUSTAT.vertical_interlace);
  const u16 old_vram_width = s_locals.crtc_state.display_vram_width;
  const u16 old_vram_height = s_locals.crtc_state.display_vram_height;

  // Determine screen size.
  cs.display_width = (cs.horizontal_visible_end - cs.horizontal_visible_start) / cs.dot_clock_divider;
  cs.display_height = (cs.vertical_visible_end - cs.vertical_visible_start) << height_shift;

  // Determine number of pixels outputted from VRAM (in general, round to 4-pixel multiple).
  // TODO: Verify behavior if values are outside of the active video portion of scanline.
  const u16 horizontal_display_ticks =
    (horizontal_display_end < horizontal_display_start) ? 0 : (horizontal_display_end - horizontal_display_start);

  const u16 horizontal_display_pixels = horizontal_display_ticks / cs.dot_clock_divider;
  if (horizontal_display_pixels == 1u)
    cs.display_vram_width = 4u;
  else
    cs.display_vram_width = (horizontal_display_pixels + 2u) & ~3u;

  // Determine if we need to adjust the VRAM rectangle (because the display is starting outside the visible area) or add
  // padding.
  u16 horizontal_skip_pixels;
  if (horizontal_display_start >= cs.horizontal_visible_start)
  {
    cs.display_origin_left = (horizontal_display_start - cs.horizontal_visible_start) / cs.dot_clock_divider;
    cs.display_vram_left = cs.regs.X;
    horizontal_skip_pixels = 0;
  }
  else
  {
    horizontal_skip_pixels = (cs.horizontal_visible_start - horizontal_display_start) / cs.dot_clock_divider;
    cs.display_origin_left = 0;
    cs.display_vram_left = (cs.regs.X + horizontal_skip_pixels) % VRAM_WIDTH;
  }

  // apply the crop from the start (usually overscan)
  cs.display_vram_width -= std::min(cs.display_vram_width, horizontal_skip_pixels);

  // Apply crop from the end by shrinking VRAM rectangle width if display would end outside the visible area.
  cs.display_vram_width = std::min<u16>(cs.display_vram_width, cs.display_width - cs.display_origin_left);

  if (vertical_display_start >= cs.vertical_visible_start)
  {
    cs.display_origin_top = (vertical_display_start - cs.vertical_visible_start) << y_shift;
    cs.display_vram_top = cs.regs.Y;
  }
  else
  {
    cs.display_origin_top = 0;
    cs.display_vram_top = (cs.regs.Y + ((cs.vertical_visible_start - vertical_display_start) << y_shift)) % VRAM_HEIGHT;
  }

  if (vertical_display_end <= cs.vertical_visible_end)
  {
    cs.display_vram_height =
      (vertical_display_end -
       std::min(vertical_display_end, std::max(vertical_display_start, cs.vertical_visible_start)))
      << height_shift;
  }
  else
  {
    cs.display_vram_height =
      (cs.vertical_visible_end -
       std::min(cs.vertical_visible_end, std::max(vertical_display_start, cs.vertical_visible_start)))
      << height_shift;
  }

  if (old_horizontal_visible_start != cs.horizontal_visible_start ||
      old_horizontal_visible_end != cs.horizontal_visible_end ||
      old_vertical_visible_start != cs.vertical_visible_start || old_vertical_visible_end != cs.vertical_visible_end)
  {
    System::UpdateGTEAspectRatio();
  }

  if ((cs.display_vram_width != old_vram_width || cs.display_vram_height != old_vram_height) &&
      g_settings.gpu_automatic_resolution_scale)
  {
    System::UpdateAutomaticResolutionScale();
  }
}

GSVector2i GPU::GetCRTCVideoSize()
{
  // Verify assumptions about struct layout.
  static_assert(offsetof(CRTCState, display_width) + sizeof(u16) == offsetof(CRTCState, display_height));
  return GSVector2i::load32(&s_locals.crtc_state.display_width).u16to32();
}

GSVector4i GPU::GetCRTCVideoActiveRect()
{
  static_assert(offsetof(CRTCState, display_origin_left) + sizeof(u16) == offsetof(CRTCState, display_origin_top) &&
                offsetof(CRTCState, display_vram_width) + sizeof(u16) == offsetof(CRTCState, display_vram_height));
  const GSVector2i origin = GSVector2i::load32(&s_locals.crtc_state.display_origin_left).u16to32();
  const GSVector2i size = GSVector2i::load32(&s_locals.crtc_state.display_vram_width).u16to32();
  return GSVector4i::xyxy(origin, origin.add32(size));
}

GSVector4i GPU::GetCRTCVRAMSourceRect()
{
  static_assert(offsetof(CRTCState, display_vram_left) + sizeof(u16) == offsetof(CRTCState, display_vram_top) &&
                offsetof(CRTCState, display_vram_top) + sizeof(u16) == offsetof(CRTCState, display_vram_width) &&
                offsetof(CRTCState, display_vram_width) + sizeof(u16) == offsetof(CRTCState, display_vram_height));
  const GSVector4i rc = GSVector4i::loadl<false>(&s_locals.crtc_state.display_vram_left).u16to32();
  const GSVector2i origin = rc.xy();
  return GSVector4i::xyxy(origin, origin.add32(rc.zw()));
}

ALWAYS_INLINE bool GPU::IsDisplayDisabled()
{
  return s_locals.GPUSTAT.display_disable || s_locals.crtc_state.display_vram_width == 0 ||
         s_locals.crtc_state.display_vram_height == 0;
}

bool GPU::IsInterlacedDisplayEnabled()
{
  return (!s_locals.force_progressive_scan && s_locals.GPUSTAT.vertical_interlace);
}

bool GPU::IsProgressiveDisplayScanForced()
{
  return (s_locals.force_progressive_scan && s_locals.GPUSTAT.vertical_interlace);
}

ALWAYS_INLINE bool GPU::IsInterlacedRenderingEnabled()
{
  return (!s_locals.force_progressive_scan && s_locals.GPUSTAT.SkipDrawingToActiveField());
}

bool GPU::IsInPALMode()
{
  return s_locals.GPUSTAT.pal_mode;
}

ALWAYS_INLINE_RELEASE TickCount GPU::GetPendingCRTCTicks()
{
  const TickCount pending_sysclk_ticks = s_locals.crtc_tick_event.GetTicksSinceLastExecution();
  TickCount fractional_ticks = s_locals.crtc_state.fractional_ticks;
  return SystemTicksToCRTCTicks(pending_sysclk_ticks, &fractional_ticks);
}

ALWAYS_INLINE_RELEASE TickCount GPU::GetPendingCommandTicks()
{
  if (!s_locals.command_tick_event.IsActive())
    return 0;

  return SystemTicksToGPUTicks(s_locals.command_tick_event.GetTicksSinceLastExecution());
}

void GPU::UpdateCRTCTickEvent()
{
  // figure out how many GPU ticks until the next vblank or event
  TickCount lines_until_event;
  if (Timers::IsSyncEnabled(HBLANK_TIMER_INDEX))
  {
    // when the timer sync is enabled we need to sync at vblank start and end
    lines_until_event = (s_locals.crtc_state.current_scanline >= s_locals.crtc_state.vertical_display_end) ?
                          (s_locals.crtc_state.vertical_total - s_locals.crtc_state.current_scanline +
                           s_locals.crtc_state.vertical_display_start) :
                          (s_locals.crtc_state.vertical_display_end - s_locals.crtc_state.current_scanline);
  }
  else
  {
    lines_until_event = (s_locals.crtc_state.current_scanline >= s_locals.crtc_state.vertical_display_end ?
                           (s_locals.crtc_state.vertical_total - s_locals.crtc_state.current_scanline +
                            s_locals.crtc_state.vertical_display_end) :
                           (s_locals.crtc_state.vertical_display_end - s_locals.crtc_state.current_scanline));
  }
  if (Timers::IsExternalIRQEnabled(HBLANK_TIMER_INDEX))
    lines_until_event = std::min(lines_until_event, Timers::GetTicksUntilIRQ(HBLANK_TIMER_INDEX));

  TickCount ticks_until_event =
    lines_until_event * s_locals.crtc_state.horizontal_total - s_locals.crtc_state.current_tick_in_scanline;
  if (Timers::IsExternalIRQEnabled(DOT_TIMER_INDEX))
  {
    const TickCount dots_until_irq = Timers::GetTicksUntilIRQ(DOT_TIMER_INDEX);
    const TickCount ticks_until_irq =
      (dots_until_irq * s_locals.crtc_state.dot_clock_divider) - s_locals.crtc_state.fractional_dot_ticks;
    ticks_until_event = std::min(ticks_until_event, std::max<TickCount>(ticks_until_irq, 0));
  }

  if (Timers::IsSyncEnabled(DOT_TIMER_INDEX))
  {
    // This could potentially be optimized to skip the time the gate is active, if we're resetting and free running.
    // But realistically, I've only seen sync off (most games), or reset+pause on gate (Konami Lightgun games).
    TickCount ticks_until_hblank_start_or_end;
    if (s_locals.crtc_state.current_tick_in_scanline >= s_locals.crtc_state.horizontal_active_end)
    {
      ticks_until_hblank_start_or_end = s_locals.crtc_state.horizontal_total -
                                        s_locals.crtc_state.current_tick_in_scanline +
                                        s_locals.crtc_state.horizontal_active_start;
    }
    else if (s_locals.crtc_state.current_tick_in_scanline < s_locals.crtc_state.horizontal_active_start)
    {
      ticks_until_hblank_start_or_end =
        s_locals.crtc_state.horizontal_active_start - s_locals.crtc_state.current_tick_in_scanline;
    }
    else
    {
      ticks_until_hblank_start_or_end =
        s_locals.crtc_state.horizontal_active_end - s_locals.crtc_state.current_tick_in_scanline;
    }

    ticks_until_event = std::min(ticks_until_event, ticks_until_hblank_start_or_end);
  }

  if (!System::IsReplayingGPUDump()) [[likely]]
    s_locals.crtc_tick_event.Schedule(CRTCTicksToSystemTicks(ticks_until_event, s_locals.crtc_state.fractional_ticks));
}

bool GPU::IsCRTCScanlinePending()
{
  // TODO: Most of these should be fields, not lines.
  const TickCount ticks = (GetPendingCRTCTicks() + s_locals.crtc_state.current_tick_in_scanline);
  return (ticks >= s_locals.crtc_state.horizontal_total);
}

ALWAYS_INLINE void GPU::UpdateCRTCHBlankFlag()
{
  s_locals.crtc_state.in_hblank =
    (s_locals.crtc_state.current_tick_in_scanline < s_locals.crtc_state.horizontal_active_start ||
     s_locals.crtc_state.current_tick_in_scanline >= s_locals.crtc_state.horizontal_active_end);
}

ALWAYS_INLINE_RELEASE bool GPU::IsCommandCompletionPending()
{
  return (s_locals.pending_command_ticks > 0 && GetPendingCommandTicks() >= s_locals.pending_command_ticks);
}

void GPU::CRTCTickEvent(void*, TickCount ticks)
{
  // convert cpu/master clock to GPU ticks, accounting for partial cycles because of the non-integer divider
  const TickCount prev_tick = s_locals.crtc_state.current_tick_in_scanline;
  const TickCount gpu_ticks = SystemTicksToCRTCTicks(ticks, &s_locals.crtc_state.fractional_ticks);
  s_locals.crtc_state.current_tick_in_scanline += gpu_ticks;

  if (Timers::IsUsingExternalClock(DOT_TIMER_INDEX))
  {
    s_locals.crtc_state.fractional_dot_ticks += gpu_ticks;
    const TickCount dots = s_locals.crtc_state.fractional_dot_ticks / s_locals.crtc_state.dot_clock_divider;
    s_locals.crtc_state.fractional_dot_ticks =
      s_locals.crtc_state.fractional_dot_ticks % s_locals.crtc_state.dot_clock_divider;
    if (dots > 0)
      Timers::AddTicks(DOT_TIMER_INDEX, dots);
  }

  if (s_locals.crtc_state.current_tick_in_scanline < s_locals.crtc_state.horizontal_total)
  {
    // short path when we execute <1 line.. this shouldn't occur often, except when gated (konami lightgun games).
    UpdateCRTCHBlankFlag();
    Timers::SetGate(DOT_TIMER_INDEX, s_locals.crtc_state.in_hblank);
    if (Timers::IsUsingExternalClock(HBLANK_TIMER_INDEX))
    {
      const u32 hblank_timer_ticks =
        BoolToUInt32(s_locals.crtc_state.current_tick_in_scanline >= s_locals.crtc_state.horizontal_active_end) -
        BoolToUInt32(prev_tick >= s_locals.crtc_state.horizontal_active_end);
      if (hblank_timer_ticks > 0)
        Timers::AddTicks(HBLANK_TIMER_INDEX, static_cast<TickCount>(hblank_timer_ticks));
    }

    UpdateCRTCTickEvent();
    return;
  }

  u32 lines_to_draw = s_locals.crtc_state.current_tick_in_scanline / s_locals.crtc_state.horizontal_total;
  s_locals.crtc_state.current_tick_in_scanline %= s_locals.crtc_state.horizontal_total;
#if 0
  WARNING_LOG("Old line: {}, new line: {}, drawing {}", s_locals.crtc_state.current_scanline,
              s_locals.crtc_state.current_scanline + lines_to_draw, lines_to_draw);
#endif

  UpdateCRTCHBlankFlag();
  Timers::SetGate(DOT_TIMER_INDEX, s_locals.crtc_state.in_hblank);

  if (Timers::IsUsingExternalClock(HBLANK_TIMER_INDEX))
  {
    // lines_to_draw => number of times ticks passed horizontal_total.
    // Subtract one if we were previously in hblank, but only on that line. If it was previously less than
    // horizontal_active_start, we still want to add one, because hblank would have gone inactive, and then active again
    // during the line. Finally add the current line being drawn, if hblank went inactive->active during the line.
    const u32 hblank_timer_ticks =
      lines_to_draw - BoolToUInt32(prev_tick >= s_locals.crtc_state.horizontal_active_end) +
      BoolToUInt32(s_locals.crtc_state.current_tick_in_scanline >= s_locals.crtc_state.horizontal_active_end);
    if (hblank_timer_ticks > 0)
      Timers::AddTicks(HBLANK_TIMER_INDEX, static_cast<TickCount>(hblank_timer_ticks));
  }

  bool frame_done = false;
  while (lines_to_draw > 0)
  {
    const u32 lines_to_draw_this_loop = std::min(
      lines_to_draw, static_cast<u32>(s_locals.crtc_state.vertical_total - s_locals.crtc_state.current_scanline));
    const u32 prev_scanline = s_locals.crtc_state.current_scanline;
    s_locals.crtc_state.current_scanline = Truncate16(s_locals.crtc_state.current_scanline + lines_to_draw_this_loop);
    DebugAssert(s_locals.crtc_state.current_scanline <= s_locals.crtc_state.vertical_total);
    lines_to_draw -= lines_to_draw_this_loop;

    // clear the vblank flag if the beam would pass through the display area
    if (prev_scanline < s_locals.crtc_state.vertical_display_start &&
        s_locals.crtc_state.current_scanline >= s_locals.crtc_state.vertical_display_end)
    {
      Timers::SetGate(HBLANK_TIMER_INDEX, false);
      InterruptController::SetLineState(InterruptController::IRQ::VBLANK, false);
      s_locals.crtc_state.in_vblank = false;
    }

    const bool new_vblank = s_locals.crtc_state.current_scanline < s_locals.crtc_state.vertical_display_start ||
                            s_locals.crtc_state.current_scanline >= s_locals.crtc_state.vertical_display_end;
    if (s_locals.crtc_state.in_vblank != new_vblank)
    {
      if (new_vblank)
      {
        DEBUG_LOG("Now in v-blank");

        if (s_locals.gpu_dump) [[unlikely]]
        {
          s_locals.gpu_dump->WriteVSync(System::GetGlobalTickCounter());
          if (s_locals.gpu_dump->IsFinished()) [[unlikely]]
            StopRecordingGPUDump();
        }

        // flush any pending draws and "scan out" the image
        // TODO: move present in here I guess
        System::IncrementFrameNumber();
        UpdateDisplay(!System::IsRunaheadActive());
        frame_done = true;

        // switch fields early. this is needed so we draw to the correct one.
        if (s_locals.GPUSTAT.InInterleaved480iMode())
          s_locals.crtc_state.interlaced_display_field = s_locals.crtc_state.interlaced_field ^ 1u;
        else
          s_locals.crtc_state.interlaced_display_field = 0;
      }

      Timers::SetGate(HBLANK_TIMER_INDEX, new_vblank);
      InterruptController::SetLineState(InterruptController::IRQ::VBLANK, new_vblank);
      s_locals.crtc_state.in_vblank = new_vblank;
    }

    // past the end of vblank?
    if (s_locals.crtc_state.current_scanline == s_locals.crtc_state.vertical_total)
    {
      // start the new frame
      s_locals.crtc_state.current_scanline = 0;
      if (s_locals.GPUSTAT.vertical_interlace)
      {
        s_locals.crtc_state.interlaced_field ^= 1u;
        s_locals.GPUSTAT.interlaced_field = BoolToUInt8(!ConvertToBoolUnchecked(s_locals.crtc_state.interlaced_field));
      }
      else
      {
        s_locals.crtc_state.interlaced_field = 0;
        s_locals.GPUSTAT.interlaced_field = 0u; // new GPU = 1, old GPU = 0
      }
    }
  }

  // alternating even line bit in 240-line mode
  if (s_locals.GPUSTAT.InInterleaved480iMode())
  {
    s_locals.crtc_state.active_line_lsb =
      Truncate8((s_locals.crtc_state.regs.Y + BoolToUInt32(s_locals.crtc_state.interlaced_display_field)) & u32(1));
    s_locals.GPUSTAT.display_line_lsb =
      ConvertToBoolUnchecked((s_locals.crtc_state.regs.Y + (BoolToUInt8(!s_locals.crtc_state.in_vblank) &
                                                            s_locals.crtc_state.interlaced_display_field)) &
                             u32(1));
  }
  else
  {
    s_locals.crtc_state.active_line_lsb = 0;
    s_locals.GPUSTAT.display_line_lsb =
      ConvertToBoolUnchecked((s_locals.crtc_state.regs.Y + s_locals.crtc_state.current_scanline) & u32(1));
  }

  UpdateCRTCTickEvent();

  if (frame_done)
  {
    // we can't issue frame done if we're in the middle of executing a rec block, e.g. from reading GPUSTAT
    // defer it until the end of the block in this case.
    if (!TimingEvents::IsRunningEvents()) [[unlikely]]
    {
      DEBUG_LOG("Deferring frame done call");
      s_locals.frame_done_event.Schedule(0);
    }
    else
    {
      System::FrameDone();
    }
  }
}

void GPU::CommandTickEvent(void*, TickCount ticks)
{
  s_locals.pending_command_ticks -= SystemTicksToGPUTicks(ticks);

  s_locals.executing_commands = true;
  ExecuteCommands();
  UpdateCommandTickEvent();
  s_locals.executing_commands = false;
}

void GPU::FrameDoneEvent(void*, TickCount ticks)
{
  DebugAssert(TimingEvents::IsRunningEvents());
  s_locals.frame_done_event.Deactivate();
  System::FrameDone();
}

void GPU::UpdateCommandTickEvent()
{
  if (s_locals.pending_command_ticks <= 0)
  {
    s_locals.pending_command_ticks = 0;
    s_locals.command_tick_event.Deactivate();
  }
  else
  {
    s_locals.command_tick_event.SetIntervalAndSchedule(GPUTicksToSystemTicks(s_locals.pending_command_ticks));
  }
}

u8 GPU::UpdateOrGetGPUBusyPct()
{
  const u32 frame_number = System::GetFrameNumber();
  if ((s_locals.GPUSTAT.pal_mode ? (frame_number % 50) : (frame_number % 60)) != 0) [[likely]]
    return s_locals.last_gpu_busy_pct;

  const double busy_frac =
    static_cast<double>(s_locals.active_ticks_since_last_update) /
    static_cast<double>(SystemTicksToGPUTicks(System::ScaleTicksToOverclock(System::MASTER_CLOCK)) *
                        (ComputeVerticalFrequency() / (s_locals.GPUSTAT.pal_mode ? 50.0f : 60.0f)));
  const double usage_pct = busy_frac * 100.0;

  DEBUG_LOG("PSX GPU Usage: {:.2f}% [{:.0f} cycles avg per frame]", usage_pct,
            static_cast<double>(s_locals.active_ticks_since_last_update) / (s_locals.GPUSTAT.pal_mode ? 50.0f : 60.0f));
  s_locals.active_ticks_since_last_update = 0;

  s_locals.last_gpu_busy_pct = static_cast<u8>(std::min<double>(std::round(usage_pct), 100));
  return s_locals.last_gpu_busy_pct;
}

GSVector2 GPU::ConvertScreenCoordinatesToDisplayCoordinates(GSVector2 window_pos)
{
  const WindowInfo& wi = VideoThread::GetRenderWindowInfo();
  if (wi.IsSurfaceless())
    return GSVector2::cxpr(-1.0f);

  GSVector4 crop_amount = GSVector4::zero();
  GSVector4i source_rc, display_rc, draw_rc;
  const GSVector2i crtc_video_size = GetCRTCVideoSize();
  CalculateDrawRect(GSVector2i(wi.surface_width, wi.surface_height), crtc_video_size, GetCRTCVideoActiveRect(),
                    GetCRTCVRAMSourceRect(), g_settings.display_rotation, g_settings.display_alignment,
                    ComputePixelAspectRatio(),
                    (g_settings.display_scaling == DisplayScalingMode::NearestInteger ||
                     g_settings.display_scaling == DisplayScalingMode::BilinearInteger),
                    g_settings.display_fine_crop_mode, g_settings.display_fine_crop_amount, &source_rc, &display_rc,
                    &draw_rc, &crop_amount);

  // convert coordinates to active display region, then to full display region
  const GSVector2 local_pos = window_pos - GSVector2(display_rc.xy());
  GSVector2 scaled_display_pos = local_pos / GSVector2(display_rc.rsize());

  // scale back to internal resolution
  const GSVector2 cropped_video_size = GSVector2(crtc_video_size) - crop_amount.xy() - crop_amount.zw();
  const GSVector2 display_pos = scaled_display_pos * cropped_video_size + crop_amount.xy();

  // TODO: apply rotation matrix

  DEV_LOG("win {} -> local {}, disp {} (size {} frac {})", window_pos, local_pos, display_pos, cropped_video_size,
          display_pos / cropped_video_size);

  return display_pos;
}

bool GPU::ConvertDisplayCoordinatesToBeamTicksAndLines(const GSVector2& display_pos, float x_scale, u32* out_tick,
                                                       u32* out_line)
{
  float display_x = display_pos.x;
  float display_y = display_pos.y;
  if (x_scale != 1.0f)
  {
    const float dw = static_cast<float>(s_locals.crtc_state.display_width);
    float scaled_x = ((display_x / dw) * 2.0f) - 1.0f; // 0..1 -> -1..1
    scaled_x *= x_scale;
    display_x = (((scaled_x + 1.0f) * 0.5f) * dw); // -1..1 -> 0..1
  }

  if (display_x < 0 || static_cast<u32>(display_x) >= s_locals.crtc_state.display_width || display_y < 0 ||
      static_cast<u32>(display_y) >= s_locals.crtc_state.display_height)
  {
    return false;
  }

  *out_line = (static_cast<u32>(std::round(display_y)) >> BoolToUInt8(s_locals.GPUSTAT.vertical_interlace)) +
              s_locals.crtc_state.vertical_visible_start;
  *out_tick = static_cast<u32>(System::ScaleTicksToOverclock(static_cast<TickCount>(
                std::round(display_x * static_cast<float>(s_locals.crtc_state.dot_clock_divider))))) +
              s_locals.crtc_state.horizontal_visible_start;
  return true;
}

void GPU::GetBeamPosition(u32* out_ticks, u32* out_line)
{
  const u32 current_tick = (GetPendingCRTCTicks() + s_locals.crtc_state.current_tick_in_scanline);
  *out_line = (s_locals.crtc_state.current_scanline + (current_tick / s_locals.crtc_state.horizontal_total)) %
              s_locals.crtc_state.vertical_total;
  *out_ticks = current_tick % s_locals.crtc_state.horizontal_total;
}

TickCount GPU::GetSystemTicksUntilTicksAndLine(u32 ticks, u32 line)
{
  u32 current_tick, current_line;
  GetBeamPosition(&current_tick, &current_line);

  u32 ticks_to_target;
  if (ticks >= current_tick)
  {
    ticks_to_target = ticks - current_tick;
  }
  else
  {
    ticks_to_target = (s_locals.crtc_state.horizontal_total - current_tick) + ticks;
    current_line = (current_line + 1) % s_locals.crtc_state.vertical_total;
  }

  const u32 lines_to_target =
    (line >= current_line) ? (line - current_line) : ((s_locals.crtc_state.vertical_total - current_line) + line);

  const TickCount total_ticks_to_target =
    static_cast<TickCount>((lines_to_target * s_locals.crtc_state.horizontal_total) + ticks_to_target);

  return CRTCTicksToSystemTicks(total_ticks_to_target, s_locals.crtc_state.fractional_ticks);
}

u16 GPU::GetCRTCActiveStartLine()
{
  return s_locals.crtc_state.vertical_display_start;
}

u16 GPU::GetCRTCActiveEndLine()
{
  return s_locals.crtc_state.vertical_display_end;
}

u32 GPU::ReadGPUREAD()
{
  if (s_locals.blitter_state != BlitterState::ReadingVRAM)
    return s_locals.GPUREAD_latch;

  // Read two pixels out of VRAM and combine them. Zero fill odd pixel counts.
  u32 value = 0;
  for (u32 i = 0; i < 2; i++)
  {
    // Read with correct wrap-around behavior.
    const u16 read_x = (s_locals.vram_transfer.x + s_locals.vram_transfer.col) % VRAM_WIDTH;
    const u16 read_y = (s_locals.vram_transfer.y + s_locals.vram_transfer.row) % VRAM_HEIGHT;
    value |= ZeroExtend32(g_vram[read_y * VRAM_WIDTH + read_x]) << (i * 16);

    if (++s_locals.vram_transfer.col == s_locals.vram_transfer.width)
    {
      s_locals.vram_transfer.col = 0;

      if (++s_locals.vram_transfer.row == s_locals.vram_transfer.height)
      {
        DEBUG_LOG("End of VRAM->CPU transfer");
        s_locals.vram_transfer = {};
        s_locals.blitter_state = BlitterState::Idle;

        // end of transfer, catch up on any commands which were written (unlikely)
        ExecuteCommands();
        break;
      }
    }
  }

  s_locals.GPUREAD_latch = value;
  return value;
}

void GPU::WriteGP1(u32 value)
{
  const u32 command = (value >> 24) & 0x3Fu;
  const u32 param = value & UINT32_C(0x00FFFFFF);
  switch (command)
  {
    case static_cast<u8>(GP1Command::ResetGPU):
    {
      DEBUG_LOG("GP1 reset GPU");
      s_locals.command_tick_event.InvokeEarly();
      SynchronizeCRTC();
      SoftReset();
    }
    break;

    case static_cast<u8>(GP1Command::ClearFIFO):
    {
      DEBUG_LOG("GP1 clear FIFO");
      s_locals.command_tick_event.InvokeEarly();
      SynchronizeCRTC();

      // flush partial writes
      if (s_locals.blitter_state == BlitterState::WritingVRAM)
        FinishVRAMWrite();

      s_locals.blitter_state = BlitterState::Idle;
      s_locals.command_total_words = 0;
      s_locals.vram_transfer = {};
      s_locals.fifo.Clear();
      s_locals.blit_buffer.clear();
      s_locals.blit_remaining_words = 0;
      s_locals.pending_command_ticks = 0;
      s_locals.command_tick_event.Deactivate();
      UpdateDMARequest();
      UpdateGPUIdle();
    }
    break;

    case static_cast<u8>(GP1Command::AcknowledgeInterrupt):
    {
      DEBUG_LOG("Acknowledge interrupt");
      s_locals.GPUSTAT.interrupt_request = false;
      InterruptController::SetLineState(InterruptController::IRQ::GPU, false);
    }
    break;

    case static_cast<u8>(GP1Command::SetDisplayDisable):
    {
      const bool disable = ConvertToBoolUnchecked(value & 0x01);
      DEBUG_LOG("Display {}", disable ? "disabled" : "enabled");
      SynchronizeCRTC();

      s_locals.GPUSTAT.display_disable = disable;
    }
    break;

    case static_cast<u8>(GP1Command::SetDMADirection):
    {
      DEBUG_LOG("DMA direction <- 0x{:02X}", static_cast<u32>(param));
      if (s_locals.GPUSTAT.dma_direction != static_cast<GPUDMADirection>(param))
      {
        s_locals.GPUSTAT.dma_direction = static_cast<GPUDMADirection>(param);
        UpdateDMARequest();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetDisplayStartAddress):
    {
      const u32 new_value = param & CRTCState::Regs::DISPLAY_ADDRESS_START_MASK;
      DEBUG_LOG("Display address start <- 0x{:08X}", new_value);

      System::IncrementInternalFrameNumber();
      if (s_locals.crtc_state.regs.display_address_start != new_value)
      {
        SynchronizeCRTC();
        s_locals.crtc_state.regs.display_address_start = new_value;
        UpdateCRTCDisplayParameters();
        GPUBackend::PushCommand(GPUBackend::NewBufferSwappedCommand());
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetHorizontalDisplayRange):
    {
      const u32 new_value = param & CRTCState::Regs::HORIZONTAL_DISPLAY_RANGE_MASK;
      DEBUG_LOG("Horizontal display range <- 0x{:08X}", new_value);

      if (s_locals.crtc_state.regs.horizontal_display_range != new_value)
      {
        SynchronizeCRTC();
        s_locals.crtc_state.regs.horizontal_display_range = new_value;
        UpdateCRTCConfig();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetVerticalDisplayRange):
    {
      const u32 new_value = param & CRTCState::Regs::VERTICAL_DISPLAY_RANGE_MASK;
      DEBUG_LOG("Vertical display range <- 0x{:08X}", new_value);

      if (s_locals.crtc_state.regs.vertical_display_range != new_value)
      {
        SynchronizeCRTC();
        s_locals.crtc_state.regs.vertical_display_range = new_value;
        UpdateCRTCConfig();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetDisplayMode):
    {
      const GP1SetDisplayMode dm{param};
      GPUSTATReg new_GPUSTAT{s_locals.GPUSTAT.bits};
      new_GPUSTAT.horizontal_resolution_1 = dm.horizontal_resolution_1;
      new_GPUSTAT.vertical_resolution = dm.vertical_resolution;
      new_GPUSTAT.pal_mode = dm.pal_mode;
      new_GPUSTAT.display_area_color_depth_24 = dm.display_area_color_depth;
      new_GPUSTAT.vertical_interlace = dm.vertical_interlace;
      new_GPUSTAT.horizontal_resolution_2 = dm.horizontal_resolution_2;
      new_GPUSTAT.reverse_flag = dm.reverse_flag;
      DEBUG_LOG("Set display mode <- 0x{:08X}", dm.bits);

      if (!s_locals.GPUSTAT.vertical_interlace && dm.vertical_interlace && !s_locals.force_progressive_scan)
      {
        // bit of a hack, technically we should pull the previous frame in, but this may not exist anymore
        ClearDisplay();
      }

      if (s_locals.GPUSTAT.bits != new_GPUSTAT.bits)
      {
        // Have to be careful when setting this because Synchronize() can modify GPUSTAT.
        static constexpr u32 SET_MASK = UINT32_C(0b00000000011111110100000000000000);
        s_locals.command_tick_event.InvokeEarly();
        SynchronizeCRTC();
        s_locals.GPUSTAT.bits = (s_locals.GPUSTAT.bits & ~SET_MASK) | (new_GPUSTAT.bits & SET_MASK);
        UpdateCRTCConfig();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetAllowTextureDisable):
    {
      s_locals.set_texture_disable_mask = ConvertToBoolUnchecked(param & 0x01);
      DEBUG_LOG("Set texture disable mask <- {}", s_locals.set_texture_disable_mask ? "allowed" : "ignored");
    }
    break;

    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F:
    {
      HandleGetGPUInfoCommand(value);
    }
    break;

    [[unlikely]] default:
      ERROR_LOG("Unimplemented GP1 command 0x{:02X}", command);
      break;
  }
}

void GPU::HandleGetGPUInfoCommand(u32 value)
{
  const u8 subcommand = Truncate8(value & 0x07);
  switch (subcommand)
  {
    case 0x00:
    case 0x01:
    case 0x06:
    case 0x07:
      // leave GPUREAD intact
      break;

    case 0x02: // Get Texture Window
    {
      s_locals.GPUREAD_latch = s_locals.draw_mode.texture_window_value;
      DEBUG_LOG("Get texture window => 0x{:08X}", s_locals.GPUREAD_latch);
    }
    break;

    case 0x03: // Get Draw Area Top Left
    {
      s_locals.GPUREAD_latch = (s_locals.drawing_area.left | (s_locals.drawing_area.top << 10));
      DEBUG_LOG("Get drawing area top left: ({}, {}) => 0x{:08X}", s_locals.drawing_area.left,
                s_locals.drawing_area.top, s_locals.GPUREAD_latch);
    }
    break;

    case 0x04: // Get Draw Area Bottom Right
    {
      s_locals.GPUREAD_latch = (s_locals.drawing_area.right | (s_locals.drawing_area.bottom << 10));
      DEBUG_LOG("Get drawing area bottom right: ({}, {}) => 0x{:08X}", s_locals.drawing_area.right,
                s_locals.drawing_area.bottom, s_locals.GPUREAD_latch);
    }
    break;

    case 0x05: // Get Drawing Offset
    {
      s_locals.GPUREAD_latch = (s_locals.drawing_offset.x & 0x7FF) | ((s_locals.drawing_offset.y & 0x7FF) << 11);
      DEBUG_LOG("Get drawing offset: ({}, {}) => 0x{:08X}", s_locals.drawing_offset.x, s_locals.drawing_offset.y,
                s_locals.GPUREAD_latch);
    }
    break;

    [[unlikely]] default:
      WARNING_LOG("Unhandled GetGPUInfo(0x{:02X})", subcommand);
      break;
  }
}

void GPU::UpdateCLUTIfNeeded(GPUTextureMode texmode, GPUTexturePaletteReg clut)
{
  if (texmode >= GPUTextureMode::Direct16Bit)
    return;

  const bool needs_8bit = (texmode == GPUTextureMode::Palette8Bit);
  if ((clut.bits != s_locals.current_clut_reg_bits) ||
      BoolToUInt8(needs_8bit) > BoolToUInt8(s_locals.current_clut_is_8bit))
  {
    DEBUG_LOG("Reloading CLUT from {},{}, {}", clut.GetXBase(), clut.GetYBase(), needs_8bit ? "8-bit" : "4-bit");
    AddCommandTicks(needs_8bit ? 256 : 16);
    s_locals.current_clut_reg_bits = clut.bits;
    s_locals.current_clut_is_8bit = needs_8bit;

    GPUBackendUpdateCLUTCommand* cmd = GPUBackend::NewUpdateCLUTCommand();
    cmd->reg.bits = clut.bits;
    cmd->clut_is_8bit = needs_8bit;
    GPUBackend::PushCommand(cmd);
  }
}

void GPU::InvalidateCLUT()
{
  s_locals.current_clut_reg_bits =
    std::numeric_limits<decltype(s_locals.current_clut_reg_bits)>::max(); // will never match
  s_locals.current_clut_is_8bit = false;
}

void GPU::SetClampedDrawingArea()
{
  s_locals.clamped_drawing_area = GetClampedDrawingArea(s_locals.drawing_area);
}

GSVector4i GPU::GetClampedDrawingArea(const GPUDrawingArea& drawing_area)
{
  if (drawing_area.left > drawing_area.right || drawing_area.top > drawing_area.bottom) [[unlikely]]
    return GSVector4i::zero();

  const u32 right = std::min(drawing_area.right + 1, static_cast<u32>(VRAM_WIDTH));
  const u32 left = std::min(drawing_area.left, std::min(drawing_area.right, VRAM_WIDTH - 1));
  const u32 bottom = std::min(drawing_area.bottom + 1, static_cast<u32>(VRAM_HEIGHT));
  const u32 top = std::min(drawing_area.top, std::min(drawing_area.bottom, VRAM_HEIGHT - 1));
  return GSVector4i(left, top, right, bottom);
}

void GPU::SetDrawMode(u16 value)
{
  GPUDrawModeReg new_mode_reg{static_cast<u16>(value & GPUDrawModeReg::MASK)};
  if (!s_locals.set_texture_disable_mask)
    new_mode_reg.texture_disable = false;

  s_locals.draw_mode.mode_reg.bits = new_mode_reg.bits;

  // Bits 0..10 are returned in the GPU status register.
  s_locals.GPUSTAT.bits = (s_locals.GPUSTAT.bits & ~(GPUDrawModeReg::GPUSTAT_MASK)) |
                          (ZeroExtend32(new_mode_reg.bits) & GPUDrawModeReg::GPUSTAT_MASK);
  s_locals.GPUSTAT.texture_disable = s_locals.draw_mode.mode_reg.texture_disable;
}

void GPU::SetTexturePalette(u16 value)
{
  value &= DrawMode::PALETTE_MASK;
  s_locals.draw_mode.palette_reg.bits = value;
}

void GPU::SetTextureWindow(u32 value)
{
  value &= DrawMode::TEXTURE_WINDOW_MASK;
  if (s_locals.draw_mode.texture_window_value == value)
    return;

  const u8 mask_x = Truncate8(value & UINT32_C(0x1F));
  const u8 mask_y = Truncate8((value >> 5) & UINT32_C(0x1F));
  const u8 offset_x = Truncate8((value >> 10) & UINT32_C(0x1F));
  const u8 offset_y = Truncate8((value >> 15) & UINT32_C(0x1F));
  DEBUG_LOG("Set texture window {:02X} {:02X} {:02X} {:02X}", mask_x, mask_y, offset_x, offset_y);

  s_locals.draw_mode.texture_window.and_x = ~(mask_x * 8);
  s_locals.draw_mode.texture_window.and_y = ~(mask_y * 8);
  s_locals.draw_mode.texture_window.or_x = (offset_x & mask_x) * 8u;
  s_locals.draw_mode.texture_window.or_y = (offset_y & mask_y) * 8u;
  s_locals.draw_mode.texture_window_value = value;
}

static bool IntegerScalePreferWidth(float display_width, float display_height, float pixel_aspect_ratio,
                                    float fwindow_width, float fwindow_height)
{
  static constexpr auto get_integer_scale = [](float dwidth, float dheight, float wwidth, float wheight) {
    if ((dwidth / dheight) >= (wwidth / wheight))
      return std::floor(wwidth / dwidth);
    else
      return std::floor(wheight / dheight);
  };

  const float scale_width =
    get_integer_scale(display_width * pixel_aspect_ratio, display_height, fwindow_width, fwindow_height);
  const float scale_height =
    get_integer_scale(display_width, display_height / pixel_aspect_ratio, fwindow_width, fwindow_height);

  return (scale_width >= scale_height);
}

void GPU::CalculateDrawRect(const GSVector2i& window_size, const GSVector2i& video_size,
                            const GSVector4i& video_active_rect, const GSVector4i& source_rect,
                            DisplayRotation rotation, DisplayAlignment alignment, float pixel_aspect_ratio,
                            bool integer_scale, DisplayFineCropMode fine_crop,
                            const std::span<const s16, 4>& fine_crop_amount, GSVector4i* out_source_rect,
                            GSVector4i* out_display_rect, GSVector4i* out_draw_rect, GSVector4* out_crop_amount)
{
  GSVector2 fwindow_size = GSVector2(window_size);
  GSVector2 fvideo_size = GSVector2(video_size);
  GSVector4 fvideo_active_rect = GSVector4(video_active_rect);
  GSVector4 fsource_rect = GSVector4(source_rect);

  // for integer scale, use whichever gets us a greater effective display size
  // this is needed for games like crash where the framebuffer is wide to not lose detail
  if (integer_scale ?
        IntegerScalePreferWidth(fvideo_size.x, fvideo_size.y, pixel_aspect_ratio, fwindow_size.x, fwindow_size.y) :
        (pixel_aspect_ratio >= 1.0f))
  {
    fvideo_size.x *= pixel_aspect_ratio;
    fvideo_active_rect = fvideo_active_rect.blend32<5>(fvideo_active_rect * pixel_aspect_ratio);
  }
  else
  {
    fvideo_size.y /= pixel_aspect_ratio;
    fvideo_active_rect = fvideo_active_rect.blend32<10>(fvideo_active_rect / pixel_aspect_ratio);
  }

  if (fine_crop != DisplayFineCropMode::None)
  {
    GSVector4 crop_amount = GSVector4(GSVector4i::loadl<false>(fine_crop_amount.data()).s16to32());
    switch (fine_crop)
    {
      case DisplayFineCropMode::VideoResolution:
        break;

      case DisplayFineCropMode::InternalResolution:
        crop_amount *= GSVector4::xyxy(fvideo_size / fsource_rect.rsize());
        break;

      case DisplayFineCropMode::WindowResolution:
        crop_amount *= GSVector4::xyxy(fvideo_size / fwindow_size);
        break;

        DefaultCaseIsUnreachable();
    }

    if (out_crop_amount)
      *out_crop_amount = crop_amount;

    // apply crop to padding first
    const GSVector2 crop_padding_left_top = fvideo_active_rect.xy().min(crop_amount.xy());
    const GSVector2 crop_padding_right_bottom = (fvideo_size - fvideo_active_rect.zw()).min(crop_amount.zw());
    fvideo_active_rect = fvideo_active_rect - GSVector4::xyxy(crop_padding_left_top);
    fvideo_size = fvideo_size - crop_padding_left_top - crop_padding_right_bottom;
    crop_amount = crop_amount - GSVector4::xyxy(crop_padding_left_top, crop_padding_right_bottom);

    // apply remaining crop to active area
    const GSVector2 crop_size = crop_amount.xy() + crop_amount.zw();
    fvideo_active_rect -= GSVector4::loadh(crop_size);
    fvideo_size -= crop_size;

    // need to take it off the source too
    const GSVector2 video_to_source = fsource_rect.rsize() / fvideo_active_rect.rsize();
    const GSVector4 source_crop_amount = crop_amount * GSVector4::xyxy(video_to_source);
    fsource_rect = (fsource_rect + source_crop_amount).blend32<12>(fsource_rect - source_crop_amount);

    // ensure we haven't cropped everything away
    fvideo_active_rect = fvideo_active_rect.sat(GSVector4::zero(), fvideo_active_rect);
    fvideo_size -= crop_size.sat(GSVector2::zero(), GSVector2::cxpr(1.0f, 1.0f));
    fsource_rect = fsource_rect.sat(GSVector4::zero(), fsource_rect);
  }

  // swap width/height when rotated, the flipping of padding is taken care of in the shader with the rotation matrix
  if (rotation == DisplayRotation::Rotate90 || rotation == DisplayRotation::Rotate270)
  {
    fvideo_size = fvideo_size.yx();
    fvideo_active_rect = fvideo_active_rect.yxwz();
  }

  // now fit it within the window
  float scale;
  GSVector2 padding;
  if ((fvideo_size.x / fvideo_size.y) >= (fwindow_size.x / fwindow_size.y))
  {
    // align in middle vertically
    scale = fwindow_size.x / fvideo_size.x;
    if (integer_scale)
    {
      // skip integer scaling if we cannot fit in the window at all
      scale = (scale >= 1.0f) ? std::floor(scale) : scale;
      padding.x = std::max<float>((fwindow_size.x - fvideo_size.x * scale) / 2.0f, 0.0f);
    }
    else
    {
      padding.x = 0.0f;
    }

    switch (alignment)
    {
      case DisplayAlignment::RightOrBottom:
        padding.y = std::max<float>(fwindow_size.y - (fvideo_size.y * scale), 0.0f);
        break;

      case DisplayAlignment::Center:
        padding.y = std::max<float>((fwindow_size.y - (fvideo_size.y * scale)) / 2.0f, 0.0f);
        break;

      case DisplayAlignment::LeftOrTop:
      default:
        padding.y = 0.0f;
        break;
    }
  }
  else
  {
    // align in middle horizontally
    scale = fwindow_size.y / fvideo_size.y;
    if (integer_scale)
    {
      // skip integer scaling if we cannot fit in the window at all
      scale = (scale >= 1.0f) ? std::floor(scale) : scale;
      padding.y = std::max<float>((fwindow_size.y - (fvideo_size.y * scale)) / 2.0f, 0.0f);
    }
    else
    {
      padding.y = 0.0f;
    }

    switch (alignment)
    {
      case DisplayAlignment::RightOrBottom:
        padding.x = std::max<float>(fwindow_size.x - (fvideo_size.x * scale), 0.0f);
        break;

      case DisplayAlignment::Center:
        padding.x = std::max<float>((fwindow_size.x - (fvideo_size.x * scale)) / 2.0f, 0.0f);
        break;

      case DisplayAlignment::LeftOrTop:
      default:
        padding.x = 0.0f;
        break;
    }
  }

  const GSVector4 padding4 = GSVector4::xyxy(padding);
  fvideo_size *= scale;
  fvideo_active_rect *= scale;
  *out_source_rect = GSVector4i(fsource_rect);
  *out_draw_rect = GSVector4i(fvideo_active_rect + padding4);
  *out_display_rect = GSVector4i(GSVector4::loadh(fvideo_size) + padding4);
}

void GPU::ReadVRAM(u16 x, u16 y, u16 width, u16 height)
{
  // If we're using the software renderer, we only need to sync the thread.
  // If stats are enabled, still send the packet to update the read counter.
  if ((!GPUBackend::IsUsingHardwareBackend() || g_settings.gpu_use_software_renderer_for_readbacks) &&
      !g_settings.display_show_gpu_stats)
  {
    VideoThread::SyncThread(true);
    return;
  }

  GPUBackendReadVRAMCommand* cmd = GPUBackend::NewReadVRAMCommand();
  cmd->x = x;
  cmd->y = y;
  cmd->width = width;
  cmd->height = height;
  GPUBackend::PushCommandAndSync(cmd, true);
}

void GPU::UpdateVRAM(u16 x, u16 y, u16 width, u16 height, const void* data, bool set_mask, bool check_mask)
{
  const u32 num_words = width * height;
  GPUBackendUpdateVRAMCommand* cmd = GPUBackend::NewUpdateVRAMCommand(num_words);
  cmd->x = x;
  cmd->y = y;
  cmd->width = width;
  cmd->height = height;
  cmd->set_mask_while_drawing = set_mask;
  cmd->check_mask_before_draw = check_mask;
  std::memcpy(cmd->data, data, num_words * sizeof(u16));
  GPUBackend::PushCommand(cmd);
}

void GPU::ClearDisplay()
{
  GPUBackend::PushCommand(GPUBackend::NewClearDisplayCommand());
}

void GPU::UpdateDisplay(bool submit_frame)
{
  const bool interlaced = IsInterlacedDisplayEnabled();
  const u8 interlaced_field = s_locals.crtc_state.interlaced_field;
  const bool line_skip = (interlaced && s_locals.GPUSTAT.vertical_resolution);

  // NOTE: Must be split out, since this can push commands itself (e.g. media capture).
  GPUBackendFramePresentationParameters frame;
  submit_frame = (submit_frame && System::GetFramePresentationParameters(&frame));

  GPUBackendUpdateDisplayCommand* cmd = GPUBackend::NewUpdateDisplayCommand();
  cmd->gpu_busy_pct = g_settings.display_show_gpu_stats ? UpdateOrGetGPUBusyPct() : 0;
  if (!g_settings.gpu_show_vram) [[likely]]
  {
    cmd->display_width = s_locals.crtc_state.display_width;
    cmd->display_height = s_locals.crtc_state.display_height;
    cmd->display_origin_left = s_locals.crtc_state.display_origin_left;
    cmd->display_origin_top = s_locals.crtc_state.display_origin_top;
    cmd->display_vram_left = s_locals.crtc_state.display_vram_left;
    cmd->display_vram_top = s_locals.crtc_state.display_vram_top;
    cmd->display_vram_width = s_locals.crtc_state.display_vram_width;
    cmd->display_vram_height = s_locals.crtc_state.display_vram_height >> BoolToUInt8(interlaced);
    cmd->X = s_locals.crtc_state.regs.X;
    cmd->interlaced_display_enabled = interlaced;
    cmd->interlaced_display_field = ConvertToBoolUnchecked(interlaced_field);
    cmd->interlaced_display_interleaved = line_skip;
    cmd->interleaved_480i_mode = s_locals.GPUSTAT.InInterleaved480iMode();
    cmd->display_24bit = s_locals.GPUSTAT.display_area_color_depth_24;
    cmd->display_disabled = IsDisplayDisabled();
    cmd->display_pixel_aspect_ratio = ComputePixelAspectRatio();
  }
  else
  {
    cmd->display_width = VRAM_WIDTH;
    cmd->display_height = VRAM_HEIGHT;
    cmd->display_origin_left = 0;
    cmd->display_origin_top = 0;
    cmd->display_vram_left = 0;
    cmd->display_vram_top = 0;
    cmd->display_vram_width = VRAM_WIDTH;
    cmd->display_vram_height = VRAM_HEIGHT;
    cmd->X = 0;
    cmd->interlaced_display_enabled = false;
    cmd->interlaced_display_field = false;
    cmd->interlaced_display_interleaved = false;
    cmd->interleaved_480i_mode = false;
    cmd->display_24bit = false;
    cmd->display_disabled = false;
    cmd->display_pixel_aspect_ratio = 1.0f;
  }

  if ((cmd->submit_frame = submit_frame))
  {
    std::memcpy(&cmd->frame, &frame, sizeof(frame));

    const bool drain_one = cmd->frame.present_frame && GPUBackend::BeginQueueFrame();
    VideoThread::PushCommandAndWakeThread(cmd);
    if (drain_one)
      GPUBackend::WaitForOneQueuedFrame();
  }
  else
  {
    VideoThread::PushCommand(cmd);
  }
}

void GPU::QueuePresentCurrentFrame()
{
  DebugAssert(g_settings.IsRunaheadEnabled());

  // NOTE: Must be split out, since this can push commands itself (e.g. media capture).
  GPUBackendFramePresentationParameters frame;
  const bool submit_frame = System::GetFramePresentationParameters(&frame);
  if (!submit_frame)
    return;

  // Submit can be skipped if it's a dupe frame and we're not dumping frames.
  GPUBackendSubmitFrameCommand* cmd = GPUBackend::NewSubmitFrameCommand();
  std::memcpy(&cmd->frame, &frame, sizeof(frame));

  const bool drain_one = cmd->frame.present_frame && GPUBackend::BeginQueueFrame();
  VideoThread::PushCommandAndWakeThread(cmd);
  if (drain_one)
    GPUBackend::WaitForOneQueuedFrame();
}

u8 GPU::CalculateAutomaticResolutionScale()
{
  // Auto scaling.
  // When the system is starting and all borders crop is enabled, the registers are zero, and
  // display_height therefore is also zero. Keep the existing resolution until it updates.
  u32 scale = 1;
  if (const WindowInfo& main_window_info = VideoThread::GetRenderWindowInfo();
      !main_window_info.IsSurfaceless() && s_locals.crtc_state.display_width > 0 &&
      s_locals.crtc_state.display_height > 0 && s_locals.crtc_state.display_vram_width > 0 &&
      s_locals.crtc_state.display_vram_height > 0)
  {
    GSVector4i source_rect, display_rect, draw_rect;
    CalculateDrawRect(GSVector2i(main_window_info.surface_width, main_window_info.surface_height), GetCRTCVideoSize(),
                      GetCRTCVideoActiveRect(), GetCRTCVRAMSourceRect(), g_settings.display_rotation,
                      g_settings.display_alignment, g_settings.gpu_show_vram ? 1.0f : ComputePixelAspectRatio(),
                      g_settings.IsUsingIntegerDisplayScaling(false), g_settings.display_fine_crop_mode,
                      g_settings.display_fine_crop_amount, &source_rect, &display_rect, &draw_rect);

    // We use the draw rect to determine scaling. This way we match the resolution as best we can, regardless of the
    // anamorphic aspect ratio.
    const s32 draw_width = draw_rect.width();
    const s32 draw_height = draw_rect.height();
    scale = static_cast<u32>(std::ceil(
      std::max(static_cast<float>(draw_width) / static_cast<float>(s_locals.crtc_state.display_vram_width),
               static_cast<float>(draw_height) / static_cast<float>(s_locals.crtc_state.display_vram_height))));
    scale = std::min<u32>(scale, std::numeric_limits<decltype(g_settings.gpu_resolution_scale)>::max());
    VERBOSE_LOG("Draw Size = {}x{}, VRAM Size = {}x{}, Preferred Scale = {}", draw_width, draw_height,
                s_locals.crtc_state.display_vram_width, s_locals.crtc_state.display_vram_height, scale);
  }

  return Truncate8(scale);
}

bool GPU::DumpVRAMToFile(std::string path, Error* error)
{
  ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);

  const std::string_view extension = Path::GetExtension(path);
  if (StringUtil::EqualNoCase(extension, "png"))
  {
    return DumpVRAMToFile(std::move(path), VRAM_WIDTH, VRAM_HEIGHT, sizeof(u16) * VRAM_WIDTH, g_vram, true, error);
  }
  else if (StringUtil::EqualNoCase(extension, "bin"))
  {
    return FileSystem::WriteAtomicRenamedFile(std::move(path), g_vram, VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16), error);
  }
  else
  {
    Error::SetStringFmt(error, "Unknown extension: '{}'", extension);
    return false;
  }
}

bool GPU::DumpVRAMToFile(std::string path, u32 width, u32 height, u32 stride, const void* buffer, bool remove_alpha,
                         Error* error /* = nullptr */)
{
  Image image(width, height, ImageFormat::RGBA8);

  const char* ptr_in = static_cast<const char*>(buffer);
  for (u32 row = 0; row < height; row++)
  {
    const char* row_ptr_in = ptr_in;
    u8* ptr_out = image.GetRowPixels(row);

    for (u32 col = 0; col < width; col++)
    {
      u16 src_col;
      std::memcpy(&src_col, row_ptr_in, sizeof(u16));
      row_ptr_in += sizeof(u16);

      const u32 pixel32 = VRAMRGBA5551ToRGBA8888(remove_alpha ? (src_col | u16(0x8000)) : src_col);
      std::memcpy(ptr_out, &pixel32, sizeof(pixel32));
      ptr_out += sizeof(pixel32);
    }

    ptr_in += stride;
  }

  return image.SaveToFile(path.c_str(), Image::DEFAULT_SAVE_QUALITY, error);
}

static constexpr GPU::GP0CommandHandlerTable s_GP0_command_handler_table = []() constexpr {
  GPU::GP0CommandHandlerTable table = {};
  for (u32 i = 0; i < static_cast<u32>(table.size()); i++)
    table[i] = &GPU::HandleUnknownGP0Command;
  table[0x00] = &GPU::HandleNOPCommand;
  table[0x01] = &GPU::HandleClearCacheCommand;
  table[0x02] = &GPU::HandleFillRectangleCommand;
  table[0x03] = &GPU::HandleNOPCommand;
  for (u32 i = 0x04; i <= 0x1E; i++)
    table[i] = &GPU::HandleNOPCommand;
  table[0x1F] = &GPU::HandleInterruptRequestCommand;
  for (u32 i = 0x20; i <= 0x7F; i++)
  {
    switch (static_cast<GPUPrimitive>((i >> 5) & 0x03))
    {
      case GPUPrimitive::Polygon:
        table[i] = &GPU::HandleRenderPolygonCommand;
        break;
      case GPUPrimitive::Line:
        table[i] = (i & 0x08) ? &GPU::HandleRenderPolyLineCommand : &GPU::HandleRenderLineCommand;
        break;
      case GPUPrimitive::Rectangle:
        table[i] = &GPU::HandleRenderRectangleCommand;
        break;
      default:
        table[i] = &GPU::HandleUnknownGP0Command;
        break;
    }
  }
  table[0xE0] = &GPU::HandleNOPCommand;
  table[0xE1] = &GPU::HandleSetDrawModeCommand;
  table[0xE2] = &GPU::HandleSetTextureWindowCommand;
  table[0xE3] = &GPU::HandleSetDrawingAreaTopLeftCommand;
  table[0xE4] = &GPU::HandleSetDrawingAreaBottomRightCommand;
  table[0xE5] = &GPU::HandleSetDrawingOffsetCommand;
  table[0xE6] = &GPU::HandleSetMaskBitCommand;
  for (u32 i = 0xE7; i <= 0xEF; i++)
    table[i] = &GPU::HandleNOPCommand;
  for (u32 i = 0x80; i <= 0x9F; i++)
    table[i] = &GPU::HandleCopyRectangleVRAMToVRAMCommand;
  for (u32 i = 0xA0; i <= 0xBF; i++)
    table[i] = &GPU::HandleCopyRectangleCPUToVRAMCommand;
  for (u32 i = 0xC0; i <= 0xDF; i++)
    table[i] = &GPU::HandleCopyRectangleVRAMToCPUCommand;

  table[0xFF] = &GPU::HandleNOPCommand;

  return table;
}();

#define CHECK_COMMAND_SIZE(num_words)                                                                                  \
  if (s_locals.fifo.GetSize() < num_words)                                                                             \
  {                                                                                                                    \
    s_locals.command_total_words = num_words;                                                                          \
    return false;                                                                                                      \
  }

static constexpr u32 ReplaceZero(u32 value, u32 value_for_zero)
{
  return value == 0 ? value_for_zero : value;
}

ALWAYS_INLINE u32 GPU::FifoPop()
{
  return Truncate32(s_locals.fifo.Pop());
}

ALWAYS_INLINE u32 GPU::FifoPeek()
{
  return Truncate32(s_locals.fifo.Peek());
}

ALWAYS_INLINE u32 GPU::FifoPeek(u32 i)
{
  return Truncate32(s_locals.fifo.Peek(i));
}

void GPU::ExecuteCommands()
{
  const bool was_executing_from_event = std::exchange(s_locals.executing_commands, true);

  TryExecuteCommands();
  UpdateDMARequest();
  UpdateGPUIdle();

  s_locals.executing_commands = was_executing_from_event;
  if (!was_executing_from_event)
    UpdateCommandTickEvent();
}

void GPU::TryExecuteCommands()
{
  while (s_locals.pending_command_ticks <= s_locals.max_run_ahead && !s_locals.fifo.IsEmpty())
  {
    switch (s_locals.blitter_state)
    {
      case BlitterState::Idle:
      {
        const u32 command = FifoPeek(0) >> 24;
        if (s_GP0_command_handler_table[command]())
          continue;
        else
          return;
      }

      case BlitterState::WritingVRAM:
      {
        DebugAssert(s_locals.blit_remaining_words > 0);
        const u32 words_to_copy = std::min(s_locals.blit_remaining_words, s_locals.fifo.GetSize());
        s_locals.blit_buffer.reserve(s_locals.blit_buffer.size() + words_to_copy);
        for (u32 i = 0; i < words_to_copy; i++)
          s_locals.blit_buffer.push_back(FifoPop());
        s_locals.blit_remaining_words -= words_to_copy;

        DEBUG_LOG("VRAM write burst of {} words, {} words remaining", words_to_copy, s_locals.blit_remaining_words);
        if (s_locals.blit_remaining_words == 0)
          FinishVRAMWrite();

        continue;
      }

      case BlitterState::ReadingVRAM:
      {
        return;
      }
      break;

      case BlitterState::DrawingPolyLine:
      {
        const u32 words_per_vertex = s_locals.render_command.shading_enable ? 2 : 1;
        u32 terminator_index =
          s_locals.render_command.shading_enable ? ((static_cast<u32>(s_locals.polyline_buffer.size()) & 1u) ^ 1u) : 0u;
        for (; terminator_index < s_locals.fifo.GetSize(); terminator_index += words_per_vertex)
        {
          // polyline must have at least two vertices, and the terminator is (word & 0xf000f000) == 0x50005000.
          // terminator is on the first word for the vertex
          if ((FifoPeek(terminator_index) & UINT32_C(0xF000F000)) == UINT32_C(0x50005000))
            break;
        }

        const bool found_terminator = (terminator_index < s_locals.fifo.GetSize());
        const u32 words_to_copy = std::min(terminator_index, s_locals.fifo.GetSize());
        if (words_to_copy > 0)
        {
          s_locals.polyline_buffer.reserve(s_locals.polyline_buffer.size() + words_to_copy);
          for (u32 i = 0; i < words_to_copy; i++)
            s_locals.polyline_buffer.push_back(s_locals.fifo.Pop());
        }

        DEBUG_LOG("Added {} words to polyline", words_to_copy);
        if (found_terminator)
        {
          // drop terminator
          s_locals.fifo.RemoveOne();
          DEBUG_LOG("Drawing poly-line with {} vertices", GetPolyLineVertexCount());
          FinishPolyline();
          s_locals.polyline_buffer.clear();
          EndCommand();
          continue;
        }
      }
      break;
    }
  }
}

void GPU::EndCommand()
{
  s_locals.blitter_state = BlitterState::Idle;
  s_locals.command_total_words = 0;
}

bool GPU::HandleUnknownGP0Command()
{
  const u32 command = FifoPeek() >> 24;
  ERROR_LOG("Unimplemented GP0 command 0x{:02X}", command);

  SmallString dump;
  for (u32 i = 0; i < s_locals.fifo.GetSize(); i++)
    dump.append_format("{}{:08X}", (i > 0) ? " " : "", FifoPeek(i));
  ERROR_LOG("FIFO: {}", dump);

  s_locals.fifo.RemoveOne();
  EndCommand();
  return true;
}

bool GPU::HandleNOPCommand()
{
  s_locals.fifo.RemoveOne();
  EndCommand();
  return true;
}

bool GPU::HandleClearCacheCommand()
{
  DEBUG_LOG("GP0 clear cache");
  InvalidateCLUT();
  GPUBackend::PushCommand(GPUBackend::NewClearCacheCommand());
  s_locals.fifo.RemoveOne();
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleInterruptRequestCommand()
{
  DEBUG_LOG("GP0 interrupt request");

  s_locals.GPUSTAT.interrupt_request = true;
  InterruptController::SetLineState(InterruptController::IRQ::GPU, true);

  s_locals.fifo.RemoveOne();
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawModeCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  DEBUG_LOG("Set draw mode {:08X}", param);
  SetDrawMode(Truncate16(param));
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetTextureWindowCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  SetTextureWindow(param);
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawingAreaTopLeftCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  const u32 left = param & DRAWING_AREA_COORD_MASK;
  const u32 top = (param >> 10) & DRAWING_AREA_COORD_MASK;
  DEBUG_LOG("Set drawing area top-left: ({}, {})", left, top);
  if (s_locals.drawing_area.left != left || s_locals.drawing_area.top != top)
  {
    s_locals.drawing_area.left = left;
    s_locals.drawing_area.top = top;
    s_locals.drawing_area_changed = true;
    SetClampedDrawingArea();
  }

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawingAreaBottomRightCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;

  const u32 right = param & DRAWING_AREA_COORD_MASK;
  const u32 bottom = (param >> 10) & DRAWING_AREA_COORD_MASK;
  DEBUG_LOG("Set drawing area bottom-right: ({}, {})", right, bottom);
  if (s_locals.drawing_area.right != right || s_locals.drawing_area.bottom != bottom)
  {
    s_locals.drawing_area.right = right;
    s_locals.drawing_area.bottom = bottom;
    s_locals.drawing_area_changed = true;
    SetClampedDrawingArea();
  }

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawingOffsetCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  const s32 x = SignExtendN<11, s32>(param & 0x7FFu);
  const s32 y = SignExtendN<11, s32>((param >> 11) & 0x7FFu);
  DEBUG_LOG("Set drawing offset ({}, {})", x, y);
  if (s_locals.drawing_offset.x != x || s_locals.drawing_offset.y != y)
  {
    s_locals.drawing_offset.x = x;
    s_locals.drawing_offset.y = y;
  }

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetMaskBitCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;

  constexpr u32 gpustat_mask = (1 << 11) | (1 << 12);
  const u32 gpustat_bits = (param & 0x03) << 11;
  s_locals.GPUSTAT.bits = (s_locals.GPUSTAT.bits & ~gpustat_mask) | gpustat_bits;
  DEBUG_LOG("Set mask bit {} {}", BoolToUInt32(s_locals.GPUSTAT.set_mask_while_drawing),
            BoolToUInt32(s_locals.GPUSTAT.check_mask_before_draw));

  AddCommandTicks(1);
  EndCommand();
  return true;
}

void GPU::PrepareForDraw()
{
  if (s_locals.drawing_area_changed)
  {
    s_locals.drawing_area_changed = false;
    GPUBackendSetDrawingAreaCommand* cmd = GPUBackend::NewSetDrawingAreaCommand();
    cmd->new_area = s_locals.drawing_area;
    GPUBackend::PushCommand(cmd);
  }
}

void GPU::FillDrawCommand(GPUBackendDrawCommand* RESTRICT cmd, GPURenderCommand rc)
{
  cmd->interlaced_rendering = IsInterlacedRenderingEnabled();
  cmd->active_line_lsb = ConvertToBoolUnchecked(s_locals.crtc_state.active_line_lsb);
  cmd->check_mask_before_draw = s_locals.GPUSTAT.check_mask_before_draw;
  cmd->set_mask_while_drawing = s_locals.GPUSTAT.set_mask_while_drawing;
  cmd->texture_enable = rc.IsTexturingEnabled();
  cmd->raw_texture_enable = rc.raw_texture_enable;
  cmd->transparency_enable = rc.transparency_enable;
  cmd->shading_enable = rc.shading_enable;
  cmd->quad_polygon = rc.quad_polygon;
  cmd->dither_enable = rc.IsDitheringEnabled() && s_locals.draw_mode.mode_reg.dither_enable;

  cmd->draw_mode.bits = s_locals.draw_mode.mode_reg.bits;
  cmd->palette.bits = s_locals.draw_mode.palette_reg.bits;
  cmd->window = s_locals.draw_mode.texture_window;
}

ALWAYS_INLINE u32 GPU::GetPolyLineVertexCount()
{
  return (static_cast<u32>(s_locals.polyline_buffer.size()) + BoolToUInt32(s_locals.render_command.shading_enable)) >>
         BoolToUInt8(s_locals.render_command.shading_enable);
}

ALWAYS_INLINE_RELEASE void GPU::AddDrawTriangleTicks(GSVector2i v1, GSVector2i v2, GSVector2i v3, bool shaded,
                                                     bool textured, bool semitransparent)
{
  // This will not produce the correct results for triangles which are partially outside the clip area.
  // However, usually it'll undershoot not overshoot. If we wanted to make this more accurate, we'd need to intersect
  // the edges with the clip rectangle.
  // TODO: Coordinates are exclusive, so off by one here...
  const GSVector2i clamp_min = GSVector2i::load<true>(&s_locals.clamped_drawing_area.x);
  const GSVector2i clamp_max = GSVector2i::load<true>(&s_locals.clamped_drawing_area.z);
  v1 = v1.sat_s32(clamp_min, clamp_max);
  v2 = v2.sat_s32(clamp_min, clamp_max);
  v3 = v3.sat_s32(clamp_min, clamp_max);

  TickCount pixels = std::abs((v1.x * v2.y + v2.x * v3.y + v3.x * v1.y - v1.x * v3.y - v2.x * v1.y - v3.x * v2.y) / 2);
  if (textured)
    pixels += pixels;
  if (semitransparent || s_locals.GPUSTAT.check_mask_before_draw)
    pixels += (pixels + 1) / 2;
  if (s_locals.GPUSTAT.SkipDrawingToActiveField())
    pixels /= 2;

  AddCommandTicks(pixels);
}

ALWAYS_INLINE_RELEASE void GPU::AddDrawRectangleTicks(const GSVector4i rect, bool textured, bool semitransparent)
{
  const GSVector4i clamped_rect = s_locals.clamped_drawing_area.rintersect(rect);

  u32 drawn_width = clamped_rect.width();
  u32 drawn_height = clamped_rect.height();

  u32 ticks_per_row = drawn_width;
  if (textured)
  {
    switch (s_locals.draw_mode.mode_reg.texture_mode)
    {
      case GPUTextureMode::Palette4Bit:
        ticks_per_row += drawn_width;
        break;

      case GPUTextureMode::Palette8Bit:
      {
        // Texture cache reload every 2 pixels, reads in 8 bytes (assuming 4x2). Cache only reloads if the
        // draw width is greater than 128, otherwise the cache hits between rows.
        if (drawn_width > 128)
          ticks_per_row += (drawn_width / 4) * 8;
        else if ((drawn_width * drawn_height) > 2048)
          ticks_per_row += ((drawn_width / 4) * (4 * (128 / drawn_width)));
        else
          ticks_per_row += drawn_width;
      }
      break;

      case GPUTextureMode::Direct16Bit:
      case GPUTextureMode::Reserved_Direct16Bit:
      {
        // Same as above, except with 2x2 blocks instead of 4x2.
        if (drawn_width > 128)
          ticks_per_row += (drawn_width / 2) * 8;
        else if ((drawn_width * drawn_height) > 1024)
          ticks_per_row += ((drawn_width / 4) * (8 * (128 / drawn_width)));
        else
          ticks_per_row += drawn_width;
      }
      break;

        DefaultCaseIsUnreachable()
    }
  }

  if (semitransparent || s_locals.GPUSTAT.check_mask_before_draw)
    ticks_per_row += (drawn_width + 1u) / 2u;
  if (s_locals.GPUSTAT.SkipDrawingToActiveField())
    drawn_height = std::max<u32>(drawn_height / 2, 1u);

  AddCommandTicks(ticks_per_row * drawn_height);
}

ALWAYS_INLINE_RELEASE void GPU::AddDrawLineTicks(const GSVector4i rect, bool shaded)
{
  const GSVector4i clamped_rect = rect.rintersect(s_locals.clamped_drawing_area);

  // Needed because we're not multiplying either dimension.
  if (clamped_rect.rempty())
    return;

  const u32 drawn_width = clamped_rect.width();
  u32 drawn_height = clamped_rect.height();

  if (s_locals.GPUSTAT.SkipDrawingToActiveField())
    drawn_height = std::max<u32>(drawn_height / 2, 1u);

  AddCommandTicks(std::max(drawn_width, drawn_height));
}

bool GPU::HandleRenderPolygonCommand()
{
  const GPURenderCommand rc{FifoPeek(0)};

  // shaded vertices use the colour from the first word for the first vertex
  const u32 words_per_vertex = 1 + BoolToUInt32(rc.texture_enable) + BoolToUInt32(rc.shading_enable);
  const u32 num_vertices = rc.quad_polygon ? 4 : 3;
  const u32 total_words = words_per_vertex * num_vertices + BoolToUInt32(!rc.shading_enable);
  CHECK_COMMAND_SIZE(total_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  // setup time
  static constexpr u16 s_setup_time[2][2][2] = {{{46, 226}, {334, 496}}, {{82, 262}, {370, 532}}};
  const TickCount setup_ticks = static_cast<TickCount>(ZeroExtend32(
    s_setup_time[BoolToUInt8(rc.quad_polygon)][BoolToUInt8(rc.shading_enable)][BoolToUInt8(rc.texture_enable)]));
  AddCommandTicks(setup_ticks);

  TRACE_LOG("Render {} {} {} {} polygon ({} verts, {} words per vert), {} setup ticks",
            rc.quad_polygon ? "four-point" : "three-point", rc.transparency_enable ? "semi-transparent" : "opaque",
            rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome", num_vertices,
            words_per_vertex, setup_ticks);

  // set draw state up
  // TODO: Get rid of SetTexturePalette() and just fill it as needed
  if (rc.texture_enable)
  {
    const u16 texpage_attribute = Truncate16((rc.shading_enable ? FifoPeek(5) : FifoPeek(4)) >> 16);
    SetDrawMode((texpage_attribute & GPUDrawModeReg::POLYGON_TEXPAGE_MASK) |
                (s_locals.draw_mode.mode_reg.bits & ~GPUDrawModeReg::POLYGON_TEXPAGE_MASK));
    SetTexturePalette(Truncate16(FifoPeek(2) >> 16));
    UpdateCLUTIfNeeded(s_locals.draw_mode.mode_reg.texture_mode, s_locals.draw_mode.palette_reg);
  }

  s_locals.render_command.bits = rc.bits;
  s_locals.fifo.RemoveOne();

  PrepareForDraw();

  if (g_settings.gpu_pgxp_enable)
  {
    GPUBackendDrawPrecisePolygonCommand* RESTRICT cmd = GPUBackend::NewDrawPrecisePolygonCommand(num_vertices);
    FillDrawCommand(cmd, rc);
    cmd->num_vertices = Truncate16(num_vertices);

    const u32 first_color = rc.color_for_first_vertex;
    const bool shaded = rc.shading_enable;
    const bool textured = rc.texture_enable;
    bool valid_w = g_settings.gpu_pgxp_texture_correction;
    for (u32 i = 0; i < num_vertices; i++)
    {
      GPUBackendDrawPrecisePolygonCommand::Vertex* RESTRICT vert = &cmd->vertices[i];
      vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
      const u64 maddr_and_pos = s_locals.fifo.Pop();
      const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
      vert->native_x = s_locals.drawing_offset.x + vp.x;
      vert->native_y = s_locals.drawing_offset.y + vp.y;
      vert->texcoord = textured ? Truncate16(FifoPop()) : 0;

      valid_w &=
        CPU::PGXP::GetPreciseVertex(Truncate32(maddr_and_pos >> 32), vp.bits, vert->native_x, vert->native_y,
                                    s_locals.drawing_offset.x, s_locals.drawing_offset.y, &vert->x, &vert->y, &vert->w);
    }

    cmd->valid_w = valid_w;
    if (!valid_w)
    {
      if (g_settings.gpu_pgxp_disable_2d)
      {
        // NOTE: This reads uninitialized data, but it's okay, it doesn't get used.
        for (u32 i = 0; i < num_vertices; i++)
        {
          GPUBackendDrawPrecisePolygonCommand::Vertex& v = cmd->vertices[i];
          GSVector2::store<false>(&v.x, GSVector2(GSVector2i::load<false>(&v.native_x)));
          v.w = 1.0f;
        }
      }
      else
      {
        for (u32 i = 0; i < num_vertices; i++)
          cmd->vertices[i].w = 1.0f;
      }
    }

    // Cull polygons which are too large.
    const GSVector2i v0 = GSVector2i::load<false>(&cmd->vertices[0].native_x);
    const GSVector2i v1 = GSVector2i::load<false>(&cmd->vertices[1].native_x);
    const GSVector2i v2 = GSVector2i::load<false>(&cmd->vertices[2].native_x);
    const GSVector2i min_pos_12 = v1.min_s32(v2);
    const GSVector2i max_pos_12 = v1.max_s32(v2);
    const GSVector4i draw_rect_012 =
      GSVector4i::xyxy(min_pos_12.min_s32(v0), max_pos_12.max_s32(v0)).add32(GSVector4i::cxpr(0, 0, 1, 1));
    const bool first_tri_culled =
      (draw_rect_012.width() > MAX_PRIMITIVE_WIDTH || draw_rect_012.height() > MAX_PRIMITIVE_HEIGHT);
    if (first_tri_culled)
    {
      DEBUG_LOG("Culling too-large polygon: {},{} {},{} {},{}", cmd->vertices[0].native_x, cmd->vertices[0].native_y,
                cmd->vertices[1].native_x, cmd->vertices[1].native_y, cmd->vertices[2].native_x,
                cmd->vertices[2].native_y);

      if (!rc.quad_polygon)
      {
        EndCommand();
        return true;
      }
    }
    else
    {
      AddDrawTriangleTicks(GSVector2i::load<false>(&cmd->vertices[0].native_x),
                           GSVector2i::load<false>(&cmd->vertices[1].native_x),
                           GSVector2i::load<false>(&cmd->vertices[2].native_x), rc.shading_enable, rc.texture_enable,
                           rc.transparency_enable);
    }

    // quads
    if (rc.quad_polygon)
    {
      const GSVector2i v3 = GSVector2i::load<false>(&cmd->vertices[3].native_x);
      const GSVector4i draw_rect_123 = GSVector4i(min_pos_12.min_s32(v3))
                                         .upl64(GSVector4i(max_pos_12.max_s32(v3)))
                                         .add32(GSVector4i::cxpr(0, 0, 1, 1));

      // Cull polygons which are too large.
      const bool second_tri_culled =
        (draw_rect_123.width() > MAX_PRIMITIVE_WIDTH || draw_rect_123.height() > MAX_PRIMITIVE_HEIGHT);
      if (second_tri_culled)
      {
        DEBUG_LOG("Culling too-large polygon (quad second half): {},{} {},{} {},{}", cmd->vertices[2].native_x,
                  cmd->vertices[2].native_y, cmd->vertices[1].native_x, cmd->vertices[1].native_y,
                  cmd->vertices[3].native_x, cmd->vertices[3].native_y);

        if (first_tri_culled)
        {
          EndCommand();
          return true;
        }

        // Remove second part of quad.
        cmd->size = VideoThreadCommand::AlignCommandSize(sizeof(GPUBackendDrawPrecisePolygonCommand) +
                                                         3 * sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
        cmd->num_vertices = 3;
      }
      else
      {
        AddDrawTriangleTicks(GSVector2i::load<false>(&cmd->vertices[2].native_x),
                             GSVector2i::load<false>(&cmd->vertices[1].native_x),
                             GSVector2i::load<false>(&cmd->vertices[3].native_x), rc.shading_enable, rc.texture_enable,
                             rc.transparency_enable);

        // If first part was culled, move the second part to the first.
        if (first_tri_culled)
        {
          std::memcpy(&cmd->vertices[0], &cmd->vertices[2], sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
          std::memcpy(&cmd->vertices[2], &cmd->vertices[3], sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
          cmd->size = VideoThreadCommand::AlignCommandSize(sizeof(GPUBackendDrawPrecisePolygonCommand) +
                                                           3 * sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
          cmd->num_vertices = 3;
        }
      }
    }

    GPUBackend::PushCommand(cmd);
  }
  else
  {
    GPUBackendDrawPolygonCommand* RESTRICT cmd = GPUBackend::NewDrawPolygonCommand(num_vertices);
    FillDrawCommand(cmd, rc);
    cmd->num_vertices = Truncate16(num_vertices);

    const u32 first_color = rc.color_for_first_vertex;
    const bool shaded = rc.shading_enable;
    const bool textured = rc.texture_enable;
    for (u32 i = 0; i < num_vertices; i++)
    {
      GPUBackendDrawPolygonCommand::Vertex* RESTRICT vert = &cmd->vertices[i];
      vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
      const u64 maddr_and_pos = s_locals.fifo.Pop();
      const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
      vert->x = s_locals.drawing_offset.x + vp.x;
      vert->y = s_locals.drawing_offset.y + vp.y;
      vert->texcoord = textured ? Truncate16(FifoPop()) : 0;
    }

    // Cull polygons which are too large.
    const GSVector2i v0 = GSVector2i::load<false>(&cmd->vertices[0].x);
    const GSVector2i v1 = GSVector2i::load<false>(&cmd->vertices[1].x);
    const GSVector2i v2 = GSVector2i::load<false>(&cmd->vertices[2].x);
    const GSVector2i min_pos_12 = v1.min_s32(v2);
    const GSVector2i max_pos_12 = v1.max_s32(v2);
    const GSVector4i draw_rect_012 =
      GSVector4i::xyxy(min_pos_12.min_s32(v0), max_pos_12.max_s32(v0)).add32(GSVector4i::cxpr(0, 0, 1, 1));
    const bool first_tri_culled =
      (draw_rect_012.width() > MAX_PRIMITIVE_WIDTH || draw_rect_012.height() > MAX_PRIMITIVE_HEIGHT);
    if (first_tri_culled)
    {
      DEBUG_LOG("Culling too-large polygon: {},{} {},{} {},{}", cmd->vertices[0].x, cmd->vertices[0].y,
                cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[2].x, cmd->vertices[2].y);

      if (!rc.quad_polygon)
      {
        EndCommand();
        return true;
      }
    }
    else
    {
      AddDrawTriangleTicks(v0, v1, v2, rc.shading_enable, rc.texture_enable, rc.transparency_enable);
    }

    // quads
    if (rc.quad_polygon)
    {
      const GSVector2i v3 = GSVector2i::load<false>(&cmd->vertices[3].x);
      const GSVector4i draw_rect_123 = GSVector4i(min_pos_12.min_s32(v3))
                                         .upl64(GSVector4i(max_pos_12.max_s32(v3)))
                                         .add32(GSVector4i::cxpr(0, 0, 1, 1));

      // Cull polygons which are too large.
      const bool second_tri_culled =
        (draw_rect_123.width() > MAX_PRIMITIVE_WIDTH || draw_rect_123.height() > MAX_PRIMITIVE_HEIGHT);
      if (second_tri_culled)
      {
        DEBUG_LOG("Culling too-large polygon (quad second half): {},{} {},{} {},{}", cmd->vertices[2].x,
                  cmd->vertices[2].y, cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[3].x, cmd->vertices[3].y);

        if (first_tri_culled)
        {
          EndCommand();
          return true;
        }

        // Remove second part of quad.
        cmd->size = VideoThreadCommand::AlignCommandSize(sizeof(GPUBackendDrawPolygonCommand) +
                                                         3 * sizeof(GPUBackendDrawPolygonCommand::Vertex));
        cmd->num_vertices = 3;
      }
      else
      {
        AddDrawTriangleTicks(v2, v1, v3, rc.shading_enable, rc.texture_enable, rc.transparency_enable);

        // If first part was culled, move the second part to the first.
        if (first_tri_culled)
        {
          std::memcpy(&cmd->vertices[0], &cmd->vertices[2], sizeof(GPUBackendDrawPolygonCommand::Vertex));
          std::memcpy(&cmd->vertices[2], &cmd->vertices[3], sizeof(GPUBackendDrawPolygonCommand::Vertex));
          cmd->size = VideoThreadCommand::AlignCommandSize(sizeof(GPUBackendDrawPolygonCommand) +
                                                           3 * sizeof(GPUBackendDrawPolygonCommand::Vertex));
          cmd->num_vertices = 3;
        }
      }
    }

    GPUBackend::PushCommand(cmd);
  }

  EndCommand();
  return true;
}

bool GPU::HandleRenderRectangleCommand()
{
  const GPURenderCommand rc{FifoPeek(0)};
  const u32 total_words =
    2 + BoolToUInt32(rc.texture_enable) + BoolToUInt32(rc.rectangle_size == GPUDrawRectangleSize::Variable);

  CHECK_COMMAND_SIZE(total_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  if (rc.texture_enable)
  {
    SetTexturePalette(Truncate16(FifoPeek(2) >> 16));
    UpdateCLUTIfNeeded(s_locals.draw_mode.mode_reg.texture_mode, s_locals.draw_mode.palette_reg);
  }

  const TickCount setup_ticks = 16;
  AddCommandTicks(setup_ticks);

  TRACE_LOG("Render {} {} {} rectangle ({} words), {} setup ticks",
            rc.transparency_enable ? "semi-transparent" : "opaque", rc.texture_enable ? "textured" : "non-textured",
            rc.shading_enable ? "shaded" : "monochrome", total_words, setup_ticks);

  s_locals.render_command.bits = rc.bits;
  s_locals.fifo.RemoveOne();

  PrepareForDraw();
  GPUBackendDrawRectangleCommand* cmd = GPUBackend::NewDrawRectangleCommand();
  FillDrawCommand(cmd, rc);
  cmd->color = rc.color_for_first_vertex;

  const GPUVertexPosition vp{FifoPop()};
  cmd->x = TruncateGPUVertexPosition(s_locals.drawing_offset.x + vp.x);
  cmd->y = TruncateGPUVertexPosition(s_locals.drawing_offset.y + vp.y);

  if (rc.texture_enable)
  {
    const u32 texcoord_and_palette = FifoPop();
    cmd->palette.bits = Truncate16(texcoord_and_palette >> 16);
    cmd->texcoord = Truncate16(texcoord_and_palette);
  }
  else
  {
    cmd->palette.bits = 0;
    cmd->texcoord = 0;
  }

  switch (rc.rectangle_size)
  {
    case GPUDrawRectangleSize::R1x1:
      cmd->width = 1;
      cmd->height = 1;
      break;
    case GPUDrawRectangleSize::R8x8:
      cmd->width = 8;
      cmd->height = 8;
      break;
    case GPUDrawRectangleSize::R16x16:
      cmd->width = 16;
      cmd->height = 16;
      break;
    default:
    {
      const u32 width_and_height = FifoPop();
      cmd->width = static_cast<u16>(width_and_height & VRAM_WIDTH_MASK);
      cmd->height = static_cast<u16>((width_and_height >> 16) & VRAM_HEIGHT_MASK);
    }
    break;
  }

  const GSVector4i rect = GSVector4i(cmd->x, cmd->y, cmd->x + cmd->width, cmd->y + cmd->height);
  AddDrawRectangleTicks(rect, rc.texture_enable, rc.transparency_enable);

  GPUBackend::PushCommand(cmd);
  EndCommand();
  return true;
}

bool GPU::HandleRenderLineCommand()
{
  const GPURenderCommand rc{FifoPeek(0)};
  const u32 total_words = rc.shading_enable ? 4 : 3;
  CHECK_COMMAND_SIZE(total_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  TRACE_LOG("Render {} {} line ({} total words)", rc.transparency_enable ? "semi-transparent" : "opaque",
            rc.shading_enable ? "shaded" : "monochrome", total_words);

  s_locals.render_command.bits = rc.bits;
  s_locals.fifo.RemoveOne();

  PrepareForDraw();

  if (g_settings.gpu_pgxp_enable)
  {
    GPUBackendDrawPreciseLineCommand* RESTRICT cmd = GPUBackend::NewDrawPreciseLineCommand(2);
    FillDrawCommand(cmd, rc);
    cmd->palette.bits = 0;

    bool valid_w = g_settings.gpu_pgxp_texture_correction;
    for (u32 i = 0; i < 2; i++)
    {
      const u32 color = ((i != 0 && rc.shading_enable) ? FifoPop() : rc.bits) & UINT32_C(0x00FFFFFF);
      const u64 maddr_and_pos = s_locals.fifo.Pop();
      const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
      GPUBackendDrawPreciseLineCommand::Vertex* RESTRICT vert = &cmd->vertices[i];
      vert->native_x = s_locals.drawing_offset.x + vp.x;
      vert->native_y = s_locals.drawing_offset.y + vp.y;
      vert->color = color;

      valid_w &=
        CPU::PGXP::GetPreciseVertex(Truncate32(maddr_and_pos >> 32), vp.bits, vert->native_x, vert->native_y,
                                    s_locals.drawing_offset.x, s_locals.drawing_offset.y, &vert->x, &vert->y, &vert->w);
    }
    if (!(cmd->valid_w = valid_w))
    {
      for (u32 i = 0; i < 2; i++)
        cmd->vertices[i].w = 1.0f;
    }

    const GSVector2i v0 = GSVector2i::load<false>(&cmd->vertices[0].native_x);
    const GSVector2i v1 = GSVector2i::load<false>(&cmd->vertices[1].native_x);
    const GSVector4i rect = GSVector4i::xyxy(v0.min_s32(v1), v0.max_s32(v1)).add32(GSVector4i::cxpr(0, 0, 1, 1));
    if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT)
    {
      DEBUG_LOG("Culling too-large line: {} - {}", v0, v1);
      EndCommand();
      return true;
    }

    AddDrawLineTicks(rect, rc.shading_enable);
    GPUBackend::PushCommand(cmd);
  }
  else
  {
    GPUBackendDrawLineCommand* RESTRICT cmd = GPUBackend::NewDrawLineCommand(2);
    FillDrawCommand(cmd, rc);
    cmd->palette.bits = 0;

    if (rc.shading_enable)
    {
      cmd->vertices[0].color = rc.color_for_first_vertex;
      const GPUVertexPosition start_pos{FifoPop()};
      cmd->vertices[0].x = s_locals.drawing_offset.x + start_pos.x;
      cmd->vertices[0].y = s_locals.drawing_offset.y + start_pos.y;

      cmd->vertices[1].color = FifoPop() & UINT32_C(0x00FFFFFF);
      const GPUVertexPosition end_pos{FifoPop()};
      cmd->vertices[1].x = s_locals.drawing_offset.x + end_pos.x;
      cmd->vertices[1].y = s_locals.drawing_offset.y + end_pos.y;
    }
    else
    {
      cmd->vertices[0].color = rc.color_for_first_vertex;
      cmd->vertices[1].color = rc.color_for_first_vertex;

      const GPUVertexPosition start_pos{FifoPop()};
      cmd->vertices[0].x = s_locals.drawing_offset.x + start_pos.x;
      cmd->vertices[0].y = s_locals.drawing_offset.y + start_pos.y;

      const GPUVertexPosition end_pos{FifoPop()};
      cmd->vertices[1].x = s_locals.drawing_offset.x + end_pos.x;
      cmd->vertices[1].y = s_locals.drawing_offset.y + end_pos.y;
    }

    const GSVector2i v0 = GSVector2i::load<false>(&cmd->vertices[0].x);
    const GSVector2i v1 = GSVector2i::load<false>(&cmd->vertices[1].x);
    const GSVector4i rect = GSVector4i::xyxy(v0.min_s32(v1), v0.max_s32(v1)).add32(GSVector4i::cxpr(0, 0, 1, 1));
    if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT)
    {
      DEBUG_LOG("Culling too-large line: {} - {}", v0, v1);
      EndCommand();
      return true;
    }

    AddDrawLineTicks(rect, rc.shading_enable);
    GPUBackend::PushCommand(cmd);
  }

  EndCommand();
  return true;
}

bool GPU::HandleRenderPolyLineCommand()
{
  // always read the first two vertices, we test for the terminator after that
  const GPURenderCommand rc{FifoPeek(0)};
  const u32 min_words = rc.shading_enable ? 3 : 4;
  CHECK_COMMAND_SIZE(min_words);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  const TickCount setup_ticks = 16;
  AddCommandTicks(setup_ticks);

  TRACE_LOG("Render {} {} poly-line, {} setup ticks", rc.transparency_enable ? "semi-transparent" : "opaque",
            rc.shading_enable ? "shaded" : "monochrome", setup_ticks);

  s_locals.render_command.bits = rc.bits;
  s_locals.fifo.RemoveOne();

  const u32 words_to_pop = min_words - 1;
  // m_blit_buffer.resize(words_to_pop);
  // FifoPopRange(m_blit_buffer.data(), words_to_pop);
  s_locals.polyline_buffer.reserve(words_to_pop);
  for (u32 i = 0; i < words_to_pop; i++)
    s_locals.polyline_buffer.push_back(s_locals.fifo.Pop());

  // polyline goes via a different path through the blit buffer
  s_locals.blitter_state = BlitterState::DrawingPolyLine;
  s_locals.command_total_words = 0;
  return true;
}

void GPU::FinishPolyline()
{
  PrepareForDraw();

  const u32 num_vertices = GetPolyLineVertexCount();
  DebugAssert(num_vertices >= 2);

  if (g_settings.gpu_pgxp_enable)
  {
    GPUBackendDrawPreciseLineCommand* RESTRICT cmd = GPUBackend::NewDrawPreciseLineCommand((num_vertices - 1) * 2);
    FillDrawCommand(cmd, s_locals.render_command);
    cmd->palette.bits = 0;

    u32 buffer_pos = 0;
    u32 out_vertex_count = 0;
    const bool shaded = s_locals.render_command.shading_enable;
    bool valid_w = g_settings.gpu_pgxp_texture_correction;
    GPUBackendDrawPreciseLineCommand::Vertex start, end;

    const auto read_vertex = [&buffer_pos, &valid_w](GPUBackendDrawPreciseLineCommand::Vertex& RESTRICT dest,
                                                     u32 color) {
      const u64 maddr_and_pos = s_locals.polyline_buffer[buffer_pos++];
      const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
      dest.native_x = s_locals.drawing_offset.x + vp.x;
      dest.native_y = s_locals.drawing_offset.y + vp.y;
      dest.color = color;
      valid_w &=
        CPU::PGXP::GetPreciseVertex(Truncate32(maddr_and_pos >> 32), vp.bits, dest.native_x, dest.native_y,
                                    s_locals.drawing_offset.x, s_locals.drawing_offset.y, &dest.x, &dest.y, &dest.w);
    };

    read_vertex(start, s_locals.render_command.color_for_first_vertex);

    for (u32 i = 1; i < num_vertices; i++)
    {
      const u32 color = (shaded ? Truncate32(s_locals.polyline_buffer[buffer_pos++]) : s_locals.render_command.bits) &
                        UINT32_C(0x00FFFFFF);
      read_vertex(end, color);

      const GSVector2i start_pos = GSVector2i::load<false>(&start.native_x);
      const GSVector2i end_pos = GSVector2i::load<false>(&end.native_x);
      const GSVector4i rect =
        GSVector4i::xyxy(start_pos.min_s32(end_pos), start_pos.max_s32(end_pos)).add32(GSVector4i::cxpr(0, 0, 1, 1));
      if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT)
      {
        DEBUG_LOG("Culling too-large line: {} - {}", start_pos, end_pos);
      }
      else
      {
        AddDrawLineTicks(rect, s_locals.render_command.shading_enable);

        cmd->vertices[out_vertex_count++] = start;
        cmd->vertices[out_vertex_count++] = end;
      }

      start = end;
    }

    if (out_vertex_count > 0)
    {
      DebugAssert(out_vertex_count <= cmd->num_vertices);
      cmd->num_vertices = Truncate16(out_vertex_count);
      GPUBackend::PushCommand(cmd);
    }
  }
  else
  {
    GPUBackendDrawLineCommand* RESTRICT cmd = GPUBackend::NewDrawLineCommand((num_vertices - 1) * 2);
    FillDrawCommand(cmd, s_locals.render_command);
    cmd->palette.bits = 0;

    u32 buffer_pos = 0;
    const GPUVertexPosition start_vp{Truncate32(s_locals.polyline_buffer[buffer_pos++])};
    const GSVector2i draw_offset = GSVector2i::load<false>(&s_locals.drawing_offset.x);
    GSVector2i start_pos = GSVector2i(start_vp.x, start_vp.y).add32(draw_offset);
    u32 start_color = s_locals.render_command.color_for_first_vertex;

    const bool shaded = s_locals.render_command.shading_enable;
    u32 out_vertex_count = 0;
    for (u32 i = 1; i < num_vertices; i++)
    {
      const u32 end_color = shaded ? (Truncate32(s_locals.polyline_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF))) :
                                     s_locals.render_command.color_for_first_vertex;
      const GPUVertexPosition vp{Truncate32(s_locals.polyline_buffer[buffer_pos++])};
      const GSVector2i end_pos = GSVector2i(vp.x, vp.y).add32(draw_offset);

      const GSVector4i rect =
        GSVector4i::xyxy(start_pos.min_s32(end_pos), start_pos.max_s32(end_pos)).add32(GSVector4i::cxpr(0, 0, 1, 1));
      if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT)
      {
        DEBUG_LOG("Culling too-large line: {},{} - {},{}", start_pos.x, start_pos.y, end_pos.x, end_pos.y);
      }
      else
      {
        AddDrawLineTicks(rect, s_locals.render_command.shading_enable);

        GPUBackendDrawLineCommand::Vertex* out_vertex = &cmd->vertices[out_vertex_count];
        out_vertex_count += 2;

        GSVector2i::store<false>(&out_vertex[0].x, start_pos);
        out_vertex[0].color = start_color;
        GSVector2i::store<false>(&out_vertex[1].x, end_pos);
        out_vertex[1].color = end_color;
      }

      start_pos = end_pos;
      start_color = end_color;
    }

    if (out_vertex_count > 0)
    {
      DebugAssert(out_vertex_count <= cmd->num_vertices);
      cmd->num_vertices = Truncate16(out_vertex_count);
      GPUBackend::PushCommand(cmd);
    }
  }
}

bool GPU::HandleFillRectangleCommand()
{
  CHECK_COMMAND_SIZE(3);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  const u32 color = FifoPop() & 0x00FFFFFF;
  const u32 dst_x = FifoPeek() & 0x3F0;
  const u32 dst_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 width = ((FifoPeek() & VRAM_WIDTH_MASK) + 0xF) & ~0xF;
  const u32 height = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;

  DEBUG_LOG("Fill VRAM rectangle offset=({},{}), size=({},{})", dst_x, dst_y, width, height);

  if (width > 0 && height > 0)
  {
    GPUBackendFillVRAMCommand* cmd = GPUBackend::NewFillVRAMCommand();
    cmd->x = static_cast<u16>(dst_x);
    cmd->y = static_cast<u16>(dst_y);
    cmd->width = static_cast<u16>(width);
    cmd->height = static_cast<u16>(height);
    cmd->color = color;
    cmd->interlaced_rendering = IsInterlacedRenderingEnabled();
    cmd->active_line_lsb = s_locals.crtc_state.active_line_lsb;
    GPUBackend::PushCommand(cmd);
  }

  AddCommandTicks(46 + ((width / 8) + 9) * height);
  EndCommand();
  return true;
}

bool GPU::HandleCopyRectangleCPUToVRAMCommand()
{
  CHECK_COMMAND_SIZE(3);
  s_locals.fifo.RemoveOne();

  const u32 coords = FifoPop();
  const u32 size = FifoPop();

  // Tenga Seiha does a bunch of completely-invalid VRAM writes on boot, then expects GPU idle to be set.
  // It's unclear what actually happens, I need to write another test, but for now, just skip these uploads.
  // Not setting GPU idle during the write command breaks Doom, so that's not an option.
  if (size == 0xFFFFFFFFu) [[unlikely]]
  {
    ERROR_LOG("Ignoring likely-invalid VRAM write to ({},{})", (coords & VRAM_WIDTH_MASK),
              ((coords >> 16) & VRAM_HEIGHT_MASK));
    return true;
  }

  const u32 dst_x = coords & VRAM_WIDTH_MASK;
  const u32 dst_y = (coords >> 16) & VRAM_HEIGHT_MASK;
  const u32 copy_width = ReplaceZero(size & VRAM_WIDTH_MASK, 0x400);
  const u32 copy_height = ReplaceZero((size >> 16) & VRAM_HEIGHT_MASK, 0x200);
  const u32 num_pixels = copy_width * copy_height;
  const u32 num_words = ((num_pixels + 1) / 2);

  DEBUG_LOG("Copy rectangle from CPU to VRAM offset=({},{}), size=({},{})", dst_x, dst_y, copy_width, copy_height);

  EndCommand();

  s_locals.blitter_state = BlitterState::WritingVRAM;
  s_locals.blit_buffer.reserve(num_words);
  s_locals.blit_remaining_words = num_words;
  s_locals.vram_transfer.x = Truncate16(dst_x);
  s_locals.vram_transfer.y = Truncate16(dst_y);
  s_locals.vram_transfer.width = Truncate16(copy_width);
  s_locals.vram_transfer.height = Truncate16(copy_height);
  return true;
}

void GPU::FinishVRAMWrite()
{
  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  if (s_locals.blit_remaining_words == 0)
  {
    if (g_settings.gpu_dump_cpu_to_vram_copies)
    {
      DumpVRAMToFile(fmt::format("{}" FS_OSPATH_SEPARATOR_STR "cpu_to_vram_copy_{}.png", EmuFolders::DataRoot,
                                 ++s_locals.cpu_to_vram_dump_id),
                     s_locals.vram_transfer.width, s_locals.vram_transfer.height,
                     sizeof(u16) * s_locals.vram_transfer.width, s_locals.blit_buffer.data(), true);
    }

    UpdateVRAM(s_locals.vram_transfer.x, s_locals.vram_transfer.y, s_locals.vram_transfer.width,
               s_locals.vram_transfer.height, s_locals.blit_buffer.data(), s_locals.GPUSTAT.set_mask_while_drawing,
               s_locals.GPUSTAT.check_mask_before_draw);
  }
  else
  {
    const u32 num_pixels = ZeroExtend32(s_locals.vram_transfer.width) * ZeroExtend32(s_locals.vram_transfer.height);
    const u32 num_words = (num_pixels + 1) / 2;
    const u32 transferred_words = num_words - s_locals.blit_remaining_words;
    const u32 transferred_pixels = transferred_words * 2;
    const u32 transferred_full_rows = transferred_pixels / s_locals.vram_transfer.width;
    const u32 transferred_width_last_row = transferred_pixels % s_locals.vram_transfer.width;

    WARNING_LOG("Partial VRAM write - transfer finished with {} of {} words remaining ({} full rows, {} last row)",
                s_locals.blit_remaining_words, num_words, transferred_full_rows, transferred_width_last_row);

    const u8* blit_ptr = reinterpret_cast<const u8*>(s_locals.blit_buffer.data());
    if (transferred_full_rows > 0)
    {
      UpdateVRAM(s_locals.vram_transfer.x, s_locals.vram_transfer.y, s_locals.vram_transfer.width,
                 static_cast<u16>(transferred_full_rows), blit_ptr, s_locals.GPUSTAT.set_mask_while_drawing,
                 s_locals.GPUSTAT.check_mask_before_draw);
      blit_ptr += (ZeroExtend32(s_locals.vram_transfer.width) * transferred_full_rows) * sizeof(u16);
    }
    if (transferred_width_last_row > 0)
    {
      UpdateVRAM(s_locals.vram_transfer.x, static_cast<u16>(s_locals.vram_transfer.y + transferred_full_rows),
                 static_cast<u16>(transferred_width_last_row), 1, blit_ptr, s_locals.GPUSTAT.set_mask_while_drawing,
                 s_locals.GPUSTAT.check_mask_before_draw);
    }
  }

  s_locals.blit_buffer.clear();
  s_locals.vram_transfer = {};
  s_locals.blitter_state = BlitterState::Idle;
}

bool GPU::HandleCopyRectangleVRAMToCPUCommand()
{
  CHECK_COMMAND_SIZE(3);
  s_locals.fifo.RemoveOne();

  s_locals.vram_transfer.x = Truncate16(FifoPeek() & VRAM_WIDTH_MASK);
  s_locals.vram_transfer.y = Truncate16((FifoPop() >> 16) & VRAM_HEIGHT_MASK);
  s_locals.vram_transfer.width = ((Truncate16(FifoPeek()) - 1) & VRAM_WIDTH_MASK) + 1;
  s_locals.vram_transfer.height = ((Truncate16(FifoPop() >> 16) - 1) & VRAM_HEIGHT_MASK) + 1;

  DEBUG_LOG("Copy rectangle from VRAM to CPU offset=({},{}), size=({},{})", s_locals.vram_transfer.x,
            s_locals.vram_transfer.y, s_locals.vram_transfer.width, s_locals.vram_transfer.height);
  DebugAssert(s_locals.vram_transfer.col == 0 && s_locals.vram_transfer.row == 0);

  // ensure VRAM shadow is up to date
  ReadVRAM(s_locals.vram_transfer.x, s_locals.vram_transfer.y, s_locals.vram_transfer.width,
           s_locals.vram_transfer.height);

  if (g_settings.gpu_dump_vram_to_cpu_copies)
  {
    DumpVRAMToFile(fmt::format("{}" FS_OSPATH_SEPARATOR_STR "vram_to_cpu_copy_{}.png", EmuFolders::DataRoot,
                               ++s_locals.vram_to_cpu_dump_id),
                   s_locals.vram_transfer.width, s_locals.vram_transfer.height, sizeof(u16) * VRAM_WIDTH,
                   &g_vram[s_locals.vram_transfer.y * VRAM_WIDTH + s_locals.vram_transfer.x], true);
  }

  // switch to pixel-by-pixel read state
  s_locals.blitter_state = BlitterState::ReadingVRAM;
  s_locals.command_total_words = 0;

  // toss the entire read in the recorded trace. we might want to change this to mirroring GPUREAD in the future..
  if (s_locals.gpu_dump) [[unlikely]]
    s_locals.gpu_dump->WriteDiscardVRAMRead(s_locals.vram_transfer.width, s_locals.vram_transfer.height);

  return true;
}

bool GPU::HandleCopyRectangleVRAMToVRAMCommand()
{
  CHECK_COMMAND_SIZE(4);
  s_locals.fifo.RemoveOne();

  const u32 src_x = FifoPeek() & VRAM_WIDTH_MASK;
  const u32 src_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 dst_x = FifoPeek() & VRAM_WIDTH_MASK;
  const u32 dst_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 width = ReplaceZero(FifoPeek() & VRAM_WIDTH_MASK, 0x400);
  const u32 height = ReplaceZero((FifoPop() >> 16) & VRAM_HEIGHT_MASK, 0x200);

  DEBUG_LOG("Copy rectangle from VRAM to VRAM src=({},{}), dst=({},{}), size=({},{})", src_x, src_y, dst_x, dst_y,
            width, height);

  // Some VRAM copies aren't going to do anything. Most games seem to send a 2x2 VRAM copy at the end of a frame.
  const bool skip_copy =
    width == 0 || height == 0 || (src_x == dst_x && src_y == dst_y && !s_locals.GPUSTAT.set_mask_while_drawing);
  if (!skip_copy)
  {
    GPUBackendCopyVRAMCommand* cmd = GPUBackend::NewCopyVRAMCommand();
    cmd->src_x = static_cast<u16>(src_x);
    cmd->src_y = static_cast<u16>(src_y);
    cmd->dst_x = static_cast<u16>(dst_x);
    cmd->dst_y = static_cast<u16>(dst_y);
    cmd->width = static_cast<u16>(width);
    cmd->height = static_cast<u16>(height);
    cmd->check_mask_before_draw = s_locals.GPUSTAT.check_mask_before_draw;
    cmd->set_mask_while_drawing = s_locals.GPUSTAT.set_mask_while_drawing;
    GPUBackend::PushCommand(cmd);
  }

  AddCommandTicks(width * height * 2);
  EndCommand();
  return true;
}

void GPU::DrawDebugStateWindow(float scale)
{
  if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr std::array<const char*, 5> state_strings = {
      {"Idle", "Reading VRAM", "Writing VRAM", "Drawing Polyline"}};

    ImGui::Text("State: %s", state_strings[static_cast<u8>(s_locals.blitter_state)]);
    ImGui::Text("Dither: %s", s_locals.GPUSTAT.dither_enable ? "Enabled" : "Disabled");
    ImGui::Text("Draw To Displayed Field: %s", s_locals.GPUSTAT.draw_to_displayed_field ? "Enabled" : "Disabled");
    ImGui::Text("Draw Set Mask Bit: %s", s_locals.GPUSTAT.set_mask_while_drawing ? "Yes" : "No");
    ImGui::Text("Draw To Masked Pixels: %s", s_locals.GPUSTAT.check_mask_before_draw ? "Yes" : "No");
    ImGui::Text("Reverse Flag: %s", s_locals.GPUSTAT.reverse_flag ? "Yes" : "No");
    ImGui::Text("Texture Disable: %s", s_locals.GPUSTAT.texture_disable ? "Yes" : "No");
    ImGui::Text("PAL Mode: %s", s_locals.GPUSTAT.pal_mode ? "Yes" : "No");
    ImGui::Text("Interrupt Request: %s", s_locals.GPUSTAT.interrupt_request ? "Yes" : "No");
    ImGui::Text("DMA Request: %s", s_locals.GPUSTAT.dma_data_request ? "Yes" : "No");
  }

  if (ImGui::CollapsingHeader("CRTC", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const auto& cs = s_locals.crtc_state;
    ImGui::Text("Clock: %s", (s_locals.console_is_pal ? (s_locals.GPUSTAT.pal_mode ? "PAL-on-PAL" : "NTSC-on-PAL") :
                                                        (s_locals.GPUSTAT.pal_mode ? "PAL-on-NTSC" : "NTSC-on-NTSC")));
    ImGui::Text("Horizontal Frequency: %.3f KHz", ComputeHorizontalFrequency() / 1000.0f);
    ImGui::Text("Vertical Frequency: %.3f Hz", ComputeVerticalFrequency());
    ImGui::Text("Dot Clock Divider: %u", cs.dot_clock_divider);
    ImGui::Text("Vertical Interlace: %s (%s field)", s_locals.GPUSTAT.vertical_interlace ? "Yes" : "No",
                cs.interlaced_field ? "odd" : "even");
    ImGui::Text("Current Scanline: %u (tick %u)", cs.current_scanline, cs.current_tick_in_scanline);
    ImGui::Text("Display Disable: %s", s_locals.GPUSTAT.display_disable ? "Yes" : "No");
    ImGui::Text("Displaying Odd Lines: %s", cs.active_line_lsb ? "Yes" : "No");
    ImGui::Text("Color Depth: %u-bit", s_locals.GPUSTAT.display_area_color_depth_24 ? 24 : 15);
    ImGui::Text("Start Offset in VRAM: (%u, %u)", cs.regs.X.GetValue(), cs.regs.Y.GetValue());
    ImGui::Text("Display Total: %u (%u) horizontal, %u vertical", cs.horizontal_total,
                cs.horizontal_total / cs.dot_clock_divider, cs.vertical_total);
    ImGui::Text("Configured Display Range: %u-%u (%u-%u), %u-%u", cs.regs.X1.GetValue(), cs.regs.X2.GetValue(),
                cs.regs.X1.GetValue() / cs.dot_clock_divider, cs.regs.X2.GetValue() / cs.dot_clock_divider,
                cs.regs.Y1.GetValue(), cs.regs.Y2.GetValue());
    ImGui::Text("Output Display Range: %u-%u (%u-%u), %u-%u", cs.horizontal_display_start, cs.horizontal_display_end,
                cs.horizontal_display_start / cs.dot_clock_divider, cs.horizontal_display_end / cs.dot_clock_divider,
                cs.vertical_display_start, cs.vertical_display_end);
    ImGui::Text("Cropping: %s", Settings::GetDisplayCropModeName(g_settings.display_crop_mode));
    ImGui::Text("Visible Display Range: %u-%u (%u-%u), %u-%u", cs.horizontal_visible_start, cs.horizontal_visible_end,
                cs.horizontal_visible_start / cs.dot_clock_divider, cs.horizontal_visible_end / cs.dot_clock_divider,
                cs.vertical_visible_start, cs.vertical_visible_end);
    ImGui::Text("Display Resolution: %ux%u", cs.display_width, cs.display_height);
    ImGui::Text("Display Origin: %u, %u", cs.display_origin_left, cs.display_origin_top);
    ImGui::Text("Displayed/Visible VRAM Portion: %ux%u @ (%u, %u)", cs.display_vram_width, cs.display_vram_height,
                cs.display_vram_left, cs.display_vram_top);
    ImGui::Text("Padding: Left=%d, Top=%d, Right=%d, Bottom=%d", cs.display_origin_left, cs.display_origin_top,
                cs.display_width - cs.display_vram_width - cs.display_origin_left,
                cs.display_height - cs.display_vram_height - cs.display_origin_top);
  }
}

GPUDump::Recorder* GPU::GetGPUDump()
{
  return s_locals.gpu_dump.get();
}

bool GPU::StartRecordingGPUDump(const char* path, u32 num_frames /* = 1 */)
{
  if (s_locals.gpu_dump)
    StopRecordingGPUDump();

  // if we're not dumping forever, compute the frame count based on the internal fps
  // +1 because we want to actually see the buffer swap...
  if (num_frames != 0)
  {
    num_frames =
      std::max(num_frames, static_cast<u32>(static_cast<float>(num_frames + 1) *
                                            std::ceil(PerformanceCounters::GetVPS() / PerformanceCounters::GetFPS())));
  }

  // ensure vram is up to date
  ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);

  std::string osd_key = fmt::format("GPUDump_{}", Path::GetFileName(path));
  Error error;
  s_locals.gpu_dump = GPUDump::Recorder::Create(path, System::GetGameSerial(), num_frames, &error);
  if (!s_locals.gpu_dump)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Error, std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format("{}\n{}", TRANSLATE_SV("GPU", "Failed to start GPU trace:"), error.GetDescription()));
    return false;
  }

  Host::AddIconOSDMessage(
    OSDMessageType::Quick, std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
    (num_frames != 0) ?
      fmt::format(TRANSLATE_FS("GPU", "Saving {0} frame GPU trace to '{1}'."), num_frames, Path::GetFileName(path)) :
      fmt::format(TRANSLATE_FS("GPU", "Saving multi-frame frame GPU trace to '{1}'."), num_frames,
                  Path::GetFileName(path)));

  // save screenshot to same location to identify it
  GPUBackend::RenderScreenshotToFile(Path::ReplaceExtension(path, "png"), DisplayScreenshotMode::ScreenResolution, 85,
                                     false);
  return true;
}

void GPU::StopRecordingGPUDump()
{
  if (!s_locals.gpu_dump)
    return;

  Error error;
  if (!s_locals.gpu_dump->Close(&error))
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Error, "GPUDump", ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format("{}\n{}", TRANSLATE_SV("GPU", "Failed to close GPU trace:"), error.GetDescription()));
    s_locals.gpu_dump.reset();
  }

  // Are we compressing the dump?
  const GPUDumpCompressionMode compress_mode =
    Settings::ParseGPUDumpCompressionMode(Core::GetTinyStringSettingValue("GPU", "DumpCompressionMode"))
      .value_or(Settings::DEFAULT_GPU_DUMP_COMPRESSION_MODE);
  std::string osd_key = fmt::format("GPUDump_{}", Path::GetFileName(s_locals.gpu_dump->GetPath()));
  if (compress_mode == GPUDumpCompressionMode::Disabled)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Info, "GPUDump", ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format(TRANSLATE_FS("GPU", "Saved GPU trace to '{}'."), Path::GetFileName(s_locals.gpu_dump->GetPath())));
    s_locals.gpu_dump.reset();
    return;
  }

  std::string source_path = s_locals.gpu_dump->GetPath();
  s_locals.gpu_dump.reset();

  Host::AddIconOSDMessage(
    OSDMessageType::Persistent, osd_key, ICON_EMOJI_CAMERA_WITH_FLASH,
    fmt::format(TRANSLATE_FS("GPU", "Compressing GPU trace '{}'..."), Path::GetFileName(source_path)));
  Host::QueueAsyncTask([compress_mode, source_path = std::move(source_path), osd_key = std::move(osd_key)]() mutable {
    Error error;
    if (GPUDump::Recorder::Compress(source_path, compress_mode, &error))
    {
      Host::AddIconOSDMessage(
        OSDMessageType::Info, std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
        fmt::format(TRANSLATE_FS("GPU", "Saved GPU trace to '{}'."), Path::GetFileName(source_path)));
    }
    else
    {
      Host::AddIconOSDMessage(
        OSDMessageType::Error, std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
        fmt::format("{}\n{}",
                    SmallString::from_format(TRANSLATE_FS("GPU", "Failed to save GPU trace to '{}':"),
                                             Path::GetFileName(source_path)),
                    error.GetDescription()));
    }
  });
}

void GPU::WriteCurrentVideoModeToDump(GPUDump::Recorder* dump)
{
  dump->WriteGP1Command(GP1Command::SetDisplayDisable, BoolToUInt32(s_locals.GPUSTAT.display_disable));
  dump->WriteGP1Command(GP1Command::SetDisplayStartAddress, s_locals.crtc_state.regs.display_address_start);
  dump->WriteGP1Command(GP1Command::SetHorizontalDisplayRange, s_locals.crtc_state.regs.horizontal_display_range);
  dump->WriteGP1Command(GP1Command::SetVerticalDisplayRange, s_locals.crtc_state.regs.vertical_display_range);
  dump->WriteGP1Command(GP1Command::SetAllowTextureDisable, BoolToUInt32(s_locals.set_texture_disable_mask));

  // display mode
  GP1SetDisplayMode dispmode = {};
  dispmode.horizontal_resolution_1 = s_locals.GPUSTAT.horizontal_resolution_1.GetValue();
  dispmode.vertical_resolution = s_locals.GPUSTAT.vertical_resolution.GetValue();
  dispmode.pal_mode = s_locals.GPUSTAT.pal_mode.GetValue();
  dispmode.display_area_color_depth = s_locals.GPUSTAT.display_area_color_depth_24.GetValue();
  dispmode.vertical_interlace = s_locals.GPUSTAT.vertical_interlace.GetValue();
  dispmode.horizontal_resolution_2 = s_locals.GPUSTAT.horizontal_resolution_2.GetValue();
  dispmode.reverse_flag = s_locals.GPUSTAT.reverse_flag.GetValue();
  dump->WriteGP1Command(GP1Command::SetDisplayMode, dispmode.bits);

  // texture window/texture page
  dump->WriteGP0Packet((0xE1u << 24) | ZeroExtend32(s_locals.draw_mode.mode_reg.bits));
  dump->WriteGP0Packet((0xE2u << 24) | s_locals.draw_mode.texture_window_value);

  // drawing area
  dump->WriteGP0Packet((0xE3u << 24) | static_cast<u32>(s_locals.drawing_area.left) |
                       (static_cast<u32>(s_locals.drawing_area.top) << 10));
  dump->WriteGP0Packet((0xE4u << 24) | static_cast<u32>(s_locals.drawing_area.right) |
                       (static_cast<u32>(s_locals.drawing_area.bottom) << 10));

  // drawing offset
  dump->WriteGP0Packet((0xE5u << 24) | (static_cast<u32>(s_locals.drawing_offset.x) & 0x7FFu) |
                       ((static_cast<u32>(s_locals.drawing_offset.y) & 0x7FFu) << 11));

  // mask bit
  dump->WriteGP0Packet((0xE6u << 24) | BoolToUInt32(s_locals.GPUSTAT.set_mask_while_drawing) |
                       (BoolToUInt32(s_locals.GPUSTAT.check_mask_before_draw) << 1));
}

void GPU::ProcessGPUDumpPacket(GPUDump::PacketType type, const std::span<const u32> data)
{
  const auto execute_all_commands = []() {
    do
    {
      s_locals.pending_command_ticks = 0;
      s_locals.command_tick_event.Deactivate();
      ExecuteCommands();
    } while (s_locals.pending_command_ticks > 0);
  };

  switch (type)
  {
    case GPUDump::PacketType::GPUPort0Data:
    {
      if (data.empty()) [[unlikely]]
      {
        WARNING_LOG("Empty GPU dump GP0 packet!");
        return;
      }

      if (data.size() == 1) [[unlikely]]
      {
        // direct GP0 write
        WriteRegister(0, data[0]);
        execute_all_commands();
      }
      else
      {
        // don't overflow the fifo...
        size_t current_word = 0;
        while (current_word < data.size())
        {
          // normally this would be constrained to the "real" fifo size, but VRAM updates also go through here
          // it's easier to just push everything in and execute
          const u32 block_size = std::min(s_locals.fifo.GetSpace(), static_cast<u32>(data.size() - current_word));
          if (block_size == 0)
          {
            ERROR_LOG("FIFO overflow while processing dump packet of {} words", data.size());
            break;
          }

          for (u32 i = 0; i < block_size; i++)
            s_locals.fifo.Push(ZeroExtend64(data[current_word++]));

          execute_all_commands();
          ;
        }
      }
    }
    break;

    case GPUDump::PacketType::GPUPort1Data:
    {
      if (data.size() != 1) [[unlikely]]
      {
        WARNING_LOG("Incorrectly-sized GPU dump GP1 packet: {} words", data.size());
        return;
      }

      WriteRegister(4, data[0]);
    }
    break;

    case GPUDump::PacketType::VSyncEvent:
    {
      // don't play silly buggers with events
      execute_all_commands();

      // we _should_ be using the tick count for the event, but it breaks with looping.
      // instead, just add a fixed amount
      const TickCount crtc_ticks_per_frame = static_cast<TickCount>(s_locals.crtc_state.horizontal_total) *
                                             static_cast<TickCount>(s_locals.crtc_state.vertical_total);
      const TickCount system_ticks_per_frame =
        CRTCTicksToSystemTicks(crtc_ticks_per_frame, s_locals.crtc_state.fractional_ticks);
      SystemTicksToCRTCTicks(system_ticks_per_frame, &s_locals.crtc_state.fractional_ticks);
      TimingEvents::SetGlobalTickCounter(TimingEvents::GetGlobalTickCounter() +
                                         static_cast<GlobalTicks>(system_ticks_per_frame));
      System::IncrementFrameNumber();
      UpdateDisplay(true);
      System::FrameDone();
    }
    break;

    default:
      break;
  }
}
