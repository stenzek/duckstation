#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include "gpu.h"
#include "interrupt_controller.h"
Log_SetChannel(GPU);

static u32 s_cpu_to_vram_dump_id = 1;
static u32 s_vram_to_cpu_dump_id = 1;

static constexpr u32 ReplaceZero(u32 value, u32 value_for_zero)
{
  return value == 0 ? value_for_zero : value;
}

GPU::GP0CommandHandlerTable GPU::GenerateGP0CommandHandlerTable()
{
  GP0CommandHandlerTable table = {};
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
    table[i] = &GPU::HandleRenderCommand;
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

  return table;
}

bool GPU::HandleUnknownGP0Command(const u32*& command_ptr, u32 command_size)
{
  const u32 command = *(command_ptr++) >> 24;
  Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);
  return true;
}

bool GPU::HandleNOPCommand(const u32*& command_ptr, u32 command_size)
{
  command_ptr++;
  return true;
}

bool GPU::HandleClearCacheCommand(const u32*& command_ptr, u32 command_size)
{
  Log_DebugPrintf("GP0 clear cache");
  command_ptr++;
  return true;
}

bool GPU::HandleInterruptRequestCommand(const u32*& command_ptr, u32 command_size)
{
  Log_WarningPrintf("GP0 interrupt request");
  if (!m_GPUSTAT.interrupt_request)
  {
    m_GPUSTAT.interrupt_request = true;
    m_interrupt_controller->InterruptRequest(InterruptController::IRQ::GPU);
  }

  return true;
}

bool GPU::HandleSetDrawModeCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;

  // 0..10 bits match GPUSTAT
  const u32 MASK = ((1 << 11) - 1);
  m_GPUSTAT.bits = (m_GPUSTAT.bits & ~MASK) | (param & MASK);
  m_GPUSTAT.texture_disable = (param & (1 << 11)) != 0;
  m_render_state.texture_x_flip = (param & (1 << 12)) != 0;
  m_render_state.texture_y_flip = (param & (1 << 13)) != 0;
  Log_DebugPrintf("Set draw mode %08X", param);
  return true;
}

bool GPU::HandleSetTextureWindowCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;
  m_render_state.SetTextureWindow(param);
  Log_DebugPrintf("Set texture window %02X %02X %02X %02X", m_render_state.texture_window_mask_x,
                  m_render_state.texture_window_mask_y, m_render_state.texture_window_offset_x,
                  m_render_state.texture_window_offset_y);
  return true;
}

bool GPU::HandleSetDrawingAreaTopLeftCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;
  const u32 left = param & 0x3FF;
  const u32 top = (param >> 10) & 0x1FF;
  Log_DebugPrintf("Set drawing area top-left: (%u, %u)", left, top);
  if (m_drawing_area.left != left || m_drawing_area.top != top)
  {
    FlushRender();

    m_drawing_area.left = left;
    m_drawing_area.top = top;
    UpdateDrawingArea();
  }

  return true;
}

bool GPU::HandleSetDrawingAreaBottomRightCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;

  const u32 right = param & 0x3FF;
  const u32 bottom = (param >> 10) & 0x1FF;
  Log_DebugPrintf("Set drawing area bottom-right: (%u, %u)", m_drawing_area.right, m_drawing_area.bottom);
  if (m_drawing_area.right != right || m_drawing_area.bottom != bottom)
  {
    FlushRender();

    m_drawing_area.right = right;
    m_drawing_area.bottom = bottom;
    UpdateDrawingArea();
  }

  return true;
}

bool GPU::HandleSetDrawingOffsetCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;
  const s32 x = SignExtendN<11, s32>(param & 0x7FF);
  const s32 y = SignExtendN<11, s32>((param >> 11) & 0x7FF);
  Log_DebugPrintf("Set drawing offset (%d, %d)", m_drawing_offset.x, m_drawing_offset.y);
  if (m_drawing_offset.x != x || m_drawing_offset.y != y)
  {
    FlushRender();

    m_drawing_offset.x = x;
    m_drawing_offset.y = y;
  }
  return true;
}

bool GPU::HandleSetMaskBitCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;

  m_GPUSTAT.draw_set_mask_bit = (param & 0x01) != 0;
  m_GPUSTAT.draw_to_masked_pixels = (param & 0x01) != 0;
  Log_DebugPrintf("Set mask bit %u %u", BoolToUInt32(m_GPUSTAT.draw_set_mask_bit),
                  BoolToUInt32(m_GPUSTAT.draw_to_masked_pixels));
  return true;
}

