// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_pgxp.h"
#include "gpu.h"
#include "gpu_backend.h"
#include "gpu_dump.h"
#include "gpu_hw_texture_cache.h"
#include "interrupt_controller.h"
#include "system.h"

#include "common/assert.h"
#include "common/gsvector_formatter.h"
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
          FinishPolyline();
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
  InvalidateCLUT();
  GPUBackend::PushCommand(GPUBackend::NewClearCacheCommand());
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
  m_GPUSTAT.bits = (m_GPUSTAT.bits & ~gpustat_mask) | gpustat_bits;
  DEBUG_LOG("Set mask bit {} {}", BoolToUInt32(m_GPUSTAT.set_mask_while_drawing),
            BoolToUInt32(m_GPUSTAT.check_mask_before_draw));

  AddCommandTicks(1);
  EndCommand();
  return true;
}

void GPU::PrepareForDraw()
{
  if (m_drawing_area_changed)
  {
    m_drawing_area_changed = false;
    GPUBackendSetDrawingAreaCommand* cmd = GPUBackend::NewSetDrawingAreaCommand();
    cmd->new_area = m_drawing_area;
    GPUBackend::PushCommand(cmd);
  }
}

void GPU::FillBackendCommandParameters(GPUBackendCommand* cmd) const
{
  cmd->params.bits = 0;
  cmd->params.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  cmd->params.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  cmd->params.active_line_lsb = m_crtc_state.active_line_lsb;
  cmd->params.interlaced_rendering = IsInterlacedRenderingEnabled();
}

