#include "gpu.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "dma.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(GPU);

GPU::GPU() = default;

GPU::~GPU() = default;

bool GPU::Initialize(System* system, DMA* dma, InterruptController* interrupt_controller)
{
  m_system = system;
  m_dma = dma;
  m_interrupt_controller = interrupt_controller;
  return true;
}

void GPU::Reset()
{
  SoftReset();
}

void GPU::SoftReset()
{
  m_GPUSTAT.bits = 0x14802000;
  m_crtc_state = {};
  m_crtc_state.regs.display_address_start = 0;
  m_crtc_state.regs.horizontal_display_range = 0xC60260;
  m_crtc_state.regs.vertical_display_range = 0x3FC10;
  UpdateGPUSTAT();
  UpdateCRTCConfig();
}

bool GPU::DoState(StateWrapper& sw)
{
  if (sw.IsReading())
    FlushRender();

  sw.Do(&m_GPUSTAT.bits);
  sw.Do(&m_texture_config.base_x);
  sw.Do(&m_texture_config.base_y);
  sw.Do(&m_texture_config.palette_x);
  sw.Do(&m_texture_config.palette_y);
  sw.Do(&m_texture_config.page_attribute);
  sw.Do(&m_texture_config.palette_attribute);
  sw.Do(&m_texture_config.color_mode);
  sw.Do(&m_texture_config.page_changed);
  sw.Do(&m_texture_config.window_mask_x);
  sw.Do(&m_texture_config.window_mask_y);
  sw.Do(&m_texture_config.window_offset_x);
  sw.Do(&m_texture_config.window_offset_y);
  sw.Do(&m_texture_config.x_flip);
  sw.Do(&m_texture_config.y_flip);
  sw.Do(&m_drawing_area.top_left_x);
  sw.Do(&m_drawing_area.top_left_y);
  sw.Do(&m_drawing_area.bottom_right_x);
  sw.Do(&m_drawing_area.bottom_right_y);
  sw.Do(&m_drawing_offset.x);
  sw.Do(&m_drawing_offset.y);
  sw.Do(&m_drawing_offset.x);

  sw.Do(&m_crtc_state.regs.display_address_start);
  sw.Do(&m_crtc_state.regs.horizontal_display_range);
  sw.Do(&m_crtc_state.regs.vertical_display_range);
  sw.Do(&m_crtc_state.horizontal_resolution);
  sw.Do(&m_crtc_state.vertical_resolution);
  sw.Do(&m_crtc_state.dot_clock_divider);
  sw.Do(&m_crtc_state.visible_horizontal_resolution);
  sw.Do(&m_crtc_state.visible_vertical_resolution);
  sw.Do(&m_crtc_state.ticks_per_scanline);
  sw.Do(&m_crtc_state.visible_ticks_per_scanline);
  sw.Do(&m_crtc_state.total_scanlines_per_frame);
  sw.Do(&m_crtc_state.fractional_ticks);
  sw.Do(&m_crtc_state.current_tick_in_scanline);
  sw.Do(&m_crtc_state.current_scanline);
  sw.Do(&m_crtc_state.in_hblank);
  sw.Do(&m_crtc_state.in_vblank);

  if (sw.IsReading())
    UpdateSliceTicks();

  sw.Do(&m_GP0_command);
  sw.Do(&m_GPUREAD_buffer);

  if (sw.IsReading())
  {
    m_texture_config.page_changed = true;
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
      // bits &= (BoolToUInt32(!m_crtc_state.in_vblank) << 31);
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

u32 GPU::DMARead()
{
  if (m_GPUSTAT.dma_direction != DMADirection::GPUREADtoCPU)
  {
    Log_ErrorPrintf("Invalid DMA direction from GPU DMA read");
    return UINT32_C(0xFFFFFFFF);
  }

  return ReadGPUREAD();
}

void GPU::DMAWrite(u32 value)
{
  switch (m_GPUSTAT.dma_direction)
  {
    case DMADirection::CPUtoGP0:
      WriteGP0(value);
      break;

    default:
      Log_ErrorPrintf("Unhandled GPU DMA write mode %u for value %08X",
                      static_cast<u32>(m_GPUSTAT.dma_direction.GetValue()), value);
      break;
  }
}

void GPU::UpdateCRTCConfig()
{
  static constexpr std::array<TickCount, 8> dot_clock_dividers = {{8, 4, 10, 5, 7, 7, 7, 7}};
  static constexpr std::array<u32, 8> horizontal_resolutions = {{256, 320, 512, 630, 368, 368, 368, 368}};
  static constexpr std::array<u32, 2> vertical_resolutions = {{240, 480}};
  CRTCState& cs = m_crtc_state;

  const u8 horizontal_resolution_index = m_GPUSTAT.horizontal_resolution_1 | (m_GPUSTAT.horizontal_resolution_2 << 2);
  cs.dot_clock_divider = dot_clock_dividers[horizontal_resolution_index];
  cs.horizontal_resolution = horizontal_resolutions[horizontal_resolution_index];
  cs.vertical_resolution = vertical_resolutions[m_GPUSTAT.vertical_resolution];

  // check for a change in resolution
  const u32 old_horizontal_resolution = cs.visible_horizontal_resolution;
  const u32 old_vertical_resolution = cs.visible_vertical_resolution;
  cs.visible_horizontal_resolution = std::max((cs.regs.X2 - cs.regs.X1) / cs.dot_clock_divider, u32(1));
  cs.visible_vertical_resolution = cs.regs.Y2 - cs.regs.Y1 + 1;
  if (cs.visible_horizontal_resolution != old_horizontal_resolution ||
      cs.visible_vertical_resolution != old_vertical_resolution)
  {
    Log_InfoPrintf("Visible resolution is now %ux%u", cs.visible_horizontal_resolution, cs.visible_vertical_resolution);
  }

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

  UpdateSliceTicks();
}

void GPU::UpdateSliceTicks()
{
  // the next event is at the end of the next scanline
  // const TickCount ticks_until_next_event = m_crtc_state.ticks_per_scanline - m_crtc_state.current_tick_in_scanline;

  // or at vblank. this will depend on the timer config..
  const TickCount ticks_until_next_event =
    ((m_crtc_state.total_scanlines_per_frame - m_crtc_state.current_scanline) * m_crtc_state.ticks_per_scanline) -
    m_crtc_state.current_tick_in_scanline;

  // convert to master clock, rounding up as we want to overshoot not undershoot
  const TickCount system_ticks = (ticks_until_next_event * 7 + 10) / 11;
  m_system->SetSliceTicks(system_ticks);
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

    const bool old_vblank = m_crtc_state.in_vblank;
    m_crtc_state.in_vblank = m_crtc_state.current_scanline >= m_crtc_state.visible_vertical_resolution;
    if (m_crtc_state.in_vblank && !old_vblank)
    {
      Log_DebugPrintf("Now in v-blank");
      m_interrupt_controller->InterruptRequest(InterruptController::IRQ::VBLANK);
    }

    // past the end of vblank?
    if (m_crtc_state.current_scanline >= m_crtc_state.total_scanlines_per_frame)
    {
      // flush any pending draws and "scan out" the image
      FlushRender();
      UpdateDisplay();

      // start the new frame
      m_system->IncrementFrameNumber();
      m_crtc_state.current_scanline = 0;
      m_crtc_state.in_hblank = false;
      m_crtc_state.in_vblank = false;

      if (m_GPUSTAT.vertical_resolution)
        m_GPUSTAT.drawing_even_line ^= true;
    }

    // alternating even line bit in 240-line mode
    if (!m_crtc_state.vertical_resolution)
      m_GPUSTAT.drawing_even_line = ConvertToBoolUnchecked(m_crtc_state.current_scanline & u32(1));
  }

  UpdateSliceTicks();
}

u32 GPU::ReadGPUREAD()
{
  if (m_GPUREAD_buffer.empty())
  {
    Log_ErrorPrintf("GPUREAD read while buffer is empty");
    return UINT32_C(0xFFFFFFFF);
  }

  const u32 value = m_GPUREAD_buffer.front();
  m_GPUREAD_buffer.pop_front();
  UpdateGPUSTAT();
  return value;
}

void GPU::WriteGP0(u32 value)
{
  m_GP0_command.push_back(value);
  Assert(m_GP0_command.size() <= 1048576);

  const u8 command = Truncate8(m_GP0_command[0] >> 24);
  const u32 param = m_GP0_command[0] & UINT32_C(0x00FFFFFF);
  UpdateGPUSTAT();

  if (command >= 0x20 && command <= 0x7F)
  {
    // Draw polygon
    if (!HandleRenderCommand())
      return;
  }
  else
  {
    switch (command)
    {
      case 0x00: // NOP
        break;

      case 0x01: // Clear cache
        break;

      case 0x02: // Fill Rectangle
      {
        if (!HandleFillRectangleCommand())
          return;
      }
      break;

      case 0xA0: // Copy Rectangle CPU->VRAM
      {
        if (!HandleCopyRectangleCPUToVRAMCommand())
          return;
      }
      break;

      case 0xC0: // Copy Rectangle VRAM->CPU
      {
        if (!HandleCopyRectangleVRAMToCPUCommand())
          return;
      }
      break;

      case 0x80: // Copy Rectangle VRAM->VRAM
      {
        if (!HandleCopyRectangleVRAMToVRAMCommand())
          return;
      }
      break;

      case 0xE1: // Set draw mode
      {
        // 0..10 bits match GPUSTAT
        const u32 MASK = ((UINT32_C(1) << 11) - 1);
        m_GPUSTAT.bits = (m_GPUSTAT.bits & ~MASK) | param & MASK;
        m_GPUSTAT.texture_disable = (param & (UINT32_C(1) << 11)) != 0;
        m_texture_config.x_flip = (param & (UINT32_C(1) << 12)) != 0;
        m_texture_config.y_flip = (param & (UINT32_C(1) << 13)) != 0;
        m_texture_config.SetColorMode(m_GPUSTAT.texture_color_mode);
        Log_DebugPrintf("Set draw mode %08X", param);
      }
      break;

      case 0xE2: // set texture window
      {
        m_texture_config.window_mask_x = param & UINT32_C(0x1F);
        m_texture_config.window_mask_y = (param >> 5) & UINT32_C(0x1F);
        m_texture_config.window_offset_x = (param >> 10) & UINT32_C(0x1F);
        m_texture_config.window_offset_y = (param >> 15) & UINT32_C(0x1F);
        Log_DebugPrintf("Set texture window %02X %02X %02X %02X", m_texture_config.window_mask_x,
                        m_texture_config.window_mask_y, m_texture_config.window_offset_x,
                        m_texture_config.window_offset_y);
      }
      break;

      case 0xE3: // Set drawing area top left
      {
        m_drawing_area.top_left_x = param & UINT32_C(0x3FF);
        m_drawing_area.top_left_y = (param >> 10) & UINT32_C(0x1FF);
        Log_DebugPrintf("Set drawing area top-left: (%u, %u)", m_drawing_area.top_left_x, m_drawing_area.top_left_y);
      }
      break;

      case 0xE4: // Set drawing area bottom right
      {
        m_drawing_area.bottom_right_x = param & UINT32_C(0x3FF);
        m_drawing_area.bottom_right_y = (param >> 10) & UINT32_C(0x1FF);
        Log_DebugPrintf("Set drawing area bottom-right: (%u, %u)", m_drawing_area.bottom_right_x,
                        m_drawing_area.bottom_right_y);
      }
      break;

      case 0xE5: // Set drawing offset
      {
        m_drawing_offset.x = S11ToS32(param & UINT32_C(0x7FF));
        m_drawing_offset.y = S11ToS32((param >> 11) & UINT32_C(0x7FF));
        Log_DebugPrintf("Set drawing offset (%d, %d)", m_drawing_offset.x, m_drawing_offset.y);
      }
      break;

      case 0xE6: // Mask bit setting
      {
        m_GPUSTAT.draw_set_mask_bit = (param & UINT32_C(0x01)) != 0;
        m_GPUSTAT.draw_to_masked_pixels = (param & UINT32_C(0x01)) != 0;
        Log_DebugPrintf("Set mask bit %u %u", BoolToUInt32(m_GPUSTAT.draw_set_mask_bit),
                        BoolToUInt32(m_GPUSTAT.draw_to_masked_pixels));
      }
      break;

      default:
      {
        Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);
      }
      break;
    }
  }

  m_GP0_command.clear();
  UpdateGPUSTAT();
}

