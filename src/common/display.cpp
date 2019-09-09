#include "display.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Math.h"
#include "display_renderer.h"
#include <algorithm>
#include <cstring>

Display::Display(DisplayRenderer* manager, const String& name, Type type, u8 priority)
  : m_renderer(manager), m_name(name), m_type(type), m_priority(priority)
{
}

Display::~Display()
{
  m_renderer->RemoveDisplay(this);
  DestroyFramebuffer(&m_front_buffer);
  DestroyFramebuffer(&m_back_buffers[0]);
  DestroyFramebuffer(&m_back_buffers[1]);
}

void Display::SetEnable(bool enabled)
{
  std::lock_guard<std::mutex> guard(m_buffer_lock);
  if (m_enabled == enabled)
    return;

  m_enabled = enabled;
  if (enabled)
    m_renderer->DisplayEnabled(this);
  else
    m_renderer->DisplayDisabled(this);
}

void Display::SetActive(bool active)
{
  m_active = active;
}

void Display::SetDisplayAspectRatio(u32 numerator, u32 denominator)
{
  if (m_display_aspect_numerator == numerator && m_display_aspect_denominator == denominator)
    return;

  m_display_aspect_numerator = numerator;
  m_display_aspect_denominator = denominator;
  ResizeDisplay();
}

void Display::ResizeDisplay(u32 width /*= 0*/, u32 height /*= 0*/)
{
  // If width/height == 0, use aspect ratio to calculate size
  if (width == 0 && height == 0)
  {
    // TODO: Remove floating point math here
    // float pixel_aspect_ratio = static_cast<float>(m_framebuffer_width) / static_cast<float>(m_framebuffer_height);
    float display_aspect_ratio =
      static_cast<float>(m_display_aspect_numerator) / static_cast<float>(m_display_aspect_denominator);
    // float ratio = pixel_aspect_ratio / display_aspect_ratio;
    m_display_width = std::max(1u, m_framebuffer_width * m_display_scale);
    m_display_height = std::max(1u, static_cast<u32>(static_cast<float>(m_display_width) / display_aspect_ratio));
  }
  else
  {
    DebugAssert(width > 0 && height > 0);
    m_display_width = width * m_display_scale;
    m_display_height = height * m_display_scale;
  }

  m_renderer->DisplayResized(this);
}

void Display::ClearFramebuffer()
{
  if (m_back_buffers[0].width > 0 && m_back_buffers[0].height > 0)
    std::memset(m_back_buffers[0].data, 0, m_back_buffers[0].stride * m_back_buffers[0].height);

  SwapFramebuffer();
}

void Display::SwapFramebuffer()
{
  // Make it visible to the render thread.
  {
    std::lock_guard<std::mutex> guard(m_buffer_lock);
    std::swap(m_back_buffers[0].data, m_back_buffers[1].data);
    std::swap(m_back_buffers[0].palette, m_back_buffers[1].palette);
    std::swap(m_back_buffers[0].width, m_back_buffers[1].width);
    std::swap(m_back_buffers[0].height, m_back_buffers[1].height);
    std::swap(m_back_buffers[0].stride, m_back_buffers[1].stride);
    std::swap(m_back_buffers[0].format, m_back_buffers[1].format);
    m_back_buffers[1].dirty = true;
    m_renderer->DisplayFramebufferSwapped(this);
  }

  // Ensure backbuffer is up to date.
  if (m_back_buffers[0].width != m_framebuffer_width || m_back_buffers[0].height != m_framebuffer_height ||
      m_back_buffers[0].format != m_framebuffer_format)
  {
    AllocateFramebuffer(&m_back_buffers[0]);
  }

  AddFrameRendered();
}

bool Display::UpdateFrontbuffer()
{
  std::lock_guard<std::mutex> guard(m_buffer_lock);
  if (!m_back_buffers[1].dirty)
    return false;

  std::swap(m_front_buffer.data, m_back_buffers[1].data);
  std::swap(m_front_buffer.palette, m_back_buffers[1].palette);
  std::swap(m_front_buffer.width, m_back_buffers[1].width);
  std::swap(m_front_buffer.height, m_back_buffers[1].height);
  std::swap(m_front_buffer.stride, m_back_buffers[1].stride);
  std::swap(m_front_buffer.format, m_back_buffers[1].format);
  m_back_buffers[1].dirty = false;
  m_front_buffer.dirty = true;
  return true;
}

