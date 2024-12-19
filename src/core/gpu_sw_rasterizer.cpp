// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_sw_rasterizer.h"
#include "gpu.h"

#include "cpuinfo.h"

#include "common/gsvector.h"
#include "common/log.h"
#include "common/string_util.h"

LOG_CHANNEL(GPU_SW);

namespace GPU_SW_Rasterizer {
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

const DrawRectangleFunctionTable* DrawRectangleFunctions = nullptr;
const DrawTriangleFunctionTable* DrawTriangleFunctions = nullptr;
const DrawLineFunctionTable* DrawLineFunctions = nullptr;
FillVRAMFunction FillVRAM = nullptr;
WriteVRAMFunction WriteVRAM = nullptr;
CopyVRAMFunction CopyVRAM = nullptr;
GPUDrawingArea g_drawing_area = {};
} // namespace GPU_SW_Rasterizer

void GPU_SW_Rasterizer::UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit)
{
  const u16* const src_row = &g_vram[reg.GetYBase() * VRAM_WIDTH];
  const u32 start_x = reg.GetXBase();
  if (!clut_is_8bit)
  {
    // Wraparound can't happen in 4-bit mode.
    std::memcpy(g_gpu_clut, &src_row[start_x], sizeof(u16) * 16);
  }
  else
  {
    if ((start_x + 256) > VRAM_WIDTH) [[unlikely]]
    {
      const u32 end = VRAM_WIDTH - start_x;
      const u32 start = 256 - end;
      std::memcpy(g_gpu_clut, &src_row[start_x], sizeof(u16) * end);
      std::memcpy(g_gpu_clut + end, src_row, sizeof(u16) * start);
    }
    else
    {
      std::memcpy(g_gpu_clut, &src_row[start_x], sizeof(u16) * 256);
    }
  }
}

// Default scalar implementation definitions.
namespace GPU_SW_Rasterizer::Scalar {
namespace {
#include "gpu_sw_rasterizer.inl"
}
} // namespace GPU_SW_Rasterizer::Scalar

// Default vector implementation definitions.
#if defined(CPU_ARCH_SSE) || defined(CPU_ARCH_NEON)
namespace GPU_SW_Rasterizer::SIMD {
namespace {
#define USE_VECTOR 1
#include "gpu_sw_rasterizer.inl"
#undef USE_VECTOR
} // namespace
} // namespace GPU_SW_Rasterizer::SIMD
#endif

// Declare alternative implementations.
void GPU_SW_Rasterizer::SelectImplementation()
{
  static bool selected = false;
  if (selected)
    return;

  selected = true;

#define SELECT_IMPLEMENTATION(isa)                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    INFO_LOG("Using " #isa " software rasterizer implementation.");                                                    \
    DrawRectangleFunctions = &isa::DrawRectangleFunctions;                                                             \
    DrawTriangleFunctions = &isa::DrawTriangleFunctions;                                                               \
    DrawLineFunctions = &isa::DrawLineFunctions;                                                                       \
    FillVRAM = &isa::FillVRAMImpl;                                                                                     \
    WriteVRAM = &isa::WriteVRAMImpl;                                                                                   \
    CopyVRAM = &isa::CopyVRAMImpl;                                                                                     \
  } while (0)

#if defined(CPU_ARCH_SSE) || defined(CPU_ARCH_NEON)
  const char* use_isa = std::getenv("SW_USE_ISA");

  // AVX2/256-bit path still has issues, and I need to make sure that it's not ODR'ing any shared
  // symbols on top of the base symbols.
#if defined(CPU_ARCH_SSE) && defined(_MSC_VER) && 0
  if (cpuinfo_has_x86_avx2() && (!use_isa || StringUtil::Strcasecmp(use_isa, "AVX2") == 0))
  {
    SELECT_IMPLEMENTATION(AVX2);
    return;
  }
#endif

  if (!use_isa || StringUtil::Strcasecmp(use_isa, "SIMD") == 0)
  {
    SELECT_IMPLEMENTATION(SIMD);
    return;
  }
#endif

  INFO_LOG("Using scalar software rasterizer implementation.");
  SELECT_IMPLEMENTATION(Scalar);

#undef SELECT_IMPLEMENTATION
}
