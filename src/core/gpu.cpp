#include "gpu.h"
#include "common/file_system.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "host_display.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "stb_image_write.h"
#include "system.h"
#include "timers.h"
#include <cmath>
#ifdef WITH_IMGUI
#include "imgui.h"
#endif
Log_SetChannel(GPU);

std::unique_ptr<GPU> g_gpu;

const GPU::GP0CommandHandlerTable GPU::s_GP0_command_handler_table = GPU::GenerateGP0CommandHandlerTable();

GPU::GPU() = default;

GPU::~GPU() = default;

bool GPU::Initialize(HostDisplay* host_display)
{
  m_host_display = host_display;
  m_force_progressive_scan = g_settings.gpu_disable_interlacing;
  m_force_ntsc_timings = g_settings.gpu_force_ntsc_timings;
  m_crtc_tick_event = TimingEvents::CreateTimingEvent(
    "GPU CRTC Tick", 1, 1, std::bind(&GPU::CRTCTickEvent, this, std::placeholders::_1), true);
  m_command_tick_event = TimingEvents::CreateTimingEvent(
    "GPU Command Tick", 1, 1, std::bind(&GPU::CommandTickEvent, this, std::placeholders::_1), true);
  m_fifo_size = g_settings.gpu_fifo_size;
  m_max_run_ahead = g_settings.gpu_max_run_ahead;
  m_console_is_pal = System::IsPALRegion();
  UpdateCRTCConfig();
  return true;
}

void GPU::UpdateSettings()
{
  m_force_progressive_scan = g_settings.gpu_disable_interlacing;
  m_fifo_size = g_settings.gpu_fifo_size;
  m_max_run_ahead = g_settings.gpu_max_run_ahead;

  if (m_force_ntsc_timings != g_settings.gpu_force_ntsc_timings || m_console_is_pal != System::IsPALRegion())
  {
    m_force_ntsc_timings = g_settings.gpu_force_ntsc_timings;
    m_console_is_pal = System::IsPALRegion();
    UpdateCRTCConfig();
  }

  // Crop mode calls this, so recalculate the display area
  UpdateCRTCDisplayParameters();
}

void GPU::CPUClockChanged()
{
  UpdateCRTCConfig();
}

void GPU::UpdateResolutionScale() {}

std::tuple<u32, u32> GPU::GetEffectiveDisplayResolution()
{
  return std::tie(m_crtc_state.display_vram_width, m_crtc_state.display_vram_height);
}

void GPU::Reset()
{
  SoftReset();
  m_set_texture_disable_mask = false;
  m_GPUREAD_latch = 0;
}

void GPU::SoftReset()
{
  FlushRender();
  if (m_blitter_state == BlitterState::WritingVRAM)
    FinishVRAMWrite();

  m_GPUSTAT.bits = 0x14802000;
  m_GPUSTAT.pal_mode = System::IsPALRegion();
  m_drawing_area.Set(0, 0, 0, 0);
  m_drawing_area_changed = true;
  m_drawing_offset = {};
  std::memset(&m_crtc_state.regs, 0, sizeof(m_crtc_state.regs));
  m_crtc_state.regs.horizontal_display_range = 0xC60260;
  m_crtc_state.regs.vertical_display_range = 0x3FC10;
  m_crtc_state.fractional_ticks = 0;
  m_crtc_state.fractional_dot_ticks = 0;
  m_crtc_state.current_tick_in_scanline = 0;
  m_crtc_state.current_scanline = 0;
  m_crtc_state.in_hblank = false;
  m_crtc_state.in_vblank = false;
  m_blitter_state = BlitterState::Idle;
  m_pending_command_ticks = 0;
  m_command_total_words = 0;
  m_vram_transfer = {};
  m_fifo.Clear();
  m_blit_buffer.clear();
  m_blit_remaining_words = 0;
  m_draw_mode.texture_window_value = 0xFFFFFFFFu;
  SetDrawMode(0);
  SetTexturePalette(0);
  SetTextureWindow(0);
  UpdateDMARequest();
  UpdateCRTCConfig();
  UpdateCRTCTickEvent();
  UpdateCommandTickEvent();
}