void Display::AllocateFramebuffer(Framebuffer* fbuf)
{
  DestroyFramebuffer(fbuf);

  fbuf->width = m_framebuffer_width;
  fbuf->height = m_framebuffer_height;
  fbuf->format = m_framebuffer_format;
  fbuf->stride = 0;

  if (m_framebuffer_width > 0 && m_framebuffer_height > 0)
  {
    switch (m_framebuffer_format)
    {
      case FramebufferFormat::RGB8:
      case FramebufferFormat::BGR8:
        fbuf->stride = m_framebuffer_width * 3;
        break;

      case FramebufferFormat::RGBX8:
      case FramebufferFormat::BGRX8:
        fbuf->stride = m_framebuffer_width * 4;
        break;

      case FramebufferFormat::RGB565:
      case FramebufferFormat::RGB555:
      case FramebufferFormat::BGR555:
      case FramebufferFormat::BGR565:
        fbuf->stride = m_framebuffer_width * 2;
        break;

      case FramebufferFormat::C8RGBX8:
        fbuf->stride = m_framebuffer_width;
        break;
    }

    fbuf->data = new byte[fbuf->stride * m_framebuffer_height];
    Y_memzero(fbuf->data, fbuf->stride * m_framebuffer_height);

    if (IsPaletteFormat(m_framebuffer_format))
    {
      fbuf->palette = new u32[PALETTE_SIZE];
      Y_memzero(fbuf->palette, sizeof(u32) * PALETTE_SIZE);
    }
  }
}

void Display::DestroyFramebuffer(Framebuffer* fbuf)
{
  delete[] fbuf->palette;
  fbuf->palette = nullptr;

  delete[] fbuf->data;
  fbuf->data = nullptr;
  fbuf->width = 0;
  fbuf->height = 0;
  fbuf->stride = 0;
}

void Display::ResizeFramebuffer(u32 width, u32 height)
{
  if (m_framebuffer_width == width && m_framebuffer_height == height)
    return;

  m_framebuffer_width = width;
  m_framebuffer_height = height;
  AllocateFramebuffer(&m_back_buffers[0]);
}

void Display::ChangeFramebufferFormat(FramebufferFormat new_format)
{
  if (m_framebuffer_format == new_format)
    return;

  m_framebuffer_format = new_format;
  AllocateFramebuffer(&m_back_buffers[0]);
}

void Display::SetPixel(u32 x, u32 y, u8 r, u8 g, u8 b)
{
  SetPixel(x, y, PackRGBX(r, g, b));
}

void Display::SetPixel(u32 x, u32 y, u32 rgb)
{
  DebugAssert(x < m_framebuffer_width && y < m_framebuffer_height);

  // Assumes LE order in rgb and framebuffer.
  switch (m_framebuffer_format)
  {
    case FramebufferFormat::RGB8:
    case FramebufferFormat::BGR8:
      std::memcpy(&m_back_buffers[0].data[y * m_back_buffers[0].stride + x * 3], &rgb, 3);
      break;

    case FramebufferFormat::RGBX8:
    case FramebufferFormat::BGRX8:
      rgb |= 0xFF000000;
      std::memcpy(&m_back_buffers[0].data[y * m_back_buffers[0].stride + x * 4], &rgb, 4);
      break;

    case FramebufferFormat::RGB555:
    case FramebufferFormat::BGR555:
      rgb &= 0x7FFF;
      std::memcpy(&m_back_buffers[0].data[y * m_back_buffers[0].stride + x * 2], &rgb, 2);
      break;

    case FramebufferFormat::RGB565:
    case FramebufferFormat::BGR565:
      std::memcpy(&m_back_buffers[0].data[y * m_back_buffers[0].stride + x * 2], &rgb, 2);
      break;

    case FramebufferFormat::C8RGBX8:
      m_back_buffers[0].data[y * m_back_buffers[0].stride + x] = Truncate8(rgb);
      break;
  }
}

