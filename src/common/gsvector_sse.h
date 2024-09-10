// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team, 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: PolyForm-Strict-1.0.0
//
// Lightweight wrapper over native SIMD types for cross-platform vector code.
// Rewritten and NEON+No-SIMD variants added for DuckStation.
//

#pragma once

#include "common/intrin.h"
#include "common/types.h"

#include <algorithm>

#ifdef CPU_ARCH_AVX2
#define GSVECTOR_HAS_UNSIGNED 1
#define GSVECTOR_HAS_SRLV 1
#endif

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

  constexpr GSVector2i(cxpr_init_tag, s32 x, s32 y) : S32{x, y, 0, 0} {}

  constexpr GSVector2i(cxpr_init_tag, s16 s0, s16 s1, s16 s2, s16 s3) : S16{s0, s1, s2, s3, 0, 0, 0, 0} {}

  constexpr GSVector2i(cxpr_init_tag, s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7)
    : S8{b0, b1, b2, b3, b4, b5, b6, b7, 0, 0, 0, 0, 0, 0, 0, 0}
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
    float F32[4];
    s8 S8[16];
    s16 S16[8];
    s32 S32[4];
    s64 S64[2];
    u8 U8[16];
    u16 U16[8];
    u32 U32[4];
    u64 U64[2];
    __m128i m;
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

  ALWAYS_INLINE GSVector2i(s32 x, s32 y) { m = _mm_set_epi32(0, 0, y, x); }
  ALWAYS_INLINE GSVector2i(s16 s0, s16 s1, s16 s2, s16 s3) { m = _mm_set_epi16(0, 0, 0, 0, s3, s2, s1, s0); }
  ALWAYS_INLINE constexpr GSVector2i(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7)
    : S8{b0, b1, b2, b3, b4, b5, b6, b7, 0, 0, 0, 0, 0, 0, 0, 0}
  {
  }
  ALWAYS_INLINE explicit GSVector2i(s32 i) { *this = i; }
  ALWAYS_INLINE explicit GSVector2i(const GSVector2& v);
  ALWAYS_INLINE constexpr explicit GSVector2i(__m128i m) : m(m) {}

  ALWAYS_INLINE GSVector2i& operator=(s32 i)
  {
    m = _mm_set1_epi32(i);
    return *this;
  }

  ALWAYS_INLINE GSVector2i& operator=(__m128i m_)
  {
    m = m_;
    return *this;
  }

  ALWAYS_INLINE operator __m128i() const { return m; }

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

  ALWAYS_INLINE GSVector2i min_i8(const GSVector2i& v) const { return GSVector2i(_mm_min_epi8(m, v)); }
  ALWAYS_INLINE GSVector2i max_i8(const GSVector2i& v) const { return GSVector2i(_mm_max_epi8(m, v)); }
  ALWAYS_INLINE GSVector2i min_i16(const GSVector2i& v) const { return GSVector2i(_mm_min_epi16(m, v)); }
  ALWAYS_INLINE GSVector2i max_i16(const GSVector2i& v) const { return GSVector2i(_mm_max_epi16(m, v)); }
  ALWAYS_INLINE GSVector2i min_i32(const GSVector2i& v) const { return GSVector2i(_mm_min_epi32(m, v)); }
  ALWAYS_INLINE GSVector2i max_i32(const GSVector2i& v) const { return GSVector2i(_mm_max_epi32(m, v)); }

  ALWAYS_INLINE GSVector2i min_u8(const GSVector2i& v) const { return GSVector2i(_mm_min_epu8(m, v)); }
  ALWAYS_INLINE GSVector2i max_u8(const GSVector2i& v) const { return GSVector2i(_mm_max_epu8(m, v)); }
  ALWAYS_INLINE GSVector2i min_u16(const GSVector2i& v) const { return GSVector2i(_mm_min_epu16(m, v)); }
  ALWAYS_INLINE GSVector2i max_u16(const GSVector2i& v) const { return GSVector2i(_mm_max_epu16(m, v)); }
  ALWAYS_INLINE GSVector2i min_u32(const GSVector2i& v) const { return GSVector2i(_mm_min_epu32(m, v)); }
  ALWAYS_INLINE GSVector2i max_u32(const GSVector2i& v) const { return GSVector2i(_mm_max_epu32(m, v)); }

  ALWAYS_INLINE s32 addv_s32() const { return _mm_cvtsi128_si32(_mm_hadd_epi32(m, m)); }

  ALWAYS_INLINE u8 minv_u8() const
  {
    __m128i vmin = _mm_min_epu8(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u8>(std::min(
      static_cast<u32>(_mm_extract_epi8(vmin, 0)),
      std::min(static_cast<u32>(_mm_extract_epi8(vmin, 1)),
               std::min(static_cast<u32>(_mm_extract_epi8(vmin, 2)), static_cast<u32>(_mm_extract_epi8(vmin, 3))))));
  }

  ALWAYS_INLINE u16 maxv_u8() const
  {
    __m128i vmax = _mm_max_epu8(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u8>(std::max(
      static_cast<u32>(_mm_extract_epi8(vmax, 0)),
      std::max(static_cast<u32>(_mm_extract_epi8(vmax, 1)),
               std::max(static_cast<u32>(_mm_extract_epi8(vmax, 2)), static_cast<u32>(_mm_extract_epi8(vmax, 3))))));
  }

  ALWAYS_INLINE u16 minv_u16() const
  {
    __m128i vmin = _mm_min_epu16(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u16>(
      std::min(static_cast<u32>(_mm_extract_epi16(vmin, 0)), static_cast<u32>(_mm_extract_epi16(vmin, 1))));
  }

  ALWAYS_INLINE u16 maxv_u16() const
  {
    __m128i vmax = _mm_max_epu16(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u16>(
      std::max<u32>(static_cast<u32>(_mm_extract_epi16(vmax, 0)), static_cast<u32>(_mm_extract_epi16(vmax, 1))));
  }

  ALWAYS_INLINE s32 minv_s32() const { return std::min<s32>(_mm_extract_epi32(m, 0), _mm_extract_epi32(m, 1)); }
  ALWAYS_INLINE u32 minv_u32() const { return std::min<u32>(_mm_extract_epi32(m, 0), _mm_extract_epi32(m, 1)); }
  ALWAYS_INLINE s32 maxv_s32() const { return std::max<s32>(_mm_extract_epi32(m, 0), _mm_extract_epi32(m, 1)); }
  ALWAYS_INLINE u32 maxv_u32() const { return std::max<u32>(_mm_extract_epi32(m, 0), _mm_extract_epi32(m, 1)); }

  ALWAYS_INLINE GSVector2i clamp8() const { return pu16().upl8(); }

  ALWAYS_INLINE GSVector2i blend8(const GSVector2i& v, const GSVector2i& mask) const
  {
    return GSVector2i(_mm_blendv_epi8(m, v, mask));
  }

  template<s32 mask>
  ALWAYS_INLINE GSVector2i blend16(const GSVector2i& v) const
  {
    return GSVector2i(_mm_blend_epi16(m, v, mask));
  }

  template<s32 mask>
  ALWAYS_INLINE GSVector2i blend32(const GSVector2i& v) const
  {
#if defined(__AVX2__)
    return GSVector2i(_mm_blend_epi32(m, v.m, mask));
#else
    constexpr s32 bit1 = ((mask & 2) * 3) << 1;
    constexpr s32 bit0 = (mask & 1) * 3;
    return blend16<bit1 | bit0>(v);
#endif
  }

  ALWAYS_INLINE GSVector2i blend(const GSVector2i& v, const GSVector2i& mask) const
  {
    return GSVector2i(_mm_or_si128(_mm_andnot_si128(mask, m), _mm_and_si128(mask, v)));
  }

  ALWAYS_INLINE GSVector2i mix16(const GSVector2i& v) const { return blend16<0xa>(v); }

  ALWAYS_INLINE GSVector2i shuffle8(const GSVector2i& mask) const { return GSVector2i(_mm_shuffle_epi8(m, mask)); }

  ALWAYS_INLINE GSVector2i ps16() const { return GSVector2i(_mm_packs_epi16(m, m)); }
  ALWAYS_INLINE GSVector2i pu16() const { return GSVector2i(_mm_packus_epi16(m, m)); }
  ALWAYS_INLINE GSVector2i ps32() const { return GSVector2i(_mm_packs_epi32(m, m)); }
  ALWAYS_INLINE GSVector2i pu32() const { return GSVector2i(_mm_packus_epi32(m, m)); }

  ALWAYS_INLINE GSVector2i upl8(const GSVector2i& v) const { return GSVector2i(_mm_unpacklo_epi8(m, v)); }
  ALWAYS_INLINE GSVector2i uph8(const GSVector2i& v) const { return GSVector2i(_mm_unpackhi_epi8(m, v)); }
  ALWAYS_INLINE GSVector2i upl16(const GSVector2i& v) const { return GSVector2i(_mm_unpacklo_epi16(m, v)); }
  ALWAYS_INLINE GSVector2i uph16(const GSVector2i& v) const { return GSVector2i(_mm_unpackhi_epi16(m, v)); }
  ALWAYS_INLINE GSVector2i upl32(const GSVector2i& v) const { return GSVector2i(_mm_unpacklo_epi32(m, v)); }
  ALWAYS_INLINE GSVector2i uph32(const GSVector2i& v) const { return GSVector2i(_mm_unpackhi_epi32(m, v)); }

  ALWAYS_INLINE GSVector2i upl8() const { return GSVector2i(_mm_unpacklo_epi8(m, _mm_setzero_si128())); }
  ALWAYS_INLINE GSVector2i uph8() const { return GSVector2i(_mm_unpackhi_epi8(m, _mm_setzero_si128())); }

  ALWAYS_INLINE GSVector2i upl16() const { return GSVector2i(_mm_unpacklo_epi16(m, _mm_setzero_si128())); }
  ALWAYS_INLINE GSVector2i uph16() const { return GSVector2i(_mm_unpackhi_epi16(m, _mm_setzero_si128())); }

  ALWAYS_INLINE GSVector2i upl32() const { return GSVector2i(_mm_unpacklo_epi32(m, _mm_setzero_si128())); }
  ALWAYS_INLINE GSVector2i uph32() const { return GSVector2i(_mm_unpackhi_epi32(m, _mm_setzero_si128())); }

  ALWAYS_INLINE GSVector2i i8to16() const { return GSVector2i(_mm_cvtepi8_epi16(m)); }

#ifdef CPU_ARCH_SSE41
  ALWAYS_INLINE GSVector2i u8to16() const { return GSVector2i(_mm_cvtepu8_epi16(m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector2i srl() const
  {
    return GSVector2i(_mm_srli_si128(m, i));
  }

  template<s32 i>
  ALWAYS_INLINE GSVector2i sll() const
  {
    return GSVector2i(_mm_slli_si128(m, i));
  }

  template<s32 i>
  ALWAYS_INLINE GSVector2i sll16() const
  {
    return GSVector2i(_mm_slli_epi16(m, i));
  }

  ALWAYS_INLINE GSVector2i sll16(s32 i) const { return GSVector2i(_mm_sll_epi16(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector2i sllv16(const GSVector2i& v) const { return GSVector2i(_mm_sllv_epi16(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector2i srl16() const
  {
    return GSVector2i(_mm_srli_epi16(m, i));
  }

  ALWAYS_INLINE GSVector2i srl16(s32 i) const { return GSVector2i(_mm_srl_epi16(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector2i srlv16(const GSVector2i& v) const { return GSVector2i(_mm_srlv_epi16(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector2i sra16() const
  {
    return GSVector2i(_mm_srai_epi16(m, i));
  }

  ALWAYS_INLINE GSVector2i sra16(s32 i) const { return GSVector2i(_mm_sra_epi16(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector2i srav16(const GSVector2i& v) const { return GSVector2i(_mm_srav_epi16(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector2i sll32() const
  {
    return GSVector2i(_mm_slli_epi32(m, i));
  }

  ALWAYS_INLINE GSVector2i sll32(s32 i) const { return GSVector2i(_mm_sll_epi32(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector2i sllv32(const GSVector2i& v) const { return GSVector2i(_mm_sllv_epi32(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector2i srl32() const
  {
    return GSVector2i(_mm_srli_epi32(m, i));
  }

  ALWAYS_INLINE GSVector2i srl32(s32 i) const { return GSVector2i(_mm_srl_epi32(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector2i srlv32(const GSVector2i& v) const { return GSVector2i(_mm_srlv_epi32(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector2i sra32() const
  {
    return GSVector2i(_mm_srai_epi32(m, i));
  }

  ALWAYS_INLINE GSVector2i sra32(s32 i) const { return GSVector2i(_mm_sra_epi32(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector2i srav32(const GSVector2i& v) const { return GSVector2i(_mm_srav_epi32(m, v.m)); }
#endif

  ALWAYS_INLINE GSVector2i add8(const GSVector2i& v) const { return GSVector2i(_mm_add_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i add16(const GSVector2i& v) const { return GSVector2i(_mm_add_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i add32(const GSVector2i& v) const { return GSVector2i(_mm_add_epi32(m, v.m)); }
  ALWAYS_INLINE GSVector2i adds8(const GSVector2i& v) const { return GSVector2i(_mm_adds_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i adds16(const GSVector2i& v) const { return GSVector2i(_mm_adds_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i addus8(const GSVector2i& v) const { return GSVector2i(_mm_adds_epu8(m, v.m)); }
  ALWAYS_INLINE GSVector2i addus16(const GSVector2i& v) const { return GSVector2i(_mm_adds_epu16(m, v.m)); }

  ALWAYS_INLINE GSVector2i sub8(const GSVector2i& v) const { return GSVector2i(_mm_sub_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i sub16(const GSVector2i& v) const { return GSVector2i(_mm_sub_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i sub32(const GSVector2i& v) const { return GSVector2i(_mm_sub_epi32(m, v.m)); }
  ALWAYS_INLINE GSVector2i subs8(const GSVector2i& v) const { return GSVector2i(_mm_subs_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i subs16(const GSVector2i& v) const { return GSVector2i(_mm_subs_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i subus8(const GSVector2i& v) const { return GSVector2i(_mm_subs_epu8(m, v.m)); }
  ALWAYS_INLINE GSVector2i subus16(const GSVector2i& v) const { return GSVector2i(_mm_subs_epu16(m, v.m)); }

  ALWAYS_INLINE GSVector2i avg8(const GSVector2i& v) const { return GSVector2i(_mm_avg_epu8(m, v.m)); }
  ALWAYS_INLINE GSVector2i avg16(const GSVector2i& v) const { return GSVector2i(_mm_avg_epu16(m, v.m)); }

  ALWAYS_INLINE GSVector2i mul16l(const GSVector2i& v) const { return GSVector2i(_mm_mullo_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i mul32l(const GSVector2i& v) const { return GSVector2i(_mm_mullo_epi32(m, v.m)); }

  ALWAYS_INLINE bool eq(const GSVector2i& v) const { return eq8(v).alltrue(); }

  ALWAYS_INLINE GSVector2i eq8(const GSVector2i& v) const { return GSVector2i(_mm_cmpeq_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i eq16(const GSVector2i& v) const { return GSVector2i(_mm_cmpeq_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i eq32(const GSVector2i& v) const { return GSVector2i(_mm_cmpeq_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector2i neq8(const GSVector2i& v) const { return ~eq8(v); }
  ALWAYS_INLINE GSVector2i neq16(const GSVector2i& v) const { return ~eq16(v); }
  ALWAYS_INLINE GSVector2i neq32(const GSVector2i& v) const { return ~eq32(v); }

  ALWAYS_INLINE GSVector2i gt8(const GSVector2i& v) const { return GSVector2i(_mm_cmpgt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i gt16(const GSVector2i& v) const { return GSVector2i(_mm_cmpgt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i gt32(const GSVector2i& v) const { return GSVector2i(_mm_cmpgt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector2i ge8(const GSVector2i& v) const { return ~GSVector2i(_mm_cmplt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i ge16(const GSVector2i& v) const { return ~GSVector2i(_mm_cmplt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i ge32(const GSVector2i& v) const { return ~GSVector2i(_mm_cmplt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector2i lt8(const GSVector2i& v) const { return GSVector2i(_mm_cmplt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i lt16(const GSVector2i& v) const { return GSVector2i(_mm_cmplt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i lt32(const GSVector2i& v) const { return GSVector2i(_mm_cmplt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector2i le8(const GSVector2i& v) const { return ~GSVector2i(_mm_cmpgt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector2i le16(const GSVector2i& v) const { return ~GSVector2i(_mm_cmpgt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector2i le32(const GSVector2i& v) const { return ~GSVector2i(_mm_cmpgt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector2i andnot(const GSVector2i& v) const { return GSVector2i(_mm_andnot_si128(v.m, m)); }

  ALWAYS_INLINE s32 mask() const { return (_mm_movemask_epi8(m) & 0xff); }

  ALWAYS_INLINE bool alltrue() const { return (mask() == 0xff); }
  ALWAYS_INLINE bool allfalse() const { return (mask() == 0x00); }

  template<s32 i>
  ALWAYS_INLINE GSVector2i insert8(s32 a) const
  {
    return GSVector2i(_mm_insert_epi8(m, a, i));
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract8() const
  {
    return _mm_extract_epi8(m, i);
  }

  template<s32 i>
  ALWAYS_INLINE GSVector2i insert16(s32 a) const
  {
    return GSVector2i(_mm_insert_epi16(m, a, i));
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract16() const
  {
    return _mm_extract_epi16(m, i);
  }

  template<s32 i>
  ALWAYS_INLINE GSVector2i insert32(s32 a) const
  {
    return GSVector2i(_mm_insert_epi32(m, a, i));
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract32() const
  {
    if constexpr (i == 0)
      return GSVector2i::store(*this);

    return _mm_extract_epi32(m, i);
  }

  ALWAYS_INLINE static GSVector2i load32(const void* p) { return GSVector2i(_mm_loadu_si32(p)); }
  ALWAYS_INLINE static GSVector2i load(const void* p)
  {
    return GSVector2i(_mm_loadl_epi64(static_cast<const __m128i*>(p)));
  }
  ALWAYS_INLINE static GSVector2i load(s32 i) { return GSVector2i(_mm_cvtsi32_si128(i)); }
  ALWAYS_INLINE static GSVector2i loadq(s64 i) { return GSVector2i(_mm_cvtsi64_si128(i)); }

  ALWAYS_INLINE static void store(void* p, const GSVector2i& v) { _mm_storel_epi64(static_cast<__m128i*>(p), v.m); }
  ALWAYS_INLINE static void store32(void* p, const GSVector2i& v) { _mm_storeu_si32(p, v); }
  ALWAYS_INLINE static s32 store(const GSVector2i& v) { return _mm_cvtsi128_si32(v.m); }
  ALWAYS_INLINE static s64 storeq(const GSVector2i& v) { return _mm_cvtsi128_si64(v.m); }

  ALWAYS_INLINE GSVector2i& operator&=(const GSVector2i& v)
  {
    m = _mm_and_si128(m, v);
    return *this;
  }

  ALWAYS_INLINE GSVector2i& operator|=(const GSVector2i& v)
  {
    m = _mm_or_si128(m, v);
    return *this;
  }

  ALWAYS_INLINE GSVector2i& operator^=(const GSVector2i& v)
  {
    m = _mm_xor_si128(m, v);
    return *this;
  }

  ALWAYS_INLINE friend GSVector2i operator&(const GSVector2i& v1, const GSVector2i& v2)
  {
    return GSVector2i(_mm_and_si128(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2i operator|(const GSVector2i& v1, const GSVector2i& v2)
  {
    return GSVector2i(_mm_or_si128(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2i operator^(const GSVector2i& v1, const GSVector2i& v2)
  {
    return GSVector2i(_mm_xor_si128(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2i operator&(const GSVector2i& v, s32 i) { return v & GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator|(const GSVector2i& v, s32 i) { return v | GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator^(const GSVector2i& v, s32 i) { return v ^ GSVector2i(i); }

  ALWAYS_INLINE friend GSVector2i operator~(const GSVector2i& v) { return v ^ v.eq32(v); }

  ALWAYS_INLINE static GSVector2i zero() { return GSVector2i(_mm_setzero_si128()); }
  ALWAYS_INLINE static GSVector2i cast(const GSVector2& v);

  ALWAYS_INLINE GSVector2i xy() const { return GSVector2i(m); }
  ALWAYS_INLINE GSVector2i xx() const { return GSVector2i(_mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 0, 0))); }
  ALWAYS_INLINE GSVector2i yx() const { return GSVector2i(_mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 0, 1))); }
  ALWAYS_INLINE GSVector2i yy() const { return GSVector2i(_mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 1, 1))); }
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
    __m128 m;
  };

  GSVector2() = default;

  constexpr static GSVector2 cxpr(float x, float y) { return GSVector2(cxpr_init, x, y); }
  constexpr static GSVector2 cxpr(float x) { return GSVector2(cxpr_init, x, x); }
  constexpr static GSVector2 cxpr(int x, int y) { return GSVector2(cxpr_init, x, y); }
  constexpr static GSVector2 cxpr(int x) { return GSVector2(cxpr_init, x, x); }

  ALWAYS_INLINE GSVector2(float x, float y) { m = _mm_set_ps(0, 0, y, x); }
  ALWAYS_INLINE GSVector2(int x, int y)
  {
    GSVector2i v_(x, y);
    m = _mm_cvtepi32_ps(v_.m);
  }

  ALWAYS_INLINE constexpr explicit GSVector2(__m128 m) : m(m) {}
  ALWAYS_INLINE explicit GSVector2(__m128d m) : m(_mm_castpd_ps(m)) {}
  ALWAYS_INLINE explicit GSVector2(float f) { *this = f; }
  ALWAYS_INLINE explicit GSVector2(int i)
  {
#ifdef CPU_ARCH_AVX2
    m = _mm_cvtepi32_ps(_mm_broadcastd_epi32(_mm_cvtsi32_si128(i)));
#else
    *this = GSVector2(GSVector2i(i));
#endif
  }

  ALWAYS_INLINE explicit GSVector2(const GSVector2i& v);

  ALWAYS_INLINE GSVector2& operator=(float f)
  {
    m = _mm_set1_ps(f);
    return *this;
  }

  ALWAYS_INLINE GSVector2& operator=(__m128 m_)
  {
    m = m_;
    return *this;
  }

  ALWAYS_INLINE operator __m128() const { return m; }

  ALWAYS_INLINE GSVector2 abs() const { return *this & cast(GSVector2i::cxpr(0x7fffffff)); }
  ALWAYS_INLINE GSVector2 neg() const { return *this ^ cast(GSVector2i::cxpr(0x80000000)); }
  ALWAYS_INLINE GSVector2 rcp() const { return GSVector2(_mm_rcp_ps(m)); }
  ALWAYS_INLINE GSVector2 floor() const
  {
    return GSVector2(_mm_round_ps(m, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
  }

  ALWAYS_INLINE GSVector2 ceil() const { return GSVector2(_mm_round_ps(m, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC)); }

  ALWAYS_INLINE GSVector2 sat(const GSVector2& min, const GSVector2& max) const
  {
    return GSVector2(_mm_min_ps(_mm_max_ps(m, min), max));
  }

  ALWAYS_INLINE GSVector2 sat(const float scale = 255) const { return sat(zero(), GSVector2(scale)); }

  ALWAYS_INLINE GSVector2 clamp(const float scale = 255) const { return min(GSVector2(scale)); }

  ALWAYS_INLINE GSVector2 min(const GSVector2& v) const { return GSVector2(_mm_min_ps(m, v)); }

  ALWAYS_INLINE GSVector2 max(const GSVector2& v) const { return GSVector2(_mm_max_ps(m, v)); }

  template<int mask>
  ALWAYS_INLINE GSVector2 blend32(const GSVector2& v) const
  {
    return GSVector2(_mm_blend_ps(m, v, mask));
  }

  ALWAYS_INLINE GSVector2 blend32(const GSVector2& v, const GSVector2& mask) const
  {
    return GSVector2(_mm_blendv_ps(m, v, mask));
  }

  ALWAYS_INLINE GSVector2 andnot(const GSVector2& v) const { return GSVector2(_mm_andnot_ps(v.m, m)); }

  ALWAYS_INLINE int mask() const { return (_mm_movemask_ps(m) & 0x3); }

  ALWAYS_INLINE bool alltrue() const { return (mask() == 0x3); }

  ALWAYS_INLINE bool allfalse() const { return (mask() == 0x0); }

  ALWAYS_INLINE GSVector2 replace_nan(const GSVector2& v) const { return v.blend32(*this, *this == *this); }

  template<int src, int dst>
  ALWAYS_INLINE GSVector2 insert32(const GSVector2& v) const
  {
    if constexpr (src == dst)
      return GSVector2(_mm_blend_ps(m, v.m, 1 << src));
    else
      return GSVector2(_mm_insert_ps(m, v.m, _MM_MK_INSERTPS_NDX(src, dst, 0)));
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return _mm_extract_ps(m, i);
  }

  ALWAYS_INLINE float dot(const GSVector2& v) const { return _mm_cvtss_f32(_mm_dp_ps(m, v.m, 0x31)); }

  ALWAYS_INLINE static GSVector2 zero() { return GSVector2(_mm_setzero_ps()); }

  ALWAYS_INLINE static GSVector2 xffffffff() { return zero() == zero(); }

  ALWAYS_INLINE static GSVector2 load(const void* p)
  {
    return GSVector2(_mm_castpd_ps(_mm_load_sd(static_cast<const double*>(p))));
  }

  ALWAYS_INLINE static GSVector2 load(float f) { return GSVector2(_mm_load_ss(&f)); }

  ALWAYS_INLINE static void store(void* p, const GSVector2& v)
  {
    _mm_store_sd(static_cast<double*>(p), _mm_castps_pd(v.m));
  }

  ALWAYS_INLINE GSVector2 operator-() const { return neg(); }

  ALWAYS_INLINE GSVector2& operator+=(const GSVector2& v_)
  {
    m = _mm_add_ps(m, v_);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator-=(const GSVector2& v_)
  {
    m = _mm_sub_ps(m, v_);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator*=(const GSVector2& v_)
  {
    m = _mm_mul_ps(m, v_);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator/=(const GSVector2& v_)
  {
    m = _mm_div_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE GSVector2& operator+=(float f)
  {
    *this += GSVector2(f);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator-=(float f)
  {
    *this -= GSVector2(f);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator*=(float f)
  {
    *this *= GSVector2(f);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator/=(float f)
  {
    *this /= GSVector2(f);
    return *this;
  }

  ALWAYS_INLINE GSVector2& operator&=(const GSVector2& v_)
  {
    m = _mm_and_ps(m, v_);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator|=(const GSVector2& v_)
  {
    m = _mm_or_ps(m, v_);
    return *this;
  }
  ALWAYS_INLINE GSVector2& operator^=(const GSVector2& v_)
  {
    m = _mm_xor_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE friend GSVector2 operator+(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_add_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator-(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_sub_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator*(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_mul_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator/(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_div_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator+(const GSVector2& v, float f) { return v + GSVector2(f); }

  ALWAYS_INLINE friend GSVector2 operator-(const GSVector2& v, float f) { return v - GSVector2(f); }

  ALWAYS_INLINE friend GSVector2 operator*(const GSVector2& v, float f) { return v * GSVector2(f); }

  ALWAYS_INLINE friend GSVector2 operator/(const GSVector2& v, float f) { return v / GSVector2(f); }

  ALWAYS_INLINE friend GSVector2 operator&(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_and_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator|(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_or_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator^(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_xor_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator==(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_cmpeq_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator!=(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_cmpneq_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator>(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_cmpgt_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator<(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_cmplt_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator>=(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_cmpge_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector2 operator<=(const GSVector2& v1, const GSVector2& v2)
  {
    return GSVector2(_mm_cmple_ps(v1, v2));
  }

  ALWAYS_INLINE static GSVector2 cast(const GSVector2i& v);

  ALWAYS_INLINE GSVector2 xy() const { return *this; }
  ALWAYS_INLINE GSVector2 xx() const { return GSVector2(_mm_shuffle_ps(m, m, _MM_SHUFFLE(3, 2, 0, 0))); }
  ALWAYS_INLINE GSVector2 yx() const { return GSVector2(_mm_shuffle_ps(m, m, _MM_SHUFFLE(3, 2, 0, 1))); }
  ALWAYS_INLINE GSVector2 yy() const { return GSVector2(_mm_shuffle_ps(m, m, _MM_SHUFFLE(3, 2, 1, 1))); }
};

class alignas(16) GSVector4i
{
  struct cxpr_init_tag
  {
  };
  static constexpr cxpr_init_tag cxpr_init{};

  constexpr GSVector4i(cxpr_init_tag, s32 x, s32 y, s32 z, s32 w) : S32{x, y, z, w} {}

  constexpr GSVector4i(cxpr_init_tag, s16 s0, s16 s1, s16 s2, s16 s3, s16 s4, s16 s5, s16 s6, s16 s7)
    : S16{s0, s1, s2, s3, s4, s5, s6, s7}
  {
  }

  constexpr GSVector4i(cxpr_init_tag, s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7, s8 b8, s8 b9, s8 b10,
                       s8 b11, s8 b12, s8 b13, s8 b14, s8 b15)
    : S8{b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15}
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
    s8 S8[16];
    s16 S16[8];
    s32 S32[4];
    s64 S64[2];
    u8 U8[16];
    u16 U16[8];
    u32 U32[4];
    u64 U64[2];
    __m128i m;
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

  ALWAYS_INLINE GSVector4i(s32 x, s32 y, s32 z, s32 w) { m = _mm_set_epi32(w, z, y, x); }
  ALWAYS_INLINE GSVector4i(s32 x, s32 y) { *this = load(x).upl32(load(y)); }
  ALWAYS_INLINE GSVector4i(s16 s0, s16 s1, s16 s2, s16 s3, s16 s4, s16 s5, s16 s6, s16 s7)
  {
    m = _mm_set_epi16(s7, s6, s5, s4, s3, s2, s1, s0);
  }

  ALWAYS_INLINE constexpr GSVector4i(s8 b0, s8 b1, s8 b2, s8 b3, s8 b4, s8 b5, s8 b6, s8 b7, s8 b8, s8 b9, s8 b10,
                                     s8 b11, s8 b12, s8 b13, s8 b14, s8 b15)
    : S8{b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15}
  {
  }

  ALWAYS_INLINE explicit GSVector4i(const GSVector2i& v) { m = v.m; }

  ALWAYS_INLINE explicit GSVector4i(s32 i) { *this = i; }

  ALWAYS_INLINE explicit GSVector4i(const GSVector2& v);

  ALWAYS_INLINE explicit GSVector4i(const GSVector4& v);

  ALWAYS_INLINE constexpr explicit GSVector4i(__m128i m) : m(m) {}

  ALWAYS_INLINE GSVector4i& operator=(s32 i)
  {
    m = _mm_set1_epi32(i);
    return *this;
  }
  ALWAYS_INLINE GSVector4i& operator=(__m128i m_)
  {
    m = m_;
    return *this;
  }

  ALWAYS_INLINE operator __m128i() const { return m; }

  ALWAYS_INLINE s32 width() const { return right - left; }

  ALWAYS_INLINE s32 height() const { return bottom - top; }

  ALWAYS_INLINE GSVector4i rsize() const
  {
    return sub32(xyxy()); // same as GSVector4i(0, 0, width(), height());
  }

  ALWAYS_INLINE s32 rarea() const { return width() * height(); }

  ALWAYS_INLINE bool rempty() const { return lt32(zwzw()).mask() != 0x00ff; }

  ALWAYS_INLINE GSVector4i runion(const GSVector4i& v) const { return min_i32(v).blend32<0xc>(max_i32(v)); }

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

  ALWAYS_INLINE GSVector4i min_i8(const GSVector4i& v) const { return GSVector4i(_mm_min_epi8(m, v)); }
  ALWAYS_INLINE GSVector4i max_i8(const GSVector4i& v) const { return GSVector4i(_mm_max_epi8(m, v)); }
  ALWAYS_INLINE GSVector4i min_i16(const GSVector4i& v) const { return GSVector4i(_mm_min_epi16(m, v)); }
  ALWAYS_INLINE GSVector4i max_i16(const GSVector4i& v) const { return GSVector4i(_mm_max_epi16(m, v)); }
  ALWAYS_INLINE GSVector4i min_i32(const GSVector4i& v) const { return GSVector4i(_mm_min_epi32(m, v)); }
  ALWAYS_INLINE GSVector4i max_i32(const GSVector4i& v) const { return GSVector4i(_mm_max_epi32(m, v)); }

  ALWAYS_INLINE GSVector4i min_u8(const GSVector4i& v) const { return GSVector4i(_mm_min_epu8(m, v)); }
  ALWAYS_INLINE GSVector4i max_u8(const GSVector4i& v) const { return GSVector4i(_mm_max_epu8(m, v)); }
  ALWAYS_INLINE GSVector4i min_u16(const GSVector4i& v) const { return GSVector4i(_mm_min_epu16(m, v)); }
  ALWAYS_INLINE GSVector4i max_u16(const GSVector4i& v) const { return GSVector4i(_mm_max_epu16(m, v)); }
  ALWAYS_INLINE GSVector4i min_u32(const GSVector4i& v) const { return GSVector4i(_mm_min_epu32(m, v)); }
  ALWAYS_INLINE GSVector4i max_u32(const GSVector4i& v) const { return GSVector4i(_mm_max_epu32(m, v)); }

  ALWAYS_INLINE GSVector4i madd_s16(const GSVector4i& v) const { return GSVector4i(_mm_madd_epi16(m, v.m)); }

  ALWAYS_INLINE GSVector4i addp_s32() const { return GSVector4i(_mm_hadd_epi32(m, m)); }

  ALWAYS_INLINE s32 addv_s32() const
  {
    const __m128i pairs = _mm_hadd_epi32(m, m);
    return _mm_cvtsi128_si32(_mm_hadd_epi32(pairs, pairs));
  }

  ALWAYS_INLINE u8 minv_u8() const
  {
    __m128i vmin = _mm_min_epu8(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    vmin = _mm_min_epu8(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u8>(std::min(
      static_cast<u32>(_mm_extract_epi8(vmin, 0)),
      std::min(static_cast<u32>(_mm_extract_epi8(vmin, 1)),
               std::min(static_cast<u32>(_mm_extract_epi8(vmin, 2)), static_cast<u32>(_mm_extract_epi8(vmin, 3))))));
  }

  ALWAYS_INLINE u16 maxv_u8() const
  {
    __m128i vmax = _mm_max_epu8(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    vmax = _mm_max_epu8(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u8>(std::max(
      static_cast<u32>(_mm_extract_epi8(vmax, 0)),
      std::max(static_cast<u32>(_mm_extract_epi8(vmax, 1)),
               std::max(static_cast<u32>(_mm_extract_epi8(vmax, 2)), static_cast<u32>(_mm_extract_epi8(vmax, 3))))));
  }

  ALWAYS_INLINE u16 minv_u16() const
  {
    __m128i vmin = _mm_min_epu16(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    vmin = _mm_min_epu16(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u16>(
      std::min(static_cast<u32>(_mm_extract_epi16(vmin, 0)), static_cast<u32>(_mm_extract_epi16(vmin, 1))));
  }

  ALWAYS_INLINE u16 maxv_u16() const
  {
    __m128i vmax = _mm_max_epu16(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    vmax = _mm_max_epu16(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(1, 1, 1, 1)));
    return static_cast<u16>(
      std::max<u32>(static_cast<u32>(_mm_extract_epi16(vmax, 0)), static_cast<u32>(_mm_extract_epi16(vmax, 1))));
  }

  ALWAYS_INLINE s32 minv_s32() const
  {
    const __m128i vmin = _mm_min_epi32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    return std::min<s32>(_mm_extract_epi32(vmin, 0), _mm_extract_epi32(vmin, 1));
  }

  ALWAYS_INLINE u32 minv_u32() const
  {
    const __m128i vmin = _mm_min_epu32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    return std::min<u32>(_mm_extract_epi32(vmin, 0), _mm_extract_epi32(vmin, 1));
  }

  ALWAYS_INLINE s32 maxv_s32() const
  {
    const __m128i vmax = _mm_max_epi32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    return std::max<s32>(_mm_extract_epi32(vmax, 0), _mm_extract_epi32(vmax, 1));
  }

  ALWAYS_INLINE u32 maxv_u32() const
  {
    const __m128i vmax = _mm_max_epu32(m, _mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2)));
    return std::max<u32>(_mm_extract_epi32(vmax, 0), _mm_extract_epi32(vmax, 1));
  }

  ALWAYS_INLINE GSVector4i clamp8() const { return pu16().upl8(); }

  ALWAYS_INLINE GSVector4i blend8(const GSVector4i& v, const GSVector4i& mask) const
  {
    return GSVector4i(_mm_blendv_epi8(m, v, mask));
  }

  template<s32 mask>
  ALWAYS_INLINE GSVector4i blend16(const GSVector4i& v) const
  {
    return GSVector4i(_mm_blend_epi16(m, v, mask));
  }

  template<s32 mask>
  ALWAYS_INLINE GSVector4i blend32(const GSVector4i& v) const
  {
#if defined(__AVX2__)
    return GSVector4i(_mm_blend_epi32(m, v.m, mask));
#else
    constexpr s32 bit3 = ((mask & 8) * 3) << 3;
    constexpr s32 bit2 = ((mask & 4) * 3) << 2;
    constexpr s32 bit1 = ((mask & 2) * 3) << 1;
    constexpr s32 bit0 = (mask & 1) * 3;
    return blend16<bit3 | bit2 | bit1 | bit0>(v);
#endif
  }

  ALWAYS_INLINE GSVector4i blend(const GSVector4i& v, const GSVector4i& mask) const
  {
    return GSVector4i(_mm_or_si128(_mm_andnot_si128(mask, m), _mm_and_si128(mask, v)));
  }

  ALWAYS_INLINE GSVector4i mix16(const GSVector4i& v) const { return blend16<0xaa>(v); }

  ALWAYS_INLINE GSVector4i shuffle8(const GSVector4i& mask) const { return GSVector4i(_mm_shuffle_epi8(m, mask)); }

  ALWAYS_INLINE GSVector4i ps16(const GSVector4i& v) const { return GSVector4i(_mm_packs_epi16(m, v)); }
  ALWAYS_INLINE GSVector4i ps16() const { return GSVector4i(_mm_packs_epi16(m, m)); }
  ALWAYS_INLINE GSVector4i pu16(const GSVector4i& v) const { return GSVector4i(_mm_packus_epi16(m, v)); }
  ALWAYS_INLINE GSVector4i pu16() const { return GSVector4i(_mm_packus_epi16(m, m)); }
  ALWAYS_INLINE GSVector4i ps32(const GSVector4i& v) const { return GSVector4i(_mm_packs_epi32(m, v)); }
  ALWAYS_INLINE GSVector4i ps32() const { return GSVector4i(_mm_packs_epi32(m, m)); }
  ALWAYS_INLINE GSVector4i pu32(const GSVector4i& v) const { return GSVector4i(_mm_packus_epi32(m, v)); }
  ALWAYS_INLINE GSVector4i pu32() const { return GSVector4i(_mm_packus_epi32(m, m)); }

  ALWAYS_INLINE GSVector4i upl8(const GSVector4i& v) const { return GSVector4i(_mm_unpacklo_epi8(m, v)); }
  ALWAYS_INLINE GSVector4i uph8(const GSVector4i& v) const { return GSVector4i(_mm_unpackhi_epi8(m, v)); }
  ALWAYS_INLINE GSVector4i upl16(const GSVector4i& v) const { return GSVector4i(_mm_unpacklo_epi16(m, v)); }
  ALWAYS_INLINE GSVector4i uph16(const GSVector4i& v) const { return GSVector4i(_mm_unpackhi_epi16(m, v)); }
  ALWAYS_INLINE GSVector4i upl32(const GSVector4i& v) const { return GSVector4i(_mm_unpacklo_epi32(m, v)); }
  ALWAYS_INLINE GSVector4i uph32(const GSVector4i& v) const { return GSVector4i(_mm_unpackhi_epi32(m, v)); }
  ALWAYS_INLINE GSVector4i upl64(const GSVector4i& v) const { return GSVector4i(_mm_unpacklo_epi64(m, v)); }
  ALWAYS_INLINE GSVector4i uph64(const GSVector4i& v) const { return GSVector4i(_mm_unpackhi_epi64(m, v)); }

  ALWAYS_INLINE GSVector4i upl8() const { return GSVector4i(_mm_unpacklo_epi8(m, _mm_setzero_si128())); }
  ALWAYS_INLINE GSVector4i uph8() const { return GSVector4i(_mm_unpackhi_epi8(m, _mm_setzero_si128())); }

  ALWAYS_INLINE GSVector4i upl16() const { return GSVector4i(_mm_unpacklo_epi16(m, _mm_setzero_si128())); }
  ALWAYS_INLINE GSVector4i uph16() const { return GSVector4i(_mm_unpackhi_epi16(m, _mm_setzero_si128())); }

  ALWAYS_INLINE GSVector4i upl32() const { return GSVector4i(_mm_unpacklo_epi32(m, _mm_setzero_si128())); }

  ALWAYS_INLINE GSVector4i uph32() const { return GSVector4i(_mm_unpackhi_epi32(m, _mm_setzero_si128())); }
  ALWAYS_INLINE GSVector4i upl64() const { return GSVector4i(_mm_unpacklo_epi64(m, _mm_setzero_si128())); }
  ALWAYS_INLINE GSVector4i uph64() const { return GSVector4i(_mm_unpackhi_epi64(m, _mm_setzero_si128())); }

  ALWAYS_INLINE GSVector4i s8to16() const { return GSVector4i(_mm_cvtepi8_epi16(m)); }
  ALWAYS_INLINE GSVector4i s8to32() const { return GSVector4i(_mm_cvtepi8_epi32(m)); }
  ALWAYS_INLINE GSVector4i s8to64() const { return GSVector4i(_mm_cvtepi8_epi64(m)); }

#ifdef CPU_ARCH_SSE41
  ALWAYS_INLINE GSVector4i s16to32() const { return GSVector4i(_mm_cvtepi16_epi32(m)); }
  ALWAYS_INLINE GSVector4i s16to64() const { return GSVector4i(_mm_cvtepi16_epi64(m)); }
  ALWAYS_INLINE GSVector4i s32to64() const { return GSVector4i(_mm_cvtepi32_epi64(m)); }
  ALWAYS_INLINE GSVector4i u8to16() const { return GSVector4i(_mm_cvtepu8_epi16(m)); }
  ALWAYS_INLINE GSVector4i u8to32() const { return GSVector4i(_mm_cvtepu8_epi32(m)); }
  ALWAYS_INLINE GSVector4i u8to64() const { return GSVector4i(_mm_cvtepu16_epi64(m)); }
  ALWAYS_INLINE GSVector4i u16to32() const { return GSVector4i(_mm_cvtepu16_epi32(m)); }
  ALWAYS_INLINE GSVector4i u16to64() const { return GSVector4i(_mm_cvtepu16_epi64(m)); }
  ALWAYS_INLINE GSVector4i u32to64() const { return GSVector4i(_mm_cvtepu32_epi64(m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector4i srl() const
  {
    return GSVector4i(_mm_srli_si128(m, i));
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i srl(const GSVector4i& v)
  {
    return GSVector4i(_mm_alignr_epi8(v.m, m, i));
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i sll() const
  {
    return GSVector4i(_mm_slli_si128(m, i));
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i sll16() const
  {
    return GSVector4i(_mm_slli_epi16(m, i));
  }

  ALWAYS_INLINE GSVector4i sll16(s32 i) const { return GSVector4i(_mm_sll_epi16(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i sllv16(const GSVector4i& v) const { return GSVector4i(_mm_sllv_epi16(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector4i srl16() const
  {
    return GSVector4i(_mm_srli_epi16(m, i));
  }

  ALWAYS_INLINE GSVector4i srl16(s32 i) const { return GSVector4i(_mm_srl_epi16(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i srlv16(const GSVector4i& v) const { return GSVector4i(_mm_srlv_epi16(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector4i sra16() const
  {
    return GSVector4i(_mm_srai_epi16(m, i));
  }

  ALWAYS_INLINE GSVector4i sra16(s32 i) const { return GSVector4i(_mm_sra_epi16(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i srav16(const GSVector4i& v) const { return GSVector4i(_mm_srav_epi16(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector4i sll32() const
  {
    return GSVector4i(_mm_slli_epi32(m, i));
  }

  ALWAYS_INLINE GSVector4i sll32(s32 i) const { return GSVector4i(_mm_sll_epi32(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i sllv32(const GSVector4i& v) const { return GSVector4i(_mm_sllv_epi32(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector4i srl32() const
  {
    return GSVector4i(_mm_srli_epi32(m, i));
  }

  ALWAYS_INLINE GSVector4i srl32(s32 i) const { return GSVector4i(_mm_srl_epi32(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i srlv32(const GSVector4i& v) const { return GSVector4i(_mm_srlv_epi32(m, v.m)); }
#endif

  template<s32 i>
  ALWAYS_INLINE GSVector4i sra32() const
  {
    return GSVector4i(_mm_srai_epi32(m, i));
  }

  ALWAYS_INLINE GSVector4i sra32(s32 i) const { return GSVector4i(_mm_sra_epi32(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i srav32(const GSVector4i& v) const { return GSVector4i(_mm_srav_epi32(m, v.m)); }
#endif

  template<s64 i>
  ALWAYS_INLINE GSVector4i sll64() const
  {
    return GSVector4i(_mm_slli_epi64(m, i));
  }

  ALWAYS_INLINE GSVector4i sll64(s32 i) const { return GSVector4i(_mm_sll_epi64(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i sllv64(const GSVector4i& v) const { return GSVector4i(_mm_sllv_epi64(m, v.m)); }
#endif

  template<s64 i>
  ALWAYS_INLINE GSVector4i srl64() const
  {
    return GSVector4i(_mm_srli_epi64(m, i));
  }

  ALWAYS_INLINE GSVector4i srl64(s32 i) const { return GSVector4i(_mm_srl_epi64(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i srlv64(const GSVector4i& v) const { return GSVector4i(_mm_srlv_epi64(m, v.m)); }
#endif

  template<s64 i>
  ALWAYS_INLINE GSVector4i sra64() const
  {
    return GSVector4i(_mm_srai_epi64(m, i));
  }

  ALWAYS_INLINE GSVector4i sra64(s32 i) const { return GSVector4i(_mm_sra_epi64(m, _mm_cvtsi32_si128(i))); }

#ifdef CPU_ARCH_AVX2
  ALWAYS_INLINE GSVector4i srav64(const GSVector4i& v) const { return GSVector4i(_mm_srav_epi64(m, v.m)); }
#endif

  ALWAYS_INLINE GSVector4i add8(const GSVector4i& v) const { return GSVector4i(_mm_add_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i add16(const GSVector4i& v) const { return GSVector4i(_mm_add_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i add32(const GSVector4i& v) const { return GSVector4i(_mm_add_epi32(m, v.m)); }
  ALWAYS_INLINE GSVector4i adds8(const GSVector4i& v) const { return GSVector4i(_mm_adds_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i adds16(const GSVector4i& v) const { return GSVector4i(_mm_adds_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i hadds16(const GSVector4i& v) const { return GSVector4i(_mm_hadds_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i addus8(const GSVector4i& v) const { return GSVector4i(_mm_adds_epu8(m, v.m)); }
  ALWAYS_INLINE GSVector4i addus16(const GSVector4i& v) const { return GSVector4i(_mm_adds_epu16(m, v.m)); }

  ALWAYS_INLINE GSVector4i sub8(const GSVector4i& v) const { return GSVector4i(_mm_sub_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i sub16(const GSVector4i& v) const { return GSVector4i(_mm_sub_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i sub32(const GSVector4i& v) const { return GSVector4i(_mm_sub_epi32(m, v.m)); }
  ALWAYS_INLINE GSVector4i subs8(const GSVector4i& v) const { return GSVector4i(_mm_subs_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i subs16(const GSVector4i& v) const { return GSVector4i(_mm_subs_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i subus8(const GSVector4i& v) const { return GSVector4i(_mm_subs_epu8(m, v.m)); }
  ALWAYS_INLINE GSVector4i subus16(const GSVector4i& v) const { return GSVector4i(_mm_subs_epu16(m, v.m)); }

  ALWAYS_INLINE GSVector4i avg8(const GSVector4i& v) const { return GSVector4i(_mm_avg_epu8(m, v.m)); }
  ALWAYS_INLINE GSVector4i avg16(const GSVector4i& v) const { return GSVector4i(_mm_avg_epu16(m, v.m)); }

  ALWAYS_INLINE GSVector4i mul16hs(const GSVector4i& v) const { return GSVector4i(_mm_mulhi_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i mul16hu(const GSVector4i& v) const { return GSVector4i(_mm_mulhi_epu16(m, v.m)); }
  ALWAYS_INLINE GSVector4i mul16l(const GSVector4i& v) const { return GSVector4i(_mm_mullo_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i mul16hrs(const GSVector4i& v) const { return GSVector4i(_mm_mulhrs_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i mul32l(const GSVector4i& v) const { return GSVector4i(_mm_mullo_epi32(m, v.m)); }

  ALWAYS_INLINE bool eq(const GSVector4i& v) const
  {
    const GSVector4i t = *this ^ v;
    return _mm_testz_si128(t, t) != 0;
  }

  ALWAYS_INLINE GSVector4i eq8(const GSVector4i& v) const { return GSVector4i(_mm_cmpeq_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i eq16(const GSVector4i& v) const { return GSVector4i(_mm_cmpeq_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i eq32(const GSVector4i& v) const { return GSVector4i(_mm_cmpeq_epi32(m, v.m)); }
  ALWAYS_INLINE GSVector4i eq64(const GSVector4i& v) const { return GSVector4i(_mm_cmpeq_epi64(m, v.m)); }

  ALWAYS_INLINE GSVector4i neq8(const GSVector4i& v) const { return ~eq8(v); }
  ALWAYS_INLINE GSVector4i neq16(const GSVector4i& v) const { return ~eq16(v); }
  ALWAYS_INLINE GSVector4i neq32(const GSVector4i& v) const { return ~eq32(v); }

  ALWAYS_INLINE GSVector4i gt8(const GSVector4i& v) const { return GSVector4i(_mm_cmpgt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i gt16(const GSVector4i& v) const { return GSVector4i(_mm_cmpgt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i gt32(const GSVector4i& v) const { return GSVector4i(_mm_cmpgt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector4i ge8(const GSVector4i& v) const { return ~GSVector4i(_mm_cmplt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i ge16(const GSVector4i& v) const { return ~GSVector4i(_mm_cmplt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i ge32(const GSVector4i& v) const { return ~GSVector4i(_mm_cmplt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector4i lt8(const GSVector4i& v) const { return GSVector4i(_mm_cmplt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i lt16(const GSVector4i& v) const { return GSVector4i(_mm_cmplt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i lt32(const GSVector4i& v) const { return GSVector4i(_mm_cmplt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector4i le8(const GSVector4i& v) const { return ~GSVector4i(_mm_cmpgt_epi8(m, v.m)); }
  ALWAYS_INLINE GSVector4i le16(const GSVector4i& v) const { return ~GSVector4i(_mm_cmpgt_epi16(m, v.m)); }
  ALWAYS_INLINE GSVector4i le32(const GSVector4i& v) const { return ~GSVector4i(_mm_cmpgt_epi32(m, v.m)); }

  ALWAYS_INLINE GSVector4i andnot(const GSVector4i& v) const { return GSVector4i(_mm_andnot_si128(v.m, m)); }

  ALWAYS_INLINE s32 mask() const { return _mm_movemask_epi8(m); }

  ALWAYS_INLINE bool alltrue() const { return mask() == 0xffff; }

  ALWAYS_INLINE bool allfalse() const { return _mm_testz_si128(m, m) != 0; }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert8(s32 a) const
  {
    return GSVector4i(_mm_insert_epi8(m, a, i));
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract8() const
  {
    return _mm_extract_epi8(m, i);
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert16(s32 a) const
  {
    return GSVector4i(_mm_insert_epi16(m, a, i));
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract16() const
  {
    return _mm_extract_epi16(m, i);
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert32(s32 a) const
  {
    return GSVector4i(_mm_insert_epi32(m, a, i));
  }

  template<s32 i>
  ALWAYS_INLINE s32 extract32() const
  {
    if constexpr (i == 0)
      return GSVector4i::store(*this);

    return _mm_extract_epi32(m, i);
  }

  template<s32 i>
  ALWAYS_INLINE GSVector4i insert64(s64 a) const
  {
    return GSVector4i(_mm_insert_epi64(m, a, i));
  }

  template<s32 i>
  ALWAYS_INLINE s64 extract64() const
  {
    if (i == 0)
      return GSVector4i::storeq(*this);

    return _mm_extract_epi64(m, i);
  }

  ALWAYS_INLINE static GSVector4i loadnt(const void* p)
  {
    // Should be const, but isn't...
    return GSVector4i(_mm_stream_load_si128(const_cast<__m128i*>(static_cast<const __m128i*>(p))));
  }

  ALWAYS_INLINE static GSVector4i load32(const void* p) { return GSVector4i(_mm_loadu_si32(p)); }

  ALWAYS_INLINE static GSVector4i loadl(const void* p)
  {
    return GSVector4i(_mm_loadl_epi64(static_cast<const __m128i*>(p)));
  }

  ALWAYS_INLINE static GSVector4i loadh(const void* p)
  {
    return GSVector4i(_mm_castps_si128(_mm_loadh_pi(_mm_setzero_ps(), static_cast<const __m64*>(p))));
  }

  ALWAYS_INLINE static GSVector4i loadh(const GSVector2i& v)
  {
    return GSVector4i(_mm_unpacklo_epi64(_mm_setzero_si128(), v.m));
  }

  template<bool aligned>
  ALWAYS_INLINE static GSVector4i load(const void* p)
  {
    return GSVector4i(aligned ? _mm_load_si128(static_cast<const __m128i*>(p)) :
                                _mm_loadu_si128(static_cast<const __m128i*>(p)));
  }

  ALWAYS_INLINE static GSVector4i load(s32 i) { return GSVector4i(_mm_cvtsi32_si128(i)); }
  ALWAYS_INLINE static GSVector4i loadq(s64 i) { return GSVector4i(_mm_cvtsi64_si128(i)); }

  ALWAYS_INLINE static void storent(void* p, const GSVector4i& v) { _mm_stream_si128(static_cast<__m128i*>(p), v.m); }
  ALWAYS_INLINE static void storel(void* p, const GSVector4i& v) { _mm_storel_epi64(static_cast<__m128i*>(p), v.m); }
  ALWAYS_INLINE static void storeh(void* p, const GSVector4i& v)
  {
    _mm_storeh_pi(static_cast<__m64*>(p), _mm_castsi128_ps(v.m));
  }

  ALWAYS_INLINE static void store(void* pl, void* ph, const GSVector4i& v)
  {
    GSVector4i::storel(pl, v);
    GSVector4i::storeh(ph, v);
  }

  template<bool aligned>
  ALWAYS_INLINE static void store(void* p, const GSVector4i& v)
  {
    if constexpr (aligned)
      _mm_store_si128(static_cast<__m128i*>(p), v.m);
    else
      _mm_storeu_si128(static_cast<__m128i*>(p), v.m);
  }

  ALWAYS_INLINE static void store32(void* p, const GSVector4i& v) { _mm_storeu_si32(p, v); }
  ALWAYS_INLINE static s32 store(const GSVector4i& v) { return _mm_cvtsi128_si32(v.m); }
  ALWAYS_INLINE static s64 storeq(const GSVector4i& v) { return _mm_cvtsi128_si64(v.m); }

  ALWAYS_INLINE GSVector4i& operator&=(const GSVector4i& v)
  {
    m = _mm_and_si128(m, v);
    return *this;
  }
  ALWAYS_INLINE GSVector4i& operator|=(const GSVector4i& v)
  {
    m = _mm_or_si128(m, v);
    return *this;
  }
  ALWAYS_INLINE GSVector4i& operator^=(const GSVector4i& v)
  {
    m = _mm_xor_si128(m, v);
    return *this;
  }

  ALWAYS_INLINE friend GSVector4i operator&(const GSVector4i& v1, const GSVector4i& v2)
  {
    return GSVector4i(_mm_and_si128(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4i operator|(const GSVector4i& v1, const GSVector4i& v2)
  {
    return GSVector4i(_mm_or_si128(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4i operator^(const GSVector4i& v1, const GSVector4i& v2)
  {
    return GSVector4i(_mm_xor_si128(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4i operator&(const GSVector4i& v, s32 i) { return v & GSVector4i(i); }
  ALWAYS_INLINE friend GSVector4i operator|(const GSVector4i& v, s32 i) { return v | GSVector4i(i); }
  ALWAYS_INLINE friend GSVector4i operator^(const GSVector4i& v, s32 i) { return v ^ GSVector4i(i); }
  ALWAYS_INLINE friend GSVector4i operator~(const GSVector4i& v) { return v ^ v.eq32(v); }

  ALWAYS_INLINE static GSVector4i zero() { return GSVector4i(_mm_setzero_si128()); }
  ALWAYS_INLINE static GSVector4i cast(const GSVector4& v);

  ALWAYS_INLINE GSVector4i xyxy(const GSVector4i& v) const { return upl64(v); }

  ALWAYS_INLINE GSVector2i xy() const { return GSVector2i(m); }

  ALWAYS_INLINE GSVector2i zw() const { return GSVector2i(_mm_shuffle_epi32(m, _MM_SHUFFLE(3, 2, 3, 2))); }

  // clang-format off

#define VECTOR4i_SHUFFLE_4(xs, xn, ys, yn, zs, zn, ws, wn) \
    ALWAYS_INLINE GSVector4i xs##ys##zs##ws() const {return GSVector4i(_mm_shuffle_epi32(m, _MM_SHUFFLE(wn, zn, yn, xn)));} \
    ALWAYS_INLINE GSVector4i xs##ys##zs##ws##l() const {return GSVector4i(_mm_shufflelo_epi16(m, _MM_SHUFFLE(wn, zn, yn, xn)));} \
    ALWAYS_INLINE GSVector4i xs##ys##zs##ws##h() const {return GSVector4i(_mm_shufflehi_epi16(m, _MM_SHUFFLE(wn, zn, yn, xn)));} \
    ALWAYS_INLINE GSVector4i xs##ys##zs##ws##lh() const {return GSVector4i(_mm_shufflehi_epi16(_mm_shufflelo_epi16(m, _MM_SHUFFLE(wn, zn, yn, xn)), _MM_SHUFFLE(wn, zn, yn, xn)));} \

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

  constexpr GSVector4(cxpr_init_tag, double x, double y) : F64{x, y} {}

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
    __m128 m;
  };

  GSVector4() = default;

  constexpr static GSVector4 cxpr(float x, float y, float z, float w) { return GSVector4(cxpr_init, x, y, z, w); }
  constexpr static GSVector4 cxpr(float x) { return GSVector4(cxpr_init, x, x, x, x); }
  constexpr static GSVector4 cxpr(int x, int y, int z, int w) { return GSVector4(cxpr_init, x, y, z, w); }
  constexpr static GSVector4 cxpr(int x) { return GSVector4(cxpr_init, x, x, x, x); }

  constexpr static GSVector4 cxpr64(u64 x, u64 y) { return GSVector4(cxpr_init, x, y); }
  constexpr static GSVector4 cxpr64(u64 x) { return GSVector4(cxpr_init, x, x); }

  constexpr static GSVector4 cxpr64(double x, double y) { return GSVector4(cxpr_init, x, y); }
  constexpr static GSVector4 cxpr64(double x) { return GSVector4(cxpr_init, x, x); }

  ALWAYS_INLINE GSVector4(float x, float y, float z, float w) { m = _mm_set_ps(w, z, y, x); }
  ALWAYS_INLINE GSVector4(float x, float y) { m = _mm_unpacklo_ps(_mm_load_ss(&x), _mm_load_ss(&y)); }
  ALWAYS_INLINE GSVector4(int x, int y, int z, int w)
  {
    GSVector4i v_(x, y, z, w);
    m = _mm_cvtepi32_ps(v_.m);
  }
  ALWAYS_INLINE GSVector4(int x, int y)
  {
    m = _mm_cvtepi32_ps(_mm_unpacklo_epi32(_mm_cvtsi32_si128(x), _mm_cvtsi32_si128(y)));
  }

  ALWAYS_INLINE explicit GSVector4(const GSVector2& v) : m(v.m) {}
  ALWAYS_INLINE explicit GSVector4(const GSVector2i& v)
    : m(_mm_castpd_ps(_mm_unpacklo_pd(_mm_castps_pd(_mm_cvtepi32_ps(v.m)), _mm_setzero_pd())))
  {
  }

  ALWAYS_INLINE constexpr explicit GSVector4(__m128 m) : m(m) {}

  ALWAYS_INLINE explicit GSVector4(__m128d m) : m(_mm_castpd_ps(m)) {}

  ALWAYS_INLINE explicit GSVector4(float f) { *this = f; }

  ALWAYS_INLINE explicit GSVector4(int i)
  {
#ifdef CPU_ARCH_AVX2
    m = _mm_cvtepi32_ps(_mm_broadcastd_epi32(_mm_cvtsi32_si128(i)));
#else
    *this = GSVector4(GSVector4i(i));
#endif
  }

  ALWAYS_INLINE explicit GSVector4(const GSVector4i& v);

  ALWAYS_INLINE static GSVector4 f64(double x, double y) { return GSVector4(_mm_castpd_ps(_mm_set_pd(y, x))); }
  ALWAYS_INLINE static GSVector4 f64(double x) { return GSVector4(_mm_castpd_ps(_mm_set1_pd(x))); }

  ALWAYS_INLINE GSVector4& operator=(float f)
  {
    m = _mm_set1_ps(f);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator=(__m128 m_)
  {
    this->m = m_;
    return *this;
  }

  ALWAYS_INLINE operator __m128() const { return m; }

  u32 rgba32() const { return GSVector4i(*this).rgba32(); }

  ALWAYS_INLINE static GSVector4 rgba32(u32 rgba) { return GSVector4(GSVector4i::load((int)rgba).u8to32()); }

  ALWAYS_INLINE static GSVector4 unorm8(u32 rgba) { return rgba32(rgba) * GSVector4::cxpr(1.0f / 255.0f); }

  ALWAYS_INLINE GSVector4 abs() const { return *this & cast(GSVector4i::cxpr(0x7fffffff)); }

  ALWAYS_INLINE GSVector4 neg() const { return *this ^ cast(GSVector4i::cxpr(0x80000000)); }

  ALWAYS_INLINE GSVector4 rcp() const { return GSVector4(_mm_rcp_ps(m)); }

  ALWAYS_INLINE GSVector4 rcpnr() const
  {
    GSVector4 v_ = rcp();

    return (v_ + v_) - (v_ * v_) * *this;
  }

  ALWAYS_INLINE GSVector4 floor() const
  {
    return GSVector4(_mm_round_ps(m, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
  }

  ALWAYS_INLINE GSVector4 ceil() const { return GSVector4(_mm_round_ps(m, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC)); }

  ALWAYS_INLINE GSVector4 madd(const GSVector4& a_, const GSVector4& b_) const
  {
#ifdef CPU_ARCH_AVX2
    return GSVector4(_mm_fmadd_ps(m, a_, b_));
#else
    return *this * a_ + b_;
#endif
  }

  ALWAYS_INLINE GSVector4 msub(const GSVector4& a_, const GSVector4& b_) const
  {
#ifdef CPU_ARCH_AVX2
    return GSVector4(_mm_fmsub_ps(m, a_, b_));
#else
    return *this * a_ - b_;
#endif
  }

  ALWAYS_INLINE GSVector4 nmadd(const GSVector4& a_, const GSVector4& b_) const
  {
#ifdef CPU_ARCH_AVX2
    return GSVector4(_mm_fnmadd_ps(m, a_, b_));
#else
    return b_ - *this * a_;
#endif
  }

  ALWAYS_INLINE GSVector4 nmsub(const GSVector4& a_, const GSVector4& b_) const
  {
#ifdef CPU_ARCH_AVX2
    return GSVector4(_mm_fnmsub_ps(m, a_, b_));
#else
    return -b_ - *this * a_;
#endif
  }

  ALWAYS_INLINE GSVector4 addm(const GSVector4& a_, const GSVector4& b_) const
  {
    return a_.madd(b_, *this); // *this + a * b
  }

  ALWAYS_INLINE GSVector4 subm(const GSVector4& a_, const GSVector4& b_) const
  {
    return a_.nmadd(b_, *this); // *this - a * b
  }

  ALWAYS_INLINE GSVector4 hadd() const { return GSVector4(_mm_hadd_ps(m, m)); }

  ALWAYS_INLINE GSVector4 hadd(const GSVector4& v) const { return GSVector4(_mm_hadd_ps(m, v.m)); }

  ALWAYS_INLINE GSVector4 hsub() const { return GSVector4(_mm_hsub_ps(m, m)); }

  ALWAYS_INLINE GSVector4 hsub(const GSVector4& v) const { return GSVector4(_mm_hsub_ps(m, v.m)); }

  template<int i>
  ALWAYS_INLINE GSVector4 dp(const GSVector4& v) const
  {
    return GSVector4(_mm_dp_ps(m, v.m, i));
  }

  ALWAYS_INLINE GSVector4 sat(const GSVector4& min, const GSVector4& max) const
  {
    return GSVector4(_mm_min_ps(_mm_max_ps(m, min), max));
  }

  ALWAYS_INLINE GSVector4 sat(const GSVector4& v) const
  {
    return GSVector4(_mm_min_ps(_mm_max_ps(m, v.xyxy()), v.zwzw()));
  }

  ALWAYS_INLINE GSVector4 sat(const float scale = 255) const { return sat(zero(), GSVector4(scale)); }

  ALWAYS_INLINE GSVector4 clamp(const float scale = 255) const { return min(GSVector4(scale)); }

  ALWAYS_INLINE GSVector4 min(const GSVector4& v) const { return GSVector4(_mm_min_ps(m, v)); }

  ALWAYS_INLINE GSVector4 max(const GSVector4& v) const { return GSVector4(_mm_max_ps(m, v)); }

  template<int mask>
  ALWAYS_INLINE GSVector4 blend32(const GSVector4& v) const
  {
    return GSVector4(_mm_blend_ps(m, v, mask));
  }

  ALWAYS_INLINE GSVector4 blend32(const GSVector4& v, const GSVector4& mask) const
  {
    return GSVector4(_mm_blendv_ps(m, v, mask));
  }

  ALWAYS_INLINE GSVector4 upl(const GSVector4& v) const { return GSVector4(_mm_unpacklo_ps(m, v)); }

  ALWAYS_INLINE GSVector4 uph(const GSVector4& v) const { return GSVector4(_mm_unpackhi_ps(m, v)); }

  ALWAYS_INLINE GSVector4 upld(const GSVector4& v) const
  {
    return GSVector4(_mm_castpd_ps(_mm_unpacklo_pd(_mm_castps_pd(m), _mm_castps_pd(v.m))));
  }

  ALWAYS_INLINE GSVector4 uphd(const GSVector4& v) const
  {
    return GSVector4(_mm_castpd_ps(_mm_unpackhi_pd(_mm_castps_pd(m), _mm_castps_pd(v.m))));
  }

  ALWAYS_INLINE GSVector4 l2h(const GSVector4& v) const { return GSVector4(_mm_movelh_ps(m, v)); }

  ALWAYS_INLINE GSVector4 h2l(const GSVector4& v) const { return GSVector4(_mm_movehl_ps(m, v)); }

  ALWAYS_INLINE GSVector4 andnot(const GSVector4& v) const { return GSVector4(_mm_andnot_ps(v.m, m)); }

  ALWAYS_INLINE int mask() const { return _mm_movemask_ps(m); }

  ALWAYS_INLINE bool alltrue() const { return mask() == 0xf; }

  ALWAYS_INLINE bool allfalse() const
  {
#ifdef CPU_ARCH_AVX2
    return _mm_testz_ps(m, m) != 0;
#else
    const __m128i ii = _mm_castps_si128(m);
    return _mm_testz_si128(ii, ii) != 0;
#endif
  }

  ALWAYS_INLINE GSVector4 replace_nan(const GSVector4& v) const { return v.blend32(*this, *this == *this); }

  template<int src, int dst>
  ALWAYS_INLINE GSVector4 insert32(const GSVector4& v) const
  {
    if constexpr (src == dst)
      return GSVector4(_mm_blend_ps(m, v.m, 1 << src));
    else
      return GSVector4(_mm_insert_ps(m, v.m, _MM_MK_INSERTPS_NDX(src, dst, 0)));
  }

  template<int i>
  ALWAYS_INLINE int extract32() const
  {
    return _mm_extract_ps(m, i);
  }

  template<int dst>
  ALWAYS_INLINE GSVector4 insert64(double v) const
  {
    if constexpr (dst == 0)
      return GSVector4(_mm_move_sd(_mm_castps_pd(m), _mm_load_pd(&v)));
    else
      return GSVector4(_mm_shuffle_pd(_mm_castps_pd(m), _mm_load_pd(&v), 0));
  }

  template<int src>
  ALWAYS_INLINE double extract64() const
  {
    double ret;
    if constexpr (src == 0)
      _mm_storel_pd(&ret, _mm_castps_pd(m));
    else
      _mm_storeh_pd(&ret, _mm_castps_pd(m));
    return ret;
  }

  ALWAYS_INLINE static GSVector4 zero() { return GSVector4(_mm_setzero_ps()); }
  ALWAYS_INLINE static GSVector4 cast(const GSVector4i& v);

  ALWAYS_INLINE static GSVector4 xffffffff() { return zero() == zero(); }

  ALWAYS_INLINE static GSVector4 loadl(const void* p)
  {
    return GSVector4(_mm_castpd_ps(_mm_load_sd(static_cast<const double*>(p))));
  }

  ALWAYS_INLINE static GSVector4 load(float f) { return GSVector4(_mm_load_ss(&f)); }

  template<bool aligned>
  ALWAYS_INLINE static GSVector4 load(const void* p)
  {
    return GSVector4(aligned ? _mm_load_ps(static_cast<const float*>(p)) : _mm_loadu_ps(static_cast<const float*>(p)));
  }

  ALWAYS_INLINE static void storent(void* p, const GSVector4& v) { _mm_stream_ps(static_cast<float*>(p), v.m); }
  ALWAYS_INLINE static void storel(void* p, const GSVector4& v)
  {
    _mm_store_sd(static_cast<double*>(p), _mm_castps_pd(v.m));
  }
  ALWAYS_INLINE static void storeh(void* p, const GSVector4& v)
  {
    _mm_storeh_pd(static_cast<double*>(p), _mm_castps_pd(v.m));
  }

  template<bool aligned>
  ALWAYS_INLINE static void store(void* p, const GSVector4& v)
  {
    if constexpr (aligned)
      _mm_store_ps(static_cast<float*>(p), v.m);
    else
      _mm_storeu_ps(static_cast<float*>(p), v.m);
  }

  ALWAYS_INLINE static void store(float* p, const GSVector4& v) { _mm_store_ss(p, v.m); }

  ALWAYS_INLINE GSVector4 operator-() const { return neg(); }

  ALWAYS_INLINE GSVector4& operator+=(const GSVector4& v_)
  {
    m = _mm_add_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator-=(const GSVector4& v_)
  {
    m = _mm_sub_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator*=(const GSVector4& v_)
  {
    m = _mm_mul_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator/=(const GSVector4& v_)
  {
    m = _mm_div_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator+=(float f)
  {
    *this += GSVector4(f);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator-=(float f)
  {
    *this -= GSVector4(f);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator*=(float f)
  {
    *this *= GSVector4(f);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator/=(float f)
  {
    *this /= GSVector4(f);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator&=(const GSVector4& v_)
  {
    m = _mm_and_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator|=(const GSVector4& v_)
  {
    m = _mm_or_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE GSVector4& operator^=(const GSVector4& v_)
  {
    m = _mm_xor_ps(m, v_);
    return *this;
  }

  ALWAYS_INLINE friend GSVector4 operator+(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_add_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator-(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_sub_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator*(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_mul_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator/(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_div_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator+(const GSVector4& v, float f) { return v + GSVector4(f); }

  ALWAYS_INLINE friend GSVector4 operator-(const GSVector4& v, float f) { return v - GSVector4(f); }

  ALWAYS_INLINE friend GSVector4 operator*(const GSVector4& v, float f) { return v * GSVector4(f); }

  ALWAYS_INLINE friend GSVector4 operator/(const GSVector4& v, float f) { return v / GSVector4(f); }

  ALWAYS_INLINE friend GSVector4 operator&(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_and_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator|(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_or_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator^(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_xor_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator==(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_cmpeq_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator!=(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_cmpneq_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator>(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_cmpgt_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator<(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_cmplt_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator>=(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_cmpge_ps(v1, v2));
  }

  ALWAYS_INLINE friend GSVector4 operator<=(const GSVector4& v1, const GSVector4& v2)
  {
    return GSVector4(_mm_cmple_ps(v1, v2));
  }

  ALWAYS_INLINE GSVector4 mul64(const GSVector4& v_) const
  {
    return GSVector4(_mm_mul_pd(_mm_castps_pd(m), _mm_castps_pd(v_.m)));
  }

  ALWAYS_INLINE GSVector4 add64(const GSVector4& v_) const
  {
    return GSVector4(_mm_add_pd(_mm_castps_pd(m), _mm_castps_pd(v_.m)));
  }

  ALWAYS_INLINE GSVector4 sub64(const GSVector4& v_) const
  {
    return GSVector4(_mm_sub_pd(_mm_castps_pd(m), _mm_castps_pd(v_.m)));
  }

  ALWAYS_INLINE GSVector4 div64(const GSVector4& v_) const
  {
    return GSVector4(_mm_div_pd(_mm_castps_pd(m), _mm_castps_pd(v_.m)));
  }

  ALWAYS_INLINE GSVector4 gt64(const GSVector4& v2) const
  {
    return GSVector4(_mm_cmpgt_pd(_mm_castps_pd(m), _mm_castps_pd(v2.m)));
  }

  ALWAYS_INLINE GSVector4 eq64(const GSVector4& v2) const
  {
    return GSVector4(_mm_cmpeq_pd(_mm_castps_pd(m), _mm_castps_pd(v2.m)));
  }

  ALWAYS_INLINE GSVector4 lt64(const GSVector4& v2) const
  {
    return GSVector4(_mm_cmplt_pd(_mm_castps_pd(m), _mm_castps_pd(v2.m)));
  }

  ALWAYS_INLINE GSVector4 ge64(const GSVector4& v2) const
  {
    return GSVector4(_mm_cmpge_pd(_mm_castps_pd(m), _mm_castps_pd(v2.m)));
  }

  ALWAYS_INLINE GSVector4 le64(const GSVector4& v2) const
  {
    return GSVector4(_mm_cmple_pd(_mm_castps_pd(m), _mm_castps_pd(v2.m)));
  }

  ALWAYS_INLINE GSVector4 min64(const GSVector4& v2) const
  {
    return GSVector4(_mm_min_pd(_mm_castps_pd(m), _mm_castps_pd(v2.m)));
  }

  ALWAYS_INLINE GSVector4 max64(const GSVector4& v2) const
  {
    return GSVector4(_mm_max_pd(_mm_castps_pd(m), _mm_castps_pd(v2.m)));
  }

  ALWAYS_INLINE GSVector4 abs64() const { return *this & GSVector4::cxpr64(static_cast<u64>(0x7FFFFFFFFFFFFFFFULL)); }

  ALWAYS_INLINE GSVector4 neg64() const { return *this ^ GSVector4::cxpr64(static_cast<u64>(0x8000000000000000ULL)); }

  ALWAYS_INLINE GSVector4 sqrt64() const { return GSVector4(_mm_sqrt_pd(_mm_castps_pd(m))); }

  ALWAYS_INLINE GSVector4 sqr64() const { return GSVector4(_mm_mul_pd(_mm_castps_pd(m), _mm_castps_pd(m))); }

  ALWAYS_INLINE GSVector4 floor64() const
  {
    return GSVector4(_mm_round_pd(_mm_castps_pd(m), _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
  }

  ALWAYS_INLINE static GSVector4 f32to64(const GSVector4& v_) { return GSVector4(_mm_cvtps_pd(v_.m)); }

  ALWAYS_INLINE static GSVector4 f32to64(const void* p)
  {
    return GSVector4(_mm_cvtps_pd(_mm_castpd_ps(_mm_load_sd(static_cast<const double*>(p)))));
  }

  ALWAYS_INLINE GSVector4i f64toi32() const { return GSVector4i(_mm_cvttpd_epi32(_mm_castps_pd(m))); }

  // clang-format off

#define VECTOR4_SHUFFLE_4(xs, xn, ys, yn, zs, zn, ws, wn) \
    ALWAYS_INLINE GSVector4 xs##ys##zs##ws() const { return GSVector4(_mm_shuffle_ps(m, m, _MM_SHUFFLE(wn, zn, yn, xn))); } \
    ALWAYS_INLINE GSVector4 xs##ys##zs##ws(const GSVector4& v_) const { return GSVector4(_mm_shuffle_ps(m, v_.m, _MM_SHUFFLE(wn, zn, yn, xn))); } \

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

#if CPU_ARCH_AVX2

  ALWAYS_INLINE GSVector4 broadcast32() const { return GSVector4(_mm_broadcastss_ps(m)); }

  ALWAYS_INLINE static GSVector4 broadcast32(const GSVector4& v) { return GSVector4(_mm_broadcastss_ps(v.m)); }

  ALWAYS_INLINE static GSVector4 broadcast32(const void* f)
  {
    return GSVector4(_mm_broadcastss_ps(_mm_load_ss(static_cast<const float*>(f))));
  }

#endif

  ALWAYS_INLINE static GSVector4 broadcast64(const void* d)
  {
    return GSVector4(_mm_loaddup_pd(static_cast<const double*>(d)));
  }
};

ALWAYS_INLINE GSVector2i::GSVector2i(const GSVector2& v)
{
  m = _mm_cvttps_epi32(v);
}

ALWAYS_INLINE GSVector2::GSVector2(const GSVector2i& v)
{
  m = _mm_cvtepi32_ps(v);
}

ALWAYS_INLINE GSVector2i GSVector2i::cast(const GSVector2& v)
{
  return GSVector2i(_mm_castps_si128(v.m));
}

ALWAYS_INLINE GSVector2 GSVector2::cast(const GSVector2i& v)
{
  return GSVector2(_mm_castsi128_ps(v.m));
}

ALWAYS_INLINE GSVector4i::GSVector4i(const GSVector4& v)
{
  m = _mm_cvttps_epi32(v);
}

ALWAYS_INLINE GSVector4::GSVector4(const GSVector4i& v)
{
  m = _mm_cvtepi32_ps(v);
}

ALWAYS_INLINE GSVector4i GSVector4i::cast(const GSVector4& v)
{
  return GSVector4i(_mm_castps_si128(v.m));
}

ALWAYS_INLINE GSVector4 GSVector4::cast(const GSVector4i& v)
{
  return GSVector4(_mm_castsi128_ps(v.m));
}
