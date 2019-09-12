#pragma once
#include "glad.h"
#include "types.h"

namespace GL {
class Texture
{
public:
  Texture(u32 width, u32 height, GLenum format, GLenum type, const void* data = nullptr, bool linear_filter = false);
  ~Texture();

  GLuint GetGLId() const { return m_id; }
  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }

  void Bind();

private:
  GLuint m_id;
  u32 m_width;
  u32 m_height;
};

} // namespace GL