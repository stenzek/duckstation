#include "gpu.h"
#include "YBaseLib/Log.h"
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

bool GPU::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller, Timers* timers)
{
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  m_timers = timers;
  return true;
}

void GPU::UpdateResolutionScale()
{
  const u32 new_scale = std::min(m_system->GetSettings().gpu_resolution_scale, m_max_resolution_scale);
  if (m_resolution_scale == new_scale)
    return;

  m_resolution_scale = new_scale;
  m_system->GetHostInterface()->AddOSDMessage(TinyString::FromFormat(
    "Changed internal resolution to %ux (%ux%u)", m_resolution_scale, VRAM_WIDTH * new_scale, VRAM_HEIGHT * new_scale));
}

void GPU::Reset()
{
  SoftReset();
}

void GPU::SoftReset()
{
  m_GPUSTAT.bits = 0x14802000;
  m_drawing_area = {};
  m_drawing_offset = {};
  std::memset(&m_crtc_state, 0, sizeof(m_crtc_state));
  m_crtc_state.regs.display_address_start = 0;
  m_crtc_state.regs.horizontal_display_range = 0xC60260;
  m_crtc_state.regs.vertical_display_range = 0x3FC10;
  m_GP0_buffer.clear();
  m_GPUREAD_buffer.clear();
  m_render_state = {};
  m_render_state.texture_page_changed = true;
  m_render_state.texture_color_mode_changed = true;
  m_render_state.transparency_mode_changed = true;
  UpdateGPUSTAT();
  UpdateCRTCConfig();
}

bool GPU::DoState(StateWrapper& sw)
{
  if (sw.IsReading())
  {
    // perform a reset to discard all pending draws/fb state
    Reset();
  }

  sw.Do(&m_GPUSTAT.bits);

  sw.Do(&m_render_state.texture_page_x);
  sw.Do(&m_render_state.texture_page_y);
  sw.Do(&m_render_state.texture_palette_x);
  sw.Do(&m_render_state.texture_palette_y);
  sw.Do(&m_render_state.texture_color_mode);
  sw.Do(&m_render_state.transparency_mode);
  sw.Do(&m_render_state.texture_window_mask_x);
  sw.Do(&m_render_state.texture_window_mask_y);
  sw.Do(&m_render_state.texture_window_offset_x);
  sw.Do(&m_render_state.texture_window_offset_y);
  sw.Do(&m_render_state.texture_x_flip);
  sw.Do(&m_render_state.texture_y_flip);
  sw.Do(&m_render_state.texpage_attribute);
  sw.Do(&m_render_state.texlut_attribute);
  sw.Do(&m_render_state.texture_window_value);
  sw.Do(&m_render_state.texture_page_changed);
  sw.Do(&m_render_state.texture_color_mode_changed);
  sw.Do(&m_render_state.transparency_mode_changed);
  sw.Do(&m_render_state.texture_window_changed);

  sw.Do(&m_drawing_area.left);
  sw.Do(&m_drawing_area.top);
  sw.Do(&m_drawing_area.right);
  sw.Do(&m_drawing_area.bottom);
  sw.Do(&m_drawing_offset.x);
  sw.Do(&m_drawing_offset.y);
  sw.Do(&m_drawing_offset.x);

  sw.Do(&m_crtc_state.regs.display_address_start);
  sw.Do(&m_crtc_state.regs.horizontal_display_range);
  sw.Do(&m_crtc_state.regs.vertical_display_range);
  sw.Do(&m_crtc_state.horizontal_resolution);
  sw.Do(&m_crtc_state.vertical_resolution);
  sw.Do(&m_crtc_state.dot_clock_divider);
  sw.Do(&m_crtc_state.display_width);
  sw.Do(&m_crtc_state.display_height);
  sw.Do(&m_crtc_state.ticks_per_scanline);
  sw.Do(&m_crtc_state.visible_ticks_per_scanline);
  sw.Do(&m_crtc_state.visible_scanlines_per_frame);
  sw.Do(&m_crtc_state.total_scanlines_per_frame);
  sw.Do(&m_crtc_state.fractional_ticks);
  sw.Do(&m_crtc_state.current_tick_in_scanline);
  sw.Do(&m_crtc_state.current_scanline);
  sw.Do(&m_crtc_state.in_hblank);
  sw.Do(&m_crtc_state.in_vblank);

  if (sw.IsReading())
    UpdateSliceTicks();

  sw.Do(&m_GP0_buffer);
  sw.Do(&m_GPUREAD_buffer);

  if (sw.IsReading())
  {
    m_render_state.texture_page_changed = true;
    m_render_state.texture_color_mode_changed = true;
    m_render_state.transparency_mode_changed = true;
    m_render_state.texture_window_changed = true;
    UpdateDrawingArea();
    UpdateGPUSTAT();
  }

  if (!sw.DoMarker("GPU-VRAM"))
    return false;

  if (sw.IsReading())
  {
    std::vector<u16> vram;
    sw.Do(&vram);
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, vram.data());
  }
  else
  {
    std::vector<u16> vram(VRAM_WIDTH * VRAM_HEIGHT);
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, vram.data());
    sw.Do(&vram);
  }

  return !sw.HasError();
}

