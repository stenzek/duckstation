#include "gpu_sw.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/cpu_detect.h"
#include "common/log.h"
#include "common/make_array.h"
#include "host_display.h"
#include "system.h"
#include <algorithm>
Log_SetChannel(GPU_SW);

#if defined(CPU_X64)
#include <emmintrin.h>
#elif defined(CPU_AARCH64)
#ifdef _MSC_VER
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

GPU_SW::GPU_SW()
{
  m_vram.fill(0);
  m_vram_ptr = m_vram.data();
}

GPU_SW::~GPU_SW()
{
  if (m_host_display)
    m_host_display->ClearDisplayTexture();
}

bool GPU_SW::IsHardwareRenderer() const
{
  return false;
}

bool GPU_SW::Initialize(HostDisplay* host_display)
{
  if (!GPU::Initialize(host_display))
    return false;

  static constexpr auto formats_for_16bit = make_array(HostDisplayPixelFormat::RGB565, HostDisplayPixelFormat::RGBA5551,
                                                       HostDisplayPixelFormat::RGBA8, HostDisplayPixelFormat::BGRA8);
  static constexpr auto formats_for_24bit =
    make_array(HostDisplayPixelFormat::RGBA8, HostDisplayPixelFormat::BGRA8, HostDisplayPixelFormat::RGB565,
               HostDisplayPixelFormat::RGBA5551);
  for (const HostDisplayPixelFormat format : formats_for_16bit)
  {
    if (m_host_display->SupportsDisplayPixelFormat(format))
    {
      m_16bit_display_format = format;
      break;
    }
  }
  for (const HostDisplayPixelFormat format : formats_for_24bit)
  {
    if (m_host_display->SupportsDisplayPixelFormat(format))
    {
      m_24bit_display_format = format;
      break;
    }
  }

  return true;
}

void GPU_SW::Reset()
{
  GPU::Reset();

  m_vram.fill(0);
}

template<HostDisplayPixelFormat out_format, typename out_type>
static void CopyOutRow16(const u16* src_ptr, out_type* dst_ptr, u32 width);

template<HostDisplayPixelFormat out_format, typename out_type>
static out_type VRAM16ToOutput(u16 value);

template<>
ALWAYS_INLINE u16 VRAM16ToOutput<HostDisplayPixelFormat::RGBA5551, u16>(u16 value)
{
  return (value & 0x3E0) | ((value >> 10) & 0x1F) | ((value & 0x1F) << 10);
}

template<>
ALWAYS_INLINE u16 VRAM16ToOutput<HostDisplayPixelFormat::RGB565, u16>(u16 value)
{
  return ((value & 0x3E0) << 1) | ((value & 0x20) << 1) | ((value >> 10) & 0x1F) | ((value & 0x1F) << 11);
}

template<>
ALWAYS_INLINE u32 VRAM16ToOutput<HostDisplayPixelFormat::RGBA8, u32>(u16 value)
{
  u8 r = Truncate8(value & 31);
  u8 g = Truncate8((value >> 5) & 31);
  u8 b = Truncate8((value >> 10) & 31);

  // 00012345 -> 1234545
  b = (b << 3) | (b & 0b111);
  g = (g << 3) | (g & 0b111);
  r = (r << 3) | (r & 0b111);

  return ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16) | (0xFF000000u);
}

template<>
ALWAYS_INLINE u32 VRAM16ToOutput<HostDisplayPixelFormat::BGRA8, u32>(u16 value)
{
  u8 r = Truncate8(value & 31);
  u8 g = Truncate8((value >> 5) & 31);
  u8 b = Truncate8((value >> 10) & 31);

  // 00012345 -> 1234545
  b = (b << 3) | (b & 0b111);
  g = (g << 3) | (g & 0b111);
  r = (r << 3) | (r & 0b111);

  return ZeroExtend32(b) | (ZeroExtend32(g) << 8) | (ZeroExtend32(r) << 16) | (0xFF000000u);
}

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::RGBA5551, u16>(const u16* src_ptr, u16* dst_ptr, u32 width)
{
  u32 col = 0;

#if defined(CPU_X64)
  const u32 aligned_width = Common::AlignDownPow2(width, 8);
  for (; col < aligned_width; col += 8)
  {
    const __m128i single_mask = _mm_set1_epi16(0x1F);
    __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_ptr));
    src_ptr += 8;
    __m128i a = _mm_and_si128(value, _mm_set1_epi16(static_cast<s16>(static_cast<u16>(0x3E0))));
    __m128i b = _mm_and_si128(_mm_srli_epi16(value, 10), single_mask);
    __m128i c = _mm_slli_epi16(_mm_and_si128(value, single_mask), 10);
    value = _mm_or_si128(_mm_or_si128(a, b), c);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr), value);
    dst_ptr += 8;
  }
#elif defined(CPU_AARCH64)
  const u32 aligned_width = Common::AlignDownPow2(width, 8);
  for (; col < aligned_width; col += 8)
  {
    const uint16x8_t single_mask = vdupq_n_u16(0x1F);
    uint16x8_t value = vld1q_u16(src_ptr);
    src_ptr += 8;
    uint16x8_t a = vandq_u16(value, vdupq_n_u16(0x3E0));
    uint16x8_t b = vandq_u16(vshrq_n_u16(value, 10), single_mask);
    uint16x8_t c = vshlq_n_u16(vandq_u16(value, single_mask), 10);
    value = vorrq_u16(vorrq_u16(a, b), c);
    vst1q_u16(dst_ptr, value);
    dst_ptr += 8;
  }
#endif

  for (; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::RGBA5551, u16>(*(src_ptr++));
}

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::RGB565, u16>(const u16* src_ptr, u16* dst_ptr, u32 width)
{
  u32 col = 0;

#if defined(CPU_X64)
  const u32 aligned_width = Common::AlignDownPow2(width, 8);
  for (; col < aligned_width; col += 8)
  {
    const __m128i single_mask = _mm_set1_epi16(0x1F);
    __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_ptr));
    src_ptr += 8;
    __m128i a = _mm_slli_epi16(_mm_and_si128(value, _mm_set1_epi16(static_cast<s16>(static_cast<u16>(0x3E0)))), 1);
    __m128i b = _mm_slli_epi16(_mm_and_si128(value, _mm_set1_epi16(static_cast<s16>(static_cast<u16>(0x20)))), 1);
    __m128i c = _mm_and_si128(_mm_srli_epi16(value, 10), single_mask);
    __m128i d = _mm_slli_epi16(_mm_and_si128(value, single_mask), 11);
    value = _mm_or_si128(_mm_or_si128(_mm_or_si128(a, b), c), d);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr), value);
    dst_ptr += 8;
  }