void GPU::WriteGP1(u32 value)
{
  const u8 command = Truncate8(value >> 24);
  const u32 param = value & UINT32_C(0x00FFFFFF);
  switch (command)
  {
    case 0x01: // Clear FIFO
    {
      m_GP0_command.clear();
      Log_DebugPrintf("GP1 clear FIFO");
      UpdateGPUSTAT();
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
        BitField<u32, u8, 2, 1> vertical_resolution;
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

    default:
      Log_ErrorPrintf("Unimplemented GP1 command 0x%02X", command);
      break;
  }
}

bool GPU::HandleRenderCommand()
{
  const u8 command = Truncate8(m_GP0_command[0] >> 24);

  const RenderCommand rc{m_GP0_command[0]};
  u8 words_per_vertex;
  u32 num_vertices;
  u32 total_words;
  switch (rc.primitive)
  {
    case Primitive::Polygon:
    {
      // shaded vertices use the colour from the first word for the first vertex
      words_per_vertex = 1 + BoolToUInt8(rc.texture_enable) + BoolToUInt8(rc.shading_enable);
      num_vertices = rc.quad_polygon ? 4 : 3;
      total_words = words_per_vertex * num_vertices + BoolToUInt8(!rc.shading_enable);
    }
    break;

    case Primitive::Line:
    {
      words_per_vertex = 1 + BoolToUInt8(rc.shading_enable);
      if (rc.polyline)
      {
        // polyline goes until we hit the termination code
        num_vertices = 0;
        bool found_terminator = false;
        for (size_t pos = 0; pos < m_GP0_command.size(); pos += words_per_vertex)
        {
          if (m_GP0_command[pos] == 0x55555555)
          {
            found_terminator = true;
            break;
          }

          num_vertices++;
        }
        if (!found_terminator)
          return false;
      }
      else
      {
        num_vertices = 2;
      }

      total_words = words_per_vertex * num_vertices + BoolToUInt8(!rc.shading_enable);
    }
    break;

    case Primitive::Rectangle:
    {
      words_per_vertex =
        2 + BoolToUInt8(rc.texture_enable) + BoolToUInt8(rc.rectangle_size == DrawRectangleSize::Variable);
      num_vertices = 1;
      total_words = words_per_vertex;
    }
    break;

    default:
      UnreachableCode();
      return true;
  }

  if (m_GP0_command.size() < total_words)
    return false;

  static constexpr std::array<const char*, 4> primitive_names = {{"", "polygon", "line", "rectangle"}};

  Log_DebugPrintf("Render %s %s %s %s %s (%u verts, %u words per vert)", rc.quad_polygon ? "four-point" : "three-point",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  primitive_names[static_cast<u8>(rc.primitive.GetValue())], ZeroExtend32(num_vertices),
                  ZeroExtend32(words_per_vertex));

  DispatchRenderCommand(rc, num_vertices);
  return true;
}

bool GPU::HandleFillRectangleCommand()
{
  if (m_GP0_command.size() < 3)
    return false;

  const u32 color = (m_GP0_command[0] & UINT32_C(0x00FFFFFF)) | UINT32_C(0xFF000000);
  const u32 dst_x = m_GP0_command[1] & UINT32_C(0xFFFF);
  const u32 dst_y = m_GP0_command[1] >> 16;
  const u32 width = m_GP0_command[2] & UINT32_C(0xFFFF);
  const u32 height = m_GP0_command[2] >> 16;

  Log_DebugPrintf("Fill VRAM rectangle offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, width, height);

  FillVRAM(dst_x, dst_y, width, height, color);
  return true;
}

bool GPU::HandleCopyRectangleCPUToVRAMCommand()
{
  if (m_GP0_command.size() < 3)
    return false;

  const u32 copy_width = m_GP0_command[2] & UINT32_C(0xFFFF);
  const u32 copy_height = m_GP0_command[2] >> 16;
  const u32 num_pixels = copy_width * copy_height;
  const u32 num_words = 3 + ((num_pixels + 1) / 2);
  if (m_GP0_command.size() < num_words)
    return false;

  const u32 dst_x = m_GP0_command[1] & UINT32_C(0xFFFF);
  const u32 dst_y = m_GP0_command[1] >> 16;

  Log_DebugPrintf("Copy rectangle from CPU to VRAM offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, copy_width,
                  copy_height);

  if ((dst_x + copy_width) > VRAM_WIDTH || (dst_y + copy_height) > VRAM_HEIGHT)
  {
    Panic("Out of bounds VRAM copy");
    return true;
  }

  FlushRender();
  UpdateVRAM(dst_x, dst_y, copy_width, copy_height, &m_GP0_command[3]);
  return true;
}

bool GPU::HandleCopyRectangleVRAMToCPUCommand()
{
  if (m_GP0_command.size() < 3)
    return false;

  const u32 width = m_GP0_command[2] & UINT32_C(0xFFFF);
  const u32 height = m_GP0_command[2] >> 16;
  const u32 num_pixels = width * height;
  const u32 num_words = ((num_pixels + 1) / 2);
  const u32 src_x = m_GP0_command[1] & UINT32_C(0xFFFF);
  const u32 src_y = m_GP0_command[1] >> 16;

  Log_DebugPrintf("Copy rectangle from VRAM to CPU offset=(%u,%u), size=(%u,%u)", src_x, src_y, width, height);

  if ((src_x + width) > VRAM_WIDTH || (src_y + height) > VRAM_HEIGHT)
  {
    Panic("Out of bounds VRAM copy");
    return true;
  }

  // all rendering should be done first...
  FlushRender();

  // TODO: A better way of doing this..
  std::vector<u32> temp(num_words);
  ReadVRAM(src_x, src_y, width, height, temp.data());
  for (const u32 bits : temp)
    m_GPUREAD_buffer.push_back(bits);

  // Is this correct?
  return true;
}

bool GPU::HandleCopyRectangleVRAMToVRAMCommand()
{
  if (m_GP0_command.size() < 4)
    return false;

  const u32 src_x = m_GP0_command[1] & UINT32_C(0xFFFF);
  const u32 src_y = m_GP0_command[1] >> 16;
  const u32 dst_x = m_GP0_command[2] & UINT32_C(0xFFFF);
  const u32 dst_y = m_GP0_command[2] >> 16;
  const u32 width = m_GP0_command[3] & UINT32_C(0xFFFF);
  const u32 height = m_GP0_command[3] >> 16;

  Log_DebugPrintf("Copy rectangle from VRAM to VRAM src=(%u,%u), dst=(%u,%u), size=(%u,%u)", src_x, src_y, dst_x, dst_y,
                  width, height);

  if ((src_x + width) > VRAM_WIDTH || (src_y + height) > VRAM_HEIGHT || (dst_x + width) > VRAM_WIDTH ||
      (dst_y + height) > VRAM_HEIGHT)
  {
    Panic("Out of bounds VRAM copy");
    return true;
  }

  CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);
  return true;
}

void GPU::UpdateDisplay()
{
  m_texture_config.page_changed = true;
  m_system->IncrementFrameNumber();
}

void GPU::ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer) {}

