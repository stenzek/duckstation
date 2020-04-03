#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(GPU);

#define CHECK_COMMAND_SIZE(num_words)                                                                                  \
  if (command_size < num_words)                                                                                        \
  {                                                                                                                    \
    m_command_total_words = num_words;                                                                                 \
    m_state = State::WaitingForParameters;                                                                             \
    return false;                                                                                                      \
  }

static u32 s_cpu_to_vram_dump_id = 1;
static u32 s_vram_to_cpu_dump_id = 1;

static constexpr u32 ReplaceZero(u32 value, u32 value_for_zero)
{
  return value == 0 ? value_for_zero : value;
}

void GPU::ExecuteCommands()
{
  Assert(m_GP0_buffer.size() < 1048576);

  const u32* command_ptr = m_GP0_buffer.data();
  u32 command_size = static_cast<u32>(m_GP0_buffer.size());
  while (m_state != State::ReadingVRAM && command_size > 0 && command_size >= m_command_total_words)
  {
    const u32 command = command_ptr[0] >> 24;
    const u32* old_command_ptr = command_ptr;
    if (!(this->*s_GP0_command_handler_table[command])(command_ptr, command_size))
      break;

    const u32 words_used = static_cast<u32>(command_ptr - old_command_ptr);
    DebugAssert(words_used <= command_size);
    command_size -= words_used;
  }

  if (command_size == 0)
    m_GP0_buffer.clear();
  else if (command_ptr > m_GP0_buffer.data())
    m_GP0_buffer.erase(m_GP0_buffer.begin(), m_GP0_buffer.begin() + (command_ptr - m_GP0_buffer.data()));

  UpdateDMARequest();
}

void GPU::EndCommand()
{
  m_state = State::Idle;
  m_command_total_words = 0;
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
  EndCommand();
  return true;
}

bool GPU::HandleNOPCommand(const u32*& command_ptr, u32 command_size)
{
  command_ptr++;
  EndCommand();
  return true;
}

bool GPU::HandleClearCacheCommand(const u32*& command_ptr, u32 command_size)
{
  Log_DebugPrintf("GP0 clear cache");
  command_ptr++;
  EndCommand();
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

  command_ptr++;
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawModeCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;
  Log_DebugPrintf("Set draw mode %08X", param);
  SetDrawMode(Truncate16(param));
  EndCommand();
  return true;
}

bool GPU::HandleSetTextureWindowCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;
  SetTextureWindow(param);
  Log_DebugPrintf("Set texture window %02X %02X %02X %02X", m_draw_mode.texture_window_mask_x,
                  m_draw_mode.texture_window_mask_y, m_draw_mode.texture_window_offset_x,
                  m_draw_mode.texture_window_offset_y);

  EndCommand();
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
    m_drawing_area_changed = true;
  }

  EndCommand();
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
    m_drawing_area_changed = true;
  }

  EndCommand();
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

  EndCommand();
  return true;
}

