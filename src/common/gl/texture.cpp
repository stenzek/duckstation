#include "texture.h"
#include "../assert.h"
#include "../log.h"
#include <array>
#include <limits>
#include <tuple>
Log_SetChannel(GL);

const std::tuple<GLenum, GLenum, GLenum>& GL::Texture::GetPixelFormatMapping(GPUTexture::Format format)
{
  static constexpr std::array<std::tuple<GLenum, GLenum, GLenum>, static_cast<u32>(GPUTexture::Format::Count)> mapping =
    {{
      {},                                                   // Unknown
      {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},                // RGBA8
      {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE},                // BGRA8
      {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},         // RGB565
      {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV}, // RGBA5551
      {GL_R8, GL_RED, GL_UNSIGNED_BYTE},                    // R8
      {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_SHORT}, // D16
    }};

  static constexpr std::array<std::tuple<GLenum, GLenum, GLenum>, static_cast<u32>(GPUTexture::Format::Count)>
    mapping_gles2 = {{
      {},                                        // Unknown
      {GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE},      // RGBA8
      {},                                        // BGRA8
      {GL_RGB, GL_RGB, GL_UNSIGNED_SHORT_5_6_5}, // RGB565
      {},                                        // RGBA5551
      {},                                        // R8
      {},                                        // D16
    }};

  if (!GLAD_GL_ES_VERSION_2_0 || GLAD_GL_ES_VERSION_3_0)
    return mapping[static_cast<u32>(format)];
  else
    return mapping_gles2[static_cast<u32>(format)];
}

GL::Texture::Texture() = default;

GL::Texture::Texture(Texture&& moved) : m_id(moved.m_id), m_fbo_id(moved.m_fbo_id)
{
  m_width = moved.m_width;
  m_height = moved.m_height;
  m_levels = moved.m_levels;
  m_layers = moved.m_layers;
  m_samples = moved.m_samples;
  m_format = moved.m_format;
  moved.m_id = 0;
  moved.m_fbo_id = 0;
  moved.ClearBaseProperties();
}

GL::Texture::~Texture()
{
  Destroy();
}

bool GL::Texture::UseTextureStorage(bool multisampled)
{
  return GLAD_GL_ARB_texture_storage || (multisampled ? GLAD_GL_ES_VERSION_3_1 : GLAD_GL_ES_VERSION_3_0);
}

bool GL::Texture::UseTextureStorage() const
{
  return UseTextureStorage(IsMultisampled());
}

bool GL::Texture::Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Format format,
                         const void* data /* = nullptr */, u32 data_pitch /* = 0 */, bool linear /* = true */,
                         bool wrap /* = true */)
{
  glGetError();

  if (width > MAX_WIDTH || height > MAX_HEIGHT || layers > MAX_LAYERS || levels > MAX_LEVELS || samples > MAX_SAMPLES)
  {
    Log_ErrorPrintf("Invalid dimensions: %ux%ux%u %u %u", width, height, layers, levels, samples);
    return false;
  }

  if (samples > 1 && levels > 1)
  {
    Log_ErrorPrintf("Multisampled textures can't have mip levels");
    return false;
  }

  if (layers > 1 && data)
  {
    Log_ErrorPrintf("Loading texture array data not currently supported");
    return false;
  }

  const GLenum target = ((samples > 1) ? ((layers > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D_MULTISAMPLE_ARRAY) :
                                         ((layers > 1) ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D));
  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(format);

  GLuint id;
  glGenTextures(1, &id);
  glBindTexture(target, id);

  if (samples > 1)
  {
    Assert(!data);
    if (UseTextureStorage(true))
    {
      if (layers > 1)
        glTexStorage3DMultisample(target, samples, gl_internal_format, width, height, layers, GL_FALSE);
      else
        glTexStorage2DMultisample(target, samples, gl_internal_format, width, height, GL_FALSE);
    }
    else
    {
      if (layers > 1)
        glTexImage3DMultisample(target, samples, gl_internal_format, width, height, layers, GL_FALSE);
      else
        glTexImage2DMultisample(target, samples, gl_internal_format, width, height, GL_FALSE);
    }
  }
  else
  {
    if (UseTextureStorage(false))
    {
      if (layers > 1)
        glTexStorage3D(target, levels, gl_internal_format, width, height, layers);
      else
        glTexStorage2D(target, levels, gl_internal_format, width, height);

      if (data)
      {
        // TODO: Fix data for mipmaps here.
        if (layers > 1)
          glTexSubImage3D(target, 0, 0, 0, 0, width, height, layers, gl_format, gl_type, data);
        else
          glTexSubImage2D(target, 0, 0, 0, width, height, gl_format, gl_type, data);
      }
    }
    else
    {
      for (u32 i = 0; i < levels; i++)
      {
        // TODO: Fix data pointer here.
        if (layers > 1)
          glTexImage3D(target, i, gl_internal_format, width, height, layers, 0, gl_format, gl_type, data);
        else
          glTexImage2D(target, i, gl_internal_format, width, height, 0, gl_format, gl_type, data);
      }

      glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
      glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels);
    }

    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);

    if (layers > 1)
      glTexParameteri(target, GL_TEXTURE_WRAP_R, wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
  }

  // This doesn't exist on GLES2.
  if (!GLAD_GL_ES_VERSION_2_0 || GLAD_GL_ES_VERSION_3_0)
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
  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_layers = static_cast<u8>(layers);
  m_levels = static_cast<u8>(levels);
  m_samples = static_cast<u8>(samples);
  m_format = format;
  return true;
}

