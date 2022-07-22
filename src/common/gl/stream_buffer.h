#pragma once
#include "../types.h"
#include "loader.h"
#include <memory>
#include <tuple>
#include <vector>

namespace GL {
class StreamBuffer
{
public:
  virtual ~StreamBuffer();

  ALWAYS_INLINE GLuint GetGLBufferId() const { return m_buffer_id; }
  ALWAYS_INLINE GLenum GetGLTarget() const { return m_target; }
  ALWAYS_INLINE u32 GetSize() const { return m_size; }

  void Bind();
  void Unbind();

  struct MappingResult
  {
    void* pointer;
    u32 buffer_offset;
    u32 index_aligned; // offset / alignment, suitable for base vertex
    u32 space_aligned; // remaining space / alignment
  };

  virtual MappingResult Map(u32 alignment, u32 min_size) = 0;
  virtual void Unmap(u32 used_size) = 0;

  static std::unique_ptr<StreamBuffer> Create(GLenum target, u32 size);

protected:
  StreamBuffer(GLenum target, GLuint buffer_id, u32 size);

  GLenum m_target;
  GLuint m_buffer_id;
  u32 m_size;
};
} // namespace GL