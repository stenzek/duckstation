// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/align.h"
#include "common/heap_array.h"
#include "common/types.h"

#include <cstdio>
#include <optional>
#include <span>
#include <string_view>

class Error;

class AnimatedImage
{
public:
  struct FrameDelay
  {
    u16 numerator;
    u16 denominator;
  };

  static constexpr u8 DEFAULT_SAVE_QUALITY = 85;

public:
  using PixelType = u32;
  using PixelStorage = DynamicHeapArray<PixelType>;

  AnimatedImage();
  AnimatedImage(u32 width, u32 height, u32 frames, const FrameDelay& default_delay);
  AnimatedImage(const AnimatedImage& copy);
  AnimatedImage(AnimatedImage&& move);

  AnimatedImage& operator=(const AnimatedImage& copy);
  AnimatedImage& operator=(AnimatedImage&& move);

  static u32 CalculatePitch(u32 width, u32 height);

  ALWAYS_INLINE bool IsValid() const { return (m_width > 0 && m_height > 0); }
  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetPitch() const { return (m_width * sizeof(u32)); }
  ALWAYS_INLINE u32 GetFrames() const { return m_frames; }
  ALWAYS_INLINE u32 GetFrameSize() const { return m_frame_size; }
  ALWAYS_INLINE const u32* GetPixels(u32 frame) const { return &m_pixels[frame * m_frame_size]; }
  ALWAYS_INLINE PixelType* GetPixels(u32 frame) { return &m_pixels[frame * m_frame_size]; }
  ALWAYS_INLINE const PixelType* GetRowPixels(u32 frame, u32 y) const
  {
    return &m_pixels[frame * m_frame_size + y * m_width];
  }
  ALWAYS_INLINE PixelType* GetRowPixels(u32 frame, u32 y) { return &m_pixels[frame * m_frame_size + y * m_width]; }
  ALWAYS_INLINE const FrameDelay& GetFrameDelay(u32 frame) const { return m_frame_delay[frame]; }

  std::span<const PixelType> GetPixelsSpan(u32 frame) const;
  std::span<PixelType> GetPixelsSpan(u32 frame);

  void Clear();
  void Invalidate();

  void Resize(u32 new_width, u32 new_height, u32 num_frames, const FrameDelay& default_delay, bool preserve);

  void SetPixels(u32 frame, const void* pixels, u32 pitch);
  void SetDelay(u32 frame, const FrameDelay& delay);

  PixelStorage TakePixels();

  bool LoadFromFile(const char* filename, Error* error = nullptr);
  bool LoadFromFile(std::string_view filename, std::FILE* fp, Error* error = nullptr);
  bool LoadFromBuffer(std::string_view filename, std::span<const u8> data, Error* error = nullptr);

  bool SaveToFile(const char* filename, u8 quality = DEFAULT_SAVE_QUALITY, Error* error = nullptr) const;
  bool SaveToFile(std::string_view filename, std::FILE* fp, u8 quality = DEFAULT_SAVE_QUALITY,
                  Error* error = nullptr) const;
  std::optional<DynamicHeapArray<u8>> SaveToBuffer(std::string_view filename, u8 quality = DEFAULT_SAVE_QUALITY,
                                                   Error* error = nullptr) const;

protected:
  using FrameDelayStorage = DynamicHeapArray<FrameDelay>;

  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_frame_size = 0;
  u32 m_frames = 0;
  PixelStorage m_pixels;
  FrameDelayStorage m_frame_delay;
};
