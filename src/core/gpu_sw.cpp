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

template<typename T>
ALWAYS_INLINE static constexpr std::tuple<T, T> MinMax(T v1, T v2)
{
  if (v1 > v2)
    return std::tie(v2, v1);
  else
    return std::tie(v1, v2);
}

GPU_SW::GPU_SW()
{
  m_vram_ptr = m_backend.GetVRAM();
}

GPU_SW::~GPU_SW()
{
  m_backend.Shutdown();
  if (m_host_display)
    m_host_display->ClearDisplayTexture();
}

bool GPU_SW::IsHardwareRenderer() const
{
  return false;
}

bool GPU_SW::Initialize(HostDisplay* host_display)
{
  if (!GPU::Initialize(host_display) || !m_backend.Initialize())
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

  m_backend.Reset();
}

void GPU_SW::UpdateSettings()
{
  GPU::UpdateSettings();
  m_backend.UpdateSettings();
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

    const u16* src_ptr = &m_vram_ptr[src_y * VRAM_WIDTH + src_x];
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
      const u16* src_row_ptr = &m_vram_ptr[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      OutputPixelType* dst_row_ptr = reinterpret_cast<OutputPixelType*>(dst_ptr);

      for (u32 col = src_x; col < end_x; col++)
        *(dst_row_ptr++) = VRAM16ToOutput<display_format, OutputPixelType>(src_row_ptr[col % VRAM_WIDTH]);

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
    const u8* src_ptr = reinterpret_cast<const u8*>(&m_vram_ptr[src_y * VRAM_WIDTH + src_x]) + (skip_x * 3);
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
      const u16* src_row_ptr = &m_vram_ptr[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
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
  m_backend.Sync();

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

void GPU_SW::FillBackendCommandParameters(GPUBackendCommand* cmd)
{
  cmd->params.bits = 0;
  cmd->params.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  cmd->params.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  cmd->params.active_line_lsb = m_crtc_state.active_line_lsb;
  cmd->params.interlaced_rendering = IsInterlacedRenderingEnabled();
}

void GPU_SW::FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc)
{
  FillBackendCommandParameters(cmd);
  cmd->rc.bits = rc.bits;
  cmd->draw_mode.bits = m_draw_mode.mode_reg.bits;
  cmd->palette.bits = m_draw_mode.palette_reg;
  cmd->window = m_draw_mode.texture_window;
}

void GPU_SW::DispatchRenderCommand()
{
  if (m_drawing_area_changed)
  {
    GPUBackendSetDrawingAreaCommand* cmd = m_backend.NewSetDrawingAreaCommand();
    cmd->new_area = m_drawing_area;
    m_backend.PushCommand(cmd);
    m_drawing_area_changed = false;
  }

  const GPURenderCommand rc{m_render_command.bits};
  const bool dithering_enable = rc.IsDitheringEnabled() && m_GPUSTAT.dither_enable;

  switch (rc.primitive)
  {
    case GPUPrimitive::Polygon:
    {
      const u32 num_vertices = rc.quad_polygon ? 4 : 3;
      GPUBackendDrawPolygonCommand* cmd = m_backend.NewDrawPolygonCommand(num_vertices);
      FillDrawCommand(cmd, rc);

      const u32 first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;
      for (u32 i = 0; i < num_vertices; i++)
      {
        GPUBackendDrawPolygonCommand::Vertex* vert = &cmd->vertices[i];
        vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        const u64 maddr_and_pos = m_fifo.Pop();
        const GPUVertexPosition vp{Truncate32(maddr_and_pos)};
        vert->x = m_drawing_offset.x + vp.x;
        vert->y = m_drawing_offset.y + vp.y;
        vert->texcoord = textured ? Truncate16(FifoPop()) : 0;
      }

      if (!IsDrawingAreaIsValid())
        return;

      // Cull polygons which are too large.
      const auto [min_x_12, max_x_12] = MinMax(cmd->vertices[1].x, cmd->vertices[2].x);
      const auto [min_y_12, max_y_12] = MinMax(cmd->vertices[1].y, cmd->vertices[2].y);
      const s32 min_x = std::min(min_x_12, cmd->vertices[0].x);
      const s32 max_x = std::max(max_x_12, cmd->vertices[0].x);
      const s32 min_y = std::min(min_y_12, cmd->vertices[0].y);
      const s32 max_y = std::max(max_y_12, cmd->vertices[0].y);

      if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
      {
        Log_DebugPrintf("Culling too-large polygon: %d,%d %d,%d %d,%d", cmd->vertices[0].x, cmd->vertices[0].y,
                        cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[2].x, cmd->vertices[2].y);
      }
      else
      {
        AddDrawTriangleTicks(cmd->vertices[0].x, cmd->vertices[0].y, cmd->vertices[1].x, cmd->vertices[1].y,
                             cmd->vertices[2].x, cmd->vertices[2].y, rc.shading_enable, rc.texture_enable,
                             rc.transparency_enable);
      }

      // quads
      if (rc.quad_polygon)
      {
        const s32 min_x_123 = std::min(min_x_12, cmd->vertices[3].x);
        const s32 max_x_123 = std::max(max_x_12, cmd->vertices[3].x);
        const s32 min_y_123 = std::min(min_y_12, cmd->vertices[3].y);
        const s32 max_y_123 = std::max(max_y_12, cmd->vertices[3].y);

        // Cull polygons which are too large.
        if ((max_x_123 - min_x_123) >= MAX_PRIMITIVE_WIDTH || (max_y_123 - min_y_123) >= MAX_PRIMITIVE_HEIGHT)
        {
          Log_DebugPrintf("Culling too-large polygon (quad second half): %d,%d %d,%d %d,%d", cmd->vertices[2].x,
                          cmd->vertices[2].y, cmd->vertices[1].x, cmd->vertices[1].y, cmd->vertices[0].x,
                          cmd->vertices[0].y);
        }
        else
        {
          AddDrawTriangleTicks(cmd->vertices[2].x, cmd->vertices[2].y, cmd->vertices[1].x, cmd->vertices[1].y,
                               cmd->vertices[3].x, cmd->vertices[3].y, rc.shading_enable, rc.texture_enable,
                               rc.transparency_enable);
        }
      }

      m_backend.PushCommand(cmd);
    }
    break;

    case GPUPrimitive::Rectangle:
    {
      GPUBackendDrawRectangleCommand* cmd = m_backend.NewDrawRectangleCommand();
      FillDrawCommand(cmd, rc);
      cmd->color = rc.color_for_first_vertex;

      const GPUVertexPosition vp{FifoPop()};
      cmd->x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
      cmd->y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

      if (rc.texture_enable)
      {
        const u32 texcoord_and_palette = FifoPop();
        cmd->palette.bits = Truncate16(texcoord_and_palette >> 16);
        cmd->texcoord = Truncate16(texcoord_and_palette);
      }
      else
      {
        cmd->palette.bits = 0;
        cmd->texcoord = 0;
      }

      switch (rc.rectangle_size)
      {
        case GPUDrawRectangleSize::R1x1:
          cmd->width = 1;
          cmd->height = 1;
          break;
        case GPUDrawRectangleSize::R8x8:
          cmd->width = 8;
          cmd->height = 8;
          break;
        case GPUDrawRectangleSize::R16x16:
          cmd->width = 16;
          cmd->height = 16;
          break;
        default:
        {
          const u32 width_and_height = FifoPop();
          cmd->width = static_cast<u16>(width_and_height & VRAM_WIDTH_MASK);
          cmd->height = static_cast<u16>((width_and_height >> 16) & VRAM_HEIGHT_MASK);

          if (cmd->width >= MAX_PRIMITIVE_WIDTH || cmd->height >= MAX_PRIMITIVE_HEIGHT)
          {
            Log_DebugPrintf("Culling too-large rectangle: %d,%d %dx%d", cmd->x, cmd->y, cmd->width, cmd->height);
            return;
          }
        }
        break;
      }

      if (!IsDrawingAreaIsValid())
        return;

      const u32 clip_left = static_cast<u32>(std::clamp<s32>(cmd->x, m_drawing_area.left, m_drawing_area.right));
      const u32 clip_right =
        static_cast<u32>(std::clamp<s32>(cmd->x + cmd->width, m_drawing_area.left, m_drawing_area.right)) + 1u;
      const u32 clip_top = static_cast<u32>(std::clamp<s32>(cmd->y, m_drawing_area.top, m_drawing_area.bottom));
      const u32 clip_bottom =
        static_cast<u32>(std::clamp<s32>(cmd->y + cmd->height, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

      // cmd->bounds.Set(Truncate16(clip_left), Truncate16(clip_top), Truncate16(clip_right), Truncate16(clip_bottom));
      AddDrawRectangleTicks(clip_right - clip_left, clip_bottom - clip_top, rc.texture_enable, rc.transparency_enable);

      m_backend.PushCommand(cmd);
    }
    break;

    case GPUPrimitive::Line:
    {
      if (!rc.polyline)
      {
        GPUBackendDrawLineCommand* cmd = m_backend.NewDrawLineCommand(2);
        FillDrawCommand(cmd, rc);
        cmd->palette.bits = 0;

        if (rc.shading_enable)
        {
          cmd->vertices[0].color = rc.color_for_first_vertex;
          const GPUVertexPosition start_pos{FifoPop()};
          cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
          cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

          cmd->vertices[1].color = FifoPop() & UINT32_C(0x00FFFFFF);
          const GPUVertexPosition end_pos{FifoPop()};
          cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
          cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
        }
        else
        {
          cmd->vertices[0].color = rc.color_for_first_vertex;
          cmd->vertices[1].color = rc.color_for_first_vertex;

          const GPUVertexPosition start_pos{FifoPop()};
          cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
          cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

          const GPUVertexPosition end_pos{FifoPop()};
          cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
          cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
        }

        if (!IsDrawingAreaIsValid())
          return;

        const auto [min_x, max_x] = MinMax(cmd->vertices[0].x, cmd->vertices[1].x);
        const auto [min_y, max_y] = MinMax(cmd->vertices[0].y, cmd->vertices[1].y);
        if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
        {
          Log_DebugPrintf("Culling too-large line: %d,%d - %d,%d", cmd->vertices[0].y, cmd->vertices[0].y,
                          cmd->vertices[1].x, cmd->vertices[1].y);
          return;
        }

        const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.right));
        const u32 clip_right = static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
        const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
        const u32 clip_bottom =
          static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;
        // cmd->bounds.Set(Truncate16(clip_left), Truncate16(clip_top), Truncate16(clip_right),
        // Truncate16(clip_bottom));
        AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable);

        m_backend.PushCommand(cmd);
      }
      else
      {
        const u32 num_vertices = GetPolyLineVertexCount();

        GPUBackendDrawLineCommand* cmd = m_backend.NewDrawLineCommand(num_vertices);
        FillDrawCommand(cmd, m_render_command);

        u32 buffer_pos = 0;
        const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
        cmd->vertices[0].x = start_vp.x + m_drawing_offset.x;
        cmd->vertices[0].y = start_vp.y + m_drawing_offset.y;
        cmd->vertices[0].color = m_render_command.color_for_first_vertex;
        // cmd->bounds.SetInvalid();

        const bool shaded = m_render_command.shading_enable;
        for (u32 i = 1; i < num_vertices; i++)
        {
          cmd->vertices[i].color =
            shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : m_render_command.color_for_first_vertex;
          const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
          cmd->vertices[i].x = m_drawing_offset.x + vp.x;
          cmd->vertices[i].y = m_drawing_offset.y + vp.y;

          const auto [min_x, max_x] = MinMax(cmd->vertices[i - 1].x, cmd->vertices[i].y);
          const auto [min_y, max_y] = MinMax(cmd->vertices[i - 1].x, cmd->vertices[i].y);
          if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
          {
            Log_DebugPrintf("Culling too-large line: %d,%d - %d,%d", cmd->vertices[i - 1].x, cmd->vertices[i - 1].y,
                            cmd->vertices[i].x, cmd->vertices[i].y);
          }
          else
          {
            const u32 clip_left = static_cast<u32>(std::clamp<s32>(min_x, m_drawing_area.left, m_drawing_area.right));
            const u32 clip_right =
              static_cast<u32>(std::clamp<s32>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
            const u32 clip_top = static_cast<u32>(std::clamp<s32>(min_y, m_drawing_area.top, m_drawing_area.bottom));
            const u32 clip_bottom =
              static_cast<u32>(std::clamp<s32>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

            // cmd->bounds.Include(Truncate16(clip_left), Truncate16(clip_right), Truncate16(clip_top),
            // Truncate16(clip_bottom));
            AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, m_render_command.shading_enable);
          }
        }

        m_backend.PushCommand(cmd);
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void GPU_SW::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  m_backend.Sync();
}

void GPU_SW::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  GPUBackendFillVRAMCommand* cmd = m_backend.NewFillVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->x = static_cast<u16>(x);
  cmd->y = static_cast<u16>(y);
  cmd->width = static_cast<u16>(width);
  cmd->height = static_cast<u16>(height);
  cmd->color = color;
  m_backend.PushCommand(cmd);
}

void GPU_SW::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  const u32 num_words = width * height;
  GPUBackendUpdateVRAMCommand* cmd = m_backend.NewUpdateVRAMCommand(num_words);
  FillBackendCommandParameters(cmd);
  cmd->params.set_mask_while_drawing = set_mask;
  cmd->params.check_mask_before_draw = check_mask;
  cmd->x = static_cast<u16>(x);
  cmd->y = static_cast<u16>(y);
  cmd->width = static_cast<u16>(width);
  cmd->height = static_cast<u16>(height);
  std::memcpy(cmd->data, data, sizeof(u16) * num_words);
  m_backend.PushCommand(cmd);
}

void GPU_SW::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  GPUBackendCopyVRAMCommand* cmd = m_backend.NewCopyVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->src_x = static_cast<u16>(src_x);
  cmd->src_y = static_cast<u16>(src_y);
  cmd->dst_x = static_cast<u16>(dst_x);
  cmd->dst_y = static_cast<u16>(dst_y);
  cmd->width = static_cast<u16>(width);
  cmd->height = static_cast<u16>(height);
  m_backend.PushCommand(cmd);
}

std::unique_ptr<GPU> GPU::CreateSoftwareRenderer()
{
  return std::make_unique<GPU_SW>();
}
