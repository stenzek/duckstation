#include "gpu_texture.h"
#include "log.h"
#include "string_util.h"
Log_SetChannel(GPUTexture);

GPUTexture::GPUTexture() = default;

GPUTexture::GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, GPUTexture::Format format)
  : m_width(width), m_height(height), m_layers(layers), m_levels(levels), m_samples(samples), m_format(format)
{
}

GPUTexture::~GPUTexture() = default;

void GPUTexture::ClearBaseProperties()
{
  m_width = 0;
  m_height = 0;
  m_layers = 0;
  m_levels = 0;
  m_samples = 0;
  m_format = GPUTexture::Format::Unknown;
}

u32 GPUTexture::GPUTexture::GetPixelSize(GPUTexture::Format format)
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

bool GPUTexture::ConvertTextureDataToRGBA8(u32 width, u32 height, std::vector<u32>& texture_data,
                                           u32& texture_data_stride, GPUTexture::Format format)
{
  switch (format)
  {
    case GPUTexture::Format::BGRA8:
    {
      for (u32 y = 0; y < height; y++)
      {
        u32* pixels = reinterpret_cast<u32*>(reinterpret_cast<u8*>(texture_data.data()) + (y * texture_data_stride));
        for (u32 x = 0; x < width; x++)
          pixels[x] = (pixels[x] & 0xFF00FF00) | ((pixels[x] & 0xFF) << 16) | ((pixels[x] >> 16) & 0xFF);
      }

      return true;
    }

    case GPUTexture::Format::RGBA8:
      return true;

    case GPUTexture::Format::RGB565:
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

    case GPUTexture::Format::RGBA5551:
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