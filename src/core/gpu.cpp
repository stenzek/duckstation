// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu.h"
#include "dma.h"
#include "gpu_dump.h"
#include "gpu_shadergen.h"
#include "gpu_sw_rasterizer.h"
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

std::unique_ptr<GPU> g_gpu;

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
  "GPU CRTC Tick", 1, 1, [](void* param, TickCount ticks, TickCount ticks_late) { g_gpu->CRTCTickEvent(ticks); },
  nullptr);
static TimingEvent s_command_tick_event(
  "GPU Command Tick", 1, 1, [](void* param, TickCount ticks, TickCount ticks_late) { g_gpu->CommandTickEvent(ticks); },
  nullptr);
static TimingEvent s_frame_done_event(
  "Frame Done", 1, 1, [](void* param, TickCount ticks, TickCount ticks_late) { g_gpu->FrameDoneEvent(ticks); },
  nullptr);

// #define PSX_GPU_STATS
#ifdef PSX_GPU_STATS
static u64 s_active_gpu_cycles = 0;
static u32 s_active_gpu_cycles_frames = 0;
#endif

static constexpr GPUTexture::Format DISPLAY_INTERNAL_POSTFX_FORMAT = GPUTexture::Format::RGBA8;

static bool CompressAndWriteTextureToFile(u32 width, u32 height, std::string path, FileSystem::ManagedCFilePtr fp,
                                          u8 quality, bool clear_alpha, bool flip_y, Image image, std::string osd_key);

GPU::GPU()
{
  GPU_SW_Rasterizer::SelectImplementation();
  ResetStatistics();
}

GPU::~GPU()
{
  s_command_tick_event.Deactivate();
  s_crtc_tick_event.Deactivate();
  s_frame_done_event.Deactivate();

  StopRecordingGPUDump();
  DestroyDeinterlaceTextures();
  g_gpu_device->RecycleTexture(std::move(m_chroma_smoothing_texture));
}

bool GPU::Initialize(Error* error)
{
  if (!System::IsReplayingGPUDump())
    s_crtc_tick_event.Activate();

  m_force_progressive_scan = (g_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive);
  m_force_frame_timings = g_settings.gpu_force_video_timing;
  m_fifo_size = g_settings.gpu_fifo_size;
  m_max_run_ahead = g_settings.gpu_max_run_ahead;
  m_console_is_pal = System::IsPALRegion();
  UpdateCRTCConfig();

  if (!CompileDisplayPipelines(true, true, g_settings.display_24bit_chroma_smoothing, error))
    return false;

#ifdef PSX_GPU_STATS
  s_active_gpu_cycles = 0;
  s_active_gpu_cycles_frames = 0;
#endif

  return true;
}

void GPU::UpdateSettings(const Settings& old_settings)
{
  FlushRender();

  m_force_progressive_scan = (g_settings.display_deinterlacing_mode == DisplayDeinterlacingMode::Progressive);
  m_fifo_size = g_settings.gpu_fifo_size;
  m_max_run_ahead = g_settings.gpu_max_run_ahead;

  if (m_force_frame_timings != g_settings.gpu_force_video_timing)
  {
    m_force_frame_timings = g_settings.gpu_force_video_timing;
    m_console_is_pal = System::IsPALRegion();
    UpdateCRTCConfig();
  }
  else if (g_settings.display_crop_mode != old_settings.display_crop_mode)
  {
    // Crop mode calls this, so recalculate the display area
    UpdateCRTCDisplayParameters();
  }

  if (g_settings.display_scaling != old_settings.display_scaling ||
      g_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode ||
      g_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing)
  {
    // Toss buffers on mode change.
    if (g_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode)
      DestroyDeinterlaceTextures();

    if (!CompileDisplayPipelines(
          g_settings.display_scaling != old_settings.display_scaling,
          g_settings.display_deinterlacing_mode != old_settings.display_deinterlacing_mode,
          g_settings.display_24bit_chroma_smoothing != old_settings.display_24bit_chroma_smoothing, nullptr))
    {
      Panic("Failed to compile display pipeline on settings change.");
    }
  }
}

void GPU::CPUClockChanged()
{
  UpdateCRTCConfig();
}

u32 GPU::GetResolutionScale() const
{
  return 1u;
}

void GPU::UpdateResolutionScale()
{
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
    height = static_cast<u32>(std::max<s32>(
      std::clamp<s32>(m_crtc_state.regs.Y2, ymin, ymax) - std::clamp<s32>(m_crtc_state.regs.Y1, ymin, ymax), 0));
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

  if (clear_vram)
  {
    std::memset(g_vram, 0, sizeof(g_vram));
    std::memset(g_gpu_clut, 0, sizeof(g_gpu_clut));
  }

  // Cancel VRAM writes.
  m_blitter_state = BlitterState::Idle;

  // Force event to reschedule itself.
  s_crtc_tick_event.Deactivate();
  s_command_tick_event.Deactivate();

  SoftReset();
  UpdateDisplay();
}

void GPU::SoftReset()
{
  FlushRender();
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

bool GPU::DoState(StateWrapper& sw, bool update_display)
{
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

  if (sw.GetVersion() < 64) [[unlikely]]
  {
    // Clear CLUT cache and let it populate later.
    InvalidateCLUT();
  }
  else
  {
    sw.Do(&m_current_clut_reg_bits);
    sw.Do(&m_current_clut_is_8bit);
    sw.DoArray(g_gpu_clut, std::size(g_gpu_clut));
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

  sw.Do(&m_max_run_ahead);
  sw.Do(&m_fifo_size);

  if (!sw.DoMarker("GPU-VRAM"))
    return false;

  sw.DoBytes(g_vram, VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));

  if (sw.IsReading())
  {
    m_draw_mode.texture_page_changed = true;
    m_drawing_area_changed = true;
    SetClampedDrawingArea();
    UpdateDMARequest();
    UpdateCRTCConfig();
    UpdateCommandTickEvent();

    // If we're paused, need to update the display FB.
    if (update_display)
      UpdateDisplay();
  }

  return !sw.HasError();
}

bool GPU::DoMemoryState(StateWrapper& sw, System::MemorySaveState& mss, bool update_display)
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
  sw.DoBytes(g_gpu_clut, sizeof(g_gpu_clut));

  sw.DoBytes(&m_vram_transfer, sizeof(m_vram_transfer));

  sw.Do(&m_fifo);
  sw.Do(&m_blit_buffer);
  sw.Do(&m_blit_remaining_words);
  sw.Do(&m_render_command.bits);

  sw.Do(&m_max_run_ahead);
  sw.Do(&m_fifo_size);

  if (sw.IsReading())
  {
    m_draw_mode.texture_page_changed = true;
    m_drawing_area_changed = true;
    SetClampedDrawingArea();
    UpdateDMARequest();
    UpdateCRTCConfig();
    UpdateCommandTickEvent();

    if (update_display)
      UpdateDisplay();
  }

  return true;
}

void GPU::RestoreDeviceContext()
{
}

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
      m_GPUSTAT.ready_to_recieve_dma = m_fifo.IsEmpty();
      break;

    case BlitterState::DrawingPolyLine:
      m_GPUSTAT.ready_to_send_vram = false;
      m_GPUSTAT.ready_to_recieve_dma = (m_fifo.GetSize() < m_fifo_size);
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
      dma_request = m_GPUSTAT.ready_to_recieve_dma;
      break;

    case GPUDMADirection::CPUtoGP0:
      dma_request = m_GPUSTAT.ready_to_recieve_dma;
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
#ifdef PSX_GPU_STATS
  s_active_gpu_cycles += ticks;
#endif
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

float GPU::ComputeDisplayAspectRatio() const
{
  if (g_settings.debugging.show_vram)
    return static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT);

  // Display off => Doesn't matter.
  if (m_crtc_state.display_width == 0 || m_crtc_state.display_height == 0)
    return 4.0f / 3.0f;

  // PAR 1:1 is not corrected.
  if (g_settings.display_aspect_ratio == DisplayAspectRatio::PAR1_1)
    return static_cast<float>(m_crtc_state.display_width) / static_cast<float>(m_crtc_state.display_height);

  float ar = 4.0f / 3.0f;
  if (!g_settings.display_force_4_3_for_24bit || !m_GPUSTAT.display_area_color_depth_24)
  {
    if (g_settings.display_aspect_ratio == DisplayAspectRatio::MatchWindow && g_gpu_device->HasMainSwapChain())
    {
      ar = static_cast<float>(g_gpu_device->GetMainSwapChain()->GetWidth()) /
           static_cast<float>(g_gpu_device->GetMainSwapChain()->GetHeight());
    }
    else if (g_settings.display_aspect_ratio == DisplayAspectRatio::Custom)
    {
      ar = static_cast<float>(g_settings.display_aspect_ratio_custom_numerator) /
           static_cast<float>(g_settings.display_aspect_ratio_custom_denominator);
    }
    else
    {
      ar = g_settings.GetDisplayAspectRatioValue();
    }
  }

  return ar;
}

