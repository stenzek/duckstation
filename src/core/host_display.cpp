#include "host_display.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "host_interface.h"
#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>
Log_SetChannel(HostDisplay);

HostDisplayTexture::~HostDisplayTexture() = default;

HostDisplay::~HostDisplay() = default;

void HostDisplay::SetDisplayMaxFPS(float max_fps)
{
  m_display_frame_interval = (max_fps > 0.0f) ? (1.0f / max_fps) : 0.0f;
}

bool HostDisplay::ShouldSkipDisplayingFrame()
{
  if (m_display_frame_interval == 0.0f)
    return false;

  const u64 now = Common::Timer::GetValue();
  const double diff = Common::Timer::ConvertValueToSeconds(now - m_last_frame_displayed_time);
  if (diff < m_display_frame_interval)
    return true;

  m_last_frame_displayed_time = now;
  return false;
}

u32 HostDisplay::GetDisplayPixelFormatSize(HostDisplayPixelFormat format)
{
  switch (format)
  {
    case HostDisplayPixelFormat::RGBA8:
    case HostDisplayPixelFormat::BGRA8:
      return 4;

    case HostDisplayPixelFormat::RGBA5551:
    case HostDisplayPixelFormat::RGB565:
      return 2;

    default:
      return 0;
  }
}

bool HostDisplay::SetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, const void* buffer, u32 pitch)
{
  void* map_ptr;
  u32 map_pitch;
  if (!BeginSetDisplayPixels(format, width, height, &map_ptr, &map_pitch))
    return false;

  if (pitch == map_pitch)
  {
    std::memcpy(map_ptr, buffer, height * map_pitch);
  }
  else
  {
    const u32 copy_size = width * GetDisplayPixelFormatSize(format);
    DebugAssert(pitch >= copy_size && map_pitch >= copy_size);

    const u8* src_ptr = static_cast<const u8*>(buffer);
    u8* dst_ptr = static_cast<u8*>(map_ptr);
    for (u32 i = 0; i < height; i++)
    {
      std::memcpy(dst_ptr, src_ptr, copy_size);
      src_ptr += pitch;
      dst_ptr += map_pitch;
    }
  }

  EndSetDisplayPixels();
  return true;
}

bool HostDisplay::GetHostRefreshRate(float* refresh_rate)
{
  return g_host_interface->GetMainDisplayRefreshRate(refresh_rate);
}

void HostDisplay::SetSoftwareCursor(std::unique_ptr<HostDisplayTexture> texture, float scale /*= 1.0f*/)
{
  m_cursor_texture = std::move(texture);
  m_cursor_texture_scale = scale;
}

bool HostDisplay::SetSoftwareCursor(const void* pixels, u32 width, u32 height, u32 stride, float scale /*= 1.0f*/)
{
  std::unique_ptr<HostDisplayTexture> tex =
    CreateTexture(width, height, 1, 1, 1, HostDisplayPixelFormat::RGBA8, pixels, stride, false);
  if (!tex)
    return false;

  SetSoftwareCursor(std::move(tex), scale);
  return true;
}

bool HostDisplay::SetSoftwareCursor(const char* path, float scale /*= 1.0f*/)
{
  auto fp = FileSystem::OpenManagedCFile(path, "rb");
  if (!fp)
  {
    return false;
  }

  int width, height, file_channels;
  u8* pixel_data = stbi_load_from_file(fp.get(), &width, &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from '%s': %s", path, error_reason ? error_reason : "unknown error");
    return false;
  }

  std::unique_ptr<HostDisplayTexture> tex =
    CreateTexture(static_cast<u32>(width), static_cast<u32>(height), 1, 1, 1, HostDisplayPixelFormat::RGBA8, pixel_data,
                  sizeof(u32) * static_cast<u32>(width), false);
  stbi_image_free(pixel_data);
  if (!tex)
    return false;

  Log_InfoPrintf("Loaded %dx%d image from '%s' for software cursor", width, height, path);
  SetSoftwareCursor(std::move(tex), scale);
  return true;
}

void HostDisplay::ClearSoftwareCursor()
{
  m_cursor_texture.reset();
  m_cursor_texture_scale = 1.0f;
}