bool GPU::DoState(StateWrapper& sw, bool update_display)
{
  if (sw.IsReading())
  {
    // perform a reset to discard all pending draws/fb state
    Reset();
  }

  sw.Do(&m_GPUSTAT.bits);

  sw.Do(&m_draw_mode.mode_reg.bits);
  sw.Do(&m_draw_mode.palette_reg);
  sw.Do(&m_draw_mode.texture_window_value);
  sw.Do(&m_draw_mode.texture_page_x);
  sw.Do(&m_draw_mode.texture_page_y);
  sw.Do(&m_draw_mode.texture_palette_x);
  sw.Do(&m_draw_mode.texture_palette_y);
  sw.Do(&m_draw_mode.texture_window.and_x);
  sw.Do(&m_draw_mode.texture_window.and_y);
  sw.Do(&m_draw_mode.texture_window.or_x);
  sw.Do(&m_draw_mode.texture_window.or_y);
  sw.Do(&m_draw_mode.texture_x_flip);
  sw.Do(&m_draw_mode.texture_y_flip);

  sw.Do(&m_drawing_area.left);
  sw.Do(&m_drawing_area.top);
  sw.Do(&m_drawing_area.right);
  sw.Do(&m_drawing_area.bottom);
  sw.Do(&m_drawing_offset.x);
  sw.Do(&m_drawing_offset.y);
  sw.Do(&m_drawing_offset.x);

  sw.Do(&m_console_is_pal);
  sw.Do(&m_set_texture_disable_mask);

  sw.Do(&m_crtc_state.regs.display_address_start);
  sw.Do(&m_crtc_state.regs.horizontal_display_range);
  sw.Do(&m_crtc_state.regs.vertical_display_range);
  sw.Do(&m_crtc_state.dot_clock_divider);
  sw.Do(&m_crtc_state.display_width);
  sw.Do(&m_crtc_state.display_height);
  sw.Do(&m_crtc_state.display_origin_left);
  sw.Do(&m_crtc_state.display_origin_top);
  sw.Do(&m_crtc_state.display_vram_left);
  sw.Do(&m_crtc_state.display_vram_top);
  sw.Do(&m_crtc_state.display_vram_width);
  sw.Do(&m_crtc_state.display_vram_height);
  sw.Do(&m_crtc_state.horizontal_total);
  sw.Do(&m_crtc_state.horizontal_active_start);
  sw.Do(&m_crtc_state.horizontal_active_end);
  sw.Do(&m_crtc_state.horizontal_display_start);
  sw.Do(&m_crtc_state.horizontal_display_end);
  sw.Do(&m_crtc_state.vertical_total);
  sw.Do(&m_crtc_state.vertical_active_start);
  sw.Do(&m_crtc_state.vertical_active_end);
  sw.Do(&m_crtc_state.vertical_display_start);
  sw.Do(&m_crtc_state.vertical_display_end);
  sw.Do(&m_crtc_state.fractional_ticks);
  sw.Do(&m_crtc_state.current_tick_in_scanline);
  sw.Do(&m_crtc_state.current_scanline);
  sw.DoEx(&m_crtc_state.fractional_dot_ticks, 46, 0);
  sw.Do(&m_crtc_state.in_hblank);
  sw.Do(&m_crtc_state.in_vblank);
  sw.Do(&m_crtc_state.interlaced_field);
  sw.Do(&m_crtc_state.interlaced_display_field);
  sw.Do(&m_crtc_state.active_line_lsb);

  sw.Do(&m_blitter_state);
  sw.Do(&m_pending_command_ticks);
  sw.Do(&m_command_total_words);
  sw.Do(&m_GPUREAD_latch);

  sw.Do(&m_vram_transfer.x);
  sw.Do(&m_vram_transfer.y);
  sw.Do(&m_vram_transfer.width);
  sw.Do(&m_vram_transfer.height);
  sw.Do(&m_vram_transfer.col);
  sw.Do(&m_vram_transfer.row);

  sw.Do(&m_fifo);
  sw.Do(&m_blit_buffer);
  sw.Do(&m_blit_remaining_words);
  sw.Do(&m_render_command.bits);

  sw.Do(&m_max_run_ahead);
  sw.Do(&m_fifo_size);

  if (sw.IsReading())
  {
    m_draw_mode.texture_page_changed = true;
    m_draw_mode.texture_window_changed = true;
    m_drawing_area_changed = true;
    UpdateDMARequest();
  }

  if (!sw.DoMarker("GPU-VRAM"))
    return false;

  if (sw.IsReading())
  {
    // Still need a temporary here.
    HeapArray<u16, VRAM_WIDTH * VRAM_HEIGHT> temp;
    sw.DoBytes(temp.data(), VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, temp.data(), false, false);

    UpdateCRTCConfig();
    if (update_display)
      UpdateDisplay();

    UpdateCRTCTickEvent();
    UpdateCommandTickEvent();
  }
  else
  {
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    sw.DoBytes(m_vram_ptr, VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
  }

  return !sw.HasError();
}

void GPU::ResetGraphicsAPIState() {}

void GPU::RestoreGraphicsAPIState() {}

void GPU::UpdateDMARequest()
{
  switch (m_blitter_state)
  {
    case BlitterState::Idle:
      m_GPUSTAT.ready_to_send_vram = false;
      m_GPUSTAT.ready_to_recieve_dma = (m_fifo.IsEmpty() || m_fifo.GetSize() < m_command_total_words);
      break;

    case BlitterState::WritingVRAM:
      m_GPUSTAT.ready_to_send_vram = false;
      m_GPUSTAT.ready_to_recieve_dma = (m_fifo.GetSize() < m_fifo_size);
      break;

    case BlitterState::ReadingVRAM:
      m_GPUSTAT.ready_to_send_vram = true;
      m_GPUSTAT.ready_to_recieve_dma = false;
      break;
  }

  bool dma_request;
  switch (m_GPUSTAT.dma_direction)
  {
    case DMADirection::Off:
      dma_request = false;
      break;

    case DMADirection::FIFO:
      dma_request = m_GPUSTAT.ready_to_recieve_dma;
      break;

    case DMADirection::CPUtoGP0:
      dma_request = m_GPUSTAT.ready_to_recieve_dma;
      break;

    case DMADirection::GPUREADtoCPU:
      dma_request = m_GPUSTAT.ready_to_send_vram;
      break;

    default:
      dma_request = false;
      break;
  }
  m_GPUSTAT.dma_data_request = dma_request;
  g_dma.SetRequest(DMA::Channel::GPU, dma_request);
}

void GPU::UpdateGPUIdle()
{
  switch (m_blitter_state)
  {
    case BlitterState::Idle:
      m_GPUSTAT.gpu_idle = (m_pending_command_ticks <= 0 && m_fifo.IsEmpty());
      break;

    case BlitterState::WritingVRAM:
      m_GPUSTAT.gpu_idle = false;
      break;

    case BlitterState::ReadingVRAM:
      m_GPUSTAT.gpu_idle = false;
      break;
  }
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
        m_command_tick_event->InvokeEarly();

      return m_GPUSTAT.bits;
    }

    default:
      Log_ErrorPrintf("Unhandled register read: %02X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void GPU::WriteRegister(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00:
      m_fifo.Push(value);
      ExecuteCommands();
      UpdateCommandTickEvent();
      return;

    case 0x04:
      WriteGP1(value);
      return;

    default:
      Log_ErrorPrintf("Unhandled register write: %02X <- %08X", offset, value);
      return;
  }
}

void GPU::DMARead(u32* words, u32 word_count)
{
  if (m_GPUSTAT.dma_direction != DMADirection::GPUREADtoCPU)
  {
    Log_ErrorPrintf("Invalid DMA direction from GPU DMA read");
    std::fill_n(words, word_count, UINT32_C(0xFFFFFFFF));
    return;
  }

  for (u32 i = 0; i < word_count; i++)
    words[i] = ReadGPUREAD();
}

void GPU::EndDMAWrite()
{
  m_fifo_pushed = true;
  if (!m_syncing)
  {
    ExecuteCommands();
    UpdateCommandTickEvent();
  }
  else
  {
    UpdateDMARequest();
  }
}

/**
 * NTSC GPU clock 53.693175 MHz
 * PAL GPU clock 53.203425 MHz
 * courtesy of @ggrtk
 *
 * NTSC - sysclk * 715909 / 451584
 * PAL - sysclk * 709379 / 451584
 */

TickCount GPU::GetCRTCFrequency() const
{
  return m_console_is_pal ? 53203425 : 53693175;
}

TickCount GPU::CRTCTicksToSystemTicks(TickCount gpu_ticks, TickCount fractional_ticks) const
{
  // convert to master clock, rounding up as we want to overshoot not undershoot
  if (!m_console_is_pal)
    return static_cast<TickCount>((u64(gpu_ticks) * u64(451584) + fractional_ticks + u64(715908)) / u64(715909));
  else
    return static_cast<TickCount>((u64(gpu_ticks) * u64(451584) + fractional_ticks + u64(709378)) / u64(709379));
}

TickCount GPU::SystemTicksToCRTCTicks(TickCount sysclk_ticks, TickCount* fractional_ticks) const
{
  if (!m_console_is_pal)
  {
    const u64 mul = u64(sysclk_ticks) * u64(715909) + u64(*fractional_ticks);
    const TickCount ticks = static_cast<TickCount>(mul / u64(451584));
    *fractional_ticks = static_cast<TickCount>(mul % u64(451584));
    return ticks;
  }
  else
  {
    const u64 mul = u64(sysclk_ticks) * u64(709379) + u64(*fractional_ticks);
    const TickCount ticks = static_cast<TickCount>(mul / u64(451584));
    *fractional_ticks = static_cast<TickCount>(mul % u64(451584));
    return ticks;
  }
}

void GPU::AddCommandTicks(TickCount ticks)
{
  m_pending_command_ticks += ticks;
}

void GPU::SynchronizeCRTC()
{
  m_crtc_tick_event->InvokeEarly();
}

float GPU::ComputeHorizontalFrequency() const
{
  const CRTCState& cs = m_crtc_state;
  TickCount fractional_ticks = 0;
  return static_cast<float>(
    static_cast<double>(SystemTicksToCRTCTicks(System::GetTicksPerSecond(), &fractional_ticks)) /
    static_cast<double>(cs.horizontal_total));
}