void Display::CopyToFramebuffer(const void* pixels, u32 stride)
{
  if (stride == m_back_buffers[0].stride)
  {
    std::memcpy(m_back_buffers[0].data, pixels, stride * m_framebuffer_height);
    return;
  }

  const byte* pixels_src = reinterpret_cast<const byte*>(pixels);
  byte* pixels_dst = m_back_buffers[0].data;
  u32 copy_stride = std::min(m_back_buffers[0].stride, stride);
  for (u32 i = 0; i < m_framebuffer_height; i++)
  {
    std::memcpy(pixels_dst, pixels_src, copy_stride);
    pixels_src += stride;
    pixels_dst += m_back_buffers[0].stride;
  }
}

void Display::RepeatFrame()
{
  // Don't change the framebuffer.
  AddFrameRendered();
}

void Display::CopyPalette(u8 start_index, u32 num_entries, const u32* entries)
{
  DebugAssert(IsPaletteFormat(m_framebuffer_format) && (ZeroExtend32(start_index) + num_entries) <= PALETTE_SIZE);
  std::copy_n(entries, num_entries, &m_back_buffers[0].palette[start_index]);
}

void Display::AddFrameRendered()
{
  m_frames_rendered++;

  // Update every 500ms
  float dt = float(m_frame_counter_timer.GetTimeSeconds());
  if (dt >= 1.0f)
  {
    m_fps = float(m_frames_rendered) * (1.0f / dt);
    m_frames_rendered = 0;
    m_frame_counter_timer.Reset();
  }
}

inline u32 ConvertRGB555ToRGBX8888(u16 color)
{
  u8 r = Truncate8(color & 31);
  u8 g = Truncate8((color >> 5) & 31);
  u8 b = Truncate8((color >> 10) & 31);

  // 00012345 -> 1234545
  b = (b << 3) | (b >> 3);
  g = (g << 3) | (g >> 3);
  r = (r << 3) | (r >> 3);

  return UINT32_C(0xFF000000) | ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16);
}

inline u32 ConvertRGB565ToRGBX8888(u16 color)
{
  u8 r = Truncate8(color & 31);
  u8 g = Truncate8((color >> 5) & 63);
  u8 b = Truncate8((color >> 11) & 31);

  // 00012345 -> 1234545 / 00123456 -> 12345656
  r = (r << 3) | (r >> 3);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 3);

  return UINT32_C(0xFF000000) | ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16);
}

inline u32 ConvertBGR555ToRGBX8888(u16 color)
{
  u8 b = Truncate8(color & 31);
  u8 g = Truncate8((color >> 5) & 31);
  u8 r = Truncate8((color >> 10) & 31);

  // 00012345 -> 1234545
  b = (b << 3) | (b >> 3);
  g = (g << 3) | (g >> 3);
  r = (r << 3) | (r >> 3);

  return UINT32_C(0xFF000000) | ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16);
}

inline u32 ConvertBGR565ToRGBX8888(u16 color)
{
  u8 b = Truncate8(color & 31);
  u8 g = Truncate8((color >> 5) & 63);
  u8 r = Truncate8((color >> 11) & 31);

  // 00012345 -> 1234545 / 00123456 -> 12345656
  b = (b << 3) | (b >> 3);
  g = (g << 2) | (g >> 4);
  r = (r << 3) | (r >> 3);

  return UINT32_C(0xFF000000) | ZeroExtend32(r) | (ZeroExtend32(g) << 8) | (ZeroExtend32(b) << 16);
}