void HostDisplay::CalculateDrawRect(s32 window_width, s32 window_height, float* out_left, float* out_top,
                                    float* out_width, float* out_height, float* out_left_padding,
                                    float* out_top_padding, float* out_scale, float* out_x_scale,
                                    bool apply_aspect_ratio /* = true */) const
{
  const float x_scale =
    apply_aspect_ratio ?
      (m_display_aspect_ratio / (static_cast<float>(m_display_width) / static_cast<float>(m_display_height))) :
      1.0f;
  const float display_width = static_cast<float>(m_display_width) * x_scale;
  const float display_height = static_cast<float>(m_display_height);
  const float active_left = static_cast<float>(m_display_active_left) * x_scale;
  const float active_top = static_cast<float>(m_display_active_top);
  const float active_width = static_cast<float>(m_display_active_width) * x_scale;
  const float active_height = static_cast<float>(m_display_active_height);
  if (out_x_scale)
    *out_x_scale = x_scale;

  // now fit it within the window
  const float window_ratio = static_cast<float>(window_width) / static_cast<float>(window_height);

  float scale;
  if ((display_width / display_height) >= window_ratio)
  {
    // align in middle vertically
    scale = static_cast<float>(window_width) / display_width;
    if (m_display_integer_scaling)
      scale = std::max(std::floor(scale), 1.0f);

    if (out_left_padding)
    {
      if (m_display_integer_scaling)
        *out_left_padding = std::max<float>((static_cast<float>(window_width) - display_width * scale) / 2.0f, 0.0f);
      else
        *out_left_padding = 0.0f;
    }
    if (out_top_padding)
    {
      switch (m_display_alignment)
      {
        case Alignment::RightOrBottom:
          *out_top_padding = std::max<float>(static_cast<float>(window_height) - (display_height * scale), 0.0f);
          break;

        case Alignment::Center:
          *out_top_padding =
            std::max<float>((static_cast<float>(window_height) - (display_height * scale)) / 2.0f, 0.0f);
          break;

        case Alignment::LeftOrTop:
        default:
          *out_top_padding = 0.0f;
          break;
      }
    }
  }
  else
  {
    // align in middle horizontally
    scale = static_cast<float>(window_height) / display_height;
    if (m_display_integer_scaling)
      scale = std::max(std::floor(scale), 1.0f);

    if (out_left_padding)
    {
      switch (m_display_alignment)
      {
        case Alignment::RightOrBottom:
          *out_left_padding = std::max<float>(static_cast<float>(window_width) - (display_width * scale), 0.0f);
          break;

        case Alignment::Center:
          *out_left_padding =
            std::max<float>((static_cast<float>(window_width) - (display_width * scale)) / 2.0f, 0.0f);
          break;

        case Alignment::LeftOrTop:
        default:
          *out_left_padding = 0.0f;
          break;
      }
    }

    if (out_top_padding)
    {
      if (m_display_integer_scaling)
        *out_top_padding = std::max<float>((static_cast<float>(window_height) - (display_height * scale)) / 2.0f, 0.0f);
      else
        *out_top_padding = 0.0f;
    }
  }

  *out_width = active_width * scale;
  *out_height = active_height * scale;
  *out_left = active_left * scale;
  *out_top = active_top * scale;
  if (out_scale)
    *out_scale = scale;
}

std::tuple<s32, s32, s32, s32> HostDisplay::CalculateDrawRect(s32 window_width, s32 window_height, s32 top_margin,
                                                              bool apply_aspect_ratio /* = true */) const
{
  float left, top, width, height, left_padding, top_padding;
  CalculateDrawRect(window_width, window_height - top_margin, &left, &top, &width, &height, &left_padding, &top_padding,
                    nullptr, nullptr, apply_aspect_ratio);

  return std::make_tuple(static_cast<s32>(left + left_padding), static_cast<s32>(top + top_padding) + top_margin,
                         static_cast<s32>(width), static_cast<s32>(height));
}

std::tuple<s32, s32, s32, s32> HostDisplay::CalculateSoftwareCursorDrawRect() const
{
  return CalculateSoftwareCursorDrawRect(m_mouse_position_x, m_mouse_position_y);
}

std::tuple<s32, s32, s32, s32> HostDisplay::CalculateSoftwareCursorDrawRect(s32 cursor_x, s32 cursor_y) const
{
  const float scale = m_window_info.surface_scale * m_cursor_texture_scale;
  const u32 cursor_extents_x = static_cast<u32>(static_cast<float>(m_cursor_texture->GetWidth()) * scale * 0.5f);
  const u32 cursor_extents_y = static_cast<u32>(static_cast<float>(m_cursor_texture->GetHeight()) * scale * 0.5f);

  const s32 out_left = cursor_x - cursor_extents_x;
  const s32 out_top = cursor_y - cursor_extents_y;
  const s32 out_width = cursor_extents_x * 2u;
  const s32 out_height = cursor_extents_y * 2u;

  return std::tie(out_left, out_top, out_width, out_height);
}