float GPU::ComputeSourceAspectRatio() const
{
  const float source_aspect_ratio =
    (g_settings.debugging.show_vram ?
       (static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT)) :
       static_cast<float>(m_crtc_state.display_width) / static_cast<float>(m_crtc_state.display_height));

  // Correction is applied to the GTE for stretch to fit, that way it fills the window.
  const float source_aspect_ratio_correction =
    (g_settings.display_aspect_ratio == DisplayAspectRatio::MatchWindow) ? 1.0f : ComputeAspectRatioCorrection();

  return source_aspect_ratio / source_aspect_ratio_correction;
}

float GPU::ComputeAspectRatioCorrection() const
{
  const CRTCState& cs = m_crtc_state;
  float relative_width = static_cast<float>(cs.horizontal_visible_end - cs.horizontal_visible_start);
  float relative_height = static_cast<float>(cs.vertical_visible_end - cs.vertical_visible_start);
  if (relative_width <= 0 || relative_height <= 0 || g_settings.debugging.show_vram ||
      g_settings.display_aspect_ratio == DisplayAspectRatio::PAR1_1 ||
      g_settings.display_crop_mode == DisplayCropMode::OverscanUncorrected ||
      g_settings.display_crop_mode == DisplayCropMode::BordersUncorrected)
  {
    return 1.0f;
  }

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

  return (relative_width / relative_height);
}