void Display::CopyFramebufferToRGBA8Buffer(const Framebuffer* fbuf, void* dst, u32 dst_stride)
{
  const byte* src_ptr = reinterpret_cast<const byte*>(fbuf->data);
  byte* dst_ptr = reinterpret_cast<byte*>(dst);

  switch (fbuf->format)
  {
    case FramebufferFormat::RGB8:
    {
      // yuck.. TODO optimize this, using vectorization?
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const byte* src_row_ptr = src_ptr;
        byte* dst_row_ptr = dst_ptr;
        for (u32 col = 0; col < fbuf->width; col++)
        {
          u32 ocol = 0xFF000000;
          ocol |= ZeroExtend32(*(src_row_ptr++));       // R
          ocol |= ZeroExtend32(*(src_row_ptr++)) << 8;  // G
          ocol |= ZeroExtend32(*(src_row_ptr++)) << 16; // B
          std::memcpy(dst_row_ptr, &ocol, sizeof(ocol));
          dst_row_ptr += sizeof(ocol);
        }

        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::RGBX8:
    {
      const u32 copy_size = std::min(fbuf->stride, dst_stride);
      for (u32 row = 0; row < fbuf->height; row++)
      {
        std::memcpy(dst_ptr, src_ptr, copy_size);
        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::BGR8:
    {
      // yuck.. TODO optimize this, using vectorization?
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const byte* src_row_ptr = src_ptr;
        byte* dst_row_ptr = dst_ptr;
        for (u32 col = 0; col < fbuf->width; col++)
        {
          u32 ocol = 0xFF000000;
          ocol |= ZeroExtend32(*(src_row_ptr++)) << 16; // B
          ocol |= ZeroExtend32(*(src_row_ptr++)) << 8;  // G
          ocol |= ZeroExtend32(*(src_row_ptr++));       // R
          std::memcpy(dst_row_ptr, &ocol, sizeof(ocol));
          dst_row_ptr += sizeof(ocol);
        }

        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::BGRX8:
    {
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const u32* row_src_ptr = reinterpret_cast<const u32*>(src_ptr);
        u32* row_dst_ptr = reinterpret_cast<u32*>(dst_ptr);
        for (u32 col = 0; col < fbuf->width; col++)
        {
          const u32 pix = *(row_src_ptr++);
          *(row_dst_ptr++) =
            (pix & UINT32_C(0xFF00FF00)) | ((pix & UINT32_C(0xFF)) << 16) | ((pix >> 16) & UINT32_C(0xFF));
        }
        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::RGB555:
    {
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const byte* src_row_ptr = src_ptr;
        byte* dst_row_ptr = dst_ptr;
        for (u32 col = 0; col < fbuf->width; col++)
        {
          u16 icol;
          std::memcpy(&icol, src_row_ptr, sizeof(icol));
          src_row_ptr += sizeof(icol);
          u32 ocol = ConvertRGB555ToRGBX8888(icol);
          std::memcpy(dst_row_ptr, &ocol, sizeof(ocol));
          dst_row_ptr += sizeof(ocol);
        }

        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::RGB565:
    {
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const byte* src_row_ptr = src_ptr;
        byte* dst_row_ptr = dst_ptr;
        for (u32 col = 0; col < fbuf->width; col++)
        {
          u16 icol;
          std::memcpy(&icol, src_row_ptr, sizeof(icol));
          src_row_ptr += sizeof(icol);
          u32 ocol = ConvertRGB565ToRGBX8888(icol);
          std::memcpy(dst_row_ptr, &ocol, sizeof(ocol));
          dst_row_ptr += sizeof(ocol);
        }

        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::BGR555:
    {
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const byte* src_row_ptr = src_ptr;
        byte* dst_row_ptr = dst_ptr;
        for (u32 col = 0; col < fbuf->width; col++)
        {
          u16 icol;
          std::memcpy(&icol, src_row_ptr, sizeof(icol));
          src_row_ptr += sizeof(icol);
          u32 ocol = ConvertBGR555ToRGBX8888(icol);
          std::memcpy(dst_row_ptr, &ocol, sizeof(ocol));
          dst_row_ptr += sizeof(ocol);
        }

        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::BGR565:
    {
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const byte* src_row_ptr = src_ptr;
        byte* dst_row_ptr = dst_ptr;
        for (u32 col = 0; col < fbuf->width; col++)
        {
          u16 icol;
          std::memcpy(&icol, src_row_ptr, sizeof(icol));
          src_row_ptr += sizeof(icol);
          u32 ocol = ConvertBGR565ToRGBX8888(icol);
          std::memcpy(dst_row_ptr, &ocol, sizeof(ocol));
          dst_row_ptr += sizeof(ocol);
        }

        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;

    case FramebufferFormat::C8RGBX8:
    {
      for (u32 row = 0; row < fbuf->height; row++)
      {
        const byte* src_row_ptr = src_ptr;
        byte* dst_row_ptr = dst_ptr;
        for (u32 col = 0; col < fbuf->width; col++)
        {
          std::memcpy(dst_row_ptr, &fbuf->palette[ZeroExtend32(*src_row_ptr++)], sizeof(u32));
          dst_row_ptr += sizeof(u32);
        }

        src_ptr += fbuf->stride;
        dst_ptr += dst_stride;
      }
    }
    break;
  }
}