std::tuple<float, float> HostDisplay::ConvertWindowCoordinatesToDisplayCoordinates(s32 window_x, s32 window_y,
                                                                                   s32 window_width, s32 window_height,
                                                                                   s32 top_margin) const
{
  float left, top, width, height, left_padding, top_padding;
  float scale, x_scale;
  CalculateDrawRect(window_width, window_height - top_margin, &left, &top, &width, &height, &left_padding, &top_padding,
                    &scale, &x_scale);

  // convert coordinates to active display region, then to full display region
  const float scaled_display_x = static_cast<float>(window_x) - left_padding;
  const float scaled_display_y = static_cast<float>(window_y) - top_padding + static_cast<float>(top_margin);

  // scale back to internal resolution
  const float display_x = scaled_display_x / scale / x_scale;
  const float display_y = scaled_display_y / scale;

  return std::make_tuple(display_x, display_y);
}

static bool ConvertTextureDataToRGBA8(u32 width, u32 height, std::vector<u32>& texture_data, u32& texture_data_stride,
                                      HostDisplayPixelFormat format)
{
  switch (format)
  {
    case HostDisplayPixelFormat::BGRA8:
    {
      for (u32 y = 0; y < height; y++)
      {
        u32* pixels = reinterpret_cast<u32*>(reinterpret_cast<u8*>(texture_data.data()) + (y * texture_data_stride));
        for (u32 x = 0; x < width; x++)
          pixels[x] = (pixels[x] & 0xFF00FF00) | ((pixels[x] & 0xFF) << 16) | ((pixels[x] >> 16) & 0xFF);
      }

      return true;
    }

    case HostDisplayPixelFormat::RGBA8:
      return true;

    case HostDisplayPixelFormat::RGB565:
    {
      std::vector<u32> temp(width * height);

      for (u32 y = 0; y < height; y++)
      {
        const u8* pixels_in = reinterpret_cast<u8*>(texture_data.data()) + (y * texture_data_stride);
        u32* pixels_out = &temp[y * width];

        for (u32 x = 0; x < width; x++)
        {
          // RGB565 -> RGBA8
          u16 pixel_in;
          std::memcpy(&pixel_in, pixels_in, sizeof(u16));
          pixels_in += sizeof(u16);
          const u8 r5 = Truncate8(pixel_in >> 11);
          const u8 g6 = Truncate8((pixel_in >> 5) & 0x3F);
          const u8 b5 = Truncate8(pixel_in & 0x1F);
          *(pixels_out++) = ZeroExtend32((r5 << 3) | (r5 & 7)) | (ZeroExtend32((g6 << 2) | (g6 & 3)) << 8) |
                            (ZeroExtend32((b5 << 3) | (b5 & 7)) << 16) | (0xFF000000u);
        }
      }

      texture_data = std::move(temp);
      texture_data_stride = sizeof(u32) * width;
      return true;
    }

    case HostDisplayPixelFormat::RGBA5551:
    {
      std::vector<u32> temp(width * height);

      for (u32 y = 0; y < height; y++)
      {
        const u8* pixels_in = reinterpret_cast<u8*>(texture_data.data()) + (y * texture_data_stride);
        u32* pixels_out = &temp[y * width];

        for (u32 x = 0; x < width; x++)
        {
          // RGBA5551 -> RGBA8
          u16 pixel_in;
          std::memcpy(&pixel_in, pixels_in, sizeof(u16));
          pixels_in += sizeof(u16);
          const u8 a1 = Truncate8(pixel_in >> 15);
          const u8 r5 = Truncate8((pixel_in >> 10) & 0x1F);
          const u8 g6 = Truncate8((pixel_in >> 5) & 0x1F);
          const u8 b5 = Truncate8(pixel_in & 0x1F);
          *(pixels_out++) = ZeroExtend32((r5 << 3) | (r5 & 7)) | (ZeroExtend32((g6 << 3) | (g6 & 7)) << 8) |
                            (ZeroExtend32((b5 << 3) | (b5 & 7)) << 16) | (a1 ? 0xFF000000u : 0u);
        }
      }

      texture_data = std::move(temp);
      texture_data_stride = sizeof(u32) * width;
      return true;
    }

    default:
      Log_ErrorPrintf("Unknown pixel format %u", static_cast<u32>(format));
      return false;
  }
}

