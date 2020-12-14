#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(GPU);

#define CHECK_COMMAND_SIZE(num_words)                                                                                  \
  if (m_fifo.GetSize() < num_words)                                                                                    \
  {                                                                                                                    \
    m_command_total_words = num_words;                                                                                 \
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
  m_syncing = true;

  for (;;)
  {
    if (m_pending_command_ticks <= m_max_run_ahead && !m_fifo.IsEmpty())
    {
      switch (m_blitter_state)
      {
        case BlitterState::Idle:
        {
          const u32 command = FifoPeek(0) >> 24;
          if ((this->*s_GP0_command_handler_table[command])())
            continue;
          else
            goto batch_done;
        }

        case BlitterState::WritingVRAM:
        {
          DebugAssert(m_blit_remaining_words > 0);
          const u32 words_to_copy = std::min(m_blit_remaining_words, m_fifo.GetSize());
          m_blit_buffer.reserve(m_blit_buffer.size() + words_to_copy);
          for (u32 i = 0; i < words_to_copy; i++)
            m_blit_buffer.push_back(FifoPop());
          m_blit_remaining_words -= words_to_copy;
          AddCommandTicks(words_to_copy);

          Log_DebugPrintf("VRAM write burst of %u words, %u words remaining", words_to_copy, m_blit_remaining_words);
          if (m_blit_remaining_words == 0)
            FinishVRAMWrite();

          continue;
        }

        case BlitterState::ReadingVRAM:
        {
          goto batch_done;
        }
        break;

        case BlitterState::DrawingPolyLine:
        {
          const u32 words_per_vertex = m_render_command.shading_enable ? 2 : 1;
          u32 terminator_index =
            m_render_command.shading_enable ? ((static_cast<u32>(m_blit_buffer.size()) & 1u) ^ 1u) : 0u;
          for (; terminator_index < m_fifo.GetSize(); terminator_index += words_per_vertex)
          {
            // polyline must have at least two vertices, and the terminator is (word & 0xf000f000) == 0x50005000.
            // terminator is on the first word for the vertex
            if ((FifoPeek(terminator_index) & UINT32_C(0xF000F000)) == UINT32_C(0x50005000))
              break;
          }

          const bool found_terminator = (terminator_index < m_fifo.GetSize());
          const u32 words_to_copy = std::min(terminator_index, m_fifo.GetSize());
          if (words_to_copy > 0)
          {
            m_blit_buffer.reserve(m_blit_buffer.size() + words_to_copy);
            for (u32 i = 0; i < words_to_copy; i++)
              m_blit_buffer.push_back(FifoPop());
          }

          Log_DebugPrintf("Added %u words to polyline", words_to_copy);
          if (found_terminator)
          {
            // drop terminator
            m_fifo.RemoveOne();
            Log_DebugPrintf("Drawing poly-line with %u vertices", GetPolyLineVertexCount());
            DispatchRenderCommand();
            m_blit_buffer.clear();
            EndCommand();
            continue;
          }
        }
        break;
      }
    }

  batch_done:
    m_fifo_pushed = false;
    UpdateDMARequest();
    if (!m_fifo_pushed)
      break;
  }

  UpdateGPUIdle();
  m_syncing = false;
}