void GPU::ResetGraphicsAPIState() {}

void GPU::RestoreGraphicsAPIState() {}

void GPU::UpdateGPUSTAT()
{
  m_GPUSTAT.ready_to_send_vram = !m_GPUREAD_buffer.empty();
  m_GPUSTAT.ready_to_recieve_cmd = m_GPUREAD_buffer.empty();
  m_GPUSTAT.ready_to_recieve_dma = m_GPUREAD_buffer.empty();

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
    {
      // Bit 31 of GPUSTAT is always clear during vblank.
      u32 bits = m_GPUSTAT.bits;
      bits &= ~(BoolToUInt32(m_crtc_state.in_vblank) << 31);
      return bits;
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

  const u32 words_to_copy = std::min(word_count, static_cast<u32>(m_GPUREAD_buffer.size()));
  if (!m_GPUREAD_buffer.empty())
  {
    auto it = m_GPUREAD_buffer.begin();
    for (u32 i = 0; i < word_count; i++)
      words[i] = *(it++);

    m_GPUREAD_buffer.erase(m_GPUREAD_buffer.begin(), it);
  }
  if (words_to_copy < word_count)
  {
    Log_WarningPrintf("Partially-empty GPUREAD buffer on GPU DMA read");
    std::fill_n(words + words_to_copy, word_count - words_to_copy, u32(0));
  }

  UpdateGPUSTAT();
}

void GPU::DMAWrite(const u32* words, u32 word_count)
{
  switch (m_GPUSTAT.dma_direction)
  {
    case DMADirection::CPUtoGP0:
    {
#if 0
      // partial command buffered? have to go through the slow path
      if (!m_GP0_buffer.empty())
      {
        std::copy(words, words + word_count, std::back_inserter(m_GP0_buffer));
        const u32* command_ptr = m_GP0_buffer.data();
        u32 command_size = static_cast<u32>(m_GP0_buffer.size());
        do
        {
          const u32* prev_command_ptr = command_ptr;
          const bool result = HandleGP0Command(command_ptr, command_size);
          command_size -= command_ptr - prev_command_ptr;
          if (!result)
            break;
        } while (command_size > 0);

        if (command_size > 0 && command_size < m_GP0_buffer.size())
          m_GP0_buffer.erase(m_GP0_buffer.begin(), m_GP0_buffer.begin() + (m_GP0_buffer.size() - command_size));
        else if (command_size == 0)
          m_GP0_buffer.clear();
      }
      else
      {
        // fast path - read directly from DMA buffer
        const u32* command_ptr = words;
        u32 command_size = word_count;
        do
        {
          const u32* prev_command_ptr = command_ptr;
          const bool result = HandleGP0Command(command_ptr, command_size);
          command_size -= command_ptr - prev_command_ptr;
          if (!result)
            break;
        } while (command_size > 0);

        if (command_size > 0)
        {
          // partial command left over
          std::copy(command_ptr, command_ptr + command_size, std::back_inserter(m_GP0_buffer));
        }
      }

      UpdateGPUSTAT();
#else
      for (u32 i = 0; i < word_count; i++)
        WriteGP0(words[i]);
#endif
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

void GPU::UpdateCRTCConfig()
{
  static constexpr std::array<TickCount, 8> dot_clock_dividers = {{10, 8, 5, 4, 7, 7, 7, 7}};
  static constexpr std::array<u32, 8> horizontal_resolutions = {{256, 320, 512, 640, 368, 368, 368, 368}};
  static constexpr std::array<u32, 2> vertical_resolutions = {{240, 480}};
  CRTCState& cs = m_crtc_state;

  if (m_GPUSTAT.pal_mode)
  {
    cs.total_scanlines_per_frame = 314;
    cs.ticks_per_scanline = 3406;
  }
  else
  {
    cs.total_scanlines_per_frame = 263;
    cs.ticks_per_scanline = 3413;
  }

  const u8 horizontal_resolution_index = m_GPUSTAT.horizontal_resolution_1 | (m_GPUSTAT.horizontal_resolution_2 << 2);
  cs.dot_clock_divider = dot_clock_dividers[horizontal_resolution_index];
  cs.horizontal_resolution = horizontal_resolutions[horizontal_resolution_index];
  cs.vertical_resolution =
    vertical_resolutions[BoolToUInt8(m_GPUSTAT.vertical_interlace && m_GPUSTAT.vertical_resolution)];
  cs.visible_ticks_per_scanline = cs.regs.X2 - cs.regs.X1;
  cs.visible_scanlines_per_frame = cs.regs.Y2 - cs.regs.Y1;

  // check for a change in resolution
  const u32 old_horizontal_resolution = cs.display_width;
  const u32 old_vertical_resolution = cs.display_height;
  cs.display_width = std::max<u32>(cs.visible_ticks_per_scanline / cs.dot_clock_divider, 1);
  cs.display_height = cs.visible_scanlines_per_frame;

  if (cs.display_width != old_horizontal_resolution || cs.display_height != old_vertical_resolution)
    Log_InfoPrintf("Visible resolution is now %ux%u", cs.display_width, cs.display_height);

  // Compute the aspect ratio necessary to display borders in the inactive region of the picture.
  // Convert total dots/lines to time.
  const float dot_clock =
    (static_cast<float>(MASTER_CLOCK) * (11.0f / 7.0f / static_cast<float>(cs.dot_clock_divider)));
  const float dot_clock_period = 1.0f / dot_clock;
  const float dots_per_scanline = static_cast<float>(cs.ticks_per_scanline) / static_cast<float>(cs.dot_clock_divider);
  const float horizontal_period = dots_per_scanline * dot_clock_period;
  const float vertical_period = horizontal_period * static_cast<float>(cs.total_scanlines_per_frame);

  // Convert active dots/lines to time.
  const float visible_dots_per_scanline =
    static_cast<float>(cs.visible_ticks_per_scanline) / static_cast<float>(cs.dot_clock_divider);
  const float horizontal_active_time = horizontal_period * visible_dots_per_scanline;
  const float vertical_active_time = horizontal_active_time * static_cast<float>(cs.visible_scanlines_per_frame);

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

  UpdateSliceTicks();
}

void GPU::UpdateSliceTicks()
{
  // the next event is at the end of the next scanline
#if 1
  const TickCount ticks_until_next_event = m_crtc_state.ticks_per_scanline - m_crtc_state.current_tick_in_scanline;
#else
  // or at vblank. this will depend on the timer config..
  const TickCount ticks_until_next_event =
    ((m_crtc_state.total_scanlines_per_frame - m_crtc_state.current_scanline) * m_crtc_state.ticks_per_scanline) -
    m_crtc_state.current_tick_in_scanline;
#endif

  // convert to master clock, rounding up as we want to overshoot not undershoot
  const TickCount system_ticks = (ticks_until_next_event * 7 + 10) / 11;
  m_system->SetDowncount(system_ticks);
}

void GPU::Execute(TickCount ticks)
{
  // convert cpu/master clock to GPU ticks, accounting for partial cycles because of the non-integer divider
  {
    const TickCount temp = (ticks * 11) + m_crtc_state.fractional_ticks;
    m_crtc_state.current_tick_in_scanline += temp / 7;
    m_crtc_state.fractional_ticks = temp % 7;
  }

  while (m_crtc_state.current_tick_in_scanline >= m_crtc_state.ticks_per_scanline)
  {
    m_crtc_state.current_tick_in_scanline -= m_crtc_state.ticks_per_scanline;
    m_crtc_state.current_scanline++;
    if (m_timers->IsUsingExternalClock(HBLANK_TIMER_INDEX))
      m_timers->AddTicks(HBLANK_TIMER_INDEX, 1);

    // past the end of vblank?
    if (m_crtc_state.current_scanline >= m_crtc_state.total_scanlines_per_frame)
    {
      // flush any pending draws and "scan out" the image
      FlushRender();
      UpdateDisplay();

      // start the new frame
      m_system->IncrementFrameNumber();
      m_crtc_state.current_scanline = 0;

      if (m_GPUSTAT.vertical_interlace & m_GPUSTAT.vertical_resolution)
        m_GPUSTAT.drawing_even_line ^= true;
    }

    const bool old_vblank = m_crtc_state.in_vblank;
    const bool new_vblank = m_crtc_state.current_scanline >= m_crtc_state.visible_scanlines_per_frame;
    if (new_vblank != old_vblank)
    {
      m_crtc_state.in_vblank = new_vblank;

      if (!old_vblank)
      {
        Log_DebugPrintf("Now in v-blank");
        m_interrupt_controller->InterruptRequest(InterruptController::IRQ::VBLANK);
      }

      m_timers->SetGate(HBLANK_TIMER_INDEX, new_vblank);
    }

    // alternating even line bit in 240-line mode
    if (!(m_GPUSTAT.vertical_interlace & m_GPUSTAT.vertical_resolution))
      m_GPUSTAT.drawing_even_line = ConvertToBoolUnchecked(m_crtc_state.current_scanline & u32(1));
  }

  UpdateSliceTicks();
}

u32 GPU::ReadGPUREAD()
{
  if (m_GPUREAD_buffer.empty())
  {
    Log_DevPrintf("GPUREAD read while buffer is empty");
    return UINT32_C(0xFFFFFFFF);
  }

  const u32 value = m_GPUREAD_buffer.front();
  m_GPUREAD_buffer.pop_front();
  UpdateGPUSTAT();
  return value;
}

void GPU::WriteGP0(u32 value)
{
  m_GP0_buffer.push_back(value);
  Assert(m_GP0_buffer.size() <= 1048576);

  const u32* command_ptr = m_GP0_buffer.data();
  const u32 command = m_GP0_buffer[0] >> 24;
  if ((this->*s_GP0_command_handler_table[command])(command_ptr, static_cast<u32>(m_GP0_buffer.size())))
  {
    DebugAssert(static_cast<size_t>(command_ptr - m_GP0_buffer.data()) == m_GP0_buffer.size());
    m_GP0_buffer.clear();
  }

  UpdateGPUSTAT();
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
      m_crtc_state.regs.horizontal_display_range = param & CRTCState::Regs::HORIZONTAL_DISPLAY_RANGE_MASK;
      Log_DebugPrintf("Horizontal display range <- 0x%08X", m_crtc_state.regs.horizontal_display_range);
      UpdateCRTCConfig();
    }
    break;

    case 0x07: // Set display start address
    {
      m_crtc_state.regs.vertical_display_range = param & CRTCState::Regs::VERTICAL_DISPLAY_RANGE_MASK;
      Log_DebugPrintf("Vertical display range <- 0x%08X", m_crtc_state.regs.vertical_display_range);
      UpdateCRTCConfig();
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
      m_GPUSTAT.horizontal_resolution_1 = dm.horizontal_resolution_1;
      m_GPUSTAT.vertical_resolution = dm.vertical_resolution;
      m_GPUSTAT.pal_mode = dm.pal_mode;
      m_GPUSTAT.display_area_color_depth_24 = dm.display_area_color_depth;
      m_GPUSTAT.vertical_interlace = dm.vertical_interlace;
      m_GPUSTAT.horizontal_resolution_2 = dm.horizontal_resolution_2;
      m_GPUSTAT.reverse_flag = dm.reverse_flag;

      Log_DebugPrintf("Set display mode <- 0x%08X", dm.bits);
      UpdateCRTCConfig();
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
      m_GPUREAD_buffer.push_back(m_render_state.texture_window_value);
    }
    break;

    case 0x03: // Get Draw Area Top Left
    {
      Log_DebugPrintf("Get drawing area top left");
      m_GPUREAD_buffer.push_back((m_drawing_area.left & UINT32_C(0b1111111111)) |
                                 ((m_drawing_area.top & UINT32_C(0b1111111111)) << 10));
    }
    break;

    case 0x04: // Get Draw Area Bottom Right
    {
      Log_DebugPrintf("Get drawing area bottom right");
      m_GPUREAD_buffer.push_back((m_drawing_area.right & UINT32_C(0b1111111111)) |
                                 ((m_drawing_area.bottom & UINT32_C(0b1111111111)) << 10));
    }
    break;

    case 0x05: // Get Drawing Offset
    {
      Log_DebugPrintf("Get drawing offset");
      m_GPUREAD_buffer.push_back((m_drawing_offset.x & INT32_C(0b11111111111)) |
                                 ((m_drawing_offset.y & INT32_C(0b11111111111)) << 11));
    }
    break;

    default:
      Log_WarningPrintf("Unhandled GetGPUInfo(0x%02X)", ZeroExtend32(subcommand));
      break;
  }
}

void GPU::UpdateDisplay() {}

void GPU::UpdateDrawingArea() {}

void GPU::ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer) {}

void GPU::FillVRAM(u32 x, u32 y, u32 width, u32 height, u16 color) {}

void GPU::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) {}

