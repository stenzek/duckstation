#pragma once
#include "types.h"
#include <algorithm>
#include <vector>

class GPUTexture
{
public:
  enum : u32
  {
    MAX_WIDTH = 65535,
    MAX_HEIGHT = 65535,
    MAX_LAYERS = 255,
    MAX_LEVELS = 255,
    MAX_SAMPLES = 255,
  };

  enum class Format : u8
  {
    Unknown,
    RGBA8,
    BGRA8,
    RGB565,
    RGBA5551,
    R8,
    D16,
    Count
  };

public:
  virtual ~GPUTexture();

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetLayers() const { return m_layers; }
  ALWAYS_INLINE u32 GetLevels() const { return m_levels; }
  ALWAYS_INLINE u32 GetSamples() const { return m_samples; }
  ALWAYS_INLINE GPUTexture::Format GetFormat() const { return m_format; }

  ALWAYS_INLINE bool IsTextureArray() const { return m_layers > 1; }
  ALWAYS_INLINE bool IsMultisampled() const { return m_samples > 1; }

  ALWAYS_INLINE u32 GetPixelSize() const { return GetPixelSize(m_format); }
  ALWAYS_INLINE u32 GetMipWidth(u32 level) const { return std::max<u32>(m_width >> level, 1u); }
  ALWAYS_INLINE u32 GetMipHeight(u32 level) const { return std::max<u32>(m_height >> level, 1u); }

  virtual bool IsValid() const = 0;

  static u32 GetPixelSize(GPUTexture::Format format);
  static bool IsDepthFormat(GPUTexture::Format format);

  static bool ConvertTextureDataToRGBA8(u32 width, u32 height, std::vector<u32>& texture_data, u32& texture_data_stride,
                                        GPUTexture::Format format);
  static void FlipTextureDataRGBA8(u32 width, u32 height, std::vector<u32>& texture_data, u32 texture_data_stride);

protected:
  GPUTexture();
  GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, Format format);

  void ClearBaseProperties();

  u16 m_width = 0;
  u16 m_height = 0;
  u8 m_layers = 0;
  u8 m_levels = 0;
  u8 m_samples = 0;
  Format m_format = Format::Unknown;
};
