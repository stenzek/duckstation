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
    DynamicTexture,
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
    R16I,
    R16U,
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
  GPUTexture(const GPUTexture&) = delete;
  virtual ~GPUTexture();

  static const char* GetFormatName(Format format);
  static u32 GetPixelSize(GPUTexture::Format format);
  static bool IsDepthFormat(GPUTexture::Format format);
  static bool IsDepthStencilFormat(GPUTexture::Format format);
  static bool IsCompressedFormat(Format format);
  static u32 GetCompressedBytesPerBlock(Format format);
  static u32 GetCompressedBlockSize(Format format);
  static u32 CalcUploadPitch(Format format, u32 width);
  static u32 CalcUploadRowLengthFromPitch(Format format, u32 pitch);
  static u32 CalcUploadSize(Format format, u32 height, u32 pitch);

  static bool ValidateConfig(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format);

  static bool ConvertTextureDataToRGBA8(u32 width, u32 height, std::vector<u32>& texture_data, u32& texture_data_stride,
                                        GPUTexture::Format format);
  static void FlipTextureDataRGBA8(u32 width, u32 height, u8* texture_data, u32 texture_data_stride);

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
  ALWAYS_INLINE bool IsTexture() const { return (m_type == Type::Texture || m_type == Type::DynamicTexture); }
  ALWAYS_INLINE bool IsDynamicTexture() const { return (m_type == Type::DynamicTexture); }

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

  size_t GetVRAMUsage() const;

  u32 GetCompressedBytesPerBlock() const;
  u32 GetCompressedBlockSize() const;
  u32 CalcUploadPitch(u32 width) const;
  u32 CalcUploadRowLengthFromPitch(u32 pitch) const;
  u32 CalcUploadSize(u32 height, u32 pitch) const;

  GPUTexture& operator=(const GPUTexture&) = delete;

  virtual bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0,
                      u32 level = 0) = 0;
  virtual bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) = 0;
  virtual void Unmap() = 0;

  // Instructs the backend that we're finished rendering to this texture. It may transition it to a new layout.
  virtual void MakeReadyForSampling();

  virtual void SetDebugName(const std::string_view& name) = 0;

protected:
  GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type, Format format);

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

class GPUDownloadTexture
{
public:
  GPUDownloadTexture(u32 width, u32 height, GPUTexture::Format format, bool is_imported);
  virtual ~GPUDownloadTexture();

  /// Basically, this has dimensions only because of DX11.
  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE GPUTexture::Format GetFormat() const { return m_format; }
  ALWAYS_INLINE bool NeedsFlush() const { return m_needs_flush; }
  ALWAYS_INLINE bool IsMapped() const { return (m_map_pointer != nullptr); }
  ALWAYS_INLINE bool IsImported() const { return m_is_imported; }
  ALWAYS_INLINE const u8* GetMapPointer() const { return m_map_pointer; }
  ALWAYS_INLINE u32 GetMapPitch() const { return m_current_pitch; }

  /// Calculates the pitch of a transfer.
  u32 GetTransferPitch(u32 width, u32 pitch_align) const;

  /// Calculates the size of the data you should transfer.
  void GetTransferSize(u32 x, u32 y, u32 width, u32 height, u32 pitch, u32* copy_offset, u32* copy_size,
                       u32* copy_rows) const;

  /// Queues a copy from the specified texture to this buffer.
  /// Does not complete immediately, you should flush before accessing the buffer.
  /// use_transfer_pitch should be true if there's only a single texture being copied to this buffer before
  /// it will be used. This allows the image to be packed tighter together, and buffer reuse.
  virtual void CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height,
                               u32 src_layer, u32 src_level, bool use_transfer_pitch = true) = 0;

  /// Maps the texture into the CPU address space, enabling it to read the contents.
  /// The Map call may not perform synchronization. If the contents of the staging texture
  /// has been updated by a CopyFromTexture() call, you must call Flush() first.
  /// If persistent mapping is supported in the backend, this may be a no-op.
  virtual bool Map(u32 x, u32 y, u32 width, u32 height) = 0;

  /// Unmaps the CPU-readable copy of the texture. May be a no-op on backends which
  /// support persistent-mapped buffers.
  virtual void Unmap() = 0;

  /// Flushes pending writes from the CPU to the GPU, and reads from the GPU to the CPU.
  /// This may cause a command buffer submit depending on if one has occurred between the last
  /// call to CopyFromTexture() and the Flush() call.
  virtual void Flush() = 0;

  /// Sets object name that will be displayed in graphics debuggers.
  virtual void SetDebugName(std::string_view name) = 0;

  /// Reads the specified rectangle from the staging texture to out_ptr, with the specified stride
  /// (length in bytes of each row). CopyFromTexture() must be called first. The contents of any
  /// texels outside of the rectangle used for CopyFromTexture is undefined.
  bool ReadTexels(u32 x, u32 y, u32 width, u32 height, void* out_ptr, u32 out_stride);

  /// Returns what the size of the specified texture would be, in bytes.
  static u32 GetBufferSize(u32 width, u32 height, GPUTexture::Format format, u32 pitch_align = 1);

protected:
  u32 m_width;
  u32 m_height;
  GPUTexture::Format m_format;

  const u8* m_map_pointer = nullptr;
  u32 m_current_pitch = 0;

  bool m_is_imported = false;
  bool m_needs_flush = false;
};
