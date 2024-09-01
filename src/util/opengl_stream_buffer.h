// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "opengl_loader.h"

#include "common/types.h"

#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

class OpenGLStreamBuffer
{
public:
  virtual ~OpenGLStreamBuffer();

  ALWAYS_INLINE GLuint GetGLBufferId() const { return m_buffer_id; }
  ALWAYS_INLINE GLenum GetGLTarget() const { return m_target; }
  ALWAYS_INLINE u32 GetSize() const { return m_size; }

  void Bind();
  void Unbind();

  void SetDebugName(std::string_view name);

  struct MappingResult
  {
    void* pointer;
    u32 buffer_offset;
    u32 index_aligned; // offset / alignment, suitable for base vertex
    u32 space_aligned; // remaining space / alignment
  };

  virtual MappingResult Map(u32 alignment, u32 min_size) = 0;

  /// Returns the position in the buffer *before* the start of used_size.
  virtual u32 Unmap(u32 used_size) = 0;

  /// Returns the minimum granularity of blocks which sync objects will be created around.
  virtual u32 GetChunkSize() const = 0;

  static std::unique_ptr<OpenGLStreamBuffer> Create(GLenum target, u32 size);

protected:
  OpenGLStreamBuffer(GLenum target, GLuint buffer_id, u32 size);

  GLenum m_target;
  GLuint m_buffer_id;
  u32 m_size;
};