static bool CompressAndWriteTextureToFile(u32 width, u32 height, std::string filename, FileSystem::ManagedCFilePtr fp,
                                          bool clear_alpha, bool flip_y, u32 resize_width, u32 resize_height,
                                          std::vector<u32> texture_data, u32 texture_data_stride,
                                          HostDisplayPixelFormat texture_format)
{

  const char* extension = std::strrchr(filename.c_str(), '.');
  if (!extension)
  {
    Log_ErrorPrintf("Unable to determine file extension for '%s'", filename.c_str());
    return false;
  }

  if (!ConvertTextureDataToRGBA8(width, height, texture_data, texture_data_stride, texture_format))
    return false;

  if (clear_alpha)
  {
    for (u32& pixel : texture_data)
      pixel |= 0xFF000000;
  }

  if (flip_y)
  {
    std::vector<u32> temp(width);
    for (u32 flip_row = 0; flip_row < (height / 2); flip_row++)
    {
      u32* top_ptr = &texture_data[flip_row * width];
      u32* bottom_ptr = &texture_data[((height - 1) - flip_row) * width];
      std::memcpy(temp.data(), top_ptr, texture_data_stride);
      std::memcpy(top_ptr, bottom_ptr, texture_data_stride);
      std::memcpy(bottom_ptr, temp.data(), texture_data_stride);
    }
  }

  if (resize_width > 0 && resize_height > 0 && (resize_width != width || resize_height != height))
  {
    std::vector<u32> resized_texture_data(resize_width * resize_height);
    u32 resized_texture_stride = sizeof(u32) * resize_width;
    if (!stbir_resize_uint8(reinterpret_cast<u8*>(texture_data.data()), width, height, texture_data_stride,
                            reinterpret_cast<u8*>(resized_texture_data.data()), resize_width, resize_height,
                            resized_texture_stride, 4))
    {
      Log_ErrorPrintf("Failed to resize texture data from %ux%u to %ux%u", width, height, resize_width, resize_height);
      return false;
    }

    width = resize_width;
    height = resize_height;
    texture_data = std::move(resized_texture_data);
    texture_data_stride = resized_texture_stride;
  }

  const auto write_func = [](void* context, void* data, int size) {
    std::fwrite(data, 1, size, static_cast<std::FILE*>(context));
  };

  bool result = false;
  if (StringUtil::Strcasecmp(extension, ".png") == 0)
  {
    result =
      (stbi_write_png_to_func(write_func, fp.get(), width, height, 4, texture_data.data(), texture_data_stride) != 0);
  }
  else if (StringUtil::Strcasecmp(extension, ".jpg") == 0)
  {
    result = (stbi_write_jpg_to_func(write_func, fp.get(), width, height, 4, texture_data.data(), 95) != 0);
  }
  else if (StringUtil::Strcasecmp(extension, ".tga") == 0)
  {
    result = (stbi_write_tga_to_func(write_func, fp.get(), width, height, 4, texture_data.data()) != 0);
  }
  else if (StringUtil::Strcasecmp(extension, ".bmp") == 0)
  {
    result = (stbi_write_bmp_to_func(write_func, fp.get(), width, height, 4, texture_data.data()) != 0);
  }

  if (!result)
  {
    Log_ErrorPrintf("Unknown extension in filename '%s' or save error: '%s'", filename.c_str(), extension);
    return false;
  }

  return true;
}

bool HostDisplay::WriteTextureToFile(const void* texture_handle, u32 x, u32 y, u32 width, u32 height,
                                     HostDisplayPixelFormat format, std::string filename, bool clear_alpha /* = true */,
                                     bool flip_y /* = false */, u32 resize_width /* = 0 */, u32 resize_height /* = 0 */,
                                     bool compress_on_thread /* = false */)
{
  std::vector<u32> texture_data(width * height);
  u32 texture_data_stride = Common::AlignUpPow2(GetDisplayPixelFormatSize(format) * width, 4);
  if (!DownloadTexture(texture_handle, format, x, y, width, height, texture_data.data(), texture_data_stride))
  {
    Log_ErrorPrintf("Texture download failed");
    return false;
  }

  auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wb");
  if (!fp)
  {
    Log_ErrorPrintf("Can't open file '%s': errno %d", filename.c_str(), errno);
    return false;
  }

  if (!compress_on_thread)
  {
    return CompressAndWriteTextureToFile(width, height, std::move(filename), std::move(fp), clear_alpha, flip_y,
                                         resize_width, resize_height, std::move(texture_data), texture_data_stride,
                                         format);
  }

  std::thread compress_thread(CompressAndWriteTextureToFile, width, height, std::move(filename), std::move(fp),
                              clear_alpha, flip_y, resize_width, resize_height, std::move(texture_data),
                              texture_data_stride, format);
  compress_thread.detach();
  return true;
}

