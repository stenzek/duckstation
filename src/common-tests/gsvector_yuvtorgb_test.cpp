// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/bitutils.h"
#include "common/gsvector.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

static void YUVToRGB_Vector(const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                            const std::array<s16, 64>& Yblk, u32* output, bool signed_output)
{
  const GSVector4i addval = signed_output ? GSVector4i::cxpr(0) : GSVector4i::cxpr(0x80808080);
  for (u32 y = 0; y < 8; y++)
  {
    const GSVector4i Cr = GSVector4i::loadl(&Crblk[(y / 2) * 8]).s16to32();
    const GSVector4i Cb = GSVector4i::loadl(&Cbblk[(y / 2) * 8]).s16to32();
    const GSVector4i Y = GSVector4i::load<true>(&Yblk[y * 8]);

    // BT.601 YUV->RGB coefficients, rounding formula from Mednafen.
    // r = clamp(sext9(Y + (((359 * Cr) + 0x80) >> 8)), -128, 127) + addval;
    // g = clamp(sext9(Y + ((((-88 * Cb) & ~0x1F) + ((-183 * Cr) & ~0x07) + 0x80) >> 8)), -128, 127) + addval
    // b = clamp(sext9<9, s32>(Y + (((454 * Cb) + 0x80) >> 8)), -128, 127) + addval

    // Need to do the multiply as 32-bit, since 127 * 359 is greater than INT16_MAX.
    // upl16(self) = interleave XYZW0000 -> XXYYZZWW.
    const GSVector4i Crmul = Cr.mul32l(GSVector4i::cxpr(359)).add16(GSVector4i::cxpr(0x80)).sra32<8>().ps32();
    const GSVector4i Cbmul = Cb.mul32l(GSVector4i::cxpr(454)).add16(GSVector4i::cxpr(0x80)).sra32<8>().ps32();
    const GSVector4i CrCbmul = (Cb.mul32l(GSVector4i::cxpr(-88)) & GSVector4i::cxpr(~0x1F))
                                 .add32(Cr.mul32l(GSVector4i::cxpr(-183)) & GSVector4i::cxpr(~0x07))
                                 .add32(GSVector4i::cxpr(0x80))
                                 .sra32<8>()
                                 .ps32();
    const GSVector4i r = Crmul.upl16(Crmul).add16(Y).sll16<7>().sra16<7>().ps16().add8(addval);
    const GSVector4i g = CrCbmul.upl16(CrCbmul).add16(Y).sll16<7>().sra16<7>().ps16().add8(addval);
    const GSVector4i b = Cbmul.upl16(Cbmul).add16(Y).sll16<7>().sra16<7>().ps16().add8(addval);
    const GSVector4i rg = r.upl8(g);
    const GSVector4i b0 = b.upl8();
    const GSVector4i rgblow = rg.upl16(b0);
    const GSVector4i rgbhigh = rg.uph16(b0);

    GSVector4i::store<false>(&output[y * 8 + 0], rgblow);
    GSVector4i::store<false>(&output[y * 8 + 4], rgbhigh);
  }
}

static void YUVToRGB_Scalar(const std::array<s16, 64>& Crblk, const std::array<s16, 64>& Cbblk,
                            const std::array<s16, 64>& Yblk, u32* output, bool signed_output)
{
  const s32 addval = signed_output ? 0 : 0x80;
  for (u32 y = 0; y < 8; y++)
  {
    for (u32 x = 0; x < 8; x++)
    {
      const s32 Cr = Crblk[(x / 2) + (y / 2) * 8];
      const s32 Cb = Cbblk[(x / 2) + (y / 2) * 8];
      const s32 Y = Yblk[x + y * 8];

      // BT.601 YUV->RGB coefficients, rounding from Mednafen.
      const s32 r = std::clamp(SignExtendN<9, s32>(Y + (((359 * Cr) + 0x80) >> 8)), -128, 127) + addval;
      const s32 g =
        std::clamp(SignExtendN<9, s32>(Y + ((((-88 * Cb) & ~0x1F) + ((-183 * Cr) & ~0x07) + 0x80) >> 8)), -128, 127) +
        addval;
      const s32 b = std::clamp(SignExtendN<9, s32>(Y + (((454 * Cb) + 0x80) >> 8)), -128, 127) + addval;

      output[y * 8 + x] =
        static_cast<u32>(Truncate8(r)) | (static_cast<u32>(Truncate8(g)) << 8) | (static_cast<u32>(Truncate8(b)) << 16);
    }
  }
}

TEST(GSVector, YUVToRGB)
{
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> crblk;
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> cbblk;
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> yblk;
  for (s16 i = -128; i < 128; i++)
  {
    for (u32 j = 0; j < 64; j++)
      crblk[j] = i;

    for (s16 k = -128; k < 128; k++)
    {
      for (u32 j = 0; j < 64; j++)
        cbblk[j] = k;

      for (s16 l = -128; l < 128; l++)
      {
        for (u32 j = 0; j < 64; j++)
          yblk[j] = l;

        alignas(VECTOR_ALIGNMENT) u32 rows[64];
        YUVToRGB_Scalar(crblk, cbblk, yblk, rows, false);

        alignas(VECTOR_ALIGNMENT) u32 rowv[64];
        YUVToRGB_Vector(crblk, cbblk, yblk, rowv, false);
        ASSERT_EQ(std::memcmp(rows, rowv, sizeof(rows)), 0);

        YUVToRGB_Scalar(crblk, cbblk, yblk, rows, true);
        YUVToRGB_Vector(crblk, cbblk, yblk, rowv, true);
        ASSERT_EQ(std::memcmp(rows, rowv, sizeof(rows)), 0);
      }
    }
  }
}

#if 0
// Performance test
alignas(VECTOR_ALIGNMENT) u32 g_gsvector_yuvtorgb_temp[64];

TEST(GSVector, YUVToRGB_Scalar)
{
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> crblk;
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> cbblk;
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> yblk;
  for (s16 i = -128; i < 128; i++)
  {
    for (u32 j = 0; j < 64; j++)
      crblk[j] = i;

    for (s16 k = -128; k < 128; k++)
    {
      for (u32 j = 0; j < 64; j++)
        cbblk[j] = k;

      for (s16 l = -128; l < 128; l++)
      {
        for (u32 j = 0; j < 64; j++)
          yblk[j] = l;

        YUVToRGB_Scalar(crblk, cbblk, yblk, g_gsvector_yuvtorgb_temp, false);
      }
    }
  }
}

TEST(GSVector, YUVToRGB_Vector)
{
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> crblk;
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> cbblk;
  alignas(VECTOR_ALIGNMENT) std::array<s16, 64> yblk;
  for (s16 i = -128; i < 128; i++)
  {
    for (u32 j = 0; j < 64; j++)
      crblk[j] = i;

    for (s16 k = -128; k < 128; k++)
    {
      for (u32 j = 0; j < 64; j++)
        cbblk[j] = k;

      for (s16 l = -128; l < 128; l++)
      {
        for (u32 j = 0; j < 64; j++)
          yblk[j] = l;

        YUVToRGB_Vector(crblk, cbblk, yblk, g_gsvector_yuvtorgb_temp, false);
      }
    }
  }
}

#endif
