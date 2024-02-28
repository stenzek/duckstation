// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "gpu_device.h"
#include "gpu_texture.h"
#include "opengl_loader.h"
#include <tuple>

class OpenGLDevice;
class OpenGLStreamBuffer;

class OpenGLTexture final : public GPUTexture
{
  friend OpenGLDevice;

public:
  OpenGLTexture(const OpenGLTexture&) = delete;
  ~OpenGLTexture();

  static bool UseTextureStorage(bool multisampled);
  static const std::tuple<GLenum, GLenum, GLenum>& GetPixelFormatMapping(Format format, bool gles);

  ALWAYS_INLINE GLuint GetGLId() const { return m_id; }

  bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0, u32 level = 0) override;
  bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) override;
  void Unmap() override;

  void SetDebugName(const std::string_view& name) override;

  static std::unique_ptr<OpenGLTexture> Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type,
                                               Format format, const void* data = nullptr, u32 data_pitch = 0);

  bool UseTextureStorage() const;

  ALWAYS_INLINE GLenum GetGLTarget() const
  {
    return (IsMultisampled() ? GL_TEXTURE_2D_MULTISAMPLE : (IsTextureArray() ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D));
  }

  void CommitClear();

  OpenGLTexture& operator=(const OpenGLTexture&) = delete;

private:
  OpenGLTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format, GLuint id);

  GLuint m_id = 0;

  u32 m_map_offset = 0;
  u16 m_map_x = 0;
  u16 m_map_y = 0;
  u16 m_map_width = 0;
  u16 m_map_height = 0;
  u8 m_map_layer = 0;
  u8 m_map_level = 0;
};

class OpenGLTextureBuffer final : public GPUTextureBuffer
{
  friend OpenGLDevice;

public:
  ~OpenGLTextureBuffer() override;

  ALWAYS_INLINE OpenGLStreamBuffer* GetBuffer() const { return m_buffer.get(); }
  ALWAYS_INLINE GLuint GetTextureId() const { return m_texture_id; }

  bool CreateBuffer();

  // Inherited via GPUTextureBuffer
  void* Map(u32 required_elements) override;
  void Unmap(u32 used_elements) override;

  void SetDebugName(const std::string_view& name) override;

private:
  OpenGLTextureBuffer(Format format, u32 size_in_elements, std::unique_ptr<OpenGLStreamBuffer> buffer,
                      GLuint texture_id);

  std::unique_ptr<OpenGLStreamBuffer> m_buffer;
  GLuint m_texture_id;
};

class OpenGLSampler final : public GPUSampler
{
  friend OpenGLDevice;

public:
  ~OpenGLSampler() override;

  ALWAYS_INLINE GLuint GetID() const { return m_id; }

  void SetDebugName(const std::string_view& name) override;

private:
  OpenGLSampler(GLuint id);

  GLuint m_id;
};

class OpenGLDownloadTexture final : public GPUDownloadTexture
{
public:
  ~OpenGLDownloadTexture() override;

  static std::unique_ptr<OpenGLDownloadTexture> Create(u32 width, u32 height, GPUTexture::Format format, void* memory,
                                                       size_t memory_size, u32 memory_pitch);

  void CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height,
                       u32 src_layer, u32 src_level, bool use_transfer_pitch) override;

  bool Map(u32 x, u32 y, u32 width, u32 height) override;
  void Unmap() override;

  void Flush() override;

  void SetDebugName(std::string_view name) override;

private:
  OpenGLDownloadTexture(u32 width, u32 height, GPUTexture::Format format, bool imported, GLuint buffer_id,
                        u8* cpu_buffer, u32 buffer_size, const u8* map_ptr, u32 map_pitch);

  GLuint m_buffer_id = 0;
  u32 m_buffer_size = 0;

  GLsync m_sync = {};

  // used when buffer storage is not available
  u8* m_cpu_buffer = nullptr;
};