void GPU::ApplyPixelAspectRatioToSize(float* width, float* height) const
{
  const float dar = ComputeDisplayAspectRatio();
  const float sar = ComputeSourceAspectRatio();
  const float par = dar / sar;

  if (par < 1.0f)
  {
    // stretch height, preserve width
    *height = std::ceil(*height / par);
  }
  else
  {
    // stretch width, preserve height
    *width = std::ceil(*width * par);
  }
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

  if (m_GPUSTAT.pal_mode && m_force_frame_timings == ForceVideoTimingMode::NTSC)
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
  else if (!m_GPUSTAT.pal_mode && m_force_frame_timings == ForceVideoTimingMode::PAL)
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
        cs.horizontal_visible_start = static_cast<u16>(std::max<int>(0, 628 + g_settings.display_active_start_offset));
        cs.horizontal_visible_end =
          static_cast<u16>(std::max<int>(cs.horizontal_visible_start, 3188 + g_settings.display_active_end_offset));
        cs.vertical_visible_start = static_cast<u16>(std::max<int>(0, 30 + g_settings.display_line_start_offset));
        cs.vertical_visible_end =
          static_cast<u16>(std::max<int>(cs.vertical_visible_start, 298 + g_settings.display_line_end_offset));
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
        cs.horizontal_visible_start = static_cast<u16>(std::max<int>(0, 608 + g_settings.display_active_start_offset));
        cs.horizontal_visible_end =
          static_cast<u16>(std::max<int>(cs.horizontal_visible_start, 3168 + g_settings.display_active_end_offset));
        cs.vertical_visible_start = static_cast<u16>(std::max<int>(0, 24 + g_settings.display_line_start_offset));
        cs.vertical_visible_end =
          static_cast<u16>(std::max<int>(cs.vertical_visible_start, 248 + g_settings.display_line_end_offset));
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

  if (cs.display_vram_width != old_vram_width || cs.display_vram_height != old_vram_height)
    UpdateResolutionScale();
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
      std::min(lines_to_draw, m_crtc_state.vertical_total - m_crtc_state.current_scanline);
    const u32 prev_scanline = m_crtc_state.current_scanline;
    m_crtc_state.current_scanline += lines_to_draw_this_loop;
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
        FlushRender();
        UpdateDisplay();
        System::IncrementFrameNumber();
        frame_done = true;

        // switch fields early. this is needed so we draw to the correct one.
        if (m_GPUSTAT.InInterleaved480iMode())
          m_crtc_state.interlaced_display_field = m_crtc_state.interlaced_field ^ 1u;
        else
          m_crtc_state.interlaced_display_field = 0;

#ifdef PSX_GPU_STATS
        if ((++s_active_gpu_cycles_frames) == 60)
        {
          const double busy_frac =
            static_cast<double>(s_active_gpu_cycles) /
            static_cast<double>(SystemTicksToGPUTicks(System::ScaleTicksToOverclock(System::MASTER_CLOCK)) *
                                (ComputeVerticalFrequency() / 60.0f));
          DEV_LOG("PSX GPU Usage: {:.2f}% [{:.0f} cycles avg per frame]", busy_frac * 100,
                  static_cast<double>(s_active_gpu_cycles) / static_cast<double>(s_active_gpu_cycles_frames));
          s_active_gpu_cycles = 0;
          s_active_gpu_cycles_frames = 0;
        }
#endif
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
        m_GPUSTAT.interlaced_field = !m_crtc_state.interlaced_field;
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

void GPU::ConvertScreenCoordinatesToDisplayCoordinates(float window_x, float window_y, float* display_x,
                                                       float* display_y) const
{
  if (!g_gpu_device->HasMainSwapChain()) [[unlikely]]
  {
    *display_x = 0.0f;
    *display_y = 0.0f;
    return;
  }

  GSVector4i display_rc, draw_rc;
  CalculateDrawRect(g_gpu_device->GetMainSwapChain()->GetWidth(), g_gpu_device->GetMainSwapChain()->GetHeight(), true,
                    true, &display_rc, &draw_rc);

  // convert coordinates to active display region, then to full display region
  const float scaled_display_x =
    (window_x - static_cast<float>(display_rc.left)) / static_cast<float>(display_rc.width());
  const float scaled_display_y =
    (window_y - static_cast<float>(display_rc.top)) / static_cast<float>(display_rc.height());

  // scale back to internal resolution
  *display_x = scaled_display_x * static_cast<float>(m_crtc_state.display_width);
  *display_y = scaled_display_y * static_cast<float>(m_crtc_state.display_height);

  // TODO: apply rotation matrix

  DEV_LOG("win {:.0f},{:.0f} -> local {:.0f},{:.0f}, disp {:.2f},{:.2f} (size {},{} frac {},{})", window_x, window_y,
          window_x - draw_rc.left, window_y - draw_rc.top, *display_x, *display_y, m_crtc_state.display_width,
          m_crtc_state.display_height, *display_x / static_cast<float>(m_crtc_state.display_width),
          *display_y / static_cast<float>(m_crtc_state.display_height));
}

bool GPU::ConvertDisplayCoordinatesToBeamTicksAndLines(float display_x, float display_y, float x_scale, u32* out_tick,
                                                       u32* out_line) const
{
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

  *out_line = (static_cast<u32>(std::round(display_y)) >> BoolToUInt8(IsInterlacedDisplayEnabled())) +
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

      if (!m_GPUSTAT.display_disable && disable && IsInterlacedDisplayEnabled())
        ClearDisplay();

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
        OnBufferSwapped();
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
    UpdateCLUT(clut, needs_8bit);
    m_current_clut_reg_bits = clut.bits;
    m_current_clut_is_8bit = needs_8bit;
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

void GPU::ClearDisplay()
{
  ClearDisplayTexture();

  // Just recycle the textures, it'll get re-fetched.
  DestroyDeinterlaceTextures();
}

void GPU::SetClampedDrawingArea()
{
  if (m_drawing_area.left > m_drawing_area.right || m_drawing_area.top > m_drawing_area.bottom) [[unlikely]]
  {
    m_clamped_drawing_area = GSVector4i::zero();
    return;
  }

  const u32 right = std::min(m_drawing_area.right + 1, static_cast<u32>(VRAM_WIDTH));
  const u32 left = std::min(m_drawing_area.left, std::min(m_drawing_area.right, VRAM_WIDTH - 1));
  const u32 bottom = std::min(m_drawing_area.bottom + 1, static_cast<u32>(VRAM_HEIGHT));
  const u32 top = std::min(m_drawing_area.top, std::min(m_drawing_area.bottom, VRAM_HEIGHT - 1));
  m_clamped_drawing_area = GSVector4i(left, top, right, bottom);
}

void GPU::SetDrawMode(u16 value)
{
  GPUDrawModeReg new_mode_reg{static_cast<u16>(value & GPUDrawModeReg::MASK)};
  if (!m_set_texture_disable_mask)
    new_mode_reg.texture_disable = false;

  if (new_mode_reg.bits == m_draw_mode.mode_reg.bits)
    return;

  m_draw_mode.texture_page_changed |= ((new_mode_reg.bits & GPUDrawModeReg::TEXTURE_MODE_AND_PAGE_MASK) !=
                                       (m_draw_mode.mode_reg.bits & GPUDrawModeReg::TEXTURE_MODE_AND_PAGE_MASK));
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
  if (m_draw_mode.palette_reg.bits == value)
    return;

  m_draw_mode.palette_reg.bits = value;
  m_draw_mode.texture_page_changed = true;
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

void GPU::ReadCLUT(u16* dest, GPUTexturePaletteReg reg, bool clut_is_8bit)
{
  const u16* src_row = &g_vram[reg.GetYBase() * VRAM_WIDTH];
  const u32 start_x = reg.GetXBase();
  if (!clut_is_8bit)
  {
    // Wraparound can't happen in 4-bit mode.
    std::memcpy(dest, &src_row[start_x], sizeof(u16) * 16);
  }
  else
  {
    if ((start_x + 256) > VRAM_WIDTH) [[unlikely]]
    {
      const u32 end = VRAM_WIDTH - start_x;
      const u32 start = 256 - end;
      std::memcpy(dest, &src_row[start_x], sizeof(u16) * end);
      std::memcpy(dest + end, src_row, sizeof(u16) * start);
    }
    else
    {
      std::memcpy(dest, &src_row[start_x], sizeof(u16) * 256);
    }
  }
}

bool GPU::CompileDisplayPipelines(bool display, bool deinterlace, bool chroma_smoothing, Error* error)
{
  GPUShaderGen shadergen(g_gpu_device->GetRenderAPI(), g_gpu_device->GetFeatures().dual_source_blend,
                         g_gpu_device->GetFeatures().framebuffer_fetch);

  GPUPipeline::GraphicsConfig plconfig;
  plconfig.input_layout.vertex_stride = 0;
  plconfig.primitive = GPUPipeline::Primitive::Triangles;
  plconfig.rasterization = GPUPipeline::RasterizationState::GetNoCullState();
  plconfig.depth = GPUPipeline::DepthState::GetNoTestsState();
  plconfig.blend = GPUPipeline::BlendState::GetNoBlendingState();
  plconfig.geometry_shader = nullptr;
  plconfig.depth_format = GPUTexture::Format::Unknown;
  plconfig.samples = 1;
  plconfig.per_sample_shading = false;
  plconfig.render_pass_flags = GPUPipeline::NoRenderPassFlags;

  if (display)
  {
    plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.SetTargetFormats(g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetFormat() :
                                                                 GPUTexture::Format::RGBA8);

    std::string vs = shadergen.GenerateDisplayVertexShader();
    std::string fs;
    switch (g_settings.display_scaling)
    {
      case DisplayScalingMode::BilinearSharp:
        fs = shadergen.GenerateDisplaySharpBilinearFragmentShader();
        break;

      case DisplayScalingMode::BilinearSmooth:
      case DisplayScalingMode::BilinearInteger:
        fs = shadergen.GenerateDisplayFragmentShader(true, false);
        break;

      case DisplayScalingMode::Nearest:
      case DisplayScalingMode::NearestInteger:
      default:
        fs = shadergen.GenerateDisplayFragmentShader(false, true);
        break;
    }

    std::unique_ptr<GPUShader> vso =
      g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(), vs, error);
    std::unique_ptr<GPUShader> fso =
      g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(), fs, error);
    if (!vso || !fso)
      return false;
    GL_OBJECT_NAME(vso, "Display Vertex Shader");
    GL_OBJECT_NAME_FMT(fso, "Display Fragment Shader [{}]",
                       Settings::GetDisplayScalingName(g_settings.display_scaling));
    plconfig.vertex_shader = vso.get();
    plconfig.fragment_shader = fso.get();
    if (!(m_display_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;
    GL_OBJECT_NAME_FMT(m_display_pipeline, "Display Pipeline [{}]",
                       Settings::GetDisplayScalingName(g_settings.display_scaling));
  }

  if (deinterlace)
  {
    plconfig.SetTargetFormats(GPUTexture::Format::RGBA8);

    std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                shadergen.GenerateScreenQuadVertexShader(), error);
    if (!vso)
      return false;
    GL_OBJECT_NAME(vso, "Deinterlace Vertex Shader");

    std::unique_ptr<GPUShader> fso;
    if (!(fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                           shadergen.GenerateInterleavedFieldExtractFragmentShader(), error)))
    {
      return false;
    }

    GL_OBJECT_NAME(fso, "Deinterlace Field Extract Fragment Shader");

    plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
    plconfig.vertex_shader = vso.get();
    plconfig.fragment_shader = fso.get();
    if (!(m_deinterlace_extract_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
      return false;

    GL_OBJECT_NAME(m_deinterlace_extract_pipeline, "Deinterlace Field Extract Pipeline");

    switch (g_settings.display_deinterlacing_mode)
    {
      case DisplayDeinterlacingMode::Disabled:
      case DisplayDeinterlacingMode::Progressive:
        break;

      case DisplayDeinterlacingMode::Weave:
      {
        if (!(fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                               shadergen.GenerateDeinterlaceWeaveFragmentShader(), error)))
        {
          return false;
        }

        GL_OBJECT_NAME(fso, "Weave Deinterlace Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
        plconfig.vertex_shader = vso.get();
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "Weave Deinterlace Pipeline");
      }
      break;

      case DisplayDeinterlacingMode::Blend:
      {
        if (!(fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                               shadergen.GenerateDeinterlaceBlendFragmentShader(), error)))
        {
          return false;
        }

        GL_OBJECT_NAME(fso, "Blend Deinterlace Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
        plconfig.vertex_shader = vso.get();
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "Blend Deinterlace Pipeline");
      }
      break;

      case DisplayDeinterlacingMode::Adaptive:
      {
        fso = g_gpu_device->CreateShader(GPUShaderStage::Fragment, shadergen.GetLanguage(),
                                         shadergen.GenerateFastMADReconstructFragmentShader(), error);
        if (!fso)
          return false;

        GL_OBJECT_NAME(fso, "FastMAD Reconstruct Fragment Shader");

        plconfig.layout = GPUPipeline::Layout::MultiTextureAndPushConstants;
        plconfig.fragment_shader = fso.get();
        if (!(m_deinterlace_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
          return false;

        GL_OBJECT_NAME(m_deinterlace_pipeline, "FastMAD Reconstruct Pipeline");
      }
      break;

      default:
        UnreachableCode();
    }
  }

  if (chroma_smoothing)
  {
    m_chroma_smoothing_pipeline.reset();
    g_gpu_device->RecycleTexture(std::move(m_chroma_smoothing_texture));

    if (g_settings.display_24bit_chroma_smoothing)
    {
      plconfig.layout = GPUPipeline::Layout::SingleTextureAndPushConstants;
      plconfig.SetTargetFormats(GPUTexture::Format::RGBA8);

      std::unique_ptr<GPUShader> vso = g_gpu_device->CreateShader(GPUShaderStage::Vertex, shadergen.GetLanguage(),
                                                                  shadergen.GenerateScreenQuadVertexShader(), error);
      std::unique_ptr<GPUShader> fso = g_gpu_device->CreateShader(
        GPUShaderStage::Fragment, shadergen.GetLanguage(), shadergen.GenerateChromaSmoothingFragmentShader(), error);
      if (!vso || !fso)
        return false;
      GL_OBJECT_NAME(vso, "Chroma Smoothing Vertex Shader");
      GL_OBJECT_NAME(fso, "Chroma Smoothing Fragment Shader");

      plconfig.vertex_shader = vso.get();
      plconfig.fragment_shader = fso.get();
      if (!(m_chroma_smoothing_pipeline = g_gpu_device->CreatePipeline(plconfig, error)))
        return false;
      GL_OBJECT_NAME(m_chroma_smoothing_pipeline, "Chroma Smoothing Pipeline");
    }
  }

  return true;
}

void GPU::ClearDisplayTexture()
{
  m_display_texture = nullptr;
  m_display_texture_view_x = 0;
  m_display_texture_view_y = 0;
  m_display_texture_view_width = 0;
  m_display_texture_view_height = 0;
}

void GPU::SetDisplayTexture(GPUTexture* texture, GPUTexture* depth_buffer, s32 view_x, s32 view_y, s32 view_width,
                            s32 view_height)
{
  DebugAssert(texture);

  if (g_settings.display_auto_resize_window &&
      (view_width != m_display_texture_view_width || view_height != m_display_texture_view_height))
  {
    System::RequestDisplaySize();
  }

  m_display_texture = texture;
  m_display_depth_buffer = depth_buffer;
  m_display_texture_view_x = view_x;
  m_display_texture_view_y = view_y;
  m_display_texture_view_width = view_width;
  m_display_texture_view_height = view_height;
}

GPUDevice::PresentResult GPU::PresentDisplay()
{
  FlushRender();

  if (!g_gpu_device->HasMainSwapChain())
    return GPUDevice::PresentResult::SkipPresent;

  GSVector4i display_rect;
  GSVector4i draw_rect;
  CalculateDrawRect(g_gpu_device->GetMainSwapChain()->GetWidth(), g_gpu_device->GetMainSwapChain()->GetHeight(),
                    !g_settings.debugging.show_vram, true, &display_rect, &draw_rect);
  return RenderDisplay(nullptr, display_rect, draw_rect, !g_settings.debugging.show_vram);
}

GPUDevice::PresentResult GPU::RenderDisplay(GPUTexture* target, const GSVector4i display_rect,
                                            const GSVector4i draw_rect, bool postfx)
{
  GL_SCOPE_FMT("RenderDisplay: {}", draw_rect);

  if (m_display_texture)
    m_display_texture->MakeReadyForSampling();

  // Internal post-processing.
  GPUTexture* display_texture = m_display_texture;
  s32 display_texture_view_x = m_display_texture_view_x;
  s32 display_texture_view_y = m_display_texture_view_y;
  s32 display_texture_view_width = m_display_texture_view_width;
  s32 display_texture_view_height = m_display_texture_view_height;
  if (postfx && display_texture && PostProcessing::InternalChain.IsActive() &&
      PostProcessing::InternalChain.CheckTargets(DISPLAY_INTERNAL_POSTFX_FORMAT, display_texture_view_width,
                                                 display_texture_view_height))
  {
    DebugAssert(display_texture_view_x == 0 && display_texture_view_y == 0 &&
                static_cast<s32>(display_texture->GetWidth()) == display_texture_view_width &&
                static_cast<s32>(display_texture->GetHeight()) == display_texture_view_height);

    // Now we can apply the post chain.
    GPUTexture* post_output_texture = PostProcessing::InternalChain.GetOutputTexture();
    if (PostProcessing::InternalChain.Apply(display_texture, m_display_depth_buffer, post_output_texture,
                                            GSVector4i(0, 0, display_texture_view_width, display_texture_view_height),
                                            display_texture_view_width, display_texture_view_height,
                                            m_crtc_state.display_width,
                                            m_crtc_state.display_height) == GPUDevice::PresentResult::OK)
    {
      display_texture_view_x = 0;
      display_texture_view_y = 0;
      display_texture = post_output_texture;
      display_texture->MakeReadyForSampling();
    }
  }

  const GPUTexture::Format hdformat = target ? target->GetFormat() : g_gpu_device->GetMainSwapChain()->GetFormat();
  const u32 target_width = target ? target->GetWidth() : g_gpu_device->GetMainSwapChain()->GetWidth();
  const u32 target_height = target ? target->GetHeight() : g_gpu_device->GetMainSwapChain()->GetHeight();
  const bool really_postfx = (postfx && PostProcessing::DisplayChain.IsActive() && g_gpu_device->HasMainSwapChain() &&
                              hdformat != GPUTexture::Format::Unknown && target_width > 0 && target_height > 0 &&
                              PostProcessing::DisplayChain.CheckTargets(hdformat, target_width, target_height));
  GSVector4i real_draw_rect = target ? draw_rect : g_gpu_device->GetMainSwapChain()->PreRotateClipRect(draw_rect);
  if (g_gpu_device->UsesLowerLeftOrigin())
  {
    real_draw_rect = GPUDevice::FlipToLowerLeft(
      real_draw_rect,
      (target || really_postfx) ? target_height : g_gpu_device->GetMainSwapChain()->GetPostRotatedHeight());
  }
  if (really_postfx)
  {
    g_gpu_device->ClearRenderTarget(PostProcessing::DisplayChain.GetInputTexture(), GPUDevice::DEFAULT_CLEAR_COLOR);
    g_gpu_device->SetRenderTarget(PostProcessing::DisplayChain.GetInputTexture());
  }
  else
  {
    if (target)
    {
      g_gpu_device->SetRenderTarget(target);
    }
    else
    {
      const GPUDevice::PresentResult pres = g_gpu_device->BeginPresent(g_gpu_device->GetMainSwapChain());
      if (pres != GPUDevice::PresentResult::OK)
        return pres;
    }
  }

  if (display_texture)
  {
    bool texture_filter_linear = false;

    struct Uniforms
    {
      float src_rect[4];
      float src_size[4];
      float clamp_rect[4];
      float params[4];
      float rotation_matrix[2][2];
    } uniforms;
    std::memset(uniforms.params, 0, sizeof(uniforms.params));

    switch (g_settings.display_scaling)
    {
      case DisplayScalingMode::Nearest:
      case DisplayScalingMode::NearestInteger:
        break;

      case DisplayScalingMode::BilinearSmooth:
      case DisplayScalingMode::BilinearInteger:
        texture_filter_linear = true;
        break;

      case DisplayScalingMode::BilinearSharp:
      {
        texture_filter_linear = true;
        uniforms.params[0] = std::max(
          std::floor(static_cast<float>(draw_rect.width()) / static_cast<float>(m_display_texture_view_width)), 1.0f);
        uniforms.params[1] = std::max(
          std::floor(static_cast<float>(draw_rect.height()) / static_cast<float>(m_display_texture_view_height)), 1.0f);
        uniforms.params[2] = 0.5f - 0.5f / uniforms.params[0];
        uniforms.params[3] = 0.5f - 0.5f / uniforms.params[1];
      }
      break;

      default:
        UnreachableCode();
        break;
    }

    g_gpu_device->SetPipeline(m_display_pipeline.get());
    g_gpu_device->SetTextureSampler(
      0, display_texture, texture_filter_linear ? g_gpu_device->GetLinearSampler() : g_gpu_device->GetNearestSampler());

    // For bilinear, clamp to 0.5/SIZE-0.5 to avoid bleeding from the adjacent texels in VRAM. This is because
    // 1.0 in UV space is not the bottom-right texel, but a mix of the bottom-right and wrapped/next texel.
    const float rcp_width = 1.0f / static_cast<float>(display_texture->GetWidth());
    const float rcp_height = 1.0f / static_cast<float>(display_texture->GetHeight());
    uniforms.src_rect[0] = static_cast<float>(display_texture_view_x) * rcp_width;
    uniforms.src_rect[1] = static_cast<float>(display_texture_view_y) * rcp_height;
    uniforms.src_rect[2] = static_cast<float>(display_texture_view_width) * rcp_width;
    uniforms.src_rect[3] = static_cast<float>(display_texture_view_height) * rcp_height;
    uniforms.clamp_rect[0] = (static_cast<float>(display_texture_view_x) + 0.5f) * rcp_width;
    uniforms.clamp_rect[1] = (static_cast<float>(display_texture_view_y) + 0.5f) * rcp_height;
    uniforms.clamp_rect[2] =
      (static_cast<float>(display_texture_view_x + display_texture_view_width) - 0.5f) * rcp_width;
    uniforms.clamp_rect[3] =
      (static_cast<float>(display_texture_view_y + display_texture_view_height) - 0.5f) * rcp_height;
    uniforms.src_size[0] = static_cast<float>(display_texture->GetWidth());
    uniforms.src_size[1] = static_cast<float>(display_texture->GetHeight());
    uniforms.src_size[2] = rcp_width;
    uniforms.src_size[3] = rcp_height;

    const WindowInfo::PreRotation surface_prerotation = (target || really_postfx) ?
                                                          WindowInfo::PreRotation::Identity :
                                                          g_gpu_device->GetMainSwapChain()->GetPreRotation();
    if (g_settings.display_rotation != DisplayRotation::Normal ||
        surface_prerotation != WindowInfo::PreRotation::Identity)
    {
      static constexpr const std::array<float, static_cast<size_t>(DisplayRotation::Count)> rotation_radians = {{
        0.0f,                                        // Disabled
        static_cast<float>(std::numbers::pi * 1.5f), // Rotate90
        static_cast<float>(std::numbers::pi),        // Rotate180
        static_cast<float>(std::numbers::pi / 2.0),  // Rotate270
      }};

      const u32 rotation_idx = (static_cast<u32>(g_settings.display_rotation) + static_cast<u32>(surface_prerotation)) %
                               static_cast<u32>(rotation_radians.size());
      GSMatrix2x2::Rotation(rotation_radians[rotation_idx]).store(uniforms.rotation_matrix);
    }
    else
    {
      GSMatrix2x2::Identity().store(uniforms.rotation_matrix);
    }

    g_gpu_device->PushUniformBuffer(&uniforms, sizeof(uniforms));

    g_gpu_device->SetViewportAndScissor(real_draw_rect);
    g_gpu_device->Draw(3, 0);
  }

  if (really_postfx)
  {
    DebugAssert(!g_settings.debugging.show_vram);

    // "original size" in postfx includes padding.
    const float upscale_x = m_display_texture ? static_cast<float>(m_display_texture_view_width) /
                                                  static_cast<float>(m_crtc_state.display_vram_width) :
                                                1.0f;
    const float upscale_y = m_display_texture ? static_cast<float>(m_display_texture_view_height) /
                                                  static_cast<float>(m_crtc_state.display_vram_height) :
                                                1.0f;
    const s32 orig_width = static_cast<s32>(std::ceil(static_cast<float>(m_crtc_state.display_width) * upscale_x));
    const s32 orig_height = static_cast<s32>(std::ceil(static_cast<float>(m_crtc_state.display_height) * upscale_y));

    return PostProcessing::DisplayChain.Apply(PostProcessing::DisplayChain.GetInputTexture(), nullptr, target,
                                              display_rect, orig_width, orig_height, m_crtc_state.display_width,
                                              m_crtc_state.display_height);
  }
  else
  {
    return GPUDevice::PresentResult::OK;
  }
}

bool GPU::SendDisplayToMediaCapture(MediaCapture* cap)
{
  GPUTexture* target = cap->GetRenderTexture();
  if (!target) [[unlikely]]
    return false;

  const bool apply_aspect_ratio =
    (g_settings.display_screenshot_mode != DisplayScreenshotMode::UncorrectedInternalResolution);
  const bool postfx = (g_settings.display_screenshot_mode != DisplayScreenshotMode::InternalResolution);
  GSVector4i display_rect, draw_rect;
  CalculateDrawRect(target->GetWidth(), target->GetHeight(), !g_settings.debugging.show_vram, apply_aspect_ratio,
                    &display_rect, &draw_rect);

  // Not cleared by RenderDisplay().
  g_gpu_device->ClearRenderTarget(target, GPUDevice::DEFAULT_CLEAR_COLOR);

  if (RenderDisplay(target, display_rect, draw_rect, postfx) != GPUDevice::PresentResult::OK) [[unlikely]]
    return false;

  return cap->DeliverVideoFrame(target);
}

void GPU::DestroyDeinterlaceTextures()
{
  for (std::unique_ptr<GPUTexture>& tex : m_deinterlace_buffers)
    g_gpu_device->RecycleTexture(std::move(tex));
  g_gpu_device->RecycleTexture(std::move(m_deinterlace_texture));
  m_current_deinterlace_buffer = 0;
}

bool GPU::Deinterlace(u32 field, u32 line_skip)
{
  GPUTexture* src = m_display_texture;
  const u32 x = m_display_texture_view_x;
  const u32 y = m_display_texture_view_y;
  const u32 width = m_display_texture_view_width;
  const u32 height = m_display_texture_view_height;

  switch (g_settings.display_deinterlacing_mode)
  {
    case DisplayDeinterlacingMode::Disabled:
    {
      if (line_skip == 0)
        return true;

      // Still have to extract the field.
      if (!DeinterlaceExtractField(0, src, x, y, width, height, line_skip)) [[unlikely]]
        return false;

      SetDisplayTexture(m_deinterlace_buffers[0].get(), m_display_depth_buffer, 0, 0, width, height);
      return true;
    }

    case DisplayDeinterlacingMode::Weave:
    {
      GL_SCOPE_FMT("DeinterlaceWeave({{{},{}}}, {}x{}, field={}, line_skip={})", x, y, width, height, field, line_skip);

      const u32 full_height = height * 2;
      if (!DeinterlaceSetTargetSize(width, full_height, true)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      src->MakeReadyForSampling();

      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, src, g_gpu_device->GetNearestSampler());
      const u32 uniforms[] = {x, y, field, line_skip};
      g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
      g_gpu_device->SetViewportAndScissor(0, 0, width, full_height);
      g_gpu_device->Draw(3, 0);

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), m_display_depth_buffer, 0, 0, width, full_height);
      return true;
    }

    case DisplayDeinterlacingMode::Blend:
    {
      constexpr u32 NUM_BLEND_BUFFERS = 2;

      GL_SCOPE_FMT("DeinterlaceBlend({{{},{}}}, {}x{}, field={}, line_skip={})", x, y, width, height, field, line_skip);

      const u32 this_buffer = m_current_deinterlace_buffer;
      m_current_deinterlace_buffer = (m_current_deinterlace_buffer + 1u) % NUM_BLEND_BUFFERS;
      GL_INS_FMT("Current buffer: {}", this_buffer);
      if (!DeinterlaceExtractField(this_buffer, src, x, y, width, height, line_skip) ||
          !DeinterlaceSetTargetSize(width, height, false)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      // TODO: could be implemented with alpha blending instead..

      g_gpu_device->InvalidateRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_deinterlace_buffers[this_buffer].get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, m_deinterlace_buffers[(this_buffer - 1) % NUM_BLEND_BUFFERS].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetViewportAndScissor(0, 0, width, height);
      g_gpu_device->Draw(3, 0);

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), m_display_depth_buffer, 0, 0, width, height);
      return true;
    }

    case DisplayDeinterlacingMode::Adaptive:
    {
      GL_SCOPE_FMT("DeinterlaceAdaptive({{{},{}}}, {}x{}, field={}, line_skip={})", x, y, width, height, field,
                   line_skip);

      const u32 full_height = height * 2;
      const u32 this_buffer = m_current_deinterlace_buffer;
      m_current_deinterlace_buffer = (m_current_deinterlace_buffer + 1u) % DEINTERLACE_BUFFER_COUNT;
      GL_INS_FMT("Current buffer: {}", this_buffer);
      if (!DeinterlaceExtractField(this_buffer, src, x, y, width, height, line_skip) ||
          !DeinterlaceSetTargetSize(width, full_height, false)) [[unlikely]]
      {
        ClearDisplayTexture();
        return false;
      }

      g_gpu_device->SetRenderTarget(m_deinterlace_texture.get());
      g_gpu_device->SetPipeline(m_deinterlace_pipeline.get());
      g_gpu_device->SetTextureSampler(0, m_deinterlace_buffers[this_buffer].get(), g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(1, m_deinterlace_buffers[(this_buffer - 1) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(2, m_deinterlace_buffers[(this_buffer - 2) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      g_gpu_device->SetTextureSampler(3, m_deinterlace_buffers[(this_buffer - 3) % DEINTERLACE_BUFFER_COUNT].get(),
                                      g_gpu_device->GetNearestSampler());
      const u32 uniforms[] = {field, full_height};
      g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
      g_gpu_device->SetViewportAndScissor(0, 0, width, full_height);
      g_gpu_device->Draw(3, 0);

      m_deinterlace_texture->MakeReadyForSampling();
      SetDisplayTexture(m_deinterlace_texture.get(), m_display_depth_buffer, 0, 0, width, full_height);
      return true;
    }

    default:
      UnreachableCode();
  }
}

bool GPU::DeinterlaceExtractField(u32 dst_bufidx, GPUTexture* src, u32 x, u32 y, u32 width, u32 height, u32 line_skip)
{
  if (!m_deinterlace_buffers[dst_bufidx] || m_deinterlace_buffers[dst_bufidx]->GetWidth() != width ||
      m_deinterlace_buffers[dst_bufidx]->GetHeight() != height)
  {
    if (!g_gpu_device->ResizeTexture(&m_deinterlace_buffers[dst_bufidx], width, height, GPUTexture::Type::RenderTarget,
                                     GPUTexture::Format::RGBA8, GPUTexture::Flags::None, false)) [[unlikely]]
    {
      return false;
    }

    GL_OBJECT_NAME_FMT(m_deinterlace_buffers[dst_bufidx], "Blend Deinterlace Buffer {}", dst_bufidx);
  }

  GPUTexture* dst = m_deinterlace_buffers[dst_bufidx].get();
  g_gpu_device->InvalidateRenderTarget(dst);

  // If we're not skipping lines, then we can simply copy the texture.
  if (line_skip == 0 && src->GetFormat() == dst->GetFormat())
  {
    GL_INS_FMT("DeinterlaceExtractField({{{},{}}} {}x{} line_skip={}) => copy direct", x, y, width, height, line_skip);
    g_gpu_device->CopyTextureRegion(dst, 0, 0, 0, 0, src, x, y, 0, 0, width, height);
  }
  else
  {
    GL_SCOPE_FMT("DeinterlaceExtractField({{{},{}}} {}x{} line_skip={}) => shader copy", x, y, width, height,
                 line_skip);

    // Otherwise, we need to extract every other line from the texture.
    src->MakeReadyForSampling();
    g_gpu_device->SetRenderTarget(dst);
    g_gpu_device->SetPipeline(m_deinterlace_extract_pipeline.get());
    g_gpu_device->SetTextureSampler(0, src, g_gpu_device->GetNearestSampler());
    const u32 uniforms[] = {x, y, line_skip};
    g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
    g_gpu_device->SetViewportAndScissor(0, 0, width, height);
    g_gpu_device->Draw(3, 0);
  }

  dst->MakeReadyForSampling();
  return true;
}

bool GPU::DeinterlaceSetTargetSize(u32 width, u32 height, bool preserve)
{
  if (!m_deinterlace_texture || m_deinterlace_texture->GetWidth() != width ||
      m_deinterlace_texture->GetHeight() != height)
  {
    if (!g_gpu_device->ResizeTexture(&m_deinterlace_texture, width, height, GPUTexture::Type::RenderTarget,
                                     GPUTexture::Format::RGBA8, GPUTexture::Flags::None, preserve)) [[unlikely]]
    {
      return false;
    }

    GL_OBJECT_NAME(m_deinterlace_texture, "Deinterlace target texture");
  }

  return true;
}

bool GPU::ApplyChromaSmoothing()
{
  const u32 x = m_display_texture_view_x;
  const u32 y = m_display_texture_view_y;
  const u32 width = m_display_texture_view_width;
  const u32 height = m_display_texture_view_height;
  if (!m_chroma_smoothing_texture || m_chroma_smoothing_texture->GetWidth() != width ||
      m_chroma_smoothing_texture->GetHeight() != height)
  {
    if (!g_gpu_device->ResizeTexture(&m_chroma_smoothing_texture, width, height, GPUTexture::Type::RenderTarget,
                                     GPUTexture::Format::RGBA8, GPUTexture::Flags::None, false))
    {
      ClearDisplayTexture();
      return false;
    }

    GL_OBJECT_NAME(m_chroma_smoothing_texture, "Chroma smoothing texture");
  }

  GL_SCOPE_FMT("ApplyChromaSmoothing({{{},{}}}, {}x{})", x, y, width, height);

  m_display_texture->MakeReadyForSampling();
  g_gpu_device->InvalidateRenderTarget(m_chroma_smoothing_texture.get());
  g_gpu_device->SetRenderTarget(m_chroma_smoothing_texture.get());
  g_gpu_device->SetPipeline(m_chroma_smoothing_pipeline.get());
  g_gpu_device->SetTextureSampler(0, m_display_texture, g_gpu_device->GetNearestSampler());
  const u32 uniforms[] = {x, y, width - 1, height - 1};
  g_gpu_device->PushUniformBuffer(uniforms, sizeof(uniforms));
  g_gpu_device->SetViewportAndScissor(0, 0, width, height);
  g_gpu_device->Draw(3, 0);

  m_chroma_smoothing_texture->MakeReadyForSampling();
  SetDisplayTexture(m_chroma_smoothing_texture.get(), m_display_depth_buffer, 0, 0, width, height);
  return true;
}

void GPU::CalculateDrawRect(s32 window_width, s32 window_height, bool apply_rotation, bool apply_aspect_ratio,
                            GSVector4i* display_rect, GSVector4i* draw_rect) const
{
  const bool integer_scale = (g_settings.display_scaling == DisplayScalingMode::NearestInteger ||
                              g_settings.display_scaling == DisplayScalingMode::BilinearInteger);
  const bool show_vram = g_settings.debugging.show_vram;
  const float window_ratio = static_cast<float>(window_width) / static_cast<float>(window_height);
  const float crtc_display_width = static_cast<float>(show_vram ? VRAM_WIDTH : m_crtc_state.display_width);
  const float crtc_display_height = static_cast<float>(show_vram ? VRAM_HEIGHT : m_crtc_state.display_height);
  const float display_aspect_ratio = ComputeDisplayAspectRatio();
  const float source_aspect_ratio = ComputeSourceAspectRatio();
  const float pixel_aspect_ratio = display_aspect_ratio / source_aspect_ratio;
  const float x_scale = apply_aspect_ratio ? pixel_aspect_ratio : 1.0f;
  float display_width = crtc_display_width;
  float display_height = crtc_display_height;
  float active_left = static_cast<float>(show_vram ? 0 : m_crtc_state.display_origin_left);
  float active_top = static_cast<float>(show_vram ? 0 : m_crtc_state.display_origin_top);
  float active_width = static_cast<float>(show_vram ? VRAM_WIDTH : m_crtc_state.display_vram_width);
  float active_height = static_cast<float>(show_vram ? VRAM_HEIGHT : m_crtc_state.display_vram_height);

  if (!g_settings.display_stretch_vertically)
  {
    display_width *= x_scale;
    active_left *= x_scale;
    active_width *= x_scale;
  }
  else
  {
    display_height /= x_scale;
    active_top /= x_scale;
    active_height /= x_scale;
  }

  // swap width/height when rotated, the flipping of padding is taken care of in the shader with the rotation matrix
  if (g_settings.display_rotation == DisplayRotation::Rotate90 ||
      g_settings.display_rotation == DisplayRotation::Rotate270)
  {
    std::swap(display_width, display_height);
    std::swap(active_width, active_height);
    std::swap(active_top, active_left);
  }

  // now fit it within the window
  float scale;
  float left_padding, top_padding;
  if ((display_width / display_height) >= window_ratio)
  {
    // align in middle vertically
    scale = static_cast<float>(window_width) / display_width;
    if (integer_scale)
    {
      scale = std::max(std::floor(scale), 1.0f);
      left_padding = std::max<float>((static_cast<float>(window_width) - display_width * scale) / 2.0f, 0.0f);
    }
    else
    {
      left_padding = 0.0f;
    }

    switch (g_settings.display_alignment)
    {
      case DisplayAlignment::RightOrBottom:
        top_padding = std::max<float>(static_cast<float>(window_height) - (display_height * scale), 0.0f);
        break;

      case DisplayAlignment::Center:
        top_padding = std::max<float>((static_cast<float>(window_height) - (display_height * scale)) / 2.0f, 0.0f);
        break;

      case DisplayAlignment::LeftOrTop:
      default:
        top_padding = 0.0f;
        break;
    }
  }
  else
  {
    // align in middle horizontally
    scale = static_cast<float>(window_height) / display_height;
    if (integer_scale)
    {
      scale = std::max(std::floor(scale), 1.0f);
      top_padding = std::max<float>((static_cast<float>(window_height) - (display_height * scale)) / 2.0f, 0.0f);
    }
    else
    {
      top_padding = 0.0f;
    }

    switch (g_settings.display_alignment)
    {
      case DisplayAlignment::RightOrBottom:
        left_padding = std::max<float>(static_cast<float>(window_width) - (display_width * scale), 0.0f);
        break;

      case DisplayAlignment::Center:
        left_padding = std::max<float>((static_cast<float>(window_width) - (display_width * scale)) / 2.0f, 0.0f);
        break;

      case DisplayAlignment::LeftOrTop:
      default:
        left_padding = 0.0f;
        break;
    }
  }

  // TODO: This should be a float rectangle. But because GL is lame, it only has integer viewports...
  const s32 left = static_cast<s32>(active_left * scale + left_padding);
  const s32 top = static_cast<s32>(active_top * scale + top_padding);
  const s32 right = left + static_cast<s32>(active_width * scale);
  const s32 bottom = top + static_cast<s32>(active_height * scale);
  *draw_rect = GSVector4i(left, top, right, bottom);
  *display_rect = GSVector4i(
    GSVector4(left_padding, top_padding, left_padding + display_width * scale, top_padding + display_height * scale));
}

bool CompressAndWriteTextureToFile(u32 width, u32 height, std::string path, FileSystem::ManagedCFilePtr fp, u8 quality,
                                   bool clear_alpha, bool flip_y, Image image, std::string osd_key)
{
  Error error;

  if (flip_y)
    image.FlipY();

  if (image.GetFormat() != ImageFormat::RGBA8)
  {
    std::optional<Image> convert_image = image.ConvertToRGBA8(&error);
    if (!convert_image.has_value())
    {
      ERROR_LOG("Failed to convert {} screenshot to RGBA8: {}", Image::GetFormatName(image.GetFormat()),
                error.GetDescription());
      image.Invalidate();
    }
    else
    {
      image = std::move(convert_image.value());
    }
  }

  bool result = false;
  if (image.IsValid())
  {
    if (clear_alpha)
      image.SetAllPixelsOpaque();

    result = image.SaveToFile(path.c_str(), fp.get(), quality, &error);
    if (!result)
      ERROR_LOG("Failed to save screenshot to '{}': '{}'", Path::GetFileName(path), error.GetDescription());
  }

  if (!osd_key.empty())
  {
    Host::AddIconOSDMessage(std::move(osd_key), ICON_EMOJI_CAMERA,
                            fmt::format(result ? TRANSLATE_FS("GPU", "Saved screenshot to '{}'.") :
                                                 TRANSLATE_FS("GPU", "Failed to save screenshot to '{}'."),
                                        Path::GetFileName(path),
                                        result ? Host::OSD_INFO_DURATION : Host::OSD_ERROR_DURATION));
  }

  return result;
}

bool GPU::WriteDisplayTextureToFile(std::string filename)
{
  if (!m_display_texture)
    return false;

  const u32 read_x = static_cast<u32>(m_display_texture_view_x);
  const u32 read_y = static_cast<u32>(m_display_texture_view_y);
  const u32 read_width = static_cast<u32>(m_display_texture_view_width);
  const u32 read_height = static_cast<u32>(m_display_texture_view_height);
  const ImageFormat read_format = GPUTexture::GetImageFormatForTextureFormat(m_display_texture->GetFormat());
  if (read_format == ImageFormat::None)
    return false;

  Image image(read_width, read_height, read_format);
  std::unique_ptr<GPUDownloadTexture> dltex;
  if (g_gpu_device->GetFeatures().memory_import)
  {
    dltex = g_gpu_device->CreateDownloadTexture(read_width, read_height, m_display_texture->GetFormat(),
                                                image.GetPixels(), image.GetStorageSize(), image.GetPitch());
  }
  if (!dltex)
  {
    if (!(dltex = g_gpu_device->CreateDownloadTexture(read_width, read_height, m_display_texture->GetFormat())))
    {
      ERROR_LOG("Failed to create {}x{} {} download texture", read_width, read_height,
                GPUTexture::GetFormatName(m_display_texture->GetFormat()));
      return false;
    }
  }

  dltex->CopyFromTexture(0, 0, m_display_texture, read_x, read_y, read_width, read_height, 0, 0, !dltex->IsImported());
  if (!dltex->ReadTexels(0, 0, read_width, read_height, image.GetPixels(), image.GetPitch()))
  {
    RestoreDeviceContext();
    return false;
  }

  RestoreDeviceContext();

  Error error;
  auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wb", &error);
  if (!fp)
  {
    ERROR_LOG("Can't open file '{}': {}", Path::GetFileName(filename), error.GetDescription());
    return false;
  }

  constexpr bool clear_alpha = true;
  const bool flip_y = g_gpu_device->UsesLowerLeftOrigin();

  return CompressAndWriteTextureToFile(read_width, read_height, std::move(filename), std::move(fp),
                                       g_settings.display_screenshot_quality, clear_alpha, flip_y, std::move(image),
                                       std::string());
}

bool GPU::RenderScreenshotToBuffer(u32 width, u32 height, const GSVector4i display_rect, const GSVector4i draw_rect,
                                   bool postfx, Image* out_image)
{
  const GPUTexture::Format hdformat =
    g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetFormat() : GPUTexture::Format::RGBA8;
  const ImageFormat image_format = GPUTexture::GetImageFormatForTextureFormat(hdformat);
  if (image_format == ImageFormat::None)
    return false;

  auto render_texture = g_gpu_device->FetchAutoRecycleTexture(width, height, 1, 1, 1, GPUTexture::Type::RenderTarget,
                                                              hdformat, GPUTexture::Flags::None);
  if (!render_texture)
    return false;

  g_gpu_device->ClearRenderTarget(render_texture.get(), GPUDevice::DEFAULT_CLEAR_COLOR);

  // TODO: this should use copy shader instead.
  RenderDisplay(render_texture.get(), display_rect, draw_rect, postfx);

  Image image(width, height, image_format);

  Error error;
  std::unique_ptr<GPUDownloadTexture> dltex;
  if (g_gpu_device->GetFeatures().memory_import)
  {
    dltex = g_gpu_device->CreateDownloadTexture(width, height, hdformat, image.GetPixels(), image.GetStorageSize(),
                                                image.GetPitch(), &error);
  }
  if (!dltex)
  {
    if (!(dltex = g_gpu_device->CreateDownloadTexture(width, height, hdformat, &error)))
    {
      ERROR_LOG("Failed to create {}x{} download texture: {}", width, height, error.GetDescription());
      return false;
    }
  }

  dltex->CopyFromTexture(0, 0, render_texture.get(), 0, 0, width, height, 0, 0, false);
  if (!dltex->ReadTexels(0, 0, width, height, image.GetPixels(), image.GetPitch()))
  {
    RestoreDeviceContext();
    return false;
  }

  RestoreDeviceContext();
  *out_image = std::move(image);
  return true;
}

void GPU::CalculateScreenshotSize(DisplayScreenshotMode mode, u32* width, u32* height, GSVector4i* display_rect,
                                  GSVector4i* draw_rect) const
{
  const bool internal_resolution = (mode != DisplayScreenshotMode::ScreenResolution || g_settings.debugging.show_vram);
  if (internal_resolution && m_display_texture_view_width != 0 && m_display_texture_view_height != 0)
  {
    if (mode == DisplayScreenshotMode::InternalResolution)
    {
      float f_width = static_cast<float>(m_display_texture_view_width);
      float f_height = static_cast<float>(m_display_texture_view_height);
      ApplyPixelAspectRatioToSize(&f_width, &f_height);

      // DX11 won't go past 16K texture size.
      const float max_texture_size = static_cast<float>(g_gpu_device->GetMaxTextureSize());
      if (f_width > max_texture_size)
      {
        f_height = f_height / (f_width / max_texture_size);
        f_width = max_texture_size;
      }
      if (f_height > max_texture_size)
      {
        f_height = max_texture_size;
        f_width = f_width / (f_height / max_texture_size);
      }

      *width = static_cast<u32>(std::ceil(f_width));
      *height = static_cast<u32>(std::ceil(f_height));
    }
    else // if (mode == DisplayScreenshotMode::UncorrectedInternalResolution)
    {
      *width = m_display_texture_view_width;
      *height = m_display_texture_view_height;
    }

    // Remove padding, it's not part of the framebuffer.
    *draw_rect = GSVector4i(0, 0, static_cast<s32>(*width), static_cast<s32>(*height));
    *display_rect = *draw_rect;
  }
  else
  {
    *width = g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetWidth() : 1;
    *height = g_gpu_device->HasMainSwapChain() ? g_gpu_device->GetMainSwapChain()->GetHeight() : 1;
    CalculateDrawRect(*width, *height, true, !g_settings.debugging.show_vram, display_rect, draw_rect);
  }
}

bool GPU::RenderScreenshotToFile(std::string path, DisplayScreenshotMode mode, u8 quality, bool compress_on_thread,
                                 bool show_osd_message)
{
  u32 width, height;
  GSVector4i display_rect, draw_rect;
  CalculateScreenshotSize(mode, &width, &height, &display_rect, &draw_rect);

  const bool internal_resolution = (mode != DisplayScreenshotMode::ScreenResolution);
  if (width == 0 || height == 0)
    return false;

  Image image;
  if (!RenderScreenshotToBuffer(width, height, display_rect, draw_rect, !internal_resolution, &image))
  {
    ERROR_LOG("Failed to render {}x{} screenshot", width, height);
    return false;
  }

  Error error;
  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb", &error);
  if (!fp)
  {
    ERROR_LOG("Can't open file '{}': {}", Path::GetFileName(path), error.GetDescription());
    return false;
  }

  std::string osd_key;
  if (show_osd_message)
  {
    // Use a 60 second timeout to give it plenty of time to actually save.
    osd_key = fmt::format("ScreenshotSaver_{}", path);
    Host::AddIconOSDMessage(osd_key, ICON_EMOJI_CAMERA_WITH_FLASH,
                            fmt::format(TRANSLATE_FS("GPU", "Saving screenshot to '{}'."), Path::GetFileName(path)),
                            60.0f);
  }

  if (compress_on_thread)
  {
    System::QueueTaskOnThread([width, height, path = std::move(path), fp = fp.release(), quality,
                               flip_y = g_gpu_device->UsesLowerLeftOrigin(), image = std::move(image),
                               osd_key = std::move(osd_key)]() mutable {
      CompressAndWriteTextureToFile(width, height, std::move(path), FileSystem::ManagedCFilePtr(fp), quality, true,
                                    flip_y, std::move(image), std::move(osd_key));
      System::RemoveSelfFromTaskThreads();
    });

    return true;
  }
  else
  {
    return CompressAndWriteTextureToFile(width, height, std::move(path), std::move(fp), quality, true,
                                         g_gpu_device->UsesLowerLeftOrigin(), std::move(image), std::move(osd_key));
  }
}

bool GPU::DumpVRAMToFile(const char* filename)
{
  ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);

  const char* extension = std::strrchr(filename, '.');
  if (extension && StringUtil::Strcasecmp(extension, ".png") == 0)
  {
    return DumpVRAMToFile(filename, VRAM_WIDTH, VRAM_HEIGHT, sizeof(u16) * VRAM_WIDTH, g_vram, true);
  }
  else if (extension && StringUtil::Strcasecmp(extension, ".bin") == 0)
  {
    return FileSystem::WriteBinaryFile(filename, g_vram, VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
  }
  else
  {
    ERROR_LOG("Unknown extension: '{}'", filename);
    return false;
  }
}

bool GPU::DumpVRAMToFile(const char* filename, u32 width, u32 height, u32 stride, const void* buffer, bool remove_alpha)
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

  return image.SaveToFile(filename);
}

void GPU::DrawDebugStateWindow(float scale)
{
  DrawRendererStats();

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

void GPU::DrawRendererStats()
{
}

void GPU::OnBufferSwapped()
{
}

void GPU::GetStatsString(SmallStringBase& str)
{
  if (IsHardwareRenderer())
  {
    str.format("{} HW | {} P | {} DC | {} B | {} RP | {} RB | {} C | {} W",
               GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()), m_stats.num_primitives,
               m_stats.host_num_draws, m_stats.host_num_barriers, m_stats.host_num_render_passes,
               m_stats.host_num_downloads, m_stats.num_copies, m_stats.num_writes);
  }
  else
  {
    str.format("{} SW | {} P | {} R | {} C | {} W", GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()),
               m_stats.num_primitives, m_stats.num_reads, m_stats.num_copies, m_stats.num_writes);
  }
}

void GPU::GetMemoryStatsString(SmallStringBase& str)
{
  const u32 vram_usage_mb = static_cast<u32>((g_gpu_device->GetVRAMUsage() + (1048576 - 1)) / 1048576);
  const u32 stream_kb = static_cast<u32>((m_stats.host_buffer_streamed + (1024 - 1)) / 1024);

  str.format("{} MB VRAM | {} KB STR | {} TC | {} TU", vram_usage_mb, stream_kb, m_stats.host_num_copies,
             m_stats.host_num_uploads);
}

void GPU::ResetStatistics()
{
  m_counters = {};
  g_gpu_device->ResetStatistics();
}

void GPU::UpdateStatistics(u32 frame_count)
{
  const GPUDevice::Statistics& stats = g_gpu_device->GetStatistics();
  const u32 round = (frame_count - 1);

#define UPDATE_COUNTER(x) m_stats.x = (m_counters.x + round) / frame_count
#define UPDATE_GPU_STAT(x) m_stats.host_##x = (stats.x + round) / frame_count

  UPDATE_COUNTER(num_reads);
  UPDATE_COUNTER(num_writes);
  UPDATE_COUNTER(num_copies);
  UPDATE_COUNTER(num_vertices);
  UPDATE_COUNTER(num_primitives);

  // UPDATE_COUNTER(num_read_texture_updates);
  // UPDATE_COUNTER(num_ubo_updates);

  UPDATE_GPU_STAT(buffer_streamed);
  UPDATE_GPU_STAT(num_draws);
  UPDATE_GPU_STAT(num_barriers);
  UPDATE_GPU_STAT(num_render_passes);
  UPDATE_GPU_STAT(num_copies);
  UPDATE_GPU_STAT(num_downloads);
  UPDATE_GPU_STAT(num_uploads);

#undef UPDATE_GPU_STAT
#undef UPDATE_COUNTER

  ResetStatistics();
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
    Host::AddIconOSDWarning(
      std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format("{}\n{}", TRANSLATE_SV("GPU", "Failed to start GPU trace:"), error.GetDescription()),
      Host::OSD_ERROR_DURATION);
    return false;
  }

  Host::AddIconOSDMessage(
    std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
    (num_frames != 0) ?
      fmt::format(TRANSLATE_FS("GPU", "Saving {0} frame GPU trace to '{1}'."), num_frames, Path::GetFileName(path)) :
      fmt::format(TRANSLATE_FS("GPU", "Saving multi-frame frame GPU trace to '{1}'."), num_frames,
                  Path::GetFileName(path)),
    Host::OSD_QUICK_DURATION);

  // save screenshot to same location to identify it
  RenderScreenshotToFile(Path::ReplaceExtension(path, "png"), DisplayScreenshotMode::ScreenResolution, 85, true, false);
  return true;
}

void GPU::StopRecordingGPUDump()
{
  if (!m_gpu_dump)
    return;

  Error error;
  if (!m_gpu_dump->Close(&error))
  {
    Host::AddIconOSDWarning(
      "GPUDump", ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format("{}\n{}", TRANSLATE_SV("GPU", "Failed to close GPU trace:"), error.GetDescription()),
      Host::OSD_ERROR_DURATION);
    m_gpu_dump.reset();
  }

  // Are we compressing the dump?
  const GPUDumpCompressionMode compress_mode =
    Settings::ParseGPUDumpCompressionMode(Host::GetTinyStringSettingValue("GPU", "DumpCompressionMode"))
      .value_or(Settings::DEFAULT_GPU_DUMP_COMPRESSION_MODE);
  std::string osd_key = fmt::format("GPUDump_{}", Path::GetFileName(m_gpu_dump->GetPath()));
  if (compress_mode == GPUDumpCompressionMode::Disabled)
  {
    Host::AddIconOSDMessage(
      "GPUDump", ICON_EMOJI_CAMERA_WITH_FLASH,
      fmt::format(TRANSLATE_FS("GPU", "Saved GPU trace to '{}'."), Path::GetFileName(m_gpu_dump->GetPath())),
      Host::OSD_QUICK_DURATION);
    m_gpu_dump.reset();
    return;
  }

  std::string source_path = m_gpu_dump->GetPath();
  m_gpu_dump.reset();

  // Use a 60 second timeout to give it plenty of time to actually save.
  Host::AddIconOSDMessage(
    osd_key, ICON_EMOJI_CAMERA_WITH_FLASH,
    fmt::format(TRANSLATE_FS("GPU", "Compressing GPU trace '{}'..."), Path::GetFileName(source_path)), 60.0f);
  System::QueueTaskOnThread(
    [compress_mode, source_path = std::move(source_path), osd_key = std::move(osd_key)]() mutable {
      Error error;
      if (GPUDump::Recorder::Compress(source_path, compress_mode, &error))
      {
        Host::AddIconOSDMessage(
          std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
          fmt::format(TRANSLATE_FS("GPU", "Saved GPU trace to '{}'."), Path::GetFileName(source_path)),
          Host::OSD_QUICK_DURATION);
      }
      else
      {
        Host::AddIconOSDWarning(
          std::move(osd_key), ICON_EMOJI_CAMERA_WITH_FLASH,
          fmt::format("{}\n{}",
                      SmallString::from_format(TRANSLATE_FS("GPU", "Failed to save GPU trace to '{}':"),
                                               Path::GetFileName(source_path)),
                      error.GetDescription()),
          Host::OSD_ERROR_DURATION);
      }

      System::RemoveSelfFromTaskThreads();
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
  switch (type)
  {
    case GPUDump::PacketType::GPUPort0Data:
    {
      if (data.empty()) [[unlikely]]
      {
        WARNING_LOG("Empty GPU dump GP0 packet!");
        return;
      }

      // ensure it doesn't block
      m_pending_command_ticks = 0;
      UpdateCommandTickEvent();

      if (data.size() == 1) [[unlikely]]
      {
        // direct GP0 write
        WriteRegister(0, data[0]);
      }
      else
      {
        // don't overflow the fifo...
        size_t current_word = 0;
        while (current_word < data.size())
        {
          const u32 block_size = std::min(m_fifo_size - m_fifo.GetSize(), static_cast<u32>(data.size() - current_word));
          if (block_size == 0)
          {
            ERROR_LOG("FIFO overflow while processing dump packet of {} words", data.size());
            break;
          }

          for (u32 i = 0; i < block_size; i++)
            m_fifo.Push(ZeroExtend64(data[current_word++]));
          ExecuteCommands();
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
      m_pending_command_ticks = 0;
      UpdateCommandTickEvent();

      // we _should_ be using the tick count for the event, but it breaks with looping.
      // instead, just add a fixed amount
      const TickCount crtc_ticks_per_frame =
        static_cast<TickCount>(m_crtc_state.horizontal_total) * static_cast<TickCount>(m_crtc_state.vertical_total);
      const TickCount system_ticks_per_frame =
        CRTCTicksToSystemTicks(crtc_ticks_per_frame, m_crtc_state.fractional_ticks);
      SystemTicksToCRTCTicks(system_ticks_per_frame, &m_crtc_state.fractional_ticks);
      TimingEvents::SetGlobalTickCounter(TimingEvents::GetGlobalTickCounter() +
                                         static_cast<GlobalTicks>(system_ticks_per_frame));

      FlushRender();
      UpdateDisplay();
      System::IncrementFrameNumber();
      System::FrameDone();
    }
    break;

    default:
      break;
  }
}
