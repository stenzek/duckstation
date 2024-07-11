// SPDX-FileCopyrightText: 2021-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "common/intrin.h"
#include "common/types.h"

#include <algorithm>

#define GSVECTOR_HAS_UNSIGNED 1
#define GSVECTOR_HAS_SRLV 1

class GSVector2;
class GSVector2i;
class GSVector4;
class GSVector4i;

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
    int32x2_t v2s;
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

  ALWAYS_INLINE GSVector2i(s32 x, s32 y) { v2s = vset_lane_s32(y, vdup_n_s32(x), 1); }

  ALWAYS_INLINE GSVector2i(s16 s0, s16 s1, s16 s2, s16 s3) : I16{s0, s1, s2, s3} {}

  ALWAYS_INLINE constexpr GSVector2i(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7)
    : I8{b0, b1, b2, b3, b4, b5, b6, b7}
  {
  }

  // MSVC has bad codegen for the constexpr version when applied to non-constexpr things (https://godbolt.org/z/h8qbn7),
  // so leave the non-constexpr version default
  ALWAYS_INLINE explicit GSVector2i(int i) { *this = i; }

  ALWAYS_INLINE constexpr explicit GSVector2i(int32x2_t m) : v2s(m) {}

  ALWAYS_INLINE explicit GSVector2i(const GSVector2& v, bool truncate = true);

  ALWAYS_INLINE static GSVector2i cast(const GSVector2& v);

  ALWAYS_INLINE void operator=(int i) { v2s = vdup_n_s32(i); }

  ALWAYS_INLINE operator int32x2_t() const { return v2s; }

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

  ALWAYS_INLINE GSVector2i min_i8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vmin_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i max_i8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vmax_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i min_i16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vmin_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i max_i16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vmax_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i min_i32(const GSVector2i& v) const { return GSVector2i(vmin_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i max_i32(const GSVector2i& v) const { return GSVector2i(vmax_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i min_u8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u8(vmin_u8(vreinterpret_u8_s32(v2s), vreinterpret_u8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i max_u8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u8(vmax_u8(vreinterpret_u8_s32(v2s), vreinterpret_u8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i min_u16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u16(vmin_u16(vreinterpret_u16_s32(v2s), vreinterpret_u16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i max_u16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u16(vmax_u16(vreinterpret_u16_s32(v2s), vreinterpret_u16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i min_u32(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u32(vmin_u32(vreinterpret_u32_s32(v2s), vreinterpret_u32_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i max_u32(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u32(vmax_u32(vreinterpret_u32_s32(v2s), vreinterpret_u32_s32(v.v2s))));
  }

  ALWAYS_INLINE u8 minv_u8() const { return vminv_u8(vreinterpret_u8_s32(v2s)); }

  ALWAYS_INLINE u16 maxv_u8() const { return vmaxv_u8(vreinterpret_u8_s32(v2s)); }

  ALWAYS_INLINE u16 minv_u16() const { return vminv_u16(vreinterpret_u16_s32(v2s)); }

  ALWAYS_INLINE u16 maxv_u16() const { return vmaxv_u16(vreinterpret_u16_s32(v2s)); }

  ALWAYS_INLINE s32 minv_s32() const { return vminv_s32(v2s); }

  ALWAYS_INLINE u32 minv_u32() const { return vminv_u32(v2s); }

  ALWAYS_INLINE s32 maxv_s32() const { return vmaxv_s32(v2s); }

  ALWAYS_INLINE u32 maxv_u32() const { return vmaxv_u32(v2s); }

  ALWAYS_INLINE GSVector2i clamp8() const { return pu16().upl8(); }

  ALWAYS_INLINE GSVector2i blend8(const GSVector2i& a, const GSVector2i& mask) const
  {
    uint8x8_t mask2 = vreinterpret_u8_s8(vshr_n_s8(vreinterpret_s8_s32(mask.v2s), 7));
    return GSVector2i(vreinterpret_s32_u8(vbsl_u8(mask2, vreinterpret_u8_s32(a.v2s), vreinterpret_u8_s32(v2s))));
  }

  template<int mask>
  ALWAYS_INLINE GSVector2i blend16(const GSVector2i& a) const
  {
    static constexpr const uint16_t _mask[4] = {
      ((mask) & (1 << 0)) ? (uint16_t)-1 : 0x0, ((mask) & (1 << 1)) ? (uint16_t)-1 : 0x0,
      ((mask) & (1 << 2)) ? (uint16_t)-1 : 0x0, ((mask) & (1 << 3)) ? (uint16_t)-1 : 0x0};
    return GSVector2i(
      vreinterpret_s32_u16(vbsl_u16(vld1_u16(_mask), vreinterpret_u16_s32(a.v2s), vreinterpret_u16_s32(v2s))));
  }

  template<int mask>
  ALWAYS_INLINE GSVector2i blend32(const GSVector2i& v) const
  {
    constexpr int bit1 = ((mask & 2) * 3) << 1;
    constexpr int bit0 = (mask & 1) * 3;
    return blend16<bit1 | bit0>(v);
  }

  ALWAYS_INLINE GSVector2i blend(const GSVector2i& v, const GSVector2i& mask) const
  {
    return GSVector2i(vreinterpret_s32_s8(vorr_s8(vbic_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(mask.v2s)),
                                                  vand_s8(vreinterpret_s8_s32(mask.v2s), vreinterpret_s8_s32(v.v2s)))));
  }

  ALWAYS_INLINE GSVector2i mix16(const GSVector2i& v) const { return blend16<0xa>(v); }

  ALWAYS_INLINE GSVector2i shuffle8(const GSVector2i& mask) const
  {
    return GSVector2i(vreinterpret_s32_s8(vtbl1_s8(vreinterpret_s8_s32(v2s), vreinterpret_u8_s32(mask.v2s))));
  }

  ALWAYS_INLINE GSVector2i ps16() const
  {
    return GSVector2i(vreinterpret_s32_s8(vqmovn_s16(vcombine_s16(vreinterpret_s16_s32(v2s), vcreate_s16(0)))));
  }

  ALWAYS_INLINE GSVector2i pu16() const
  {
    return GSVector2i(vreinterpret_s32_u8(vqmovn_u16(vcombine_u16(vreinterpret_u16_s32(v2s), vcreate_u16(0)))));
  }

  ALWAYS_INLINE GSVector2i ps32() const
  {
    return GSVector2i(vreinterpret_s32_s16(vqmovn_s16(vcombine_s32(v2s, vcreate_s32(0)))));
  }

  ALWAYS_INLINE GSVector2i pu32() const
  {
    return GSVector2i(vreinterpret_s32_u16(vqmovn_u32(vcombine_u32(vreinterpret_u32_s32(v2s), vcreate_u32(0)))));
  }

  ALWAYS_INLINE GSVector2i upl8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vzip1_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i upl16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vzip1_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }
  ALWAYS_INLINE GSVector2i upl32(const GSVector2i& v) const { return GSVector2i(vzip1_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i upl8() const
  {
    return GSVector2i(vreinterpret_s32_s8(vzip1_s8(vreinterpret_s8_s32(v2s), vdup_n_s8(0))));
  }

  ALWAYS_INLINE GSVector2i upl16() const
  {
    return GSVector2i(vreinterpret_s32_s16(vzip1_s16(vreinterpret_s16_s32(v2s), vdup_n_s16(0))));
  }

  ALWAYS_INLINE GSVector2i upl32() const { return GSVector2i(vzip1_s32(v2s, vdup_n_s32(0))); }

  ALWAYS_INLINE GSVector2i i8to16() const
  {
    return GSVector2i(vreinterpret_s32_s16(vget_low_s8(vmovl_s8(vreinterpret_s8_s32(v2s)))));
  }

  ALWAYS_INLINE GSVector2i u8to16() const
  {
    return GSVector2i(vreinterpret_s32_u16(vget_low_u8(vmovl_u8(vreinterpret_u8_s32(v2s)))));
  }

  template<int i>
  ALWAYS_INLINE GSVector2i srl() const
  {
    return GSVector2i(vreinterpret_s32_s8(vext_s8(vreinterpret_s8_s32(v2s), vdup_n_s8(0), i)));
  }

  template<int i>
  ALWAYS_INLINE GSVector2i sll() const
  {
    return GSVector2i(vreinterpret_s32_s8(vext_s8(vdup_n_s8(0), vreinterpret_s8_s32(v2s), 16 - i)));
  }

  template<int i>
  ALWAYS_INLINE GSVector2i sll16() const
  {
    return GSVector2i(vreinterpret_s32_s16(vshl_n_s16(vreinterpret_s16_s32(v2s), i)));
  }

  ALWAYS_INLINE GSVector2i sll16(s32 i) const
  {
    return GSVector2i(vreinterpret_s32_s16(vshl_s16(vreinterpret_s16_s32(v2s), vdup_n_s16(i))));
  }

  ALWAYS_INLINE GSVector2i sllv16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vshl_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  template<int i>
  ALWAYS_INLINE GSVector2i srl16() const
  {
    return GSVector2i(vreinterpret_s32_u16(vshr_n_u16(vreinterpret_u16_s32(v2s), i)));
  }

  ALWAYS_INLINE GSVector2i srl16(s32 i) const
  {
    return GSVector2i(vreinterpret_s32_u16(vshl_u16(vreinterpret_u16_s32(v2s), vdup_n_u16(-i))));
  }

  ALWAYS_INLINE GSVector2i srlv16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vshl_s16(vreinterpret_s16_s32(v2s), vneg_s16(vreinterpret_s16_s32(v.v2s)))));
  }

  template<int i>
  ALWAYS_INLINE GSVector2i sra16() const
  {
    constexpr int count = (i & ~15) ? 15 : i;
    return GSVector2i(vreinterpret_s32_s16(vshr_n_s16(vreinterpret_s16_s32(v2s), count)));
  }

  ALWAYS_INLINE GSVector2i sra16(s32 i) const
  {
    return GSVector2i(vreinterpret_s32_s16(vshl_s16(vreinterpret_s16_s32(v2s), vdup_n_s16(-i))));
  }

  ALWAYS_INLINE GSVector2i srav16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u16(vshl_u16(vreinterpret_u16_s32(v2s), vneg_s16(vreinterpret_s16_s32(v.v2s)))));
  }

  template<int i>
  ALWAYS_INLINE GSVector2i sll32() const
  {
    return GSVector2i(vshl_n_s32(v2s, i));
  }

  ALWAYS_INLINE GSVector2i sll32(s32 i) const { return GSVector2i(vshl_s32(v2s, vdup_n_s32(i))); }

  ALWAYS_INLINE GSVector2i sllv32(const GSVector2i& v) const { return GSVector2i(vshl_s32(v2s, v.v2s)); }

  template<int i>
  ALWAYS_INLINE GSVector2i srl32() const
  {
    return GSVector2i(vreinterpret_s32_u32(vshr_n_u32(vreinterpret_u32_s32(v2s), i)));
  }

  ALWAYS_INLINE GSVector2i srl32(s32 i) const
  {
    return GSVector2i(vreinterpret_s32_u32(vshl_u32(vreinterpret_u32_s32(v2s), vdup_n_s32(-i))));
  }

  ALWAYS_INLINE GSVector2i srlv32(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u32(vshl_u32(vreinterpret_u32_s32(v2s), vneg_s32(v.v2s))));
  }

  template<int i>
  ALWAYS_INLINE GSVector2i sra32() const
  {
    return GSVector2i(vshr_n_s32(v2s, i));
  }

  ALWAYS_INLINE GSVector2i sra32(s32 i) const { return GSVector2i(vshl_s32(v2s, vdup_n_s32(-i))); }

  ALWAYS_INLINE GSVector2i srav32(const GSVector2i& v) const
  {
    return GSVector2i(vshl_s32(vreinterpret_u32_s32(v2s), vneg_s32(v.v2s)));
  }

  ALWAYS_INLINE GSVector2i add8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vadd_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i add16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vadd_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i add32(const GSVector2i& v) const { return GSVector2i(vadd_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i adds8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vqadd_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i adds16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vqadd_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i addus8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u8(vqadd_u8(vreinterpret_u8_s32(v2s), vreinterpret_u8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i addus16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u16(vqadd_u16(vreinterpret_u16_s32(v2s), vreinterpret_u16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i sub8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vsub_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i sub16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vsub_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i sub32(const GSVector2i& v) const { return GSVector2i(vsub_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i subs8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vqsub_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i subs16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vqsub_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i subus8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u8(vqsub_u8(vreinterpret_u8_s32(v2s), vreinterpret_u8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i subus16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u16(vqsub_u16(vreinterpret_u16_s32(v2s), vreinterpret_u16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i avg8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u8(vrhadd_u8(vreinterpret_u8_s32(v2s), vreinterpret_u8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i avg16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u16(vrhadd_u16(vreinterpret_u16_s32(v2s), vreinterpret_u16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i mul16l(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vmul_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i mul32l(const GSVector2i& v) const { return GSVector2i(vmul_s32(v2s, v.v2s)); }

  ALWAYS_INLINE bool eq(const GSVector2i& v) const
  {
    return (vmaxv_u32(vreinterpret_u32_s32(veor_s32(v2s, v.v2s))) == 0);
  }

  ALWAYS_INLINE GSVector2i eq8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u8(vceq_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i eq16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u16(vceq_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i eq32(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u32(vceq_s32(v2s, v.v2s)));
  }

  ALWAYS_INLINE GSVector2i eq64(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_u64(vceq_s64(vreinterpret_s64_s32(v2s), vreinterpret_s64_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i neq8(const GSVector2i& v) const { return ~eq8(v); }

  ALWAYS_INLINE GSVector2i neq16(const GSVector2i& v) const { return ~eq16(v); }

  ALWAYS_INLINE GSVector2i neq32(const GSVector2i& v) const { return ~eq32(v); }

  ALWAYS_INLINE GSVector2i gt8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vcgt_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i gt16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vcgt_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i gt32(const GSVector2i& v) const { return GSVector2i(vcgt_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i ge8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vcge_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }
  ALWAYS_INLINE GSVector2i ge16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vcge_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }
  ALWAYS_INLINE GSVector2i ge32(const GSVector2i& v) const { return GSVector2i(vcge_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i lt8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vclt_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i lt16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vclt_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }

  ALWAYS_INLINE GSVector2i lt32(const GSVector2i& v) const { return GSVector2i(vclt_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i le8(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s8(vcle_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s))));
  }
  ALWAYS_INLINE GSVector2i le16(const GSVector2i& v) const
  {
    return GSVector2i(vreinterpret_s32_s16(vcle_s16(vreinterpret_s16_s32(v2s), vreinterpret_s16_s32(v.v2s))));
  }
  ALWAYS_INLINE GSVector2i le32(const GSVector2i& v) const { return GSVector2i(vcle_s32(v2s, v.v2s)); }

  ALWAYS_INLINE GSVector2i andnot(const GSVector2i& v) const { return GSVector2i(vbic_s32(v2s, v.v2s)); }

  ALWAYS_INLINE int mask() const
  {
    // borrowed from sse2neon
    const uint16x4_t high_bits = vreinterpret_u16_u8(vshr_n_u8(vreinterpret_u8_s32(v2s), 7));
    const uint32x2_t paired16 = vreinterpret_u32_u16(vsra_n_u16(high_bits, high_bits, 7));
    const uint64x1_t paired32 = vreinterpret_u64_u32(vsra_n_u32(paired16, paired16, 14));
    const uint8x8_t paired64 = vreinterpret_u8_u64(vsra_n_u64(paired32, paired32, 28));
    return static_cast<int>(vget_lane_u8(paired64, 0));
  }

  ALWAYS_INLINE bool alltrue() const
  {
    // MSB should be set in all 8-bit lanes.
    return (vminv_u8(vreinterpret_u8_s32(v2s)) & 0x80) == 0x80;
  }

  ALWAYS_INLINE bool allfalse() const
  {
    // MSB should be clear in all 8-bit lanes.
    return (vmaxv_u32(vreinterpret_u8_s32(v2s)) & 0x80) != 0x80;
  }

  template<int i>
  ALWAYS_INLINE GSVector2i insert8(int a) const
  {
    return GSVector2i(vreinterpret_s32_u8(vset_lane_u8(a, vreinterpret_u8_s32(v2s), static_cast<uint8_t>(i))));
  }

  template<int i>
  ALWAYS_INLINE int extract8() const
  {
    return vget_lane_u8(vreinterpret_u8_s32(v2s), i);
  }

  template<int i>
  ALWAYS_INLINE GSVector2i insert16(int a) const
  {
    return GSVector2i(vreinterpret_s32_u16(vset_lane_u16(a, vreinterpret_u16_s32(v2s), static_cast<uint16_t>(i))));
  }

  template<int i>
  ALWAYS_INLINE int extract16() const
  {
    return vget_lane_u16(vreinterpret_u16_s32(v2s), i);
  }

  template<int i>
  ALWAYS_INLINE GSVector2i insert32(int a) const
  {
    return GSVector2i(vset_lane_s32(a, v2s, i));
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return vget_lane_s32(v2s, i);
  }

  ALWAYS_INLINE static GSVector2i load32(const void* p)
  {
    // should be ldr s0, [x0]
    u32 val;
    std::memcpy(&val, p, sizeof(u32));
    return GSVector2i(vset_lane_u32(val, vdup_n_u32(0), 0));
  }

  ALWAYS_INLINE static GSVector2i load(const void* p) { return GSVector2i(vld1_s32((const int32_t*)p)); }

  ALWAYS_INLINE static GSVector2i load(int i) { return GSVector2i(vset_lane_s32(i, vdup_n_s32(0), 0)); }

  ALWAYS_INLINE static void store32(void* p, const GSVector2i& v)
  {
    s32 val = vget_lane_s32(v, 0);
    std::memcpy(p, &val, sizeof(s32));
  }

  ALWAYS_INLINE static void store(void* p, const GSVector2i& v) { vst1_s32((int32_t*)p, v.v2s); }

  ALWAYS_INLINE static int store(const GSVector2i& v) { return vget_lane_s32(v.v2s, 0); }

  ALWAYS_INLINE void operator&=(const GSVector2i& v)
  {
    v2s = vreinterpret_s32_s8(vand_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s)));
  }

  ALWAYS_INLINE void operator|=(const GSVector2i& v)
  {
    v2s = vreinterpret_s32_s8(vorr_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s)));
  }

  ALWAYS_INLINE void operator^=(const GSVector2i& v)
  {
    v2s = vreinterpret_s32_s8(veor_s8(vreinterpret_s8_s32(v2s), vreinterpret_s8_s32(v.v2s)));
  }

  ALWAYS_INLINE friend GSVector2i operator&(const GSVector2i& v1, const GSVector2i& v2)
  {
    return GSVector2i(vreinterpret_s32_s8(vand_s8(vreinterpret_s8_s32(v1.v2s), vreinterpret_s8_s32(v2.v2s))));
  }

  ALWAYS_INLINE friend GSVector2i operator|(const GSVector2i& v1, const GSVector2i& v2)
  {
    return GSVector2i(vreinterpret_s32_s8(vorr_s8(vreinterpret_s8_s32(v1.v2s), vreinterpret_s8_s32(v2.v2s))));
  }

  ALWAYS_INLINE friend GSVector2i operator^(const GSVector2i& v1, const GSVector2i& v2)
  {
    return GSVector2i(vreinterpret_s32_s8(veor_s8(vreinterpret_s8_s32(v1.v2s), vreinterpret_s8_s32(v2.v2s))));
  }

  ALWAYS_INLINE friend GSVector2i operator&(const GSVector2i& v, int i) { return v & GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator|(const GSVector2i& v, int i) { return v | GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator^(const GSVector2i& v, int i) { return v ^ GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator~(const GSVector2i& v) { return GSVector2i(vmvn_s32(v.v2s)); }

  ALWAYS_INLINE static GSVector2i zero() { return GSVector2i(0); }

  ALWAYS_INLINE GSVector2i xy() const { return *this; }
  ALWAYS_INLINE GSVector2i xx() const { return GSVector2i(__builtin_shufflevector(v2s, v2s, 0, 0)); }
  ALWAYS_INLINE GSVector2i yx() const { return GSVector2i(__builtin_shufflevector(v2s, v2s, 1, 0)); }
  ALWAYS_INLINE GSVector2i yy() const { return GSVector2i(__builtin_shufflevector(v2s, v2s, 1, 1)); }
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
    float F32[2];
    double F64[1];
    s8 I8[8];
    s16 I16[4];
    s32 I32[2];
    s64 I64[1];
    u8 U8[8];
    u16 U16[4];
    u32 U32[2];
    u64 U64[1];
    float32x2_t v2s;
  };

  GSVector2() = default;

  constexpr static GSVector2 cxpr(float x, float y) { return GSVector2(cxpr_init, x, y); }

  constexpr static GSVector2 cxpr(float x) { return GSVector2(cxpr_init, x, x); }

  constexpr static GSVector2 cxpr(int x, int y) { return GSVector2(cxpr_init, x, y); }

  constexpr static GSVector2 cxpr(int x) { return GSVector2(cxpr_init, x, x); }

  ALWAYS_INLINE GSVector2(float x, float y) : v2s(vset_lane_f32(y, vdup_n_f32(x), 1)) {}

  ALWAYS_INLINE GSVector2(int x, int y) : v2s(vcvt_f32_s32(vset_lane_s32(y, vdup_n_s32(x), 1))) {}

  ALWAYS_INLINE constexpr explicit GSVector2(float32x2_t m) : v2s(m) {}

  ALWAYS_INLINE explicit GSVector2(float f) { v2s = vdup_n_f32(f); }

  ALWAYS_INLINE explicit GSVector2(int i) { v2s = vcvt_f32_s32(vdup_n_s32(i)); }

  ALWAYS_INLINE explicit GSVector2(const GSVector2i& v);

  ALWAYS_INLINE static GSVector2 cast(const GSVector2i& v);

  ALWAYS_INLINE void operator=(float f) { v2s = vdup_n_f32(f); }

  ALWAYS_INLINE void operator=(float32x2_t m) { v2s = m; }

  ALWAYS_INLINE operator float32x2_t() const { return v2s; }

  ALWAYS_INLINE GSVector2 abs() const { return GSVector2(vabs_f32(v2s)); }

  ALWAYS_INLINE GSVector2 neg() const { return GSVector2(vneg_f32(v2s)); }

  ALWAYS_INLINE GSVector2 rcp() const { return GSVector2(vrecpe_f32(v2s)); }

  ALWAYS_INLINE GSVector2 rcpnr() const
  {
    float32x2_t recip = vrecpe_f32(v2s);
    recip = vmul_f32(recip, vrecps_f32(recip, v2s));
    return GSVector2(recip);
  }

  ALWAYS_INLINE GSVector2 floor() const { return GSVector2(vrndm_f32(v2s)); }

  ALWAYS_INLINE GSVector2 ceil() const { return GSVector2(vrndp_f32(v2s)); }

  ALWAYS_INLINE GSVector2 sat(const GSVector2& a, const GSVector2& b) const { return max(a).min(b); }

  ALWAYS_INLINE GSVector2 sat(const float scale = 255) const { return sat(zero(), GSVector2(scale)); }

  ALWAYS_INLINE GSVector2 clamp(const float scale = 255) const { return min(GSVector2(scale)); }

  ALWAYS_INLINE GSVector2 min(const GSVector2& a) const { return GSVector2(vmin_f32(v2s, a.v2s)); }

  ALWAYS_INLINE GSVector2 max(const GSVector2& a) const { return GSVector2(vmax_f32(v2s, a.v2s)); }

  template<int mask>
  ALWAYS_INLINE GSVector2 blend32(const GSVector2& a) const
  {
    return GSVector2(__builtin_shufflevector(v2s, a.v2s, (mask & 1) ? 4 : 0, (mask & 2) ? 5 : 1));
  }

  ALWAYS_INLINE GSVector2 blend32(const GSVector2& a, const GSVector2& mask) const
  {
    // duplicate sign bit across and bit select
    const uint32x2_t bitmask = vreinterpret_u32_s32(vshr_n_s32(vreinterpret_s32_f32(mask.v2s), 31));
    return GSVector2(vbsl_f32(bitmask, a.v2s, v2s));
  }

  ALWAYS_INLINE GSVector2 andnot(const GSVector2& v) const
  {
    return GSVector2(vreinterpret_f32_s32(vbic_s32(vreinterpret_s32_f32(v2s), vreinterpret_s32_f32(v.v2s))));
  }

  ALWAYS_INLINE int mask() const
  {
    const uint32x2_t masks = vshr_n_u32(vreinterpret_u32_s32(v2s), 31);
    return (vget_lane_u32(masks, 0) | (vget_lane_u32(masks, 1) << 1));
  }

  ALWAYS_INLINE bool alltrue() const { return (vget_lane_u64(vreinterpret_u64_f32(v2s), 0) == 0xFFFFFFFFFFFFFFFFULL); }

  ALWAYS_INLINE bool allfalse() const { return (vget_lane_u64(vreinterpret_u64_f32(v2s), 0) == 0); }

  ALWAYS_INLINE GSVector2 replace_nan(const GSVector2& v) const { return v.blend32(*this, *this == *this); }

  template<int src, int dst>
  ALWAYS_INLINE GSVector2 insert32(const GSVector2& v) const
  {
    return GSVector2(vcopy_lane_f32(v2s, dst, v.v2s, src));
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return vget_lane_s32(vreinterpret_s32_f32(v2s), i);
  }

  ALWAYS_INLINE static GSVector2 zero() { return GSVector2(vdup_n_f32(0.0f)); }

  ALWAYS_INLINE static GSVector2 xffffffff() { return GSVector2(vreinterpret_f32_u32(vdup_n_u32(0xFFFFFFFFu))); }

  ALWAYS_INLINE static GSVector2 load(float f) { return GSVector2(vset_lane_f32(f, vmov_n_f32(0.0f), 0)); }

  ALWAYS_INLINE static GSVector2 load(const void* p) { return GSVector2(vld1_f32((const float*)p)); }

  ALWAYS_INLINE static void store(void* p, const GSVector2& v) { vst1_f32((float*)p, v.v2s); }

  ALWAYS_INLINE GSVector2 operator-() const { return neg(); }

  ALWAYS_INLINE void operator+=(const GSVector2& v) { v2s = vadd_f32(v2s, v.v2s); }
  ALWAYS_INLINE void operator-=(const GSVector2& v) { v2s = vsub_f32(v2s, v.v2s); }
  ALWAYS_INLINE void operator*=(const GSVector2& v) { v2s = vmul_f32(v2s, v.v2s); }
  ALWAYS_INLINE void operator/=(const GSVector2& v) { v2s = vdiv_f32(v2s, v.v2s); }

  ALWAYS_INLINE void operator+=(float f) { *this += GSVector2(f); }
  ALWAYS_INLINE void operator-=(float f) { *this -= GSVector2(f); }
  ALWAYS_INLINE void operator*=(float f) { *this *= GSVector2(f); }
  ALWAYS_INLINE void operator/=(float f) { *this /= GSVector2(f); }

  ALWAYS_INLINE void operator&=(const GSVector2& v)
  {
    v2s = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(v2s), vreinterpret_u32_f32(v.v2s)));
  }

  ALWAYS_INLINE void operator|=(const GSVector2& v)
  {
    v2s = vreinterpret_f32_u32(vorr_u32(vreinterpret_u32_f32(v2s), vreinterpret_u32_f32(v.v2s)));
  }

  ALWAYS_INLINE void operator^=(const GSVector2& v)
  {
    v2s = vreinterpret_f32_u32(veor_u32(vreinterpret_u32_f32(v2s), vreinterpret_u32_f32(v.v2s)));
  }

  ALWAYS_INLINE friend GSVector2 operator+(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vadd_f32(v1.v2s, v2.v2s));
  }

  ALWAYS_INLINE friend GSVector2 operator-(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vsub_f32(v1.v2s, v2.v2s));
  }

  ALWAYS_INLINE friend GSVector2 operator*(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vmul_f32(v1.v2s, v2.v2s));
  }

  ALWAYS_INLINE friend GSVector2 operator/(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vdiv_f32(v1.v2s, v2.v2s));
  }

  ALWAYS_INLINE friend GSVector2 operator+(const GSVector2& v, float f) { return v + GSVector2(f); }
  ALWAYS_INLINE friend GSVector2 operator-(const GSVector2& v, float f) { return v - GSVector2(f); }
  ALWAYS_INLINE friend GSVector2 operator*(const GSVector2& v, float f) { return v * GSVector2(f); }
  ALWAYS_INLINE friend GSVector2 operator/(const GSVector2& v, float f) { return v / GSVector2(f); }

  ALWAYS_INLINE friend GSVector2 operator&(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(v1.v2s), vreinterpret_u32_f32(v2.v2s))));
  }

  ALWAYS_INLINE friend GSVector2 operator|(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(vorr_u32(vreinterpret_u32_f32(v1.v2s), vreinterpret_u32_f32(v2.v2s))));
  }

  ALWAYS_INLINE friend GSVector2 operator^(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(veor_u32(vreinterpret_u32_f32(v1.v2s), vreinterpret_u32_f32(v2.v2s))));
  }

  ALWAYS_INLINE friend GSVector2 operator==(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(vceq_f32(v1.v2s, v2.v2s)));
  }

  ALWAYS_INLINE friend GSVector2 operator!=(const GSVector2& v1, const GSVector2& v2)
  {
    // NEON has no !=
    return GSVector2(vreinterpret_f32_u32(vmvn_u32(vceq_f32(v1.v2s, v2.v2s))));
  }

  ALWAYS_INLINE friend GSVector2 operator>(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(vcgt_f32(v1.v2s, v2.v2s)));
  }

  ALWAYS_INLINE friend GSVector2 operator<(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(vclt_f32(v1.v2s, v2.v2s)));
  }

  ALWAYS_INLINE friend GSVector2 operator>=(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(vcge_f32(v1.v2s, v2.v2s)));
  }

  ALWAYS_INLINE friend GSVector2 operator<=(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(vreinterpret_f32_u32(vcle_f32(v1.v2s, v2.v2s)));
  }

  ALWAYS_INLINE GSVector2 xy() const { return *this; }
  ALWAYS_INLINE GSVector2 xx() const { return GSVector2(__builtin_shufflevector(v2s, v2s, 0, 0)); }
  ALWAYS_INLINE GSVector2 yx() const { return GSVector2(__builtin_shufflevector(v2s, v2s, 1, 0)); }
  ALWAYS_INLINE GSVector2 yy() const { return GSVector2(__builtin_shufflevector(v2s, v2s, 1, 1)); }
};

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
      int x, y, z, w;
    };
    struct
    {
      int r, g, b, a;
    };
    struct
    {
      int left, top, right, bottom;
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
    int32x4_t v4s;
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
    GSVector4i xz = load(x).upl32(load(z));
    GSVector4i yw = load(y).upl32(load(w));

    *this = xz.upl32(yw);
  }

  ALWAYS_INLINE GSVector4i(s32 x, s32 y) { *this = load(x).upl32(load(y)); }

  ALWAYS_INLINE GSVector4i(s16 s0, s16 s1, s16 s2, s16 s3, s16 s4, s16 s5, s16 s6, s16 s7)
    : I16{s0, s1, s2, s3, s4, s5, s6, s7}
  {
  }

  constexpr GSVector4i(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7, s8 b8, s8 b9, s8 b10, s8 b11, s8 b12,
                       s8 b13, s8 b14, s8 b15)
    : I8{b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15}
  {
  }

  // MSVC has bad codegen for the constexpr version when applied to non-constexpr things (https://godbolt.org/z/h8qbn7),
  // so leave the non-constexpr version default
  ALWAYS_INLINE explicit GSVector4i(int i) { *this = i; }

  ALWAYS_INLINE explicit GSVector4i(int32x2_t m) : v4s(vcombine_s32(m, vcreate_s32(0))) {}
  ALWAYS_INLINE constexpr explicit GSVector4i(int32x4_t m) : v4s(m) {}

  ALWAYS_INLINE explicit GSVector4i(const GSVector2& v, bool truncate = true);
  ALWAYS_INLINE explicit GSVector4i(const GSVector4& v, bool truncate = true);

  ALWAYS_INLINE static GSVector4i cast(const GSVector4& v);

  ALWAYS_INLINE void operator=(int i) { v4s = vdupq_n_s32(i); }

  ALWAYS_INLINE operator int32x4_t() const { return v4s; }

  // rect

  ALWAYS_INLINE int width() const { return right - left; }

  ALWAYS_INLINE int height() const { return bottom - top; }

  ALWAYS_INLINE GSVector4i rsize() const
  {
    return sub32(xyxy()); // same as GSVector4i(0, 0, width(), height());
  }

  ALWAYS_INLINE s32 rarea() const { return width() * height(); }

  ALWAYS_INLINE bool rempty() const { return (vminv_u32(vreinterpret_u32_s32(vget_low_s32(lt32(zwzw())))) == 0); }

  ALWAYS_INLINE GSVector4i runion(const GSVector4i& a) const { return min_i32(a).upl64(max_i32(a).srl<8>()); }

  ALWAYS_INLINE GSVector4i rintersect(const GSVector4i& a) const { return sat_i32(a); }
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

  ALWAYS_INLINE GSVector4i min_i8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vminq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i max_i8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vmaxq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i min_i16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vminq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i max_i16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vmaxq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i min_i32(const GSVector4i& v) const { return GSVector4i(vminq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i max_i32(const GSVector4i& v) const { return GSVector4i(vmaxq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i min_u8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u8(vminq_u8(vreinterpretq_u8_s32(v4s), vreinterpretq_u8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i max_u8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u8(vmaxq_u8(vreinterpretq_u8_s32(v4s), vreinterpretq_u8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i min_u16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vminq_u16(vreinterpretq_u16_s32(v4s), vreinterpretq_u16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i max_u16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vmaxq_u16(vreinterpretq_u16_s32(v4s), vreinterpretq_u16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i min_u32(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u32(vminq_u32(vreinterpretq_u32_s32(v4s), vreinterpretq_u32_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i max_u32(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u32(vmaxq_u32(vreinterpretq_u32_s32(v4s), vreinterpretq_u32_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i madd_s16(const GSVector4i& v) const
  {
    int32x4_t acc =
      vmlal_s16(vdupq_n_s32(0), vget_low_s16(vreinterpretq_s16_s32(v4s)), vget_low_s16(vreinterpretq_s16_s32(v.v4s)));
    acc = vmlal_high_s16(acc, vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s));
    return GSVector4i(acc);
  }

  ALWAYS_INLINE GSVector4i addp_s32() const { return GSVector4i(vpaddq_s32(v4s, v4s)); }

  ALWAYS_INLINE u8 minv_u8() const { return vminvq_u8(vreinterpretq_u8_s32(v4s)); }

  ALWAYS_INLINE u16 maxv_u8() const { return vmaxvq_u8(vreinterpretq_u8_s32(v4s)); }

  ALWAYS_INLINE u16 minv_u16() const { return vminvq_u16(vreinterpretq_u16_s32(v4s)); }

  ALWAYS_INLINE u16 maxv_u16() const { return vmaxvq_u16(vreinterpretq_u16_s32(v4s)); }

  ALWAYS_INLINE s32 minv_s32() const { return vminvq_s32(v4s); }

  ALWAYS_INLINE u32 minv_u32() const { return vminvq_u32(v4s); }

  ALWAYS_INLINE s32 maxv_s32() const { return vmaxvq_s32(v4s); }

  ALWAYS_INLINE u32 maxv_u32() const { return vmaxvq_u32(v4s); }

  ALWAYS_INLINE GSVector4i clamp8() const { return pu16().upl8(); }

  ALWAYS_INLINE GSVector4i blend8(const GSVector4i& a, const GSVector4i& mask) const
  {
    uint8x16_t mask2 = vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_s32(mask.v4s), 7));
    return GSVector4i(vreinterpretq_s32_u8(vbslq_u8(mask2, vreinterpretq_u8_s32(a.v4s), vreinterpretq_u8_s32(v4s))));
  }

  template<int mask>
  ALWAYS_INLINE GSVector4i blend16(const GSVector4i& a) const
  {
    static constexpr const uint16_t _mask[8] = {
      ((mask) & (1 << 0)) ? (uint16_t)-1 : 0x0, ((mask) & (1 << 1)) ? (uint16_t)-1 : 0x0,
      ((mask) & (1 << 2)) ? (uint16_t)-1 : 0x0, ((mask) & (1 << 3)) ? (uint16_t)-1 : 0x0,
      ((mask) & (1 << 4)) ? (uint16_t)-1 : 0x0, ((mask) & (1 << 5)) ? (uint16_t)-1 : 0x0,
      ((mask) & (1 << 6)) ? (uint16_t)-1 : 0x0, ((mask) & (1 << 7)) ? (uint16_t)-1 : 0x0};
    return GSVector4i(
      vreinterpretq_s32_u16(vbslq_u16(vld1q_u16(_mask), vreinterpretq_u16_s32(a.v4s), vreinterpretq_u16_s32(v4s))));
  }

  template<int mask>
  ALWAYS_INLINE GSVector4i blend32(const GSVector4i& v) const
  {
    constexpr int bit3 = ((mask & 8) * 3) << 3;
    constexpr int bit2 = ((mask & 4) * 3) << 2;
    constexpr int bit1 = ((mask & 2) * 3) << 1;
    constexpr int bit0 = (mask & 1) * 3;
    return blend16<bit3 | bit2 | bit1 | bit0>(v);
  }

  ALWAYS_INLINE GSVector4i blend(const GSVector4i& v, const GSVector4i& mask) const
  {
    return GSVector4i(
      vreinterpretq_s32_s8(vorrq_s8(vbicq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(mask.v4s)),
                                    vandq_s8(vreinterpretq_s8_s32(mask.v4s), vreinterpretq_s8_s32(v.v4s)))));
  }

  ALWAYS_INLINE GSVector4i mix16(const GSVector4i& v) const { return blend16<0xaa>(v); }

  ALWAYS_INLINE GSVector4i shuffle8(const GSVector4i& mask) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vqtbl1q_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_u8_s32(mask.v4s))));
  }

  ALWAYS_INLINE GSVector4i ps16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(
      vcombine_s8(vqmovn_s16(vreinterpretq_s16_s32(v4s)), vqmovn_s16(vreinterpretq_s16_s32(v.v4s)))));
  }

  ALWAYS_INLINE GSVector4i ps16() const
  {
    return GSVector4i(vreinterpretq_s32_s8(
      vcombine_s8(vqmovn_s16(vreinterpretq_s16_s32(v4s)), vqmovn_s16(vreinterpretq_s16_s32(v4s)))));
  }

  ALWAYS_INLINE GSVector4i pu16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u8(
      vcombine_u8(vqmovun_s16(vreinterpretq_s16_s32(v4s)), vqmovun_s16(vreinterpretq_s16_s32(v.v4s)))));
  }

  ALWAYS_INLINE GSVector4i pu16() const
  {
    return GSVector4i(vreinterpretq_s32_u8(
      vcombine_u8(vqmovun_s16(vreinterpretq_s16_s32(v4s)), vqmovun_s16(vreinterpretq_s16_s32(v4s)))));
  }

  ALWAYS_INLINE GSVector4i ps32(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vcombine_s16(vqmovn_s32(v4s), vqmovn_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i ps32() const
  {
    return GSVector4i(vreinterpretq_s32_s16(vcombine_s16(vqmovn_s32(v4s), vqmovn_s32(v4s))));
  }

  ALWAYS_INLINE GSVector4i pu32(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vcombine_u16(vqmovun_s32(v4s), vqmovun_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i pu32() const
  {
    return GSVector4i(vreinterpretq_s32_u16(vcombine_u16(vqmovun_s32(v4s), vqmovun_s32(v4s))));
  }

  ALWAYS_INLINE GSVector4i upl8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vzip1q_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i uph8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vzip2q_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i upl16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vzip1q_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i uph16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vzip2q_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i upl32(const GSVector4i& v) const { return GSVector4i(vzip1q_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i uph32(const GSVector4i& v) const { return GSVector4i(vzip2q_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i upl64(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s64(
      vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(v4s)), vget_low_s64(vreinterpretq_s64_s32(v.v4s)))));
  }

  ALWAYS_INLINE GSVector4i uph64(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s64(
      vcombine_s64(vget_high_s64(vreinterpretq_s64_s32(v4s)), vget_high_s64(vreinterpretq_s64_s32(v.v4s)))));
  }

  ALWAYS_INLINE GSVector4i upl8() const
  {
    return GSVector4i(vreinterpretq_s32_s8(vzip1q_s8(vreinterpretq_s8_s32(v4s), vdupq_n_s8(0))));
  }

  ALWAYS_INLINE GSVector4i uph8() const
  {
    return GSVector4i(vreinterpretq_s32_s8(vzip2q_s8(vreinterpretq_s8_s32(v4s), vdupq_n_s8(0))));
  }

  ALWAYS_INLINE GSVector4i upl16() const
  {
    return GSVector4i(vreinterpretq_s32_s16(vzip1q_s16(vreinterpretq_s16_s32(v4s), vdupq_n_s16(0))));
  }

  ALWAYS_INLINE GSVector4i uph16() const
  {
    return GSVector4i(vreinterpretq_s32_s16(vzip2q_s16(vreinterpretq_s16_s32(v4s), vdupq_n_s16(0))));
  }

  ALWAYS_INLINE GSVector4i upl32() const { return GSVector4i(vzip1q_s32(v4s, vdupq_n_s32(0))); }

  ALWAYS_INLINE GSVector4i uph32() const { return GSVector4i(vzip2q_s32(v4s, vdupq_n_s32(0))); }

  ALWAYS_INLINE GSVector4i upl64() const
  {
    return GSVector4i(vreinterpretq_s32_s64(vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(v4s)), vdup_n_s64(0))));
  }

  ALWAYS_INLINE GSVector4i uph64() const
  {
    return GSVector4i(vreinterpretq_s32_s64(vcombine_s64(vget_high_s64(vreinterpretq_s64_s32(v4s)), vdup_n_s64(0))));
  }

  ALWAYS_INLINE GSVector4i i8to16() const
  {
    return GSVector4i(vreinterpretq_s32_s16(vmovl_s8(vget_low_s8(vreinterpretq_s8_s32(v4s)))));
  }

  ALWAYS_INLINE GSVector4i u8to16() const
  {
    return GSVector4i(vreinterpretq_s32_u16(vmovl_u8(vget_low_u8(vreinterpretq_u8_s32(v4s)))));
  }

  ALWAYS_INLINE GSVector4i i8to32() const
  {
    return GSVector4i(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(vreinterpretq_s8_s32(v4s))))));
  }

  ALWAYS_INLINE GSVector4i u8to32() const
  {
    return GSVector4i(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vget_low_u8(vreinterpretq_u8_s32(v4s)))))));
  }

  ALWAYS_INLINE GSVector4i i8to64() const
  {
    return GSVector4i(vreinterpretq_s32_s64(
      vmovl_s32(vget_low_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(vreinterpretq_s8_s32(v4s)))))))));
  }

  ALWAYS_INLINE GSVector4i u8to64() const
  {
    return GSVector4i(vreinterpretq_s32_u64(
      vmovl_u32(vget_low_u32(vmovl_u16(vget_low_u16(vmovl_u8(vget_low_u8(vreinterpretq_u8_s32(v4s)))))))));
  }

  ALWAYS_INLINE GSVector4i i16to32() const { return GSVector4i(vmovl_s16(vget_low_s16(vreinterpretq_s16_s32(v4s)))); }

  ALWAYS_INLINE GSVector4i u16to32() const
  {
    return GSVector4i(vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vreinterpretq_u16_s32(v4s)))));
  }

  ALWAYS_INLINE GSVector4i i16to64() const
  {
    return GSVector4i(
      vreinterpretq_s32_s64(vmovl_s32(vget_low_s32(vmovl_s16(vget_low_s16(vreinterpretq_s16_s32(v4s)))))));
  }

  ALWAYS_INLINE GSVector4i u16to64() const
  {
    return GSVector4i(
      vreinterpretq_s32_u64(vmovl_u32(vget_low_u32(vmovl_u16(vget_low_u16(vreinterpretq_u16_s32(v4s)))))));
  }

  ALWAYS_INLINE GSVector4i i32to64() const { return GSVector4i(vreinterpretq_s32_s64(vmovl_s32(vget_low_s32(v4s)))); }

  ALWAYS_INLINE GSVector4i u32to64() const
  {
    return GSVector4i(vreinterpretq_s32_u64(vmovl_u32(vget_low_u32(vreinterpretq_u32_s32(v4s)))));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i srl() const
  {
    return GSVector4i(vreinterpretq_s32_s8(vextq_s8(vreinterpretq_s8_s32(v4s), vdupq_n_s8(0), i)));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i srl(const GSVector4i& v)
  {
    if constexpr (i >= 16)
      return GSVector4i(vreinterpretq_s32_u8(vextq_u8(vreinterpretq_u8_s32(v.v4s), vdupq_n_u8(0), i - 16)));
    else
      return GSVector4i(vreinterpretq_s32_u8(vextq_u8(vreinterpretq_u8_s32(v4s), vreinterpretq_u8_s32(v.v4s), i)));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i sll() const
  {
    return GSVector4i(vreinterpretq_s32_s8(vextq_s8(vdupq_n_s8(0), vreinterpretq_s8_s32(v4s), 16 - i)));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i sll16() const
  {
    return GSVector4i(vreinterpretq_s32_s16(vshlq_n_s16(vreinterpretq_s16_s32(v4s), i)));
  }

  ALWAYS_INLINE GSVector4i sll16(s32 i) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vshlq_s16(vreinterpretq_s16_s32(v4s), vdupq_n_s16(i))));
  }

  ALWAYS_INLINE GSVector4i sllv16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vshlq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i srl16() const
  {
    return GSVector4i(vreinterpretq_s32_u16(vshrq_n_u16(vreinterpretq_u16_s32(v4s), i)));
  }

  ALWAYS_INLINE GSVector4i srl16(s32 i) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vshlq_u16(vreinterpretq_u16_s32(v4s), vdupq_n_u16(-i))));
  }

  ALWAYS_INLINE GSVector4i srlv16(const GSVector4i& v) const
  {
    return GSVector4i(
      vreinterpretq_s32_s16(vshlq_s16(vreinterpretq_s16_s32(v4s), vnegq_s16(vreinterpretq_s16_s32(v.v4s)))));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i sra16() const
  {
    constexpr int count = (i & ~15) ? 15 : i;
    return GSVector4i(vreinterpretq_s32_s16(vshrq_n_s16(vreinterpretq_s16_s32(v4s), count)));
  }

  ALWAYS_INLINE GSVector4i sra16(s32 i) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vshlq_s16(vreinterpretq_s16_s32(v4s), vdupq_n_s16(-i))));
  }

  ALWAYS_INLINE GSVector4i srav16(const GSVector4i& v) const
  {
    return GSVector4i(
      vreinterpretq_s32_u16(vshlq_u16(vreinterpretq_u16_s32(v4s), vnegq_s16(vreinterpretq_s16_s32(v.v4s)))));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i sll32() const
  {
    return GSVector4i(vshlq_n_s32(v4s, i));
  }

  ALWAYS_INLINE GSVector4i sll32(s32 i) const { return GSVector4i(vshlq_s32(v4s, vdupq_n_s32(i))); }

  ALWAYS_INLINE GSVector4i sllv32(const GSVector4i& v) const { return GSVector4i(vshlq_s32(v4s, v.v4s)); }

  template<int i>
  ALWAYS_INLINE GSVector4i srl32() const
  {
    return GSVector4i(vreinterpretq_s32_u32(vshrq_n_u32(vreinterpretq_u32_s32(v4s), i)));
  }

  ALWAYS_INLINE GSVector4i srl32(s32 i) const
  {
    return GSVector4i(vreinterpretq_s32_u32(vshlq_u32(vreinterpretq_u32_s32(v4s), vdupq_n_s32(-i))));
  }

  ALWAYS_INLINE GSVector4i srlv32(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u32(vshlq_u32(vreinterpretq_u32_s32(v4s), vnegq_s32(v.v4s))));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i sra32() const
  {
    return GSVector4i(vshrq_n_s32(v4s, i));
  }

  ALWAYS_INLINE GSVector4i sra32(s32 i) const { return GSVector4i(vshlq_s32(v4s, vdupq_n_s32(-i))); }

  ALWAYS_INLINE GSVector4i srav32(const GSVector4i& v) const
  {
    return GSVector4i(vshlq_s32(vreinterpretq_u32_s32(v4s), vnegq_s32(v.v4s)));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i sll64() const
  {
    return GSVector4i(vreinterpretq_s32_s64(vshlq_n_s64(vreinterpretq_s64_s32(v4s), i)));
  }

  ALWAYS_INLINE GSVector4i sll64(s32 i) const
  {
    return GSVector4i(vreinterpretq_s32_s64(vshlq_s64(vreinterpretq_s64_s32(v4s), vdupq_n_s16(i))));
  }

  ALWAYS_INLINE GSVector4i sllv64(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s64(vshlq_s64(vreinterpretq_s64_s32(v4s), vreinterpretq_s64_s32(v.v4s))));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i sra64() const
  {
    return GSVector4i(vreinterpretq_s32_s64(vshrq_n_s64(vreinterpretq_s64_s32(v4s), i)));
  }

  ALWAYS_INLINE GSVector4i sra64(s32 i) const
  {
    return GSVector4i(vreinterpretq_s32_s64(vshlq_s64(vreinterpretq_s64_s32(v4s), vdupq_n_s16(-i))));
  }

  ALWAYS_INLINE GSVector4i srav64(const GSVector4i& v) const
  {
    return GSVector4i(
      vreinterpretq_s32_s64(vshlq_s64(vreinterpretq_s64_s32(v4s), vnegq_s64(vreinterpretq_s64_s32(v.v4s)))));
  }

  template<int i>
  ALWAYS_INLINE GSVector4i srl64() const
  {
    return GSVector4i(vreinterpretq_s32_u64(vshrq_n_u64(vreinterpretq_u64_s32(v4s), i)));
  }

  ALWAYS_INLINE GSVector4i srl64(s32 i) const
  {
    return GSVector4i(vreinterpretq_s32_u64(vshlq_u64(vreinterpretq_u64_s32(v4s), vdupq_n_u16(-i))));
  }

  ALWAYS_INLINE GSVector4i srlv64(const GSVector4i& v) const
  {
    return GSVector4i(
      vreinterpretq_s32_u64(vshlq_u64(vreinterpretq_u64_s32(v4s), vnegq_s64(vreinterpretq_s64_s32(v.v4s)))));
  }

  ALWAYS_INLINE GSVector4i add8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vaddq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i add16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vaddq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i add32(const GSVector4i& v) const { return GSVector4i(vaddq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i adds8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vqaddq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i adds16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vqaddq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i hadds16(const GSVector4i& v) const
  {
    // can't use vpaddq_s16() here, because we need saturation.
    // return GSVector4i(vreinterpretq_s32_s16(vpaddq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
    const int16x8_t a = vreinterpretq_s16_s32(v4s);
    const int16x8_t b = vreinterpretq_s16_s32(v.v4s);
    return GSVector4i(vqaddq_s16(vuzp1q_s16(a, b), vuzp2q_s16(a, b)));
  }

  ALWAYS_INLINE GSVector4i addus8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u8(vqaddq_u8(vreinterpretq_u8_s32(v4s), vreinterpretq_u8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i addus16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vqaddq_u16(vreinterpretq_u16_s32(v4s), vreinterpretq_u16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i sub8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vsubq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i sub16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vsubq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i sub32(const GSVector4i& v) const { return GSVector4i(vsubq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i subs8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vqsubq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i subs16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vqsubq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i subus8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u8(vqsubq_u8(vreinterpretq_u8_s32(v4s), vreinterpretq_u8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i subus16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vqsubq_u16(vreinterpretq_u16_s32(v4s), vreinterpretq_u16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i avg8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u8(vrhaddq_u8(vreinterpretq_u8_s32(v4s), vreinterpretq_u8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i avg16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vrhaddq_u16(vreinterpretq_u16_s32(v4s), vreinterpretq_u16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i mul16hs(const GSVector4i& v) const
  {
    // from sse2neon
    int16x4_t a3210 = vget_low_s16(vreinterpretq_s16_s32(v4s));
    int16x4_t b3210 = vget_low_s16(vreinterpretq_s16_s32(v.v4s));
    int32x4_t ab3210 = vmull_s16(a3210, b3210); /* 3333222211110000 */
    int16x4_t a7654 = vget_high_s16(vreinterpretq_s16_s32(v4s));
    int16x4_t b7654 = vget_high_s16(vreinterpretq_s16_s32(v.v4s));
    int32x4_t ab7654 = vmull_s16(a7654, b7654); /* 7777666655554444 */
    uint16x8x2_t r = vuzpq_u16(vreinterpretq_u16_s32(ab3210), vreinterpretq_u16_s32(ab7654));
    return GSVector4i(vreinterpretq_s32_u16(r.val[1]));
  }

  ALWAYS_INLINE GSVector4i mul16l(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vmulq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i mul16hrs(const GSVector4i& v) const
  {
    int32x4_t mul_lo = vmull_s16(vget_low_s16(vreinterpretq_s16_s32(v4s)), vget_low_s16(vreinterpretq_s16_s32(v.v4s)));
    int32x4_t mul_hi =
      vmull_s16(vget_high_s16(vreinterpretq_s16_s32(v4s)), vget_high_s16(vreinterpretq_s16_s32(v.v4s)));
    int16x4_t narrow_lo = vrshrn_n_s32(mul_lo, 15);
    int16x4_t narrow_hi = vrshrn_n_s32(mul_hi, 15);
    return GSVector4i(vreinterpretq_s32_s16(vcombine_s16(narrow_lo, narrow_hi)));
  }

  ALWAYS_INLINE GSVector4i mul32l(const GSVector4i& v) const { return GSVector4i(vmulq_s32(v4s, v.v4s)); }

  template<int shift>
  ALWAYS_INLINE GSVector4i lerp16(const GSVector4i& a, const GSVector4i& f) const
  {
    // (a - this) * f << shift + this

    return add16(a.sub16(*this).modulate16<shift>(f));
  }

  template<int shift>
  ALWAYS_INLINE static GSVector4i lerp16(const GSVector4i& a, const GSVector4i& b, const GSVector4i& c)
  {
    // (a - b) * c << shift

    return a.sub16(b).modulate16<shift>(c);
  }

  template<int shift>
  ALWAYS_INLINE static GSVector4i lerp16(const GSVector4i& a, const GSVector4i& b, const GSVector4i& c,
                                         const GSVector4i& d)
  {
    // (a - b) * c << shift + d

    return d.add16(a.sub16(b).modulate16<shift>(c));
  }

  ALWAYS_INLINE GSVector4i lerp16_4(const GSVector4i& a, const GSVector4i& f) const
  {
    // (a - this) * f >> 4 + this (a, this: 8-bit, f: 4-bit)

    return add16(a.sub16(*this).mul16l(f).sra16<4>());
  }

  template<int shift>
  ALWAYS_INLINE GSVector4i modulate16(const GSVector4i& f) const
  {
    // a * f << shift
    if (shift == 0)
    {
      return mul16hrs(f);
    }

    return sll16<shift + 1>().mul16hs(f);
  }

  ALWAYS_INLINE bool eq(const GSVector4i& v) const
  {
    return (vmaxvq_u32(vreinterpretq_u32_s32(veorq_s32(v4s, v.v4s))) == 0);
  }

  ALWAYS_INLINE GSVector4i eq8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u8(vceqq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i eq16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vceqq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i eq32(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u32(vceqq_s32(v4s, v.v4s)));
  }

  ALWAYS_INLINE GSVector4i eq64(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_u64(vceqq_s64(vreinterpretq_s64_s32(v4s), vreinterpretq_s64_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i neq8(const GSVector4i& v) const { return ~eq8(v); }

  ALWAYS_INLINE GSVector4i neq16(const GSVector4i& v) const { return ~eq16(v); }

  ALWAYS_INLINE GSVector4i neq32(const GSVector4i& v) const { return ~eq32(v); }

  ALWAYS_INLINE GSVector4i gt8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vcgtq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i gt16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vcgtq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i gt32(const GSVector4i& v) const { return GSVector4i(vcgtq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i ge8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vcgeq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }
  ALWAYS_INLINE GSVector4i ge16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vcgeq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }
  ALWAYS_INLINE GSVector4i ge32(const GSVector4i& v) const { return GSVector4i(vcgeq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i lt8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vcltq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i lt16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vcltq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }

  ALWAYS_INLINE GSVector4i lt32(const GSVector4i& v) const { return GSVector4i(vcltq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i le8(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s8(vcleq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s))));
  }
  ALWAYS_INLINE GSVector4i le16(const GSVector4i& v) const
  {
    return GSVector4i(vreinterpretq_s32_s16(vcleq_s16(vreinterpretq_s16_s32(v4s), vreinterpretq_s16_s32(v.v4s))));
  }
  ALWAYS_INLINE GSVector4i le32(const GSVector4i& v) const { return GSVector4i(vcleq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4i andnot(const GSVector4i& v) const { return GSVector4i(vbicq_s32(v4s, v.v4s)); }

  ALWAYS_INLINE int mask() const
  {
    // borrowed from sse2neon
    const uint16x8_t high_bits = vreinterpretq_u16_u8(vshrq_n_u8(vreinterpretq_u8_s32(v4s), 7));
    const uint32x4_t paired16 = vreinterpretq_u32_u16(vsraq_n_u16(high_bits, high_bits, 7));
    const uint64x2_t paired32 = vreinterpretq_u64_u32(vsraq_n_u32(paired16, paired16, 14));
    const uint8x16_t paired64 = vreinterpretq_u8_u64(vsraq_n_u64(paired32, paired32, 28));
    return static_cast<int>(vgetq_lane_u8(paired64, 0) | ((int)vgetq_lane_u8(paired64, 8) << 8));
  }

  ALWAYS_INLINE bool alltrue() const
  {
    // MSB should be set in all 8-bit lanes.
    return (vminvq_u8(vreinterpretq_u8_s32(v4s)) & 0x80) == 0x80;
  }

  ALWAYS_INLINE bool allfalse() const
  {
    // MSB should be clear in all 8-bit lanes.
    return (vmaxvq_u32(vreinterpretq_u8_s32(v4s)) & 0x80) != 0x80;
  }

  template<int i>
  ALWAYS_INLINE GSVector4i insert8(int a) const
  {
    return GSVector4i(vreinterpretq_s32_u8(vsetq_lane_u8(a, vreinterpretq_u8_s32(v4s), static_cast<uint8_t>(i))));
  }

  template<int i>
  ALWAYS_INLINE int extract8() const
  {
    return vgetq_lane_u8(vreinterpretq_u8_s32(v4s), i);
  }

  template<int i>
  ALWAYS_INLINE GSVector4i insert16(int a) const
  {
    return GSVector4i(vreinterpretq_s32_u16(vsetq_lane_u16(a, vreinterpretq_u16_s32(v4s), static_cast<uint16_t>(i))));
  }

  template<int i>
  ALWAYS_INLINE int extract16() const
  {
    return vgetq_lane_u16(vreinterpretq_u16_s32(v4s), i);
  }

  template<int i>
  ALWAYS_INLINE GSVector4i insert32(int a) const
  {
    return GSVector4i(vsetq_lane_s32(a, v4s, i));
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return vgetq_lane_s32(v4s, i);
  }

  template<int i>
  ALWAYS_INLINE GSVector4i insert64(s64 a) const
  {
    return GSVector4i(vreinterpretq_s32_s64(vsetq_lane_s64(a, vreinterpretq_s64_s32(v4s), i)));
  }

  template<int i>
  ALWAYS_INLINE s64 extract64() const
  {
    return vgetq_lane_s64(vreinterpretq_s64_s32(v4s), i);
  }

  ALWAYS_INLINE static GSVector4i loadnt(const void* p)
  {
#if __has_builtin(__builtin_nontemporal_store)
    return GSVector4i(__builtin_nontemporal_load((int32x4_t*)p));
#else
    return GSVector4i(vreinterpretq_s32_s64(vld1q_s64((int64_t*)p)));
#endif
  }

  ALWAYS_INLINE static GSVector4i load32(const void* p)
  {
    // should be ldr s0, [x0]
    u32 val;
    std::memcpy(&val, p, sizeof(u32));
    return GSVector4i(vsetq_lane_u32(val, vdupq_n_u32(0), 0));
  }

  ALWAYS_INLINE static GSVector4i loadl(const void* p)
  {
    return GSVector4i(vcombine_s32(vld1_s32((const int32_t*)p), vcreate_s32(0)));
  }

  ALWAYS_INLINE static GSVector4i loadh(const void* p)
  {
    return GSVector4i(vreinterpretq_s32_s64(vcombine_s64(vdup_n_s64(0), vld1_s64((int64_t*)p))));
  }

  ALWAYS_INLINE static GSVector4i loadh(const void* p, const GSVector4i& v)
  {
    return GSVector4i(
      vreinterpretq_s32_s64(vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(v.v4s)), vld1_s64((int64_t*)p))));
  }

  ALWAYS_INLINE static GSVector4i loadh(const GSVector2i& v) { return GSVector4i(vcombine_s32(vcreate_s32(0), v.v2s)); }

  ALWAYS_INLINE static GSVector4i load(const void* pl, const void* ph)
  {
    return GSVector4i(vreinterpretq_s32_s64(vcombine_s64(vld1_s64((int64_t*)pl), vld1_s64((int64_t*)ph))));
  }

  template<bool aligned>
  ALWAYS_INLINE static GSVector4i load(const void* p)
  {
    return GSVector4i(vreinterpretq_s32_s64(vld1q_s64((int64_t*)p)));
  }

  ALWAYS_INLINE static GSVector4i load(int i) { return GSVector4i(vsetq_lane_s32(i, vdupq_n_s32(0), 0)); }

  ALWAYS_INLINE static GSVector4i loadq(s64 i)
  {
    return GSVector4i(vreinterpretq_s32_s64(vsetq_lane_s64(i, vdupq_n_s64(0), 0)));
  }

  ALWAYS_INLINE static void storent(void* p, const GSVector4i& v)
  {
#if __has_builtin(__builtin_nontemporal_store)
    __builtin_nontemporal_store(v.v4s, ((int32x4_t*)p));
#else
    vst1q_s64((int64_t*)p, vreinterpretq_s64_s32(v.v4s));
#endif
  }

  ALWAYS_INLINE static void store32(void* p, const GSVector4i& v)
  {
    u32 val = vgetq_lane_s32(v, 0);
    std::memcpy(p, &val, sizeof(u32));
  }

  ALWAYS_INLINE static void storel(void* p, const GSVector4i& v)
  {
    vst1_s64((int64_t*)p, vget_low_s64(vreinterpretq_s64_s32(v.v4s)));
  }

  ALWAYS_INLINE static void storeh(void* p, const GSVector4i& v)
  {
    vst1_s64((int64_t*)p, vget_high_s64(vreinterpretq_s64_s32(v.v4s)));
  }

  ALWAYS_INLINE static void store(void* pl, void* ph, const GSVector4i& v)
  {
    GSVector4i::storel(pl, v);
    GSVector4i::storeh(ph, v);
  }

  template<bool aligned>
  ALWAYS_INLINE static void store(void* p, const GSVector4i& v)
  {
    vst1q_s64((int64_t*)p, vreinterpretq_s64_s32(v.v4s));
  }

  ALWAYS_INLINE static int store(const GSVector4i& v) { return vgetq_lane_s32(v.v4s, 0); }

  ALWAYS_INLINE static s64 storeq(const GSVector4i& v) { return vgetq_lane_s64(vreinterpretq_s64_s32(v.v4s), 0); }

  ALWAYS_INLINE void operator&=(const GSVector4i& v)
  {
    v4s = vreinterpretq_s32_s8(vandq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s)));
  }

  ALWAYS_INLINE void operator|=(const GSVector4i& v)
  {
    v4s = vreinterpretq_s32_s8(vorrq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s)));
  }

  ALWAYS_INLINE void operator^=(const GSVector4i& v)
  {
    v4s = vreinterpretq_s32_s8(veorq_s8(vreinterpretq_s8_s32(v4s), vreinterpretq_s8_s32(v.v4s)));
  }

  ALWAYS_INLINE friend GSVector4i operator&(const GSVector4i& v1, const GSVector4i& v2)
  {
    return GSVector4i(vreinterpretq_s32_s8(vandq_s8(vreinterpretq_s8_s32(v1.v4s), vreinterpretq_s8_s32(v2.v4s))));
  }

  ALWAYS_INLINE friend GSVector4i operator|(const GSVector4i& v1, const GSVector4i& v2)
  {
    return GSVector4i(vreinterpretq_s32_s8(vorrq_s8(vreinterpretq_s8_s32(v1.v4s), vreinterpretq_s8_s32(v2.v4s))));
  }

  ALWAYS_INLINE friend GSVector4i operator^(const GSVector4i& v1, const GSVector4i& v2)
  {
    return GSVector4i(vreinterpretq_s32_s8(veorq_s8(vreinterpretq_s8_s32(v1.v4s), vreinterpretq_s8_s32(v2.v4s))));
  }

  ALWAYS_INLINE friend GSVector4i operator&(const GSVector4i& v, int i) { return v & GSVector4i(i); }

  ALWAYS_INLINE friend GSVector4i operator|(const GSVector4i& v, int i) { return v | GSVector4i(i); }

  ALWAYS_INLINE friend GSVector4i operator^(const GSVector4i& v, int i) { return v ^ GSVector4i(i); }

  ALWAYS_INLINE friend GSVector4i operator~(const GSVector4i& v) { return GSVector4i(vmvnq_s32(v.v4s)); }

  ALWAYS_INLINE static GSVector4i zero() { return GSVector4i(0); }

  ALWAYS_INLINE static GSVector4i xffffffff() { return GSVector4i(0xFFFFFFFF); }

  ALWAYS_INLINE GSVector4i xyxy(const GSVector4i& v) const { return upl64(v); }

  ALWAYS_INLINE GSVector2i xy() const { return GSVector2i(vget_low_s32(v4s)); }

  ALWAYS_INLINE GSVector2i zw() const { return GSVector2i(vget_high_s32(v4s)); }

  // clang-format off


#define VECTOR4i_SHUFFLE_4(xs, xn, ys, yn, zs, zn, ws, wn) \
    ALWAYS_INLINE GSVector4i xs##ys##zs##ws() const { return GSVector4i(__builtin_shufflevector(v4s, v4s, xn, yn, zn, wn)); }

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
    float32x4_t v4s;
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
    const float arr[4] = {x, y, z, w};
    v4s = vld1q_f32(arr);
  }

  ALWAYS_INLINE GSVector4(float x, float y)
  {
    v4s = vzip1q_f32(vsetq_lane_f32(x, vdupq_n_f32(0.0f), 0), vsetq_lane_f32(y, vdupq_n_f32(0.0f), 0));
  }

  ALWAYS_INLINE GSVector4(int x, int y, int z, int w)
  {
    const int arr[4] = {x, y, z, w};
    v4s = vcvtq_f32_s32(vld1q_s32(arr));
  }

  ALWAYS_INLINE GSVector4(int x, int y)
  {
    v4s = vcvtq_f32_s32(vzip1q_s32(vsetq_lane_s32(x, vdupq_n_s32(0), 0), vsetq_lane_s32(y, vdupq_n_s32(0), 0)));
  }

  ALWAYS_INLINE explicit GSVector4(const GSVector2& v) { v4s = vcombine_f32(v.v2s, vcreate_f32(0)); }

  ALWAYS_INLINE explicit GSVector4(const GSVector2i& v) { v4s = vcombine_f32(vcvt_f32_s32(v.v2s), vcreate_f32(0)); }

  ALWAYS_INLINE constexpr explicit GSVector4(float32x4_t m) : v4s(m) {}

  ALWAYS_INLINE explicit GSVector4(float f) { v4s = vdupq_n_f32(f); }

  ALWAYS_INLINE explicit GSVector4(int i) { v4s = vcvtq_f32_s32(vdupq_n_s32(i)); }

  ALWAYS_INLINE explicit GSVector4(const GSVector4i& v);

  ALWAYS_INLINE static GSVector4 cast(const GSVector4i& v);

  ALWAYS_INLINE static GSVector4 f64(double x, double y)
  {
    return GSVector4(vreinterpretq_f32_f64(vsetq_lane_f64(y, vdupq_n_f64(x), 1)));
  }

  ALWAYS_INLINE void operator=(float f) { v4s = vdupq_n_f32(f); }

  ALWAYS_INLINE void operator=(float32x4_t m) { v4s = m; }

  ALWAYS_INLINE operator float32x4_t() const { return v4s; }

  /// Makes Clang think that the whole vector is needed, preventing it from changing shuffles around because it thinks
  /// we don't need the whole vector Useful for e.g. preventing clang from optimizing shuffles that remove
  /// possibly-denormal garbage data from vectors before computing with them
  ALWAYS_INLINE GSVector4 noopt()
  {
    // Note: Clang is currently the only compiler that attempts to optimize vector intrinsics, if that changes in the
    // future the implementation should be updated
#ifdef __clang__
    // __asm__("":"+x"(m)::);
#endif
    return *this;
  }

  ALWAYS_INLINE u32 rgba32() const { return GSVector4i(*this).rgba32(); }

  ALWAYS_INLINE static GSVector4 rgba32(u32 rgba) { return GSVector4(GSVector4i::load((int)rgba).u8to32()); }

  ALWAYS_INLINE static GSVector4 unorm8(u32 rgba) { return rgba32(rgba) * GSVector4::cxpr(1.0f / 255.0f); }

  ALWAYS_INLINE GSVector4 abs() const { return GSVector4(vabsq_f32(v4s)); }

  ALWAYS_INLINE GSVector4 neg() const { return GSVector4(vnegq_f32(v4s)); }

  ALWAYS_INLINE GSVector4 rcp() const { return GSVector4(vrecpeq_f32(v4s)); }

  ALWAYS_INLINE GSVector4 rcpnr() const
  {
    float32x4_t recip = vrecpeq_f32(v4s);
    recip = vmulq_f32(recip, vrecpsq_f32(recip, v4s));
    return GSVector4(recip);
  }

  ALWAYS_INLINE GSVector4 floor() const { return GSVector4(vrndmq_f32(v4s)); }

  ALWAYS_INLINE GSVector4 ceil() const { return GSVector4(vrndpq_f32(v4s)); }

  ALWAYS_INLINE GSVector4 madd(const GSVector4& a, const GSVector4& b) const
  {
    return GSVector4(vfmaq_f32(b.v4s, v4s, a.v4s));
  }
  ALWAYS_INLINE GSVector4 msub(const GSVector4& a, const GSVector4& b) const
  {
    return GSVector4(vfmsq_f32(b.v4s, v4s, a.v4s));
  }
  ALWAYS_INLINE GSVector4 nmadd(const GSVector4& a, const GSVector4& b) const { return b - *this * a; }
  ALWAYS_INLINE GSVector4 nmsub(const GSVector4& a, const GSVector4& b) const { return -b - *this * a; }

  ALWAYS_INLINE GSVector4 addm(const GSVector4& a, const GSVector4& b) const
  {
    return a.madd(b, *this); // *this + a * b
  }

  ALWAYS_INLINE GSVector4 subm(const GSVector4& a, const GSVector4& b) const
  {
    return a.nmadd(b, *this); // *this - a * b
  }

  ALWAYS_INLINE GSVector4 hadd() const { return GSVector4(vpaddq_f32(v4s, v4s)); }

  ALWAYS_INLINE GSVector4 hadd(const GSVector4& v) const { return GSVector4(vpaddq_f32(v4s, v.v4s)); }

  ALWAYS_INLINE GSVector4 hsub() const { return GSVector4(vsubq_f32(vuzp1q_f32(v4s, v4s), vuzp2q_f32(v4s, v4s))); }

  ALWAYS_INLINE GSVector4 hsub(const GSVector4& v) const
  {
    return GSVector4(vsubq_f32(vuzp1q_f32(v4s, v.v4s), vuzp2q_f32(v4s, v.v4s)));
  }

  ALWAYS_INLINE GSVector4 sat(const GSVector4& a, const GSVector4& b) const { return max(a).min(b); }

  ALWAYS_INLINE GSVector4 sat(const GSVector4& a) const
  {
    const GSVector4 minv(vreinterpretq_f32_f64(vdupq_laneq_f64(vreinterpretq_f64_f32(a.v4s), 0)));
    const GSVector4 maxv(vreinterpretq_f32_f64(vdupq_laneq_f64(vreinterpretq_f64_f32(a.v4s), 1)));
    return sat(minv, maxv);
  }

  ALWAYS_INLINE GSVector4 sat(const float scale = 255) const { return sat(zero(), GSVector4(scale)); }

  ALWAYS_INLINE GSVector4 clamp(const float scale = 255) const { return min(GSVector4(scale)); }

  ALWAYS_INLINE GSVector4 min(const GSVector4& a) const { return GSVector4(vminq_f32(v4s, a.v4s)); }

  ALWAYS_INLINE GSVector4 max(const GSVector4& a) const { return GSVector4(vmaxq_f32(v4s, a.v4s)); }

  template<int mask>
  ALWAYS_INLINE GSVector4 blend32(const GSVector4& a) const
  {
    return GSVector4(__builtin_shufflevector(v4s, a.v4s, (mask & 1) ? 4 : 0, (mask & 2) ? 5 : 1, (mask & 4) ? 6 : 2,
                                             (mask & 8) ? 7 : 3));
  }

  ALWAYS_INLINE GSVector4 blend32(const GSVector4& a, const GSVector4& mask) const
  {
    // duplicate sign bit across and bit select
    const uint32x4_t bitmask = vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_f32(mask.v4s), 31));
    return GSVector4(vbslq_f32(bitmask, a.v4s, v4s));
  }

  ALWAYS_INLINE GSVector4 upl(const GSVector4& a) const { return GSVector4(vzip1q_f32(v4s, a.v4s)); }

  ALWAYS_INLINE GSVector4 uph(const GSVector4& a) const { return GSVector4(vzip2q_f32(v4s, a.v4s)); }

  ALWAYS_INLINE GSVector4 upld(const GSVector4& a) const
  {
    return GSVector4(vreinterpretq_f32_f64(vzip1q_f64(vreinterpretq_f64_f32(v4s), vreinterpretq_f64_f32(a.v4s))));
  }

  ALWAYS_INLINE GSVector4 uphd(const GSVector4& a) const
  {
    return GSVector4(vreinterpretq_f32_f64(vzip2q_f64(vreinterpretq_f64_f32(v4s), vreinterpretq_f64_f32(a.v4s))));
  }

  ALWAYS_INLINE GSVector4 l2h(const GSVector4& a) const
  {
    return GSVector4(vcombine_f32(vget_low_f32(v4s), vget_low_f32(a.v4s)));
  }

  ALWAYS_INLINE GSVector4 h2l(const GSVector4& a) const
  {
    return GSVector4(vcombine_f32(vget_high_f32(v4s), vget_high_f32(a.v4s)));
  }

  ALWAYS_INLINE GSVector4 andnot(const GSVector4& v) const
  {
    return GSVector4(vreinterpretq_f32_s32(vbicq_s32(vreinterpretq_s32_f32(v4s), vreinterpretq_s32_f32(v.v4s))));
  }

  ALWAYS_INLINE int mask() const
  {
    static constexpr const int32_t shifts[] = {0, 1, 2, 3};
    return static_cast<int>(vaddvq_u32(vshlq_u32(vshrq_n_u32(vreinterpretq_u32_f32(v4s), 31), vld1q_s32(shifts))));
  }

  ALWAYS_INLINE bool alltrue() const
  {
    // return mask() == 0xf;
    return ~(vgetq_lane_u64(vreinterpretq_u64_f32(v4s), 0) & vgetq_lane_u64(vreinterpretq_u64_f32(v4s), 1)) == 0;
  }

  ALWAYS_INLINE bool allfalse() const
  {
    return (vgetq_lane_u64(vreinterpretq_u64_f32(v4s), 0) | vgetq_lane_u64(vreinterpretq_u64_f32(v4s), 1)) == 0;
  }

  ALWAYS_INLINE GSVector4 replace_nan(const GSVector4& v) const { return v.blend32(*this, *this == *this); }

  template<int src, int dst>
  ALWAYS_INLINE GSVector4 insert32(const GSVector4& v) const
  {
    return GSVector4(vcopyq_laneq_f32(v4s, dst, v.v4s, src));
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return vgetq_lane_s32(vreinterpretq_s32_f32(v4s), i);
  }

  ALWAYS_INLINE static GSVector4 zero() { return GSVector4(vdupq_n_f32(0.0f)); }

  ALWAYS_INLINE static GSVector4 xffffffff() { return GSVector4(vreinterpretq_f32_u32(vdupq_n_u32(0xFFFFFFFFu))); }

  ALWAYS_INLINE static GSVector4 loadl(const void* p)
  {
    return GSVector4(vcombine_f32(vld1_f32((const float*)p), vcreate_f32(0)));
  }

  ALWAYS_INLINE static GSVector4 load(float f) { return GSVector4(vsetq_lane_f32(f, vmovq_n_f32(0.0f), 0)); }

  template<bool aligned>
  ALWAYS_INLINE static GSVector4 load(const void* p)
  {
    return GSVector4(vld1q_f32((const float*)p));
  }

  ALWAYS_INLINE static void storent(void* p, const GSVector4& v) { vst1q_f32((float*)p, v.v4s); }

  ALWAYS_INLINE static void storel(void* p, const GSVector4& v)
  {
    vst1_f64((double*)p, vget_low_f64(vreinterpretq_f64_f32(v.v4s)));
  }

  ALWAYS_INLINE static void storeh(void* p, const GSVector4& v)
  {
    vst1_f64((double*)p, vget_high_f64(vreinterpretq_f64_f32(v.v4s)));
  }

  template<bool aligned>
  ALWAYS_INLINE static void store(void* p, const GSVector4& v)
  {
    vst1q_f32((float*)p, v.v4s);
  }

  ALWAYS_INLINE static void store(float* p, const GSVector4& v) { vst1q_lane_f32(p, v.v4s, 0); }

  ALWAYS_INLINE GSVector4 operator-() const { return neg(); }

  ALWAYS_INLINE void operator+=(const GSVector4& v) { v4s = vaddq_f32(v4s, v.v4s); }
  ALWAYS_INLINE void operator-=(const GSVector4& v) { v4s = vsubq_f32(v4s, v.v4s); }
  ALWAYS_INLINE void operator*=(const GSVector4& v) { v4s = vmulq_f32(v4s, v.v4s); }
  ALWAYS_INLINE void operator/=(const GSVector4& v) { v4s = vdivq_f32(v4s, v.v4s); }

  ALWAYS_INLINE void operator+=(float f) { *this += GSVector4(f); }
  ALWAYS_INLINE void operator-=(float f) { *this -= GSVector4(f); }
  ALWAYS_INLINE void operator*=(float f) { *this *= GSVector4(f); }
  ALWAYS_INLINE void operator/=(float f) { *this /= GSVector4(f); }

  ALWAYS_INLINE void operator&=(const GSVector4& v)
  {
    v4s = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(v4s), vreinterpretq_u32_f32(v.v4s)));
  }

  ALWAYS_INLINE void operator|=(const GSVector4& v)
  {
    v4s = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(v4s), vreinterpretq_u32_f32(v.v4s)));
  }

  ALWAYS_INLINE void operator^=(const GSVector4& v)
  {
    v4s = vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(v4s), vreinterpretq_u32_f32(v.v4s)));
  }

  ALWAYS_INLINE friend GSVector4 operator+(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vaddq_f32(v1.v4s, v2.v4s));
  }

  ALWAYS_INLINE friend GSVector4 operator-(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vsubq_f32(v1.v4s, v2.v4s));
  }

  ALWAYS_INLINE friend GSVector4 operator*(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vmulq_f32(v1.v4s, v2.v4s));
  }

  ALWAYS_INLINE friend GSVector4 operator/(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vdivq_f32(v1.v4s, v2.v4s));
  }

  ALWAYS_INLINE friend GSVector4 operator+(const GSVector4& v, float f) { return v + GSVector4(f); }
  ALWAYS_INLINE friend GSVector4 operator-(const GSVector4& v, float f) { return v - GSVector4(f); }
  ALWAYS_INLINE friend GSVector4 operator*(const GSVector4& v, float f) { return v * GSVector4(f); }
  ALWAYS_INLINE friend GSVector4 operator/(const GSVector4& v, float f) { return v / GSVector4(f); }

  ALWAYS_INLINE friend GSVector4 operator&(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(v1.v4s), vreinterpretq_u32_f32(v2.v4s))));
  }

  ALWAYS_INLINE friend GSVector4 operator|(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(v1.v4s), vreinterpretq_u32_f32(v2.v4s))));
  }

  ALWAYS_INLINE friend GSVector4 operator^(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(v1.v4s), vreinterpretq_u32_f32(v2.v4s))));
  }

  ALWAYS_INLINE friend GSVector4 operator==(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(vceqq_f32(v1.v4s, v2.v4s)));
  }

  ALWAYS_INLINE friend GSVector4 operator!=(const GSVector4& v1, const GSVector4& v2)
  {
    // NEON has no !=
    return GSVector4(vreinterpretq_f32_u32(vmvnq_u32(vceqq_f32(v1.v4s, v2.v4s))));
  }

  ALWAYS_INLINE friend GSVector4 operator>(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(vcgtq_f32(v1.v4s, v2.v4s)));
  }

  ALWAYS_INLINE friend GSVector4 operator<(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(vcltq_f32(v1.v4s, v2.v4s)));
  }

  ALWAYS_INLINE friend GSVector4 operator>=(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(vcgeq_f32(v1.v4s, v2.v4s)));
  }

  ALWAYS_INLINE friend GSVector4 operator<=(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(vreinterpretq_f32_u32(vcleq_f32(v1.v4s, v2.v4s)));
  }

  ALWAYS_INLINE GSVector4 mul64(const GSVector4& v) const
  {
    return GSVector4(vmulq_f64(vreinterpretq_f64_f32(v4s), vreinterpretq_f64_f32(v.v4s)));
  }

  ALWAYS_INLINE GSVector4 add64(const GSVector4& v) const
  {
    return GSVector4(vaddq_f64(vreinterpretq_f64_f32(v4s), vreinterpretq_f64_f32(v.v4s)));
  }

  ALWAYS_INLINE GSVector4 sub64(const GSVector4& v) const
  {
    return GSVector4(vsubq_f64(vreinterpretq_f64_f32(v4s), vreinterpretq_f64_f32(v.v4s)));
  }

  ALWAYS_INLINE static GSVector4 f32to64(const GSVector4& v)
  {
    return GSVector4(vreinterpretq_f32_f64(vcvt_f64_f32(vget_low_f32(v.v4s))));
  }

  ALWAYS_INLINE static GSVector4 f32to64(const void* p)
  {
    return GSVector4(vreinterpretq_f32_f64(vcvt_f64_f32(vld1_f32(static_cast<const float*>(p)))));
  }

  ALWAYS_INLINE GSVector4i f64toi32(bool truncate = true) const
  {
    const float64x2_t r = truncate ? v4s : vrndiq_f64(vreinterpretq_f64_f32(v4s));
    const s32 low = static_cast<s32>(vgetq_lane_f64(r, 0));
    const s32 high = static_cast<s32>(vgetq_lane_f64(r, 1));
    return GSVector4i(vsetq_lane_s32(high, vsetq_lane_s32(low, vdupq_n_s32(0), 0), 1));
  }

  // clang-format off

