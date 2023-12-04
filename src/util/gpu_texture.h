// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/types.h"

#include <algorithm>
#include <array>
#include <string_view>
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

  enum class Type : u8
  {
    Unknown,
    RenderTarget,
    DepthStencil,
    Texture,
    RWTexture,
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
    R16,
    R16F,
    R32I,
    R32U,
    R32F,
    RG8,
    RG16,
    RG16F,
    RG32F,
    RGBA16,
    RGBA16F,
    RGBA32F,
    RGB10A2,
    MaxCount
  };

  enum class State : u8
  {
    Dirty,
    Cleared,
    Invalidated
  };

  union ClearValue
  {
    u32 color;
    float depth;
  };

public:
  virtual ~GPUTexture();

  static const char* GetFormatName(Format format);

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetLayers() const { return m_layers; }
  ALWAYS_INLINE u32 GetLevels() const { return m_levels; }
  ALWAYS_INLINE u32 GetSamples() const { return m_samples; }
  ALWAYS_INLINE Type GetType() const { return m_type; }
  ALWAYS_INLINE Format GetFormat() const { return m_format; }

  ALWAYS_INLINE bool IsTextureArray() const { return m_layers > 1; }
  ALWAYS_INLINE bool IsMultisampled() const { return m_samples > 1; }

  ALWAYS_INLINE u32 GetPixelSize() const { return GetPixelSize(m_format); }
  ALWAYS_INLINE u32 GetMipWidth(u32 level) const { return std::max<u32>(m_width >> level, 1u); }
  ALWAYS_INLINE u32 GetMipHeight(u32 level) const { return std::max<u32>(m_height >> level, 1u); }

  ALWAYS_INLINE State GetState() const { return m_state; }
  ALWAYS_INLINE void SetState(State state) { m_state = state; }
  ALWAYS_INLINE bool IsDirty() const { return (m_state == State::Dirty); }
  ALWAYS_INLINE bool IsClearedOrInvalidated() const { return (m_state != State::Dirty); }

  ALWAYS_INLINE bool IsRenderTargetOrDepthStencil() const
  {
    return (m_type >= Type::RenderTarget && m_type <= Type::DepthStencil);
  }
  ALWAYS_INLINE bool IsRenderTarget() const { return (m_type == Type::RenderTarget); }
  ALWAYS_INLINE bool IsDepthStencil() const { return (m_type == Type::DepthStencil); }
  ALWAYS_INLINE bool IsTexture() const { return (m_type == Type::Texture); }

  ALWAYS_INLINE const ClearValue& GetClearValue() const { return m_clear_value; }
  ALWAYS_INLINE u32 GetClearColor() const { return m_clear_value.color; }
  ALWAYS_INLINE float GetClearDepth() const { return m_clear_value.depth; }
  std::array<float, 4> GetUNormClearColor() const;

  ALWAYS_INLINE void SetClearColor(u32 color)
  {
    m_state = State::Cleared;
    m_clear_value.color = color;
  }
  ALWAYS_INLINE void SetClearDepth(float depth)
  {
    m_state = State::Cleared;
    m_clear_value.depth = depth;
  }

  static u32 GetPixelSize(GPUTexture::Format format);
  static bool IsDepthFormat(GPUTexture::Format format);
  static bool IsDepthStencilFormat(GPUTexture::Format format);
  static bool ValidateConfig(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format);

  static bool ConvertTextureDataToRGBA8(u32 width, u32 height, std::vector<u32>& texture_data, u32& texture_data_stride,
                                        GPUTexture::Format format);
  static void FlipTextureDataRGBA8(u32 width, u32 height, std::vector<u32>& texture_data, u32 texture_data_stride);

  virtual bool IsValid() const = 0;

  virtual bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0,
                      u32 level = 0) = 0;
  virtual bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) = 0;
  virtual void Unmap() = 0;

  // Instructs the backend that we're finished rendering to this texture. It may transition it to a new layout.
  virtual void MakeReadyForSampling();

  virtual void SetDebugName(const std::string_view& name) = 0;

protected:
  GPUTexture();
  GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type, Format format);

  void ClearBaseProperties();

  u16 m_width = 0;
  u16 m_height = 0;
  u8 m_layers = 0;
  u8 m_levels = 0;
  u8 m_samples = 0;
  Type m_type = Type::Unknown;
  Format m_format = Format::Unknown;
  State m_state = State::Dirty;

  ClearValue m_clear_value = {};
};