void GPU::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) {}

void GPU::DispatchRenderCommand(RenderCommand rc, u32 num_vertices, const u32* command_ptr) {}

void GPU::FlushRender() {}

void GPU::RenderState::SetFromPolygonTexcoord(u32 texcoord0, u32 texcoord1)
{
  SetFromPaletteAttribute(Truncate16(texcoord0 >> 16));
  SetFromPageAttribute(Truncate16(texcoord1 >> 16));
}

void GPU::RenderState::SetFromRectangleTexcoord(u32 texcoord)
{
  SetFromPaletteAttribute(Truncate16(texcoord >> 16));
}

void GPU::RenderState::SetFromPageAttribute(u16 value)
{
  const u16 old_page_attribute = texpage_attribute;
  value &= PAGE_ATTRIBUTE_MASK;
  if (texpage_attribute == value)
    return;

  texpage_attribute = value;
  texture_page_x = static_cast<s32>(ZeroExtend32(value & UINT16_C(0x0F)) * UINT32_C(64));
  texture_page_y = static_cast<s32>(ZeroExtend32((value >> 4) & UINT16_C(1)) * UINT32_C(256));
  texture_page_changed |=
    (old_page_attribute & PAGE_ATTRIBUTE_TEXTURE_PAGE_MASK) != (value & PAGE_ATTRIBUTE_TEXTURE_PAGE_MASK);

  const TextureColorMode old_color_mode = texture_color_mode;
  texture_color_mode = (static_cast<TextureColorMode>((value >> 7) & UINT16_C(0x03)));
  if (texture_color_mode == TextureColorMode::Reserved_Direct16Bit)
    texture_color_mode = TextureColorMode::Direct16Bit;
  texture_color_mode_changed |= old_color_mode != texture_color_mode;

  const TransparencyMode old_transparency_mode = transparency_mode;
  transparency_mode = (static_cast<TransparencyMode>((value >> 5) & UINT16_C(0x03)));
  transparency_mode_changed = old_transparency_mode != transparency_mode;
}