void GPU::FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const
{
  FillBackendCommandParameters(cmd);
  cmd->rc.bits = rc.bits;
  cmd->draw_mode.bits = m_draw_mode.mode_reg.bits;
  cmd->draw_mode.dither_enable = rc.IsDitheringEnabled() && cmd->draw_mode.dither_enable;
  cmd->palette.bits = m_draw_mode.palette_reg.bits;
  cmd->window = m_draw_mode.texture_window;
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
  // TODO: Get rid of SetTexturePalette() and just fill it as needed
  if (rc.texture_enable)
  {
    const u16 texpage_attribute = Truncate16((rc.shading_enable ? FifoPeek(5) : FifoPeek(4)) >> 16);
    SetDrawMode((texpage_attribute & GPUDrawModeReg::POLYGON_TEXPAGE_MASK) |
                (m_draw_mode.mode_reg.bits & ~GPUDrawModeReg::POLYGON_TEXPAGE_MASK));
    SetTexturePalette(Truncate16(FifoPeek(2) >> 16));
    UpdateCLUTIfNeeded(m_draw_mode.mode_reg.texture_mode, m_draw_mode.palette_reg);
  }

  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  PrepareForDraw();

  if (g_settings.gpu_pgxp_enable)
  {
    GPUBackendDrawPrecisePolygonCommand* cmd = GPUBackend::NewDrawPrecisePolygonCommand(num_vertices);
    FillDrawCommand(cmd, rc);

    const u32 first_color = rc.color_for_first_vertex;
    const bool shaded = rc.shading_enable;
    const bool textured = rc.texture_enable;
    bool valid_w = g_settings.gpu_pgxp_texture_correction;
    for (u32 i = 0; i < num_vertices; i++)
    {
      GPUBackendDrawPrecisePolygonCommand::Vertex* vert = &cmd->vertices[i];
      vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
      const u64 maddr_and_pos = m_fifo.Pop();
      const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
      vert->native_x = m_drawing_offset.x + vp.x;
      vert->native_y = m_drawing_offset.y + vp.y;
      vert->texcoord = textured ? Truncate16(FifoPop()) : 0;

      valid_w &= CPU::PGXP::GetPreciseVertex(Truncate32(maddr_and_pos >> 32), vp.bits, vert->native_x, vert->native_y,
                                             m_drawing_offset.x, m_drawing_offset.y, &vert->x, &vert->y, &vert->w);
    }

    cmd->valid_w = valid_w;
    if (!valid_w)
    {
      if (g_settings.gpu_pgxp_disable_2d)
      {
        // NOTE: This reads uninitialized data, but it's okay, it doesn't get used.
        for (u32 i = 0; i < num_vertices; i++)
        {
          GPUBackendDrawPrecisePolygonCommand::Vertex& v = cmd->vertices[i];
          GSVector2::store<false>(&v.x, GSVector2(GSVector2i::load<false>(&v.native_x)));
          v.w = 1.0f;
        }
      }
      else
      {
        for (u32 i = 0; i < num_vertices; i++)
          cmd->vertices[i].w = 1.0f;
      }
    }

    // Cull polygons which are too large.
    const GSVector2 v0f = GSVector2::load<false>(&cmd->vertices[0].x);
    const GSVector2 v1f = GSVector2::load<false>(&cmd->vertices[1].x);
    const GSVector2 v2f = GSVector2::load<false>(&cmd->vertices[2].x);
    const GSVector2 min_pos_12 = v1f.min(v2f);
    const GSVector2 max_pos_12 = v1f.max(v2f);
    const GSVector4i draw_rect_012 = GSVector4i(GSVector4(min_pos_12.min(v0f)).upld(GSVector4(max_pos_12.max(v0f))))
                                       .add32(GSVector4i::cxpr(0, 0, 1, 1));
    const bool first_tri_culled =
      (draw_rect_012.width() > MAX_PRIMITIVE_WIDTH || draw_rect_012.height() > MAX_PRIMITIVE_HEIGHT ||
       !draw_rect_012.rintersects(m_clamped_drawing_area));
    if (first_tri_culled)
    {
      // TODO: GPU events... somehow.
      DEBUG_LOG("Culling off-screen/too-large polygon: {},{} {},{} {},{}", cmd->vertices[0].native_x,
                cmd->vertices[0].native_y, cmd->vertices[1].native_x, cmd->vertices[1].native_y,
                cmd->vertices[2].native_x, cmd->vertices[2].native_y);

      if (!rc.quad_polygon)
      {
        EndCommand();
        return true;
      }
    }
    else
    {
      AddDrawTriangleTicks(GSVector2i::load<false>(&cmd->vertices[0].native_x), GSVector2i::load<false>(&cmd->vertices[1].native_x),
                           GSVector2i::load<false>(&cmd->vertices[2].native_x), rc.shading_enable, rc.texture_enable,
                           rc.transparency_enable);
    }

    // quads
    if (rc.quad_polygon)
    {
      const GSVector2 v3f = GSVector2::load<false>(&cmd->vertices[3].x);
      const GSVector4i draw_rect_123 = GSVector4i(GSVector4(min_pos_12.min(v3f)).upld(GSVector4(max_pos_12.max(v3f))))
                                         .add32(GSVector4i::cxpr(0, 0, 1, 1));

      // Cull polygons which are too large.
      const bool second_tri_culled =
        (draw_rect_123.width() > MAX_PRIMITIVE_WIDTH || draw_rect_123.height() > MAX_PRIMITIVE_HEIGHT ||
         !draw_rect_123.rintersects(m_clamped_drawing_area));
      if (second_tri_culled)
      {
        DEBUG_LOG("Culling off-screen/too-large polygon (quad second half): {},{} {},{} {},{}",
                  cmd->vertices[2].native_x, cmd->vertices[2].native_y, cmd->vertices[1].native_x,
                  cmd->vertices[1].native_y, cmd->vertices[0].native_x, cmd->vertices[0].native_y);

        if (first_tri_culled)
        {
          EndCommand();
          return true;
        }

        // Remove second part of quad.
        cmd->num_vertices = 3;
      }
      else
      {
        AddDrawTriangleTicks(GSVector2i::load<false>(&cmd->vertices[2].native_x), GSVector2i::load<false>(&cmd->vertices[1].native_x),
                             GSVector2i::load<false>(&cmd->vertices[3].native_x), rc.shading_enable, rc.texture_enable,
                             rc.transparency_enable);

        // If first part was culled, move the second part to the first.
        if (first_tri_culled)
        {
          std::memcpy(&cmd->vertices[0], &cmd->vertices[2], sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
          std::memcpy(&cmd->vertices[2], &cmd->vertices[3], sizeof(GPUBackendDrawPrecisePolygonCommand::Vertex));
          cmd->num_vertices = 3;
        }
      }
    }

    GPUBackend::PushCommand(cmd);
  }
  else
  {
    GPUBackendDrawPolygonCommand* cmd = GPUBackend::NewDrawPolygonCommand(num_vertices);
    FillDrawCommand(cmd, rc);

    const u32 first_color = rc.color_for_first_vertex;
    const bool shaded = rc.shading_enable;
    const bool textured = rc.texture_enable;
    for (u32 i = 0; i < num_vertices; i++)
    {
      GPUBackendDrawPolygonCommand::Vertex* vert = &cmd->vertices[i];
      vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
      const u64 maddr_and_pos = m_fifo.Pop();
      const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
      vert->x = m_drawing_offset.x + vp.x;
      vert->y = m_drawing_offset.y + vp.y;
      vert->texcoord = textured ? Truncate16(FifoPop()) : 0;
    }

    // Cull polygons which are too large.
    const GSVector2i v0 = GSVector2i::load<false>(&cmd->vertices[0].x);
    const GSVector2i v1 = GSVector2i::load<false>(&cmd->vertices[1].x);
    const GSVector2i v2 = GSVector2i::load<false>(&cmd->vertices[2].x);
    const GSVector2i min_pos_12 = v1.min_s32(v2);
    const GSVector2i max_pos_12 = v1.max_s32(v2);
    const GSVector4i draw_rect_012 =
      GSVector4i::xyxy(min_pos_12.min_s32(v0), max_pos_12.max_s32(v0)).add32(GSVector4i::cxpr(0, 0, 1, 1));
    const bool first_tri_culled =
      (draw_rect_012.width() > MAX_PRIMITIVE_WIDTH || draw_rect_012.height() > MAX_PRIMITIVE_HEIGHT ||
       !draw_rect_012.rintersects(m_clamped_drawing_area));
    if (first_tri_culled)
    {
      DEBUG_LOG("Culling off-screen/too-large polygon: {},{} {},{} {},{}", cmd->vertices[0].x, cmd->vertices[0].y,
                cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[2].x, cmd->vertices[2].y);

      if (!rc.quad_polygon)
      {
        EndCommand();
        return true;
      }
    }
    else
    {
      AddDrawTriangleTicks(v0, v1, v2, rc.shading_enable, rc.texture_enable, rc.transparency_enable);
    }

    // quads
    if (rc.quad_polygon)
    {
      const GSVector2i v3 = GSVector2i::load<false>(&cmd->vertices[3].x);
      const GSVector4i draw_rect_123 = GSVector4i(min_pos_12.min_s32(v3))
                                         .upl64(GSVector4i(max_pos_12.max_s32(v3)))
                                         .add32(GSVector4i::cxpr(0, 0, 1, 1));

      // Cull polygons which are too large.
      const bool second_tri_culled =
        (draw_rect_123.width() > MAX_PRIMITIVE_WIDTH || draw_rect_123.height() > MAX_PRIMITIVE_HEIGHT ||
         !draw_rect_123.rintersects(m_clamped_drawing_area));
      if (second_tri_culled)
      {
        DEBUG_LOG("Culling too-large polygon (quad second half): {},{} {},{} {},{}", cmd->vertices[2].x,
                  cmd->vertices[2].y, cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[0].x, cmd->vertices[0].y);

        if (first_tri_culled)
        {
          EndCommand();
          return true;
        }

        // Remove second part of quad.
        cmd->num_vertices = 3;
      }
      else
      {
        AddDrawTriangleTicks(v2, v1, v3, rc.shading_enable, rc.texture_enable, rc.transparency_enable);

        // If first part was culled, move the second part to the first.
        if (first_tri_culled)
        {
          std::memcpy(&cmd->vertices[0], &cmd->vertices[2], sizeof(GPUBackendDrawPolygonCommand::Vertex));
          std::memcpy(&cmd->vertices[2], &cmd->vertices[3], sizeof(GPUBackendDrawPolygonCommand::Vertex));
          cmd->num_vertices = 3;
        }
      }
    }

    GPUBackend::PushCommand(cmd);
  }

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

  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  PrepareForDraw();
  GPUBackendDrawRectangleCommand* cmd = GPUBackend::NewDrawRectangleCommand();
  FillDrawCommand(cmd, rc);
  cmd->color = rc.color_for_first_vertex;

  const GPUVertexPosition vp{FifoPop()};
  cmd->x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
  cmd->y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

  if (rc.texture_enable)
  {
    const u32 texcoord_and_palette = FifoPop();
    cmd->palette.bits = Truncate16(texcoord_and_palette >> 16);
    cmd->texcoord = Truncate16(texcoord_and_palette);
  }
  else
  {
    cmd->palette.bits = 0;
    cmd->texcoord = 0;
  }

  switch (rc.rectangle_size)
  {
    case GPUDrawRectangleSize::R1x1:
      cmd->width = 1;
      cmd->height = 1;
      break;
    case GPUDrawRectangleSize::R8x8:
      cmd->width = 8;
      cmd->height = 8;
      break;
    case GPUDrawRectangleSize::R16x16:
      cmd->width = 16;
      cmd->height = 16;
      break;
    default:
    {
      const u32 width_and_height = FifoPop();
      cmd->width = static_cast<u16>(width_and_height & VRAM_WIDTH_MASK);
      cmd->height = static_cast<u16>((width_and_height >> 16) & VRAM_HEIGHT_MASK);
    }
    break;
  }

  const GSVector4i rect = GSVector4i(cmd->x, cmd->y, cmd->x + cmd->width, cmd->y + cmd->height);
  const GSVector4i clamped_rect = m_clamped_drawing_area.rintersect(rect);
  if (clamped_rect.rempty()) [[unlikely]]
  {
    DEBUG_LOG("Culling off-screen rectangle {}", rect);
    EndCommand();
    return true;
  }

  AddDrawRectangleTicks(clamped_rect, rc.texture_enable, rc.transparency_enable);

  GPUBackend::PushCommand(cmd);
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

  m_render_command.bits = rc.bits;
  m_fifo.RemoveOne();

  PrepareForDraw();
  GPUBackendDrawLineCommand* cmd = GPUBackend::NewDrawLineCommand(2);
  FillDrawCommand(cmd, rc);
  cmd->palette.bits = 0;

  if (rc.shading_enable)
  {
    cmd->vertices[0].color = rc.color_for_first_vertex;
    const GPUVertexPosition start_pos{FifoPop()};
    cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
    cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

    cmd->vertices[1].color = FifoPop() & UINT32_C(0x00FFFFFF);
    const GPUVertexPosition end_pos{FifoPop()};
    cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
    cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
  }
  else
  {
    cmd->vertices[0].color = rc.color_for_first_vertex;
    cmd->vertices[1].color = rc.color_for_first_vertex;

    const GPUVertexPosition start_pos{FifoPop()};
    cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
    cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

    const GPUVertexPosition end_pos{FifoPop()};
    cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
    cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
  }

  const GSVector2i v0 = GSVector2i::load<false>(&cmd->vertices[0].x);
  const GSVector2i v1 = GSVector2i::load<false>(&cmd->vertices[1].x);
  const GSVector4i rect = GSVector4i::xyxy(v0.min_s32(v1), v0.max_s32(v1)).add32(GSVector4i::cxpr(0, 0, 1, 1));
  const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);

  if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT || clamped_rect.rempty())
  {
    DEBUG_LOG("Culling too-large/off-screen line: {},{} - {},{}", cmd->vertices[0].y, cmd->vertices[0].y,
              cmd->vertices[1].x, cmd->vertices[1].y);
    EndCommand();
    return true;
  }

  AddDrawLineTicks(clamped_rect, rc.shading_enable);
  GPUBackend::PushCommand(cmd);
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