float GPU::ComputeVerticalFrequency() const
{
  const CRTCState& cs = m_crtc_state;
  const TickCount ticks_per_frame = cs.horizontal_total * cs.vertical_total;
  TickCount fractional_ticks = 0;
  return static_cast<float>(
    static_cast<double>(SystemTicksToCRTCTicks(System::GetTicksPerSecond(), &fractional_ticks)) /
    static_cast<double>(ticks_per_frame));
}

float GPU::GetDisplayAspectRatio() const
{
  if (g_settings.display_force_4_3_for_24bit && m_GPUSTAT.display_area_color_depth_24)
    return 4.0f / 3.0f;
  else
    return Settings::GetDisplayAspectRatioValue(g_settings.display_aspect_ratio);
}

void GPU::UpdateCRTCConfig()
{
  static constexpr std::array<u16, 8> dot_clock_dividers = {{10, 8, 5, 4, 7, 7, 7, 7}};
  CRTCState& cs = m_crtc_state;

  if (m_GPUSTAT.pal_mode)
  {
    cs.vertical_total = PAL_TOTAL_LINES;
    cs.current_scanline %= PAL_TOTAL_LINES;
    cs.horizontal_total = PAL_TICKS_PER_LINE;
    cs.horizontal_sync_start = PAL_HSYNC_TICKS;
    cs.current_tick_in_scanline %= System::ScaleTicksToOverclock(PAL_TICKS_PER_LINE);
  }
  else
  {
    cs.vertical_total = NTSC_TOTAL_LINES;
    cs.current_scanline %= NTSC_TOTAL_LINES;
    cs.horizontal_total = NTSC_TICKS_PER_LINE;
    cs.horizontal_sync_start = NTSC_HSYNC_TICKS;
    cs.current_tick_in_scanline %= System::ScaleTicksToOverclock(NTSC_TICKS_PER_LINE);
  }

  cs.in_hblank = (cs.current_tick_in_scanline >= cs.horizontal_sync_start);

  const u8 horizontal_resolution_index = m_GPUSTAT.horizontal_resolution_1 | (m_GPUSTAT.horizontal_resolution_2 << 2);
  cs.dot_clock_divider = dot_clock_dividers[horizontal_resolution_index];
  cs.horizontal_display_start =
    (std::min<u16>(cs.regs.X1, cs.horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  cs.horizontal_display_end =
    (std::min<u16>(cs.regs.X2, cs.horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  cs.vertical_display_start = std::min<u16>(cs.regs.Y1, cs.vertical_total);
  cs.vertical_display_end = std::min<u16>(cs.regs.Y2, cs.vertical_total);

  if (m_GPUSTAT.pal_mode && m_force_ntsc_timings)
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

  cs.horizontal_display_start =
    static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_display_start)));
  cs.horizontal_display_end =
    static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_display_end)));
  cs.horizontal_total = static_cast<u16>(System::ScaleTicksToOverclock(static_cast<TickCount>(cs.horizontal_total)));

  System::SetThrottleFrequency(ComputeVerticalFrequency());

  UpdateCRTCDisplayParameters();
  UpdateCRTCTickEvent();
}

