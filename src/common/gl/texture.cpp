#include "texture.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
Log_SetChannel(GL);

namespace GL {

Texture::Texture(u32 width, u32 height, GLenum format, GLenum type, const void* data /* = nullptr */,
                 bool linear_filter /* = false */, bool create_framebuffer /* = false */)
  : m_width(width), m_height(height)
{
  glGenTextures(1, &m_id);
  glBindTexture(GL_TEXTURE_2D, m_id);
  glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, type, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);

  if (create_framebuffer)
  {
    glGenFramebuffers(1, &m_fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo_id);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_id, 0);
    Assert(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  }
}

Texture::~Texture()
{
  if (m_fbo_id != 0)
    glDeleteFramebuffers(1, &m_fbo_id);

  glDeleteTextures(1, &m_id);
}

void Texture::Bind()
{
  glBindTexture(GL_TEXTURE_2D, m_id);
}

void Texture::BindFramebuffer(GLenum target /*= GL_DRAW_FRAMEBUFFER*/)
{
  DebugAssert(m_fbo_id != 0);
  glBindFramebuffer(target, m_fbo_id);
}

void Texture::Unbind()
{
  glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace GL