void GPU::EndCommand()
{
  m_blitter_state = BlitterState::Idle;
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
  {
    const GPURenderCommand rc{i << 24};
    switch (rc.primitive)
    {
      case GPUPrimitive::Polygon:
        table[i] = &GPU::HandleRenderPolygonCommand;
        break;
      case GPUPrimitive::Line:
        table[i] = rc.polyline ? &GPU::HandleRenderPolyLineCommand : &GPU::HandleRenderLineCommand;
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

  return table;
}

bool GPU::HandleUnknownGP0Command()
{
  const u32 command = FifoPeek() >> 24;
  Log_ErrorPrintf("Unimplemented GP0 command 0x%02X", command);

  SmallString dump;
  for (u32 i = 0; i < m_fifo.GetSize(); i++)
    dump.AppendFormattedString("%s0x%08X", (i > 0) ? " " : "", FifoPeek(i));
  Log_ErrorPrintf("FIFO: %s", dump.GetCharArray());

  m_fifo.RemoveOne();
  EndCommand();
  return true;
}

bool GPU::HandleNOPCommand()
{
  m_fifo.RemoveOne();
  EndCommand();
  return true;
}

bool GPU::HandleClearCacheCommand()
{
  Log_DebugPrintf("GP0 clear cache");
  m_fifo.RemoveOne();
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleInterruptRequestCommand()
{
  Log_WarningPrintf("GP0 interrupt request");
  if (!m_GPUSTAT.interrupt_request)
  {
    m_GPUSTAT.interrupt_request = true;
    g_interrupt_controller.InterruptRequest(InterruptController::IRQ::GPU);
  }

  m_fifo.RemoveOne();
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawModeCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;
  Log_DebugPrintf("Set draw mode %08X", param);
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
  const u32 left = param & VRAM_WIDTH_MASK;
  const u32 top = (param >> 10) & VRAM_HEIGHT_MASK;
  Log_DebugPrintf("Set drawing area top-left: (%u, %u)", left, top);
  if (m_drawing_area.left != left || m_drawing_area.top != top)
  {
    FlushRender();

    m_drawing_area.left = left;
    m_drawing_area.top = top;
    m_drawing_area_changed = true;
  }

  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleSetDrawingAreaBottomRightCommand()
{
  const u32 param = FifoPop() & 0x00FFFFFFu;

  const u32 right = param & VRAM_WIDTH_MASK;
  const u32 bottom = (param >> 10) & VRAM_HEIGHT_MASK;
  Log_DebugPrintf("Set drawing area bottom-right: (%u, %u)", m_drawing_area.right, m_drawing_area.bottom);
  if (m_drawing_area.right != right || m_drawing_area.bottom != bottom)
  {
    FlushRender();

    m_drawing_area.right = right;
    m_drawing_area.bottom = bottom;
    m_drawing_area_changed = true;
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
  Log_DebugPrintf("Set drawing offset (%d, %d)", m_drawing_offset.x, m_drawing_offset.y);
  if (m_drawing_offset.x != x || m_drawing_offset.y != y)
  {
    FlushRender();

    m_drawing_offset.x = x;
    m_drawing_offset.y = y;
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
  if ((m_GPUSTAT.bits & gpustat_mask) != gpustat_bits)
  {
    FlushRender();
    m_GPUSTAT.bits = (m_GPUSTAT.bits & ~gpustat_mask) | gpustat_bits;
  }
  Log_DebugPrintf("Set mask bit %u %u", BoolToUInt32(m_GPUSTAT.set_mask_while_drawing),
                  BoolToUInt32(m_GPUSTAT.check_mask_before_draw));

  AddCommandTicks(1);
  EndCommand();
  return true;
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

  Log_TracePrintf("Render %s %s %s %s polygon (%u verts, %u words per vert), %d setup ticks",
                  rc.quad_polygon ? "four-point" : "three-point",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  ZeroExtend32(num_vertices), ZeroExtend32(words_per_vertex), setup_ticks);

  // set draw state up
  if (rc.texture_enable)
  {
    const u16 texpage_attribute = Truncate16((rc.shading_enable ? FifoPeek(5) : FifoPeek(4)) >> 16);
    SetDrawMode((texpage_attribute & GPUDrawModeReg::POLYGON_TEXPAGE_MASK) |
                (m_draw_mode.mode_reg.bits & ~GPUDrawModeReg::POLYGON_TEXPAGE_MASK));
    SetTexturePalette(Truncate16(FifoPeek(2) >> 16));
  }

  m_stats.num_vertices += num_vertices;
  m_stats.num_polygons++;
  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  DispatchRenderCommand();
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
    SetTexturePalette(Truncate16(FifoPeek(2) >> 16));

  const TickCount setup_ticks = 16;
  AddCommandTicks(setup_ticks);

  Log_TracePrintf("Render %s %s %s rectangle (%u words), %d setup ticks",
                  rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome",
                  total_words, setup_ticks);

  m_stats.num_vertices++;
  m_stats.num_polygons++;
  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  DispatchRenderCommand();
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

  Log_TracePrintf("Render %s %s line (%u total words)", rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.shading_enable ? "shaded" : "monochrome", total_words);

  m_stats.num_vertices += 2;
  m_stats.num_polygons++;
  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  DispatchRenderCommand();
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

  Log_TracePrintf("Render %s %s poly-line, %d setup ticks", rc.transparency_enable ? "semi-transparent" : "opaque",
                  rc.shading_enable ? "shaded" : "monochrome", setup_ticks);

  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  const u32 words_to_pop = min_words - 1;
  // m_blit_buffer.resize(words_to_pop);
  // FifoPopRange(m_blit_buffer.data(), words_to_pop);
  m_blit_buffer.reserve(words_to_pop);
  for (u32 i = 0; i < words_to_pop; i++)
    m_blit_buffer.push_back(Truncate32(FifoPop()));

  // polyline goes via a different path through the blit buffer
  m_blitter_state = BlitterState::DrawingPolyLine;
  m_command_total_words = 0;
  return true;
}

bool GPU::HandleFillRectangleCommand()
{
  CHECK_COMMAND_SIZE(3);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  FlushRender();

  const u32 color = FifoPop() & 0x00FFFFFF;
  const u32 dst_x = FifoPeek() & 0x3F0;
  const u32 dst_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 width = ((FifoPeek() & VRAM_WIDTH_MASK) + 0xF) & ~0xF;
  const u32 height = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;

  Log_DebugPrintf("Fill VRAM rectangle offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, width, height);

  if (width > 0 && height > 0)
    FillVRAM(dst_x, dst_y, width, height, color);

  m_stats.num_vram_fills++;
  AddCommandTicks(46 + ((width / 8) + 9) * height);
  EndCommand();
  return true;
}

bool GPU::HandleCopyRectangleCPUToVRAMCommand()
{
  CHECK_COMMAND_SIZE(3);
  m_fifo.RemoveOne();

  const u32 dst_x = FifoPeek() & VRAM_WIDTH_MASK;
  const u32 dst_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 copy_width = ReplaceZero(FifoPeek() & VRAM_WIDTH_MASK, 0x400);
  const u32 copy_height = ReplaceZero((FifoPop() >> 16) & VRAM_HEIGHT_MASK, 0x200);
  const u32 num_pixels = copy_width * copy_height;
  const u32 num_words = ((num_pixels + 1) / 2);

  Log_DebugPrintf("Copy rectangle from CPU to VRAM offset=(%u,%u), size=(%u,%u)", dst_x, dst_y, copy_width,
                  copy_height);

  EndCommand();

  m_blitter_state = BlitterState::WritingVRAM;
  m_blit_buffer.reserve(num_words);
  m_blit_remaining_words = num_words;
  m_vram_transfer.x = Truncate16(dst_x);
  m_vram_transfer.y = Truncate16(dst_y);
  m_vram_transfer.width = Truncate16(copy_width);
  m_vram_transfer.height = Truncate16(copy_height);
  return true;
}

void GPU::FinishVRAMWrite()
{
  if (g_settings.debugging.dump_cpu_to_vram_copies && m_blit_remaining_words == 0)
  {
    DumpVRAMToFile(StringUtil::StdStringFromFormat("cpu_to_vram_copy_%u.png", s_cpu_to_vram_dump_id++).c_str(),
                   m_vram_transfer.width, m_vram_transfer.height, sizeof(u16) * m_vram_transfer.width,
                   m_blit_buffer.data(), true);
  }

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  FlushRender();

  if (m_blit_remaining_words == 0)
  {
    UpdateVRAM(m_vram_transfer.x, m_vram_transfer.y, m_vram_transfer.width, m_vram_transfer.height,
               m_blit_buffer.data(), m_GPUSTAT.set_mask_while_drawing, m_GPUSTAT.check_mask_before_draw);
  }
  else
  {
    const u32 num_pixels = ZeroExtend32(m_vram_transfer.width) * ZeroExtend32(m_vram_transfer.height);
    const u32 num_words = (num_pixels + 1) / 2;
    const u32 transferred_words = num_words - m_blit_remaining_words;
    const u32 transferred_pixels = transferred_words * 2;
    const u32 transferred_full_rows = transferred_pixels / m_vram_transfer.width;
    const u32 transferred_width_last_row = transferred_pixels % m_vram_transfer.width;

    Log_WarningPrintf(
      "Partial VRAM write - transfer finished with %u of %u words remaining (%u full rows, %u last row)",
      m_blit_remaining_words, num_words, transferred_full_rows, transferred_width_last_row);

    const u8* blit_ptr = reinterpret_cast<const u8*>(m_blit_buffer.data());
    if (transferred_full_rows > 0)
    {
      UpdateVRAM(m_vram_transfer.x, m_vram_transfer.y, m_vram_transfer.width, transferred_full_rows, blit_ptr,
                 m_GPUSTAT.set_mask_while_drawing, m_GPUSTAT.check_mask_before_draw);
      blit_ptr += (ZeroExtend32(m_vram_transfer.width) * transferred_full_rows) * sizeof(u16);
    }
    if (transferred_width_last_row > 0)
    {
      UpdateVRAM(m_vram_transfer.x, m_vram_transfer.y + transferred_full_rows, transferred_width_last_row, 1, blit_ptr,
                 m_GPUSTAT.set_mask_while_drawing, m_GPUSTAT.check_mask_before_draw);
    }
  }

  m_blit_buffer.clear();
  m_vram_transfer = {};
  m_blitter_state = BlitterState::Idle;
  m_stats.num_vram_writes++;
}

bool GPU::HandleCopyRectangleVRAMToCPUCommand()
{
  CHECK_COMMAND_SIZE(3);
  m_fifo.RemoveOne();

  m_vram_transfer.x = Truncate16(FifoPeek() & VRAM_WIDTH_MASK);
  m_vram_transfer.y = Truncate16((FifoPop() >> 16) & VRAM_HEIGHT_MASK);
  m_vram_transfer.width = ((Truncate16(FifoPeek()) - 1) & VRAM_WIDTH_MASK) + 1;
  m_vram_transfer.height = ((Truncate16(FifoPop() >> 16) - 1) & VRAM_HEIGHT_MASK) + 1;

  Log_DebugPrintf("Copy rectangle from VRAM to CPU offset=(%u,%u), size=(%u,%u)", m_vram_transfer.x, m_vram_transfer.y,
                  m_vram_transfer.width, m_vram_transfer.height);
  DebugAssert(m_vram_transfer.col == 0 && m_vram_transfer.row == 0);

  // all rendering should be done first...
  FlushRender();

  // ensure VRAM shadow is up to date
  ReadVRAM(m_vram_transfer.x, m_vram_transfer.y, m_vram_transfer.width, m_vram_transfer.height);

  if (g_settings.debugging.dump_vram_to_cpu_copies)
  {
    DumpVRAMToFile(StringUtil::StdStringFromFormat("vram_to_cpu_copy_%u.png", s_vram_to_cpu_dump_id++).c_str(),
                   m_vram_transfer.width, m_vram_transfer.height, sizeof(u16) * VRAM_WIDTH,
                   &m_vram_ptr[m_vram_transfer.y * VRAM_WIDTH + m_vram_transfer.x], true);
  }

  // switch to pixel-by-pixel read state
  m_stats.num_vram_reads++;
  m_blitter_state = BlitterState::ReadingVRAM;
  m_command_total_words = 0;
  return true;
}

bool GPU::HandleCopyRectangleVRAMToVRAMCommand()
{
  CHECK_COMMAND_SIZE(4);
  m_fifo.RemoveOne();

  const u32 src_x = FifoPeek() & VRAM_WIDTH_MASK;
  const u32 src_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 dst_x = FifoPeek() & VRAM_WIDTH_MASK;
  const u32 dst_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 width = ReplaceZero(FifoPeek() & VRAM_WIDTH_MASK, 0x400);
  const u32 height = ReplaceZero((FifoPop() >> 16) & VRAM_HEIGHT_MASK, 0x200);

  Log_DebugPrintf("Copy rectangle from VRAM to VRAM src=(%u,%u), dst=(%u,%u), size=(%u,%u)", src_x, src_y, dst_x, dst_y,
                  width, height);

  FlushRender();
  CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);
  m_stats.num_vram_copies++;
  AddCommandTicks(width * height * 2);
  EndCommand();
  return true;
}
