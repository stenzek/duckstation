#pragma once
#include "../gpu_texture.h"
#include "loader.h"
#include <tuple>

namespace GL {

class Texture final : public GPUTexture
{
public:
  Texture();
  Texture(Texture&& moved);
  ~Texture();

  static bool UseTextureStorage(bool multisampled);
  static const std::tuple<GLenum, GLenum, GLenum>& GetPixelFormatMapping(Format format);

  ALWAYS_INLINE GLuint GetGLId() const { return m_id; }
  bool IsValid() const override { return m_id != 0; }

  bool Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Format format, const void* data = nullptr,
              u32 data_pitch = 0, bool linear = true, bool wrap = true);
  void Destroy();

  void Replace(u32 width, u32 height, GLenum internal_format, GLenum format, GLenum type, const void* data);
  void ReplaceImage(u32 layer, u32 level, GLenum format, GLenum type, const void* data);
  void ReplaceSubImage(u32 layer, u32 level, u32 x, u32 y, u32 width, u32 height, GLenum format, GLenum type,
                       const void* data);
  bool CreateFramebuffer();

  bool UseTextureStorage() const;

  void SetLinearFilter(bool enabled) const;
  void SetWrap(bool enabled) const;

  ALWAYS_INLINE GLuint GetGLFramebufferID() const { return m_fbo_id; }
  ALWAYS_INLINE GLenum GetGLTarget() const
  {
    return (IsMultisampled() ? (IsTextureArray() ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D_MULTISAMPLE_ARRAY) :
                               (IsTextureArray() ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D));
  }

  void Bind() const;
  void BindFramebuffer(GLenum target = GL_DRAW_FRAMEBUFFER) const;
  void Unbind() const;

  Texture& operator=(const Texture& copy) = delete;
  Texture& operator=(Texture&& moved);

  // Helper which uses glGetTextureSubImage where available, otherwise a temporary FBO.
  static void GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                 GLsizei bufSize, void* pixels);

private:
  GLuint m_id = 0;
  GLuint m_fbo_id = 0;
};

} // namespace GL