void GL::Texture::Replace(u32 width, u32 height, GLenum internal_format, GLenum format, GLenum type, const void* data)
{
  Assert(IsValid() && width < MAX_WIDTH && height < MAX_HEIGHT && m_layers == 1 && m_samples == 1 && m_levels == 1);

  const bool size_changed = (width != m_width || height != m_height);

  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_levels = 1;

  const GLenum target = GetGLTarget();
  glBindTexture(target, m_id);

  if (UseTextureStorage())
  {
    if (size_changed)
    {
      if (m_layers > 0)
        glTexStorage3D(target, m_levels, internal_format, m_width, m_height, m_levels);
      else
        glTexStorage2D(target, m_levels, internal_format, m_width, m_height);
    }

    glTexSubImage2D(target, 0, 0, 0, m_width, m_height, format, type, data);
  }
  else
  {
    glTexImage2D(target, 0, internal_format, width, height, 0, format, type, data);
  }
}

void GL::Texture::ReplaceImage(u32 layer, u32 level, GLenum format, GLenum type, const void* data)
{
  Assert(IsValid() && !IsMultisampled());

  const GLenum target = GetGLTarget();
  if (IsTextureArray())
    glTexSubImage3D(target, level, 0, 0, layer, m_width, m_height, 1, format, type, data);
  else
    glTexSubImage2D(target, level, 0, 0, m_width, m_height, format, type, data);
}

void GL::Texture::ReplaceSubImage(u32 layer, u32 level, u32 x, u32 y, u32 width, u32 height, GLenum format, GLenum type,
                                  const void* data)
{
  Assert(IsValid() && !IsMultisampled());

  const GLenum target = GetGLTarget();
  if (IsTextureArray())
    glTexSubImage3D(target, level, x, y, layer, width, height, 1, format, type, data);
  else
    glTexSubImage2D(target, level, x, y, width, height, format, type, data);
}

void GL::Texture::SetLinearFilter(bool enabled) const
{
  Assert(!IsMultisampled());

  Bind();

  const GLenum target = GetGLTarget();
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, enabled ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, enabled ? GL_LINEAR : GL_NEAREST);
}

void GL::Texture::SetWrap(bool enabled) const
{
  const GLenum target = GetGLTarget();
  glTexParameteri(target, GL_TEXTURE_WRAP_S, enabled ? GL_REPEAT : GL_CLAMP_TO_EDGE);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, enabled ? GL_REPEAT : GL_CLAMP_TO_EDGE);

  if (m_layers > 1)
    glTexParameteri(target, GL_TEXTURE_WRAP_R, enabled ? GL_REPEAT : GL_CLAMP_TO_EDGE);
}

bool GL::Texture::CreateFramebuffer()
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

void GL::Texture::Destroy()
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

  ClearBaseProperties();
}

void GL::Texture::Bind() const
{
  glBindTexture(GetGLTarget(), m_id);
}

void GL::Texture::BindFramebuffer(GLenum target /*= GL_DRAW_FRAMEBUFFER*/) const
{
  DebugAssert(m_fbo_id != 0);
  glBindFramebuffer(target, m_fbo_id);
}

void GL::Texture::Unbind() const
{
  glBindTexture(GetGLTarget(), 0);
}

GL::Texture& GL::Texture::operator=(Texture&& moved)
{
  Destroy();

  m_id = moved.m_id;
  m_fbo_id = moved.m_fbo_id;
  m_width = moved.m_width;
  m_height = moved.m_height;
  m_layers = moved.m_layers;
  m_levels = moved.m_levels;
  m_samples = moved.m_samples;

  moved.m_id = 0;
  moved.m_fbo_id = 0;
  moved.ClearBaseProperties();
  return *this;
}

void GL::Texture::GetTextureSubImage(GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                     GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                     GLsizei bufSize, void* pixels)
{
  if (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_get_texture_sub_image)
  {
    glGetTextureSubImage(texture, level, xoffset, yoffset, zoffset, width, height, depth, format, type, bufSize,
                         pixels);
    return;
  }

  GLenum target = GL_READ_FRAMEBUFFER;
  GLenum target_binding = GL_READ_FRAMEBUFFER_BINDING;
  if (GLAD_GL_ES_VERSION_2_0 && !GLAD_GL_ES_VERSION_3_0)
  {
    // GLES2 doesn't have GL_READ_FRAMEBUFFER.
    target = GL_FRAMEBUFFER;
    target_binding = GL_FRAMEBUFFER_BINDING;
  }

  Assert(depth == 1);

  GLuint old_read_fbo;
  glGetIntegerv(target_binding, reinterpret_cast<GLint*>(&old_read_fbo));

  GLuint temp_fbo;
  glGenFramebuffers(1, &temp_fbo);
  glBindFramebuffer(target, temp_fbo);
  if (zoffset > 0 && (GLAD_GL_VERSION_3_0 || GLAD_GL_ES_VERSION_3_0))
    glFramebufferTextureLayer(target, GL_COLOR_ATTACHMENT0, texture, level, zoffset);
  else
    glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);

  DebugAssert(glCheckFramebufferStatus(target) == GL_FRAMEBUFFER_COMPLETE);
  glReadPixels(xoffset, yoffset, width, height, format, type, pixels);

  glBindFramebuffer(target, old_read_fbo);
  glDeleteFramebuffers(1, &temp_fbo);
}
