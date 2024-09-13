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

bool GPU_SW_Backend::Initialize(bool force_thread)
{
  GPU_SW_Rasterizer::SelectImplementation();

  return GPUBackend::Initialize(force_thread);
}

void GPU_SW_Backend::Reset()
{
  GPUBackend::Reset();
}

void GPU_SW_Backend::DrawPolygon(const GPUBackendDrawPolygonCommand* cmd)
{
  const GPURenderCommand rc{cmd->rc.bits};
  const bool dithering_enable = rc.IsDitheringEnabled() && cmd->draw_mode.dither_enable;

  const GPU_SW_Rasterizer::DrawTriangleFunction DrawFunction = GPU_SW_Rasterizer::GetDrawTriangleFunction(
    rc.shading_enable, rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable, dithering_enable);

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
  const GPU_SW_Rasterizer::DrawLineFunction DrawFunction = GPU_SW_Rasterizer::GetDrawLineFunction(
    cmd->rc.shading_enable, cmd->rc.transparency_enable, cmd->IsDitheringEnabled());

  for (u16 i = 1; i < cmd->num_vertices; i++)
    DrawFunction(cmd, &cmd->vertices[i - 1], &cmd->vertices[i]);
}

void GPU_SW_Backend::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params)
{
  const u16 color16 = VRAMRGBA8888ToRGBA5551(color);
  const GSVector4i fill = GSVector4i(color16, color16, color16, color16, color16, color16, color16, color16);
  constexpr u32 vector_width = 8;
  const u32 aligned_width = Common::AlignDownPow2(width, vector_width);

  if ((x + width) <= VRAM_WIDTH && !params.interlaced_rendering)
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;

      u16* row_ptr = &g_vram[row * VRAM_WIDTH + x];
      u32 xoffs = 0;
      for (; xoffs < aligned_width; xoffs += vector_width, row_ptr += vector_width)
        GSVector4i::store<false>(row_ptr, fill);
      for (; xoffs < width; xoffs++)
        *(row_ptr++) = color16;
    }
  }
  else if (params.interlaced_rendering)
  {
    // Hardware tests show that fills seem to break on the first two lines when the offset matches the displayed field.
    const u32 active_field = params.active_line_lsb;

    if ((x + width) <= VRAM_WIDTH)
    {
      for (u32 yoffs = 0; yoffs < height; yoffs++)
      {
        const u32 row = (y + yoffs) % VRAM_HEIGHT;
        if ((row & u32(1)) == active_field)
          continue;

        u16* row_ptr = &g_vram[row * VRAM_WIDTH + x];
        u32 xoffs = 0;
        for (; xoffs < aligned_width; xoffs += vector_width, row_ptr += vector_width)
          GSVector4i::store<false>(row_ptr, fill);
        for (; xoffs < width; xoffs++)
          *(row_ptr++) = color16;
      }
    }
    else
    {
      for (u32 yoffs = 0; yoffs < height; yoffs++)
      {
        const u32 row = (y + yoffs) % VRAM_HEIGHT;
        if ((row & u32(1)) == active_field)
          continue;

        u16* row_ptr = &g_vram[row * VRAM_WIDTH];
        for (u32 xoffs = 0; xoffs < width; xoffs++)
        {
          const u32 col = (x + xoffs) % VRAM_WIDTH;
          row_ptr[col] = color16;
        }
      }
    }
  }
  else
  {
    for (u32 yoffs = 0; yoffs < height; yoffs++)
    {
      const u32 row = (y + yoffs) % VRAM_HEIGHT;
      u16* row_ptr = &g_vram[row * VRAM_WIDTH];
      for (u32 xoffs = 0; xoffs < width; xoffs++)
      {
        const u32 col = (x + xoffs) % VRAM_WIDTH;
        row_ptr[col] = color16;
      }
    }
  }
}

