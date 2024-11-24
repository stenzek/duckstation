// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_texture.h"
#include "gpu_device.h"
#include "image.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/string_util.h"

GPUTexture::GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type, Format format, Flags flags)
  : m_width(width), m_height(height), m_layers(layers), m_levels(levels), m_samples(samples), m_type(type),
    m_format(format), m_flags(flags)
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
    "D24S8",   // D24S8
    "D32F",    // D32F
    "D32FS8S", // D32FS8
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

u32 GPUTexture::GetFullMipmapCount(u32 width, u32 height)
{
  const u32 max_dim = Common::PreviousPow2(std::max(width, height));
  return (std::countr_zero(max_dim) + 1);
}

GPUTexture::Format GPUTexture::GetTextureFormatForImageFormat(ImageFormat format)
{
  static constexpr const std::array<Format, static_cast<size_t>(ImageFormat::MaxCount)> mapping = {{
    Format::Unknown, // None
    Format::RGBA8,   // RGBA8
    Format::BGRA8,   // BGRA8
    Format::RGB565,  // RGB565
    Format::Unknown, // RGBA5551
    Format::Unknown, // BC1
    Format::Unknown, // BC2
    Format::Unknown, // BC3
    Format::Unknown, // BC7
  }};

  return mapping[static_cast<size_t>(format)];
}

ImageFormat GPUTexture::GetImageFormatForTextureFormat(Format format)
{
  static constexpr const std::array<ImageFormat, static_cast<size_t>(Format::MaxCount)> mapping = {{
    ImageFormat::None,     // Unknown
    ImageFormat::RGBA8,    // RGBA8
    ImageFormat::BGRA8,    // BGRA8
    ImageFormat::RGB565,   // RGB565
    ImageFormat::RGBA5551, // RGBA5551
    ImageFormat::None,     // R8
    ImageFormat::None,     // D16
    ImageFormat::None,     // D24S8
    ImageFormat::None,     // D32F
    ImageFormat::None,     // D32FS8
    ImageFormat::None,     // R16
    ImageFormat::None,     // R16I
    ImageFormat::None,     // R16U
    ImageFormat::None,     // R16F
    ImageFormat::None,     // R32I
    ImageFormat::None,     // R32U
    ImageFormat::None,     // R32F
    ImageFormat::None,     // RG8
    ImageFormat::None,     // RG16
    ImageFormat::None,     // RG16F
    ImageFormat::None,     // RG32F
    ImageFormat::None,     // RGBA16
    ImageFormat::None,     // RGBA16F
    ImageFormat::None,     // RGBA32F
    ImageFormat::None,     // RGB10A2
  }};

  return mapping[static_cast<size_t>(format)];
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
    4,  // D24S8
    4,  // D32F
    8,  // D32FS8
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
  return (format >= Format::D16 && format <= Format::D32FS8);
}

bool GPUTexture::IsDepthStencilFormat(Format format)
{
  return (format == Format::D24S8 || format == Format::D32FS8);
}

bool GPUTexture::IsCompressedFormat(Format format)
{
  // TODO: Implement me
  return false;
}

bool GPUTexture::ValidateConfig(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                                Flags flags, Error* error)
{
  if (width == 0 || width > MAX_WIDTH || height == 0 || height > MAX_HEIGHT || layers == 0 || layers > MAX_LAYERS ||
      levels == 0 || levels > MAX_LEVELS || samples == 0 || samples > MAX_SAMPLES)
  {
    Error::SetStringFmt(error, "Invalid dimensions: {}x{}x{} {} {}.", width, height, layers, levels, samples);
    return false;
  }

  const u32 max_texture_size = g_gpu_device->GetMaxTextureSize();
  if (width > max_texture_size || height > max_texture_size)
  {
    Error::SetStringFmt(error, "Texture width ({}) or height ({}) exceeds max texture size ({}).", width, height,
                        max_texture_size);
    return false;
  }

  const u32 max_samples = g_gpu_device->GetMaxMultisamples();
  if (samples > max_samples)
  {
    Error::SetStringFmt(error, "Texture samples ({}) exceeds max samples ({}).", samples, max_samples);
    return false;
  }

  if (samples > 1)
  {
    if (levels > 1)
    {
      Error::SetStringView(error, "Multisampled textures can't have mip levels.");
      return false;
    }
    else if (type != Type::RenderTarget && type != Type::DepthStencil)
    {
      Error::SetStringView(error, "Multisampled textures must be render targets or depth stencil targets.");
      return false;
    }
  }

  if (layers > 1 && type != Type::Texture)
  {
    Error::SetStringView(error, "Texture arrays are not supported on targets.");
    return false;
  }

  if (levels > 1 && type != Type::Texture)
  {
    Error::SetStringView(error, "Mipmaps are not supported on targets.");
    return false;
  }

  if ((flags & Flags::AllowGenerateMipmaps) != Flags::None && levels <= 1)
  {
    Error::SetStringView(error, "Allow generate mipmaps requires >1 level.");
    return false;
  }

  if ((flags & Flags::AllowBindAsImage) != Flags::None &&
      ((type != Type::Texture && type != Type::RenderTarget) || levels > 1))
  {
    Error::SetStringView(error, "Bind as image is not allowed on depth or mipmapped targets.");
    return false;
  }

  if ((flags & Flags::AllowMap) != Flags::None &&
      (type != Type::Texture || (flags & Flags::AllowGenerateMipmaps) != Flags::None))
  {
    Error::SetStringView(error, "Allow map is not supported on targets.");
    return false;
  }

  return true;
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
