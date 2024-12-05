// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu.h"
#include "gpu_dump.h"
#include "gpu_hw_texture_cache.h"
#include "interrupt_controller.h"
#include "system.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"

LOG_CHANNEL(GPU);

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

void GPU::TryExecuteCommands()
{
  while (m_pending_command_ticks <= m_max_run_ahead && !m_fifo.IsEmpty())
  {
    switch (m_blitter_state)
    {
      case BlitterState::Idle:
      {
        const u32 command = FifoPeek(0) >> 24;
        if ((this->*s_GP0_command_handler_table[command])())
          continue;
        else
          return;
      }

      case BlitterState::WritingVRAM:
      {
        DebugAssert(m_blit_remaining_words > 0);
        const u32 words_to_copy = std::min(m_blit_remaining_words, m_fifo.GetSize());
        m_blit_buffer.reserve(m_blit_buffer.size() + words_to_copy);
        for (u32 i = 0; i < words_to_copy; i++)
          m_blit_buffer.push_back(FifoPop());
        m_blit_remaining_words -= words_to_copy;

        DEBUG_LOG("VRAM write burst of {} words, {} words remaining", words_to_copy, m_blit_remaining_words);
        if (m_blit_remaining_words == 0)
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

        DEBUG_LOG("Added {} words to polyline", words_to_copy);
        if (found_terminator)
        {
          // drop terminator
          m_fifo.RemoveOne();
          DEBUG_LOG("Drawing poly-line with {} vertices", GetPolyLineVertexCount());
          DispatchRenderCommand();
          m_blit_buffer.clear();
          EndCommand();
          continue;
        }
      }
      break;
    }
  }
}

void GPU::ExecuteCommands()
{
  const bool was_executing_from_event = std::exchange(m_executing_commands, true);

  TryExecuteCommands();
  UpdateDMARequest();
  UpdateGPUIdle();

  m_executing_commands = was_executing_from_event;
  if (!was_executing_from_event)
    UpdateCommandTickEvent();
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

  table[0xFF] = &GPU::HandleNOPCommand;

  return table;
}

bool GPU::HandleUnknownGP0Command()
{
  const u32 command = FifoPeek() >> 24;
  ERROR_LOG("Unimplemented GP0 command 0x{:02X}", command);

  SmallString dump;
  for (u32 i = 0; i < m_fifo.GetSize(); i++)
    dump.append_format("{}{:08X}", (i > 0) ? " " : "", FifoPeek(i));
  ERROR_LOG("FIFO: {}", dump);

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
  DEBUG_LOG("GP0 clear cache");
  m_draw_mode.SetTexturePageChanged();
  InvalidateCLUT();
  m_fifo.RemoveOne();
  AddCommandTicks(1);
  EndCommand();
  return true;
}