void GPU::FinishPolyline()
{
  PrepareForDraw();

  const u32 num_vertices = GetPolyLineVertexCount();
  DebugAssert(num_vertices >= 2);

  GPUBackendDrawLineCommand* cmd = GPUBackend::NewDrawLineCommand((num_vertices - 1) * 2);
  FillDrawCommand(cmd, m_render_command);

  u32 buffer_pos = 0;
  const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
  const GSVector2i draw_offset = GSVector2i::load<false>(&m_drawing_offset.x);
  GSVector2i start_pos = GSVector2i(start_vp.x, start_vp.y).add32(draw_offset);
  u32 start_color = m_render_command.color_for_first_vertex;

  const bool shaded = m_render_command.shading_enable;
  u32 out_vertex_count = 0;
  for (u32 i = 1; i < num_vertices; i++)
  {
    const u32 end_color =
      shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : m_render_command.color_for_first_vertex;
    const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
    const GSVector2i end_pos = GSVector2i(vp.x, vp.y).add32(draw_offset);

    const GSVector4i rect =
      GSVector4i::xyxy(start_pos.min_s32(end_pos), start_pos.max_s32(end_pos)).add32(GSVector4i::cxpr(0, 0, 1, 1));
    const GSVector4i clamped_rect = rect.rintersect(m_clamped_drawing_area);

    if (rect.width() > MAX_PRIMITIVE_WIDTH || rect.height() > MAX_PRIMITIVE_HEIGHT || clamped_rect.rempty())
    {
      DEBUG_LOG("Culling too-large/off-screen line: {},{} - {},{}", start_pos.x, start_pos.y, end_pos.x, end_pos.y);
    }
    else
    {
      AddDrawLineTicks(clamped_rect, m_render_command.shading_enable);

      GPUBackendDrawLineCommand::Vertex* out_vertex = &cmd->vertices[out_vertex_count];
      out_vertex_count += 2;

      GSVector2i::store<false>(&out_vertex[0].x, start_pos);
      out_vertex[0].color = start_color;
      GSVector2i::store<false>(&out_vertex[1].x, end_pos);
      out_vertex[1].color = end_color;
    }

    start_pos = end_pos;
    start_color = end_color;
  }

  if (out_vertex_count > 0)
  {
    DebugAssert(out_vertex_count <= cmd->num_vertices);
    cmd->num_vertices = Truncate16(out_vertex_count);
    GPUBackend::PushCommand(cmd);
  }
}