void GPU::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color) {}

void GPU::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data) {}

void GPU::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height) {}

void GPU::DispatchRenderCommand(RenderCommand rc, u32 num_vertices) {}

void GPU::FlushRender() {}

void GPU::TextureConfig::SetColorMode(TextureColorMode new_color_mode)
{
  if (new_color_mode == TextureColorMode::Reserved_Direct16Bit)
    new_color_mode = TextureColorMode::Direct16Bit;

  if (color_mode == new_color_mode)
    return;

  color_mode = new_color_mode;
  page_changed = true;
}

void GPU::TextureConfig::SetFromPolygonTexcoord(u32 texcoord0, u32 texcoord1)
{
  SetFromPaletteAttribute(Truncate16(texcoord0 >> 16));
  SetFromPageAttribute(Truncate16(texcoord1 >> 16));
}

void GPU::TextureConfig::SetFromRectangleTexcoord(u32 texcoord)
{
  SetFromPaletteAttribute(Truncate16(texcoord >> 16));
}

void GPU::TextureConfig::SetFromPageAttribute(u16 value)
{
  value &= PAGE_ATTRIBUTE_MASK;
  if (page_attribute == value)
    return;

  base_x = static_cast<s32>(ZeroExtend32(value & UINT16_C(0x0F)) * UINT32_C(64));
  base_y = static_cast<s32>(ZeroExtend32((value >> 11) & UINT16_C(1)) * UINT32_C(512));
  page_attribute = value;
  page_changed = true;
}

void GPU::TextureConfig::SetFromPaletteAttribute(u16 value)
{
  value &= PALETTE_ATTRIBUTE_MASK;
  if (palette_attribute == value)
    return;

  palette_x = static_cast<s32>(ZeroExtend32(value & UINT16_C(0x3F)) * UINT32_C(16));
  palette_y = static_cast<s32>(ZeroExtend32((value >> 6) & UINT16_C(0x1FF)));
  palette_attribute = value;
  page_changed = true;
}
