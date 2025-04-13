// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/align.h"
#include "common/heap_array.h"
#include "common/intrin.h"
#include "common/types.h"

#include <cstdio>
#include <optional>
#include <span>
#include <string_view>

class Error;

enum class ImageFormat : u8
{
  None,
  RGBA8,
  BGRA8,
  RGB565,
  RGB5A1,
  A1BGR5,
  BGR8,
  BC1,
  BC2,
  BC3,
  BC7,
  MaxCount,
};

class Image
{
public:
  static constexpr u8 DEFAULT_SAVE_QUALITY = 85;

public:
  using PixelStorage = Common::unique_aligned_ptr<u8[]>;

  Image();
  Image(u32 width, u32 height, ImageFormat format);
  Image(u32 width, u32 height, ImageFormat format, const void* pixels, u32 pitch);
  Image(u32 width, u32 height, ImageFormat format, PixelStorage pixels, u32 pitch);
  Image(const Image& copy);
  Image(Image&& move);

  Image& operator=(const Image& copy);
  Image& operator=(Image&& move);

  static const char* GetFormatName(ImageFormat format);
  static u32 GetPixelSize(ImageFormat format);
  static bool IsCompressedFormat(ImageFormat format);
  static u32 CalculatePitch(u32 width, u32 height, ImageFormat format);
  static u32 CalculateStorageSize(u32 width, u32 height, ImageFormat format);
  static u32 CalculateStorageSize(u32 width, u32 height, u32 pitch, ImageFormat format);

  ALWAYS_INLINE bool IsValid() const { return (m_width > 0 && m_height > 0); }
  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetPitch() const { return m_pitch; }
  ALWAYS_INLINE ImageFormat GetFormat() const { return m_format; }
  ALWAYS_INLINE const u8* GetPixels() const { return std::assume_aligned<VECTOR_ALIGNMENT>(m_pixels.get()); }
  ALWAYS_INLINE u8* GetPixels() { return std::assume_aligned<VECTOR_ALIGNMENT>(m_pixels.get()); }
  ALWAYS_INLINE const u8* GetRowPixels(u32 y) const { return &m_pixels[y * m_pitch]; }
  ALWAYS_INLINE u8* GetRowPixels(u32 y) { return &m_pixels[y * m_pitch]; }

  u32 GetBlocksWide() const;
  u32 GetBlocksHigh() const;
  u32 GetStorageSize() const;

  std::span<const u8> GetPixelsSpan() const;
  std::span<u8> GetPixelsSpan();

  void Clear();
  void Invalidate();

  void Resize(u32 new_width, u32 new_height, bool preserve);
  void Resize(u32 new_width, u32 new_height, ImageFormat format, bool preserve);

  void SetPixels(u32 width, u32 height, ImageFormat format, const void* pixels, u32 pitch);
  void SetPixels(u32 width, u32 height, ImageFormat format, PixelStorage pixels, u32 pitch);

  bool SetAllPixelsOpaque();

  PixelStorage TakePixels();

  bool LoadFromFile(const char* filename, Error* error = nullptr);
  bool LoadFromFile(std::string_view filename, std::FILE* fp, Error* error = nullptr);
  bool LoadFromBuffer(std::string_view filename, std::span<const u8> data, Error* error = nullptr);

  bool RasterizeSVG(const std::span<const u8> data, u32 width, u32 height, Error* error = nullptr);

  bool SaveToFile(const char* filename, u8 quality = DEFAULT_SAVE_QUALITY, Error* error = nullptr) const;
  bool SaveToFile(std::string_view filename, std::FILE* fp, u8 quality = DEFAULT_SAVE_QUALITY,
                  Error* error = nullptr) const;
  std::optional<DynamicHeapArray<u8>> SaveToBuffer(std::string_view filename, u8 quality = DEFAULT_SAVE_QUALITY,
                                                   Error* error = nullptr) const;

  std::optional<Image> ConvertToRGBA8(Error* error = nullptr) const;

  static bool ConvertToRGBA8(void* RESTRICT pixels_out, u32 pixels_out_pitch, const void* RESTRICT pixels_in,
                             u32 pixels_in_pitch, u32 width, u32 height, ImageFormat format, Error* error = nullptr);

  void FlipY();

protected:
  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_pitch = 0;
  ImageFormat m_format = ImageFormat::None;
  PixelStorage m_pixels;
};
