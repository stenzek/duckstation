// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu.h"
#include "core.h"
#include "dma.h"
#include "gpu_backend.h"
#include "gpu_dump.h"
#include "gpu_helpers.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_shadergen.h"
#include "gpu_sw_rasterizer.h"
#include "gpu_thread.h"
#include "host.h"
#include "interrupt_controller.h"
#include "performance_counters.h"
#include "settings.h"
#include "system.h"
#include "system_private.h"
#include "timers.h"
#include "timing_event.h"

#include "util/gpu_device.h"
#include "util/image.h"
#include "util/imgui_manager.h"
#include "util/media_capture.h"
#include "util/postprocessing.h"
#include "util/shadergen.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/align.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector_formatter.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "IconsEmoji.h"
#include "fmt/format.h"
#include "imgui.h"

#include <cmath>
#include <numbers>
#include <thread>

LOG_CHANNEL(GPU);

ALIGN_TO_CACHE_LINE GPU g_gpu;

// aligning VRAM to 4K is fine, since the ARM64 instructions compute 4K page aligned addresses
// or it would be, except we want to import the memory for readbacks on metal..
#ifdef DYNAMIC_HOST_PAGE_SIZE
#define VRAM_STORAGE_ALIGNMENT MIN_HOST_PAGE_SIZE
#else
#define VRAM_STORAGE_ALIGNMENT HOST_PAGE_SIZE
#endif
alignas(VRAM_STORAGE_ALIGNMENT) u16 g_vram[VRAM_SIZE / sizeof(u16)];
u16 g_gpu_clut[GPU_CLUT_SIZE];

const GPU::GP0CommandHandlerTable GPU::s_GP0_command_handler_table = GPU::GenerateGP0CommandHandlerTable();

static TimingEvent s_crtc_tick_event(
  "GPU CRTC Tick", 1, 1, [](void* param, TickCount ticks, TickCount ticks_late) { g_gpu.CRTCTickEvent(ticks); },
  nullptr);
static TimingEvent s_command_tick_event(
  "GPU Command Tick", 1, 1, [](void* param, TickCount ticks, TickCount ticks_late) { g_gpu.CommandTickEvent(ticks); },
  nullptr);
static TimingEvent s_frame_done_event(
  "Frame Done", 1, 1, [](void* param, TickCount ticks, TickCount ticks_late) { g_gpu.FrameDoneEvent(ticks); }, nullptr);

GPU::GPU() = default;

GPU::~GPU() = default;

void GPU::Initialize()
{
  if (!System::IsReplayingGPUDump())
    s_crtc_tick_event.Activate();

  m_force_progressive_scan = (g_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive);
  m_fifo_size = g_settings.gpu_fifo_size;
  m_max_run_ahead = g_settings.gpu_max_run_ahead;
  m_console_is_pal = System::IsPALRegion();
  UpdateCRTCConfig();
}

void GPU::Shutdown()
{
  s_command_tick_event.Deactivate();
  s_crtc_tick_event.Deactivate();
  s_frame_done_event.Deactivate();

  StopRecordingGPUDump();
}

void GPU::UpdateSettings(const Settings& old_settings)
{
  m_force_progressive_scan = (g_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive);
  m_fifo_size = g_settings.gpu_fifo_size;
  m_max_run_ahead = g_settings.gpu_max_run_ahead;

  if (g_settings.gpu_force_video_timing != old_settings.gpu_force_video_timing)
  {
    m_console_is_pal = System::IsPALRegion();
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

std::tuple<u32, u32> GPU::GetFullDisplayResolution() const
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
    if (!m_GPUSTAT.pal_mode)
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

    width = static_cast<u32>(std::max<s32>(std::clamp<s32>(m_crtc_state.regs.X2, xmin, xmax) -
                                             std::clamp<s32>(m_crtc_state.regs.X1, xmin, xmax),
                                           0) /
                             m_crtc_state.dot_clock_divider);
    height =
      static_cast<u32>(std::max<s32>(
        std::clamp<s32>(m_crtc_state.regs.Y2, ymin, ymax) - std::clamp<s32>(m_crtc_state.regs.Y1, ymin, ymax), 0))
      << BoolToUInt8(m_GPUSTAT.vertical_interlace);
  }

  return std::tie(width, height);
}

void GPU::Reset(bool clear_vram)
{
  m_GPUSTAT.bits = 0x14802000;
  m_set_texture_disable_mask = false;
  m_GPUREAD_latch = 0;
  m_crtc_state.fractional_ticks = 0;
  m_crtc_state.fractional_dot_ticks = 0;
  m_crtc_state.current_tick_in_scanline = 0;
  m_crtc_state.current_scanline = 0;
  m_crtc_state.in_hblank = false;
  m_crtc_state.in_vblank = false;
  m_crtc_state.interlaced_field = 0;
  m_crtc_state.interlaced_display_field = 0;

  // Cancel VRAM writes.
  m_blitter_state = BlitterState::Idle;
  m_active_ticks_since_last_update = 0;

  // Force event to reschedule itself.
  s_crtc_tick_event.Deactivate();
  s_command_tick_event.Deactivate();

  SoftReset();

  // Can skip the VRAM clear if it's not a hardware reset.
  if (clear_vram)
    GPUBackend::PushCommand(GPUBackend::NewClearVRAMCommand());
}