bool GPU::HandleFillRectangleCommand()
{
  CHECK_COMMAND_SIZE(3);

  if (IsInterlacedRenderingEnabled() && IsCRTCScanlinePending())
    SynchronizeCRTC();

  const u32 color = FifoPop() & 0x00FFFFFF;
  const u32 dst_x = FifoPeek() & 0x3F0;
  const u32 dst_y = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;
  const u32 width = ((FifoPeek() & VRAM_WIDTH_MASK) + 0xF) & ~0xF;
  const u32 height = (FifoPop() >> 16) & VRAM_HEIGHT_MASK;

  DEBUG_LOG("Fill VRAM rectangle offset=({},{}), size=({},{})", dst_x, dst_y, width, height);

  if (width > 0 && height > 0)
  {
    GPUBackendFillVRAMCommand* cmd = GPUBackend::NewFillVRAMCommand();
    FillBackendCommandParameters(cmd);
    cmd->x = static_cast<u16>(dst_x);
    cmd->y = static_cast<u16>(dst_y);
    cmd->width = static_cast<u16>(width);
    cmd->height = static_cast<u16>(height);
    cmd->color = color;
    GPUBackend::PushCommand(cmd);
  }

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
      UpdateVRAM(m_vram_transfer.x, m_vram_transfer.y, m_vram_transfer.width, static_cast<u16>(transferred_full_rows),
                 blit_ptr, m_GPUSTAT.set_mask_while_drawing, m_GPUSTAT.check_mask_before_draw);
      blit_ptr += (ZeroExtend32(m_vram_transfer.width) * transferred_full_rows) * sizeof(u16);
    }
    if (transferred_width_last_row > 0)
    {
      UpdateVRAM(m_vram_transfer.x, static_cast<u16>(m_vram_transfer.y + transferred_full_rows),
                 static_cast<u16>(transferred_width_last_row), 1, blit_ptr, m_GPUSTAT.set_mask_while_drawing,
                 m_GPUSTAT.check_mask_before_draw);
    }
  }

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

  // ensure VRAM shadow is up to date
  ReadVRAM(m_vram_transfer.x, m_vram_transfer.y, m_vram_transfer.width, m_vram_transfer.height);

  if (g_settings.debugging.dump_vram_to_cpu_copies)
  {
    DumpVRAMToFile(TinyString::from_format("vram_to_cpu_copy_{}.png", s_vram_to_cpu_dump_id++), m_vram_transfer.width,
                   m_vram_transfer.height, sizeof(u16) * VRAM_WIDTH,
                   &g_vram[m_vram_transfer.y * VRAM_WIDTH + m_vram_transfer.x], true);
  }

  // switch to pixel-by-pixel read state
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
    GPUBackendCopyVRAMCommand* cmd = GPUBackend::NewCopyVRAMCommand();
    FillBackendCommandParameters(cmd);
    cmd->src_x = static_cast<u16>(src_x);
    cmd->src_y = static_cast<u16>(src_y);
    cmd->dst_x = static_cast<u16>(dst_x);
    cmd->dst_y = static_cast<u16>(dst_y);
    cmd->width = static_cast<u16>(width);
    cmd->height = static_cast<u16>(height);
    GPUBackend::PushCommand(cmd);
  }

  AddCommandTicks(width * height * 2);
  EndCommand();
  return true;
}
