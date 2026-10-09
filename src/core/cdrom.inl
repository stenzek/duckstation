// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// XA-ADPCM resampling filters. Kept separate from cdrom.cpp so that the tests can include them.

#include "common/gsvector.h"
#include "common/types.h"

#include <algorithm>
#include <array>

namespace CDROM {

inline constexpr u32 XA_RESAMPLE_RING_BUFFER_SIZE = 32;
inline constexpr u32 XA_RESAMPLE_RING_BUFFER_MASK = XA_RESAMPLE_RING_BUFFER_SIZE - 1;
inline constexpr u32 XA_RESAMPLE_NUM_TABLES = 7;

#ifdef CPU_ARCH_SIMD

// Both filters are a dot product of the most recent samples in the ring buffer with a table of coefficients. Reading
// the samples in filter order means wrapping around the ring buffer, which cannot be done with a vector load. Since
// the order of a sum does not matter, the coefficients are rotated to match the ring buffer instead: sample k is
// always multiplied by coefficient ((k + offset) % 32), where offset depends only on the write position.
//
// To make every rotation loadable, each table is padded with zeros to the size of the ring buffer (so that samples
// outside of the filter contribute nothing), and then stored twice back-to-back. The 32 coefficients starting at
// offset are then the table rotated by offset.
using XAResampleTable = std::array<s16, XA_RESAMPLE_RING_BUFFER_SIZE * 2>;

template<size_t NUM_TAPS>
inline consteval std::array<XAResampleTable, XA_RESAMPLE_NUM_TABLES>
MakeXAResampleTables(const std::array<std::array<s16, NUM_TAPS>, XA_RESAMPLE_NUM_TABLES>& taps, bool reverse)
{
  static_assert(NUM_TAPS <= XA_RESAMPLE_RING_BUFFER_SIZE);

  std::array<XAResampleTable, XA_RESAMPLE_NUM_TABLES> ret = {};
  for (size_t table = 0; table < XA_RESAMPLE_NUM_TABLES; table++)
  {
    for (size_t i = 0; i < NUM_TAPS; i++)
    {
      const s16 tap = taps[table][reverse ? (NUM_TAPS - 1 - i) : i];
      ret[table][i] = tap;
      ret[table][i + XA_RESAMPLE_RING_BUFFER_SIZE] = tap;
    }
  }

  return ret;
}

#endif // CPU_ARCH_SIMD

// 37800hz -> 44100hz, producing 7 output samples (one per table) for every 6 input samples.
// Computes sum((ringbuf[(p - i) % 32] * taps[table_index][i]) >> 15) for each of the 29 taps.
[[maybe_unused]] inline s16 XAZigZagInterpolate(const s16* ringbuf, u32 table_index, u32 p)
{
  static constexpr u32 NUM_TAPS = 29;
  static constexpr std::array<std::array<s16, NUM_TAPS>, XA_RESAMPLE_NUM_TABLES> taps = {
    {{0,      0x0,     0x0,     0x0,    0x0,     -0x0002, 0x000A,  -0x0022, 0x0041, -0x0054,
      0x0034, 0x0009,  -0x010A, 0x0400, -0x0A78, 0x234C,  0x6794,  -0x1780, 0x0BCD, -0x0623,
      0x0350, -0x016D, 0x006B,  0x000A, -0x0010, 0x0011,  -0x0008, 0x0003,  -0x0001},
     {0,       0x0,    0x0,     -0x0002, 0x0,    0x0003,  -0x0013, 0x003C,  -0x004B, 0x00A2,
      -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB,  -0x11B4, 0x09B8,  -0x05BF, 0x0372,
      -0x01A8, 0x00A6, -0x001B, 0x0005,  0x0006, -0x0008, 0x0003,  -0x0001, 0x0},
     {0,      0x0,     -0x0001, 0x0003,  -0x0002, -0x0005, 0x001F,  -0x004A, 0x00B3, -0x0192,
      0x02B1, -0x039E, 0x04F8,  -0x05A6, 0x7939,  -0x05A6, 0x04F8,  -0x039E, 0x02B1, -0x0192,
      0x00B3, -0x004A, 0x001F,  -0x0005, -0x0002, 0x0003,  -0x0001, 0x0,     0x0},
     {0,       -0x0001, 0x0003,  -0x0008, 0x0006, 0x0005,  -0x001B, 0x00A6, -0x01A8, 0x0372,
      -0x05BF, 0x09B8,  -0x11B4, 0x74BB,  0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2,
      -0x004B, 0x003C,  -0x0013, 0x0003,  0x0,    -0x0002, 0x0,     0x0,    0x0},
     {-0x0001, 0x0003,  -0x0008, 0x0011,  -0x0010, 0x000A, 0x006B,  -0x016D, 0x0350, -0x0623,
      0x0BCD,  -0x1780, 0x6794,  0x234C,  -0x0A78, 0x0400, -0x010A, 0x0009,  0x0034, -0x0054,
      0x0041,  -0x0022, 0x000A,  -0x0001, 0x0,     0x0001, 0x0,     0x0,     0x0},
     {0x0002,  -0x0008, 0x0010,  -0x0023, 0x002B, 0x001A,  -0x00EB, 0x027B,  -0x0548, 0x0AFA,
      -0x16FA, 0x53E0,  0x3C07,  -0x1249, 0x080E, -0x0347, 0x015B,  -0x0044, -0x0017, 0x0046,
      -0x0023, 0x0011,  -0x0005, 0x0,     0x0,    0x0,     0x0,     0x0,     0x0},
     {-0x0005, 0x0011,  -0x0023, 0x0046, -0x0017, -0x0044, 0x015B,  -0x0347, 0x080E, -0x1249,
      0x3C07,  0x53E0,  -0x16FA, 0x0AFA, -0x0548, 0x027B,  -0x00EB, 0x001A,  0x002B, -0x0023,
      0x0010,  -0x0008, 0x0002,  0x0,    0x0,     0x0,     0x0,     0x0,     0x0}}};

#ifdef CPU_ARCH_SIMD
  // The taps walk backwards through the ring buffer, so the table is reversed to walk forwards instead.
  // ringbuf[k] is paired with taps[(p - k) % 32], which is reversed[(k + NUM_TAPS - 1 - p) % 32].
  static constexpr auto tables = MakeXAResampleTables(taps, true);
  const s16* const coeffs = &tables[table_index][(NUM_TAPS - 1 - p) & XA_RESAMPLE_RING_BUFFER_MASK];

  // Each product is shifted down before it is summed, which rules out letting madd_s16() add adjacent products.
  // Zeroing every second coefficient makes one half of each pair zero, leaving the other product intact.
  static constexpr GSVector4i even_mask = GSVector4i::cxpr(0x0000FFFF);

  GSVector4i sum = GSVector4i::zero();
  for (u32 k = 0; k < XA_RESAMPLE_RING_BUFFER_SIZE; k += 8)
  {
    const GSVector4i samples = GSVector4i::load<false>(&ringbuf[k]);
    const GSVector4i coeffs_k = GSVector4i::load<false>(&coeffs[k]);
    sum = sum.add32(samples.madd_s16(coeffs_k & even_mask).sra32<15>());
    sum = sum.add32(samples.madd_s16(coeffs_k.andnot(even_mask)).sra32<15>());
  }

  return static_cast<s16>(std::clamp<s32>(sum.addv_s32(), -0x8000, 0x7FFF));
#else
  // Without vector instructions, GSVector would multiply the padding and masked-out coefficients one at a time.
  const s16* const table = taps[table_index].data();
  s32 sum = 0;
  for (u32 i = 0; i < NUM_TAPS; i++)
    sum += (static_cast<s32>(ringbuf[(p - i) & XA_RESAMPLE_RING_BUFFER_MASK]) * static_cast<s32>(table[i])) >> 15;

  return static_cast<s16>(std::clamp<s32>(sum, -0x8000, 0x7FFF));
#endif
}

// 18900hz -> 44100hz, producing 7 output samples (one per table) for every 3 input samples.
// Computes sum(ringbuf[(p - 25 + i) % 32] * taps[table_index][i]) >> 15 for each of the 25 taps.
[[maybe_unused]] inline s16 XAInterpolate18900(const s16* ringbuf, u32 table_index, u32 p)
{
  // Weights originally from Mednafen's interpolator. It's unclear where these came from, perhaps it was calculated
  // somehow. This doesn't appear to use a zigzag pattern like psx-spx suggests, therefore it is restricted to only
  // 18900hz resampling. Duplicating the 18900hz samples to 37800hz sounds even more awful than lower sample rate
  // audio should, with a big spike at ~16KHz, especially with music in FMVs. Fortunately, few games actually use
  // 18900hz XA.
  static constexpr u32 NUM_TAPS = 25;
  static constexpr std::array<std::array<s16, NUM_TAPS>, XA_RESAMPLE_NUM_TABLES> taps = {{
    {{0x0,     -0x5,  0x11,   -0x23, 0x46,  -0x17, -0x44, 0x15b, -0x347, 0x80e, -0x1249, 0x3c07, 0x53e0,
      -0x16fa, 0xafa, -0x548, 0x27b, -0xeb, 0x1a,  0x2b,  -0x23, 0x10,   -0x8,  0x2,     0x0}},
    {{0x0,     -0x2,  0xa,    -0x22, 0x41,   -0x54, 0x34, 0x9,   -0x10a, 0x400, -0xa78, 0x234c, 0x6794,
      -0x1780, 0xbcd, -0x623, 0x350, -0x16d, 0x6b,  0xa,  -0x10, 0x11,   -0x8,  0x3,    -0x1}},
    {{-0x2,    0x0,   0x3,    -0x13, 0x3c,   -0x4b, 0xa2,  -0xe3, 0x132, -0x43, -0x267, 0xc9d, 0x74bb,
      -0x11b4, 0x9b8, -0x5bf, 0x372, -0x1a8, 0xa6,  -0x1b, 0x5,   0x6,   -0x8,  0x3,    -0x1}},
    {{-0x1,   0x3,   -0x2,   -0x5,  0x1f,   -0x4a, 0xb3,  -0x192, 0x2b1, -0x39e, 0x4f8, -0x5a6, 0x7939,
      -0x5a6, 0x4f8, -0x39e, 0x2b1, -0x192, 0xb3,  -0x4a, 0x1f,   -0x5,  -0x2,   0x3,   -0x1}},
    {{-0x1,  0x3,    -0x8,  0x6,   0x5,   -0x1b, 0xa6,  -0x1a8, 0x372, -0x5bf, 0x9b8, -0x11b4, 0x74bb,
      0xc9d, -0x267, -0x43, 0x132, -0xe3, 0xa2,  -0x4b, 0x3c,   -0x13, 0x3,    0x0,   -0x2}},
    {{-0x1,   0x3,    -0x8,  0x11,   -0x10, 0xa,  0x6b,  -0x16d, 0x350, -0x623, 0xbcd, -0x1780, 0x6794,
      0x234c, -0xa78, 0x400, -0x10a, 0x9,   0x34, -0x54, 0x41,   -0x22, 0xa,    -0x2,  0x0}},
    {{0x0,    0x2,     -0x8,  0x10,   -0x23, 0x2b,  0x1a,  -0xeb, 0x27b, -0x548, 0xafa, -0x16fa, 0x53e0,
      0x3c07, -0x1249, 0x80e, -0x347, 0x15b, -0x44, -0x17, 0x46,  -0x23, 0x11,   -0x5,  0x0}},
  }};

#ifdef CPU_ARCH_SIMD
  // ringbuf[k] is paired with taps[(k + NUM_TAPS - p) % 32].
  static constexpr auto tables = MakeXAResampleTables(taps, false);
  const s16* const coeffs = &tables[table_index][(NUM_TAPS - p) & XA_RESAMPLE_RING_BUFFER_MASK];

  GSVector4i sum = GSVector4i::zero();
  for (u32 k = 0; k < XA_RESAMPLE_RING_BUFFER_SIZE; k += 8)
    sum = sum.add32(GSVector4i::load<false>(&ringbuf[k]).madd_s16(GSVector4i::load<false>(&coeffs[k])));

  return static_cast<s16>(std::clamp<s32>(sum.addv_s32() >> 15, -0x8000, 0x7FFF));
#else
  const s16* const table = taps[table_index].data();
  s32 sum = 0;
  for (u32 i = 0; i < NUM_TAPS; i++)
    sum += static_cast<s32>(ringbuf[(p - NUM_TAPS + i) & XA_RESAMPLE_RING_BUFFER_MASK]) * static_cast<s32>(table[i]);

  return static_cast<s16>(std::clamp<s32>(sum >> 15, -0x8000, 0x7FFF));
#endif
}

} // namespace CDROM
