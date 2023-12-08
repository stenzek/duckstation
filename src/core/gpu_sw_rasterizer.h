// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu.h"
#include "gpu_types.h"

#include "common/intrin.h"
#include "common/types.h"

#include <array>

namespace GPU_SW_Rasterizer {

// this is actually (31 * 255) >> 4) == 494, but to simplify addressing we use the next power of two (512)
static constexpr u32 DITHER_LUT_SIZE = 512;
using DitherLUT = std::array<std::array<std::array<u8, DITHER_LUT_SIZE>, DITHER_MATRIX_SIZE>, DITHER_MATRIX_SIZE>;
extern const DitherLUT g_dither_lut;

// TODO: Pack in struct
extern GPUDrawingArea g_drawing_area;

extern void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit);

using DrawRectangleFunction = void (*)(const GPUBackendDrawRectangleCommand* cmd);
typedef const DrawRectangleFunction DrawRectangleFunctionTable[2][2][2];

using DrawTriangleFunction = void (*)(const GPUBackendDrawCommand* cmd,
                                      const GPUBackendDrawPolygonCommand::Vertex* v0,
                                      const GPUBackendDrawPolygonCommand::Vertex* v1,
                                      const GPUBackendDrawPolygonCommand::Vertex* v2);
typedef const DrawTriangleFunction DrawTriangleFunctionTable[2][2][2][2];

using DrawLineFunction = void (*)(const GPUBackendDrawLineCommand* cmd, const GPUBackendDrawLineCommand::Vertex* p0,
                                  const GPUBackendDrawLineCommand::Vertex* p1);
typedef const DrawLineFunction DrawLineFunctionTable[2][2];

using FillVRAMFunction = void (*)(u32 x, u32 y, u32 width, u32 height, u32 color, bool interlaced, u8 active_line_lsb);
using WriteVRAMFunction = void (*)(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask,
                                   bool check_mask);
using CopyVRAMFunction = void (*)(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height, bool set_mask,
                                  bool check_mask);

// Current implementation, selected at runtime.
extern const DrawRectangleFunctionTable* DrawRectangleFunctions;
extern const DrawTriangleFunctionTable* DrawTriangleFunctions;
extern const DrawLineFunctionTable* DrawLineFunctions;
extern FillVRAMFunction FillVRAM;
extern WriteVRAMFunction WriteVRAM;
extern CopyVRAMFunction CopyVRAM;

extern void SelectImplementation();

ALWAYS_INLINE static DrawLineFunction GetDrawLineFunction(bool shading_enable, bool transparency_enable)
{
  return (*DrawLineFunctions)[u8(shading_enable)][u8(transparency_enable)];
}

ALWAYS_INLINE static DrawRectangleFunction GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                                    bool transparency_enable)
{
  return (*DrawRectangleFunctions)[u8(texture_enable)][u8(raw_texture_enable)][u8(transparency_enable)];
}

ALWAYS_INLINE static DrawTriangleFunction GetDrawTriangleFunction(bool shading_enable, bool texture_enable,
                                                                  bool raw_texture_enable, bool transparency_enable)
{
  return (
    *DrawTriangleFunctions)[u8(shading_enable)][u8(texture_enable)][u8(raw_texture_enable)][u8(transparency_enable)];
}

#define DECLARE_ALTERNATIVE_RASTERIZER(isa)                                                                            \
  namespace isa {                                                                                                      \
  extern const DrawRectangleFunctionTable DrawRectangleFunctions;                                                      \
  extern const DrawTriangleFunctionTable DrawTriangleFunctions;                                                        \
  extern const DrawLineFunctionTable DrawLineFunctions;                                                                \
  }

// Have to define the symbols globally, because clang won't include them otherwise.
#if defined(CPU_ARCH_SSE) && 0
#define ALTERNATIVE_RASTERIZER_LIST() DECLARE_ALTERNATIVE_RASTERIZER(AVX2)
#else
#define ALTERNATIVE_RASTERIZER_LIST()
#endif

ALTERNATIVE_RASTERIZER_LIST()

#undef DECLARE_ALTERNATIVE_RASTERIZER

} // namespace GPU_SW_Rasterizer