bool GPU::HandleInterruptRequestCommand()
{
  DEBUG_LOG("GP0 interrupt request");

  m_GPUSTAT.interrupt_request = true;
  InterruptController::SetLineState(InterruptController::IRQ::GPU, true);

  m_fifo.RemoveOne();
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
  if (m_drawing_area.left != left || m_drawing_area.top != top)
  {
    FlushRender();

    m_drawing_area.left = left;
    m_drawing_area.top = top;
    m_drawing_area_changed = true;
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
  if (m_drawing_area.right != right || m_drawing_area.bottom != bottom)
  {
    FlushRender();

    m_drawing_area.right = right;
    m_drawing_area.bottom = bottom;
    m_drawing_area_changed = true;
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
  DEBUG_LOG("Set mask bit {} {}", BoolToUInt32(m_GPUSTAT.set_mask_while_drawing),
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

  TRACE_LOG("Render {} {} {} {} polygon ({} verts, {} words per vert), {} setup ticks",
            rc.quad_polygon ? "four-point" : "three-point", rc.transparency_enable ? "semi-transparent" : "opaque",
            rc.texture_enable ? "textured" : "non-textured", rc.shading_enable ? "shaded" : "monochrome", num_vertices,
            words_per_vertex, setup_ticks);

  // set draw state up
  if (rc.texture_enable)
  {
    const u16 texpage_attribute = Truncate16((rc.shading_enable ? FifoPeek(5) : FifoPeek(4)) >> 16);
    SetDrawMode((texpage_attribute & GPUDrawModeReg::POLYGON_TEXPAGE_MASK) |
                (m_draw_mode.mode_reg.bits & ~GPUDrawModeReg::POLYGON_TEXPAGE_MASK));
    SetTexturePalette(Truncate16(FifoPeek(2) >> 16));
    UpdateCLUTIfNeeded(m_draw_mode.mode_reg.texture_mode, m_draw_mode.palette_reg);
  }

  m_counters.num_vertices += num_vertices;
  m_counters.num_primitives++;
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
  {
    SetTexturePalette(Truncate16(FifoPeek(2) >> 16));
    UpdateCLUTIfNeeded(m_draw_mode.mode_reg.texture_mode, m_draw_mode.palette_reg);
  }

  const TickCount setup_ticks = 16;
  AddCommandTicks(setup_ticks);

  TRACE_LOG("Render {} {} {} rectangle ({} words), {} setup ticks",
            rc.transparency_enable ? "semi-transparent" : "opaque", rc.texture_enable ? "textured" : "non-textured",
            rc.shading_enable ? "shaded" : "monochrome", total_words, setup_ticks);

  m_counters.num_vertices++;
  m_counters.num_primitives++;
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

  TRACE_LOG("Render {} {} line ({} total words)", rc.transparency_enable ? "semi-transparent" : "opaque",
            rc.shading_enable ? "shaded" : "monochrome", total_words);

  m_counters.num_vertices += 2;
  m_counters.num_primitives++;
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

  TRACE_LOG("Render {} {} poly-line, {} setup ticks", rc.transparency_enable ? "semi-transparent" : "opaque",
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

  DEBUG_LOG("Fill VRAM rectangle offset=({},{}), size=({},{})", dst_x, dst_y, width, height);

  if (width > 0 && height > 0)
    FillVRAM(dst_x, dst_y, width, height, color);

  m_counters.num_writes++;
  AddCommandTicks(46 + ((width / 8) + 9) * height);
  EndCommand();
  return true;
}

bool GPU::HandleCopyRectangleCPUToVRAMCommand()
{
  CHECK_COMMAND_SIZE(3);
  m_fifo.RemoveOne();

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
  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  FlushRender();

  if (m_blit_remaining_words == 0)
  {
    if (g_settings.debugging.dump_cpu_to_vram_copies)
    {
      DumpVRAMToFile(TinyString::from_format("cpu_to_vram_copy_{}.png", s_cpu_to_vram_dump_id++), m_vram_transfer.width,
                     m_vram_transfer.height, sizeof(u16) * m_vram_transfer.width, m_blit_buffer.data(), true);
    }

    if (GPUTextureCache::ShouldDumpVRAMWrite(m_vram_transfer.width, m_vram_transfer.height))
    {
      GPUTextureCache::DumpVRAMWrite(m_vram_transfer.width, m_vram_transfer.height,
                                     reinterpret_cast<const u16*>(m_blit_buffer.data()));
    }

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

    WARNING_LOG("Partial VRAM write - transfer finished with {} of {} words remaining ({} full rows, {} last row)",
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

  m_counters.num_writes++;
  m_blit_buffer.clear();
  m_vram_transfer = {};
  m_blitter_state = BlitterState::Idle;
}

bool GPU::HandleCopyRectangleVRAMToCPUCommand()
{
  CHECK_COMMAND_SIZE(3);
  m_fifo.RemoveOne();

  m_vram_transfer.x = Truncate16(FifoPeek() & VRAM_WIDTH_MASK);
  m_vram_transfer.y = Truncate16((FifoPop() >> 16) & VRAM_HEIGHT_MASK);
  m_vram_transfer.width = ((Truncate16(FifoPeek()) - 1) & VRAM_WIDTH_MASK) + 1;
  m_vram_transfer.height = ((Truncate16(FifoPop() >> 16) - 1) & VRAM_HEIGHT_MASK) + 1;

  DEBUG_LOG("Copy rectangle from VRAM to CPU offset=({},{}), size=({},{})", m_vram_transfer.x, m_vram_transfer.y,
            m_vram_transfer.width, m_vram_transfer.height);
  DebugAssert(m_vram_transfer.col == 0 && m_vram_transfer.row == 0);

  // all rendering should be done first...
  FlushRender();

  // ensure VRAM shadow is up to date
  ReadVRAM(m_vram_transfer.x, m_vram_transfer.y, m_vram_transfer.width, m_vram_transfer.height);

  if (g_settings.debugging.dump_vram_to_cpu_copies)
  {
    DumpVRAMToFile(TinyString::from_format("vram_to_cpu_copy_{}.png", s_vram_to_cpu_dump_id++), m_vram_transfer.width,
                   m_vram_transfer.height, sizeof(u16) * VRAM_WIDTH,
                   &g_vram[m_vram_transfer.y * VRAM_WIDTH + m_vram_transfer.x], true);
  }

  // switch to pixel-by-pixel read state
  m_counters.num_reads++;
  m_blitter_state = BlitterState::ReadingVRAM;
  m_command_total_words = 0;

  // toss the entire read in the recorded trace. we might want to change this to mirroring GPUREAD in the future..
  if (m_gpu_dump) [[unlikely]]
    m_gpu_dump->WriteDiscardVRAMRead(m_vram_transfer.width, m_vram_transfer.height);

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

  DEBUG_LOG("Copy rectangle from VRAM to VRAM src=({},{}), dst=({},{}), size=({},{})", src_x, src_y, dst_x, dst_y,
            width, height);

  // Some VRAM copies aren't going to do anything. Most games seem to send a 2x2 VRAM copy at the end of a frame.
  const bool skip_copy =
    width == 0 || height == 0 || (src_x == dst_x && src_y == dst_y && !m_GPUSTAT.set_mask_while_drawing);
  if (!skip_copy)
  {
    m_counters.num_copies++;

    FlushRender();
    CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);
  }

  AddCommandTicks(width * height * 2);
  EndCommand();
  return true;
}