bool HostDisplay::WriteDisplayTextureToFile(std::string filename, bool full_resolution /* = true */,
                                            bool apply_aspect_ratio /* = true */, bool compress_on_thread /* = false */)
{
  if (!m_display_texture_handle)
    return false;

  s32 resize_width = 0;
  s32 resize_height = std::abs(m_display_texture_view_height);
  if (apply_aspect_ratio)
  {
    const float ss_width_scale = static_cast<float>(m_display_active_width) / static_cast<float>(m_display_width);
    const float ss_height_scale = static_cast<float>(m_display_active_height) / static_cast<float>(m_display_height);
    const float ss_aspect_ratio = m_display_aspect_ratio * ss_width_scale / ss_height_scale;
    resize_width = static_cast<s32>(static_cast<float>(resize_height) * ss_aspect_ratio);
  }
  else
  {
    resize_width = m_display_texture_view_width;
  }

  if (!full_resolution)
  {
    const s32 resolution_scale = std::abs(m_display_texture_view_height) / m_display_active_height;
    resize_height /= resolution_scale;
    resize_width /= resolution_scale;
  }

  if (resize_width <= 0 || resize_height <= 0)
    return false;

  const bool flip_y = (m_display_texture_view_height < 0);
  s32 read_height = m_display_texture_view_height;
  s32 read_y = m_display_texture_view_y;
  if (flip_y)
  {
    read_height = -m_display_texture_view_height;
    read_y = (m_display_texture_height - read_height) - (m_display_texture_height - m_display_texture_view_y);
  }

  return WriteTextureToFile(m_display_texture_handle, m_display_texture_view_x, read_y, m_display_texture_view_width,
                            read_height, m_display_texture_format, std::move(filename), true, flip_y,
                            static_cast<u32>(resize_width), static_cast<u32>(resize_height), compress_on_thread);
}

bool HostDisplay::WriteDisplayTextureToBuffer(std::vector<u32>* buffer, u32 resize_width /* = 0 */,
                                              u32 resize_height /* = 0 */, bool clear_alpha /* = true */)
{
  if (!m_display_texture_handle)
    return false;

  const bool flip_y = (m_display_texture_view_height < 0);
  s32 read_width = m_display_texture_view_width;
  s32 read_height = m_display_texture_view_height;
  s32 read_x = m_display_texture_view_x;
  s32 read_y = m_display_texture_view_y;
  if (flip_y)
  {
    read_height = -m_display_texture_view_height;
    read_y = (m_display_texture_height - read_height) - (m_display_texture_height - m_display_texture_view_y);
  }

  u32 width = static_cast<u32>(read_width);
  u32 height = static_cast<u32>(read_height);
  std::vector<u32> texture_data(width * height);
  u32 texture_data_stride = Common::AlignUpPow2(GetDisplayPixelFormatSize(m_display_texture_format) * width, 4);
  if (!DownloadTexture(m_display_texture_handle, m_display_texture_format, read_x, read_y, width, height,
                       texture_data.data(), texture_data_stride))
  {
    Log_ErrorPrintf("Failed to download texture from GPU.");
    return false;
  }

  if (!ConvertTextureDataToRGBA8(width, height, texture_data, texture_data_stride, m_display_texture_format))
    return false;

  if (clear_alpha)
  {
    for (u32& pixel : texture_data)
      pixel |= 0xFF000000;
  }

  if (flip_y)
  {
    std::vector<u32> temp(width);
    for (u32 flip_row = 0; flip_row < (height / 2); flip_row++)
    {
      u32* top_ptr = &texture_data[flip_row * width];
      u32* bottom_ptr = &texture_data[((height - 1) - flip_row) * width];
      std::memcpy(temp.data(), top_ptr, texture_data_stride);
      std::memcpy(top_ptr, bottom_ptr, texture_data_stride);
      std::memcpy(bottom_ptr, temp.data(), texture_data_stride);
    }
  }

  if (resize_width > 0 && resize_height > 0 && (resize_width != width || resize_height != height))
  {
    std::vector<u32> resized_texture_data(resize_width * resize_height);
    u32 resized_texture_stride = sizeof(u32) * resize_width;
    if (!stbir_resize_uint8(reinterpret_cast<u8*>(texture_data.data()), width, height, texture_data_stride,
                            reinterpret_cast<u8*>(resized_texture_data.data()), resize_width, resize_height,
                            resized_texture_stride, 4))
    {
      Log_ErrorPrintf("Failed to resize texture data from %ux%u to %ux%u", width, height, resize_width, resize_height);
      return false;
    }

    width = resize_width;
    height = resize_height;
    *buffer = std::move(resized_texture_data);
    texture_data_stride = resized_texture_stride;
  }
  else
  {
    *buffer = texture_data;
  }

  return true;
}
