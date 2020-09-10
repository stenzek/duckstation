#include "host_display.h"
#include "common/log.h"
#include "common/string_util.h"
#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"
#include <cmath>
#include <cstring>
#include <vector>
Log_SetChannel(HostDisplay);

HostDisplayTexture::~HostDisplayTexture() = default;

HostDisplay::~HostDisplay() = default;

void HostDisplay::SetSoftwareCursor(std::unique_ptr<HostDisplayTexture> texture, float scale /*= 1.0f*/)
{
  m_cursor_texture = std::move(texture);
  m_cursor_texture_scale = scale;
}

bool HostDisplay::SetSoftwareCursor(const void* pixels, u32 width, u32 height, u32 stride, float scale /*= 1.0f*/)
{
  std::unique_ptr<HostDisplayTexture> tex = CreateTexture(width, height, pixels, stride, false);
  if (!tex)
    return false;

  SetSoftwareCursor(std::move(tex), scale);
  return true;
}

bool HostDisplay::SetSoftwareCursor(const char* path, float scale /*= 1.0f*/)
{
  int width, height, file_channels;
  u8* pixel_data = stbi_load(path, &width, &height, &file_channels, 4);
  if (!pixel_data)
  {
    const char* error_reason = stbi_failure_reason();
    Log_ErrorPrintf("Failed to load image from '%s': %s", path, error_reason ? error_reason : "unknown error");
    return false;
  }

  std::unique_ptr<HostDisplayTexture> tex = CreateTexture(static_cast<u32>(width), static_cast<u32>(height), pixel_data,
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

void HostDisplay::CalculateDrawRect(s32 window_width, s32 window_height, s32* out_left, s32* out_top, s32* out_width,
                                    s32* out_height, s32* out_left_padding, s32* out_top_padding, float* out_scale,
                                    float* out_y_scale, bool apply_aspect_ratio) const
{
  apply_aspect_ratio = (m_display_aspect_ratio > 0) ? apply_aspect_ratio : false;
  const float y_scale =
    apply_aspect_ratio ?
      ((static_cast<float>(m_display_width) / static_cast<float>(m_display_height)) / m_display_aspect_ratio) :
      1.0f;
  const float display_width = static_cast<float>(m_display_width);
  const float display_height = static_cast<float>(m_display_height) * y_scale;
  const float active_left = static_cast<float>(m_display_active_left);
  const float active_top = static_cast<float>(m_display_active_top) * y_scale;
  const float active_width = static_cast<float>(m_display_active_width);
  const float active_height = static_cast<float>(m_display_active_height) * y_scale;
  if (out_y_scale)
    *out_y_scale = y_scale;

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
        *out_left_padding = std::max<s32>((window_width - static_cast<s32>(display_width * scale)) / 2, 0);
      else
        *out_left_padding = 0;
    }
    if (out_top_padding)
    {
      switch (m_display_alignment)
      {
        case Alignment::LeftOrTop:
          *out_top_padding = 0;
          break;

        case Alignment::Center:
          *out_top_padding = std::max<s32>((window_height - static_cast<s32>(display_height * scale)) / 2, 0);
          break;

        case Alignment::RightOrBottom:
          *out_top_padding = std::max<s32>(window_height - static_cast<s32>(display_height * scale), 0);
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
        case Alignment::LeftOrTop:
          *out_left_padding = 0;
          break;

        case Alignment::Center:
          *out_left_padding = std::max<s32>((window_width - static_cast<s32>(display_width * scale)) / 2, 0);
          break;

        case Alignment::RightOrBottom:
          *out_left_padding = std::max<s32>(window_width - static_cast<s32>(display_width * scale), 0);
          break;
      }
    }

    if (out_top_padding)
    {
      if (m_display_integer_scaling)
        *out_top_padding = std::max<s32>((window_height - static_cast<s32>(display_height * scale)) / 2, 0);
      else
        *out_top_padding = 0;
    }
  }

  *out_width = static_cast<s32>(active_width * scale);
  *out_height = static_cast<s32>(active_height * scale);
  *out_left = static_cast<s32>(active_left * scale);
  *out_top = static_cast<s32>(active_top * scale);
  if (out_scale)
    *out_scale = scale;
}

