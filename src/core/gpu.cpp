#include "gpu.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "stb_image_write.h"
#include "system.h"
#include "timers.h"
#include <cmath>
#include <imgui.h>
Log_SetChannel(GPU);

const GPU::GP0CommandHandlerTable GPU::s_GP0_command_handler_table = GPU::GenerateGP0CommandHandlerTable();

GPU::GPU() = default;

GPU::~GPU() = default;

bool GPU::Initialize(HostDisplay* host_display, System* system, DMA* dma, InterruptController* interrupt_controller,
                     Timers* timers)
{
  m_host_display = host_display;
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  m_timers = timers;
  m_force_progressive_scan = m_system->GetSettings().gpu_force_progressive_scan;
  m_tick_event =
    m_system->CreateTimingEvent("GPU Tick", 1, 1, std::bind(&GPU::Execute, this, std::placeholders::_1), true);
  return true;
}

void GPU::UpdateSettings()
{
  m_force_progressive_scan = m_system->GetSettings().gpu_force_progressive_scan;
}

void GPU::Reset()
{
  SoftReset();
  m_set_texture_disable_mask = false;
  m_GPUREAD_latch = 0;
}

void GPU::SoftReset()
{
  m_GPUSTAT.bits = 0x14802000;
  m_GPUSTAT.pal_mode = m_system->IsPALRegion();
  m_drawing_area.Set(0, 0, 0, 0);
  m_drawing_area_changed = true;
  m_drawing_offset = {};
  m_drawing_offset_changed = true;
  std::memset(&m_crtc_state, 0, sizeof(m_crtc_state));
  m_crtc_state.regs.display_address_start = 0;
  m_crtc_state.regs.horizontal_display_range = 0xC60260;
  m_crtc_state.regs.vertical_display_range = 0x3FC10;
  m_state = State::Idle;
  m_command_total_words = 0;
  m_vram_transfer = {};
  m_GP0_buffer.clear();
  SetDrawMode(0);
  SetTexturePalette(0);
  m_draw_mode.SetTextureWindow(0);
  UpdateGPUSTAT();
  UpdateCRTCConfig();

  m_tick_event->Deactivate();
  UpdateSliceTicks();
}

bool GPU::DoState(StateWrapper& sw)
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
  sw.Do(&m_draw_mode.texture_window_mask_x);
  sw.Do(&m_draw_mode.texture_window_mask_y);
  sw.Do(&m_draw_mode.texture_window_offset_x);
  sw.Do(&m_draw_mode.texture_window_offset_y);
  sw.Do(&m_draw_mode.texture_x_flip);
  sw.Do(&m_draw_mode.texture_y_flip);

  sw.Do(&m_drawing_area.left);
  sw.Do(&m_drawing_area.top);
  sw.Do(&m_drawing_area.right);
  sw.Do(&m_drawing_area.bottom);
  sw.Do(&m_drawing_offset.x);
  sw.Do(&m_drawing_offset.y);
  sw.Do(&m_drawing_offset.x);

  sw.Do(&m_set_texture_disable_mask);

  sw.Do(&m_crtc_state.regs.display_address_start);
  sw.Do(&m_crtc_state.regs.horizontal_display_range);
  sw.Do(&m_crtc_state.regs.vertical_display_range);
  sw.Do(&m_crtc_state.dot_clock_divider);
  sw.Do(&m_crtc_state.display_width);
  sw.Do(&m_crtc_state.display_height);
  sw.Do(&m_crtc_state.horizontal_total);
  sw.Do(&m_crtc_state.horizontal_display_start);
  sw.Do(&m_crtc_state.horizontal_display_end);
  sw.Do(&m_crtc_state.vertical_total);
  sw.Do(&m_crtc_state.vertical_display_start);
  sw.Do(&m_crtc_state.vertical_display_end);
  sw.Do(&m_crtc_state.fractional_ticks);
  sw.Do(&m_crtc_state.current_tick_in_scanline);
  sw.Do(&m_crtc_state.current_scanline);
  sw.Do(&m_crtc_state.display_aspect_ratio);
  sw.Do(&m_crtc_state.in_hblank);
  sw.Do(&m_crtc_state.in_vblank);

  sw.Do(&m_GPUREAD_latch);

  sw.Do(&m_vram_transfer.x);
  sw.Do(&m_vram_transfer.y);
  sw.Do(&m_vram_transfer.width);
  sw.Do(&m_vram_transfer.height);
  sw.Do(&m_vram_transfer.col);
  sw.Do(&m_vram_transfer.row);

  sw.Do(&m_GP0_buffer);

  if (sw.IsReading())
  {
    m_draw_mode.texture_page_changed = true;
    m_draw_mode.texture_window_changed = true;
    m_drawing_area_changed = true;
    m_drawing_offset_changed = true;
    UpdateGPUSTAT();
  }

  if (!sw.DoMarker("GPU-VRAM"))
    return false;

  if (sw.IsReading())
  {
    // Need to clear the mask bits since we want to pull it in from the copy.
    const u32 old_GPUSTAT = m_GPUSTAT.bits;
    m_GPUSTAT.check_mask_before_draw = false;
    m_GPUSTAT.set_mask_while_drawing = false;

    // Still need a temporary here.
    HeapArray<u16, VRAM_WIDTH * VRAM_HEIGHT> temp;
    sw.DoBytes(temp.data(), VRAM_WIDTH * VRAM_HEIGHT * sizeof(u16));
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, temp.data());

    // Restore mask setting.
    m_GPUSTAT.bits = old_GPUSTAT;

    UpdateDisplay();
    UpdateSliceTicks();
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

