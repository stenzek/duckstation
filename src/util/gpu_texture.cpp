// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_texture.h"
#include "gpu_device.h"

#include "common/align.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"

Log_SetChannel(GPUTexture);

GPUTexture::GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type, Format format)
  : m_width(width), m_height(height), m_layers(layers), m_levels(levels), m_samples(samples), m_type(type),
    m_format(format)
{
  GPUDevice::s_total_vram_usage += GetVRAMUsage();
}

GPUTexture::~GPUTexture()
{
  GPUDevice::s_total_vram_usage -= GetVRAMUsage();
}

const char* GPUTexture::GetFormatName(Format format)
{
  static constexpr const char* format_names[static_cast<u8>(Format::MaxCount)] = {
    "Unknown", // Unknown
    "RGBA8",   // RGBA8
    "BGRA8",   // BGRA8
    "RGB565",  // RGB565
    "RGB5551", // RGBA5551
    "R8",      // R8
    "D16",     // D16
    "R16",     // R16
    "R16I",    // R16I
    "R16U",    // R16U
    "R16F",    // R16F
    "R32I",    // R32I
    "R32U",    // R32U
    "R32F",    // R32F
    "RG8",     // RG8
    "RG16",    // RG16
    "RG16F",   // RG16F
    "RG32F",   // RG32F
    "RGBA16",  // RGBA16
    "RGBA16F", // RGBA16F
    "RGBA32F", // RGBA32F
    "RGB10A2", // RGB10A2
  };

  return format_names[static_cast<u8>(format)];
}

u32 GPUTexture::GetCompressedBytesPerBlock() const
{
  return GetCompressedBytesPerBlock(m_format);
}

u32 GPUTexture::GetCompressedBytesPerBlock(Format format)
{
  // TODO: Implement me
  return GetPixelSize(format);
}

u32 GPUTexture::GetCompressedBlockSize() const
{
  return GetCompressedBlockSize(m_format);
}

u32 GPUTexture::GetCompressedBlockSize(Format format)
{
  // TODO: Implement me
  /*if (format >= Format::BC1 && format <= Format::BC7)
    return 4;
  else*/
  return 1;
}

u32 GPUTexture::CalcUploadPitch(Format format, u32 width)
{
  /*
  if (format >= Format::BC1 && format <= Format::BC7)
    width = Common::AlignUpPow2(width, 4) / 4;
  */
  return width * GetCompressedBytesPerBlock(format);
}

u32 GPUTexture::CalcUploadPitch(u32 width) const
{
  return CalcUploadPitch(m_format, width);
}

u32 GPUTexture::CalcUploadRowLengthFromPitch(u32 pitch) const
{
  return CalcUploadRowLengthFromPitch(m_format, pitch);
}

u32 GPUTexture::CalcUploadRowLengthFromPitch(Format format, u32 pitch)
{
  const u32 block_size = GetCompressedBlockSize(format);
  const u32 bytes_per_block = GetCompressedBytesPerBlock(format);
  return ((pitch + (bytes_per_block - 1)) / bytes_per_block) * block_size;
}

u32 GPUTexture::CalcUploadSize(u32 height, u32 pitch) const
{
  return CalcUploadSize(m_format, height, pitch);
}

u32 GPUTexture::CalcUploadSize(Format format, u32 height, u32 pitch)
{
  const u32 block_size = GetCompressedBlockSize(format);
  return pitch * ((static_cast<u32>(height) + (block_size - 1)) / block_size);
}

std::array<float, 4> GPUTexture::GetUNormClearColor() const
{
  return GPUDevice::RGBA8ToFloat(m_clear_value.color);
}

size_t GPUTexture::GetVRAMUsage() const
{
  if (m_levels == 1) [[likely]]
    return ((static_cast<size_t>(m_width * m_height) * GetPixelSize(m_format)) * m_layers * m_samples);

  const size_t ps = GetPixelSize(m_format) * m_layers * m_samples;
  u32 width = m_width;
  u32 height = m_height;
  size_t ts = 0;
  for (u32 i = 0; i < m_levels; i++)
  {
    width = (width > 1) ? (width / 2) : width;
    height = (height > 1) ? (height / 2) : height;
    ts += static_cast<size_t>(width * height) * ps;
  }

  return ts;
}