std::tuple<s32, s32, s32, s32> HostDisplay::CalculateDrawRect(s32 window_width, s32 window_height, s32 top_margin,
                                                              bool apply_aspect_ratio /* = true */) const
{
  s32 left, top, width, height, left_padding, top_padding;
  CalculateDrawRect(window_width, window_height - top_margin, &left, &top, &width, &height, &left_padding, &top_padding,
                    nullptr, nullptr, apply_aspect_ratio);
  return std::make_tuple(left + left_padding, top + top_padding + top_margin, width, height);
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

std::tuple<s32, s32> HostDisplay::ConvertWindowCoordinatesToDisplayCoordinates(s32 window_x, s32 window_y,
                                                                               s32 window_width, s32 window_height,
                                                                               s32 top_margin) const
{
  s32 left, top, width, height, left_padding, top_padding;
  float scale, y_scale;
  CalculateDrawRect(window_width, window_height - top_margin, &left, &top, &width, &height, &left_padding, &top_padding,
                    &scale, &y_scale);

  // convert coordinates to active display region, then to full display region
  const float scaled_display_x = static_cast<float>(window_x - (left_padding));
  const float scaled_display_y = static_cast<float>(window_y - (top_padding + top_margin));

  // scale back to internal resolution
  const float display_x = scaled_display_x / scale;
  const float display_y = scaled_display_y / scale / y_scale;

  return std::make_tuple(static_cast<s32>(display_x), static_cast<s32>(display_y));
}

bool HostDisplay::WriteTextureToFile(const void* texture_handle, u32 x, u32 y, u32 width, u32 height,
                                     const char* filename, bool clear_alpha /* = true */, bool flip_y /* = false */,
                                     u32 resize_width /* = 0 */, u32 resize_height /* = 0 */)
{
  std::vector<u32> texture_data(width * height);
  u32 texture_data_stride = sizeof(u32) * width;
  if (!DownloadTexture(texture_handle, x, y, width, height, texture_data.data(), texture_data_stride))
  {
    Log_ErrorPrintf("Texture download failed");
    return false;
  }

  const char* extension = std::strrchr(filename, '.');
  if (!extension)
  {
    Log_ErrorPrintf("Unable to determine file extension for '%s'", filename);
    return false;
  }

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

  bool result;
  if (StringUtil::Strcasecmp(extension, ".png") == 0)
  {
    result = (stbi_write_png(filename, width, height, 4, texture_data.data(), texture_data_stride) != 0);
  }
  else if (StringUtil::Strcasecmp(filename, ".jpg") == 0)
  {
    result = (stbi_write_jpg(filename, width, height, 4, texture_data.data(), 95) != 0);
  }
  else if (StringUtil::Strcasecmp(filename, ".tga") == 0)
  {
    result = (stbi_write_tga(filename, width, height, 4, texture_data.data()) != 0);
  }
  else if (StringUtil::Strcasecmp(filename, ".bmp") == 0)
  {
    result = (stbi_write_bmp(filename, width, height, 4, texture_data.data()) != 0);
  }
  else
  {
    Log_ErrorPrintf("Unknown extension in filename '%s': '%s'", filename, extension);
    return false;
  }

  if (!result)
  {
    Log_ErrorPrintf("Failed to save texture to '%s'", filename);
    return false;
  }

  return true;
}

bool HostDisplay::WriteDisplayTextureToFile(const char* filename, bool full_resolution /* = true */,
                                            bool apply_aspect_ratio /* = true */)
{
  if (!m_display_texture_handle)
    return false;

  apply_aspect_ratio = (m_display_aspect_ratio > 0) ? apply_aspect_ratio : false;

  s32 resize_width = 0;
  s32 resize_height = 0;
  if (apply_aspect_ratio && full_resolution)
  {
    if (m_display_aspect_ratio > 1.0f)
    {
      resize_width = m_display_texture_view_width;
      resize_height = static_cast<s32>(static_cast<float>(resize_width) / m_display_aspect_ratio);
    }
    else
    {
      resize_height = std::abs(m_display_texture_view_height);
      resize_width = static_cast<s32>(static_cast<float>(resize_height) * m_display_aspect_ratio);
    }
  }
  else if (apply_aspect_ratio)
  {
    const auto [left, top, right, bottom] =
      CalculateDrawRect(GetWindowWidth(), GetWindowHeight(), m_display_top_margin);
    resize_width = right - left;
    resize_height = bottom - top;
  }
  else if (!full_resolution)
  {
    const auto [left, top, right, bottom] =
      CalculateDrawRect(GetWindowWidth(), GetWindowHeight(), m_display_top_margin);
    const float ratio =
      static_cast<float>(m_display_texture_view_width) / static_cast<float>(std::abs(m_display_texture_view_height));
    if (ratio > 1.0f)
    {
      resize_width = right - left;
      resize_height = static_cast<s32>(static_cast<float>(resize_width) / ratio);
    }
    else
    {
      resize_height = bottom - top;
      resize_width = static_cast<s32>(static_cast<float>(resize_height) * ratio);
    }
  }

  if (resize_width < 0)
    resize_width = 1;
  if (resize_height < 0)
    resize_height = 1;

  const bool flip_y = (m_display_texture_view_height < 0);
  s32 read_height = m_display_texture_view_height;
  s32 read_y = m_display_texture_view_y;
  if (flip_y)
  {
    read_height = -m_display_texture_view_height;
    read_y = (m_display_texture_height - read_height) - (m_display_texture_height - m_display_texture_view_y);
  }

  return WriteTextureToFile(m_display_texture_handle, m_display_texture_view_x, read_y, m_display_texture_view_width,
                            read_height, filename, true, flip_y, static_cast<u32>(resize_width),
                            static_cast<u32>(resize_height));
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
  u32 texture_data_stride = sizeof(u32) * width;
  if (!DownloadTexture(m_display_texture_handle, read_x, read_y, width, height, texture_data.data(),
                       texture_data_stride))
  {
    Log_ErrorPrintf("Failed to download texture from GPU.");
    return false;
  }

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