void GPU_SW_Backend::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data,
                                GPUBackendCommandParameters params)
{
  // Fast path when the copy is not oversized.
  if ((x + width) <= VRAM_WIDTH && (y + height) <= VRAM_HEIGHT && !params.IsMaskingEnabled())
  {
    const u16* src_ptr = static_cast<const u16*>(data);
    u16* dst_ptr = &g_vram[y * VRAM_WIDTH + x];
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
    const u16 mask_and = params.GetMaskAND();
    const u16 mask_or = params.GetMaskOR();

    for (u32 row = 0; row < height;)
    {
      u16* dst_row_ptr = &g_vram[((y + row++) % VRAM_HEIGHT) * VRAM_WIDTH];
      for (u32 col = 0; col < width;)
      {
        // TODO: Handle unaligned reads...
        u16* pixel_ptr = &dst_row_ptr[(x + col++) % VRAM_WIDTH];
        if (((*pixel_ptr) & mask_and) == 0)
          *pixel_ptr = *(src_ptr++) | mask_or;
      }
    }
  }
}

void GPU_SW_Backend::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                              GPUBackendCommandParameters params)
{
  // Break up oversized copies. This behavior has not been verified on console.
  if ((src_x + width) > VRAM_WIDTH || (dst_x + width) > VRAM_WIDTH)
  {
    u32 remaining_rows = height;
    u32 current_src_y = src_y;
    u32 current_dst_y = dst_y;
    while (remaining_rows > 0)
    {
      const u32 rows_to_copy =
        std::min<u32>(remaining_rows, std::min<u32>(VRAM_HEIGHT - current_src_y, VRAM_HEIGHT - current_dst_y));

      u32 remaining_columns = width;
      u32 current_src_x = src_x;
      u32 current_dst_x = dst_x;
      while (remaining_columns > 0)
      {
        const u32 columns_to_copy =
          std::min<u32>(remaining_columns, std::min<u32>(VRAM_WIDTH - current_src_x, VRAM_WIDTH - current_dst_x));
        CopyVRAM(current_src_x, current_src_y, current_dst_x, current_dst_y, columns_to_copy, rows_to_copy, params);
        current_src_x = (current_src_x + columns_to_copy) % VRAM_WIDTH;
        current_dst_x = (current_dst_x + columns_to_copy) % VRAM_WIDTH;
        remaining_columns -= columns_to_copy;
      }

      current_src_y = (current_src_y + rows_to_copy) % VRAM_HEIGHT;
      current_dst_y = (current_dst_y + rows_to_copy) % VRAM_HEIGHT;
      remaining_rows -= rows_to_copy;
    }

    return;
  }

  // This doesn't have a fast path, but do we really need one? It's not common.
  const u16 mask_and = params.GetMaskAND();
  const u16 mask_or = params.GetMaskOR();

  // Copy in reverse when src_x < dst_x, this is verified on console.
  if (src_x < dst_x || ((src_x + width - 1) % VRAM_WIDTH) < ((dst_x + width - 1) % VRAM_WIDTH))
  {
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &g_vram[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      for (s32 col = static_cast<s32>(width - 1); col >= 0; col--)
      {
        const u16 src_pixel = src_row_ptr[(src_x + static_cast<u32>(col)) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + static_cast<u32>(col)) % VRAM_WIDTH];
        if ((*dst_pixel_ptr & mask_and) == 0)
          *dst_pixel_ptr = src_pixel | mask_or;
      }
    }
  }
  else
  {
    for (u32 row = 0; row < height; row++)
    {
      const u16* src_row_ptr = &g_vram[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
      u16* dst_row_ptr = &g_vram[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];

      for (u32 col = 0; col < width; col++)
      {
        const u16 src_pixel = src_row_ptr[(src_x + col) % VRAM_WIDTH];
        u16* dst_pixel_ptr = &dst_row_ptr[(dst_x + col) % VRAM_WIDTH];
        if ((*dst_pixel_ptr & mask_and) == 0)
          *dst_pixel_ptr = src_pixel | mask_or;
      }
    }
  }
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
