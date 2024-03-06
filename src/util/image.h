// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/assert.h"
#include "common/types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

template<typename PixelType>
class Image
{
public:
  Image() = default;
  Image(u32 width, u32 height) { SetSize(width, height); }
  Image(u32 width, u32 height, const PixelType* pixels) { SetPixels(width, height, pixels); }
  Image(u32 width, u32 height, std::vector<PixelType> pixels) { SetPixels(width, height, std::move(pixels)); }
  Image(const Image& copy)
  {
    m_width = copy.m_width;
    m_height = copy.m_height;
    m_pixels = copy.m_pixels;
  }
  Image(Image&& move)
  {
    m_width = move.m_width;
    m_height = move.m_height;
    m_pixels = std::move(move.m_pixels);
    move.m_width = 0;
    move.m_height = 0;
  }

  Image& operator=(const Image& copy)
  {
    m_width = copy.m_width;
    m_height = copy.m_height;
    m_pixels = copy.m_pixels;
    return *this;
  }
  Image& operator=(Image&& move)
  {
    m_width = move.m_width;
    m_height = move.m_height;
    m_pixels = std::move(move.m_pixels);
    move.m_width = 0;
    move.m_height = 0;
    return *this;
  }

  ALWAYS_INLINE bool IsValid() const { return (m_width > 0 && m_height > 0); }
  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetPitch() const { return (sizeof(PixelType) * m_width); }
  ALWAYS_INLINE const PixelType* GetPixels() const { return m_pixels.data(); }
  ALWAYS_INLINE PixelType* GetPixels() { return m_pixels.data(); }
  ALWAYS_INLINE const PixelType* GetRowPixels(u32 y) const { return &m_pixels[y * m_width]; }
  ALWAYS_INLINE PixelType* GetRowPixels(u32 y) { return &m_pixels[y * m_width]; }
  ALWAYS_INLINE void SetPixel(u32 x, u32 y, PixelType pixel) { m_pixels[y * m_width + x] = pixel; }
  ALWAYS_INLINE PixelType GetPixel(u32 x, u32 y) const { return m_pixels[y * m_width + x]; }

  void Clear(PixelType fill_value = static_cast<PixelType>(0))
  {
    std::fill(m_pixels.begin(), m_pixels.end(), fill_value);
  }

  void Invalidate()
  {
    m_width = 0;
    m_height = 0;
    m_pixels.clear();
  }

  void SetSize(u32 new_width, u32 new_height, PixelType fill_value = static_cast<PixelType>(0))
  {
    m_width = new_width;
    m_height = new_height;
    m_pixels.resize(new_width * new_height);
    Clear(fill_value);
  }

  void SetPixels(u32 width, u32 height, const PixelType* pixels)
  {
    m_width = width;
    m_height = height;
    m_pixels.resize(width * height);
    std::memcpy(m_pixels.data(), pixels, width * height * sizeof(PixelType));
  }

  void SetPixels(u32 width, u32 height, std::vector<PixelType> pixels)
  {
    m_width = width;
    m_height = height;
    m_pixels = std::move(pixels);
  }

  std::vector<PixelType> TakePixels()
  {
    m_width = 0;
    m_height = 0;
    return std::move(m_pixels);
  }

protected:
  u32 m_width = 0;
  u32 m_height = 0;
  std::vector<PixelType> m_pixels;
};

class RGBA8Image : public Image<u32>
{
public:
  static constexpr u8 DEFAULT_SAVE_QUALITY = 85;

  RGBA8Image();
  RGBA8Image(u32 width, u32 height);
  RGBA8Image(u32 width, u32 height, const u32* pixels);
  RGBA8Image(u32 width, u32 height, std::vector<u32> pixels);
  RGBA8Image(const RGBA8Image& copy);
  RGBA8Image(RGBA8Image&& move);

  RGBA8Image& operator=(const RGBA8Image& copy);
  RGBA8Image& operator=(RGBA8Image&& move);

  bool LoadFromFile(const char* filename);
  bool LoadFromFile(const char* filename, std::FILE* fp);
  bool LoadFromBuffer(const char* filename, const void* buffer, size_t buffer_size);

  bool SaveToFile(const char* filename, u8 quality = DEFAULT_SAVE_QUALITY) const;
  bool SaveToFile(const char* filename, std::FILE* fp, u8 quality = DEFAULT_SAVE_QUALITY) const;
  std::optional<std::vector<u8>> SaveToBuffer(const char* filename, u8 quality = DEFAULT_SAVE_QUALITY) const;
};