#define VECTOR4_SHUFFLE_4(xs, xn, ys, yn, zs, zn, ws, wn) \
    ALWAYS_INLINE GSVector4 xs##ys##zs##ws() const { return GSVector4(__builtin_shufflevector(v4s, v4s, xn, yn, zn, wn)); } \
    ALWAYS_INLINE GSVector4 xs##ys##zs##ws(const GSVector4& v) const { return GSVector4(__builtin_shufflevector(v4s, v.v4s, xn, yn, 4 + zn, 4 + wn)); }

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

  ALWAYS_INLINE GSVector4 broadcast32() const { return GSVector4(vdupq_laneq_f32(v4s, 0)); }

  ALWAYS_INLINE static GSVector4 broadcast32(const GSVector4& v) { return GSVector4(vdupq_laneq_f32(v.v4s, 0)); }

  ALWAYS_INLINE static GSVector4 broadcast32(const void* f) { return GSVector4(vld1q_dup_f32((const float*)f)); }

  ALWAYS_INLINE static GSVector4 broadcast64(const void* f)
  {
    return GSVector4(vreinterpretq_f64_f32(vld1q_dup_f64((const double*)f)));
  }
};

ALWAYS_INLINE GSVector2i::GSVector2i(const GSVector2& v, bool truncate)
{
  v2s = truncate ? vcvt_s32_f32(v.v2s) : vcvtn_u32_f32(v.v2s);
}

ALWAYS_INLINE GSVector2::GSVector2(const GSVector2i& v)
{
  v2s = vcvt_f32_s32(v.v2s);
}

ALWAYS_INLINE GSVector2i GSVector2i::cast(const GSVector2& v)
{
  return GSVector2i(vreinterpret_s32_f32(v.v2s));
}

ALWAYS_INLINE GSVector2 GSVector2::cast(const GSVector2i& v)
{
  return GSVector2(vreinterpret_f32_s32(v.v2s));
}

ALWAYS_INLINE GSVector4i::GSVector4i(const GSVector4& v, bool truncate)
{
  v4s = truncate ? vcvtq_s32_f32(v.v4s) : vcvtnq_u32_f32(v.v4s);
}

ALWAYS_INLINE GSVector4::GSVector4(const GSVector4i& v)
{
  v4s = vcvtq_f32_s32(v.v4s);
}

ALWAYS_INLINE GSVector4i GSVector4i::cast(const GSVector4& v)
{
  return GSVector4i(vreinterpretq_s32_f32(v.v4s));
}

ALWAYS_INLINE GSVector4 GSVector4::cast(const GSVector4i& v)
{
  return GSVector4(vreinterpretq_f32_s32(v.v4s));
}
