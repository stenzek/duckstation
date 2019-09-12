#include "gl_texture.h"
#include "YBaseLib/Log.h"
Log_SetChannel(GL);

namespace GL {

Texture::Texture(u32 width, u32 height, GLenum format, GLenum type, const void* data /* = nullptr */,
                 bool linear_filter /* = false */)
  : m_width(width), m_height(height)
{
  glGenTextures(1, &m_id);
  glBindTexture(GL_TEXTURE_2D, m_id);
  glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, type, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
}

Texture::~Texture()
{
  glDeleteTextures(1, &m_id);
}

void Texture::Bind()
{
  glBindTexture(GL_TEXTURE_2D, m_id);
}

} // namespace GL