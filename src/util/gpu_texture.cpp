// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "gpu_texture.h"
#include "gpu_device.h"

#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"

Log_SetChannel(GPUTexture);

GPUTexture::GPUTexture() = default;

GPUTexture::GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type, Format format)
  : m_width(width), m_height(height), m_layers(layers), m_levels(levels), m_samples(samples), m_type(type),
    m_format(format)
{
}

GPUTexture::~GPUTexture() = default;

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

void GPUTexture::ClearBaseProperties()
{
  m_width = 0;
  m_height = 0;
  m_layers = 0;
  m_levels = 0;
  m_samples = 0;
  m_type = GPUTexture::Type::Unknown;
  m_format = GPUTexture::Format::Unknown;
  m_state = State::Dirty;
}

std::array<float, 4> GPUTexture::GetUNormClearColor() const
{
  return GPUDevice::RGBA8ToFloat(m_clear_value.color);
}

u32 GPUTexture::GetPixelSize(GPUTexture::Format format)
{
  switch (format)
  {
    case Format::RGBA8:
    case Format::BGRA8:
      return 4;

    case Format::RGBA5551:
    case Format::RGB565:
    case Format::D16:
      return 2;

    case Format::R8:
      return 1;

    default:
      return 0;
  }
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

  if (layers > 1 && type != Type::Texture)
  {
    Log_ErrorPrintf("Texture arrays are not supported on targets.");
    return false;
  }

  if (levels > 1 && type != Type::Texture)
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
        u32* pixels = reinterpret_cast<u32*>(reinterpret_cast<u8*>(texture_data.data()) + (y * texture_data_stride));
        for (u32 x = 0; x < width; x++)
          pixels[x] = (pixels[x] & 0xFF00FF00) | ((pixels[x] & 0xFF) << 16) | ((pixels[x] >> 16) & 0xFF);
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

    case Format::RGBA5551:
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

void GPUTexture::FlipTextureDataRGBA8(u32 width, u32 height, std::vector<u32>& texture_data, u32 texture_data_stride)
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

void GPUTexture::MakeReadyForSampling()
{
}
