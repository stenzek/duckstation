// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_sw_rasterizer.h"
#include "gpu.h"

#include "cpuinfo.h"

#include "common/log.h"
#include "common/string_util.h"

Log_SetChannel(GPU_SW_Rasterizer);

namespace GPU_SW_Rasterizer {
// Default implementation, compatible with all ISAs.
extern const DrawRectangleFunctionTable DrawRectangleFunctions;
extern const DrawTriangleFunctionTable DrawTriangleFunctions;
extern const DrawLineFunctionTable DrawLineFunctions;

constinit const DitherLUT g_dither_lut = []() constexpr {
  DitherLUT lut = {};
  for (u32 i = 0; i < DITHER_MATRIX_SIZE; i++)
  {
    for (u32 j = 0; j < DITHER_MATRIX_SIZE; j++)
    {
      for (u32 value = 0; value < DITHER_LUT_SIZE; value++)
      {
        const s32 dithered_value = (static_cast<s32>(value) + DITHER_MATRIX[i][j]) >> 3;
        lut[i][j][value] = static_cast<u8>((dithered_value < 0) ? 0 : ((dithered_value > 31) ? 31 : dithered_value));
      }
    }
  }
  return lut;
}();

GPUDrawingArea g_drawing_area = {};
} // namespace GPU_SW_Rasterizer

// Default implementation definitions.
namespace GPU_SW_Rasterizer {
#include "gpu_sw_rasterizer.inl"
}

// Default vector implementation definitions.
#if defined(CPU_ARCH_SSE) || defined(CPU_ARCH_NEON)
namespace GPU_SW_Rasterizer::SIMD {
#include "gpu_sw_rasterizer.inl"
}
#endif

// Initialize with default implementation.
namespace GPU_SW_Rasterizer {
const DrawRectangleFunctionTable* SelectedDrawRectangleFunctions = &DrawRectangleFunctions;
const DrawTriangleFunctionTable* SelectedDrawTriangleFunctions = &DrawTriangleFunctions;
const DrawLineFunctionTable* SelectedDrawLineFunctions = &DrawLineFunctions;
} // namespace GPU_SW_Rasterizer

// Declare alternative implementations.
void GPU_SW_Rasterizer::SelectImplementation()
{
  static bool selected = false;
  if (selected)
    return;

  selected = true;

#define SELECT_ALTERNATIVE_RASTERIZER(isa)                                                                             \
  do                                                                                                                   \
  {                                                                                                                    \
    INFO_LOG("Using " #isa " software rasterizer implementation.");                                                    \
    SelectedDrawRectangleFunctions = &isa::DrawRectangleFunctions;                                                     \
    SelectedDrawTriangleFunctions = &isa::DrawTriangleFunctions;                                                       \
    SelectedDrawLineFunctions = &isa::DrawLineFunctions;                                                               \
  } while (0)

#if defined(CPU_ARCH_SSE) || defined(CPU_ARCH_NEON)
  const char* use_isa = std::getenv("SW_USE_ISA");

  // Default to scalar for now, until vector is finished.
  use_isa = use_isa ? use_isa : "Scalar";

#if defined(CPU_ARCH_SSE) && defined(_MSC_VER)
  if (cpuinfo_has_x86_avx2() && (!use_isa || StringUtil::Strcasecmp(use_isa, "AVX2") == 0))
  {
    SELECT_ALTERNATIVE_RASTERIZER(AVX2);
    return;
  }
#endif

  if (!use_isa || StringUtil::Strcasecmp(use_isa, "SIMD") == 0)
  {
    SELECT_ALTERNATIVE_RASTERIZER(SIMD);
    return;
  }
#endif

  INFO_LOG("Using scalar software rasterizer implementation.");

#undef SELECT_ALTERNATIVE_RASTERIZER
}
