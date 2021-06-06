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

  static bool UseTextureStorage(bool multisampled);

  bool Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, GLenum internal_format, GLenum format,
              GLenum type, const void* data = nullptr, bool linear_filter = false, bool wrap = false);
  void Replace(u32 width, u32 height, GLenum internal_format, GLenum format, GLenum type, const void* data);
  void ReplaceImage(u32 layer, u32 level, GLenum format, GLenum type, const void* data);
  void ReplaceSubImage(u32 layer, u32 level, u32 x, u32 y, u32 width, u32 height, GLenum format, GLenum type,
                       const void* data);
  bool CreateFramebuffer();

  void Destroy();

  bool UseTextureStorage() const;
  void SetLinearFilter(bool enabled);

  ALWAYS_INLINE bool IsValid() const { return m_id != 0; }
  ALWAYS_INLINE bool IsTextureArray() const { return m_layers > 1; }
  ALWAYS_INLINE bool IsMultisampled() const { return m_samples > 1; }
  ALWAYS_INLINE GLuint GetGLId() const { return m_id; }
  ALWAYS_INLINE u16 GetWidth() const { return m_width; }
  ALWAYS_INLINE u16 GetHeight() const { return m_height; }
  ALWAYS_INLINE u16 GetLayers() const { return m_layers; }
  ALWAYS_INLINE u8 GetLevels() const { return m_levels; }
  ALWAYS_INLINE u8 GetSamples() const { return m_samples; }

  ALWAYS_INLINE GLuint GetGLFramebufferID() const { return m_fbo_id; }
  ALWAYS_INLINE GLenum GetGLTarget() const
  {
    return (IsMultisampled() ? (IsTextureArray() ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D_MULTISAMPLE_ARRAY) :
                               (IsTextureArray() ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D));
  }

  void Bind();
  void BindFramebuffer(GLenum target = GL_DRAW_FRAMEBUFFER);
  void Unbind();

  Texture& operator=(const Texture& copy) = delete;
  Texture& operator=(Texture&& moved);

  // Helper which uses glGetTextureSubImage where available, otherwise a temporary FBO.
  static void GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                 GLsizei bufSize, void* pixels);

private:
  GLuint m_id = 0;
  u16 m_width = 0;
  u16 m_height = 0;
  u16 m_layers = 0;
  u8 m_levels = 0;
  u8 m_samples = 0;

  GLuint m_fbo_id = 0;
};

} // namespace GL