#elif defined(CPU_AARCH64)
  const u32 aligned_width = Common::AlignDownPow2(width, 8);
  const uint16x8_t single_mask = vdupq_n_u16(0x1F);
  for (; col < aligned_width; col += 8)
  {
    uint16x8_t value = vld1q_u16(src_ptr);
    src_ptr += 8;
    uint16x8_t a = vshlq_n_u16(vandq_u16(value, vdupq_n_u16(0x3E0)), 1); // (value & 0x3E0) << 1
    uint16x8_t b = vshlq_n_u16(vandq_u16(value, vdupq_n_u16(0x20)), 1);  // (value & 0x20) << 1
    uint16x8_t c = vandq_u16(vshrq_n_u16(value, 10), single_mask);       // ((value >> 10) & 0x1F)
    uint16x8_t d = vshlq_n_u16(vandq_u16(value, single_mask), 11);       // ((value & 0x1F) << 11)
    value = vorrq_u16(vorrq_u16(vorrq_u16(a, b), c), d);
    vst1q_u16(dst_ptr, value);
    dst_ptr += 8;
  }
#endif

  for (; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::RGB565, u16>(*(src_ptr++));
}

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::RGBA8, u32>(const u16* src_ptr, u32* dst_ptr, u32 width)
{
  for (u32 col = 0; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::RGBA8, u32>(*(src_ptr++));
}

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::BGRA8, u32>(const u16* src_ptr, u32* dst_ptr, u32 width)
{
  for (u32 col = 0; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::BGRA8, u32>(*(src_ptr++));
}

template<HostDisplayPixelFormat display_format>
void GPU_SW::CopyOut15Bit(u32 src_x, u32 src_y, u32 width, u32 height, u32 field, bool interlaced, bool interleaved)
{
  u8* dst_ptr;
  u32 dst_stride;

  using OutputPixelType = std::conditional_t<
    display_format == HostDisplayPixelFormat::RGBA8 || display_format == HostDisplayPixelFormat::BGRA8, u32, u16>;

  if (!interlaced)
  {
    if (!m_host_display->BeginSetDisplayPixels(display_format, width, height, reinterpret_cast<void**>(&dst_ptr),
                                               &dst_stride))
    {
      return;
    }
  }
  else
  {
    dst_stride = Common::AlignUpPow2<u32>(width * sizeof(OutputPixelType), 4);
    dst_ptr = m_display_texture_buffer.data() + (field != 0 ? dst_stride : 0);
  }

  const u32 output_stride = dst_stride;
  const u8 interlaced_shift = BoolToUInt8(interlaced);
  const u8 interleaved_shift = BoolToUInt8(interleaved);

  // Fast path when not wrapping around.
  if ((src_x + width) <= VRAM_WIDTH && (src_y + height) <= VRAM_HEIGHT)
  {
    const u32 rows = height >> interlaced_shift;
    dst_stride <<= interlaced_shift;

    const u16* src_ptr = &m_vram[src_y * VRAM_WIDTH + src_x];
    const u32 src_step = VRAM_WIDTH << interleaved_shift;
    for (u32 row = 0; row < rows; row++)
    {
      CopyOutRow16<display_format>(src_ptr, reinterpret_cast<OutputPixelType*>(dst_ptr), width);
      src_ptr += src_step;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    const u32 rows = height >> interlaced_shift;
    dst_stride <<= interlaced_shift;

    const u32 end_x = src_x + width;
    for (u32 row = 0; row < rows; row++)
    {
      const u16* src_row_ptr = &m_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      OutputPixelType* dst_row_ptr = reinterpret_cast<OutputPixelType*>(dst_ptr);
      for (u32 col = src_x; col < end_x; col++)
      {
        *(dst_row_ptr++) = VRAM16ToOutput<display_format, OutputPixelType>(src_row_ptr[col % VRAM_WIDTH]);
        src_y += (1 << interleaved_shift);
        dst_ptr += dst_stride;
      }
    }
  }

  if (!interlaced)
  {
    m_host_display->EndSetDisplayPixels();
  }
  else
  {
    m_host_display->SetDisplayPixels(display_format, width, height, m_display_texture_buffer.data(), output_stride);
  }
}

void GPU_SW::CopyOut15Bit(HostDisplayPixelFormat display_format, u32 src_x, u32 src_y, u32 width, u32 height, u32 field,
                          bool interlaced, bool interleaved)
{
  switch (display_format)
  {
    case HostDisplayPixelFormat::RGBA5551:
      CopyOut15Bit<HostDisplayPixelFormat::RGBA5551>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::RGB565:
      CopyOut15Bit<HostDisplayPixelFormat::RGB565>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::RGBA8:
      CopyOut15Bit<HostDisplayPixelFormat::RGBA8>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::BGRA8:
      CopyOut15Bit<HostDisplayPixelFormat::BGRA8>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    default:
      break;
  }
}

template<HostDisplayPixelFormat display_format>
void GPU_SW::CopyOut24Bit(u32 src_x, u32 src_y, u32 skip_x, u32 width, u32 height, u32 field, bool interlaced,
                          bool interleaved)
{
  u8* dst_ptr;
  u32 dst_stride;

  using OutputPixelType = std::conditional_t<
    display_format == HostDisplayPixelFormat::RGBA8 || display_format == HostDisplayPixelFormat::BGRA8, u32, u16>;

  if (!interlaced)
  {
    if (!m_host_display->BeginSetDisplayPixels(display_format, width, height, reinterpret_cast<void**>(&dst_ptr),
                                               &dst_stride))
    {
      return;
    }
  }
  else
  {
    dst_stride = Common::AlignUpPow2<u32>(width * sizeof(OutputPixelType), 4);
    dst_ptr = m_display_texture_buffer.data() + (field != 0 ? dst_stride : 0);
  }

  const u32 output_stride = dst_stride;
  const u8 interlaced_shift = BoolToUInt8(interlaced);
  const u8 interleaved_shift = BoolToUInt8(interleaved);
  const u32 rows = height >> interlaced_shift;
  dst_stride <<= interlaced_shift;

  if ((src_x + width) <= VRAM_WIDTH && (src_y + (rows << interleaved_shift)) <= VRAM_HEIGHT)
  {
    const u8* src_ptr = reinterpret_cast<const u8*>(&m_vram[src_y * VRAM_WIDTH + src_x]) + (skip_x * 3);
    const u32 src_stride = (VRAM_WIDTH << interleaved_shift) * sizeof(u16);
    for (u32 row = 0; row < rows; row++)
    {
      if constexpr (display_format == HostDisplayPixelFormat::RGBA8)
      {
        const u8* src_row_ptr = src_ptr;
        u8* dst_row_ptr = reinterpret_cast<u8*>(dst_ptr);
        for (u32 col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = *(src_row_ptr++);
          *(dst_row_ptr++) = *(src_row_ptr++);
          *(dst_row_ptr++) = *(src_row_ptr++);
          *(dst_row_ptr++) = 0xFF;
        }
      }
      else if constexpr (display_format == HostDisplayPixelFormat::BGRA8)
      {
        const u8* src_row_ptr = src_ptr;
        u8* dst_row_ptr = reinterpret_cast<u8*>(dst_ptr);
        for (u32 col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = src_row_ptr[2];
          *(dst_row_ptr++) = src_row_ptr[1];
          *(dst_row_ptr++) = src_row_ptr[0];
          *(dst_row_ptr++) = 0xFF;
          src_row_ptr += 3;
        }
      }
      else if constexpr (display_format == HostDisplayPixelFormat::RGB565)
      {
        const u8* src_row_ptr = src_ptr;
        u16* dst_row_ptr = reinterpret_cast<u16*>(dst_ptr);
        for (u32 col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = ((static_cast<u16>(src_row_ptr[0]) >> 3) << 11) |
                             ((static_cast<u16>(src_row_ptr[1]) >> 2) << 5) | (static_cast<u16>(src_row_ptr[2]) >> 3);
          src_row_ptr += 3;
        }
      }
      else if constexpr (display_format == HostDisplayPixelFormat::RGBA5551)
      {
        const u8* src_row_ptr = src_ptr;
        u16* dst_row_ptr = reinterpret_cast<u16*>(dst_ptr);
        for (u32 col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = ((static_cast<u16>(src_row_ptr[0]) >> 3) << 10) |
                             ((static_cast<u16>(src_row_ptr[1]) >> 3) << 5) | (static_cast<u16>(src_row_ptr[2]) >> 3);
          src_row_ptr += 3;
        }
      }

      src_ptr += src_stride;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    for (u32 row = 0; row < rows; row++)
    {
      const u16* src_row_ptr = &m_vram[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      OutputPixelType* dst_row_ptr = reinterpret_cast<OutputPixelType*>(dst_ptr);

      for (u32 col = 0; col < width; col++)
      {
        const u32 offset = (src_x + (((skip_x + col) * 3) / 2));
        const u16 s0 = src_row_ptr[offset % VRAM_WIDTH];
        const u16 s1 = src_row_ptr[(offset + 1) % VRAM_WIDTH];
        const u8 shift = static_cast<u8>(col & 1u) * 8;
        const u32 rgb = (((ZeroExtend32(s1) << 16) | ZeroExtend32(s0)) >> shift);

        if constexpr (display_format == HostDisplayPixelFormat::RGBA8)
        {
          *(dst_row_ptr++) = rgb | 0xFF000000u;
        }
        else if constexpr (display_format == HostDisplayPixelFormat::BGRA8)
        {
          *(dst_row_ptr++) = (rgb & 0x00FF00) | ((rgb & 0xFF) << 16) | ((rgb >> 16) & 0xFF) | 0xFF000000u;
        }
        else if constexpr (display_format == HostDisplayPixelFormat::RGB565)
        {
          *(dst_row_ptr++) = ((rgb >> 3) & 0x1F) | (((rgb >> 10) << 5) & 0x7E0) | (((rgb >> 19) << 11) & 0x3E0000);
        }
        else if constexpr (display_format == HostDisplayPixelFormat::RGBA5551)
        {
          *(dst_row_ptr++) = ((rgb >> 3) & 0x1F) | (((rgb >> 11) << 5) & 0x3E0) | (((rgb >> 19) << 10) & 0x1F0000);
        }
      }

      src_y += (1 << interleaved_shift);
      dst_ptr += dst_stride;
    }
  }

  if (!interlaced)
  {
    m_host_display->EndSetDisplayPixels();
  }
  else
  {
    m_host_display->SetDisplayPixels(display_format, width, height, m_display_texture_buffer.data(), output_stride);
  }
}

void GPU_SW::CopyOut24Bit(HostDisplayPixelFormat display_format, u32 src_x, u32 src_y, u32 skip_x, u32 width,
                          u32 height, u32 field, bool interlaced, bool interleaved)
{
  switch (display_format)
  {
    case HostDisplayPixelFormat::RGBA5551:
      CopyOut24Bit<HostDisplayPixelFormat::RGBA5551>(src_x, src_y, skip_x, width, height, field, interlaced,
                                                     interleaved);
      break;
    case HostDisplayPixelFormat::RGB565:
      CopyOut24Bit<HostDisplayPixelFormat::RGB565>(src_x, src_y, skip_x, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::RGBA8:
      CopyOut24Bit<HostDisplayPixelFormat::RGBA8>(src_x, src_y, skip_x, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::BGRA8:
      CopyOut24Bit<HostDisplayPixelFormat::BGRA8>(src_x, src_y, skip_x, width, height, field, interlaced, interleaved);
      break;
    default:
      break;
  }
}

void GPU_SW::ClearDisplay()
{
  std::memset(m_display_texture_buffer.data(), 0, m_display_texture_buffer.size());
}

void GPU_SW::UpdateDisplay()
{
  // fill display texture
  if (!g_settings.debugging.show_vram)
  {
    if (IsDisplayDisabled())
    {
      m_host_display->ClearDisplayTexture();
      return;
    }

    const u32 vram_offset_y = m_crtc_state.display_vram_top;
    const u32 display_width = m_crtc_state.display_vram_width;
    const u32 display_height = m_crtc_state.display_vram_height;

    if (IsInterlacedDisplayEnabled())
    {
      const u32 field = GetInterlacedDisplayField();
      if (m_GPUSTAT.display_area_color_depth_24)
      {
        CopyOut24Bit(m_24bit_display_format, m_crtc_state.regs.X, vram_offset_y + field,
                     m_crtc_state.display_vram_left - m_crtc_state.regs.X, display_width, display_height, field, true,
                     m_GPUSTAT.vertical_resolution);
      }
      else
      {
        CopyOut15Bit(m_16bit_display_format, m_crtc_state.display_vram_left, vram_offset_y + field, display_width,
                     display_height, field, true, m_GPUSTAT.vertical_resolution);
      }
    }
    else
    {
      if (m_GPUSTAT.display_area_color_depth_24)
      {
        CopyOut24Bit(m_24bit_display_format, m_crtc_state.regs.X, vram_offset_y,
                     m_crtc_state.display_vram_left - m_crtc_state.regs.X, display_width, display_height, 0, false,
                     false);
      }
      else
      {
        CopyOut15Bit(m_16bit_display_format, m_crtc_state.display_vram_left, vram_offset_y, display_width,
                     display_height, 0, false, false);
      }
    }

    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         GetDisplayAspectRatio());
  }
  else
  {
    CopyOut15Bit(m_16bit_display_format, 0, 0, VRAM_WIDTH, VRAM_HEIGHT, 0, false, false);
    m_host_display->SetDisplayParameters(VRAM_WIDTH, VRAM_HEIGHT, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                                         static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT));
  }
}

void GPU_SW::DispatchRenderCommand()
{
  const GPURenderCommand rc{m_render_command.bits};
  const bool dithering_enable = rc.IsDitheringEnabled() && m_GPUSTAT.dither_enable;

  switch (rc.primitive)
  {
    case GPUPrimitive::Polygon:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;

      const u32 num_vertices = rc.quad_polygon ? 4 : 3;
      std::array<SWVertex, 4> vertices;
      for (u32 i = 0; i < num_vertices; i++)
      {
        SWVertex& vert = vertices[i];
        const u32 color_rgb = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        vert.r = Truncate8(color_rgb);
        vert.g = Truncate8(color_rgb >> 8);
        vert.b = Truncate8(color_rgb >> 16);

        const GPUVertexPosition vp{FifoPop()};
        vert.x = m_drawing_offset.x + vp.x;
        vert.y = m_drawing_offset.y + vp.y;

        if (textured)
        {
          std::tie(vert.u, vert.v) = UnpackTexcoord(Truncate16(FifoPop()));
        }
        else
        {
          vert.u = 0;
          vert.v = 0;
        }
      }

      if (!IsDrawingAreaIsValid())
        return;

      const DrawTriangleFunction DrawFunction = GetDrawTriangleFunction(
        rc.shading_enable, rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable, dithering_enable);

      (this->*DrawFunction)(&vertices[0], &vertices[1], &vertices[2]);
      if (num_vertices > 3)
        (this->*DrawFunction)(&vertices[2], &vertices[1], &vertices[3]);
    }
    break;

    case GPUPrimitive::Rectangle:
    {
      const auto [r, g, b] = UnpackColorRGB24(rc.color_for_first_vertex);
      const GPUVertexPosition vp{FifoPop()};
      const u32 texcoord_and_palette = rc.texture_enable ? FifoPop() : 0;
      const auto [texcoord_x, texcoord_y] = UnpackTexcoord(Truncate16(texcoord_and_palette));

      u32 width;
      u32 height;
      switch (rc.rectangle_size)
      {
        case GPUDrawRectangleSize::R1x1:
          width = 1;
          height = 1;
          break;
        case GPUDrawRectangleSize::R8x8:
          width = 8;
          height = 8;
          break;
        case GPUDrawRectangleSize::R16x16:
          width = 16;
          height = 16;
          break;
        default:
        {
          const u32 width_and_height = FifoPop();
          width = static_cast<u32>(width_and_height & VRAM_WIDTH_MASK);
          height = static_cast<u32>((width_and_height >> 16) & VRAM_HEIGHT_MASK);

          if (width >= MAX_PRIMITIVE_WIDTH || height >= MAX_PRIMITIVE_HEIGHT)
          {
            Log_DebugPrintf("Culling too-large rectangle: %d,%d %dx%d", vp.x.GetValue(), vp.y.GetValue(), width,
                            height);
            return;
          }
        }
        break;
      }

      if (!IsDrawingAreaIsValid())
        return;

      const DrawRectangleFunction DrawFunction =
        GetDrawRectangleFunction(rc.texture_enable, rc.raw_texture_enable, rc.transparency_enable);

      (this->*DrawFunction)(vp.x, vp.y, width, height, r, g, b, texcoord_x, texcoord_y);
    }
    break;

    case GPUPrimitive::Line:
    {
      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;

      const DrawLineFunction DrawFunction = GetDrawLineFunction(shaded, rc.transparency_enable, dithering_enable);

      std::array<SWVertex, 2> vertices = {};
      u32 buffer_pos = 0;

      // first vertex
      SWVertex* p0 = &vertices[0];
      SWVertex* p1 = &vertices[1];
      p0->SetPosition(GPUVertexPosition{rc.polyline ? m_blit_buffer[buffer_pos++] : Truncate32(FifoPop())},
                      m_drawing_offset.x, m_drawing_offset.y);
      p0->SetColorRGB24(first_color);

      // remaining vertices in line strip
      const u32 num_vertices = rc.polyline ? GetPolyLineVertexCount() : 2;
      for (u32 i = 1; i < num_vertices; i++)
      {
        if (rc.polyline)
        {
          p1->SetColorRGB24(shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : first_color);
          p1->SetPosition(GPUVertexPosition{m_blit_buffer[buffer_pos++]}, m_drawing_offset.x, m_drawing_offset.y);
        }
        else
        {
          p1->SetColorRGB24(shaded ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color);
          p1->SetPosition(GPUVertexPosition{Truncate32(FifoPop())}, m_drawing_offset.x, m_drawing_offset.y);
        }

        // down here because of the FIFO pops
        if (IsDrawingAreaIsValid())
          (this->*DrawFunction)(p0, p1);

        // swap p0/p1 so that the last vertex is used as the first for the next line
        std::swap(p0, p1);
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

constexpr GPU_SW::DitherLUT GPU_SW::ComputeDitherLUT()
{
  DitherLUT lut = {};
  for (u32 i = 0; i < DITHER_MATRIX_SIZE; i++)
  {
    for (u32 j = 0; j < DITHER_MATRIX_SIZE; j++)
    {
      for (s32 value = 0; value < DITHER_LUT_SIZE; value++)
      {
        const s32 dithered_value = (value + DITHER_MATRIX[i][j]) >> 3;
        lut[i][j][value] = static_cast<u8>((dithered_value < 0) ? 0 : ((dithered_value > 31) ? 31 : dithered_value));
      }
    }
  }
  return lut;
}

static constexpr GPU_SW::DitherLUT s_dither_lut = GPU_SW::ComputeDitherLUT();

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
void ALWAYS_INLINE_RELEASE GPU_SW::ShadePixel(u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x,
                                              u8 texcoord_y)
{
  VRAMPixel color;
  bool transparent;
  if constexpr (texture_enable)
  {
    // Apply texture window
    // TODO: Precompute the second half
    texcoord_x = (texcoord_x & m_draw_mode.texture_window_and_x) | m_draw_mode.texture_window_or_x;
    texcoord_y = (texcoord_y & m_draw_mode.texture_window_and_y) | m_draw_mode.texture_window_or_y;

    VRAMPixel texture_color;
    switch (m_draw_mode.mode_reg.texture_mode)
    {
      case GPUTextureMode::Palette4Bit:
      {
        const u16 palette_value = GetPixel((m_draw_mode.texture_page_x + ZeroExtend32(texcoord_x / 4)) % VRAM_WIDTH,
                                           (m_draw_mode.texture_page_y + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT);
        const u16 palette_index = (palette_value >> ((texcoord_x % 4) * 4)) & 0x0Fu;
        texture_color.bits = GetPixel((m_draw_mode.texture_palette_x + ZeroExtend32(palette_index)) % VRAM_WIDTH,
                                      m_draw_mode.texture_palette_y);
      }
      break;

      case GPUTextureMode::Palette8Bit:
      {
        const u16 palette_value = GetPixel((m_draw_mode.texture_page_x + ZeroExtend32(texcoord_x / 2)) % VRAM_WIDTH,
                                           (m_draw_mode.texture_page_y + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT);
        const u16 palette_index = (palette_value >> ((texcoord_x % 2) * 8)) & 0xFFu;
        texture_color.bits = GetPixel((m_draw_mode.texture_palette_x + ZeroExtend32(palette_index)) % VRAM_WIDTH,
                                      m_draw_mode.texture_palette_y);
      }
      break;

      default:
      {
        texture_color.bits = GetPixel((m_draw_mode.texture_page_x + ZeroExtend32(texcoord_x)) % VRAM_WIDTH,
                                      (m_draw_mode.texture_page_y + ZeroExtend32(texcoord_y)) % VRAM_HEIGHT);
      }
      break;
    }

    if (texture_color.bits == 0)
      return;

    transparent = texture_color.c;

    if constexpr (raw_texture_enable)
    {
      color.bits = texture_color.bits;
    }
    else
    {
      const u32 dither_y = (dithering_enable) ? (y & 3u) : 2u;
      const u32 dither_x = (dithering_enable) ? (x & 3u) : 3u;

      color.bits = (ZeroExtend16(s_dither_lut[dither_y][dither_x][(u16(texture_color.r) * u16(color_r)) >> 4]) << 0) |
                   (ZeroExtend16(s_dither_lut[dither_y][dither_x][(u16(texture_color.g) * u16(color_g)) >> 4]) << 5) |
                   (ZeroExtend16(s_dither_lut[dither_y][dither_x][(u16(texture_color.b) * u16(color_b)) >> 4]) << 10) |
                   (texture_color.bits & 0x8000u);
    }
  }
  else
  {
    transparent = true;

    const u32 dither_y = (dithering_enable) ? (y & 3u) : 2u;
    const u32 dither_x = (dithering_enable) ? (x & 3u) : 3u;

    color.bits = (ZeroExtend16(s_dither_lut[dither_y][dither_x][color_r]) << 0) |
                 (ZeroExtend16(s_dither_lut[dither_y][dither_x][color_g]) << 5) |
                 (ZeroExtend16(s_dither_lut[dither_y][dither_x][color_b]) << 10);
  }

  const VRAMPixel bg_color{GetPixel(static_cast<u32>(x), static_cast<u32>(y))};
  if constexpr (transparency_enable)
  {
    if (transparent)
    {
#define BLEND_AVERAGE(bg, fg) Truncate8(std::min<u32>((ZeroExtend32(bg) / 2) + (ZeroExtend32(fg) / 2), 0x1F))
#define BLEND_ADD(bg, fg) Truncate8(std::min<u32>(ZeroExtend32(bg) + ZeroExtend32(fg), 0x1F))
#define BLEND_SUBTRACT(bg, fg) Truncate8((bg > fg) ? ((bg) - (fg)) : 0)
#define BLEND_QUARTER(bg, fg) Truncate8(std::min<u32>(ZeroExtend32(bg) + ZeroExtend32(fg / 4), 0x1F))

#define BLEND_RGB(func)                                                                                                \
  color.Set(func(bg_color.r.GetValue(), color.r.GetValue()), func(bg_color.g.GetValue(), color.g.GetValue()),          \
            func(bg_color.b.GetValue(), color.b.GetValue()), color.c.GetValue())

      switch (m_draw_mode.mode_reg.transparency_mode)
      {
        case GPUTransparencyMode::HalfBackgroundPlusHalfForeground:
          BLEND_RGB(BLEND_AVERAGE);
          break;
        case GPUTransparencyMode::BackgroundPlusForeground:
          BLEND_RGB(BLEND_ADD);
          break;
        case GPUTransparencyMode::BackgroundMinusForeground:
          BLEND_RGB(BLEND_SUBTRACT);
          break;
        case GPUTransparencyMode::BackgroundPlusQuarterForeground:
          BLEND_RGB(BLEND_QUARTER);
          break;
        default:
          break;
      }

#undef BLEND_RGB

#undef BLEND_QUARTER
#undef BLEND_SUBTRACT
#undef BLEND_ADD
#undef BLEND_AVERAGE
    }
  }
  else
  {
    UNREFERENCED_VARIABLE(transparent);
  }

  const u16 mask_and = m_GPUSTAT.GetMaskAND();
  if ((bg_color.bits & mask_and) != 0)
    return;

  SetPixel(static_cast<u32>(x), static_cast<u32>(y), color.bits | m_GPUSTAT.GetMaskOR());
}

template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
void GPU_SW::DrawRectangle(s32 origin_x, s32 origin_y, u32 width, u32 height, u8 r, u8 g, u8 b, u8 origin_texcoord_x,
                           u8 origin_texcoord_y)
{
  const s32 start_x = TruncateGPUVertexPosition(m_drawing_offset.x + origin_x);
  const s32 start_y = TruncateGPUVertexPosition(m_drawing_offset.y + origin_y);

  {
    const u32 clip_left = static_cast<u32>(std::clamp<s32>(start_x, m_drawing_area.left, m_drawing_area.right));
    const u32 clip_right =
      static_cast<u32>(std::clamp<s32>(start_x + static_cast<s32>(width), m_drawing_area.left, m_drawing_area.right)) +
      1u;
    const u32 clip_top = static_cast<u32>(std::clamp<s32>(start_y, m_drawing_area.top, m_drawing_area.bottom));
    const u32 clip_bottom =
      static_cast<u32>(std::clamp<s32>(start_y + static_cast<s32>(height), m_drawing_area.top, m_drawing_area.bottom)) +
      1u;
    AddDrawRectangleTicks(clip_right - clip_left, clip_bottom - clip_top, texture_enable, transparency_enable);
  }

  for (u32 offset_y = 0; offset_y < height; offset_y++)
  {
    const s32 y = start_y + static_cast<s32>(offset_y);
    if (y < static_cast<s32>(m_drawing_area.top) || y > static_cast<s32>(m_drawing_area.bottom) ||
        (IsInterlacedRenderingEnabled() && GetActiveLineLSB() == (static_cast<u32>(y) & 1u)))
    {
      continue;
    }

    const u8 texcoord_y = Truncate8(ZeroExtend32(origin_texcoord_y) + offset_y);

    for (u32 offset_x = 0; offset_x < width; offset_x++)
    {
      const s32 x = start_x + static_cast<s32>(offset_x);
      if (x < static_cast<s32>(m_drawing_area.left) || x > static_cast<s32>(m_drawing_area.right))
        continue;

      const u8 texcoord_x = Truncate8(ZeroExtend32(origin_texcoord_x) + offset_x);

      ShadePixel<texture_enable, raw_texture_enable, transparency_enable, false>(
        static_cast<u32>(x), static_cast<u32>(y), r, g, b, texcoord_x, texcoord_y);
    }
  }
}

std::unique_ptr<GPU> GPU::CreateSoftwareRenderer()
{
  return std::make_unique<GPU_SW>();
}

//////////////////////////////////////////////////////////////////////////
// Polygon and line rasterization ported from Mednafen
//////////////////////////////////////////////////////////////////////////

#define COORD_FBS 12
#define COORD_MF_INT(n) ((n) << COORD_FBS)
#define COORD_POST_PADDING 12

static ALWAYS_INLINE_RELEASE s64 MakePolyXFP(s32 x)
{
  return ((u64)x << 32) + ((1ULL << 32) - (1 << 11));
}

static ALWAYS_INLINE_RELEASE s64 MakePolyXFPStep(s32 dx, s32 dy)
{
  s64 ret;
  s64 dx_ex = (u64)dx << 32;

  if (dx_ex < 0)
    dx_ex -= dy - 1;

  if (dx_ex > 0)
    dx_ex += dy - 1;

  ret = dx_ex / dy;

  return (ret);
}

static ALWAYS_INLINE_RELEASE s32 GetPolyXFP_Int(s64 xfp)
{
  return (xfp >> 32);
}

template<bool shading_enable, bool texture_enable>
bool ALWAYS_INLINE_RELEASE GPU_SW::CalcIDeltas(i_deltas& idl, const SWVertex* A, const SWVertex* B, const SWVertex* C)
{
#define CALCIS(x, y) (((B->x - A->x) * (C->y - B->y)) - ((C->x - B->x) * (B->y - A->y)))

  s32 denom = CALCIS(x, y);

  if (!denom)
    return false;

  if constexpr (shading_enable)
  {
    idl.dr_dx = (u32)(CALCIS(r, y) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
    idl.dr_dy = (u32)(CALCIS(x, r) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;

    idl.dg_dx = (u32)(CALCIS(g, y) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
    idl.dg_dy = (u32)(CALCIS(x, g) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;

    idl.db_dx = (u32)(CALCIS(b, y) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
    idl.db_dy = (u32)(CALCIS(x, b) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
  }

  if constexpr (texture_enable)
  {
    idl.du_dx = (u32)(CALCIS(u, y) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
    idl.du_dy = (u32)(CALCIS(x, u) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;

    idl.dv_dx = (u32)(CALCIS(v, y) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
    idl.dv_dy = (u32)(CALCIS(x, v) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
  }

  return true;

#undef CALCIS
}

template<bool shading_enable, bool texture_enable>
void ALWAYS_INLINE_RELEASE GPU_SW::AddIDeltas_DX(i_group& ig, const i_deltas& idl, u32 count /*= 1*/)
{
  if constexpr (shading_enable)
  {
    ig.r += idl.dr_dx * count;
    ig.g += idl.dg_dx * count;
    ig.b += idl.db_dx * count;
  }

  if constexpr (texture_enable)
  {
    ig.u += idl.du_dx * count;
    ig.v += idl.dv_dx * count;
  }
}

template<bool shading_enable, bool texture_enable>
void ALWAYS_INLINE_RELEASE GPU_SW::AddIDeltas_DY(i_group& ig, const i_deltas& idl, u32 count /*= 1*/)
{
  if constexpr (shading_enable)
  {
    ig.r += idl.dr_dy * count;
    ig.g += idl.dg_dy * count;
    ig.b += idl.db_dy * count;
  }

  if constexpr (texture_enable)
  {
    ig.u += idl.du_dy * count;
    ig.v += idl.dv_dy * count;
  }
}

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
         bool dithering_enable>
void GPU_SW::DrawSpan(s32 y, s32 x_start, s32 x_bound, i_group ig, const i_deltas& idl)
{
  if (IsInterlacedRenderingEnabled() && GetActiveLineLSB() == (static_cast<u32>(y) & 1u))
    return;

  s32 x_ig_adjust = x_start;
  s32 w = x_bound - x_start;
  s32 x = TruncateGPUVertexPosition(x_start);

  if (x < static_cast<s32>(m_drawing_area.left))
  {
    s32 delta = static_cast<s32>(m_drawing_area.left) - x;
    x_ig_adjust += delta;
    x += delta;
    w -= delta;
  }

  if ((x + w) > (static_cast<s32>(m_drawing_area.right) + 1))
    w = static_cast<s32>(m_drawing_area.right) + 1 - x;

  if (w <= 0)
    return;

  AddIDeltas_DX<shading_enable, texture_enable>(ig, idl, x_ig_adjust);
  AddIDeltas_DY<shading_enable, texture_enable>(ig, idl, y);

  do
  {
    const u32 r = ig.r >> (COORD_FBS + COORD_POST_PADDING);
    const u32 g = ig.g >> (COORD_FBS + COORD_POST_PADDING);
    const u32 b = ig.b >> (COORD_FBS + COORD_POST_PADDING);
    const u32 u = ig.u >> (COORD_FBS + COORD_POST_PADDING);
    const u32 v = ig.v >> (COORD_FBS + COORD_POST_PADDING);

    ShadePixel<texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
      static_cast<u32>(x), static_cast<u32>(y), Truncate8(r), Truncate8(g), Truncate8(b), Truncate8(u), Truncate8(v));

    x++;
    AddIDeltas_DX<shading_enable, texture_enable>(ig, idl);
  } while (--w > 0);
}

template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
         bool dithering_enable>
void GPU_SW::DrawTriangle(const SWVertex* v0, const SWVertex* v1, const SWVertex* v2)
{
  u32 core_vertex;
  {
    u32 cvtemp = 0;

    if (v1->x <= v0->x)
    {
      if (v2->x <= v1->x)
        cvtemp = (1 << 2);
      else
        cvtemp = (1 << 1);
    }
    else if (v2->x < v0->x)
      cvtemp = (1 << 2);
    else
      cvtemp = (1 << 0);

    if (v2->y < v1->y)
    {
      std::swap(v2, v1);
      cvtemp = ((cvtemp >> 1) & 0x2) | ((cvtemp << 1) & 0x4) | (cvtemp & 0x1);
    }

    if (v1->y < v0->y)
    {
      std::swap(v1, v0);
      cvtemp = ((cvtemp >> 1) & 0x1) | ((cvtemp << 1) & 0x2) | (cvtemp & 0x4);
    }

    if (v2->y < v1->y)
    {
      std::swap(v2, v1);
      cvtemp = ((cvtemp >> 1) & 0x2) | ((cvtemp << 1) & 0x4) | (cvtemp & 0x1);
    }

    core_vertex = cvtemp >> 1;
  }

  if (v0->y == v2->y)
    return;

  if (static_cast<u32>(std::abs(v2->x - v0->x)) >= MAX_PRIMITIVE_WIDTH ||
      static_cast<u32>(std::abs(v2->x - v1->x)) >= MAX_PRIMITIVE_WIDTH ||
      static_cast<u32>(std::abs(v1->x - v0->x)) >= MAX_PRIMITIVE_WIDTH ||
      static_cast<u32>(v2->y - v0->y) >= MAX_PRIMITIVE_HEIGHT)
  {
    return;
  }

  AddDrawTriangleTicks(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y, shading_enable, texture_enable, transparency_enable);

  s64 base_coord = MakePolyXFP(v0->x);
  s64 base_step = MakePolyXFPStep((v2->x - v0->x), (v2->y - v0->y));
  s64 bound_coord_us;
  s64 bound_coord_ls;
  bool right_facing;

  if (v1->y == v0->y)
  {
    bound_coord_us = 0;
    right_facing = (bool)(v1->x > v0->x);
  }
  else
  {
    bound_coord_us = MakePolyXFPStep((v1->x - v0->x), (v1->y - v0->y));
    right_facing = (bool)(bound_coord_us > base_step);
  }

  if (v2->y == v1->y)
    bound_coord_ls = 0;
  else
    bound_coord_ls = MakePolyXFPStep((v2->x - v1->x), (v2->y - v1->y));

  i_deltas idl;
  if (!CalcIDeltas<shading_enable, texture_enable>(idl, v0, v1, v2))
    return;

  const SWVertex* vertices[3] = {v0, v1, v2};

  i_group ig;
  if constexpr (texture_enable)
  {
    ig.u = (COORD_MF_INT(vertices[core_vertex]->u) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
    ig.v = (COORD_MF_INT(vertices[core_vertex]->v) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
  }

  ig.r = (COORD_MF_INT(vertices[core_vertex]->r) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
  ig.g = (COORD_MF_INT(vertices[core_vertex]->g) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
  ig.b = (COORD_MF_INT(vertices[core_vertex]->b) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;

  AddIDeltas_DX<shading_enable, texture_enable>(ig, idl, -vertices[core_vertex]->x);
  AddIDeltas_DY<shading_enable, texture_enable>(ig, idl, -vertices[core_vertex]->y);

  struct TriangleHalf
  {
    u64 x_coord[2];
    u64 x_step[2];

    s32 y_coord;
    s32 y_bound;

    bool dec_mode;
  } tripart[2];

  u32 vo = 0;
  u32 vp = 0;
  if (core_vertex != 0)
    vo = 1;
  if (core_vertex == 2)
    vp = 3;

  {
    TriangleHalf* tp = &tripart[vo];
    tp->y_coord = vertices[0 ^ vo]->y;
    tp->y_bound = vertices[1 ^ vo]->y;
    tp->x_coord[right_facing] = MakePolyXFP(vertices[0 ^ vo]->x);
    tp->x_step[right_facing] = bound_coord_us;
    tp->x_coord[!right_facing] = base_coord + ((vertices[vo]->y - vertices[0]->y) * base_step);
    tp->x_step[!right_facing] = base_step;
    tp->dec_mode = vo;
  }

  {
    TriangleHalf* tp = &tripart[vo ^ 1];
    tp->y_coord = vertices[1 ^ vp]->y;
    tp->y_bound = vertices[2 ^ vp]->y;
    tp->x_coord[right_facing] = MakePolyXFP(vertices[1 ^ vp]->x);
    tp->x_step[right_facing] = bound_coord_ls;
    tp->x_coord[!right_facing] =
      base_coord + ((vertices[1 ^ vp]->y - vertices[0]->y) *
                    base_step); // base_coord + ((vertices[1].y - vertices[0].y) * base_step);
    tp->x_step[!right_facing] = base_step;
    tp->dec_mode = vp;
  }

  for (u32 i = 0; i < 2; i++)
  {
    s32 yi = tripart[i].y_coord;
    s32 yb = tripart[i].y_bound;

    u64 lc = tripart[i].x_coord[0];
    u64 ls = tripart[i].x_step[0];

    u64 rc = tripart[i].x_coord[1];
    u64 rs = tripart[i].x_step[1];

    if (tripart[i].dec_mode)
    {
      while (yi > yb)
      {
        yi--;
        lc -= ls;
        rc -= rs;

        s32 y = TruncateGPUVertexPosition(yi);

        if (y < static_cast<s32>(m_drawing_area.top))
          break;

        if (y > static_cast<s32>(m_drawing_area.bottom))
          continue;

        DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
          yi, GetPolyXFP_Int(lc), GetPolyXFP_Int(rc), ig, idl);
      }
    }
    else
    {
      while (yi < yb)
      {
        s32 y = TruncateGPUVertexPosition(yi);

        if (y > static_cast<s32>(m_drawing_area.bottom))
          break;

        if (y >= static_cast<s32>(m_drawing_area.top))
        {

          DrawSpan<shading_enable, texture_enable, raw_texture_enable, transparency_enable, dithering_enable>(
            yi, GetPolyXFP_Int(lc), GetPolyXFP_Int(rc), ig, idl);
        }

        yi++;
        lc += ls;
        rc += rs;
      }
    }
  }
}

GPU_SW::DrawTriangleFunction GPU_SW::GetDrawTriangleFunction(bool shading_enable, bool texture_enable,
                                                             bool raw_texture_enable, bool transparency_enable,
                                                             bool dithering_enable)
{
#define F(SHADING, TEXTURE, RAW_TEXTURE, TRANSPARENCY, DITHERING)                                                      \
  &GPU_SW::DrawTriangle<SHADING, TEXTURE, RAW_TEXTURE, TRANSPARENCY, DITHERING>

  static constexpr DrawTriangleFunction funcs[2][2][2][2][2] = {
    {{{{F(false, false, false, false, false), F(false, false, false, false, true)},
       {F(false, false, false, true, false), F(false, false, false, true, true)}},
      {{F(false, false, true, false, false), F(false, false, true, false, true)},
       {F(false, false, true, true, false), F(false, false, true, true, true)}}},
     {{{F(false, true, false, false, false), F(false, true, false, false, true)},
       {F(false, true, false, true, false), F(false, true, false, true, true)}},
      {{F(false, true, true, false, false), F(false, true, true, false, true)},
       {F(false, true, true, true, false), F(false, true, true, true, true)}}}},
    {{{{F(true, false, false, false, false), F(true, false, false, false, true)},
       {F(true, false, false, true, false), F(true, false, false, true, true)}},
      {{F(true, false, true, false, false), F(true, false, true, false, true)},
       {F(true, false, true, true, false), F(true, false, true, true, true)}}},
     {{{F(true, true, false, false, false), F(true, true, false, false, true)},
       {F(true, true, false, true, false), F(true, true, false, true, true)}},
      {{F(true, true, true, false, false), F(true, true, true, false, true)},
       {F(true, true, true, true, false), F(true, true, true, true, true)}}}}};

#undef F

  return funcs[u8(shading_enable)][u8(texture_enable)][u8(raw_texture_enable)][u8(transparency_enable)]
              [u8(dithering_enable)];
}

enum
{
  Line_XY_FractBits = 32
};
enum
{
  Line_RGB_FractBits = 12
};

struct line_fxp_coord
{
  u64 x, y;
  u32 r, g, b;
};

struct line_fxp_step
{
  s64 dx_dk, dy_dk;
  s32 dr_dk, dg_dk, db_dk;
};

static ALWAYS_INLINE_RELEASE s64 LineDivide(s64 delta, s32 dk)
{
  delta = (u64)delta << Line_XY_FractBits;

  if (delta < 0)
    delta -= dk - 1;
  if (delta > 0)
    delta += dk - 1;

  return (delta / dk);
}

template<bool shading_enable, bool transparency_enable, bool dithering_enable>
void GPU_SW::DrawLine(const SWVertex* p0, const SWVertex* p1)
{
  const s32 i_dx = std::abs(p1->x - p0->x);
  const s32 i_dy = std::abs(p1->y - p0->y);
  const s32 k = (i_dx > i_dy) ? i_dx : i_dy;
  if (i_dx >= MAX_PRIMITIVE_WIDTH || i_dy >= MAX_PRIMITIVE_HEIGHT)
    return;

  {
    // TODO: Move to base class
    const u32 clip_left =
      static_cast<u32>(std::clamp<s32>(std::min(p0->x, p1->x), m_drawing_area.left, m_drawing_area.left));
    const u32 clip_right =
      static_cast<u32>(std::clamp<s32>(std::max(p0->x, p1->x), m_drawing_area.left, m_drawing_area.right)) + 1u;
    const u32 clip_top =
      static_cast<u32>(std::clamp<s32>(std::min(p0->y, p1->y), m_drawing_area.top, m_drawing_area.bottom));
    const u32 clip_bottom =
      static_cast<u32>(std::clamp<s32>(std::max(p0->y, p1->y), m_drawing_area.top, m_drawing_area.bottom)) + 1u;

    AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, shading_enable);
  }

  if (p0->x >= p1->x && k > 0)
    std::swap(p0, p1);

  line_fxp_step step;
  if (k == 0)
  {
    step.dx_dk = 0;
    step.dy_dk = 0;

    if constexpr (shading_enable)
    {
      step.dr_dk = 0;
      step.dg_dk = 0;
      step.db_dk = 0;
    }
  }
  else
  {
    step.dx_dk = LineDivide(p1->x - p0->x, k);
    step.dy_dk = LineDivide(p1->y - p0->y, k);

    if constexpr (shading_enable)
    {
      step.dr_dk = (s32)((u32)(p1->r - p0->r) << Line_RGB_FractBits) / k;
      step.dg_dk = (s32)((u32)(p1->g - p0->g) << Line_RGB_FractBits) / k;
      step.db_dk = (s32)((u32)(p1->b - p0->b) << Line_RGB_FractBits) / k;
    }
  }

  line_fxp_coord cur_point;
  cur_point.x = ((u64)p0->x << Line_XY_FractBits) | (1ULL << (Line_XY_FractBits - 1));
  cur_point.y = ((u64)p0->y << Line_XY_FractBits) | (1ULL << (Line_XY_FractBits - 1));

  cur_point.x -= 1024;

  if (step.dy_dk < 0)
    cur_point.y -= 1024;

  if constexpr (shading_enable)
  {
    cur_point.r = (p0->r << Line_RGB_FractBits) | (1 << (Line_RGB_FractBits - 1));
    cur_point.g = (p0->g << Line_RGB_FractBits) | (1 << (Line_RGB_FractBits - 1));
    cur_point.b = (p0->b << Line_RGB_FractBits) | (1 << (Line_RGB_FractBits - 1));
  }

  for (s32 i = 0; i <= k; i++)
  {
    // Sign extension is not necessary here for x and y, due to the maximum values that ClipX1 and ClipY1 can contain.
    const s32 x = (cur_point.x >> Line_XY_FractBits) & 2047;
    const s32 y = (cur_point.y >> Line_XY_FractBits) & 2047;

    if (!IsInterlacedRenderingEnabled() || GetActiveLineLSB() != (static_cast<u32>(y) & 1u))
    {
      const u8 r = shading_enable ? static_cast<u8>(cur_point.r >> Line_RGB_FractBits) : p0->r;
      const u8 g = shading_enable ? static_cast<u8>(cur_point.g >> Line_RGB_FractBits) : p0->g;
      const u8 b = shading_enable ? static_cast<u8>(cur_point.b >> Line_RGB_FractBits) : p0->b;

      if (x >= static_cast<s32>(m_drawing_area.left) && x <= static_cast<s32>(m_drawing_area.right) &&
          y >= static_cast<s32>(m_drawing_area.top) && y <= static_cast<s32>(m_drawing_area.bottom))
      {
        ShadePixel<false, false, transparency_enable, dithering_enable>(static_cast<u32>(x), static_cast<u32>(y), r, g,
                                                                        b, 0, 0);
      }
    }

    cur_point.x += step.dx_dk;
    cur_point.y += step.dy_dk;

    if constexpr (shading_enable)
    {
      cur_point.r += step.dr_dk;
      cur_point.g += step.dg_dk;
      cur_point.b += step.db_dk;
    }
  }
}

GPU_SW::DrawLineFunction GPU_SW::GetDrawLineFunction(bool shading_enable, bool transparency_enable,
                                                     bool dithering_enable)
{
#define F(SHADING, TRANSPARENCY, DITHERING) &GPU_SW::DrawLine<SHADING, TRANSPARENCY, DITHERING>

  static constexpr DrawLineFunction funcs[2][2][2] = {
    {{F(false, false, false), F(false, false, true)}, {F(false, true, false), F(false, true, true)}},
    {{F(true, false, false), F(true, false, true)}, {F(true, true, false), F(true, true, true)}}};

#undef F

  return funcs[u8(shading_enable)][u8(transparency_enable)][u8(dithering_enable)];
}

GPU_SW::DrawRectangleFunction GPU_SW::GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                               bool transparency_enable)
{
#define F(TEXTURE, RAW_TEXTURE, TRANSPARENCY) &GPU_SW::DrawRectangle<TEXTURE, RAW_TEXTURE, TRANSPARENCY>

  static constexpr DrawRectangleFunction funcs[2][2][2] = {
    {{F(false, false, false), F(false, false, true)}, {F(false, true, false), F(false, true, true)}},
    {{F(true, false, false), F(true, false, true)}, {F(true, true, false), F(true, true, true)}}};

#undef F

  return funcs[u8(texture_enable)][u8(raw_texture_enable)][u8(transparency_enable)];
}
