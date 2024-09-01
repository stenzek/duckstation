// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu.h"
#include "gpu_backend.h"

#include <array>

class GPU_SW_Backend final : public GPUBackend
{
public:
  GPU_SW_Backend();
  ~GPU_SW_Backend() override;

  bool Initialize(bool force_thread) override;
  void Reset() override;

protected:
  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, GPUBackendCommandParameters params) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                GPUBackendCommandParameters params) override;

  void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) override;
  void DrawLine(const GPUBackendDrawLineCommand* cmd) override;
  void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) override;
  void DrawingAreaChanged(const GPUDrawingArea& new_drawing_area, const GSVector4i clamped_drawing_area) override;
  void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit) override;
  void FlushRender() override;
};
