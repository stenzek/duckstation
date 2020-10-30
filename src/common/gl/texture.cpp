#include "texture.h"
#include "../assert.h"
#include "../log.h"
Log_SetChannel(GL);

namespace GL {

Texture::Texture() = default;

Texture::Texture(Texture&& moved)
  : m_id(moved.m_id), m_width(moved.m_width), m_height(moved.m_height), m_samples(moved.m_samples),
    m_fbo_id(moved.m_fbo_id)
{
  moved.m_id = 0;
  moved.m_width = 0;
  moved.m_height = 0;
  moved.m_samples = 0;
  moved.m_fbo_id = 0;
}

Texture::~Texture()
{
  Destroy();
}

bool Texture::Create(u32 width, u32 height, u32 samples, GLenum internal_format, GLenum format, GLenum type,
                     const void* data, bool linear_filter, bool wrap)
{
  glGetError();

  const GLenum target = (samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

  GLuint id;
  glGenTextures(1, &id);
  glBindTexture(target, id);

  if (samples > 1)
  {
    glTexImage2DMultisample(target, samples, internal_format, width, height, GL_FALSE);
  }
  else
  {
    glTexImage2D(target, 0, internal_format, width, height, 0, format, type, data);

    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
  }

  glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 1);

  GLenum error = glGetError();
  if (error != GL_NO_ERROR)
  {
    Log_ErrorPrintf("Failed to create texture: 0x%X", error);
    glDeleteTextures(1, &id);
    return false;
  }

  if (IsValid())
    Destroy();

  m_id = id;
  m_width = width;
  m_height = height;
  m_samples = samples;
  return true;
}

void Texture::SetLinearFilter(bool enabled)
{
  Assert(!IsMultisampled());

  Bind();

  const GLenum target = GetGLTarget();
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, enabled ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, enabled ? GL_LINEAR : GL_NEAREST);
}

bool Texture::CreateFramebuffer()
{
  if (!IsValid())
    return false;

  glGetError();

  GLuint fbo_id;
  glGenFramebuffers(1, &fbo_id);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_id);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_id, 0);
  if (glGetError() != GL_NO_ERROR || glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    glDeleteFramebuffers(1, &fbo_id);
    return false;
  }

  if (m_fbo_id != 0)
    glDeleteFramebuffers(1, &m_fbo_id);

  m_fbo_id = fbo_id;
  return true;
}

void Texture::Destroy()
{
  if (m_fbo_id != 0)
  {
    glDeleteFramebuffers(1, &m_fbo_id);
    m_fbo_id = 0;
  }
  if (m_id != 0)
  {
    glDeleteTextures(1, &m_id);
    m_id = 0;
  }

  m_width = 0;
  m_height = 0;
  m_samples = 0;
}

void Texture::Bind()
{
  glBindTexture(GetGLTarget(), m_id);
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

Texture& Texture::operator=(Texture&& moved)
{
  Destroy();

  m_id = moved.m_id;
  m_width = moved.m_width;
  m_height = moved.m_height;
  m_samples = moved.m_samples;
  m_fbo_id = moved.m_fbo_id;

  moved.m_id = 0;
  moved.m_width = 0;
  moved.m_height = 0;
  moved.m_samples = 0;
  moved.m_fbo_id = 0;
  return *this;
}

void Texture::GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                 GLsizei bufSize, void* pixels)
{
  if (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_get_texture_sub_image)
  {
    glGetTextureSubImage(texture, level, xoffset, yoffset, zoffset, width, height, depth, format, type, bufSize,
                         pixels);
    return;
  }

  Assert(depth == 1);

  GLuint old_read_fbo;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&old_read_fbo));

  GLuint temp_fbo;
  glGenFramebuffers(1, &temp_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
  if (zoffset > 0 && (GLAD_GL_VERSION_3_0 || GLAD_GL_ES_VERSION_3_0))
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, level, zoffset);
  else
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);

  DebugAssert(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  glReadPixels(xoffset, yoffset, width, height, format, type, pixels);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read_fbo);
  glDeleteFramebuffers(1, &temp_fbo);
}

} // namespace GL