void GPU::SoftReset()
{
  if (m_blitter_state == BlitterState::WritingVRAM)
    FinishVRAMWrite();

  m_GPUSTAT.texture_page_x_base = 0;
  m_GPUSTAT.texture_page_y_base = 0;
  m_GPUSTAT.semi_transparency_mode = GPUTransparencyMode::HalfBackgroundPlusHalfForeground;
  m_GPUSTAT.texture_color_mode = GPUTextureMode::Palette4Bit;
  m_GPUSTAT.dither_enable = false;
  m_GPUSTAT.draw_to_displayed_field = false;
  m_GPUSTAT.set_mask_while_drawing = false;
  m_GPUSTAT.check_mask_before_draw = false;
  m_GPUSTAT.reverse_flag = false;
  m_GPUSTAT.texture_disable = false;
  m_GPUSTAT.horizontal_resolution_2 = 0;
  m_GPUSTAT.horizontal_resolution_1 = 0;
  m_GPUSTAT.vertical_resolution = false;
  m_GPUSTAT.pal_mode = System::IsPALRegion();
  m_GPUSTAT.display_area_color_depth_24 = false;
  m_GPUSTAT.vertical_interlace = false;
  m_GPUSTAT.display_disable = true;
  m_GPUSTAT.dma_direction = GPUDMADirection::Off;
  m_drawing_area = {};
  m_drawing_area_changed = true;
  m_drawing_offset = {};
  std::memset(&m_crtc_state.regs, 0, sizeof(m_crtc_state.regs));
  m_crtc_state.regs.horizontal_display_range = 0xC60260;
  m_crtc_state.regs.vertical_display_range = 0x3FC10;
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

  sw.Do(&m_GPUSTAT.bits);

  sw.Do(&m_draw_mode.mode_reg.bits);
  sw.Do(&m_draw_mode.palette_reg.bits);
  sw.Do(&m_draw_mode.texture_window_value);

  if (sw.GetVersion() < 62) [[unlikely]]
  {
    // texture_page_x, texture_page_y, texture_palette_x, texture_palette_y
    DebugAssert(sw.IsReading());
    sw.SkipBytes(sizeof(u32) * 4);
  }

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
  sw.Do(&m_crtc_state.horizontal_visible_start);
  sw.Do(&m_crtc_state.horizontal_visible_end);
  sw.Do(&m_crtc_state.horizontal_display_start);
  sw.Do(&m_crtc_state.horizontal_display_end);
  sw.Do(&m_crtc_state.vertical_total);
  sw.Do(&m_crtc_state.vertical_visible_start);
  sw.Do(&m_crtc_state.vertical_visible_end);
  sw.Do(&m_crtc_state.vertical_display_start);
  sw.Do(&m_crtc_state.vertical_display_end);
  sw.Do(&m_crtc_state.fractional_ticks);
  sw.Do(&m_crtc_state.current_tick_in_scanline);
  m_crtc_state.current_scanline = Truncate16(sw.DoValue(static_cast<u32>(m_crtc_state.current_scanline)));
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

  u16 load_clut_data[GPU_CLUT_SIZE];
  if (sw.GetVersion() < 64) [[unlikely]]
  {
    // Clear CLUT cache and let it populate later.
    InvalidateCLUT();
    std::memset(load_clut_data, 0, sizeof(load_clut_data));
  }
  else
  {
    sw.Do(&m_current_clut_reg_bits);
    sw.Do(&m_current_clut_is_8bit);

    // I hate this extra copy... because I'm a moron and put it in the middle of the state data.
    sw.DoArray(sw.IsReading() ? load_clut_data : g_gpu_clut, std::size(g_gpu_clut));
  }

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
    GPUBackendLoadStateCommand* cmd = static_cast<GPUBackendLoadStateCommand*>(
      GPUThread::AllocateCommand(GPUBackendCommandType::LoadState, sizeof(GPUBackendLoadStateCommand) + tc_data_size));
    std::memcpy(cmd->clut_data, load_clut_data, sizeof(cmd->clut_data));
    std::memcpy(cmd->vram_data, sw.GetData() + vram_start_pos, VRAM_SIZE);
    cmd->texture_cache_state_version = sw.GetVersion();
    cmd->texture_cache_state_size = tc_data_size;
    if (tc_data_size > 0)
      std::memcpy(cmd->texture_cache_state, sw.GetData() + vram_start_pos + VRAM_SIZE, tc_data_size);
    GPUThread::PushCommand(cmd);

    m_drawing_area_changed = true;
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
  sw.Do(&m_GPUSTAT.bits);

  sw.DoBytes(&m_draw_mode, sizeof(m_draw_mode));
  sw.DoBytes(&m_drawing_area, sizeof(m_drawing_area));
  sw.DoBytes(&m_drawing_offset, sizeof(m_drawing_offset));

  sw.Do(&m_console_is_pal);
  sw.Do(&m_set_texture_disable_mask);

  sw.DoBytes(&m_crtc_state, sizeof(m_crtc_state));

  sw.Do(&m_blitter_state);
  sw.Do(&m_pending_command_ticks);
  sw.Do(&m_command_total_words);
  sw.Do(&m_GPUREAD_latch);

  sw.Do(&m_current_clut_reg_bits);
  sw.Do(&m_current_clut_is_8bit);

  sw.DoBytes(&m_vram_transfer, sizeof(m_vram_transfer));

  sw.Do(&m_fifo);
  sw.Do(&m_blit_buffer);
  sw.Do(&m_blit_remaining_words);
  sw.Do(&m_render_command.bits);

  if (sw.IsReading())
  {
    m_drawing_area_changed = true;
    SetClampedDrawingArea();
    UpdateDMARequest();
    UpdateCRTCConfig();
    UpdateCommandTickEvent();
  }

  // Push to thread.
  GPUBackendDoMemoryStateCommand* cmd = static_cast<GPUBackendDoMemoryStateCommand*>(GPUThread::AllocateCommand(
    sw.IsReading() ? GPUBackendCommandType::LoadMemoryState : GPUBackendCommandType::SaveMemoryState,
    sizeof(GPUBackendDoMemoryStateCommand)));
  cmd->memory_save_state = &mss;
  GPUThread::PushCommandAndWakeThread(cmd);
}

void GPU::UpdateDMARequest()
{
  switch (m_blitter_state)
  {
    case BlitterState::Idle:
      m_GPUSTAT.ready_to_send_vram = false;
      m_GPUSTAT.ready_to_receive_dma = (m_fifo.IsEmpty() || m_fifo.GetSize() < m_command_total_words);
      break;

    case BlitterState::WritingVRAM:
      m_GPUSTAT.ready_to_send_vram = false;
      m_GPUSTAT.ready_to_receive_dma = (m_fifo.GetSize() < m_fifo_size);
      break;

    case BlitterState::ReadingVRAM:
      m_GPUSTAT.ready_to_send_vram = true;
      m_GPUSTAT.ready_to_receive_dma = false;
      break;

    case BlitterState::DrawingPolyLine:
      m_GPUSTAT.ready_to_send_vram = false;
      m_GPUSTAT.ready_to_receive_dma = (m_fifo.GetSize() < m_fifo_size);
      break;

    default:
      UnreachableCode();
      break;
  }

  bool dma_request;
  switch (m_GPUSTAT.dma_direction)
  {
    case GPUDMADirection::Off:
      dma_request = false;
      break;

    case GPUDMADirection::FIFO:
      dma_request = m_GPUSTAT.ready_to_receive_dma;
      break;

    case GPUDMADirection::CPUtoGP0:
      dma_request = m_GPUSTAT.ready_to_receive_dma;
      break;

    case GPUDMADirection::GPUREADtoCPU:
      dma_request = m_GPUSTAT.ready_to_send_vram;
      break;

    default:
      dma_request = false;
      break;
  }
  m_GPUSTAT.dma_data_request = dma_request;
  DMA::SetRequest(DMA::Channel::GPU, dma_request);
}