void GPU::RenderState::SetFromPaletteAttribute(u16 value)
{
  value &= PALETTE_ATTRIBUTE_MASK;
  if (texlut_attribute == value)
    return;

  texture_palette_x = static_cast<s32>(ZeroExtend32(value & UINT16_C(0x3F)) * UINT32_C(16));
  texture_palette_y = static_cast<s32>(ZeroExtend32((value >> 6) & UINT16_C(0x1FF)));
  texlut_attribute = value;
  texture_page_changed = true;
}

void GPU::RenderState::SetTextureWindow(u32 value)
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
  if (!ImGui::Begin("GPU State", &m_system->GetSettings().debugging.show_gpu_state))
  {
    ImGui::End();
    return;
  }

  if (ImGui::CollapsingHeader("CRTC", ImGuiTreeNodeFlags_DefaultOpen))
  {
    const auto& cs = m_crtc_state;
    ImGui::Text("Resolution: %ux%u", cs.horizontal_resolution, cs.vertical_resolution);
    ImGui::Text("Dot Clock Divider: %u", cs.dot_clock_divider);
    ImGui::Text("Vertical Interlace: %s (%s field)", m_GPUSTAT.vertical_interlace ? "Yes" : "No",
                m_GPUSTAT.interlaced_field ? "odd" : "even");
    ImGui::Text("Display Enable: %s", m_GPUSTAT.display_enable ? "Yes" : "No");
    ImGui::Text("Drawing Even Line: %s", m_GPUSTAT.drawing_even_line ? "Yes" : "No");
    ImGui::NewLine();

    ImGui::Text("Color Depth: %u-bit", m_GPUSTAT.display_area_color_depth_24 ? 24 : 15);
    ImGui::Text("Start Offset: (%u, %u)", cs.regs.X.GetValue(), cs.regs.Y.GetValue());
    ImGui::Text("Display Range: %u-%u, %u-%u", cs.regs.X1.GetValue(), cs.regs.X2.GetValue(), cs.regs.Y1.GetValue(),
                cs.regs.Y2.GetValue());
    ImGui::NewLine();

    ImGui::Text("Display Resolution: %ux%u", cs.display_width, cs.display_height);
    ImGui::Text("Ticks Per Scanline: %u (%u visible)", cs.ticks_per_scanline, cs.visible_ticks_per_scanline);
    ImGui::Text("Scanlines Per Frame: %u (%u visible)", cs.total_scanlines_per_frame, cs.visible_scanlines_per_frame);
    ImGui::Text("Current Scanline: %u (tick %u)", cs.current_scanline, cs.current_tick_in_scanline);
    ImGui::Text("Horizontal Blank: %s", cs.in_hblank ? "Yes" : "No");
    ImGui::Text("Vertical Blank: %s", cs.in_vblank ? "Yes" : "No");
  }

  if (ImGui::CollapsingHeader("GPU", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::Text("Dither: %s", m_GPUSTAT.dither_enable ? "Enabled" : "Disabled");
    ImGui::Text("Draw To Display Area: %s", m_GPUSTAT.dither_enable ? "Yes" : "No");
    ImGui::Text("Draw Set Mask Bit: %s", m_GPUSTAT.draw_set_mask_bit ? "Yes" : "No");
    ImGui::Text("Draw To Masked Pixels: %s", m_GPUSTAT.draw_to_masked_pixels ? "Yes" : "No");
    ImGui::Text("Reverse Flag: %s", m_GPUSTAT.reverse_flag ? "Yes" : "No");
    ImGui::Text("Texture Disable: %s", m_GPUSTAT.texture_disable ? "Yes" : "No");
    ImGui::Text("PAL Mode: %s", m_GPUSTAT.pal_mode ? "Yes" : "No");
    ImGui::Text("Interrupt Request: %s", m_GPUSTAT.interrupt_request ? "Yes" : "No");
    ImGui::Text("DMA Request: %s", m_GPUSTAT.dma_data_request ? "Yes" : "No");
  }

  ImGui::End();
}

void GPU::DrawRendererStatsWindow() {}