u32 GPUTexture::GetPixelSize(GPUTexture::Format format)
{
  static constexpr std::array<u8, static_cast<size_t>(Format::MaxCount)> sizes = {{
    0,  // Unknown
    4,  // RGBA8
    4,  // BGRA8
    2,  // RGB565
    2,  // RGBA5551
    1,  // R8
    2,  // D16
    2,  // R16
    2,  // R16I
    2,  // R16U
    2,  // R16F
    4,  // R32I
    4,  // R32U
    4,  // R32F
    2,  // RG8
    2,  // RG16
    2,  // RG16F
    8,  // RG32F
    8,  // RGBA16
    8,  // RGBA16F
    16, // RGBA32F
    4,  // RGB10A2
  }};

  return sizes[static_cast<size_t>(format)];
}

bool GPUTexture::IsDepthFormat(Format format)
{
  return (format == Format::D16);
}

bool GPUTexture::IsDepthStencilFormat(Format format)
{
  // None needed yet.
  return false;
}

bool GPUTexture::IsCompressedFormat(Format format)
{
  // TODO: Implement me
  return false;
}

bool GPUTexture::ValidateConfig(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format)
{
  if (width > MAX_WIDTH || height > MAX_HEIGHT || layers > MAX_LAYERS || levels > MAX_LEVELS || samples > MAX_SAMPLES)
  {
    Log_ErrorPrintf("Invalid dimensions: %ux%ux%u %u %u.", width, height, layers, levels, samples);
    return false;
  }

  const u32 max_texture_size = g_gpu_device->GetMaxTextureSize();
  if (width > max_texture_size || height > max_texture_size)
  {
    Log_ErrorPrintf("Texture width (%u) or height (%u) exceeds max texture size (%u).", width, height,
                    max_texture_size);
    return false;
  }

  const u32 max_samples = g_gpu_device->GetMaxMultisamples();
  if (samples > max_samples)
  {
    Log_ErrorPrintf("Texture samples (%u) exceeds max samples (%u).", samples, max_samples);
    return false;
  }

  if (samples > 1 && levels > 1)
  {
    Log_ErrorPrintf("Multisampled textures can't have mip levels.");
    return false;
  }

  if (layers > 1 && type != Type::Texture && type != Type::DynamicTexture)
  {
    Log_ErrorPrintf("Texture arrays are not supported on targets.");
    return false;
  }

  if (levels > 1 && type != Type::Texture && type != Type::DynamicTexture)
  {
    Log_ErrorPrintf("Mipmaps are not supported on targets.");
    return false;
  }

  return true;
}