void GPU::UpdateGPUIdle()
{
  m_GPUSTAT.gpu_idle = (m_blitter_state == BlitterState::Idle && m_pending_command_ticks <= 0 && m_fifo.IsEmpty());
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
        s_command_tick_event.InvokeEarly();

      return m_GPUSTAT.bits;
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
      if (m_gpu_dump) [[unlikely]]
        m_gpu_dump->WriteGP0Packet(value);

      // FIFO can be overflowed through direct GP0 writes if the command tick event hasn't run, because
      // there's no backpressure applied to the CPU. Instead force the GPU to run and catch up.
      if (m_fifo.GetSize() >= m_fifo_size) [[unlikely]]
      {
        s_command_tick_event.InvokeEarly();

        if (m_fifo.GetSize() >= m_fifo.GetCapacity()) [[unlikely]]
        {
          WARNING_LOG("GPU FIFO overflow via GP0 write, size={}", m_fifo.GetSize());
          return;
        }
      }

      m_fifo.Push(value);
      ExecuteCommands();
      return;
    }

    case 0x04:
    {
      if (m_gpu_dump) [[unlikely]]
        m_gpu_dump->WriteGP1Packet(value);

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
  if (m_GPUSTAT.dma_direction != GPUDMADirection::GPUREADtoCPU)
  {
    ERROR_LOG("Invalid DMA direction from GPU DMA read");
    std::fill_n(words, word_count, UINT32_C(0xFFFFFFFF));
    return;
  }

  for (u32 i = 0; i < word_count; i++)
    words[i] = ReadGPUREAD();
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
  u64 mul = u64(sysclk_ticks);
  mul *= !m_console_is_pal ? u64(715909) : u64(709379);
  mul += u64(*fractional_ticks);

  const TickCount ticks = static_cast<TickCount>(mul / u64(451584));
  *fractional_ticks = static_cast<TickCount>(mul % u64(451584));
  return ticks;
}

void GPU::AddCommandTicks(TickCount ticks)
{
  m_pending_command_ticks += ticks;
  m_active_ticks_since_last_update += ticks;
}

