// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_sw_backend.h"
#include "gpu.h"
#include "gpu_sw_rasterizer.h"
#include "system.h"

#include "util/gpu_device.h"

#include <algorithm>

GPU_SW_Backend::GPU_SW_Backend() = default;

GPU_SW_Backend::~GPU_SW_Backend() = default;

bool GPU_SW_Backend::Initialize(bool use_thread)
{
  return GPUBackend::Initialize(use_thread);
}

void GPU_SW_Backend::Reset()
{
  GPUBackend::Reset();
}

void GPU_SW_Backend::DrawPolygon(const GPUBackendDrawPolygonCommand* cmd)
{
  const GPURenderCommand rc{cmd->rc.bits};

  const GPU_SW_Rasterizer::DrawTriangleFunction DrawFunction = GPU_SW_Rasterizer::GetDrawTriangleFunction(
    rc.shading_enable, rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable);

  DrawFunction(cmd, &cmd->vertices[0], &cmd->vertices[1], &cmd->vertices[2]);
  if (rc.quad_polygon)
    DrawFunction(cmd, &cmd->vertices[2], &cmd->vertices[1], &cmd->vertices[3]);
}

void GPU_SW_Backend::DrawRectangle(const GPUBackendDrawRectangleCommand* cmd)
{
  const GPURenderCommand rc{cmd->rc.bits};

  const GPU_SW_Rasterizer::DrawRectangleFunction DrawFunction =
    GPU_SW_Rasterizer::GetDrawRectangleFunction(rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable);

  DrawFunction(cmd);
}

void GPU_SW_Backend::DrawLine(const GPUBackendDrawLineCommand* cmd)
{
  const GPU_SW_Rasterizer::DrawLineFunction DrawFunction =
    GPU_SW_Rasterizer::GetDrawLineFunction(cmd->rc.shading_enable, cmd->rc.transparency_enable);

  for (u16 i = 1; i < cmd->num_vertices; i += 2)
    DrawFunction(cmd, &cmd->vertices[i - 1], &cmd->vertices[i]);
}

void GPU_SW_Backend::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params)
{
  GPU_SW_Rasterizer::FillVRAM(x, y, width, height, color, params.interlaced_rendering, params.active_line_lsb);
}

void GPU_SW_Backend::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data,
                                GPUBackendCommandParameters params)
{
  GPU_SW_Rasterizer::WriteVRAM(x, y, width, height, data, params.set_mask_while_drawing, params.check_mask_before_draw);
}

void GPU_SW_Backend::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                              GPUBackendCommandParameters params)
{
  GPU_SW_Rasterizer::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height, params.set_mask_while_drawing,
                              params.check_mask_before_draw);
}

void GPU_SW_Backend::UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit)
{
  GPU::ReadCLUT(g_gpu_clut, reg, clut_is_8bit);
}

void GPU_SW_Backend::DrawingAreaChanged(const GPUDrawingArea& new_drawing_area, const GSVector4i clamped_drawing_area)
{
  GPU_SW_Rasterizer::g_drawing_area = new_drawing_area;
}

void GPU_SW_Backend::FlushRender()
{
}