bool GPU::HandleRenderCommand(const u32*& command_ptr, u32 command_size)
{
  const RenderCommand rc{command_ptr[0]};
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
        for (u32 pos = 1 + BoolToUInt32(!rc.shading_enable); pos < command_size; pos += words_per_vertex)
        {
          if (command_ptr[pos] == 0x55555555)
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

  if (command_size < total_words)
    return false;

  static constexpr std::array<const char*, 4> primitive_names = {{"", "polygon", "line", "rectangle"}};

  Log_DebugPrintf("Render %s %s %s %s %s (%u verts, %u words per vert)", rc.quad_polygon ? "four-point" : "three-point",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  primitive_names[static_cast<u8>(rc.primitive.GetValue())], ZeroExtend32(num_vertices),
                  ZeroExtend32(words_per_vertex));

  DispatchRenderCommand(rc, num_vertices, command_ptr);
  command_ptr += total_words;
  return true;
}

bool GPU::HandleFillRectangleCommand(const u32*& command_ptr, u32 command_size)
{
  if (command_size < 3)
    return false;

  FlushRender();

  const u32 color = command_ptr[0] & 0x00FFFFFF;
  const u32 dst_x = command_ptr[1] & 0x3F0;
  const u32 dst_y = (command_ptr[1] >> 16) & 0x3FF;
  const u32 width = ((command_ptr[2] & 0x3FF) + 0xF) & ~0xF;
  const u32 height = (command_ptr[2] >> 16) & 0x1FF;
  command_ptr += 3;

  Log_DebugPrintf("Fill VRAM rectangle offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, width, height);

  // Drop higher precision when filling. Bit15 is set to 0.
  // TODO: Force 8-bit color option.
  const u16 color16 = RGBA8888ToRGBA5551(color);

  FillVRAM(dst_x, dst_y, width, height, color16);
  return true;
}

bool GPU::HandleCopyRectangleCPUToVRAMCommand(const u32*& command_ptr, u32 command_size)
{
  if (command_size < 3)
    return false;

  const u32 copy_width = ReplaceZero(command_ptr[2] & 0x3FF, 0x400);
  const u32 copy_height = ReplaceZero((command_ptr[2] >> 16) & 0x1FF, 0x200);
  const u32 num_pixels = copy_width * copy_height;
  const u32 num_words = 3 + ((num_pixels + 1) / 2);
  if (command_size < num_words)
    return false;

  const u32 dst_x = command_ptr[1] & 0x3FF;
  const u32 dst_y = (command_ptr[1] >> 16) & 0x3FF;

  Log_DebugPrintf("Copy rectangle from CPU to VRAM offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, copy_width,
                  copy_height);

  if ((dst_x + copy_width) > VRAM_WIDTH || (dst_y + copy_height) > VRAM_HEIGHT)
  {
    Log_ErrorPrintf("Out of bounds CPU->VRAM copy (%u,%u) @ (%u,%u)", copy_width, copy_height, dst_x, dst_y);
    command_ptr += num_words;
    return true;
  }

  if (m_debug_options.dump_cpu_to_vram_copies)
  {
    DumpVRAMToFile(SmallString::FromFormat("cpu_to_vram_copy_%u.png", s_cpu_to_vram_dump_id++), copy_width, copy_height,
                   sizeof(u16) * copy_width, &command_ptr[3], true);
  }

  FlushRender();
  UpdateVRAM(dst_x, dst_y, copy_width, copy_height, &command_ptr[3]);
  command_ptr += num_words;
  return true;
}

bool GPU::HandleCopyRectangleVRAMToCPUCommand(const u32*& command_ptr, u32 command_size)
{
  if (command_size < 3)
    return false;

  const u32 width = ReplaceZero(command_ptr[2] & 0x3FF, 0x400);
  const u32 height = ReplaceZero((command_ptr[2] >> 16) & 0x1FF, 0x200);
  const u32 num_pixels = width * height;
  const u32 num_words = ((num_pixels + 1) / 2);
  const u32 src_x = command_ptr[1] & 0x3FF;
  const u32 src_y = (command_ptr[1] >> 16) & 0x3FF;
  command_ptr += 3;

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

  if (m_debug_options.dump_vram_to_cpu_copies)
  {
    DumpVRAMToFile(SmallString::FromFormat("vram_to_cpu_copy_%u.png", s_vram_to_cpu_dump_id++), width, height,
                   sizeof(u16) * width, temp.data(), true);
  }

  // Is this correct?
  return true;
}

bool GPU::HandleCopyRectangleVRAMToVRAMCommand(const u32*& command_ptr, u32 command_size)
{
  if (command_size < 4)
    return false;

  const u32 src_x = command_ptr[1] & 0x3FF;
  const u32 src_y = (command_ptr[1] >> 16) & 0x3FF;
  const u32 dst_x = command_ptr[2] & 0x3FF;
  const u32 dst_y = (command_ptr[2] >> 16) & 0x3FF;
  const u32 width = ReplaceZero(command_ptr[3] & 0x3FF, 0x400);
  const u32 height = ReplaceZero((command_ptr[3] >> 16) & 0x1FF, 0x200);
  command_ptr += 4;

  Log_DebugPrintf("Copy rectangle from VRAM to VRAM src=(%u,%u), dst=(%u,%u), size=(%u,%u)", src_x, src_y, dst_x, dst_y,
                  width, height);

  if ((src_x + width) > VRAM_WIDTH || (src_y + height) > VRAM_HEIGHT || (dst_x + width) > VRAM_WIDTH ||
      (dst_y + height) > VRAM_HEIGHT)
  {
    Panic("Out of bounds VRAM copy");
    return true;
  }

  FlushRender();
  CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);
  return true;
}