void GPU::SynchronizeCRTC()
{
  s_crtc_tick_event.InvokeEarly();
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

float GPU::ComputePixelAspectRatio() const
{
  float sar = (m_crtc_state.display_width > 0 && m_crtc_state.display_height > 0) ?
                static_cast<float>(m_crtc_state.display_width) / static_cast<float>(m_crtc_state.display_height) :
                1.0f;

  // Force 4:3 for 24-bit modes option.
  const DisplayAspectRatio dar_type =
    (!g_settings.display_force_4_3_for_24bit || !m_GPUSTAT.display_area_color_depth_24) ?
      g_settings.display_aspect_ratio :
      DisplayAspectRatio::Auto();
  float dar = 4.0f / 3.0f;
  if (dar_type == DisplayAspectRatio::PAR1_1())
  {
    dar = sar;
  }
  else if (dar_type == DisplayAspectRatio::Stretch())
  {
    const WindowInfo& wi = GPUThread::GetRenderWindowInfo();
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

float GPU::ComputeAspectRatioCorrection() const
{
  const CRTCState& cs = m_crtc_state;
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
      if (m_GPUSTAT.pal_mode)
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
      if (m_GPUSTAT.pal_mode)
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

GSVector2 GPU::CalculateDisplayWindowSize(DisplayFineCropMode mode, std::span<const s16, 4> amount,
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
  CRTCState& cs = m_crtc_state;

  cs.vertical_total = m_GPUSTAT.pal_mode ? PAL_TOTAL_LINES : NTSC_TOTAL_LINES;
  cs.horizontal_total = m_GPUSTAT.pal_mode ? PAL_TICKS_PER_LINE : NTSC_TICKS_PER_LINE;
  cs.horizontal_active_start = m_GPUSTAT.pal_mode ? PAL_HORIZONTAL_ACTIVE_START : NTSC_HORIZONTAL_ACTIVE_START;
  cs.horizontal_active_end = m_GPUSTAT.pal_mode ? PAL_HORIZONTAL_ACTIVE_END : NTSC_HORIZONTAL_ACTIVE_END;

  const u8 horizontal_resolution_index = m_GPUSTAT.horizontal_resolution_1 | (m_GPUSTAT.horizontal_resolution_2 << 2);
  cs.dot_clock_divider = dot_clock_dividers[horizontal_resolution_index];
  cs.horizontal_display_start =
    (std::min<u16>(cs.regs.X1, cs.horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  cs.horizontal_display_end =
    (std::min<u16>(cs.regs.X2, cs.horizontal_total) / cs.dot_clock_divider) * cs.dot_clock_divider;
  cs.vertical_display_start = std::min<u16>(cs.regs.Y1, cs.vertical_total);
  cs.vertical_display_end = std::min<u16>(cs.regs.Y2, cs.vertical_total);

  if (m_GPUSTAT.pal_mode && g_settings.gpu_force_video_timing == ForceVideoTimingMode::NTSC)
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
  else if (!m_GPUSTAT.pal_mode && g_settings.gpu_force_video_timing == ForceVideoTimingMode::PAL)
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
  cs.UpdateHBlankFlag();

  cs.current_scanline %= cs.vertical_total;

  System::SetVideoFrameRate(ComputeVerticalFrequency());

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
  const u16 old_horizontal_visible_start = cs.horizontal_visible_start;
  const u16 old_horizontal_visible_end = cs.horizontal_visible_end;
  const u16 old_vertical_visible_start = cs.vertical_visible_start;
  const u16 old_vertical_visible_end = cs.vertical_visible_end;

  if (m_GPUSTAT.pal_mode)
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
  const u8 y_shift = BoolToUInt8(m_GPUSTAT.vertical_interlace && m_GPUSTAT.vertical_resolution);
  const u8 height_shift = m_force_progressive_scan ? y_shift : BoolToUInt8(m_GPUSTAT.vertical_interlace);
  const u16 old_vram_width = m_crtc_state.display_vram_width;
  const u16 old_vram_height = m_crtc_state.display_vram_height;

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

GSVector2i GPU::GetCRTCVideoSize() const
{
  // Verify assumptions about struct layout.
  static_assert(offsetof(CRTCState, display_width) + sizeof(u16) == offsetof(CRTCState, display_height));
  return GSVector2i::load32(&m_crtc_state.display_width).u16to32();
}

GSVector4i GPU::GetCRTCVideoActiveRect() const
{
  static_assert(offsetof(CRTCState, display_origin_left) + sizeof(u16) == offsetof(CRTCState, display_origin_top) &&
                offsetof(CRTCState, display_vram_width) + sizeof(u16) == offsetof(CRTCState, display_vram_height));
  const GSVector2i origin = GSVector2i::load32(&m_crtc_state.display_origin_left).u16to32();
  const GSVector2i size = GSVector2i::load32(&m_crtc_state.display_vram_width).u16to32();
  return GSVector4i::xyxy(origin, origin.add32(size));
}

GSVector4i GPU::GetCRTCVRAMSourceRect() const
{
  static_assert(offsetof(CRTCState, display_vram_left) + sizeof(u16) == offsetof(CRTCState, display_vram_top) &&
                offsetof(CRTCState, display_vram_top) + sizeof(u16) == offsetof(CRTCState, display_vram_width) &&
                offsetof(CRTCState, display_vram_width) + sizeof(u16) == offsetof(CRTCState, display_vram_height));
  const GSVector4i rc = GSVector4i::loadl<false>(&m_crtc_state.display_vram_left).u16to32();
  const GSVector2i origin = rc.xy();
  return GSVector4i::xyxy(origin, origin.add32(rc.zw()));
}

TickCount GPU::GetPendingCRTCTicks() const
{
  const TickCount pending_sysclk_ticks = s_crtc_tick_event.GetTicksSinceLastExecution();
  TickCount fractional_ticks = m_crtc_state.fractional_ticks;
  return SystemTicksToCRTCTicks(pending_sysclk_ticks, &fractional_ticks);
}

TickCount GPU::GetPendingCommandTicks() const
{
  if (!s_command_tick_event.IsActive())
    return 0;

  return SystemTicksToGPUTicks(s_command_tick_event.GetTicksSinceLastExecution());
}

TickCount GPU::GetRemainingCommandTicks() const
{
  return std::max<TickCount>(m_pending_command_ticks - GetPendingCommandTicks(), 0);
}

void GPU::UpdateCRTCTickEvent()
{
  // figure out how many GPU ticks until the next vblank or event
  TickCount lines_until_event;
  if (Timers::IsSyncEnabled(HBLANK_TIMER_INDEX))
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
  if (Timers::IsExternalIRQEnabled(HBLANK_TIMER_INDEX))
    lines_until_event = std::min(lines_until_event, Timers::GetTicksUntilIRQ(HBLANK_TIMER_INDEX));

  TickCount ticks_until_event =
    lines_until_event * m_crtc_state.horizontal_total - m_crtc_state.current_tick_in_scanline;
  if (Timers::IsExternalIRQEnabled(DOT_TIMER_INDEX))
  {
    const TickCount dots_until_irq = Timers::GetTicksUntilIRQ(DOT_TIMER_INDEX);
    const TickCount ticks_until_irq =
      (dots_until_irq * m_crtc_state.dot_clock_divider) - m_crtc_state.fractional_dot_ticks;
    ticks_until_event = std::min(ticks_until_event, std::max<TickCount>(ticks_until_irq, 0));
  }

  if (Timers::IsSyncEnabled(DOT_TIMER_INDEX))
  {
    // This could potentially be optimized to skip the time the gate is active, if we're resetting and free running.
    // But realistically, I've only seen sync off (most games), or reset+pause on gate (Konami Lightgun games).
    TickCount ticks_until_hblank_start_or_end;
    if (m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_active_end)
    {
      ticks_until_hblank_start_or_end =
        m_crtc_state.horizontal_total - m_crtc_state.current_tick_in_scanline + m_crtc_state.horizontal_active_start;
    }
    else if (m_crtc_state.current_tick_in_scanline < m_crtc_state.horizontal_active_start)
    {
      ticks_until_hblank_start_or_end = m_crtc_state.horizontal_active_start - m_crtc_state.current_tick_in_scanline;
    }
    else
    {
      ticks_until_hblank_start_or_end = m_crtc_state.horizontal_active_end - m_crtc_state.current_tick_in_scanline;
    }

    ticks_until_event = std::min(ticks_until_event, ticks_until_hblank_start_or_end);
  }

  if (!System::IsReplayingGPUDump()) [[likely]]
    s_crtc_tick_event.Schedule(CRTCTicksToSystemTicks(ticks_until_event, m_crtc_state.fractional_ticks));
}

bool GPU::IsCRTCScanlinePending() const
{
  // TODO: Most of these should be fields, not lines.
  const TickCount ticks = (GetPendingCRTCTicks() + m_crtc_state.current_tick_in_scanline);
  return (ticks >= m_crtc_state.horizontal_total);
}

bool GPU::IsCommandCompletionPending() const
{
  return (m_pending_command_ticks > 0 && GetPendingCommandTicks() >= m_pending_command_ticks);
}

void GPU::CRTCTickEvent(TickCount ticks)
{
  // convert cpu/master clock to GPU ticks, accounting for partial cycles because of the non-integer divider
  const TickCount prev_tick = m_crtc_state.current_tick_in_scanline;
  const TickCount gpu_ticks = SystemTicksToCRTCTicks(ticks, &m_crtc_state.fractional_ticks);
  m_crtc_state.current_tick_in_scanline += gpu_ticks;

  if (Timers::IsUsingExternalClock(DOT_TIMER_INDEX))
  {
    m_crtc_state.fractional_dot_ticks += gpu_ticks;
    const TickCount dots = m_crtc_state.fractional_dot_ticks / m_crtc_state.dot_clock_divider;
    m_crtc_state.fractional_dot_ticks = m_crtc_state.fractional_dot_ticks % m_crtc_state.dot_clock_divider;
    if (dots > 0)
      Timers::AddTicks(DOT_TIMER_INDEX, dots);
  }

  if (m_crtc_state.current_tick_in_scanline < m_crtc_state.horizontal_total)
  {
    // short path when we execute <1 line.. this shouldn't occur often, except when gated (konami lightgun games).
    m_crtc_state.UpdateHBlankFlag();
    Timers::SetGate(DOT_TIMER_INDEX, m_crtc_state.in_hblank);
    if (Timers::IsUsingExternalClock(HBLANK_TIMER_INDEX))
    {
      const u32 hblank_timer_ticks =
        BoolToUInt32(m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_active_end) -
        BoolToUInt32(prev_tick >= m_crtc_state.horizontal_active_end);
      if (hblank_timer_ticks > 0)
        Timers::AddTicks(HBLANK_TIMER_INDEX, static_cast<TickCount>(hblank_timer_ticks));
    }

    UpdateCRTCTickEvent();
    return;
  }

  u32 lines_to_draw = m_crtc_state.current_tick_in_scanline / m_crtc_state.horizontal_total;
  m_crtc_state.current_tick_in_scanline %= m_crtc_state.horizontal_total;
#if 0
  Log_WarningPrintf("Old line: %u, new line: %u, drawing %u", m_crtc_state.current_scanline,
    m_crtc_state.current_scanline + lines_to_draw, lines_to_draw);
#endif

  m_crtc_state.UpdateHBlankFlag();
  Timers::SetGate(DOT_TIMER_INDEX, m_crtc_state.in_hblank);

  if (Timers::IsUsingExternalClock(HBLANK_TIMER_INDEX))
  {
    // lines_to_draw => number of times ticks passed horizontal_total.
    // Subtract one if we were previously in hblank, but only on that line. If it was previously less than
    // horizontal_active_start, we still want to add one, because hblank would have gone inactive, and then active again
    // during the line. Finally add the current line being drawn, if hblank went inactive->active during the line.
    const u32 hblank_timer_ticks =
      lines_to_draw - BoolToUInt32(prev_tick >= m_crtc_state.horizontal_active_end) +
      BoolToUInt32(m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_active_end);
    if (hblank_timer_ticks > 0)
      Timers::AddTicks(HBLANK_TIMER_INDEX, static_cast<TickCount>(hblank_timer_ticks));
  }

  bool frame_done = false;
  while (lines_to_draw > 0)
  {
    const u32 lines_to_draw_this_loop =
      std::min(lines_to_draw, static_cast<u32>(m_crtc_state.vertical_total - m_crtc_state.current_scanline));
    const u32 prev_scanline = m_crtc_state.current_scanline;
    m_crtc_state.current_scanline = Truncate16(m_crtc_state.current_scanline + lines_to_draw_this_loop);
    DebugAssert(m_crtc_state.current_scanline <= m_crtc_state.vertical_total);
    lines_to_draw -= lines_to_draw_this_loop;

    // clear the vblank flag if the beam would pass through the display area
    if (prev_scanline < m_crtc_state.vertical_display_start &&
        m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end)
    {
      Timers::SetGate(HBLANK_TIMER_INDEX, false);
      InterruptController::SetLineState(InterruptController::IRQ::VBLANK, false);
      m_crtc_state.in_vblank = false;
    }

    const bool new_vblank = m_crtc_state.current_scanline < m_crtc_state.vertical_display_start ||
                            m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end;
    if (m_crtc_state.in_vblank != new_vblank)
    {
      if (new_vblank)
      {
        DEBUG_LOG("Now in v-blank");

        if (m_gpu_dump) [[unlikely]]
        {
          m_gpu_dump->WriteVSync(System::GetGlobalTickCounter());
          if (m_gpu_dump->IsFinished()) [[unlikely]]
            StopRecordingGPUDump();
        }

        // flush any pending draws and "scan out" the image
        // TODO: move present in here I guess
        System::IncrementFrameNumber();
        UpdateDisplay(!System::IsRunaheadActive());
        frame_done = true;

        // switch fields early. this is needed so we draw to the correct one.
        if (m_GPUSTAT.InInterleaved480iMode())
          m_crtc_state.interlaced_display_field = m_crtc_state.interlaced_field ^ 1u;
        else
          m_crtc_state.interlaced_display_field = 0;
      }

      Timers::SetGate(HBLANK_TIMER_INDEX, new_vblank);
      InterruptController::SetLineState(InterruptController::IRQ::VBLANK, new_vblank);
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
        m_GPUSTAT.interlaced_field = BoolToUInt8(!ConvertToBoolUnchecked(m_crtc_state.interlaced_field));
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
      (m_crtc_state.regs.Y + (BoolToUInt8(!m_crtc_state.in_vblank) & m_crtc_state.interlaced_display_field)) & u32(1));
  }
  else
  {
    m_crtc_state.active_line_lsb = 0;
    m_GPUSTAT.display_line_lsb = ConvertToBoolUnchecked((m_crtc_state.regs.Y + m_crtc_state.current_scanline) & u32(1));
  }

  UpdateCRTCTickEvent();

  if (frame_done)
  {
    // we can't issue frame done if we're in the middle of executing a rec block, e.g. from reading GPUSTAT
    // defer it until the end of the block in this case.
    if (!TimingEvents::IsRunningEvents()) [[unlikely]]
    {
      DEBUG_LOG("Deferring frame done call");
      s_frame_done_event.Schedule(0);
    }
    else
    {
      System::FrameDone();
    }
  }
}

void GPU::CommandTickEvent(TickCount ticks)
{
  m_pending_command_ticks -= SystemTicksToGPUTicks(ticks);

  m_executing_commands = true;
  ExecuteCommands();
  UpdateCommandTickEvent();
  m_executing_commands = false;
}

void GPU::FrameDoneEvent(TickCount ticks)
{
  DebugAssert(TimingEvents::IsRunningEvents());
  s_frame_done_event.Deactivate();
  System::FrameDone();
}

void GPU::UpdateCommandTickEvent()
{
  if (m_pending_command_ticks <= 0)
  {
    m_pending_command_ticks = 0;
    s_command_tick_event.Deactivate();
  }
  else
  {
    s_command_tick_event.SetIntervalAndSchedule(GPUTicksToSystemTicks(m_pending_command_ticks));
  }
}

u8 GPU::UpdateOrGetGPUBusyPct()
{
  const u32 frame_number = System::GetFrameNumber();
  if ((m_GPUSTAT.pal_mode ? (frame_number % 50) : (frame_number % 60)) != 0) [[likely]]
    return m_last_gpu_busy_pct;

  const double busy_frac =
    static_cast<double>(m_active_ticks_since_last_update) /
    static_cast<double>(SystemTicksToGPUTicks(System::ScaleTicksToOverclock(System::MASTER_CLOCK)) *
                        (ComputeVerticalFrequency() / (m_GPUSTAT.pal_mode ? 50.0f : 60.0f)));
  const double usage_pct = busy_frac * 100.0;

  DEBUG_LOG("PSX GPU Usage: {:.2f}% [{:.0f} cycles avg per frame]", usage_pct,
            static_cast<double>(m_active_ticks_since_last_update) / (m_GPUSTAT.pal_mode ? 50.0f : 60.0f));
  m_active_ticks_since_last_update = 0;

  m_last_gpu_busy_pct = static_cast<u8>(std::min<double>(std::round(usage_pct), 100));
  return m_last_gpu_busy_pct;
}

GSVector2 GPU::ConvertScreenCoordinatesToDisplayCoordinates(GSVector2 window_pos) const
{
  const WindowInfo& wi = GPUThread::GetRenderWindowInfo();
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
                                                       u32* out_line) const
{
  float display_x = display_pos.x;
  float display_y = display_pos.y;
  if (x_scale != 1.0f)
  {
    const float dw = static_cast<float>(m_crtc_state.display_width);
    float scaled_x = ((display_x / dw) * 2.0f) - 1.0f; // 0..1 -> -1..1
    scaled_x *= x_scale;
    display_x = (((scaled_x + 1.0f) * 0.5f) * dw); // -1..1 -> 0..1
  }

  if (display_x < 0 || static_cast<u32>(display_x) >= m_crtc_state.display_width || display_y < 0 ||
      static_cast<u32>(display_y) >= m_crtc_state.display_height)
  {
    return false;
  }

  *out_line = (static_cast<u32>(std::round(display_y)) >> BoolToUInt8(m_GPUSTAT.vertical_interlace)) +
              m_crtc_state.vertical_visible_start;
  *out_tick = static_cast<u32>(System::ScaleTicksToOverclock(
                static_cast<TickCount>(std::round(display_x * static_cast<float>(m_crtc_state.dot_clock_divider))))) +
              m_crtc_state.horizontal_visible_start;
  return true;
}

void GPU::GetBeamPosition(u32* out_ticks, u32* out_line)
{
  const u32 current_tick = (GetPendingCRTCTicks() + m_crtc_state.current_tick_in_scanline);
  *out_line =
    (m_crtc_state.current_scanline + (current_tick / m_crtc_state.horizontal_total)) % m_crtc_state.vertical_total;
  *out_ticks = current_tick % m_crtc_state.horizontal_total;
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
    ticks_to_target = (m_crtc_state.horizontal_total - current_tick) + ticks;
    current_line = (current_line + 1) % m_crtc_state.vertical_total;
  }

  const u32 lines_to_target =
    (line >= current_line) ? (line - current_line) : ((m_crtc_state.vertical_total - current_line) + line);

  const TickCount total_ticks_to_target =
    static_cast<TickCount>((lines_to_target * m_crtc_state.horizontal_total) + ticks_to_target);

  return CRTCTicksToSystemTicks(total_ticks_to_target, m_crtc_state.fractional_ticks);
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
    value |= ZeroExtend32(g_vram[read_y * VRAM_WIDTH + read_x]) << (i * 16);

    if (++m_vram_transfer.col == m_vram_transfer.width)
    {
      m_vram_transfer.col = 0;

      if (++m_vram_transfer.row == m_vram_transfer.height)
      {
        DEBUG_LOG("End of VRAM->CPU transfer");
        m_vram_transfer = {};
        m_blitter_state = BlitterState::Idle;

        // end of transfer, catch up on any commands which were written (unlikely)
        ExecuteCommands();
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
    case static_cast<u8>(GP1Command::ResetGPU):
    {
      DEBUG_LOG("GP1 reset GPU");
      s_command_tick_event.InvokeEarly();
      SynchronizeCRTC();
      SoftReset();
    }
    break;

    case static_cast<u8>(GP1Command::ClearFIFO):
    {
      DEBUG_LOG("GP1 clear FIFO");
      s_command_tick_event.InvokeEarly();
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
      s_command_tick_event.Deactivate();
      UpdateDMARequest();
      UpdateGPUIdle();
    }
    break;

    case static_cast<u8>(GP1Command::AcknowledgeInterrupt):
    {
      DEBUG_LOG("Acknowledge interrupt");
      m_GPUSTAT.interrupt_request = false;
      InterruptController::SetLineState(InterruptController::IRQ::GPU, false);
    }
    break;

    case static_cast<u8>(GP1Command::SetDisplayDisable):
    {
      const bool disable = ConvertToBoolUnchecked(value & 0x01);
      DEBUG_LOG("Display {}", disable ? "disabled" : "enabled");
      SynchronizeCRTC();

      m_GPUSTAT.display_disable = disable;
    }
    break;

    case static_cast<u8>(GP1Command::SetDMADirection):
    {
      DEBUG_LOG("DMA direction <- 0x{:02X}", static_cast<u32>(param));
      if (m_GPUSTAT.dma_direction != static_cast<GPUDMADirection>(param))
      {
        m_GPUSTAT.dma_direction = static_cast<GPUDMADirection>(param);
        UpdateDMARequest();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetDisplayStartAddress):
    {
      const u32 new_value = param & CRTCState::Regs::DISPLAY_ADDRESS_START_MASK;
      DEBUG_LOG("Display address start <- 0x{:08X}", new_value);

      System::IncrementInternalFrameNumber();
      if (m_crtc_state.regs.display_address_start != new_value)
      {
        SynchronizeCRTC();
        m_crtc_state.regs.display_address_start = new_value;
        UpdateCRTCDisplayParameters();
        GPUBackend::PushCommand(GPUBackend::NewBufferSwappedCommand());
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetHorizontalDisplayRange):
    {
      const u32 new_value = param & CRTCState::Regs::HORIZONTAL_DISPLAY_RANGE_MASK;
      DEBUG_LOG("Horizontal display range <- 0x{:08X}", new_value);

      if (m_crtc_state.regs.horizontal_display_range != new_value)
      {
        SynchronizeCRTC();
        m_crtc_state.regs.horizontal_display_range = new_value;
        UpdateCRTCConfig();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetVerticalDisplayRange):
    {
      const u32 new_value = param & CRTCState::Regs::VERTICAL_DISPLAY_RANGE_MASK;
      DEBUG_LOG("Vertical display range <- 0x{:08X}", new_value);

      if (m_crtc_state.regs.vertical_display_range != new_value)
      {
        SynchronizeCRTC();
        m_crtc_state.regs.vertical_display_range = new_value;
        UpdateCRTCConfig();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetDisplayMode):
    {
      const GP1SetDisplayMode dm{param};
      GPUSTAT new_GPUSTAT{m_GPUSTAT.bits};
      new_GPUSTAT.horizontal_resolution_1 = dm.horizontal_resolution_1;
      new_GPUSTAT.vertical_resolution = dm.vertical_resolution;
      new_GPUSTAT.pal_mode = dm.pal_mode;
      new_GPUSTAT.display_area_color_depth_24 = dm.display_area_color_depth;
      new_GPUSTAT.vertical_interlace = dm.vertical_interlace;
      new_GPUSTAT.horizontal_resolution_2 = dm.horizontal_resolution_2;
      new_GPUSTAT.reverse_flag = dm.reverse_flag;
      DEBUG_LOG("Set display mode <- 0x{:08X}", dm.bits);

      if (!m_GPUSTAT.vertical_interlace && dm.vertical_interlace && !m_force_progressive_scan)
      {
        // bit of a hack, technically we should pull the previous frame in, but this may not exist anymore
        ClearDisplay();
      }

      if (m_GPUSTAT.bits != new_GPUSTAT.bits)
      {
        // Have to be careful when setting this because Synchronize() can modify GPUSTAT.
        static constexpr u32 SET_MASK = UINT32_C(0b00000000011111110100000000000000);
        s_command_tick_event.InvokeEarly();
        SynchronizeCRTC();
        m_GPUSTAT.bits = (m_GPUSTAT.bits & ~SET_MASK) | (new_GPUSTAT.bits & SET_MASK);
        UpdateCRTCConfig();
      }
    }
    break;

    case static_cast<u8>(GP1Command::SetAllowTextureDisable):
    {
      m_set_texture_disable_mask = ConvertToBoolUnchecked(param & 0x01);
      DEBUG_LOG("Set texture disable mask <- {}", m_set_texture_disable_mask ? "allowed" : "ignored");
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
      m_GPUREAD_latch = m_draw_mode.texture_window_value;
      DEBUG_LOG("Get texture window => 0x{:08X}", m_GPUREAD_latch);
    }
    break;

    case 0x03: // Get Draw Area Top Left
    {
      m_GPUREAD_latch = (m_drawing_area.left | (m_drawing_area.top << 10));
      DEBUG_LOG("Get drawing area top left: ({}, {}) => 0x{:08X}", m_drawing_area.left, m_drawing_area.top,
                m_GPUREAD_latch);
    }
    break;

    case 0x04: // Get Draw Area Bottom Right
    {
      m_GPUREAD_latch = (m_drawing_area.right | (m_drawing_area.bottom << 10));
      DEBUG_LOG("Get drawing area bottom right: ({}, {}) => 0x{:08X}", m_drawing_area.bottom, m_drawing_area.right,
                m_GPUREAD_latch);
    }
    break;

    case 0x05: // Get Drawing Offset
    {
      m_GPUREAD_latch = (m_drawing_offset.x & 0x7FF) | ((m_drawing_offset.y & 0x7FF) << 11);
      DEBUG_LOG("Get drawing offset: ({}, {}) => 0x{:08X}", m_drawing_offset.x, m_drawing_offset.y, m_GPUREAD_latch);
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
  if ((clut.bits != m_current_clut_reg_bits) || BoolToUInt8(needs_8bit) > BoolToUInt8(m_current_clut_is_8bit))
  {
    DEBUG_LOG("Reloading CLUT from {},{}, {}", clut.GetXBase(), clut.GetYBase(), needs_8bit ? "8-bit" : "4-bit");
    AddCommandTicks(needs_8bit ? 256 : 16);
    m_current_clut_reg_bits = clut.bits;
    m_current_clut_is_8bit = needs_8bit;

    GPUBackendUpdateCLUTCommand* cmd = GPUBackend::NewUpdateCLUTCommand();
    cmd->reg.bits = clut.bits;
    cmd->clut_is_8bit = needs_8bit;
    GPUBackend::PushCommand(cmd);
  }
}

void GPU::InvalidateCLUT()
{
  m_current_clut_reg_bits = std::numeric_limits<decltype(m_current_clut_reg_bits)>::max(); // will never match
  m_current_clut_is_8bit = false;
}

bool GPU::IsCLUTValid() const
{
  return (m_current_clut_reg_bits != std::numeric_limits<decltype(m_current_clut_reg_bits)>::max());
}

void GPU::SetClampedDrawingArea()
{
  m_clamped_drawing_area = GetClampedDrawingArea(m_drawing_area);
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
  if (!m_set_texture_disable_mask)
    new_mode_reg.texture_disable = false;

  m_draw_mode.mode_reg.bits = new_mode_reg.bits;

  // Bits 0..10 are returned in the GPU status register.
  m_GPUSTAT.bits = (m_GPUSTAT.bits & ~(GPUDrawModeReg::GPUSTAT_MASK)) |
                   (ZeroExtend32(new_mode_reg.bits) & GPUDrawModeReg::GPUSTAT_MASK);
  m_GPUSTAT.texture_disable = m_draw_mode.mode_reg.texture_disable;
}

void GPU::SetTexturePalette(u16 value)
{
  value &= DrawMode::PALETTE_MASK;
  m_draw_mode.palette_reg.bits = value;
}

void GPU::SetTextureWindow(u32 value)
{
  value &= DrawMode::TEXTURE_WINDOW_MASK;
  if (m_draw_mode.texture_window_value == value)
    return;

  const u8 mask_x = Truncate8(value & UINT32_C(0x1F));
  const u8 mask_y = Truncate8((value >> 5) & UINT32_C(0x1F));
  const u8 offset_x = Truncate8((value >> 10) & UINT32_C(0x1F));
  const u8 offset_y = Truncate8((value >> 15) & UINT32_C(0x1F));
  DEBUG_LOG("Set texture window {:02X} {:02X} {:02X} {:02X}", mask_x, mask_y, offset_x, offset_y);

  m_draw_mode.texture_window.and_x = ~(mask_x * 8);
  m_draw_mode.texture_window.and_y = ~(mask_y * 8);
  m_draw_mode.texture_window.or_x = (offset_x & mask_x) * 8u;
  m_draw_mode.texture_window.or_y = (offset_y & mask_y) * 8u;
  m_draw_mode.texture_window_value = value;
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
    GPUBackend::SyncGPUThread(true);
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
  const u8 interlaced_field = m_crtc_state.interlaced_field;
  const bool line_skip = (interlaced && m_GPUSTAT.vertical_resolution);

  // NOTE: Must be split out, since this can push commands itself (e.g. media capture).
  GPUBackendFramePresentationParameters frame;
  submit_frame = (submit_frame && System::GetFramePresentationParameters(&frame));

  GPUBackendUpdateDisplayCommand* cmd = GPUBackend::NewUpdateDisplayCommand();
  cmd->gpu_busy_pct = g_settings.display_show_gpu_stats ? UpdateOrGetGPUBusyPct() : 0;
  if (!g_settings.gpu_show_vram) [[likely]]
  {
    cmd->display_width = m_crtc_state.display_width;
    cmd->display_height = m_crtc_state.display_height;
    cmd->display_origin_left = m_crtc_state.display_origin_left;
    cmd->display_origin_top = m_crtc_state.display_origin_top;
    cmd->display_vram_left = m_crtc_state.display_vram_left;
    cmd->display_vram_top = m_crtc_state.display_vram_top;
    cmd->display_vram_width = m_crtc_state.display_vram_width;
    cmd->display_vram_height = m_crtc_state.display_vram_height >> BoolToUInt8(interlaced);
    cmd->X = m_crtc_state.regs.X;
    cmd->interlaced_display_enabled = interlaced;
    cmd->interlaced_display_field = ConvertToBoolUnchecked(interlaced_field);
    cmd->interlaced_display_interleaved = line_skip;
    cmd->interleaved_480i_mode = m_GPUSTAT.InInterleaved480iMode();
    cmd->display_24bit = m_GPUSTAT.display_area_color_depth_24;
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
    GPUThread::PushCommandAndWakeThread(cmd);
    if (drain_one)
      GPUBackend::WaitForOneQueuedFrame();
  }
  else
  {
    GPUThread::PushCommand(cmd);
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
  GPUThread::PushCommandAndWakeThread(cmd);
  if (drain_one)
    GPUBackend::WaitForOneQueuedFrame();
}

u8 GPU::CalculateAutomaticResolutionScale() const
{
  // Auto scaling.
  // When the system is starting and all borders crop is enabled, the registers are zero, and
  // display_height therefore is also zero. Keep the existing resolution until it updates.
  u32 scale = 1;
  if (const WindowInfo& main_window_info = GPUThread::GetRenderWindowInfo();
      !main_window_info.IsSurfaceless() && m_crtc_state.display_width > 0 && m_crtc_state.display_height > 0 &&
      m_crtc_state.display_vram_width > 0 && m_crtc_state.display_vram_height > 0)
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
    scale = static_cast<u32>(
      std::ceil(std::max(static_cast<float>(draw_width) / static_cast<float>(m_crtc_state.display_vram_width),
                         static_cast<float>(draw_height) / static_cast<float>(m_crtc_state.display_vram_height))));
    scale = std::min<u32>(scale, std::numeric_limits<decltype(g_settings.gpu_resolution_scale)>::max());
    VERBOSE_LOG("Draw Size = {}x{}, VRAM Size = {}x{}, Preferred Scale = {}", draw_width, draw_height,
                m_crtc_state.display_vram_width, m_crtc_state.display_vram_height, scale);
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

void GPU::DrawDebugStateWindow(float scale)
{
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

bool GPU::StartRecordingGPUDump(const char* path, u32 num_frames /* = 1 */)
{
  if (m_gpu_dump)
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
  m_gpu_dump = GPUDump::Recorder::Create(path, System::GetGameSerial(), num_frames, &error);
  if (!m_gpu_dump)
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
  if (!m_gpu_dump)
    return;

  Error error;
  if (!m_gpu_dump->Close(&error))
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Error, "GPUDump", ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format("{}\n{}", TRANSLATE_SV("GPU", "Failed to close GPU trace:"), error.GetDescription()));
    m_gpu_dump.reset();
  }

  // Are we compressing the dump?
  const GPUDumpCompressionMode compress_mode =
    Settings::ParseGPUDumpCompressionMode(Core::GetTinyStringSettingValue("GPU", "DumpCompressionMode"))
      .value_or(Settings::DEFAULT_GPU_DUMP_COMPRESSION_MODE);
  std::string osd_key = fmt::format("GPUDump_{}", Path::GetFileName(m_gpu_dump->GetPath()));
  if (compress_mode == GPUDumpCompressionMode::Disabled)
  {
    Host::AddIconOSDMessage(
      OSDMessageType::Info, "GPUDump", ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format(TRANSLATE_FS("GPU", "Saved GPU trace to '{}'."), Path::GetFileName(m_gpu_dump->GetPath())));
    m_gpu_dump.reset();
    return;
  }

  std::string source_path = m_gpu_dump->GetPath();
  m_gpu_dump.reset();

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

void GPU::WriteCurrentVideoModeToDump(GPUDump::Recorder* dump) const
{
  dump->WriteGP1Command(GP1Command::SetDisplayDisable, BoolToUInt32(m_GPUSTAT.display_disable));
  dump->WriteGP1Command(GP1Command::SetDisplayStartAddress, m_crtc_state.regs.display_address_start);
  dump->WriteGP1Command(GP1Command::SetHorizontalDisplayRange, m_crtc_state.regs.horizontal_display_range);
  dump->WriteGP1Command(GP1Command::SetVerticalDisplayRange, m_crtc_state.regs.vertical_display_range);
  dump->WriteGP1Command(GP1Command::SetAllowTextureDisable, BoolToUInt32(m_set_texture_disable_mask));

  // display mode
  GP1SetDisplayMode dispmode = {};
  dispmode.horizontal_resolution_1 = m_GPUSTAT.horizontal_resolution_1.GetValue();
  dispmode.vertical_resolution = m_GPUSTAT.vertical_resolution.GetValue();
  dispmode.pal_mode = m_GPUSTAT.pal_mode.GetValue();
  dispmode.display_area_color_depth = m_GPUSTAT.display_area_color_depth_24.GetValue();
  dispmode.vertical_interlace = m_GPUSTAT.vertical_interlace.GetValue();
  dispmode.horizontal_resolution_2 = m_GPUSTAT.horizontal_resolution_2.GetValue();
  dispmode.reverse_flag = m_GPUSTAT.reverse_flag.GetValue();
  dump->WriteGP1Command(GP1Command::SetDisplayMode, dispmode.bits);

  // texture window/texture page
  dump->WriteGP0Packet((0xE1u << 24) | ZeroExtend32(m_draw_mode.mode_reg.bits));
  dump->WriteGP0Packet((0xE2u << 24) | m_draw_mode.texture_window_value);

  // drawing area
  dump->WriteGP0Packet((0xE3u << 24) | static_cast<u32>(m_drawing_area.left) |
                       (static_cast<u32>(m_drawing_area.top) << 10));
  dump->WriteGP0Packet((0xE4u << 24) | static_cast<u32>(m_drawing_area.right) |
                       (static_cast<u32>(m_drawing_area.bottom) << 10));

  // drawing offset
  dump->WriteGP0Packet((0xE5u << 24) | (static_cast<u32>(m_drawing_offset.x) & 0x7FFu) |
                       ((static_cast<u32>(m_drawing_offset.y) & 0x7FFu) << 11));

  // mask bit
  dump->WriteGP0Packet((0xE6u << 24) | BoolToUInt32(m_GPUSTAT.set_mask_while_drawing) |
                       (BoolToUInt32(m_GPUSTAT.check_mask_before_draw) << 1));
}

void GPU::ProcessGPUDumpPacket(GPUDump::PacketType type, const std::span<const u32> data)
{
  const auto execute_all_commands = [this]() {
    do
    {
      m_pending_command_ticks = 0;
      s_command_tick_event.Deactivate();
      ExecuteCommands();
    } while (m_pending_command_ticks > 0);
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
          const u32 block_size = std::min(m_fifo.GetSpace(), static_cast<u32>(data.size() - current_word));
          if (block_size == 0)
          {
            ERROR_LOG("FIFO overflow while processing dump packet of {} words", data.size());
            break;
          }

          for (u32 i = 0; i < block_size; i++)
            m_fifo.Push(ZeroExtend64(data[current_word++]));

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
      const TickCount crtc_ticks_per_frame =
        static_cast<TickCount>(m_crtc_state.horizontal_total) * static_cast<TickCount>(m_crtc_state.vertical_total);
      const TickCount system_ticks_per_frame =
        CRTCTicksToSystemTicks(crtc_ticks_per_frame, m_crtc_state.fractional_ticks);
      SystemTicksToCRTCTicks(system_ticks_per_frame, &m_crtc_state.fractional_ticks);
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