bool GPU::HandleSetMaskBitCommand(const u32*& command_ptr, u32 command_size)
{
  const u32 param = *(command_ptr++) & 0x00FFFFFF;

  constexpr u32 gpustat_mask = (1 << 11) | (1 << 12);
  const u32 gpustat_bits = (param & 0x03) << 11;
  if ((m_GPUSTAT.bits & gpustat_mask) != gpustat_bits)
  {
    FlushRender();
    m_GPUSTAT.bits = (m_GPUSTAT.bits & ~gpustat_mask) | gpustat_bits;
  }
  Log_DebugPrintf("Set mask bit %u %u", BoolToUInt32(m_GPUSTAT.set_mask_while_drawing),
                  BoolToUInt32(m_GPUSTAT.check_mask_before_draw));

  EndCommand();
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
      CHECK_COMMAND_SIZE(total_words);

      // set draw state up
      if (rc.texture_enable)
      {
        const u16 texpage_attribute = Truncate16((rc.shading_enable ? command_ptr[5] : command_ptr[4]) >> 16);
        SetDrawMode((texpage_attribute & DrawMode::Reg::POLYGON_TEXPAGE_MASK) |
                    (m_draw_mode.mode_reg.bits & ~DrawMode::Reg::POLYGON_TEXPAGE_MASK));
        SetTexturePalette(Truncate16(command_ptr[2] >> 16));
      }
    }
    break;

    case Primitive::Line:
    {
      words_per_vertex = 1 + BoolToUInt8(rc.shading_enable);
      if (rc.polyline)
      {
        // polyline must have at least two vertices, and the terminator is (word & 0xf000f000) == 0x50005000. terminator
        // is on the first word for the vertex
        num_vertices = 2;
        bool found_terminator = false;
        for (u32 pos = rc.shading_enable ? 4 : 3; pos < command_size; pos += words_per_vertex)
        {
          if ((command_ptr[pos] & UINT32_C(0xF000F000)) == UINT32_C(0x50005000))
          {
            found_terminator = true;
            break;
          }

          num_vertices++;
        }
        if (!found_terminator)
          return false;

        total_words = words_per_vertex * num_vertices + BoolToUInt32(!rc.shading_enable) + 1;
      }
      else
      {
        num_vertices = 2;
        total_words = words_per_vertex * num_vertices + BoolToUInt32(!rc.shading_enable);
      }
    }
    break;

    case Primitive::Rectangle:
    {
      words_per_vertex =
        2 + BoolToUInt8(rc.texture_enable) + BoolToUInt8(rc.rectangle_size == DrawRectangleSize::Variable);
      num_vertices = 1;
      total_words = words_per_vertex;

      if (rc.texture_enable)
        SetTexturePalette(Truncate16(command_ptr[2] >> 16));
    }
    break;

    default:
      UnreachableCode();
      return true;
  }

  CHECK_COMMAND_SIZE(total_words);

  static constexpr std::array<const char*, 4> primitive_names = {{"", "polygon", "line", "rectangle"}};

  Log_TracePrintf("Render %s %s %s %s %s (%u verts, %u words per vert)", rc.quad_polygon ? "four-point" : "three-point",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  primitive_names[static_cast<u8>(rc.primitive.GetValue())], ZeroExtend32(num_vertices),
                  ZeroExtend32(words_per_vertex));

  if (IsInterlacedRenderingEnabled() && IsRasterScanlinePending())
    Synchronize();

  DispatchRenderCommand(rc, num_vertices, command_ptr);
  command_ptr += total_words;
  m_stats.num_vertices += num_vertices;
  m_stats.num_polygons++;
  EndCommand();
  return true;
}