void GPU::UpdateGPUSTAT()
{
  m_GPUSTAT.ready_to_send_vram = (m_state == State::ReadingVRAM);
  m_GPUSTAT.ready_to_recieve_cmd = (m_state == State::Idle);
  m_GPUSTAT.ready_to_recieve_dma =
    (m_state == State::Idle || (m_state != State::ReadingVRAM && m_command_total_words > 0));

  bool dma_request;
  switch (m_GPUSTAT.dma_direction)
  {
    case DMADirection::Off:
      dma_request = false;
      break;

    case DMADirection::FIFO:
      dma_request = true; // FIFO not full/full
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
  m_dma->SetRequest(DMA::Channel::GPU, dma_request);
}

u32 GPU::ReadRegister(u32 offset)
{
  switch (offset)
  {
    case 0x00:
      return ReadGPUREAD();

    case 0x04:
      return m_GPUSTAT.bits;

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
      WriteGP0(value);
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

void GPU::DMAWrite(const u32* words, u32 word_count)
{
  switch (m_GPUSTAT.dma_direction)
  {
    case DMADirection::CPUtoGP0:
    {
      std::copy(words, words + word_count, std::back_inserter(m_GP0_buffer));
      ExecuteCommands();
    }
    break;

    default:
    {
      Log_ErrorPrintf("Unhandled GPU DMA write mode %u for %u words",
                      static_cast<u32>(m_GPUSTAT.dma_direction.GetValue()), word_count);
    }
    break;
  }
}

void GPU::Synchronize()
{
  m_tick_event->InvokeEarly();
}

void GPU::UpdateCRTCConfig()
{
  static constexpr std::array<TickCount, 8> dot_clock_dividers = {{10, 8, 5, 4, 7, 7, 7, 7}};
  CRTCState& cs = m_crtc_state;

  if (m_GPUSTAT.pal_mode)
  {
    cs.vertical_total = 314;
    cs.horizontal_total = 3406;
  }
  else
  {
    cs.vertical_total = 263;
    cs.horizontal_total = 3413;
  }

  const TickCount ticks_per_frame = cs.horizontal_total * cs.vertical_total;
  const double vertical_frequency =
    static_cast<double>((u64(MASTER_CLOCK) * 11) / 7) / static_cast<double>(ticks_per_frame);
  m_system->SetThrottleFrequency(vertical_frequency);

  const u8 horizontal_resolution_index = m_GPUSTAT.horizontal_resolution_1 | (m_GPUSTAT.horizontal_resolution_2 << 2);
  cs.dot_clock_divider = dot_clock_dividers[horizontal_resolution_index];
  cs.horizontal_display_start = static_cast<TickCount>(std::min<u32>(cs.regs.X1, cs.horizontal_total));
  cs.horizontal_display_end = static_cast<TickCount>(std::min<u32>(cs.regs.X2, cs.horizontal_total));
  cs.vertical_display_start = static_cast<TickCount>(std::min<u32>(cs.regs.Y1, cs.vertical_total));
  cs.vertical_display_end = static_cast<TickCount>(std::min<u32>(cs.regs.Y2, cs.vertical_total));

  // check for a change in resolution
  const u32 old_horizontal_resolution = cs.display_width;
  const u32 old_vertical_resolution = cs.display_height;
  const u32 visible_lines = cs.regs.Y2 - cs.regs.Y1;
  cs.display_width = std::max<u32>((cs.regs.X2 - cs.regs.X1) / cs.dot_clock_divider, 1);
  cs.display_height = visible_lines << BoolToUInt8(m_GPUSTAT.In480iMode());

  if (cs.display_width != old_horizontal_resolution || cs.display_height != old_vertical_resolution)
    Log_InfoPrintf("Visible resolution is now %ux%u", cs.display_width, cs.display_height);

  // Compute the aspect ratio necessary to display borders in the inactive region of the picture.
  // Convert total dots/lines to time.
  const float dot_clock =
    (static_cast<float>(MASTER_CLOCK) * (11.0f / 7.0f / static_cast<float>(cs.dot_clock_divider)));
  const float dot_clock_period = 1.0f / dot_clock;
  const float dots_per_scanline = static_cast<float>(cs.horizontal_total) / static_cast<float>(cs.dot_clock_divider);
  const float horizontal_period = dots_per_scanline * dot_clock_period;
  const float vertical_period = horizontal_period * static_cast<float>(cs.vertical_total);

  // Convert active dots/lines to time.
  const float visible_dots_per_scanline = static_cast<float>(cs.display_width);
  const float horizontal_active_time = horizontal_period * visible_dots_per_scanline;
  const float vertical_active_time = horizontal_active_time * static_cast<float>(visible_lines);

  // Use the reference active time/lines for the signal to work out the border area, and thus aspect ratio
  // transformation for the active area in our framebuffer. For the purposes of these calculations, we're assuming
  // progressive scan.
  float display_ratio;
  if (m_GPUSTAT.pal_mode)
  {
    // Wikipedia says PAL is active 51.95us of 64.00us, and 576/625 lines.
    const float signal_horizontal_active_time = 51.95f;
    const float signal_horizontal_total_time = 64.0f;
    const float signal_vertical_active_lines = 576.0f;
    const float signal_vertical_total_lines = 625.0f;
    const float h_ratio =
      (horizontal_active_time / horizontal_period) * (signal_horizontal_total_time / signal_horizontal_active_time);
    const float v_ratio =
      (vertical_active_time / vertical_period) * (signal_vertical_total_lines / signal_vertical_active_lines);
    display_ratio = h_ratio / v_ratio;
  }
  else
  {
    const float signal_horizontal_active_time = 52.66f;
    const float signal_horizontal_total_time = 63.56f;
    const float signal_vertical_active_lines = 486.0f;
    const float signal_vertical_total_lines = 525.0f;
    const float h_ratio =
      (horizontal_active_time / horizontal_period) * (signal_horizontal_total_time / signal_horizontal_active_time);
    const float v_ratio =
      (vertical_active_time / vertical_period) * (signal_vertical_total_lines / signal_vertical_active_lines);
    display_ratio = h_ratio / v_ratio;
  }

  // Ensure the numbers are sane, and not due to a misconfigured active display range.
  cs.display_aspect_ratio = (std::isnormal(display_ratio) && display_ratio != 0.0f) ? display_ratio : (4.0f / 3.0f);
  m_tick_event->SetInterval(cs.horizontal_total);
}

static TickCount GPUTicksToSystemTicks(u32 gpu_ticks)
{
  // convert to master clock, rounding up as we want to overshoot not undershoot
  return (gpu_ticks * 7 + 10) / 11;
}

void GPU::UpdateSliceTicks()
{
  // figure out how many GPU ticks until the next vblank
  const u32 lines_until_vblank =
    (m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end ?
       (m_crtc_state.vertical_total - m_crtc_state.current_scanline + m_crtc_state.vertical_display_end) :
       (m_crtc_state.vertical_display_end - m_crtc_state.current_scanline));
  const u32 ticks_until_vblank =
    lines_until_vblank * m_crtc_state.horizontal_total - m_crtc_state.current_tick_in_scanline;
  const u32 ticks_until_hblank =
    (m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_display_end) ?
      (m_crtc_state.horizontal_total - m_crtc_state.current_tick_in_scanline + m_crtc_state.horizontal_display_end) :
      (m_crtc_state.horizontal_display_end - m_crtc_state.current_tick_in_scanline);

  m_tick_event->Schedule(GPUTicksToSystemTicks(ticks_until_vblank));
  m_tick_event->SetPeriod(GPUTicksToSystemTicks(ticks_until_hblank));
}

void GPU::Execute(TickCount ticks)
{
  // convert cpu/master clock to GPU ticks, accounting for partial cycles because of the non-integer divider
  {
    const TickCount temp = (ticks * 11) + m_crtc_state.fractional_ticks;
    m_crtc_state.current_tick_in_scanline += temp / 7;
    m_crtc_state.fractional_ticks = temp % 7;
  }

  if (m_crtc_state.current_tick_in_scanline < m_crtc_state.horizontal_total)
  {
    // short path when we execute <1 line.. this shouldn't occur often.
    const bool old_hblank = m_crtc_state.in_hblank;
    const bool new_hblank = m_crtc_state.current_tick_in_scanline < m_crtc_state.horizontal_display_start ||
                            m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_display_end;
    if (!old_hblank && new_hblank && m_timers->IsUsingExternalClock(HBLANK_TIMER_INDEX))
      m_timers->AddTicks(HBLANK_TIMER_INDEX, 1);

    UpdateSliceTicks();
    return;
  }

  u32 lines_to_draw = m_crtc_state.current_tick_in_scanline / m_crtc_state.horizontal_total;
  m_crtc_state.current_tick_in_scanline %= m_crtc_state.horizontal_total;
#if 0
  Log_WarningPrintf("Old line: %u, new line: %u, drawing %u", m_crtc_state.current_scanline,
                    m_crtc_state.current_scanline + lines_to_draw, lines_to_draw);
#endif

  const bool old_hblank = m_crtc_state.in_hblank;
  const bool new_hblank = m_crtc_state.current_tick_in_scanline < m_crtc_state.horizontal_display_start ||
                          m_crtc_state.current_tick_in_scanline >= m_crtc_state.horizontal_display_end;
  m_crtc_state.in_hblank = new_hblank;
  if (m_timers->IsUsingExternalClock(HBLANK_TIMER_INDEX))
  {
    const u32 hblank_timer_ticks = BoolToUInt32(!old_hblank) + BoolToUInt32(new_hblank) + (lines_to_draw - 1);
    m_timers->AddTicks(HBLANK_TIMER_INDEX, static_cast<TickCount>(hblank_timer_ticks));
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
      m_timers->SetGate(HBLANK_TIMER_INDEX, false);
      m_crtc_state.in_vblank = false;
    }

    const bool new_vblank = m_crtc_state.current_scanline < m_crtc_state.vertical_display_start ||
                            m_crtc_state.current_scanline >= m_crtc_state.vertical_display_end;
    if (m_crtc_state.in_vblank != new_vblank)
    {
      if (new_vblank)
      {
        Log_DebugPrintf("Now in v-blank");
        m_interrupt_controller->InterruptRequest(InterruptController::IRQ::VBLANK);

        // flush any pending draws and "scan out" the image
        FlushRender();
        UpdateDisplay();
        m_system->IncrementFrameNumber();
      }

      m_timers->SetGate(HBLANK_TIMER_INDEX, new_vblank);
      m_crtc_state.in_vblank = new_vblank;
    }

    // past the end of vblank?
    if (m_crtc_state.current_scanline == m_crtc_state.vertical_total)
    {
      // start the new frame
      m_crtc_state.current_scanline = 0;

      // switch fields for interlaced modes
      if (m_GPUSTAT.vertical_interlace)
      {
        m_GPUSTAT.interlaced_field ^= true;
        m_crtc_state.current_scanline = BoolToUInt32(!m_GPUSTAT.interlaced_field);
      }
      else
      {
        m_GPUSTAT.interlaced_field = false;
      }
    }
  }

  // alternating even line bit in 240-line mode
  if (m_GPUSTAT.In480iMode())
  {
    m_GPUSTAT.drawing_even_line =
      ConvertToBoolUnchecked((m_crtc_state.regs.Y + BoolToUInt32(!m_GPUSTAT.interlaced_field)) & u32(1));
  }
  else
  {
    m_GPUSTAT.drawing_even_line =
      ConvertToBoolUnchecked((m_crtc_state.regs.Y + m_crtc_state.current_scanline) & u32(1));
  }

  UpdateSliceTicks();
}

u32 GPU::ReadGPUREAD()
{
  if (m_state != State::ReadingVRAM)
    return m_GPUREAD_latch;

  // Read two pixels out of VRAM and combine them. Zero fill odd pixel counts.
  u32 value = 0;
  for (u32 i = 0; i < 2; i++)
  {
    // Read with correct wrap-around behavior.
    const u16 read_x = (m_vram_transfer.x + m_vram_transfer.col) % VRAM_WIDTH;
    const u16 read_y = (m_vram_transfer.y + m_vram_transfer.row) % VRAM_HEIGHT;
    value = (ZeroExtend32(m_vram_ptr[read_y * VRAM_WIDTH + read_x]) << 16) | (value >> 16);

    if (++m_vram_transfer.col == m_vram_transfer.width)
    {
      m_vram_transfer.col = 0;

      if (++m_vram_transfer.row == m_vram_transfer.height)
      {
        Log_DebugPrintf("End of VRAM->CPU transfer");
        m_vram_transfer = {};
        m_state = State::Idle;
        UpdateGPUSTAT();

        // end of transfer, catch up on any commands which were written (unlikely)
        ExecuteCommands();
        break;
      }
    }
  }

  m_GPUREAD_latch = value;
  return value;
}

void GPU::WriteGP0(u32 value)
{
  m_GP0_buffer.push_back(value);
  ExecuteCommands();
}

void GPU::WriteGP1(u32 value)
{
  const u8 command = Truncate8(value >> 24);
  const u32 param = value & UINT32_C(0x00FFFFFF);
  switch (command)
  {
    case 0x00: // Reset GPU
    {
      Log_DebugPrintf("GP1 reset GPU");
      SoftReset();
    }
    break;

    case 0x01: // Clear FIFO
    {
      Log_DebugPrintf("GP1 clear FIFO");
      m_state = State::Idle;
      m_command_total_words = 0;
      m_vram_transfer = {};
      m_GP0_buffer.clear();
      UpdateGPUSTAT();
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
      m_GPUSTAT.display_disable = disable;
    }
    break;

    case 0x04: // DMA Direction
    {
      m_GPUSTAT.dma_direction = static_cast<DMADirection>(param);
      Log_DebugPrintf("DMA direction <- 0x%02X", static_cast<u32>(m_GPUSTAT.dma_direction.GetValue()));
      UpdateGPUSTAT();
    }
    break;

    case 0x05: // Set display start address
    {
      m_crtc_state.regs.display_address_start = param & CRTCState::Regs::DISPLAY_ADDRESS_START_MASK;
      Log_DebugPrintf("Display address start <- 0x%08X", m_crtc_state.regs.display_address_start);
      m_system->IncrementInternalFrameNumber();
    }
    break;

    case 0x06: // Set horizontal display range
    {
      const u32 new_value = param & CRTCState::Regs::HORIZONTAL_DISPLAY_RANGE_MASK;
      Log_DebugPrintf("Horizontal display range <- 0x%08X", new_value);

      if (m_crtc_state.regs.horizontal_display_range != new_value)
      {
        m_tick_event->InvokeEarly(true);
        m_crtc_state.regs.horizontal_display_range = new_value;
        UpdateCRTCConfig();
      }
    }
    break;

    case 0x07: // Set display start address
    {
      const u32 new_value = param & CRTCState::Regs::VERTICAL_DISPLAY_RANGE_MASK;
      Log_DebugPrintf("Vertical display range <- 0x%08X", new_value);

      if (m_crtc_state.regs.vertical_display_range != new_value)
      {
        m_tick_event->InvokeEarly(true);
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

      if (m_GPUSTAT.bits != new_GPUSTAT.bits)
      {
        m_tick_event->InvokeEarly(true);
        m_GPUSTAT.bits = new_GPUSTAT.bits;
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

void GPU::UpdateDisplay() {}

void GPU::ReadVRAM(u32 x, u32 y, u32 width, u32 height) {}

void GPU::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) {}

void GPU::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  // Fast path when the copy is not oversized.
  if ((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT && !m_GPUSTAT.IsMaskingEnabled())
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
    const u16* src_ptr = static_cast<const u16*>(data);
    const u16 mask_and = m_GPUSTAT.GetMaskAND();
    const u16 mask_or = m_GPUSTAT.GetMaskOR();

    for (u32 row = 0; row < height;)
    {
      u16* dst_row_ptr = &m_vram_ptr[((y + row++) % VRAM_HEIGHT) * VRAM_WIDTH];
      for (u32 col = 0; col < width;)
      {
        // TODO: Handle unaligned reads...
        u16* pixel_ptr = &dst_row_ptr[(x + col++) % VRAM_WIDTH];
        if (((*pixel_ptr) & mask_and) == mask_and)
          *pixel_ptr = *(src_ptr++) | mask_or;
      }
    }
  }
}

void GPU::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) {}

void GPU::DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr) {}

void GPU::FlushRender() {}

void GPU::SetDrawMode(u16 value)
{
  DrawMode::Reg new_mode_reg{static_cast<u16>(value & DrawMode::Reg::MASK)};
  if (!m_set_texture_disable_mask)
    new_mode_reg.texture_disable = false;

  if (new_mode_reg.bits == m_draw_mode.mode_reg.bits)
    return;

  if ((new_mode_reg.bits & DrawMode::Reg::TEXTURE_PAGE_MASK) !=
      (m_draw_mode.mode_reg.bits & DrawMode::Reg::TEXTURE_PAGE_MASK))
  {
    m_draw_mode.texture_page_x = new_mode_reg.GetTexturePageXBase();
    m_draw_mode.texture_page_y = new_mode_reg.GetTexturePageYBase();
    m_draw_mode.texture_page_changed = true;
  }

  m_draw_mode.mode_reg.bits = new_mode_reg.bits;

  // Bits 0..10 are returned in the GPU status register.
  m_GPUSTAT.bits =
    (m_GPUSTAT.bits & ~(DrawMode::Reg::GPUSTAT_MASK)) | (ZeroExtend32(new_mode_reg.bits) & DrawMode::Reg::GPUSTAT_MASK);
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

void GPU::DrawMode::SetTextureWindow(u32 value)
{
  value &= TEXTURE_WINDOW_MASK;
  if (texture_window_value == value)
    return;

  texture_window_mask_x = value & UINT32_C(0x1F);
  texture_window_mask_y = (value >> 5) & UINT32_C(0x1F);
  texture_window_offset_x = (value >> 10) & UINT32_C(0x1F);
  texture_window_offset_y = (value >> 15) & UINT32_C(0x1F);
  texture_window_value = value;
  texture_window_changed = true;
}

bool GPU::DumpVRAMToFile(const char* filename, u32 width, u32 height, u32 stride, const void* buffer, bool remove_alpha)
{
  std::vector<u32> rgba8_buf(width * height);

  const char* ptr_in = static_cast<const char*>(buffer);
  u32* ptr_out = rgba8_buf.data();
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
  return (stbi_write_png(filename, width, height, 4, rgba8_buf.data(), sizeof(u32) * width) != 0);
}

void GPU::DrawDebugStateWindow()
{
  ImGui::SetNextWindowSize(ImVec2(450, 550), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("GPU", &m_system->GetSettings().debugging.show_gpu_state))
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
    ImGui::SetColumnWidth(0, 200.0f);

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

  if (ImGui::CollapsingHeader("CRTC", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const auto& cs = m_crtc_state;
    ImGui::Text("Dot Clock Divider: %u", cs.dot_clock_divider);
    ImGui::Text("Vertical Interlace: %s (%s field)", m_GPUSTAT.vertical_interlace ? "Yes" : "No",
                m_GPUSTAT.interlaced_field ? "odd" : "even");
    ImGui::Text("Display Disable: %s", m_GPUSTAT.display_disable ? "Yes" : "No");
    ImGui::Text("Drawing Even Line: %s", m_GPUSTAT.drawing_even_line ? "Yes" : "No");
    ImGui::Text("Display Resolution: %ux%u", cs.display_width, cs.display_height);
    ImGui::Text("Color Depth: %u-bit", m_GPUSTAT.display_area_color_depth_24 ? 24 : 15);
    ImGui::Text("Start Offset: (%u, %u)", cs.regs.X.GetValue(), cs.regs.Y.GetValue());
    ImGui::Text("Display Total: %u (%u) horizontal, %u vertical", cs.horizontal_total,
                cs.horizontal_total / cs.dot_clock_divider, cs.vertical_total);
    ImGui::Text("Display Range: %u-%u (%u-%u), %u-%u", cs.regs.X1.GetValue(), cs.regs.X2.GetValue(),
                cs.regs.X1.GetValue() / cs.dot_clock_divider, cs.regs.X2.GetValue() / cs.dot_clock_divider,
                cs.regs.Y1.GetValue(), cs.regs.Y2.GetValue());
    ImGui::Text("Current Scanline: %u (tick %u)", cs.current_scanline, cs.current_tick_in_scanline);
  }

  if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Text("Dither: %s", m_GPUSTAT.dither_enable ? "Enabled" : "Disabled");
    ImGui::Text("Draw To Display Area: %s", m_GPUSTAT.dither_enable ? "Yes" : "No");
    ImGui::Text("Draw Set Mask Bit: %s", m_GPUSTAT.set_mask_while_drawing ? "Yes" : "No");
    ImGui::Text("Draw To Masked Pixels: %s", m_GPUSTAT.check_mask_before_draw ? "Yes" : "No");
    ImGui::Text("Reverse Flag: %s", m_GPUSTAT.reverse_flag ? "Yes" : "No");
    ImGui::Text("Texture Disable: %s", m_GPUSTAT.texture_disable ? "Yes" : "No");
    ImGui::Text("PAL Mode: %s", m_GPUSTAT.pal_mode ? "Yes" : "No");
    ImGui::Text("Interrupt Request: %s", m_GPUSTAT.interrupt_request ? "Yes" : "No");
    ImGui::Text("DMA Request: %s", m_GPUSTAT.dma_data_request ? "Yes" : "No");
  }

  ImGui::End();
}

void GPU::DrawRendererStats(bool is_idle_frame) {}
