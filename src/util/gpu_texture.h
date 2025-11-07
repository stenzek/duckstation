// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/gsvector.h"
#include "common/small_string.h"
#include "common/types.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

class Error;

enum class ImageFormat : u8;

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
    Texture,
    RenderTarget,
    DepthStencil,
  };

  enum class Format : u8
  {
    Unknown,
    RGBA8,
    BGRA8,
    RGB565,
    RGB5A1,
    A1BGR5,
    R8,
    D16,
    D24S8,
    D32F,
    D32FS8,
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
    SRGBA8,
    BC1, ///< BC1, aka DXT1 compressed texture
    BC2, ///< BC2, aka DXT2/3 compressed texture
    BC3, ///< BC3, aka DXT4/5 compressed texture
    BC7, ///< BC7, aka BPTC compressed texture
    MaxCount,
  };

  enum class State : u8
  {
    Dirty,
    Cleared,
    Invalidated
  };

  enum class Flags : u8
  {
    None = 0,
    AllowMap = (1 << 0),
    AllowBindAsImage = (1 << 2),
    AllowGenerateMipmaps = (1 << 3),
    AllowMSAAResolveTarget = (1 << 4),
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
  static u32 GetPixelSize(Format format);
  static bool IsDepthFormat(Format format);
  static bool IsDepthStencilFormat(Format format);
  static bool IsCompressedFormat(Format format);
  static u32 GetBlockSize(Format format);
  static u32 CalcUploadPitch(Format format, u32 width);
  static u32 CalcUploadRowLengthFromPitch(Format format, u32 pitch);
  static u32 CalcUploadSize(Format format, u32 height, u32 pitch);
  static u32 GetFullMipmapCount(u32 width, u32 height);
  static void CopyTextureDataForUpload(u32 width, u32 height, Format format, void* dst, u32 dst_pitch, const void* src,
                                       u32 src_pitch);

  static Format GetTextureFormatForImageFormat(ImageFormat format);
  static ImageFormat GetImageFormatForTextureFormat(Format format);

  static bool ValidateConfig(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                             Flags flags, Error* error);

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE u32 GetLayers() const { return m_layers; }
  ALWAYS_INLINE u32 GetLevels() const { return m_levels; }
  ALWAYS_INLINE u32 GetSamples() const { return m_samples; }
  ALWAYS_INLINE Type GetType() const { return m_type; }
  ALWAYS_INLINE Format GetFormat() const { return m_format; }
  ALWAYS_INLINE Flags GetFlags() const { return m_flags; }
  ALWAYS_INLINE bool HasFlag(Flags flag) const { return ((static_cast<u8>(m_flags) & static_cast<u8>(flag)) != 0); }
  ALWAYS_INLINE GSVector2i GetSizeVec() const { return GSVector2i(m_width, m_height); }
  ALWAYS_INLINE GSVector4i GetRect() const
  {
    return GSVector4i(0, 0, static_cast<s32>(m_width), static_cast<s32>(m_height));
  }

  ALWAYS_INLINE bool IsTextureArray() const { return m_layers > 1; }
  ALWAYS_INLINE bool IsMultisampled() const { return m_samples > 1; }

  ALWAYS_INLINE u32 GetPixelSize() const { return GetPixelSize(m_format); }
  ALWAYS_INLINE u32 GetMipWidth(u32 level) const { return std::max<u32>(m_width >> level, 1u); }
  ALWAYS_INLINE u32 GetMipHeight(u32 level) const { return std::max<u32>(m_height >> level, 1u); }

  ALWAYS_INLINE State GetState() const { return m_state; }
  ALWAYS_INLINE void SetState(State state) { m_state = state; }
  ALWAYS_INLINE bool IsDirty() const { return (m_state == State::Dirty); }
  ALWAYS_INLINE bool IsClearedOrInvalidated() const { return (m_state != State::Dirty); }

  ALWAYS_INLINE bool IsTexture() const { return (m_type == Type::Texture); }
  ALWAYS_INLINE bool IsRenderTarget() const { return (m_type == Type::RenderTarget); }
  ALWAYS_INLINE bool IsDepthStencil() const { return (m_type == Type::DepthStencil); }
  ALWAYS_INLINE bool IsRenderTargetOrDepthStencil() const
  {
    return (m_type >= Type::RenderTarget && m_type <= Type::DepthStencil);
  }

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

  bool IsCompressedFormat() const;
  u32 GetBlockSize() const;
  u32 CalcUploadPitch(u32 width) const;
  u32 CalcUploadRowLengthFromPitch(u32 pitch) const;
  u32 CalcUploadSize(u32 height, u32 pitch) const;

  GPUTexture& operator=(const GPUTexture&) = delete;

  virtual bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0,
                      u32 level = 0) = 0;
  virtual bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) = 0;
  virtual void Unmap() = 0;

  virtual void GenerateMipmaps() = 0;

  // Instructs the backend that we're finished rendering to this texture. It may transition it to a new layout.
  virtual void MakeReadyForSampling();

#if defined(_DEBUG) || defined(_DEVEL)
  virtual void SetDebugName(std::string_view name) = 0;
  template<typename... T>
  void SetDebugName(fmt::format_string<T...> fmt, T&&... args)
  {
    SetDebugName(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
#endif

protected:
  GPUTexture(u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type, Format format, Flags flags);

  static constexpr u32 COMPRESSED_TEXTURE_BLOCK_SIZE = 4;

  u16 m_width = 0;
  u16 m_height = 0;
  u8 m_layers = 0;
  u8 m_levels = 0;
  u8 m_samples = 0;
  Type m_type = Type::Texture;
  Format m_format = Format::Unknown;
  Flags m_flags = Flags::None;

  State m_state = State::Dirty;

  ClearValue m_clear_value = {};
};

IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(GPUTexture::Flags);

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

#if defined(_DEBUG) || defined(_DEVEL)
  /// Sets object name that will be displayed in graphics debuggers.
  virtual void SetDebugName(std::string_view name) = 0;
  template<typename... T>
  void SetDebugName(fmt::format_string<T...> fmt, T&&... args)
  {
    SetDebugName(TinyString::from_vformat(fmt, fmt::make_format_args(args...)));
  }
#endif

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