bool GPU::HandleFillRectangleCommand(const u32*& command_ptr, u32 command_size)
{
  CHECK_COMMAND_SIZE(3);

  FlushRender();

  const u32 color = command_ptr[0] & 0x00FFFFFF;
  const u32 dst_x = command_ptr[1] & 0x3F0;
  const u32 dst_y = (command_ptr[1] >> 16) & 0x3FF;
  const u32 width = ((command_ptr[2] & 0x3FF) + 0xF) & ~0xF;
  const u32 height = (command_ptr[2] >> 16) & 0x1FF;
  command_ptr += 3;

  Log_DebugPrintf("Fill VRAM rectangle offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, width, height);

  FillVRAM(dst_x, dst_y, width, height, color);
  m_stats.num_vram_fills++;
  EndCommand();
  return true;
}

bool GPU::HandleCopyRectangleCPUToVRAMCommand(const u32*& command_ptr, u32 command_size)
{
  CHECK_COMMAND_SIZE(3);

  const u32 copy_width = ReplaceZero(command_ptr[2] & 0x3FF, 0x400);
  const u32 copy_height = ReplaceZero((command_ptr[2] >> 16) & 0x1FF, 0x200);
  const u32 num_pixels = copy_width * copy_height;
  const u32 num_words = 3 + ((num_pixels + 1) / 2);
  if (command_size < num_words)
  {
    m_command_total_words = num_words;
    m_state = State::WritingVRAM;
    return false;
  }

  const u32 dst_x = command_ptr[1] & 0x3FF;
  const u32 dst_y = (command_ptr[1] >> 16) & 0x3FF;

  Log_DebugPrintf("Copy rectangle from CPU to VRAM offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, copy_width,
                  copy_height);

  if (m_system->GetSettings().debugging.dump_cpu_to_vram_copies)
  {
    DumpVRAMToFile(StringUtil::StdStringFromFormat("cpu_to_vram_copy_%u.png", s_cpu_to_vram_dump_id++).c_str(),
                   copy_width, copy_height, sizeof(u16) * copy_width, &command_ptr[3], true);
  }

  FlushRender();
  UpdateVRAM(dst_x, dst_y, copy_width, copy_height, &command_ptr[3]);
  command_ptr += num_words;
  m_stats.num_vram_writes++;
  EndCommand();
  return true;
}

bool GPU::HandleCopyRectangleVRAMToCPUCommand(const u32*& command_ptr, u32 command_size)
{
  CHECK_COMMAND_SIZE(3);

  m_vram_transfer.width = ((Truncate16(command_ptr[2]) - 1) & 0x3FF) + 1;
  m_vram_transfer.height = ((Truncate16(command_ptr[2] >> 16) - 1) & 0x1FF) + 1;
  m_vram_transfer.x = Truncate16(command_ptr[1] & 0x3FF);
  m_vram_transfer.y = Truncate16((command_ptr[1] >> 16) & 0x3FF);
  command_ptr += 3;

  Log_DebugPrintf("Copy rectangle from VRAM to CPU offset=(%u,%u), size=(%u,%u)", m_vram_transfer.x, m_vram_transfer.y,
                  m_vram_transfer.width, m_vram_transfer.height);
  DebugAssert(m_vram_transfer.col == 0 && m_vram_transfer.row == 0);

  // all rendering should be done first...
  FlushRender();

  // ensure VRAM shadow is up to date
  ReadVRAM(m_vram_transfer.x, m_vram_transfer.y, m_vram_transfer.width, m_vram_transfer.height);

  if (m_system->GetSettings().debugging.dump_vram_to_cpu_copies)
  {
    DumpVRAMToFile(StringUtil::StdStringFromFormat("vram_to_cpu_copy_%u.png", s_vram_to_cpu_dump_id++).c_str(),
                   m_vram_transfer.width, m_vram_transfer.height, sizeof(u16) * VRAM_WIDTH,
                   &m_vram_ptr[m_vram_transfer.y * VRAM_WIDTH + m_vram_transfer.x], true);
  }

  // switch to pixel-by-pixel read state
  m_stats.num_vram_reads++;
  m_state = State::ReadingVRAM;
  m_command_total_words = 0;
  return true;
}

bool GPU::HandleCopyRectangleVRAMToVRAMCommand(const u32*& command_ptr, u32 command_size)
{
  CHECK_COMMAND_SIZE(4);

  const u32 src_x = command_ptr[1] & 0x3FF;
  const u32 src_y = (command_ptr[1] >> 16) & 0x3FF;
  const u32 dst_x = command_ptr[2] & 0x3FF;
  const u32 dst_y = (command_ptr[2] >> 16) & 0x3FF;
  const u32 width = ReplaceZero(command_ptr[3] & 0x3FF, 0x400);
  const u32 height = ReplaceZero((command_ptr[3] >> 16) & 0x1FF, 0x200);
  command_ptr += 4;

  Log_DebugPrintf("Copy rectangle from VRAM to VRAM src=(%u,%u), dst=(%u,%u), size=(%u,%u)", src_x, src_y, dst_x, dst_y,
                  width, height);

  FlushRender();
  CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);
  m_stats.num_vram_copies++;
  EndCommand();
  return true;
}