void GPU::UpdateCRTCDisplayParameters()
{
  CRTCState& cs = m_crtc_state;
  const DisplayCropMode crop_mode = g_settings.display_crop_mode;

  const u16 horizontal_total = m_GPUSTAT.pal_mode ? PAL_TICKS_PER_LINE : NTSC_TICKS_PER_LINE;
  const u16 vertical_total = m_GPUSTAT.pal_mode ? PAL_TOTAL_LINES : NTSC_TOTAL_LINES;
  const u16 horizontal_display_start =
    (std::min<u16>(cs.regs.X1, horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  const u16 horizontal_display_end =
    (std::min<u16>(cs.regs.X2, horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  const u16 vertical_display_start = std::min<u16>(cs.regs.Y1, vertical_total);
  const u16 vertical_display_end = std::min<u16>(cs.regs.Y2, vertical_total);

  if (m_GPUSTAT.pal_mode)
  {
    // TODO: Verify PAL numbers.
    switch (crop_mode)
    {
      case DisplayCropMode::None:
        cs.horizontal_active_start = static_cast<u16>(std::max<int>(0, 487 + g_settings.display_active_start_offset));
        cs.horizontal_active_end = static_cast<u16>(std::max<int>(0, 3282 + g_settings.display_active_end_offset));
        cs.vertical_active_start = static_cast<u16>(std::max<int>(0, 20 + g_settings.display_line_start_offset));
        cs.vertical_active_end = static_cast<u16>(std::max<int>(0, 308 + g_settings.display_line_end_offset));
        break;

      case DisplayCropMode::Overscan:
        cs.horizontal_active_start = static_cast<u16>(std::max<int>(0, 628 + g_settings.display_active_start_offset));
        cs.horizontal_active_end = static_cast<u16>(std::max<int>(0, 3188 + g_settings.display_active_end_offset));
        cs.vertical_active_start = static_cast<u16>(std::max<int>(0, 30 + g_settings.display_line_start_offset));
        cs.vertical_active_end = static_cast<u16>(std::max<int>(0, 298 + g_settings.display_line_end_offset));
        break;

      case DisplayCropMode::Borders:
      default:
        cs.horizontal_active_start = horizontal_display_start;
        cs.horizontal_active_end = horizontal_display_end;
        cs.vertical_active_start = vertical_display_start;
        cs.vertical_active_end = vertical_display_end;
        break;
    }
  }
  else
  {
    switch (crop_mode)
    {
      case DisplayCropMode::None:
        cs.horizontal_active_start = static_cast<u16>(std::max<int>(0, 488 + g_settings.display_active_start_offset));
        cs.horizontal_active_end = static_cast<u16>(std::max<int>(0, 3288 + g_settings.display_active_end_offset));
        cs.vertical_active_start = static_cast<u16>(std::max<int>(0, 16 + g_settings.display_line_start_offset));
        cs.vertical_active_end = static_cast<u16>(std::max<int>(0, 256 + g_settings.display_line_end_offset));
        break;

      case DisplayCropMode::Overscan:
        cs.horizontal_active_start = static_cast<u16>(std::max<int>(0, 608 + g_settings.display_active_start_offset));
        cs.horizontal_active_end = static_cast<u16>(std::max<int>(0, 3168 + g_settings.display_active_end_offset));
        cs.vertical_active_start = static_cast<u16>(std::max<int>(0, 24 + g_settings.display_line_start_offset));
        cs.vertical_active_end = static_cast<u16>(std::max<int>(0, 248 + g_settings.display_line_end_offset));
        break;

      case DisplayCropMode::Borders:
      default:
        cs.horizontal_active_start = horizontal_display_start;
        cs.horizontal_active_end = horizontal_display_end;
        cs.vertical_active_start = vertical_display_start;
        cs.vertical_active_end = vertical_display_end;
        break;
    }
  }

  // If force-progressive is enabled, we only double the height in 480i mode. This way non-interleaved 480i framebuffers
  // won't be broken when displayed.
  const u8 y_shift = BoolToUInt8(m_GPUSTAT.vertical_interlace && m_GPUSTAT.vertical_resolution);
  const u8 height_shift = m_force_progressive_scan ? y_shift : BoolToUInt8(m_GPUSTAT.vertical_interlace);

  // Determine screen size.
  cs.display_width = (cs.horizontal_active_end - cs.horizontal_active_start) / cs.dot_clock_divider;
  cs.display_height = (cs.vertical_active_end - cs.vertical_active_start) << height_shift;

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
  if (horizontal_display_start >= cs.horizontal_active_start)
  {
    cs.display_origin_left = (horizontal_display_start - cs.horizontal_active_start) / cs.dot_clock_divider;
    cs.display_vram_left = std::min<u16>(m_crtc_state.regs.X, VRAM_WIDTH - 1);
    horizontal_skip_pixels = 0;
  }
  else
  {
    horizontal_skip_pixels = (cs.horizontal_active_start - horizontal_display_start) / cs.dot_clock_divider;
    cs.display_origin_left = 0;
    cs.display_vram_left = std::min<u16>(m_crtc_state.regs.X + horizontal_skip_pixels, VRAM_WIDTH - 1);
  }

  // apply the crop from the start (usually overscan)
  cs.display_vram_width -= std::min(cs.display_vram_width, horizontal_skip_pixels);

  // Apply crop from the end by shrinking VRAM rectangle width if display would end outside the visible area.
  cs.display_vram_width = std::min<u16>(cs.display_vram_width, cs.display_width - cs.display_origin_left);

  if (vertical_display_start >= cs.vertical_active_start)
  {
    cs.display_origin_top = (vertical_display_start - cs.vertical_active_start) << y_shift;
    cs.display_vram_top = m_crtc_state.regs.Y;
  }
  else
  {
    cs.display_origin_top = 0;
    cs.display_vram_top = m_crtc_state.regs.Y + ((cs.vertical_active_start - vertical_display_start) << y_shift);
  }

  if (vertical_display_end <= cs.vertical_active_end)
  {
    cs.display_vram_height = (vertical_display_end - std::min(vertical_display_end, std::max(vertical_display_start,
                                                                                             cs.vertical_active_start)))
                             << height_shift;
  }
  else
  {
    cs.display_vram_height =
      (cs.vertical_active_end -
       std::min(cs.vertical_active_end, std::max(vertical_display_start, cs.vertical_active_start)))
      << height_shift;
  }
}

TickCount GPU::GetPendingCRTCTicks() const
{
  const TickCount pending_sysclk_ticks = m_crtc_tick_event->GetTicksSinceLastExecution();
  TickCount fractional_ticks = m_crtc_state.fractional_ticks;
  return SystemTicksToCRTCTicks(pending_sysclk_ticks, &fractional_ticks);
}

TickCount GPU::GetPendingCommandTicks() const
{
  if (!m_command_tick_event->IsActive())
    return 0;

  return SystemTicksToGPUTicks(m_command_tick_event->GetTicksSinceLastExecution());
}

void GPU::UpdateCRTCTickEvent()
{
  // figure out how many GPU ticks until the next vblank or event
  TickCount lines_until_event;
  if (g_timers.IsSyncEnabled(HBLANK_TIMER_INDEX))
  {
    // when the timer sync is enabled we need to sync at vblank start and end
    lines_until_event =
      (m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end) ?
        (m_crtc_state.vertical_total - m_crtc_state.current_scanline + m_crtc_state.vertical_display_start) :
        (m_crtc_state.vertical_display_end - m_crtc_state.current_scanline);
  }
  else
  {
    lines_until_event =
      (m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end ?
         (m_crtc_state.vertical_total - m_crtc_state.current_scanline + m_crtc_state.vertical_display_end) :
         (m_crtc_state.vertical_display_end - m_crtc_state.current_scanline));
  }
  if (g_timers.IsExternalIRQEnabled(HBLANK_TIMER_INDEX))
    lines_until_event = std::min(lines_until_event, g_timers.GetTicksUntilIRQ(HBLANK_TIMER_INDEX));

  TickCount ticks_until_event =
    lines_until_event * m_crtc_state.horizontal_total - m_crtc_state.current_tick_in_scanline;
  if (g_timers.IsExternalIRQEnabled(DOT_TIMER_INDEX))
  {
    const TickCount dots_until_irq = g_timers.GetTicksUntilIRQ(DOT_TIMER_INDEX);
    const TickCount ticks_until_irq =
      (dots_until_irq * m_crtc_state.dot_clock_divider) - m_crtc_state.fractional_dot_ticks;
    ticks_until_event = std::min(ticks_until_event, std::max<TickCount>(ticks_until_irq, 0));
  }

#if 0
  const TickCount ticks_until_hblank =
    (m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_display_end) ?
    (m_crtc_state.horizontal_total - m_crtc_state.current_tick_in_scanline + m_crtc_state.horizontal_display_end) :
    (m_crtc_state.horizontal_display_end - m_crtc_state.current_tick_in_scanline);
#endif

  m_crtc_tick_event->Schedule(CRTCTicksToSystemTicks(ticks_until_event, m_crtc_state.fractional_ticks));
}

bool GPU::IsCRTCScanlinePending() const
{
  return (GetPendingCRTCTicks() + m_crtc_state.current_tick_in_scanline) >= m_crtc_state.horizontal_total;
}

bool GPU::IsCommandCompletionPending() const
{
  return (m_pending_command_ticks > 0 && GetPendingCommandTicks() >= m_pending_command_ticks);
}

void GPU::CRTCTickEvent(TickCount ticks)
{
  // convert cpu/master clock to GPU ticks, accounting for partial cycles because of the non-integer divider
  {
    const TickCount gpu_ticks = SystemTicksToCRTCTicks(ticks, &m_crtc_state.fractional_ticks);
    m_crtc_state.current_tick_in_scanline += gpu_ticks;

    if (g_timers.IsUsingExternalClock(DOT_TIMER_INDEX))
    {
      m_crtc_state.fractional_dot_ticks += gpu_ticks;
      const TickCount dots = m_crtc_state.fractional_dot_ticks / m_crtc_state.dot_clock_divider;
      m_crtc_state.fractional_dot_ticks = m_crtc_state.fractional_dot_ticks % m_crtc_state.dot_clock_divider;
      if (dots > 0)
        g_timers.AddTicks(DOT_TIMER_INDEX, dots);
    }
  }

  if (m_crtc_state.current_tick_in_scanline < m_crtc_state.horizontal_total)
  {
    // short path when we execute <1 line.. this shouldn't occur often.
    const bool old_hblank = m_crtc_state.in_hblank;
    const bool new_hblank = (m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_sync_start);
    m_crtc_state.in_hblank = new_hblank;
    if (!old_hblank && new_hblank && g_timers.IsUsingExternalClock(HBLANK_TIMER_INDEX))
      g_timers.AddTicks(HBLANK_TIMER_INDEX, 1);

    UpdateCRTCTickEvent();
    return;
  }

  u32 lines_to_draw = m_crtc_state.current_tick_in_scanline / m_crtc_state.horizontal_total;
  m_crtc_state.current_tick_in_scanline %= m_crtc_state.horizontal_total;
#if 0
  Log_WarningPrintf("Old line: %u, new line: %u, drawing %u", m_crtc_state.current_scanline,
                    m_crtc_state.current_scanline + lines_to_draw, lines_to_draw);
#endif

  const bool old_hblank = m_crtc_state.in_hblank;
  const bool new_hblank = (m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_sync_start);
  m_crtc_state.in_hblank = new_hblank;
  if (g_timers.IsUsingExternalClock(HBLANK_TIMER_INDEX))
  {
    const u32 hblank_timer_ticks = BoolToUInt32(!old_hblank) + BoolToUInt32(new_hblank) + (lines_to_draw - 1);
    g_timers.AddTicks(HBLANK_TIMER_INDEX, static_cast<TickCount>(hblank_timer_ticks));
  }

  while (lines_to_draw > 0)
  {
    const u32 lines_to_draw_this_loop =
      std::min(lines_to_draw, m_crtc_state.vertical_total - m_crtc_state.current_scanline);
    const u32 prev_scanline = m_crtc_state.current_scanline;
    m_crtc_state.current_scanline += lines_to_draw_this_loop;
    DebugAssert(m_crtc_state.current_scanline <= m_crtc_state.vertical_total);
    lines_to_draw -= lines_to_draw_this_loop;

    // clear the vblank flag if the beam would pass through the display area
    if (prev_scanline < m_crtc_state.vertical_display_start &&
        m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end)
    {
      g_timers.SetGate(HBLANK_TIMER_INDEX, false);
      m_crtc_state.in_vblank = false;
    }

    const bool new_vblank = m_crtc_state.current_scanline < m_crtc_state.vertical_display_start ||
                            m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end;
    if (m_crtc_state.in_vblank != new_vblank)
    {
      if (new_vblank)
      {
        Log_DebugPrintf("Now in v-blank");
        g_interrupt_controller.InterruptRequest(InterruptController::IRQ::VBLANK);

        // flush any pending draws and "scan out" the image
        FlushRender();
        UpdateDisplay();
        System::FrameDone();

        // switch fields early. this is needed so we draw to the correct one.
        if (m_GPUSTAT.InInterleaved480iMode())
          m_crtc_state.interlaced_display_field = m_crtc_state.interlaced_field ^ 1u;
        else
          m_crtc_state.interlaced_display_field = 0;
      }

      g_timers.SetGate(HBLANK_TIMER_INDEX, new_vblank);
      m_crtc_state.in_vblank = new_vblank;
    }

    // past the end of vblank?
    if (m_crtc_state.current_scanline == m_crtc_state.vertical_total)
    {
      // start the new frame
      m_crtc_state.current_scanline = 0;
      if (m_GPUSTAT.vertical_interlace)
      {
        m_crtc_state.interlaced_field ^= 1u;
        m_GPUSTAT.interlaced_field = m_crtc_state.interlaced_field;
      }
      else
      {
        m_crtc_state.interlaced_field = 0;
        m_GPUSTAT.interlaced_field = 0u; // new GPU = 1, old GPU = 0
      }
    }
  }

  // alternating even line bit in 240-line mode
  if (m_GPUSTAT.InInterleaved480iMode())
  {
    m_crtc_state.active_line_lsb =
      Truncate8((m_crtc_state.regs.Y + BoolToUInt32(m_crtc_state.interlaced_display_field)) & u32(1));
    m_GPUSTAT.display_line_lsb = ConvertToBoolUnchecked(
      (m_crtc_state.regs.Y + (BoolToUInt8(m_crtc_state.in_vblank) ^ m_crtc_state.interlaced_display_field)) & u32(1));
  }
  else
  {
    m_crtc_state.active_line_lsb = 0;
    m_GPUSTAT.display_line_lsb = ConvertToBoolUnchecked((m_crtc_state.regs.Y + m_crtc_state.current_scanline) & u32(1));
  }

  UpdateCRTCTickEvent();
}

void GPU::CommandTickEvent(TickCount ticks)
{
  m_pending_command_ticks -= SystemTicksToGPUTicks(ticks);
  m_command_tick_event->Deactivate();

  // we can be syncing if this came from a DMA write. recursively executing commands would be bad.
  if (!m_syncing)
    ExecuteCommands();

  UpdateGPUIdle();

  if (m_pending_command_ticks <= 0)
    m_pending_command_ticks = 0;
  else
    m_command_tick_event->SetIntervalAndSchedule(GPUTicksToSystemTicks(m_pending_command_ticks));
}

void GPU::UpdateCommandTickEvent()
{
  if (m_pending_command_ticks <= 0)
    m_command_tick_event->Deactivate();
  else if (!m_command_tick_event->IsActive())
    m_command_tick_event->SetIntervalAndSchedule(GPUTicksToSystemTicks(m_pending_command_ticks));
}

bool GPU::ConvertScreenCoordinatesToBeamTicksAndLines(s32 window_x, s32 window_y, u32* out_tick, u32* out_line) const
{
  const auto [display_x, display_y] = m_host_display->ConvertWindowCoordinatesToDisplayCoordinates(
    window_x, window_y, m_host_display->GetWindowWidth(), m_host_display->GetWindowHeight(),
    m_host_display->GetDisplayTopMargin());
  Log_DebugPrintf("win %d,%d -> disp %d,%d (size %u,%u frac %f,%f)", window_x, window_y, display_x, display_y,
                  m_crtc_state.display_width, m_crtc_state.display_height,
                  static_cast<float>(display_x) / static_cast<float>(m_crtc_state.display_width),
                  static_cast<float>(display_y) / static_cast<float>(m_crtc_state.display_height));

  if (display_x < 0 || static_cast<u32>(display_x) >= m_crtc_state.display_width || display_y < 0 ||
      static_cast<u32>(display_y) >= m_crtc_state.display_height)
  {
    return false;
  }

  *out_line =
    (static_cast<u32>(display_y) >> BoolToUInt8(m_GPUSTAT.vertical_interlace)) + m_crtc_state.vertical_active_start;
  *out_tick = (static_cast<u32>(display_x) * m_crtc_state.dot_clock_divider) + m_crtc_state.horizontal_active_start;
  return true;
}

u32 GPU::ReadGPUREAD()
{
  if (m_blitter_state != BlitterState::ReadingVRAM)
    return m_GPUREAD_latch;

  // Read two pixels out of VRAM and combine them. Zero fill odd pixel counts.
  u32 value = 0;
  for (u32 i = 0; i < 2; i++)
  {
    // Read with correct wrap-around behavior.
    const u16 read_x = (m_vram_transfer.x + m_vram_transfer.col) % VRAM_WIDTH;
    const u16 read_y = (m_vram_transfer.y + m_vram_transfer.row) % VRAM_HEIGHT;
    value |= ZeroExtend32(m_vram_ptr[read_y * VRAM_WIDTH + read_x]) << (i * 16);

    if (++m_vram_transfer.col == m_vram_transfer.width)
    {
      m_vram_transfer.col = 0;

      if (++m_vram_transfer.row == m_vram_transfer.height)
      {
        Log_DebugPrintf("End of VRAM->CPU transfer");
        m_vram_transfer = {};
        m_blitter_state = BlitterState::Idle;

        // end of transfer, catch up on any commands which were written (unlikely)
        ExecuteCommands();
        UpdateCommandTickEvent();
        break;
      }
    }
  }

  m_GPUREAD_latch = value;
  return value;
}

void GPU::WriteGP1(u32 value)
{
  const u32 command = (value >> 24) & 0x3Fu;
  const u32 param = value & UINT32_C(0x00FFFFFF);
  switch (command)
  {
    case 0x00: // Reset GPU
    {
      Log_DebugPrintf("GP1 reset GPU");
      m_command_tick_event->InvokeEarly();
      SynchronizeCRTC();
      SoftReset();
    }
    break;

    case 0x01: // Clear FIFO
    {
      Log_DebugPrintf("GP1 clear FIFO");
      m_command_tick_event->InvokeEarly();
      SynchronizeCRTC();

      // flush partial writes
      if (m_blitter_state == BlitterState::WritingVRAM)
        FinishVRAMWrite();

      m_blitter_state = BlitterState::Idle;
      m_command_total_words = 0;
      m_vram_transfer = {};
      m_fifo.Clear();
      m_blit_buffer.clear();
      m_blit_remaining_words = 0;
      m_pending_command_ticks = 0;
      m_command_tick_event->Deactivate();
      UpdateDMARequest();
      UpdateGPUIdle();
    }
    break;

    case 0x02: // Acknowledge Interrupt
    {
      Log_DebugPrintf("Acknowledge interrupt");
      m_GPUSTAT.interrupt_request = false;
    }
    break;

    case 0x03: // Display on/off
    {
      const bool disable = ConvertToBoolUnchecked(value & 0x01);
      Log_DebugPrintf("Display %s", disable ? "disabled" : "enabled");
      SynchronizeCRTC();
      m_GPUSTAT.display_disable = disable;
    }
    break;

    case 0x04: // DMA Direction
    {
      Log_DebugPrintf("DMA direction <- 0x%02X", static_cast<u32>(param));
      if (m_GPUSTAT.dma_direction != static_cast<DMADirection>(param))
      {
        m_GPUSTAT.dma_direction = static_cast<DMADirection>(param);
        UpdateDMARequest();
      }
    }
    break;

    case 0x05: // Set display start address
    {
      const u32 new_value = param & CRTCState::Regs::DISPLAY_ADDRESS_START_MASK;
      Log_DebugPrintf("Display address start <- 0x%08X", new_value);

      System::IncrementInternalFrameNumber();
      if (m_crtc_state.regs.display_address_start != new_value)
      {
        SynchronizeCRTC();
        m_crtc_state.regs.display_address_start = new_value;
        UpdateCRTCDisplayParameters();
      }
    }
    break;

    case 0x06: // Set horizontal display range
    {
      const u32 new_value = param & CRTCState::Regs::HORIZONTAL_DISPLAY_RANGE_MASK;
      Log_DebugPrintf("Horizontal display range <- 0x%08X", new_value);

      if (m_crtc_state.regs.horizontal_display_range != new_value)
      {
        SynchronizeCRTC();
        m_crtc_state.regs.horizontal_display_range = new_value;
        UpdateCRTCConfig();
      }
    }
    break;

    case 0x07: // Set vertical display range
    {
      const u32 new_value = param & CRTCState::Regs::VERTICAL_DISPLAY_RANGE_MASK;
      Log_DebugPrintf("Vertical display range <- 0x%08X", new_value);

      if (m_crtc_state.regs.vertical_display_range != new_value)
      {
        SynchronizeCRTC();
        m_crtc_state.regs.vertical_display_range = new_value;
        UpdateCRTCConfig();
      }
    }
    break;

    case 0x08: // Set display mode
    {
      union GP1_08h
      {
        u32 bits;

        BitField<u32, u8, 0, 2> horizontal_resolution_1;
        BitField<u32, bool, 2, 1> vertical_resolution;
        BitField<u32, bool, 3, 1> pal_mode;
        BitField<u32, bool, 4, 1> display_area_color_depth;
        BitField<u32, bool, 5, 1> vertical_interlace;
        BitField<u32, bool, 6, 1> horizontal_resolution_2;
        BitField<u32, bool, 7, 1> reverse_flag;
      };

      const GP1_08h dm{param};
      GPUSTAT new_GPUSTAT{m_GPUSTAT.bits};
      new_GPUSTAT.horizontal_resolution_1 = dm.horizontal_resolution_1;
      new_GPUSTAT.vertical_resolution = dm.vertical_resolution;
      new_GPUSTAT.pal_mode = dm.pal_mode;
      new_GPUSTAT.display_area_color_depth_24 = dm.display_area_color_depth;
      new_GPUSTAT.vertical_interlace = dm.vertical_interlace;
      new_GPUSTAT.horizontal_resolution_2 = dm.horizontal_resolution_2;
      new_GPUSTAT.reverse_flag = dm.reverse_flag;
      Log_DebugPrintf("Set display mode <- 0x%08X", dm.bits);

      if (!m_GPUSTAT.vertical_interlace && dm.vertical_interlace && !m_force_progressive_scan)
      {
        // bit of a hack, technically we should pull the previous frame in, but this may not exist anymore
        ClearDisplay();
      }

      if (m_GPUSTAT.bits != new_GPUSTAT.bits)
      {
        // Have to be careful when setting this because Synchronize() can modify GPUSTAT.
        static constexpr u32 SET_MASK = UINT32_C(0b00000000011111110100000000000000);
        m_command_tick_event->InvokeEarly();
        SynchronizeCRTC();
        m_GPUSTAT.bits = (m_GPUSTAT.bits & ~SET_MASK) | (new_GPUSTAT.bits & SET_MASK);
        UpdateCRTCConfig();
      }
    }
    break;

    case 0x09: // Allow texture disable
    {
      m_set_texture_disable_mask = ConvertToBoolUnchecked(param & 0x01);
      Log_DebugPrintf("Set texture disable mask <- %s", m_set_texture_disable_mask ? "allowed" : "ignored");
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

    default:
      Log_ErrorPrintf("Unimplemented GP1 command 0x%02X", command);
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
      Log_DebugPrintf("Get texture window");
      m_GPUREAD_latch = m_draw_mode.texture_window_value;
    }
    break;

    case 0x03: // Get Draw Area Top Left
    {
      Log_DebugPrintf("Get drawing area top left");
      m_GPUREAD_latch =
        ((m_drawing_area.left & UINT32_C(0b1111111111)) | ((m_drawing_area.top & UINT32_C(0b1111111111)) << 10));
    }
    break;

    case 0x04: // Get Draw Area Bottom Right
    {
      Log_DebugPrintf("Get drawing area bottom right");
      m_GPUREAD_latch =
        ((m_drawing_area.right & UINT32_C(0b1111111111)) | ((m_drawing_area.bottom & UINT32_C(0b1111111111)) << 10));
    }
    break;

    case 0x05: // Get Drawing Offset
    {
      Log_DebugPrintf("Get drawing offset");
      m_GPUREAD_latch =
        ((m_drawing_offset.x & INT32_C(0b11111111111)) | ((m_drawing_offset.y & INT32_C(0b11111111111)) << 11));
    }
    break;

    default:
      Log_WarningPrintf("Unhandled GetGPUInfo(0x%02X)", ZeroExtend32(subcommand));
      break;
  }
}

void GPU::ClearDisplay() {}

void GPU::UpdateDisplay() {}

void GPU::ReadVRAM(u32 x, u32 y, u32 width, u32 height) {}

void GPU::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  const u16 color16 = RGBA8888ToRGBA5551(color);
  if ((x + width) <= VRAM_WIDTH && !IsInterlacedRenderingEnabled())
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      std::fill_n(&m_vram_ptr[row * VRAM_WIDTH + x], width, color16);
    }
  }
  else if (IsInterlacedRenderingEnabled())
  {
    // Hardware tests show that fills seem to break on the first two lines when the offset matches the displayed field.
    if (IsCRTCScanlinePending())
      SynchronizeCRTC();

    const u32 active_field = GetActiveLineLSB();
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      if ((row & u32(1)) == active_field)
        continue;

      u16* row_ptr = &m_vram_ptr[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
  else
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* row_ptr = &m_vram_ptr[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
}

void GPU::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  // Fast path when the copy is not oversized.
  if ((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT && !set_mask && !check_mask)
  {
    const u16* src_ptr = static_cast<const u16*>(data);
    u16* dst_ptr = &m_vram_ptr[y * VRAM_WIDTH + x];
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      std::copy_n(src_ptr, width, dst_ptr);
      src_ptr += width;
      dst_ptr += VRAM_WIDTH;
    }
  }
  else
  {
    // Slow path when we need to handle wrap-around.
    // During transfer/render operations, if ((dst_pixel & mask_and) == 0) { pixel = src_pixel | mask_or }
    const u16* src_ptr = static_cast<const u16*>(data);
    const u16 mask_and = check_mask ? 0x8000 : 0;
    const u16 mask_or = set_mask ? 0x8000 : 0;

    for (u32 row = 0; row < height;)
    {
      u16* dst_row_ptr = &m_vram_ptr[((y + row++) % VRAM_HEIGHT) * VRAM_WIDTH];
      for (u32 col = 0; col < width;)
      {
        // TODO: Handle unaligned reads...
        u16* pixel_ptr = &dst_row_ptr[(x + col++) % VRAM_WIDTH];
        if (((*pixel_ptr) & mask_and) == 0)
          *pixel_ptr = *(src_ptr++) | mask_or;
      }
    }
  }
}

void GPU::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  // Break up oversized copies. This behavior has not been verified on console.
  if ((src_x + width) > VRAM_WIDTH || (dst_x + width) > VRAM_WIDTH)
  {
    u32 remaining_rows = height;
    u32 current_src_y = src_y;
    u32 current_dst_y = dst_y;
    while (remaining_rows > 0)
    {
      const u32 rows_to_copy =
        std::min<u32>(remaining_rows, std::min<u32>(VRAM_HEIGHT - current_src_y, VRAM_HEIGHT - current_dst_y));

      u32 remaining_columns = width;
      u32 current_src_x = src_x;
      u32 current_dst_x = dst_x;
      while (remaining_columns > 0)
      {
        const u32 columns_to_copy =
          std::min<u32>(remaining_columns, std::min<u32>(VRAM_WIDTH - current_src_x, VRAM_WIDTH - current_dst_x));
        CopyVRAM(current_src_x, current_src_y, current_dst_x, current_dst_y, columns_to_copy, rows_to_copy);
        current_src_x = (current_src_x + columns_to_copy) % VRAM_WIDTH;
        current_dst_x = (current_dst_x + columns_to_copy) % VRAM_WIDTH;
        remaining_columns -= columns_to_copy;
      }

      current_src_y = (current_src_y + rows_to_copy) % VRAM_HEIGHT;
      current_dst_y = (current_dst_y + rows_to_copy) % VRAM_HEIGHT;
      remaining_rows -= rows_to_copy;
    }

    return;
  }

  // This doesn't have a fast path, but do we really need one? It's not common.
  const u16 mask_and = m_GPUSTAT.GetMaskAND();
  const u16 mask_or = m_GPUSTAT.GetMaskOR();

  // Copy in reverse when src_x < dst_x, this is verified on console.
  if (src_x < dst_x || ((src_x + width - 1) % VRAM_WIDTH) < ((dst_x + width - 1) % VRAM_WIDTH))
  {
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &m_vram_ptr[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &m_vram_ptr[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      for (s32 col = static_cast<s32>(width - 1); col >= 0; col--)
      {
        const u16 src_pixel = src_row_ptr[(src_x + static_cast<u32>(col)) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + static_cast<u32>(col)) % VRAM_WIDTH];
        if ((*dst_pixel_ptr & mask_and) == 0)
          *dst_pixel_ptr = src_pixel | mask_or;
      }
    }
  }
  else
  {
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &m_vram_ptr[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &m_vram_ptr[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      for (u32 col = 0; col < width; col++)
      {
        const u16 src_pixel = src_row_ptr[(src_x + col) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + col) % VRAM_WIDTH];
        if ((*dst_pixel_ptr & mask_and) == 0)
          *dst_pixel_ptr = src_pixel | mask_or;
      }
    }
  }
}

void GPU::DispatchRenderCommand() {}

void GPU::FlushRender() {}

void GPU::SetDrawMode(u16 value)
{
  GPUDrawModeReg new_mode_reg{static_cast<u16>(value & GPUDrawModeReg::MASK)};
  if (!m_set_texture_disable_mask)
    new_mode_reg.texture_disable = false;

  if (new_mode_reg.bits == m_draw_mode.mode_reg.bits)
    return;

  if ((new_mode_reg.bits & GPUDrawModeReg::TEXTURE_PAGE_MASK) !=
      (m_draw_mode.mode_reg.bits & GPUDrawModeReg::TEXTURE_PAGE_MASK))
  {
    m_draw_mode.texture_page_x = new_mode_reg.GetTexturePageBaseX();
    m_draw_mode.texture_page_y = new_mode_reg.GetTexturePageBaseY();
    m_draw_mode.texture_page_changed = true;
  }

  m_draw_mode.mode_reg.bits = new_mode_reg.bits;

  if (m_GPUSTAT.draw_to_displayed_field != new_mode_reg.draw_to_displayed_field)
    FlushRender();

  // Bits 0..10 are returned in the GPU status register.
  m_GPUSTAT.bits = (m_GPUSTAT.bits & ~(GPUDrawModeReg::GPUSTAT_MASK)) |
                   (ZeroExtend32(new_mode_reg.bits) & GPUDrawModeReg::GPUSTAT_MASK);
  m_GPUSTAT.texture_disable = m_draw_mode.mode_reg.texture_disable;
}

void GPU::SetTexturePalette(u16 value)
{
  value &= DrawMode::PALETTE_MASK;
  if (m_draw_mode.palette_reg == value)
    return;

  m_draw_mode.texture_palette_x = ZeroExtend32(value & 0x3F) * 16;
  m_draw_mode.texture_palette_y = ZeroExtend32(value >> 6);
  m_draw_mode.palette_reg = value;
  m_draw_mode.texture_page_changed = true;
}

void GPU::SetTextureWindow(u32 value)
{
  value &= DrawMode::TEXTURE_WINDOW_MASK;
  if (m_draw_mode.texture_window_value == value)
    return;

  FlushRender();

  const u8 mask_x = Truncate8(value & UINT32_C(0x1F));
  const u8 mask_y = Truncate8((value >> 5) & UINT32_C(0x1F));
  const u8 offset_x = Truncate8((value >> 10) & UINT32_C(0x1F));
  const u8 offset_y = Truncate8((value >> 15) & UINT32_C(0x1F));
  Log_DebugPrintf("Set texture window %02X %02X %02X %02X", mask_x, mask_y, offset_x, offset_y);

  m_draw_mode.texture_window.and_x = ~(mask_x * 8);
  m_draw_mode.texture_window.and_y = ~(mask_y * 8);
  m_draw_mode.texture_window.or_x = (offset_x & mask_x) * 8u;
  m_draw_mode.texture_window.or_y = (offset_y & mask_y) * 8u;
  m_draw_mode.texture_window_value = value;
  m_draw_mode.texture_window_changed = true;
}

bool GPU::DumpVRAMToFile(const char* filename, u32 width, u32 height, u32 stride, const void* buffer, bool remove_alpha)
{
  auto fp = FileSystem::OpenManagedCFile(filename, "wb");
  if (!fp)
  {
    Log_ErrorPrintf("Can't open file '%s'", filename);
    return false;
  }

  auto rgba8_buf = std::make_unique<u32[]>(width * height);

  const char* ptr_in = static_cast<const char*>(buffer);
  u32* ptr_out = rgba8_buf.get();
  for (u32 row = 0; row < height; row++)
  {
    const char* row_ptr_in = ptr_in;

    for (u32 col = 0; col < width; col++)
    {
      u16 src_col;
      std::memcpy(&src_col, row_ptr_in, sizeof(u16));
      row_ptr_in += sizeof(u16);
      *(ptr_out++) = RGBA5551ToRGBA8888(remove_alpha ? (src_col | u16(0x8000)) : src_col);
    }

    ptr_in += stride;
  }

  const auto write_func = [](void* context, void* data, int size) {
    std::fwrite(data, 1, size, static_cast<std::FILE*>(context));
  };
  return (stbi_write_png_to_func(write_func, fp.get(), width, height, 4, rgba8_buf.get(), sizeof(u32) * width) != 0);
}

void GPU::DrawDebugStateWindow()
{
#ifdef WITH_IMGUI
  const float framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale.x;

  ImGui::SetNextWindowSize(ImVec2(450.0f * framebuffer_scale, 550.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("GPU", &g_settings.debugging.show_gpu_state))
  {
    ImGui::End();
    return;
  }

  const bool is_idle_frame = m_stats.num_polygons == 0;
  if (!is_idle_frame)
  {
    m_last_stats = m_stats;
    m_stats = {};
  }

  if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const Stats& stats = m_last_stats;

    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f * framebuffer_scale);

    ImGui::TextUnformatted("Idle Frame: ");
    ImGui::NextColumn();
    ImGui::Text("%s", is_idle_frame ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Reads: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_vram_reads);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Fills: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_vram_fills);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Writes: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_vram_writes);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Copies: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_vram_copies);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Vertices Processed: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_vertices);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Polygons Drawn: ");
    ImGui::NextColumn();
    ImGui::Text("%u", stats.num_polygons);
    ImGui::NextColumn();

    ImGui::Columns(1);
  }

  DrawRendererStats(is_idle_frame);

  if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static constexpr std::array<const char*, 5> state_strings = {
      {"Idle", "Reading VRAM", "Writing VRAM", "Drawing Polyline"}};

    ImGui::Text("State: %s", state_strings[static_cast<u8>(m_blitter_state)]);
    ImGui::Text("Dither: %s", m_GPUSTAT.dither_enable ? "Enabled" : "Disabled");
    ImGui::Text("Draw To Displayed Field: %s", m_GPUSTAT.draw_to_displayed_field ? "Enabled" : "Disabled");
    ImGui::Text("Draw Set Mask Bit: %s", m_GPUSTAT.set_mask_while_drawing ? "Yes" : "No");
    ImGui::Text("Draw To Masked Pixels: %s", m_GPUSTAT.check_mask_before_draw ? "Yes" : "No");
    ImGui::Text("Reverse Flag: %s", m_GPUSTAT.reverse_flag ? "Yes" : "No");
    ImGui::Text("Texture Disable: %s", m_GPUSTAT.texture_disable ? "Yes" : "No");
    ImGui::Text("PAL Mode: %s", m_GPUSTAT.pal_mode ? "Yes" : "No");
    ImGui::Text("Interrupt Request: %s", m_GPUSTAT.interrupt_request ? "Yes" : "No");
    ImGui::Text("DMA Request: %s", m_GPUSTAT.dma_data_request ? "Yes" : "No");
  }

  if (ImGui::CollapsingHeader("CRTC", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const auto& cs = m_crtc_state;
    ImGui::Text("Clock: %s", (m_console_is_pal ? (m_GPUSTAT.pal_mode ? "PAL-on-PAL" : "NTSC-on-PAL") :
                                                 (m_GPUSTAT.pal_mode ? "PAL-on-NTSC" : "NTSC-on-NTSC")));
    ImGui::Text("Horizontal Frequency: %.3f KHz", ComputeHorizontalFrequency() / 1000.0f);
    ImGui::Text("Vertical Frequency: %.3f Hz", ComputeVerticalFrequency());
    ImGui::Text("Dot Clock Divider: %u", cs.dot_clock_divider);
    ImGui::Text("Vertical Interlace: %s (%s field)", m_GPUSTAT.vertical_interlace ? "Yes" : "No",
                cs.interlaced_field ? "odd" : "even");
    ImGui::Text("Current Scanline: %u (tick %u)", cs.current_scanline, cs.current_tick_in_scanline);
    ImGui::Text("Display Disable: %s", m_GPUSTAT.display_disable ? "Yes" : "No");
    ImGui::Text("Displaying Odd Lines: %s", cs.active_line_lsb ? "Yes" : "No");
    ImGui::Text("Color Depth: %u-bit", m_GPUSTAT.display_area_color_depth_24 ? 24 : 15);
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
    ImGui::Text("Visible Display Range: %u-%u (%u-%u), %u-%u", cs.horizontal_active_start, cs.horizontal_active_end,
                cs.horizontal_active_start / cs.dot_clock_divider, cs.horizontal_active_end / cs.dot_clock_divider,
                cs.vertical_active_start, cs.vertical_active_end);
    ImGui::Text("Display Resolution: %ux%u", cs.display_width, cs.display_height);
    ImGui::Text("Display Origin: %u, %u", cs.display_origin_left, cs.display_origin_top);
    ImGui::Text("Displayed/Visible VRAM Portion: %ux%u @ (%u, %u)", cs.display_vram_width, cs.display_vram_height,
                cs.display_vram_left, cs.display_vram_top);
    ImGui::Text("Padding: Left=%d, Top=%d, Right=%d, Bottom=%d", cs.display_origin_left, cs.display_origin_top,
                cs.display_width - cs.display_vram_width - cs.display_origin_left,
                cs.display_height - cs.display_vram_height - cs.display_origin_top);
  }

  ImGui::End();
#endif
}

void GPU::DrawRendererStats(bool is_idle_frame) {}
