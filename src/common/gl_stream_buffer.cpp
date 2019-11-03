#include "gl_stream_buffer.h"

namespace GL {

StreamBuffer::StreamBuffer(GLenum target, GLuint buffer_id, u32 size)
  : m_target(target), m_buffer_id(buffer_id), m_size(size), m_cpu_buffer(size)
{
}

StreamBuffer::~StreamBuffer()
{
  glDeleteBuffers(1, &m_buffer_id);
}

void StreamBuffer::Bind()
{
  glBindBuffer(m_target, m_buffer_id);
}

void StreamBuffer::Unbind()
{
  glBindBuffer(m_target, 0);
}

StreamBuffer::MappingResult StreamBuffer::Map(u32 alignment, u32 min_size)
{
  return MappingResult{static_cast<void*>(m_cpu_buffer.data()), 0, 0, m_size / alignment};
}

void StreamBuffer::Unmap(u32 used_size)
{
  if (used_size == 0)
    return;

  glBindBuffer(m_target, m_buffer_id);
  glBufferSubData(m_target, 0, used_size, m_cpu_buffer.data());
}

std::unique_ptr<StreamBuffer> StreamBuffer::Create(GLenum target, u32 size)
{
  glGetError();

  GLuint buffer_id;
  glGenBuffers(1, &buffer_id);
  glBindBuffer(target, buffer_id);
  glBufferData(target, size, nullptr, GL_STREAM_DRAW);

  GLenum err = glGetError();
  if (err != GL_NO_ERROR)
  {
    glDeleteBuffers(1, &buffer_id);
    return {};
  }

  return std::unique_ptr<StreamBuffer>(new StreamBuffer(target, buffer_id, size));
}

} // namespace GL