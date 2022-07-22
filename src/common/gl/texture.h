#pragma once
#include "../types.h"
#include "loader.h"

namespace GL {
class Texture
{
public:
  Texture();
  Texture(Texture&& moved);
  ~Texture();

  bool Create(u32 width, u32 height, u32 samples, GLenum internal_format, GLenum format, GLenum type,
              const void* data = nullptr, bool linear_filter = false, bool wrap = false);
  void Replace(u32 width, u32 height, GLenum internal_format, GLenum format, GLenum type, const void* data);
  bool CreateFramebuffer();

  void Destroy();

  void SetLinearFilter(bool enabled);

  bool IsValid() const { return m_id != 0; }
  bool IsMultisampled() const { return m_samples > 1; }
  GLuint GetGLId() const { return m_id; }
  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }
  u32 GetSamples() const { return m_samples; }

  GLuint GetGLFramebufferID() const { return m_fbo_id; }
  GLenum GetGLTarget() const { return IsMultisampled() ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D; }

  void Bind();
  void BindFramebuffer(GLenum target = GL_DRAW_FRAMEBUFFER);

  static void Unbind();

  Texture& operator=(const Texture& copy) = delete;
  Texture& operator=(Texture&& moved);

  // Helper which uses glGetTextureSubImage where available, otherwise a temporary FBO.
  static void GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                 GLsizei bufSize, void* pixels);

private:
  GLuint m_id = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_samples = 0;

  GLuint m_fbo_id = 0;
};

} // namespace GL