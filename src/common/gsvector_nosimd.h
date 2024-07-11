// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team, 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: LGPL-3.0+

// Implementation of GSVector4/GSVector4i when the host does not support any form of SIMD.

#pragma once

#include "common/types.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define GSVECTOR_HAS_UNSIGNED 1
#define GSVECTOR_HAS_SRLV 1

class GSVector2;
class GSVector2i;
class GSVector4;
class GSVector4i;

#define SSATURATE8(expr) static_cast<s8>(std::clamp<decltype(expr)>(expr, -128, 127))
#define USATURATE8(expr) static_cast<u8>(std::clamp<decltype(expr)>(expr, 0, 255))
#define SSATURATE16(expr) static_cast<s16>(std::clamp<decltype(expr)>(expr, -32768, 32767))
#define USATURATE16(expr) static_cast<u16>(std::clamp<decltype(expr)>(expr, 0, 65535))

#define ALL_LANES_8(expr)                                                                                              \
  GSVector2i ret;                                                                                                      \
  for (size_t i = 0; i < 8; i++)                                                                                       \
    expr;                                                                                                              \
  return ret;
#define ALL_LANES_16(expr)                                                                                             \
  GSVector2i ret;                                                                                                      \
  for (size_t i = 0; i < 4; i++)                                                                                       \
    expr;                                                                                                              \
  return ret;
#define ALL_LANES_32(expr)                                                                                             \
  GSVector2i ret;                                                                                                      \
  for (size_t i = 0; i < 2; i++)                                                                                       \
    expr;                                                                                                              \
  return ret;

class alignas(16) GSVector2i
{
  struct cxpr_init_tag
  {
  };
  static constexpr cxpr_init_tag cxpr_init{};

  constexpr GSVector2i(cxpr_init_tag, s32 x, s32 y) : I32{x, y} {}

  constexpr GSVector2i(cxpr_init_tag, s16 s0, s16 s1, s16 s2, s16 s3) : I16{s0, s1, s2, s3} {}

  constexpr GSVector2i(cxpr_init_tag, s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7)
    : I8{b0, b1, b2, b3, b4, b5, b6, b7}
  {
  }

public:
  union
  {
    struct
    {
      s32 x, y;
    };
    struct
    {
      s32 r, g;
    };
    float F32[2];
    s8 I8[8];
    s16 I16[4];
    s32 I32[2];
    s64 I64[1];
    u8 U8[8];
    u16 U16[4];
    u32 U32[2];
    u64 U64[1];
  };

  GSVector2i() = default;

  ALWAYS_INLINE constexpr static GSVector2i cxpr(s32 x, s32 y) { return GSVector2i(cxpr_init, x, y); }

  ALWAYS_INLINE constexpr static GSVector2i cxpr(s32 x) { return GSVector2i(cxpr_init, x, x); }

  ALWAYS_INLINE constexpr static GSVector2i cxpr16(s16 x) { return GSVector2i(cxpr_init, x, x, x, x); }

  ALWAYS_INLINE constexpr static GSVector2i cxpr16(s16 s0, s16 s1, s16 s2, s16 s3)
  {
    return GSVector2i(cxpr_init, s0, s1, s2, s3);
  }

  ALWAYS_INLINE constexpr static GSVector2i cxpr8(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7)
  {
    return GSVector2i(cxpr_init, b0, b1, b2, b3, b4, b5, b6, b7);
  }

  ALWAYS_INLINE GSVector2i(s32 x, s32 y)
  {
    this->x = x;
    this->y = y;
  }

  ALWAYS_INLINE GSVector2i(s16 s0, s16 s1, s16 s2, s16 s3)
  {
    I16[0] = s0;
    I16[1] = s1;
    I16[2] = s2;
    I16[3] = s3;
  }

  ALWAYS_INLINE constexpr GSVector2i(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7)
    : I8{b0, b1, b2, b3, b4, b5, b6, b7}
  {
  }

  ALWAYS_INLINE explicit GSVector2i(const GSVector2i& v) { std::memcpy(I32, v.I32, sizeof(I32)); }

  // MSVC has bad codegen for the constexpr version when applied to non-constexpr things (https://godbolt.org/z/h8qbn7),
  // so leave the non-constexpr version default
  ALWAYS_INLINE explicit GSVector2i(s32 i) { *this = i; }

  ALWAYS_INLINE explicit GSVector2i(const GSVector2& v, bool truncate = true);

  ALWAYS_INLINE static GSVector2i cast(const GSVector2& v);

  ALWAYS_INLINE void operator=(const GSVector2i& v) { std::memcpy(I32, v.I32, sizeof(I32)); }
  ALWAYS_INLINE void operator=(s32 i)
  {
    x = i;
    y = i;
  }

  ALWAYS_INLINE GSVector2i sat_i8(const GSVector2i& min, const GSVector2i& max) const
  {
    return max_i8(min).min_i8(max);
  }
  ALWAYS_INLINE GSVector2i sat_i16(const GSVector2i& min, const GSVector2i& max) const
  {
    return max_i16(min).min_i16(max);
  }
  ALWAYS_INLINE GSVector2i sat_i32(const GSVector2i& min, const GSVector2i& max) const
  {
    return max_i32(min).min_i32(max);
  }

  ALWAYS_INLINE GSVector2i sat_u8(const GSVector2i& min, const GSVector2i& max) const
  {
    return max_u8(min).min_u8(max);
  }
  ALWAYS_INLINE GSVector2i sat_u16(const GSVector2i& min, const GSVector2i& max) const
  {
    return max_u16(min).min_u16(max);
  }
  ALWAYS_INLINE GSVector2i sat_u32(const GSVector2i& min, const GSVector2i& max) const
  {
    return max_u32(min).min_u32(max);
  }