bool GPUTexture::ConvertTextureDataToRGBA8(u32 width, u32 height, std::vector<u32>& texture_data,
                                           u32& texture_data_stride, GPUTexture::Format format)
{
  switch (format)
  {
    case Format::BGRA8:
    {
      for (u32 y = 0; y < height; y++)
      {
        u8* pixels = reinterpret_cast<u8*>(texture_data.data()) + (y * texture_data_stride);
        for (u32 x = 0; x < width; x++)
        {
          u32 pixel;
          std::memcpy(&pixel, pixels, sizeof(pixel));
          pixel = (pixel & 0xFF00FF00) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
          std::memcpy(pixels, &pixel, sizeof(pixel));
          pixels += sizeof(pixel);
        }
      }

      return true;
    }

    case Format::RGBA8:
      return true;

    case Format::RGB565:
    {
      std::vector<u32> temp(width * height);

      for (u32 y = 0; y < height; y++)
      {
        const u8* pixels_in = reinterpret_cast<const u8*>(texture_data.data()) + (y * texture_data_stride);
        u8* pixels_out = reinterpret_cast<u8*>(temp.data()) + (y * width * sizeof(u32));

        for (u32 x = 0; x < width; x++)
        {
          // RGB565 -> RGBA8
          u16 pixel_in;
          std::memcpy(&pixel_in, pixels_in, sizeof(u16));
          pixels_in += sizeof(u16);
          const u8 r5 = Truncate8(pixel_in >> 11);
          const u8 g6 = Truncate8((pixel_in >> 5) & 0x3F);
          const u8 b5 = Truncate8(pixel_in & 0x1F);
          const u32 rgba8 = ZeroExtend32((r5 << 3) | (r5 & 7)) | (ZeroExtend32((g6 << 2) | (g6 & 3)) << 8) |
                            (ZeroExtend32((b5 << 3) | (b5 & 7)) << 16) | (0xFF000000u);
          std::memcpy(pixels_out, &rgba8, sizeof(u32));
          pixels_out += sizeof(u32);
        }
      }

      texture_data = std::move(temp);
      texture_data_stride = sizeof(u32) * width;
      return true;
    }

    case Format::RGBA5551:
    {
      std::vector<u32> temp(width * height);

      for (u32 y = 0; y < height; y++)
      {
        const u8* pixels_in = reinterpret_cast<const u8*>(texture_data.data()) + (y * texture_data_stride);
        u8* pixels_out = reinterpret_cast<u8*>(temp.data()) + (y * width * sizeof(u32));

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
          const u32 rgba8 = ZeroExtend32((r5 << 3) | (r5 & 7)) | (ZeroExtend32((g6 << 3) | (g6 & 7)) << 8) |
                            (ZeroExtend32((b5 << 3) | (b5 & 7)) << 16) | (a1 ? 0xFF000000u : 0u);
          std::memcpy(pixels_out, &rgba8, sizeof(u32));
          pixels_out += sizeof(u32);
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

void GPUTexture::FlipTextureDataRGBA8(u32 width, u32 height, u8* texture_data, u32 texture_data_stride)
{
  std::unique_ptr<u8[]> temp = std::make_unique<u8[]>(texture_data_stride);
  for (u32 flip_row = 0; flip_row < (height / 2); flip_row++)
  {
    u8* top_ptr = &texture_data[flip_row * texture_data_stride];
    u8* bottom_ptr = &texture_data[((height - 1) - flip_row) * texture_data_stride];
    std::memcpy(temp.get(), top_ptr, texture_data_stride);
    std::memcpy(top_ptr, bottom_ptr, texture_data_stride);
    std::memcpy(bottom_ptr, temp.get(), texture_data_stride);
  }
}

void GPUTexture::MakeReadyForSampling()
{
}

GPUDownloadTexture::GPUDownloadTexture(u32 width, u32 height, GPUTexture::Format format, bool is_imported)
  : m_width(width), m_height(height), m_format(format), m_is_imported(is_imported)
{
}

GPUDownloadTexture::~GPUDownloadTexture() = default;

u32 GPUDownloadTexture::GetBufferSize(u32 width, u32 height, GPUTexture::Format format, u32 pitch_align /* = 1 */)
{
  DebugAssert(std::has_single_bit(pitch_align));
  const u32 bytes_per_pixel = GPUTexture::GetPixelSize(format);
  const u32 pitch = Common::AlignUpPow2(width * bytes_per_pixel, pitch_align);
  return (pitch * height);
}

u32 GPUDownloadTexture::GetTransferPitch(u32 width, u32 pitch_align) const
{
  DebugAssert(std::has_single_bit(pitch_align));
  const u32 bytes_per_pixel = GPUTexture::GetPixelSize(m_format);
  return Common::AlignUpPow2(width * bytes_per_pixel, pitch_align);
}

void GPUDownloadTexture::GetTransferSize(u32 x, u32 y, u32 width, u32 height, u32 pitch, u32* copy_offset,
                                         u32* copy_size, u32* copy_rows) const
{
  const u32 bytes_per_pixel = GPUTexture::GetPixelSize(m_format);
  *copy_offset = (y * pitch) + (x * bytes_per_pixel);
  *copy_size = width * bytes_per_pixel;
  *copy_rows = height;
}

bool GPUDownloadTexture::ReadTexels(u32 x, u32 y, u32 width, u32 height, void* out_ptr, u32 out_stride)
{
  if (m_needs_flush)
    Flush();

  // if we're imported, and this is the same buffer, bail out
  if (m_map_pointer == out_ptr)
  {
    // but stride should match
    DebugAssert(x == 0 && y == 0 && width <= m_width && height <= m_height && out_stride == m_current_pitch);
    return true;
  }

  if (!Map(x, y, width, height))
    return false;

  u32 copy_offset, copy_size, copy_rows;
  GetTransferSize(x, y, width, height, m_current_pitch, &copy_offset, &copy_size, &copy_rows);
  StringUtil::StrideMemCpy(out_ptr, out_stride, m_map_pointer + copy_offset, m_current_pitch, copy_size, copy_rows);
  return true;
}