  GSVector2i min_i8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = std::min(I8[i], v.I8[i])); }
  GSVector2i max_i8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = std::max(I8[i], v.I8[i])); }
  GSVector2i min_i16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = std::min(I16[i], v.I16[i])); }
  GSVector2i max_i16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = std::max(I16[i], v.I16[i])); }
  GSVector2i min_i32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = std::min(I32[i], v.I32[i])); }
  GSVector2i max_i32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = std::max(I32[i], v.I32[i])); }

  GSVector2i min_u8(const GSVector2i& v) const { ALL_LANES_8(ret.U8[i] = std::min(U8[i], v.U8[i])); }
  GSVector2i max_u8(const GSVector2i& v) const { ALL_LANES_8(ret.U8[i] = std::max(U8[i], v.U8[i])); }
  GSVector2i min_u16(const GSVector2i& v) const { ALL_LANES_16(ret.U16[i] = std::min(U16[i], v.U16[i])); }
  GSVector2i max_u16(const GSVector2i& v) const { ALL_LANES_16(ret.U16[i] = std::max(U16[i], v.U16[i])); }
  GSVector2i min_u32(const GSVector2i& v) const { ALL_LANES_32(ret.U32[i] = std::min(U32[i], v.U32[i])); }
  GSVector2i max_u32(const GSVector2i& v) const { ALL_LANES_32(ret.U32[i] = std::max(U32[i], v.U32[i])); }

  u8 minv_u8() const
  {
    return std::min(
      U8[0],
      std::min(U8[1], std::min(U8[2], std::min(U8[3], std::min(U8[4], std::min(U8[5], std::min(U8[6], U8[7])))))));
  }

  u16 maxv_u8() const
  {
    return std::max(
      U8[0],
      std::max(U8[1], std::max(U8[2], std::max(U8[3], std::max(U8[4], std::max(U8[5], std::max(U8[6], U8[7])))))));
  }

  u16 minv_u16() const { return std::min(U16[0], std::min(U16[1], std::min(U16[2], U16[3]))); }

  u16 maxv_u16() const { return std::max(U16[0], std::max(U16[1], std::max(U16[2], U16[3]))); }

  s32 minv_s32() const { return std::min(x, y); }

  u32 minv_u32() const { return std::min(U32[0], U32[1]); }

  s32 maxv_s32() const { return std::max(x, y); }

  u32 maxv_u32() const { return std::max(U32[0], U32[1]); }

  ALWAYS_INLINE GSVector2i clamp8() const { return pu16().upl8(); }

  GSVector2i blend8(const GSVector2i& v, const GSVector2i& mask) const
  {
    GSVector2i ret;
    for (size_t i = 0; i < 8; i++)
      ret.U8[i] = (mask.U8[i] & 0x80) ? v.U8[i] : U8[i];
    return ret;
  }

  template<s32 mask>
  GSVector2i blend16(const GSVector2i& v) const
  {
    GSVector2i ret;
    for (size_t i = 0; i < 4; i++)
      ret.U16[i] = ((mask & (1 << i)) != 0) ? v.U16[i] : U16[i];
    return ret;
  }

  template<s32 mask>
  GSVector2i blend32(const GSVector2i& v) const
  {
    GSVector2i ret;
    for (size_t i = 0; i < 2; i++)
      ret.U32[i] = ((mask & (1 << i)) != 0) ? v.U32[i] : U32[i];
    return ret;
  }

  GSVector2i blend(const GSVector2i& v, const GSVector2i& mask) const
  {
    GSVector2i ret;
    ret.U64[0] = (v.U64[0] & mask.U64[0]);
    return ret;
  }

  ALWAYS_INLINE GSVector2i mix16(const GSVector2i& v) const { return blend16<0xa>(v); }

  GSVector2i shuffle8(const GSVector2i& mask) const
  {
    ALL_LANES_8(ret.I8[i] = (mask.I8[i] & 0x80) ? 0 : (I8[mask.I8[i] & 0xf]));
  }

  GSVector2i ps16() const { ALL_LANES_8(ret.I8[i] = SSATURATE8(I16[(i < 4) ? i : (i - 4)])); }
  GSVector2i pu16() const { ALL_LANES_8(ret.U8[i] = USATURATE8(U16[(i < 4) ? i : (i - 4)])); }
  GSVector2i ps32() const { ALL_LANES_16(ret.I16[i] = SSATURATE16(I32[(i < 2) ? i : (i - 2)])); }
  GSVector2i pu32() const { ALL_LANES_16(ret.U16[i] = USATURATE16(U32[(i < 2) ? i : (i - 2)])); }

  GSVector2i upl8() const { return GSVector2i(I8[0], 0, I8[1], 0, I8[2], 0, I8[3], 0); }

  GSVector2i upl16() const { return GSVector2i(I16[0], 0, I16[1], 0); }

  GSVector2i upl32() const { return GSVector2i(I32[0], 0); }

  GSVector2i i8to16() const { ALL_LANES_16(ret.I16[i] = I8[i]); }

  template<s32 v>
  GSVector2i srl() const
  {
    GSVector2i ret = {};
    if constexpr (v < 8)
    {
      for (s32 i = 0; i < (8 - v); i++)
        ret.U8[i] = U8[v + i];
    }
    return ret;
  }

  template<s32 v>
  GSVector2i sll() const
  {
    GSVector2i ret = {};
    if constexpr (v < 8)
    {
      for (s32 i = 0; i < (8 - v); i++)
        ret.U8[v + i] = U8[i];
    }
    return ret;
  }

  template<s32 v>
  GSVector2i sll16() const
  {
    ALL_LANES_16(ret.U16[i] = U16[i] << v);
  }

  GSVector2i sll16(s32 v) const { ALL_LANES_16(ret.U16[i] = U16[i] << v); }

  GSVector2i sllv16(const GSVector2i& v) const { ALL_LANES_16(ret.U16[i] = U16[i] << v.U16[i]); }

  template<s32 v>
  GSVector2i srl16() const
  {
    ALL_LANES_16(ret.U16[i] = U16[i] >> v);
  }

  GSVector2i srl16(s32 v) const { ALL_LANES_16(ret.U16[i] = U16[i] >> v); }

  GSVector2i srlv16(const GSVector2i& v) const { ALL_LANES_16(ret.U16[i] = U16[i] >> v.U16[i]); }

  template<s32 v>
  GSVector2i sra16() const
  {
    ALL_LANES_16(ret.I16[i] = I16[i] >> v);
  }

  GSVector2i sra16(s32 v) const { ALL_LANES_16(ret.I16[i] = I16[i] >> v); }

  GSVector2i srav16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] >> v.I16[i]); }

  template<s32 v>
  GSVector2i sll32() const
  {
    ALL_LANES_32(ret.U32[i] = U32[i] << v);
  }

  GSVector2i sll32(s32 v) const { ALL_LANES_32(ret.U32[i] = U32[i] << v); }

  GSVector2i sllv32(const GSVector2i& v) const { ALL_LANES_32(ret.U32[i] = U32[i] << v.U32[i]); }

  template<s32 v>
  GSVector2i srl32() const
  {
    ALL_LANES_32(ret.U32[i] = U32[i] >> v);
  }

  GSVector2i srl32(s32 v) const { ALL_LANES_32(ret.U32[i] = U32[i] >> v); }

  GSVector2i srlv32(const GSVector2i& v) const { ALL_LANES_32(ret.U32[i] = U32[i] >> v.U32[i]); }

  template<s32 v>
  GSVector2i sra32() const
  {
    ALL_LANES_32(ret.I32[i] = I32[i] >> v);
  }

  GSVector2i sra32(s32 v) const { ALL_LANES_32(ret.I32[i] = I32[i] >> v); }

  GSVector2i srav32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] >> v.I32[i]); }

  GSVector2i add8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = I8[i] + v.I8[i]); }

  GSVector2i add16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] + v.I16[i]); }

  GSVector2i add32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] + v.I32[i]); }

  GSVector2i adds8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = SSATURATE8(I8[i] + v.I8[i])); }

  GSVector2i adds16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = SSATURATE16(I16[i] + v.I16[i])); }

  GSVector2i addus8(const GSVector2i& v) const { ALL_LANES_8(ret.U8[i] = USATURATE8(U8[i] + v.U8[i])); }

  GSVector2i addus16(const GSVector2i& v) const { ALL_LANES_16(ret.U16[i] = USATURATE16(U16[i] + v.U16[i])); }

  GSVector2i sub8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = I8[i] - v.I8[i]); }

  GSVector2i sub16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] - v.I16[i]); }

  GSVector2i sub32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] - v.I32[i]); }

  GSVector2i subs8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = SSATURATE8(I8[i] - v.I8[i])); }

  GSVector2i subs16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = SSATURATE16(I16[i] - v.I16[i])); }

  GSVector2i subus8(const GSVector2i& v) const { ALL_LANES_8(ret.U8[i] = USATURATE8(U8[i] - v.U8[i])); }

  GSVector2i subus16(const GSVector2i& v) const { ALL_LANES_16(ret.U16[i] = USATURATE16(U16[i] - v.U16[i])); }

  GSVector2i avg8(const GSVector2i& v) const { ALL_LANES_8(ret.U8[i] = (U8[i] + v.U8[i]) >> 1); }

  GSVector2i avg16(const GSVector2i& v) const { ALL_LANES_16(ret.U16[i] = (U16[i] + v.U16[i]) >> 1); }

  GSVector2i mul16l(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] * v.I16[i]); }

  GSVector2i mul32l(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] * v.I32[i]); }

  ALWAYS_INLINE bool eq(const GSVector2i& v) const { return (std::memcmp(I32, v.I32, sizeof(I32))) == 0; }

  GSVector2i eq8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] == v.I8[i]) ? -1 : 0); }
  GSVector2i eq16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] == v.I16[i]) ? -1 : 0); }
  GSVector2i eq32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] == v.I32[i]) ? -1 : 0); }

  GSVector2i neq8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] != v.I8[i]) ? -1 : 0); }
  GSVector2i neq16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] != v.I16[i]) ? -1 : 0); }
  GSVector2i neq32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] != v.I32[i]) ? -1 : 0); }

  GSVector2i gt8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] > v.I8[i]) ? -1 : 0); }
  GSVector2i gt16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] > v.I16[i]) ? -1 : 0); }
  GSVector2i gt32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] > v.I32[i]) ? -1 : 0); }

  GSVector2i ge8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] >= v.I8[i]) ? -1 : 0); }
  GSVector2i ge16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] >= v.I16[i]) ? -1 : 0); }
  GSVector2i ge32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] >= v.I32[i]) ? -1 : 0); }

  GSVector2i lt8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] < v.I8[i]) ? -1 : 0); }
  GSVector2i lt16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] < v.I16[i]) ? -1 : 0); }
  GSVector2i lt32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] < v.I32[i]) ? -1 : 0); }

  GSVector2i le8(const GSVector2i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] <= v.I8[i]) ? -1 : 0); }
  GSVector2i le16(const GSVector2i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] <= v.I16[i]) ? -1 : 0); }
  GSVector2i le32(const GSVector2i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] <= v.I32[i]) ? -1 : 0); }

  ALWAYS_INLINE GSVector2i andnot(const GSVector2i& v) const
  {
    GSVector2i ret;
    ret.U64[0] = (~v.U64[0]) & U64[0];
    return ret;
  }

  s32 mask() const
  {
    return static_cast<s32>((static_cast<u32>(U8[0] >> 7) << 0) | (static_cast<u32>(U8[1] >> 7) << 1) |
                            (static_cast<u32>(U8[2] >> 7) << 2) | (static_cast<u32>(U8[3] >> 7) << 3) |
                            (static_cast<u32>(U8[4] >> 7) << 4) | (static_cast<u32>(U8[5] >> 7) << 5) |
                            (static_cast<u32>(U8[6] >> 7) << 6) | (static_cast<u32>(U8[7] >> 7) << 7));
  }

  ALWAYS_INLINE bool alltrue() const { return (U64[0] == 0xFFFFFFFFFFFFFFFFULL); }

  ALWAYS_INLINE bool allfalse() const { return (U64[0] == 0); }

  template<s32 i>
  ALWAYS_INLINE GSVector2i insert8(s32 a) const
  {
    GSVector2i ret = *this;
    ret.I8[i] = static_cast<s8>(a);
    return ret;
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract8() const
  {
    return I8[i];
  }

  template<s32 i>
  ALWAYS_INLINE GSVector2i insert16(s32 a) const
  {
    GSVector2i ret = *this;
    ret.I16[i] = static_cast<s16>(a);
    return ret;
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract16() const
  {
    return I16[i];
  }

  template<s32 i>
  ALWAYS_INLINE GSVector2i insert32(s32 a) const
  {
    GSVector2i ret = *this;
    ret.I32[i] = a;
    return ret;
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract32() const
  {
    return I32[i];
  }

  ALWAYS_INLINE static GSVector2i load32(const void* p)
  {
    GSVector2i ret;
    std::memcpy(&ret.x, p, sizeof(s32));
    ret.y = 0;
    return ret;
  }

  ALWAYS_INLINE static GSVector2i load(const void* p)
  {
    GSVector2i ret;
    std::memcpy(ret.I32, p, sizeof(ret.I32));
    return ret;
  }

  ALWAYS_INLINE static GSVector2i load(s32 i)
  {
    GSVector2i ret;
    ret.x = i;
    return ret;
  }

  ALWAYS_INLINE static void store(void* p, const GSVector2i& v) { std::memcpy(p, v.I32, sizeof(I32)); }

  ALWAYS_INLINE static void store32(void* p, const GSVector2i& v) { std::memcpy(p, &v.x, sizeof(s32)); }

  ALWAYS_INLINE static s32 store(const GSVector2i& v) { return v.x; }

  ALWAYS_INLINE void operator&=(const GSVector2i& v) { U64[0] &= v.U64[0]; }
  ALWAYS_INLINE void operator|=(const GSVector2i& v) { U64[0] |= v.U64[0]; }
  ALWAYS_INLINE void operator^=(const GSVector2i& v) { U64[0] ^= v.U64[0]; }

  ALWAYS_INLINE friend GSVector2i operator&(const GSVector2i& v1, const GSVector2i& v2)
  {
    GSVector2i ret;
    ret.U64[0] = v1.U64[0] & v2.U64[0];
    return ret;
  }

  ALWAYS_INLINE friend GSVector2i operator|(const GSVector2i& v1, const GSVector2i& v2)
  {
    GSVector2i ret;
    ret.U64[0] = v1.U64[0] | v2.U64[0];
    return ret;
  }

  ALWAYS_INLINE friend GSVector2i operator^(const GSVector2i& v1, const GSVector2i& v2)
  {
    GSVector2i ret;
    ret.U64[0] = v1.U64[0] ^ v2.U64[0];
    return ret;
  }

  ALWAYS_INLINE friend GSVector2i operator&(const GSVector2i& v, s32 i) { return v & GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator|(const GSVector2i& v, s32 i) { return v | GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator^(const GSVector2i& v, s32 i) { return v ^ GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator~(const GSVector2i& v) { return v ^ v.eq32(v); }

  ALWAYS_INLINE static constexpr GSVector2i zero() { return GSVector2i::cxpr(0, 0); }

  ALWAYS_INLINE GSVector2i xy() const { return *this; }
  ALWAYS_INLINE GSVector2i xx() const { return GSVector2i(x, x); }
  ALWAYS_INLINE GSVector2i yx() const { return GSVector2i(y, x); }
  ALWAYS_INLINE GSVector2i yy() const { return GSVector2i(y, y); }
};

class alignas(16) GSVector2
{
  struct cxpr_init_tag
  {
  };
  static constexpr cxpr_init_tag cxpr_init{};

  constexpr GSVector2(cxpr_init_tag, float x, float y) : F32{x, y} {}

  constexpr GSVector2(cxpr_init_tag, int x, int y) : I32{x, y} {}

public:
  union
  {
    struct
    {
      float x, y;
    };
    struct
    {
      float r, g;
    };
    float F32[4];
    double F64[2];
    s8 I8[16];
    s16 I16[8];
    s32 I32[4];
    s64 I64[2];
    u8 U8[16];
    u16 U16[8];
    u32 U32[4];
    u64 U64[2];
  };

  GSVector2() = default;

  constexpr static GSVector2 cxpr(float x, float y) { return GSVector2(cxpr_init, x, y); }

  constexpr static GSVector2 cxpr(float x) { return GSVector2(cxpr_init, x, x); }

  constexpr static GSVector2 cxpr(int x, int y) { return GSVector2(cxpr_init, x, y); }

  constexpr static GSVector2 cxpr(int x) { return GSVector2(cxpr_init, x, x); }

  ALWAYS_INLINE GSVector2(float x, float y)
  {
    this->x = x;
    this->y = y;
  }

  ALWAYS_INLINE GSVector2(int x, int y)
  {
    this->x = static_cast<float>(x);
    this->y = static_cast<float>(y);
  }

  ALWAYS_INLINE explicit GSVector2(float f) { x = y = f; }

  ALWAYS_INLINE explicit GSVector2(int i) { x = y = static_cast<float>(i); }

  ALWAYS_INLINE explicit GSVector2(const GSVector2i& v);

  ALWAYS_INLINE static GSVector2 cast(const GSVector2i& v);

  ALWAYS_INLINE void operator=(float f) { x = y = f; }

  GSVector2 abs() const { return GSVector2(std::fabs(x), std::fabs(y)); }

  GSVector2 neg() const { return GSVector2(-x, -y); }

  GSVector2 rcp() const { return GSVector2(1.0f / x, 1.0f / y); }

  GSVector2 rcpnr() const
  {
    GSVector2 v_ = rcp();

    return (v_ + v_) - (v_ * v_) * *this;
  }

  GSVector2 floor() const { return GSVector2(std::floor(x), std::floor(y)); }

  GSVector2 ceil() const { return GSVector2(std::ceil(x), std::ceil(y)); }

  GSVector2 sat(const GSVector2& min, const GSVector2& max) const
  {
    return GSVector2(std::clamp(x, min.x, max.x), std::clamp(y, min.y, max.y));
  }

  GSVector2 sat(const float scale = 255) const { return sat(zero(), GSVector2(scale)); }

  GSVector2 clamp(const float scale = 255) const { return min(GSVector2(scale)); }

  GSVector2 min(const GSVector2& v) const { return GSVector2(std::min(x, v.x), std::min(y, v.y)); }

  GSVector2 max(const GSVector2& v) const { return GSVector2(std::max(x, v.x), std::max(y, v.y)); }

  template<int mask>
  GSVector2 blend32(const GSVector2& v) const
  {
    return GSVector2(v.F32[mask & 1], v.F32[(mask >> 1) & 1]);
  }

  ALWAYS_INLINE GSVector2 blend32(const GSVector2& v, const GSVector2& mask) const
  {
    return GSVector2((mask.U32[0] & 0x80000000u) ? v.x : x, (mask.U32[1] & 0x80000000u) ? v.y : y);
  }

  ALWAYS_INLINE GSVector2 andnot(const GSVector2& v) const
  {
    GSVector2 ret;
    ret.U32[0] = ((~v.U32[0]) & U32[0]);
    ret.U32[1] = ((~v.U32[1]) & U32[1]);
    return ret;
  }

  ALWAYS_INLINE int mask() const { return (U32[0] >> 31) | ((U32[1] >> 30) & 2); }

  ALWAYS_INLINE bool alltrue() const { return (U64[0] == 0xFFFFFFFFFFFFFFFFULL); }

  ALWAYS_INLINE bool allfalse() const { return (U64[0] == 0); }

  ALWAYS_INLINE GSVector2 replace_nan(const GSVector2& v) const { return v.blend32(*this, *this == *this); }

  template<int src, int dst>
  ALWAYS_INLINE GSVector2 insert32(const GSVector2& v) const
  {
    GSVector2 ret = *this;
    ret.F32[dst] = v.F32[src];
    return ret;
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return I32[i];
  }

  ALWAYS_INLINE static constexpr GSVector2 zero() { return GSVector2::cxpr(0.0f, 0.0f); }

  ALWAYS_INLINE static constexpr GSVector2 xffffffff()
  {
    GSVector2 ret = zero();
    ret.U64[0] = ~ret.U64[0];
    return ret;
  }

  ALWAYS_INLINE static GSVector2 load(float f) { return GSVector2(f, f); }

  ALWAYS_INLINE static GSVector2 load(const void* p)
  {
    GSVector2 ret;
    std::memcpy(ret.F32, p, sizeof(F32));
    return ret;
  }

  ALWAYS_INLINE static void store(void* p, const GSVector2& v) { std::memcpy(p, &v.F32, sizeof(F32)); }

  ALWAYS_INLINE GSVector2 operator-() const { return neg(); }

  void operator+=(const GSVector2& v_)
  {
    x = x + v_.x;
    y = y + v_.y;
  }
  void operator-=(const GSVector2& v_)
  {
    x = x - v_.x;
    y = y - v_.y;
  }
  void operator*=(const GSVector2& v_)
  {
    x = x * v_.x;
    y = y * v_.y;
  }
  void operator/=(const GSVector2& v_)
  {
    x = x / v_.x;
    y = y / v_.y;
  }

  void operator+=(const float v_)
  {
    x = x + v_;
    y = y + v_;
  }
  void operator-=(const float v_)
  {
    x = x - v_;
    y = y - v_;
  }
  void operator*=(const float v_)
  {
    x = x * v_;
    y = y * v_;
  }
  void operator/=(const float v_)
  {
    x = x / v_;
    y = y / v_;
  }

  void operator&=(const GSVector2& v_) { U64[0] &= v_.U64[0]; }
  void operator|=(const GSVector2& v_) { U64[0] |= v_.U64[0]; }
  void operator^=(const GSVector2& v_) { U64[0] ^= v_.U64[0]; }

  friend GSVector2 operator+(const GSVector2& v1, const GSVector2& v2) { return GSVector2(v1.x + v2.x, v1.y + v2.y); }

  friend GSVector2 operator-(const GSVector2& v1, const GSVector2& v2) { return GSVector2(v1.x - v2.x, v1.y - v2.y); }

  friend GSVector2 operator*(const GSVector2& v1, const GSVector2& v2) { return GSVector2(v1.x * v2.x, v1.y * v2.y); }

  friend GSVector2 operator/(const GSVector2& v1, const GSVector2& v2) { return GSVector2(v1.x / v2.x, v1.y / v2.y); }

  friend GSVector2 operator+(const GSVector2& v, float f) { return GSVector2(v.x + f, v.y + f); }

  friend GSVector2 operator-(const GSVector2& v, float f) { return GSVector2(v.x - f, v.y - f); }

  friend GSVector2 operator*(const GSVector2& v, float f) { return GSVector2(v.x * f, v.y * f); }

  friend GSVector2 operator/(const GSVector2& v, float f) { return GSVector2(v.x / f, v.y / f); }

  friend GSVector2 operator&(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.U64[0] = v1.U64[0] & v2.U64[0];
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator|(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.U64[0] = v1.U64[0] | v2.U64[0];
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator^(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.U64[0] = v1.U64[0] ^ v2.U64[0];
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator==(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.I32[0] = (v1.x == v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y == v2.y) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator!=(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.I32[0] = (v1.x != v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y != v2.y) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator>(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.I32[0] = (v1.x > v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y > v2.y) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator<(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.I32[0] = (v1.x < v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y < v2.y) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator>=(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.I32[0] = (v1.x >= v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y >= v2.y) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector2 operator<=(const GSVector2& v1, const GSVector2& v2)
  {
    GSVector2 ret;
    ret.I32[0] = (v1.x <= v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y <= v2.y) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE GSVector2 xy() const { return *this; }
  ALWAYS_INLINE GSVector2 xx() const { return GSVector2(x, x); }
  ALWAYS_INLINE GSVector2 yx() const { return GSVector2(y, x); }
  ALWAYS_INLINE GSVector2 yy() const { return GSVector2(y, y); }
};

#undef ALL_LANES_8
#undef ALL_LANES_16
#undef ALL_LANES_32

#define ALL_LANES_8(expr)                                                                                              \
  GSVector4i ret;                                                                                                      \
  for (size_t i = 0; i < 16; i++)                                                                                      \
    expr;                                                                                                              \
  return ret;
#define ALL_LANES_16(expr)                                                                                             \
  GSVector4i ret;                                                                                                      \
  for (size_t i = 0; i < 8; i++)                                                                                       \
    expr;                                                                                                              \
  return ret;
#define ALL_LANES_32(expr)                                                                                             \
  GSVector4i ret;                                                                                                      \
  for (size_t i = 0; i < 4; i++)                                                                                       \
    expr;                                                                                                              \
  return ret;
#define ALL_LANES_64(expr)                                                                                             \
  GSVector4i ret;                                                                                                      \
  for (size_t i = 0; i < 2; i++)                                                                                       \
    expr;                                                                                                              \
  return ret;

class alignas(16) GSVector4i
{
  struct cxpr_init_tag
  {
  };
  static constexpr cxpr_init_tag cxpr_init{};

  constexpr GSVector4i(cxpr_init_tag, s32 x, s32 y, s32 z, s32 w) : I32{x, y, z, w} {}

  constexpr GSVector4i(cxpr_init_tag, s16 s0, s16 s1, s16 s2, s16 s3, s16 s4, s16 s5, s16 s6, s16 s7)
    : I16{s0, s1, s2, s3, s4, s5, s6, s7}
  {
  }

  constexpr GSVector4i(cxpr_init_tag, s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7, s8 b8, s8 b9, s8 b10,
                       s8 b11, s8 b12, s8 b13, s8 b14, s8 b15)
    : I8{b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15}
  {
  }

public:
  union
  {
    struct
    {
      s32 x, y, z, w;
    };
    struct
    {
      s32 r, g, b, a;
    };
    struct
    {
      s32 left, top, right, bottom;
    };
    float F32[4];
    s8 I8[16];
    s16 I16[8];
    s32 I32[4];
    s64 I64[2];
    u8 U8[16];
    u16 U16[8];
    u32 U32[4];
    u64 U64[2];
  };

  GSVector4i() = default;

  ALWAYS_INLINE constexpr static GSVector4i cxpr(s32 x, s32 y, s32 z, s32 w)
  {
    return GSVector4i(cxpr_init, x, y, z, w);
  }

  ALWAYS_INLINE constexpr static GSVector4i cxpr(s32 x) { return GSVector4i(cxpr_init, x, x, x, x); }

  ALWAYS_INLINE constexpr static GSVector4i cxpr16(s16 x) { return GSVector4i(cxpr_init, x, x, x, x, x, x, x, x); }

  ALWAYS_INLINE constexpr static GSVector4i cxpr16(s16 s0, s16 s1, s16 s2, s16 s3, s16 s4, s16 s5, s16 s6, s16 s7)
  {
    return GSVector4i(cxpr_init, s0, s1, s2, s3, s4, s5, s6, s7);
  }

  ALWAYS_INLINE constexpr static GSVector4i cxpr8(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7, s8 b8, s8 b9,
                                                  s8 b10, s8 b11, s8 b12, s8 b13, s8 b14, s8 b15)
  {
    return GSVector4i(cxpr_init, b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15);
  }

  ALWAYS_INLINE GSVector4i(s32 x, s32 y, s32 z, s32 w)
  {
    this->x = x;
    this->y = y;
    this->z = z;
    this->w = w;
  }

  ALWAYS_INLINE GSVector4i(s32 x, s32 y) { *this = load(x).upl32(load(y)); }

  ALWAYS_INLINE GSVector4i(s16 s0, s16 s1, s16 s2, s16 s3, s16 s4, s16 s5, s16 s6, s16 s7)
  {
    I16[0] = s0;
    I16[1] = s1;
    I16[2] = s2;
    I16[3] = s3;
    I16[4] = s4;
    I16[5] = s5;
    I16[6] = s6;
    I16[7] = s7;
  }

  ALWAYS_INLINE constexpr GSVector4i(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7, s8 b8, s8 b9, s8 b10,
                                     s8 b11, s8 b12, s8 b13, s8 b14, s8 b15)
    : I8{b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15}
  {
  }

  ALWAYS_INLINE GSVector4i(const GSVector4i& v) { std::memcpy(I32, v.I32, sizeof(I32)); }
  ALWAYS_INLINE explicit GSVector4i(const GSVector2i& v) : I32{v.I32[0], v.I32[1], 0, 0} {}

  // MSVC has bad codegen for the constexpr version when applied to non-constexpr things (https://godbolt.org/z/h8qbn7),
  // so leave the non-constexpr version default
  ALWAYS_INLINE explicit GSVector4i(s32 i) { *this = i; }

  ALWAYS_INLINE explicit GSVector4i(const GSVector4& v, bool truncate = true);

  ALWAYS_INLINE static GSVector4i cast(const GSVector4& v);

  ALWAYS_INLINE void operator=(const GSVector4i& v) { std::memcpy(I32, v.I32, sizeof(I32)); }
  ALWAYS_INLINE void operator=(s32 i)
  {
    x = i;
    y = i;
    z = i;
    w = i;
  }

  // rect

  ALWAYS_INLINE s32 width() const { return right - left; }

  ALWAYS_INLINE s32 height() const { return bottom - top; }

  ALWAYS_INLINE GSVector4i rsize() const
  {
    return sub32(xyxy()); // same as GSVector4i(0, 0, width(), height());
  }

  ALWAYS_INLINE s32 rarea() const { return width() * height(); }

  ALWAYS_INLINE bool rempty() const { return lt32(zwzw()).mask() != 0x00ff; }

  // TODO: Optimize for no-simd, this generates crap code.
  ALWAYS_INLINE GSVector4i runion(const GSVector4i& v) const { return min_i32(v).upl64(max_i32(v).srl<8>()); }

  ALWAYS_INLINE GSVector4i rintersect(const GSVector4i& v) const { return sat_i32(v); }
  ALWAYS_INLINE bool rintersects(const GSVector4i& v) const { return !rintersect(v).rempty(); }
  ALWAYS_INLINE bool rcontains(const GSVector4i& v) const { return rintersect(v).eq(v); }

  ALWAYS_INLINE u32 rgba32() const
  {
    GSVector4i v = *this;

    v = v.ps32(v);
    v = v.pu16(v);

    return (u32)store(v);
  }

  ALWAYS_INLINE GSVector4i sat_i8(const GSVector4i& min, const GSVector4i& max) const
  {
    return max_i8(min).min_i8(max);
  }
  ALWAYS_INLINE GSVector4i sat_i8(const GSVector4i& minmax) const
  {
    return max_i8(minmax.xyxy()).min_i8(minmax.zwzw());
  }
  ALWAYS_INLINE GSVector4i sat_i16(const GSVector4i& min, const GSVector4i& max) const
  {
    return max_i16(min).min_i16(max);
  }
  ALWAYS_INLINE GSVector4i sat_i16(const GSVector4i& minmax) const
  {
    return max_i16(minmax.xyxy()).min_i16(minmax.zwzw());
  }
  ALWAYS_INLINE GSVector4i sat_i32(const GSVector4i& min, const GSVector4i& max) const
  {
    return max_i32(min).min_i32(max);
  }
  ALWAYS_INLINE GSVector4i sat_i32(const GSVector4i& minmax) const
  {
    return max_i32(minmax.xyxy()).min_i32(minmax.zwzw());
  }

  ALWAYS_INLINE GSVector4i sat_u8(const GSVector4i& min, const GSVector4i& max) const
  {
    return max_u8(min).min_u8(max);
  }
  ALWAYS_INLINE GSVector4i sat_u8(const GSVector4i& minmax) const
  {
    return max_u8(minmax.xyxy()).min_u8(minmax.zwzw());
  }
  ALWAYS_INLINE GSVector4i sat_u16(const GSVector4i& min, const GSVector4i& max) const
  {
    return max_u16(min).min_u16(max);
  }
  ALWAYS_INLINE GSVector4i sat_u16(const GSVector4i& minmax) const
  {
    return max_u16(minmax.xyxy()).min_u16(minmax.zwzw());
  }
  ALWAYS_INLINE GSVector4i sat_u32(const GSVector4i& min, const GSVector4i& max) const
  {
    return max_u32(min).min_u32(max);
  }
  ALWAYS_INLINE GSVector4i sat_u32(const GSVector4i& minmax) const
  {
    return max_u32(minmax.xyxy()).min_u32(minmax.zwzw());
  }

  GSVector4i min_i8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = std::min(I8[i], v.I8[i])); }
  GSVector4i max_i8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = std::max(I8[i], v.I8[i])); }
  GSVector4i min_i16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = std::min(I16[i], v.I16[i])); }
  GSVector4i max_i16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = std::max(I16[i], v.I16[i])); }
  GSVector4i min_i32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = std::min(I32[i], v.I32[i])); }
  GSVector4i max_i32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = std::max(I32[i], v.I32[i])); }

  GSVector4i min_u8(const GSVector4i& v) const { ALL_LANES_8(ret.U8[i] = std::min(U8[i], v.U8[i])); }
  GSVector4i max_u8(const GSVector4i& v) const { ALL_LANES_8(ret.U8[i] = std::max(U8[i], v.U8[i])); }
  GSVector4i min_u16(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = std::min(U16[i], v.U16[i])); }
  GSVector4i max_u16(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = std::max(U16[i], v.U16[i])); }
  GSVector4i min_u32(const GSVector4i& v) const { ALL_LANES_32(ret.U32[i] = std::min(U32[i], v.U32[i])); }
  GSVector4i max_u32(const GSVector4i& v) const { ALL_LANES_32(ret.U32[i] = std::max(U32[i], v.U32[i])); }

  GSVector4i madd_s16(const GSVector4i& v) const
  {
    ALL_LANES_32(ret.I32[i] = (I16[i * 2] * v.I16[i * 2]) + (I16[i * 2 + 1] * v.I16[i * 2 + 1]));
  }

  GSVector4i addp_s32() const { return GSVector4i(x + y, z + w, 0, 0); }

  u8 minv_u8() const
  {
    return std::min(
      U8[0],
      std::min(
        U8[1],
        std::min(
          U8[2],
          std::min(
            U8[3],
            std::min(
              U8[4],
              std::min(
                U8[5],
                std::min(
                  U8[6],
                  std::min(
                    U8[7],
                    std::min(
                      U8[9],
                      std::min(U8[10],
                               std::min(U8[11], std::min(U8[12], std::min(U8[13], std::min(U8[14], U8[15]))))))))))))));
  }

  u16 maxv_u8() const
  {
    return std::max(
      U8[0],
      std::max(
        U8[1],
        std::max(
          U8[2],
          std::max(
            U8[3],
            std::max(
              U8[4],
              std::max(
                U8[5],
                std::max(
                  U8[6],
                  std::max(
                    U8[7],
                    std::max(
                      U8[9],
                      std::max(U8[10],
                               std::max(U8[11], std::max(U8[12], std::max(U8[13], std::max(U8[14], U8[15]))))))))))))));
  }

  u16 minv_u16() const
  {
    return std::min(
      U16[0],
      std::min(U16[1],
               std::min(U16[2], std::min(U16[3], std::min(U16[4], std::min(U16[5], std::min(U16[6], U16[7])))))));
  }

  u16 maxv_u16() const
  {
    return std::max(
      U16[0],
      std::max(U16[1],
               std::max(U16[2], std::max(U16[3], std::max(U16[4], std::max(U16[5], std::max(U16[6], U16[7])))))));
  }

  s32 minv_s32() const { return std::min(x, std::min(y, std::min(z, w))); }

  u32 minv_u32() const { return std::min(U32[0], std::min(U32[1], std::min(U32[2], U32[3]))); }

  s32 maxv_s32() const { return std::max(x, std::max(y, std::max(z, w))); }

  u32 maxv_u32() const { return std::max(U32[0], std::max(U32[1], std::max(U32[2], U32[3]))); }

  static s32 min_i16(s32 a, s32 b) { return store(load(a).min_i16(load(b))); }

  ALWAYS_INLINE GSVector4i clamp8() const { return pu16().upl8(); }

  GSVector4i blend8(const GSVector4i& v, const GSVector4i& mask) const
  {
    GSVector4i ret;
    for (size_t i = 0; i < 16; i++)
      ret.U8[i] = (mask.U8[i] & 0x80) ? v.U8[i] : U8[i];
    return ret;
  }

  template<s32 mask>
  GSVector4i blend16(const GSVector4i& v) const
  {
    GSVector4i ret;
    for (size_t i = 0; i < 8; i++)
      ret.U16[i] = ((mask & (1 << i)) != 0) ? v.U16[i] : U16[i];
    return ret;
  }

  template<s32 mask>
  GSVector4i blend32(const GSVector4i& v) const
  {
    GSVector4i ret;
    for (size_t i = 0; i < 4; i++)
      ret.U32[i] = ((mask & (1 << i)) != 0) ? v.U32[i] : U32[i];
    return ret;
  }

  GSVector4i blend(const GSVector4i& v, const GSVector4i& mask) const
  {
    GSVector4i ret;
    for (size_t i = 0; i < 2; i++)
      ret.U64[i] = (v.U64[i] & mask.U64[i]) | (U64[i] & ~mask.U64[i]);
    return ret;
  }

  ALWAYS_INLINE GSVector4i mix16(const GSVector4i& v) const { return blend16<0xaa>(v); }

  GSVector4i shuffle8(const GSVector4i& mask) const
  {
    ALL_LANES_8(ret.I8[i] = (mask.I8[i] & 0x80) ? 0 : (I8[mask.I8[i] & 0xf]));
  }

  GSVector4i ps16(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = SSATURATE8((i < 8) ? I16[i] : v.I16[i - 8])); }
  GSVector4i ps16() const { ALL_LANES_8(ret.I8[i] = SSATURATE8(I16[(i < 8) ? i : (i - 8)])); }
  GSVector4i pu16(const GSVector4i& v) const { ALL_LANES_8(ret.U8[i] = USATURATE8((i < 8) ? U16[i] : v.U16[i - 8])); }
  GSVector4i pu16() const { ALL_LANES_8(ret.U8[i] = USATURATE8(U16[(i < 8) ? i : (i - 8)])); }
  GSVector4i ps32(const GSVector4i& v) const
  {
    ALL_LANES_16(ret.U16[i] = SSATURATE16((i < 4) ? I32[i] : v.I32[i - 4]));
  }
  GSVector4i ps32() const { ALL_LANES_16(ret.I16[i] = SSATURATE16(I32[(i < 4) ? i : (i - 4)])); }
  GSVector4i pu32(const GSVector4i& v) const
  {
    ALL_LANES_16(ret.U16[i] = USATURATE16((i < 4) ? U32[i] : v.U32[i - 4]));
  }
  GSVector4i pu32() const { ALL_LANES_16(ret.U16[i] = USATURATE16(U32[(i < 4) ? i : (i - 4)])); }

  GSVector4i upl8(const GSVector4i& v) const
  {
    return GSVector4i(I8[0], v.I8[0], I8[1], v.I8[1], I8[2], v.I8[2], I8[3], v.I8[3], I8[4], v.I8[4], I8[5], v.I8[5],
                      I8[6], v.I8[6], I8[7], v.I8[7]);
  }
  GSVector4i uph8(const GSVector4i& v) const
  {
    return GSVector4i(I8[8], v.I8[8], I8[9], v.I8[9], I8[10], v.I8[10], I8[11], v.I8[11], I8[12], v.I8[12], I8[13],
                      v.I8[13], I8[14], v.I8[14], I8[15], v.I8[15]);
  }
  GSVector4i upl16(const GSVector4i& v) const
  {
    return GSVector4i(I16[0], v.I16[0], I16[1], v.I16[1], I16[2], v.I16[2], I16[3], v.I16[3]);
  }
  GSVector4i uph16(const GSVector4i& v) const
  {
    return GSVector4i(I16[4], v.I16[4], I16[5], v.I16[5], I16[6], v.I16[6], I16[7], v.I16[7]);
  }
  GSVector4i upl32(const GSVector4i& v) const { return GSVector4i(I32[0], v.I32[0], I32[1], v.I32[1]); }
  GSVector4i uph32(const GSVector4i& v) const { return GSVector4i(I32[2], v.I32[2], I32[3], v.I32[3]); }
  GSVector4i upl64(const GSVector4i& v) const
  {
    GSVector4i ret;
    ret.I64[0] = I64[0];
    ret.I64[1] = v.I64[0];
    return ret;
  }
  GSVector4i uph64(const GSVector4i& v) const
  {
    GSVector4i ret;
    ret.I64[0] = I64[1];
    ret.I64[1] = v.I64[1];
    return ret;
  }

  GSVector4i upl8() const
  {
    return GSVector4i(I8[0], 0, I8[1], 0, I8[2], 0, I8[3], 0, I8[4], 0, I8[5], 0, I8[6], 0, I8[7], 0);
  }
  GSVector4i uph8() const
  {
    return GSVector4i(I8[8], 0, I8[9], 0, I8[10], 0, I8[11], 0, I8[12], 0, I8[13], 0, I8[14], 0, I8[15], 0);
  }

  GSVector4i upl16() const { return GSVector4i(I16[0], 0, I16[1], 0, I16[2], 0, I16[3], 0); }
  GSVector4i uph16() const { return GSVector4i(I16[4], 0, I16[5], 0, I16[6], 0, I16[7], 0); }

  GSVector4i upl32() const { return GSVector4i(I32[0], 0, I32[1], 0); }
  GSVector4i uph32() const { return GSVector4i(I32[2], 0, I32[3], 0); }
  GSVector4i upl64() const
  {
    GSVector4i ret;
    ret.I64[0] = I64[0];
    ret.I64[1] = 0;
    return ret;
  }
  GSVector4i uph64() const
  {
    GSVector4i ret;
    ret.I64[0] = I64[1];
    ret.I64[1] = 0;
    return ret;
  }

  GSVector4i i8to16() const { ALL_LANES_16(ret.I16[i] = I8[i]); }
  GSVector4i i8to32() const { ALL_LANES_32(ret.I32[i] = I8[i]); }
  GSVector4i i8to64() const { ALL_LANES_64(ret.I64[i] = I8[i]); }

  GSVector4i i16to32() const { ALL_LANES_32(ret.I32[i] = I16[i]); }
  GSVector4i i16to64() const { ALL_LANES_64(ret.I64[i] = I16[i]); }
  GSVector4i i32to64() const { ALL_LANES_64(ret.I64[i] = I32[i]); }
  GSVector4i u8to16() const { ALL_LANES_64(ret.U16[i] = U8[i]); }
  GSVector4i u8to32() const { ALL_LANES_32(ret.U32[i] = U8[i]); }
  GSVector4i u8to64() const { ALL_LANES_64(ret.U64[i] = U8[i]); }
  GSVector4i u16to32() const { ALL_LANES_32(ret.U32[i] = U16[i]); }
  GSVector4i u16to64() const { ALL_LANES_64(ret.U64[i] = U16[i]); }
  GSVector4i u32to64() const { ALL_LANES_64(ret.U64[i] = U32[i]); }

  template<s32 v>
  GSVector4i srl() const
  {
    GSVector4i ret = {};
    if constexpr (v < 16)
    {
      for (s32 i = 0; i < (16 - v); i++)
        ret.U8[i] = U8[v + i];
    }
    return ret;
  }

  template<s32 v>
  GSVector4i srl(const GSVector4i& r)
  {
    // This sucks. Hopefully it's never used.
    u8 concat[32];
    std::memcpy(concat, U8, sizeof(u8) * 16);
    std::memcpy(concat + 16, r.U8, sizeof(u8) * 16);

    GSVector4i ret;
    std::memcpy(ret.U8, &concat[v], sizeof(u8) * 16);
    return ret;
  }

  template<s32 v>
  GSVector4i sll() const
  {
    GSVector4i ret = {};
    if constexpr (v < 16)
    {
      for (s32 i = 0; i < (16 - v); i++)
        ret.U8[v + i] = U8[i];
    }
    return ret;
  }

  template<s32 v>
  GSVector4i sll16() const
  {
    ALL_LANES_16(ret.U16[i] = U16[i] << v);
  }

  GSVector4i sll16(s32 v) const { ALL_LANES_16(ret.U16[i] = U16[i] << v); }

  GSVector4i sllv16(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = U16[i] << v.U16[i]); }

  template<s32 v>
  GSVector4i srl16() const
  {
    ALL_LANES_16(ret.U16[i] = U16[i] >> v);
  }

  GSVector4i srl16(s32 v) const { ALL_LANES_16(ret.U16[i] = U16[i] >> v); }

  GSVector4i srlv16(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = U16[i] >> v.U16[i]); }

  template<s32 v>
  GSVector4i sra16() const
  {
    ALL_LANES_16(ret.I16[i] = I16[i] >> v);
  }

  GSVector4i sra16(s32 v) const { ALL_LANES_16(ret.I16[i] = I16[i] >> v); }

  GSVector4i srav16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] >> v.I16[i]); }

  template<s32 v>
  GSVector4i sll32() const
  {
    ALL_LANES_32(ret.U32[i] = U32[i] << v);
  }

  GSVector4i sll32(s32 v) const { ALL_LANES_32(ret.U32[i] = U32[i] << v); }

  GSVector4i sllv32(const GSVector4i& v) const { ALL_LANES_32(ret.U32[i] = U32[i] << v.U32[i]); }

  template<s32 v>
  GSVector4i srl32() const
  {
    ALL_LANES_32(ret.U32[i] = U32[i] >> v);
  }

  GSVector4i srl32(s32 v) const { ALL_LANES_32(ret.U32[i] = U32[i] >> v); }

  GSVector4i srlv32(const GSVector4i& v) const { ALL_LANES_32(ret.U32[i] = U32[i] >> v.U32[i]); }

  template<s32 v>
  GSVector4i sra32() const
  {
    ALL_LANES_32(ret.I32[i] = I32[i] >> v);
  }

  GSVector4i sra32(s32 v) const { ALL_LANES_32(ret.I32[i] = I32[i] >> v); }

  GSVector4i srav32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] >> v.I32[i]); }

  template<s64 v>
  GSVector4i sll64() const
  {
    ALL_LANES_64(ret.U64[i] = U64[i] << v);
  }

  GSVector4i sll64(s32 v) const { ALL_LANES_64(ret.U64[i] = U64[i] << v); }

  GSVector4i sllv64(const GSVector4i& v) const { ALL_LANES_64(ret.U64[i] = U64[i] << v.U64[i]); }

  template<s64 v>
  GSVector4i srl64() const
  {
    ALL_LANES_64(ret.U64[i] = U64[i] >> v);
  }

  GSVector4i srl64(s32 v) const { ALL_LANES_64(ret.U64[i] = U64[i] >> v); }

  GSVector4i srlv64(const GSVector4i& v) const { ALL_LANES_64(ret.U64[i] = U64[i] >> v.U64[i]); }

  template<s64 v>
  GSVector4i sra64() const
  {
    ALL_LANES_64(ret.I64[i] = I64[i] >> v);
  }

  GSVector4i sra64(s32 v) const { ALL_LANES_64(ret.I64[i] = I64[i] >> v); }

  GSVector4i srav64(const GSVector4i& v) const { ALL_LANES_64(ret.I64[i] = I64[i] >> v.I64[i]); }

  GSVector4i add8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = I8[i] + v.I8[i]); }

  GSVector4i add16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] + v.I16[i]); }

  GSVector4i add32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] + v.I32[i]); }

  GSVector4i adds8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = SSATURATE8(I8[i] + v.I8[i])); }

  GSVector4i adds16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = SSATURATE16(I16[i] + v.I16[i])); }

  GSVector4i hadds16(const GSVector4i& v) const
  {
    return GSVector4i(SSATURATE16(I16[0] + I16[1]), SSATURATE16(I16[2] + I16[3]), SSATURATE16(I16[4] + I16[5]),
                      SSATURATE16(I16[6] + I16[7]), SSATURATE16(v.I16[0] + v.I16[1]), SSATURATE16(v.I16[2] + v.I16[3]),
                      SSATURATE16(v.I16[4] + v.I16[5]), SSATURATE16(v.I16[6] + v.I16[7]));
  }

  GSVector4i addus8(const GSVector4i& v) const { ALL_LANES_8(ret.U8[i] = USATURATE8(U8[i] + v.U8[i])); }

  GSVector4i addus16(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = USATURATE16(U16[i] + v.U16[i])); }

  GSVector4i sub8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = I8[i] - v.I8[i]); }

  GSVector4i sub16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] - v.I16[i]); }

  GSVector4i sub32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] - v.I32[i]); }

  GSVector4i subs8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = SSATURATE8(I8[i] - v.I8[i])); }

  GSVector4i subs16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = SSATURATE16(I16[i] - v.I16[i])); }

  GSVector4i subus8(const GSVector4i& v) const { ALL_LANES_8(ret.U8[i] = USATURATE8(U8[i] - v.U8[i])); }

  GSVector4i subus16(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = USATURATE16(U16[i] - v.U16[i])); }

  GSVector4i avg8(const GSVector4i& v) const { ALL_LANES_8(ret.U8[i] = (U8[i] + v.U8[i]) >> 1); }

  GSVector4i avg16(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = (U16[i] + v.U16[i]) >> 1); }

  GSVector4i mul16hs(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] * v.I16[i]) >> 16); }

  GSVector4i mul16hu(const GSVector4i& v) const { ALL_LANES_16(ret.U16[i] = (U16[i] * v.U16[i]) >> 16); }

  GSVector4i mul16l(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = I16[i] * v.I16[i]); }

  GSVector4i mul16hrs(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = ((I16[i] * v.I16[i]) >> 14) + 1); }

  GSVector4i mul32l(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = I32[i] * v.I32[i]); }

  template<s32 shift>
  ALWAYS_INLINE GSVector4i lerp16(const GSVector4i& a, const GSVector4i& f) const
  {
    // (a - this) * f << shift + this

    return add16(a.sub16(*this).modulate16<shift>(f));
  }

  template<s32 shift>
  ALWAYS_INLINE static GSVector4i lerp16(const GSVector4i& a, const GSVector4i& b, const GSVector4i& c)
  {
    // (a - b) * c << shift

    return a.sub16(b).modulate16<shift>(c);
  }

  template<s32 shift>
  ALWAYS_INLINE static GSVector4i lerp16(const GSVector4i& a, const GSVector4i& b, const GSVector4i& c,
                                         const GSVector4i& d)
  {
    // (a - b) * c << shift + d

    return d.add16(a.sub16(b).modulate16<shift>(c));
  }

  ALWAYS_INLINE GSVector4i lerp16_4(const GSVector4i& a_, const GSVector4i& f) const
  {
    // (a - this) * f >> 4 + this (a, this: 8-bit, f: 4-bit)

    return add16(a_.sub16(*this).mul16l(f).sra16<4>());
  }

  template<s32 shift>
  ALWAYS_INLINE GSVector4i modulate16(const GSVector4i& f) const
  {
    // a * f << shift
    if constexpr (shift == 0)
    {
      return mul16hrs(f);
    }

    return sll16<shift + 1>().mul16hs(f);
  }

  ALWAYS_INLINE bool eq(const GSVector4i& v) const { return (std::memcmp(I32, v.I32, sizeof(I32))) == 0; }

  GSVector4i eq8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] == v.I8[i]) ? -1 : 0); }
  GSVector4i eq16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] == v.I16[i]) ? -1 : 0); }
  GSVector4i eq32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] == v.I32[i]) ? -1 : 0); }
  GSVector4i eq64(const GSVector4i& v) const { ALL_LANES_64(ret.I64[i] = (I64[i] == v.I64[i]) ? -1 : 0); }

  GSVector4i neq8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] != v.I8[i]) ? -1 : 0); }
  GSVector4i neq16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] != v.I16[i]) ? -1 : 0); }
  GSVector4i neq32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] != v.I32[i]) ? -1 : 0); }

  GSVector4i gt8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] > v.I8[i]) ? -1 : 0); }
  GSVector4i gt16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] > v.I16[i]) ? -1 : 0); }
  GSVector4i gt32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] > v.I32[i]) ? -1 : 0); }

  GSVector4i ge8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] >= v.I8[i]) ? -1 : 0); }
  GSVector4i ge16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] >= v.I16[i]) ? -1 : 0); }
  GSVector4i ge32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] >= v.I32[i]) ? -1 : 0); }

  GSVector4i lt8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] < v.I8[i]) ? -1 : 0); }
  GSVector4i lt16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] < v.I16[i]) ? -1 : 0); }
  GSVector4i lt32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] < v.I32[i]) ? -1 : 0); }

  GSVector4i le8(const GSVector4i& v) const { ALL_LANES_8(ret.I8[i] = (I8[i] <= v.I8[i]) ? -1 : 0); }
  GSVector4i le16(const GSVector4i& v) const { ALL_LANES_16(ret.I16[i] = (I16[i] <= v.I16[i]) ? -1 : 0); }
  GSVector4i le32(const GSVector4i& v) const { ALL_LANES_32(ret.I32[i] = (I32[i] <= v.I32[i]) ? -1 : 0); }

  ALWAYS_INLINE GSVector4i andnot(const GSVector4i& v) const { ALL_LANES_64(ret.U64[i] = (~v.U64[i]) & U64[i]); }

  s32 mask() const
  {
    return static_cast<s32>((static_cast<u32>(U8[0] >> 7) << 0) | (static_cast<u32>(U8[1] >> 7) << 1) |
                            (static_cast<u32>(U8[2] >> 7) << 2) | (static_cast<u32>(U8[3] >> 7) << 3) |
                            (static_cast<u32>(U8[4] >> 7) << 4) | (static_cast<u32>(U8[5] >> 7) << 5) |
                            (static_cast<u32>(U8[6] >> 7) << 6) | (static_cast<u32>(U8[7] >> 7) << 7) |
                            (static_cast<u32>(U8[8] >> 7) << 8) | (static_cast<u32>(U8[9] >> 7) << 9) |
                            (static_cast<u32>(U8[10] >> 7) << 10) | (static_cast<u32>(U8[11] >> 7) << 11) |
                            (static_cast<u32>(U8[12] >> 7) << 12) | (static_cast<u32>(U8[13] >> 7) << 13) |
                            (static_cast<u32>(U8[14] >> 7) << 14) | (static_cast<u32>(U8[15] >> 7) << 15));
  }

  ALWAYS_INLINE bool alltrue() const { return ((U64[0] & U64[1]) == 0xFFFFFFFFFFFFFFFFULL); }

  ALWAYS_INLINE bool allfalse() const { return ((U64[0] | U64[1]) == 0); }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert8(s32 a) const
  {
    GSVector4i ret = *this;
    ret.I8[i] = static_cast<s8>(a);
    return ret;
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract8() const
  {
    return I8[i];
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert16(s32 a) const
  {
    GSVector4i ret = *this;
    ret.I16[i] = static_cast<s16>(a);
    return ret;
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract16() const
  {
    return I16[i];
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert32(s32 a) const
  {
    GSVector4i ret = *this;
    ret.I32[i] = a;
    return ret;
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract32() const
  {
    return I32[i];
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert64(s64 a) const
  {
    GSVector4i ret = *this;
    ret.I64[i] = a;
    return ret;
  }

  template<s32 i>
  ALWAYS_INLINE s64 extract64() const
  {
    return I64[i];
  }

  ALWAYS_INLINE static GSVector4i loadnt(const void* p)
  {
    GSVector4i ret;
    std::memcpy(&ret, p, sizeof(ret.I32));
    return ret;
  }

  ALWAYS_INLINE static GSVector4i load32(const void* p)
  {
    GSVector4i ret;
    std::memcpy(&ret.x, p, sizeof(s32));
    ret.y = 0;
    ret.z = 0;
    ret.w = 0;
    return ret;
  }

  ALWAYS_INLINE static GSVector4i loadl(const void* p)
  {
    GSVector4i ret;
    std::memcpy(&ret.U64[0], p, sizeof(ret.U64[0]));
    ret.U64[1] = 0;
    return ret;
  }

  ALWAYS_INLINE static GSVector4i loadh(const void* p)
  {
    GSVector4i ret;
    ret.U64[0] = 0;
    std::memcpy(&ret.U64[1], p, sizeof(ret.U64[1]));
    return ret;
  }

  ALWAYS_INLINE static GSVector4i loadh(const GSVector2i& v) { return loadh(&v); }

  template<bool aligned>
  ALWAYS_INLINE static GSVector4i load(const void* p)
  {
    GSVector4i ret;
    std::memcpy(ret.I32, p, sizeof(ret.I32));
    return ret;
  }

  ALWAYS_INLINE static GSVector4i load(s32 i)
  {
    GSVector4i ret;
    ret.x = i;
    ret.y = 0;
    ret.z = 0;
    ret.w = 0;
    return ret;
  }

  ALWAYS_INLINE static GSVector4i loadq(s64 i)
  {
    GSVector4i ret;
    ret.I64[0] = i;
    ret.I64[1] = 0;
    return ret;
  }

  ALWAYS_INLINE static void storent(void* p, const GSVector4i& v) { std::memcpy(p, v.I32, sizeof(v.I32)); }

  ALWAYS_INLINE static void storel(void* p, const GSVector4i& v) { std::memcpy(p, &v.I32[0], sizeof(s32) * 2); }

  ALWAYS_INLINE static void storeh(void* p, const GSVector4i& v) { std::memcpy(p, &v.I32[2], sizeof(s32) * 2); }

  ALWAYS_INLINE static void store(void* pl, void* ph, const GSVector4i& v)
  {
    GSVector4i::storel(pl, v);
    GSVector4i::storeh(ph, v);
  }

  template<bool aligned>
  ALWAYS_INLINE static void store(void* p, const GSVector4i& v)
  {
    std::memcpy(p, v.I32, sizeof(I32));
  }

  ALWAYS_INLINE static void store32(void* p, const GSVector4i& v) { std::memcpy(p, &v.x, sizeof(s32)); }

  ALWAYS_INLINE static s32 store(const GSVector4i& v) { return v.x; }

  ALWAYS_INLINE static s64 storeq(const GSVector4i& v) { return v.I64[0]; }

  ALWAYS_INLINE void operator&=(const GSVector4i& v)
  {
    U64[0] &= v.U64[0];
    U64[1] &= v.U64[1];
  }
  ALWAYS_INLINE void operator|=(const GSVector4i& v)
  {
    U64[0] |= v.U64[0];
    U64[1] |= v.U64[1];
  }
  ALWAYS_INLINE void operator^=(const GSVector4i& v)
  {
    U64[0] ^= v.U64[0];
    U64[1] ^= v.U64[1];
  }

  ALWAYS_INLINE friend GSVector4i operator&(const GSVector4i& v1, const GSVector4i& v2)
  {
    GSVector4i ret;
    ret.U64[0] = v1.U64[0] & v2.U64[0];
    ret.U64[1] = v1.U64[1] & v2.U64[1];
    return ret;
  }

  ALWAYS_INLINE friend GSVector4i operator|(const GSVector4i& v1, const GSVector4i& v2)
  {
    GSVector4i ret;
    ret.U64[0] = v1.U64[0] | v2.U64[0];
    ret.U64[1] = v1.U64[1] | v2.U64[1];
    return ret;
  }

  ALWAYS_INLINE friend GSVector4i operator^(const GSVector4i& v1, const GSVector4i& v2)
  {
    GSVector4i ret;
    ret.U64[0] = v1.U64[0] ^ v2.U64[0];
    ret.U64[1] = v1.U64[1] ^ v2.U64[1];
    return ret;
  }

  ALWAYS_INLINE friend GSVector4i operator&(const GSVector4i& v, s32 i) { return v & GSVector4i(i); }

  ALWAYS_INLINE friend GSVector4i operator|(const GSVector4i& v, s32 i) { return v | GSVector4i(i); }

  ALWAYS_INLINE friend GSVector4i operator^(const GSVector4i& v, s32 i) { return v ^ GSVector4i(i); }

  ALWAYS_INLINE friend GSVector4i operator~(const GSVector4i& v) { return v ^ v.eq32(v); }

  ALWAYS_INLINE static constexpr GSVector4i zero() { return GSVector4i::cxpr(0, 0, 0, 0); }

  ALWAYS_INLINE GSVector4i xyxy(const GSVector4i& v) const { return upl64(v); }

  ALWAYS_INLINE GSVector2i xy() const { return GSVector2i(x, y); }
  ALWAYS_INLINE GSVector2i zw() const { return GSVector2i(z, w); }

  // clang-format off
  // l/h/lh not implemented until needed

#define VECTOR4i_SHUFFLE_4(xs, xn, ys, yn, zs, zn, ws, wn) \
    ALWAYS_INLINE GSVector4i xs##ys##zs##ws() const {return GSVector4i(I32[xn], I32[yn], I32[zn], I32[wn]);}

#define VECTOR4i_SHUFFLE_3(xs, xn, ys, yn, zs, zn) \
    VECTOR4i_SHUFFLE_4(xs, xn, ys, yn, zs, zn, x, 0) \
    VECTOR4i_SHUFFLE_4(xs, xn, ys, yn, zs, zn, y, 1) \
    VECTOR4i_SHUFFLE_4(xs, xn, ys, yn, zs, zn, z, 2) \
    VECTOR4i_SHUFFLE_4(xs, xn, ys, yn, zs, zn, w, 3) \

#define VECTOR4i_SHUFFLE_2(xs, xn, ys, yn) \
    VECTOR4i_SHUFFLE_3(xs, xn, ys, yn, x, 0) \
    VECTOR4i_SHUFFLE_3(xs, xn, ys, yn, y, 1) \
    VECTOR4i_SHUFFLE_3(xs, xn, ys, yn, z, 2) \
    VECTOR4i_SHUFFLE_3(xs, xn, ys, yn, w, 3) \

#define VECTOR4i_SHUFFLE_1(xs, xn) \
    VECTOR4i_SHUFFLE_2(xs, xn, x, 0) \
    VECTOR4i_SHUFFLE_2(xs, xn, y, 1) \
    VECTOR4i_SHUFFLE_2(xs, xn, z, 2) \
    VECTOR4i_SHUFFLE_2(xs, xn, w, 3) \

  VECTOR4i_SHUFFLE_1(x, 0)
    VECTOR4i_SHUFFLE_1(y, 1)
    VECTOR4i_SHUFFLE_1(z, 2)
    VECTOR4i_SHUFFLE_1(w, 3)

  // clang-format on
};

class alignas(16) GSVector4
{
  struct cxpr_init_tag
  {
  };
  static constexpr cxpr_init_tag cxpr_init{};

  constexpr GSVector4(cxpr_init_tag, float x, float y, float z, float w) : F32{x, y, z, w} {}

  constexpr GSVector4(cxpr_init_tag, int x, int y, int z, int w) : I32{x, y, z, w} {}

  constexpr GSVector4(cxpr_init_tag, u64 x, u64 y) : U64{x, y} {}

public:
  union
  {
    struct
    {
      float x, y, z, w;
    };
    struct
    {
      float r, g, b, a;
    };
    struct
    {
      float left, top, right, bottom;
    };
    float F32[4];
    double F64[2];
    s8 I8[16];
    s16 I16[8];
    s32 I32[4];
    s64 I64[2];
    u8 U8[16];
    u16 U16[8];
    u32 U32[4];
    u64 U64[2];
  };

  GSVector4() = default;

  constexpr static GSVector4 cxpr(float x, float y, float z, float w) { return GSVector4(cxpr_init, x, y, z, w); }

  constexpr static GSVector4 cxpr(float x) { return GSVector4(cxpr_init, x, x, x, x); }

  constexpr static GSVector4 cxpr(int x, int y, int z, int w) { return GSVector4(cxpr_init, x, y, z, w); }

  constexpr static GSVector4 cxpr(int x) { return GSVector4(cxpr_init, x, x, x, x); }

  constexpr static GSVector4 cxpr64(u64 x, u64 y) { return GSVector4(cxpr_init, x, y); }

  constexpr static GSVector4 cxpr64(u64 x) { return GSVector4(cxpr_init, x, x); }

  ALWAYS_INLINE GSVector4(float x, float y, float z, float w)
  {
    this->x = x;
    this->y = y;
    this->z = z;
    this->w = w;
  }

  ALWAYS_INLINE GSVector4(float x, float y)
  {
    this->x = x;
    this->y = y;
    this->z = 0.0f;
    this->w = 0.0f;
  }

  ALWAYS_INLINE GSVector4(int x, int y, int z, int w)
  {
    this->x = static_cast<float>(x);
    this->y = static_cast<float>(y);
    this->z = static_cast<float>(z);
    this->w = static_cast<float>(w);
  }

  ALWAYS_INLINE GSVector4(int x, int y)
  {
    this->x = static_cast<float>(x);
    this->y = static_cast<float>(y);
    this->z = 0.0f;
    this->w = 0.0f;
  }

  ALWAYS_INLINE explicit GSVector4(float f) { x = y = z = w = f; }

  ALWAYS_INLINE explicit GSVector4(int i) { x = y = z = w = static_cast<float>(i); }

  ALWAYS_INLINE explicit GSVector4(const GSVector2& v) : x(v.x), y(v.y), z(0.0f), w(0.0f) {}
  ALWAYS_INLINE explicit GSVector4(const GSVector4i& v);

  ALWAYS_INLINE static GSVector4 cast(const GSVector4i& v);

  ALWAYS_INLINE static GSVector4 f64(double x, double y)
  {
    GSVector4 ret;
    ret.F64[0] = x;
    ret.F64[1] = y;
    return ret;
  }

  ALWAYS_INLINE void operator=(float f) { x = y = z = w = f; }

  ALWAYS_INLINE GSVector4 noopt() { return *this; }

  u32 rgba32() const { return GSVector4i(*this).rgba32(); }

  ALWAYS_INLINE static GSVector4 rgba32(u32 rgba) { return GSVector4(GSVector4i::load((int)rgba).u8to32()); }

  ALWAYS_INLINE static GSVector4 unorm8(u32 rgba) { return rgba32(rgba) * GSVector4::cxpr(1.0f / 255.0f); }

  GSVector4 abs() const { return GSVector4(std::fabs(x), std::fabs(y), std::fabs(z), std::fabs(w)); }

  GSVector4 neg() const { return GSVector4(-x, -y, -z, -w); }

  GSVector4 rcp() const { return GSVector4(1.0f / x, 1.0f / y, 1.0f / z, 1.0f / w); }

  GSVector4 rcpnr() const
  {
    GSVector4 v_ = rcp();

    return (v_ + v_) - (v_ * v_) * *this;
  }

  GSVector4 floor() const { return GSVector4(std::floor(x), std::floor(y), std::floor(z), std::floor(w)); }

  GSVector4 ceil() const { return GSVector4(std::ceil(x), std::ceil(y), std::ceil(z), std::ceil(w)); }

  GSVector4 madd(const GSVector4& a_, const GSVector4& b_) const { return *this * a_ + b_; }

  GSVector4 msub(const GSVector4& a_, const GSVector4& b_) const { return *this * a_ - b_; }

  GSVector4 nmadd(const GSVector4& a_, const GSVector4& b_) const { return b_ - *this * a_; }

  GSVector4 nmsub(const GSVector4& a_, const GSVector4& b_) const { return -b_ - *this * a_; }

  GSVector4 addm(const GSVector4& a_, const GSVector4& b_) const
  {
    return a_.madd(b_, *this); // *this + a * b
  }

  GSVector4 subm(const GSVector4& a_, const GSVector4& b_) const
  {
    return a_.nmadd(b_, *this); // *this - a * b
  }

  GSVector4 hadd() const { return GSVector4(x + y, z + w, x + y, z + w); }

  GSVector4 hadd(const GSVector4& v) const { return GSVector4(x + y, z + w, v.x + v.y, v.z + v.w); }

  GSVector4 hsub() const { return GSVector4(x - y, z - w, x - y, z - w); }

  GSVector4 hsub(const GSVector4& v) const { return GSVector4(x - y, z - w, v.x - v.y, v.z - v.w); }

  template<int i>
  GSVector4 dp(const GSVector4& v) const
  {
    float res = 0.0f;
    if constexpr (i & 0x10)
      res += x * v.x;
    if constexpr (i & 0x20)
      res += y * v.y;
    if constexpr (i & 0x40)
      res += z * v.z;
    if constexpr (i & 0x80)
      res += w * v.w;
    return GSVector4((i & 0x01) ? res : 0.0f, (i & 0x02) ? res : 0.0f, (i & 0x04) ? res : 0.0f,
                     (i & 0x08) ? res : 0.0f);
  }

  GSVector4 sat(const GSVector4& min, const GSVector4& max) const
  {
    return GSVector4(std::clamp(x, min.x, max.x), std::clamp(y, min.y, max.y), std::clamp(z, min.z, max.z),
                     std::clamp(w, min.w, max.w));
  }

  GSVector4 sat(const GSVector4& v) const
  {
    return GSVector4(std::clamp(x, v.x, v.z), std::clamp(y, v.y, v.w), std::clamp(z, v.x, v.z),
                     std::clamp(w, v.y, v.w));
  }

  GSVector4 sat(const float scale = 255) const { return sat(zero(), GSVector4(scale)); }

  GSVector4 clamp(const float scale = 255) const { return min(GSVector4(scale)); }

  GSVector4 min(const GSVector4& v) const
  {
    return GSVector4(std::min(x, v.x), std::min(y, v.y), std::min(z, v.z), std::min(w, v.w));
  }

  GSVector4 max(const GSVector4& v) const
  {
    return GSVector4(std::max(x, v.x), std::max(y, v.y), std::max(z, v.z), std::max(w, v.w));
  }

  template<int mask>
  GSVector4 blend32(const GSVector4& v) const
  {
    return GSVector4(v.F32[mask & 1], v.F32[(mask >> 1) & 1], v.F32[(mask >> 2) & 1], v.F32[(mask >> 3) & 1]);
  }

  ALWAYS_INLINE GSVector4 blend32(const GSVector4& v, const GSVector4& mask) const
  {
    return GSVector4((mask.U32[0] & 0x80000000u) ? v.x : x, (mask.U32[1] & 0x80000000u) ? v.y : y,
                     (mask.U32[2] & 0x80000000u) ? v.z : z, (mask.U32[3] & 0x80000000u) ? v.w : w);
  }

  GSVector4 upl(const GSVector4& v) const { return GSVector4(x, y, v.x, v.y); }

  GSVector4 uph(const GSVector4& v) const { return GSVector4(z, w, v.z, v.w); }

  GSVector4 upld(const GSVector4& v) const
  {
    GSVector4 ret;
    ret.U64[0] = U64[0];
    ret.U64[1] = v.U64[0];
    return ret;
  }

  GSVector4 uphd(const GSVector4& v) const
  {
    GSVector4 ret;
    ret.U64[0] = U64[1];
    ret.U64[1] = v.U64[1];
    return ret;
  }

  ALWAYS_INLINE GSVector4 l2h(const GSVector4& v) const { return GSVector4(x, y, v.x, v.y); }

  ALWAYS_INLINE GSVector4 h2l(const GSVector4& v) const { return GSVector4(v.z, v.w, z, w); }

  ALWAYS_INLINE GSVector4 andnot(const GSVector4& v) const
  {
    GSVector4 ret;
    ret.U32[0] = ((~v.U32[0]) & U32[0]);
    ret.U32[1] = ((~v.U32[1]) & U32[1]);
    ret.U32[2] = ((~v.U32[2]) & U32[2]);
    ret.U32[3] = ((~v.U32[3]) & U32[3]);
    return ret;
  }

  ALWAYS_INLINE int mask() const
  {
    return (U32[0] >> 31) | ((U32[1] >> 30) & 2) | ((U32[2] >> 29) & 4) | ((U32[3] >> 28) & 8);
  }

  ALWAYS_INLINE bool alltrue() const { return ((U64[0] & U64[1]) == 0xFFFFFFFFFFFFFFFFULL); }

  ALWAYS_INLINE bool allfalse() const { return ((U64[0] | U64[1]) == 0); }

  ALWAYS_INLINE GSVector4 replace_nan(const GSVector4& v) const { return v.blend32(*this, *this == *this); }

  template<int src, int dst>
  ALWAYS_INLINE GSVector4 insert32(const GSVector4& v) const
  {
    GSVector4 ret = *this;
    ret.F32[dst] = v.F32[src];
    return ret;
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return I32[i];
  }

  ALWAYS_INLINE static constexpr GSVector4 zero() { return GSVector4::cxpr(0.0f, 0.0f, 0.0f, 0.0f); }

  ALWAYS_INLINE static constexpr GSVector4 xffffffff()
  {
    GSVector4 ret = zero();
    ret.U64[0] = ~ret.U64[0];
    ret.U64[1] = ~ret.U64[1];
    return ret;
  }

  ALWAYS_INLINE static GSVector4 loadl(const void* p)
  {
    GSVector4 ret;
    std::memcpy(&ret.x, p, sizeof(float) * 2);
    ret.z = 0.0f;
    ret.w = 0.0f;
    return ret;
  }

  ALWAYS_INLINE static GSVector4 load(float f) { return GSVector4(f, f, f, f); }

  template<bool aligned>
  ALWAYS_INLINE static GSVector4 load(const void* p)
  {
    GSVector4 ret;
    std::memcpy(&ret.x, p, sizeof(float) * 4);
    return ret;
  }

  ALWAYS_INLINE static void storent(void* p, const GSVector4& v) { std::memcpy(p, &v, sizeof(v)); }

  ALWAYS_INLINE static void storel(void* p, const GSVector4& v) { std::memcpy(p, &v.x, sizeof(float) * 2); }

  ALWAYS_INLINE static void storeh(void* p, const GSVector4& v) { std::memcpy(p, &v.z, sizeof(float) * 2); }

  template<bool aligned>
  ALWAYS_INLINE static void store(void* p, const GSVector4& v)
  {
    std::memcpy(p, v.F32, sizeof(F32));
  }

  ALWAYS_INLINE static void store(float* p, const GSVector4& v) { *p = v.x; }

  ALWAYS_INLINE GSVector4 operator-() const { return neg(); }

  void operator+=(const GSVector4& v_)
  {
    x = x + v_.x;
    y = y + v_.y;
    z = z + v_.z;
    w = w + v_.w;
  }
  void operator-=(const GSVector4& v_)
  {
    x = x - v_.x;
    y = y - v_.y;
    z = z - v_.z;
    w = w - v_.w;
  }
  void operator*=(const GSVector4& v_)
  {
    x = x * v_.x;
    y = y * v_.y;
    z = z * v_.z;
    w = w * v_.w;
  }
  void operator/=(const GSVector4& v_)
  {
    x = x / v_.x;
    y = y / v_.y;
    z = z / v_.z;
    w = w / v_.w;
  }

  void operator+=(const float v_)
  {
    x = x + v_;
    y = y + v_;
    z = z + v_;
    w = w + v_;
  }
  void operator-=(const float v_)
  {
    x = x - v_;
    y = y - v_;
    z = z - v_;
    w = w - v_;
  }
  void operator*=(const float v_)
  {
    x = x * v_;
    y = y * v_;
    z = z * v_;
    w = w * v_;
  }
  void operator/=(const float v_)
  {
    x = x / v_;
    y = y / v_;
    z = z / v_;
    w = w / v_;
  }

  void operator&=(const GSVector4& v_)
  {
    U64[0] &= v_.U64[0];
    U64[1] &= v_.U64[1];
  }
  void operator|=(const GSVector4& v_)
  {
    U64[0] |= v_.U64[0];
    U64[1] |= v_.U64[1];
  }
  void operator^=(const GSVector4& v_)
  {
    U64[0] ^= v_.U64[0];
    U64[1] ^= v_.U64[1];
  }

  friend GSVector4 operator+(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(v1.x + v2.x, v1.y + v2.y, v1.z + v2.z, v1.w + v2.w);
  }

  friend GSVector4 operator-(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(v1.x - v2.x, v1.y - v2.y, v1.z - v2.z, v1.w - v2.w);
  }

  friend GSVector4 operator*(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(v1.x * v2.x, v1.y * v2.y, v1.z * v2.z, v1.w * v2.w);
  }

  friend GSVector4 operator/(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(v1.x / v2.x, v1.y / v2.y, v1.z / v2.z, v1.w / v2.w);
  }

  friend GSVector4 operator+(const GSVector4& v, float f) { return GSVector4(v.x + f, v.y + f, v.z + f, v.w + f); }

  friend GSVector4 operator-(const GSVector4& v, float f) { return GSVector4(v.x - f, v.y - f, v.z - f, v.w - f); }

  friend GSVector4 operator*(const GSVector4& v, float f) { return GSVector4(v.x * f, v.y * f, v.z * f, v.w * f); }

  friend GSVector4 operator/(const GSVector4& v, float f) { return GSVector4(v.x / f, v.y / f, v.z / f, v.w / f); }

  friend GSVector4 operator&(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.U64[0] = v1.U64[0] & v2.U64[0];
    ret.U64[1] = v1.U64[1] & v2.U64[1];
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator|(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.U64[0] = v1.U64[0] | v2.U64[0];
    ret.U64[1] = v1.U64[1] | v2.U64[1];
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator^(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.U64[0] = v1.U64[0] ^ v2.U64[0];
    ret.U64[1] = v1.U64[1] ^ v2.U64[1];
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator==(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.I32[0] = (v1.x == v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y == v2.y) ? -1 : 0;
    ret.I32[2] = (v1.z == v2.z) ? -1 : 0;
    ret.I32[3] = (v1.w == v2.w) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator!=(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.I32[0] = (v1.x != v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y != v2.y) ? -1 : 0;
    ret.I32[2] = (v1.z != v2.z) ? -1 : 0;
    ret.I32[3] = (v1.w != v2.w) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator>(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.I32[0] = (v1.x > v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y > v2.y) ? -1 : 0;
    ret.I32[2] = (v1.z > v2.z) ? -1 : 0;
    ret.I32[3] = (v1.w > v2.w) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator<(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.I32[0] = (v1.x < v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y < v2.y) ? -1 : 0;
    ret.I32[2] = (v1.z < v2.z) ? -1 : 0;
    ret.I32[3] = (v1.w < v2.w) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator>=(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.I32[0] = (v1.x >= v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y >= v2.y) ? -1 : 0;
    ret.I32[2] = (v1.z >= v2.z) ? -1 : 0;
    ret.I32[3] = (v1.w >= v2.w) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE friend GSVector4 operator<=(const GSVector4& v1, const GSVector4& v2)
  {
    GSVector4 ret;
    ret.I32[0] = (v1.x <= v2.x) ? -1 : 0;
    ret.I32[1] = (v1.y <= v2.y) ? -1 : 0;
    ret.I32[2] = (v1.z <= v2.z) ? -1 : 0;
    ret.I32[3] = (v1.w <= v2.w) ? -1 : 0;
    return ret;
  }

  ALWAYS_INLINE GSVector4 mul64(const GSVector4& v_) const
  {
    GSVector4 ret;
    ret.F64[0] = F64[0] * v_.F64[0];
    ret.F64[1] = F64[1] * v_.F64[1];
    return ret;
  }

  ALWAYS_INLINE GSVector4 add64(const GSVector4& v_) const
  {
    GSVector4 ret;
    ret.F64[0] = F64[0] + v_.F64[0];
    ret.F64[1] = F64[1] + v_.F64[1];
    return ret;
  }

  ALWAYS_INLINE GSVector4 sub64(const GSVector4& v_) const
  {
    GSVector4 ret;
    ret.F64[0] = F64[0] - v_.F64[0];
    ret.F64[1] = F64[1] - v_.F64[1];
    return ret;
  }

  ALWAYS_INLINE static GSVector4 f32to64(const GSVector4& v_)
  {
    GSVector4 ret;
    ret.F64[0] = v_.x;
    ret.F64[1] = v_.y;
    return ret;
  }

  ALWAYS_INLINE static GSVector4 f32to64(const void* p)
  {
    float f[2];
    std::memcpy(f, p, sizeof(f));
    GSVector4 ret;
    ret.F64[0] = f[0];
    ret.F64[1] = f[1];
    return ret;
  }

  ALWAYS_INLINE GSVector4i f64toi32(bool truncate = true) const
  {
    return GSVector4i(static_cast<s32>(F64[0]), static_cast<s32>(F64[1]), 0, 0);
  }

  // clang-format off

#define VECTOR4_SHUFFLE_4(xs, xn, ys, yn, zs, zn, ws, wn) \
    ALWAYS_INLINE GSVector4 xs##ys##zs##ws() const { return GSVector4(F32[xn], F32[yn], F32[zn], F32[wn]); } \
    ALWAYS_INLINE GSVector4 xs##ys##zs##ws(const GSVector4& v_) const { return GSVector4(F32[xn], F32[yn], v_.F32[zn], v_.F32[wn]); }

#define VECTOR4_SHUFFLE_3(xs, xn, ys, yn, zs, zn) \
    VECTOR4_SHUFFLE_4(xs, xn, ys, yn, zs, zn, x, 0) \
    VECTOR4_SHUFFLE_4(xs, xn, ys, yn, zs, zn, y, 1) \
    VECTOR4_SHUFFLE_4(xs, xn, ys, yn, zs, zn, z, 2) \
    VECTOR4_SHUFFLE_4(xs, xn, ys, yn, zs, zn, w, 3) \

#define VECTOR4_SHUFFLE_2(xs, xn, ys, yn) \
    VECTOR4_SHUFFLE_3(xs, xn, ys, yn, x, 0) \
    VECTOR4_SHUFFLE_3(xs, xn, ys, yn, y, 1) \
    VECTOR4_SHUFFLE_3(xs, xn, ys, yn, z, 2) \
    VECTOR4_SHUFFLE_3(xs, xn, ys, yn, w, 3) \

#define VECTOR4_SHUFFLE_1(xs, xn) \
    VECTOR4_SHUFFLE_2(xs, xn, x, 0) \
    VECTOR4_SHUFFLE_2(xs, xn, y, 1) \
    VECTOR4_SHUFFLE_2(xs, xn, z, 2) \
    VECTOR4_SHUFFLE_2(xs, xn, w, 3) \

  VECTOR4_SHUFFLE_1(x, 0)
    VECTOR4_SHUFFLE_1(y, 1)
    VECTOR4_SHUFFLE_1(z, 2)
    VECTOR4_SHUFFLE_1(w, 3)

  // clang-format on

  ALWAYS_INLINE GSVector4 broadcast32() const { return GSVector4(x, x, x, x); }

  ALWAYS_INLINE static GSVector4 broadcast32(const GSVector4& v) { return GSVector4(v.x, v.x, v.x, v.x); }

  ALWAYS_INLINE static GSVector4 broadcast32(const void* f)
  {
    float ff;
    std::memcpy(&ff, f, sizeof(ff));
    return GSVector4(ff, ff, ff, ff);
  }

  ALWAYS_INLINE static GSVector4 broadcast64(const void* d)
  {
    GSVector4 ret;
    std::memcpy(&ret.F64[0], d, sizeof(ret.F64[0]));
    ret.F64[1] = ret.F64[0];
    return ret;
  }
};

ALWAYS_INLINE GSVector2i::GSVector2i(const GSVector2& v, bool truncate)
{
  // TODO: Truncation vs rounding...
  x = static_cast<s32>(v.x);
  y = static_cast<s32>(v.y);
}

ALWAYS_INLINE GSVector2::GSVector2(const GSVector2i& v)
{
  x = static_cast<float>(v.x);
  y = static_cast<float>(v.y);
}

ALWAYS_INLINE GSVector2i GSVector2i::cast(const GSVector2& v)
{
  GSVector2i ret;
  std::memcpy(&ret, &v, sizeof(ret));
  return ret;
}

ALWAYS_INLINE GSVector2 GSVector2::cast(const GSVector2i& v)
{
  GSVector2 ret;
  std::memcpy(&ret, &v, sizeof(ret));
  return ret;
}

ALWAYS_INLINE GSVector4i::GSVector4i(const GSVector4& v, bool truncate)
{
  // TODO: Truncation vs rounding...
  x = static_cast<s32>(v.x);
  y = static_cast<s32>(v.y);
  z = static_cast<s32>(v.z);
  w = static_cast<s32>(v.w);
}

ALWAYS_INLINE GSVector4::GSVector4(const GSVector4i& v)
{
  x = static_cast<float>(v.x);
  y = static_cast<float>(v.y);
  z = static_cast<float>(v.z);
  w = static_cast<float>(v.w);
}

ALWAYS_INLINE GSVector4i GSVector4i::cast(const GSVector4& v)
{
  GSVector4i ret;
  std::memcpy(&ret, &v, sizeof(ret));
  return ret;
}

ALWAYS_INLINE GSVector4 GSVector4::cast(const GSVector4i& v)
{
  GSVector4 ret;
  std::memcpy(&ret, &v, sizeof(ret));
  return ret;
}

#undef SSATURATE8
#undef USATURATE8
#undef SSATURATE16
#undef USATURATE16
#undef ALL_LANES_8
#undef ALL_LANES_16
#undef ALL_LANES_32
#undef ALL_LANES_64
