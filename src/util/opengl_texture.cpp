// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_texture.h"
#include "opengl_device.h"
#include "opengl_stream_buffer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/string_util.h"

#include <array>
#include <limits>
#include <tuple>

LOG_CHANNEL(GPUDevice);

// Looking across a range of GPUs, the optimal copy alignment for Vulkan drivers seems
// to be between 1 (AMD/NV) and 64 (Intel). So, we'll go with 64 here.
static constexpr u32 TEXTURE_UPLOAD_ALIGNMENT = 64;

// The pitch alignment must be less or equal to the upload alignment.
// We need 32 here for AVX2, so 64 is also fine.
static constexpr u32 TEXTURE_UPLOAD_PITCH_ALIGNMENT = 64;

// Default upload alignment, for restoring.
static constexpr u32 DEFAULT_UPLOAD_ALIGNMENT = 4;

const std::tuple<GLenum, GLenum, GLenum>& OpenGLTexture::GetPixelFormatMapping(GPUTexture::Format format, bool gles)
{
  static constexpr std::array<std::tuple<GLenum, GLenum, GLenum>, static_cast<u32>(GPUTexture::Format::MaxCount)>
    mapping = {{
      {},                                                                                       // Unknown
      {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},                                                    // RGBA8
      {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE},                                                    // BGRA8
      {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},                                             // RGB565
      {},                                                                                       // RGB5A1
      {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},                                         // A1BGR5
      {GL_R8, GL_RED, GL_UNSIGNED_BYTE},                                                        // R8
      {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_SHORT},                                     // D16
      {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT},                                 // D24S8
      {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT},                                    // D32F
      {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT},                                       // D32FS8
      {GL_R16, GL_RED, GL_UNSIGNED_SHORT},                                                      // R16
      {GL_R16I, GL_RED_INTEGER, GL_SHORT},                                                      // R16I
      {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT},                                            // R16U
      {GL_R16F, GL_RED, GL_HALF_FLOAT},                                                         // R16F
      {GL_R32I, GL_RED_INTEGER, GL_INT},                                                        // R32I
      {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT},                                              // R32U
      {GL_R32F, GL_RED, GL_FLOAT},                                                              // R32F
      {GL_RG8, GL_RG_INTEGER, GL_UNSIGNED_BYTE},                                                // RG8
      {GL_RG16F, GL_RG, GL_UNSIGNED_SHORT},                                                     // RG16
      {GL_RG16F, GL_RG, GL_HALF_FLOAT},                                                         // RG16F
      {GL_RG32F, GL_RG, GL_FLOAT},                                                              // RG32F
      {GL_RGBA16, GL_RGBA, GL_UNSIGNED_BYTE},                                                   // RGBA16
      {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT},                                                     // RGBA16F
      {GL_RGBA32F, GL_RGBA, GL_FLOAT},                                                          // RGBA32F
      {GL_RGB10_A2, GL_BGRA, GL_UNSIGNED_INT_2_10_10_10_REV},                                   // RGB10A2
      {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE},                                             // SRGBA8
      {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_UNSIGNED_BYTE},   // BC1
      {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_UNSIGNED_BYTE},   // BC2
      {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_UNSIGNED_BYTE},   // BC3
      {GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_UNSIGNED_BYTE}, // BC7
    }};

  // GLES doesn't have the non-normalized 16-bit formats.. use float and hope for the best, lol.
  static constexpr std::array<std::tuple<GLenum, GLenum, GLenum>, static_cast<u32>(GPUTexture::Format::MaxCount)>
    mapping_gles = {{
      {},                                                                                       // Unknown
      {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},                                                    // RGBA8
      {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE},                                                    // BGRA8
      {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},                                             // RGB565
      {},                                                                                       // RGB5A1
      {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},                                         // A1BGR5
      {GL_R8, GL_RED, GL_UNSIGNED_BYTE},                                                        // R8
      {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_SHORT},                                     // D16
      {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT},                                 // D24S8
      {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT},                                    // D32F
      {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT},                                       // D32FS8
      {GL_R16F, GL_RED, GL_HALF_FLOAT},                                                         // R16
      {GL_R16I, GL_RED_INTEGER, GL_SHORT},                                                      // R16I
      {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT},                                            // R16U
      {GL_R16F, GL_RED, GL_HALF_FLOAT},                                                         // R16F
      {GL_R32I, GL_RED_INTEGER, GL_INT},                                                        // R32I
      {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT},                                              // R32U
      {GL_R32F, GL_RED, GL_FLOAT},                                                              // R32F
      {GL_RG8, GL_RG, GL_UNSIGNED_BYTE},                                                        // RG8
      {GL_RG16F, GL_RG, GL_HALF_FLOAT},                                                         // RG16
      {GL_RG16F, GL_RG, GL_HALF_FLOAT},                                                         // RG16F
      {GL_RG32F, GL_RG, GL_FLOAT},                                                              // RG32F
      {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT},                                                     // RGBA16
      {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT},                                                     // RGBA16F
      {GL_RGBA32F, GL_RGBA, GL_FLOAT},                                                          // RGBA32F
      {GL_RGB10_A2, GL_BGRA, GL_UNSIGNED_INT_2_10_10_10_REV},                                   // RGB10A2
      {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE},                                             // SRGBA8
      {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_UNSIGNED_BYTE},   // BC1
      {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_UNSIGNED_BYTE},   // BC2
      {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_UNSIGNED_BYTE},   // BC3
      {GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_COMPRESSED_RGBA_BPTC_UNORM_ARB, GL_UNSIGNED_BYTE}, // BC7
    }};

  return gles ? mapping_gles[static_cast<u32>(format)] : mapping[static_cast<u32>(format)];
}

ALWAYS_INLINE static u32 GetUploadAlignment(u32 pitch)
{
  return ((pitch % 4) == 0) ? 4 : (((pitch % 2) == 0) ? 2 : 1);
}

OpenGLTexture::OpenGLTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                             Flags flags, GLuint id)
  : GPUTexture(static_cast<u16>(width), static_cast<u16>(height), static_cast<u8>(layers), static_cast<u8>(levels),
               static_cast<u8>(samples), type, format, flags),
    m_id(id)
{
}

OpenGLTexture::~OpenGLTexture()
{
  if (m_id != 0)
  {
    OpenGLDevice::GetInstance().UnbindTexture(this);
    glDeleteTextures(1, &m_id);
    m_id = 0;
  }
}

bool OpenGLTexture::UseTextureStorage(bool multisampled)
{
  return GLAD_GL_ARB_texture_storage || (multisampled ? GLAD_GL_ES_VERSION_3_1 : GLAD_GL_ES_VERSION_3_0);
}

bool OpenGLTexture::UseTextureStorage() const
{
  return UseTextureStorage(IsMultisampled());
}

std::unique_ptr<OpenGLTexture> OpenGLTexture::Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                     Type type, Format format, Flags flags, const void* data,
                                                     u32 data_pitch, Error* error)
{
  if (!ValidateConfig(width, height, layers, levels, samples, type, format, flags, error))
    return nullptr;

  if (layers > 1 && data)
  {
    Error::SetStringView(error, "Loading texture array data not currently supported");
    return nullptr;
  }

  const GLenum target =
    ((samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : ((layers > 1) ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D));
  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(format, OpenGLDevice::IsGLES());

  OpenGLDevice::BindUpdateTextureUnit();

  glGetError();

  GLuint id;
  glGenTextures(1, &id);
  glBindTexture(target, id);

  if (samples > 1)
  {
    Assert(!data);
    if (UseTextureStorage(true))
    {
      glTexStorage2DMultisample(target, samples, gl_internal_format, width, height, GL_FALSE);
    }
    else
    {
      glTexImage2DMultisample(target, samples, gl_internal_format, width, height, GL_FALSE);
    }

    glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels);
  }
  else
  {
    const bool use_texture_storage = UseTextureStorage(false);
    const bool is_compressed = IsCompressedFormat(format);
    if (use_texture_storage)
    {
      if (layers > 1)
        glTexStorage3D(target, levels, gl_internal_format, width, height, layers);
      else
        glTexStorage2D(target, levels, gl_internal_format, width, height);
    }

    if (!use_texture_storage || data)
    {
      const u32 pixel_size = GetPixelSize(format);
      const u32 alignment = GetUploadAlignment(data_pitch);
      if (data)
      {
        GPUDevice::GetStatistics().buffer_streamed += CalcUploadSize(format, height, data_pitch);
        GPUDevice::GetStatistics().num_uploads++;

        glPixelStorei(GL_UNPACK_ROW_LENGTH, CalcUploadRowLengthFromPitch(format, data_pitch));
        if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
          glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
      }

      const u8* data_ptr = static_cast<const u8*>(data);
      u32 current_width = width;
      u32 current_height = height;
      for (u32 i = 0; i < levels; i++)
      {
        if (use_texture_storage)
        {
          if (is_compressed)
          {
            const u32 size = CalcUploadSize(format, current_height, data_pitch);
            if (layers > 1)
            {
              glCompressedTexSubImage3D(target, i, 0, 0, 0, current_width, current_height, layers, gl_format, size,
                                        data_ptr);
            }
            else
            {
              glCompressedTexSubImage2D(target, i, 0, 0, current_width, current_height, gl_format, size, data_ptr);
            }
          }
          else
          {
            if (layers > 1)
              glTexSubImage3D(target, i, 0, 0, 0, current_width, current_height, layers, gl_format, gl_type, data_ptr);
            else
              glTexSubImage2D(target, i, 0, 0, current_width, current_height, gl_format, gl_type, data_ptr);
          }
        }
        else
        {
          if (is_compressed)
          {
            const u32 size = CalcUploadSize(format, current_height, data_pitch);
            if (layers > 1)
            {
              glCompressedTexImage3D(target, i, gl_internal_format, current_width, current_height, layers, 0, size,
                                     data_ptr);
            }
            else
            {
              glCompressedTexImage2D(target, i, gl_internal_format, current_width, current_height, 0, size, data_ptr);
            }
          }
          else
          {
            if (layers > 1)
            {
              glTexImage3D(target, i, gl_internal_format, current_width, current_height, layers, 0, gl_format, gl_type,
                           data_ptr);
            }
            else
            {
              glTexImage2D(target, i, gl_internal_format, current_width, current_height, 0, gl_format, gl_type,
                           data_ptr);
            }
          }
        }

        if (data_ptr)
          data_ptr += data_pitch * current_width;

        current_width = (current_width > 1) ? (current_width / 2u) : current_width;
        current_height = (current_height > 1) ? (current_height / 2u) : current_height;

        // TODO: Incorrect assumption.
        data_pitch = pixel_size * current_width;
      }

      if (data)
      {
        if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
          glPixelStorei(GL_UNPACK_ALIGNMENT, DEFAULT_UPLOAD_ALIGNMENT);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      }
    }

    if (!use_texture_storage)
    {
      glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
      glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    }
  }

  const GLenum gl_error = glGetError();
  if (gl_error != GL_NO_ERROR)
  {
    Error::SetStringFmt(error, "Failed to create texture: 0x{:X}", gl_error);
    glDeleteTextures(1, &id);
    return nullptr;
  }

  return std::unique_ptr<OpenGLTexture>(
    new OpenGLTexture(width, height, layers, levels, samples, type, format, flags, id));
}

void OpenGLTexture::CommitClear()
{
  OpenGLDevice::GetInstance().CommitClear(this);
}

bool OpenGLTexture::Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer /*= 0*/,
                           u32 level /*= 0*/)
{
  // Worth using the PBO? Driver probably knows better...
  const GLenum target = GetGLTarget();
  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(m_format, OpenGLDevice::IsGLES());
  const u32 preferred_pitch = Common::AlignUpPow2(CalcUploadPitch(width), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 map_size = CalcUploadSize(height, pitch);
  OpenGLStreamBuffer* sb = OpenGLDevice::GetTextureStreamBuffer();

  CommitClear();

  GPUDevice::GetStatistics().buffer_streamed += map_size;
  GPUDevice::GetStatistics().num_uploads++;

  OpenGLDevice::BindUpdateTextureUnit();
  glBindTexture(target, m_id);

  if (!sb || map_size > sb->GetChunkSize())
  {
    GL_INS_FMT("Not using PBO for map size {}", map_size);

    const u32 alignment = GetUploadAlignment(pitch);
    if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
      glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, CalcUploadRowLengthFromPitch(pitch));
    if (IsCompressedFormat())
    {
      const u32 size = CalcUploadSize(height, pitch);
      if (IsTextureArray())
        glCompressedTexSubImage3D(target, level, x, y, layer, width, height, 1, gl_format, size, data);
      else
        glCompressedTexSubImage2D(target, level, x, y, width, height, gl_format, size, data);
    }
    else
    {
      if (IsTextureArray())
        glTexSubImage3D(target, level, x, y, layer, width, height, 1, gl_format, gl_type, data);
      else
        glTexSubImage2D(target, level, x, y, width, height, gl_format, gl_type, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (alignment != DEFAULT_UPLOAD_ALIGNMENT)
      glPixelStorei(GL_UNPACK_ALIGNMENT, DEFAULT_UPLOAD_ALIGNMENT);
  }
  else
  {
    const auto map = sb->Map(TEXTURE_UPLOAD_ALIGNMENT, map_size);
    CopyTextureDataForUpload(width, height, m_format, map.pointer, preferred_pitch, data, pitch);
    sb->Unmap(map_size);
    sb->Bind();

    glPixelStorei(GL_UNPACK_ROW_LENGTH, CalcUploadRowLengthFromPitch(preferred_pitch));
    if (IsCompressedFormat())
    {
      const u32 size = CalcUploadSize(height, pitch);
      if (IsTextureArray())
      {
        glCompressedTexSubImage3D(target, level, x, y, layer, width, height, 1, gl_format, size,
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(map.buffer_offset)));
      }
      else
      {
        glCompressedTexSubImage2D(target, level, x, y, width, height, gl_format, size,
                                  reinterpret_cast<void*>(static_cast<uintptr_t>(map.buffer_offset)));
      }
    }
    else
    {
      if (IsTextureArray())
      {
        glTexSubImage3D(target, level, x, y, layer, width, height, 1, gl_format, gl_type,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(map.buffer_offset)));
      }
      else
      {
        glTexSubImage2D(target, level, x, y, width, height, gl_format, gl_type,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(map.buffer_offset)));
      }
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    sb->Unbind();
  }

  glBindTexture(target, 0);
  return true;
}

bool OpenGLTexture::Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer /*= 0*/,
                        u32 level /*= 0*/)
{
  if ((x + width) > GetMipWidth(level) || (y + height) > GetMipHeight(level) || layer > m_layers || level > m_levels)
    return false;

  const u32 pitch = Common::AlignUpPow2(CalcUploadPitch(width), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 upload_size = CalcUploadSize(height, pitch);
  OpenGLStreamBuffer* sb = OpenGLDevice::GetTextureStreamBuffer();
  if (!sb || upload_size > sb->GetSize())
    return false;

  const auto res = sb->Map(TEXTURE_UPLOAD_ALIGNMENT, upload_size);
  *map = res.pointer;
  *map_stride = pitch;

  m_map_offset = res.buffer_offset;
  m_map_x = static_cast<u16>(x);
  m_map_y = static_cast<u16>(y);
  m_map_width = static_cast<u16>(width);
  m_map_height = static_cast<u16>(height);
  m_map_layer = static_cast<u8>(layer);
  m_map_level = static_cast<u8>(level);
  return true;
}

void OpenGLTexture::Unmap()
{
  CommitClear();

  const u32 pitch = Common::AlignUpPow2(CalcUploadPitch(m_map_width), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 upload_size = CalcUploadSize(m_map_height, pitch);

  GPUDevice::GetStatistics().buffer_streamed += upload_size;
  GPUDevice::GetStatistics().num_uploads++;

  OpenGLStreamBuffer* sb = OpenGLDevice::GetTextureStreamBuffer();
  sb->Unmap(upload_size);
  sb->Bind();

  OpenGLDevice::BindUpdateTextureUnit();

  const GLenum target = GetGLTarget();
  glBindTexture(target, m_id);

  glPixelStorei(GL_UNPACK_ROW_LENGTH, CalcUploadRowLengthFromPitch(pitch));

  const auto [gl_internal_format, gl_format, gl_type] = GetPixelFormatMapping(m_format, OpenGLDevice::IsGLES());
  if (IsCompressedFormat())
  {
    const u32 size = CalcUploadSize(m_map_height, pitch);
    if (IsTextureArray())
    {
      glCompressedTexSubImage3D(target, m_map_level, m_map_x, m_map_y, m_map_layer, m_map_width, m_map_height, 1,
                                gl_format, size, reinterpret_cast<void*>(static_cast<uintptr_t>(m_map_offset)));
    }
    else
    {
      glCompressedTexSubImage2D(target, m_map_level, m_map_x, m_map_y, m_map_width, m_map_height, gl_format, size,
                                reinterpret_cast<void*>(static_cast<uintptr_t>(m_map_offset)));
    }
  }
  else
  {
    if (IsTextureArray())
    {
      glTexSubImage3D(target, m_map_level, m_map_x, m_map_y, m_map_layer, m_map_width, m_map_height, 1, gl_format,
                      gl_type, reinterpret_cast<void*>(static_cast<uintptr_t>(m_map_offset)));
    }
    else
    {
      glTexSubImage2D(target, m_map_level, m_map_x, m_map_y, m_map_width, m_map_height, gl_format, gl_type,
                      reinterpret_cast<void*>(static_cast<uintptr_t>(m_map_offset)));
    }
  }

  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  glBindTexture(target, 0);

  sb->Unbind();
}

void OpenGLTexture::GenerateMipmaps()
{
  DebugAssert(HasFlag(Flags::AllowGenerateMipmaps));
  OpenGLDevice::BindUpdateTextureUnit();
  const GLenum target = GetGLTarget();
  glBindTexture(target, m_id);
  glGenerateMipmap(target);
  glBindTexture(target, 0);
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void OpenGLTexture::SetDebugName(std::string_view name)
{
  if (glObjectLabel)
    glObjectLabel(GL_TEXTURE, m_id, static_cast<GLsizei>(name.length()), static_cast<const GLchar*>(name.data()));
}

#endif

//////////////////////////////////////////////////////////////////////////

OpenGLSampler::OpenGLSampler(GLuint id) : GPUSampler(), m_id(id)
{
}

OpenGLSampler::~OpenGLSampler()
{
  OpenGLDevice::GetInstance().UnbindSampler(m_id);
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void OpenGLSampler::SetDebugName(std::string_view name)
{
  if (glObjectLabel)
    glObjectLabel(GL_SAMPLER, m_id, static_cast<GLsizei>(name.length()), static_cast<const GLchar*>(name.data()));
}

#endif

std::unique_ptr<GPUSampler> OpenGLDevice::CreateSampler(const GPUSampler::Config& config, Error* error /* = nullptr */)
{
  static constexpr std::array<GLenum, static_cast<u8>(GPUSampler::AddressMode::MaxCount)> ta = {{
    GL_REPEAT,          // Repeat
    GL_CLAMP_TO_EDGE,   // ClampToEdge
    GL_CLAMP_TO_BORDER, // ClampToBorder
    GL_MIRRORED_REPEAT, // MirrorRepeat
  }};

  // [mipmap_on_off][mipmap][filter]
  static constexpr GLenum filters[2][2][2] = {
    {
      // mipmap=off
      {GL_NEAREST, GL_LINEAR}, // mipmap=nearest
      {GL_NEAREST, GL_LINEAR}, // mipmap=linear
    },
    {
      // mipmap=on
      {GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST}, // mipmap=nearest
      {GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR},   // mipmap=linear
    },
  };

  GLuint sampler;
  glGetError();
  glGenSamplers(1, &sampler);
  if (glGetError() != GL_NO_ERROR)
  {
    Error::SetStringFmt(error, "Failed to create sampler: {:X}", sampler);
    return {};
  }

  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, ta[static_cast<u8>(config.address_u.GetValue())]);
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, ta[static_cast<u8>(config.address_v.GetValue())]);
  glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, ta[static_cast<u8>(config.address_w.GetValue())]);
  const u8 mipmap_on_off = (config.min_lod != 0 || config.max_lod != 0);
  glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                      filters[mipmap_on_off][static_cast<u8>(config.mip_filter.GetValue())]
                             [static_cast<u8>(config.min_filter.GetValue())]);
  glSamplerParameteri(
    sampler, GL_TEXTURE_MAG_FILTER,
    filters[0][static_cast<u8>(config.mip_filter.GetValue())][static_cast<u8>(config.mag_filter.GetValue())]);
  glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, static_cast<float>(config.min_lod));
  glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, static_cast<float>(config.max_lod));
  glSamplerParameterfv(sampler, GL_TEXTURE_BORDER_COLOR, config.GetBorderFloatColor().data());
  if (config.anisotropy > 1)
    glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY, static_cast<float>(config.anisotropy.GetValue()));

  return std::unique_ptr<GPUSampler>(new OpenGLSampler(sampler));
}

//////////////////////////////////////////////////////////////////////////

void OpenGLDevice::CommitClear(OpenGLTexture* tex)
{
  switch (tex->GetState())
  {
    case GPUTexture::State::Invalidated:
    {
      tex->SetState(GPUTexture::State::Dirty);

      if (glInvalidateTexImage)
      {
        glInvalidateTexImage(tex->GetGLId(), 0);
      }
      else if (glInvalidateFramebuffer)
      {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_write_fbo);

        const GLenum attachment = tex->IsDepthStencil() ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, tex->GetGLTarget(), tex->GetGLId(), 0);

        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_fbo);
      }
    }
    break;

    case GPUTexture::State::Cleared:
    {
      tex->SetState(GPUTexture::State::Dirty);

      if (glClearTexImage)
      {
        const auto [gl_internal_format, gl_format, gl_type] =
          OpenGLTexture::GetPixelFormatMapping(tex->GetFormat(), m_gl_context->IsGLES());
        glClearTexImage(tex->GetGLId(), 0, gl_format, gl_type, &tex->GetClearValue());
      }
      else
      {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_write_fbo);

        const GLenum attachment = tex->IsDepthStencil() ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, tex->GetGLTarget(), tex->GetGLId(), 0);

        if (tex->IsDepthStencil())
        {
          const float depth = tex->GetClearDepth();
          glDisable(GL_SCISSOR_TEST);
          if (!m_last_depth_state.depth_write)
            glDepthMask(GL_TRUE);
          glClearBufferfv(GL_DEPTH, 0, &depth);
          if (!m_last_depth_state.depth_write)
            glDepthMask(GL_FALSE);
          glEnable(GL_SCISSOR_TEST);
        }
        else
        {
          const auto color = tex->GetUNormClearColor();
          glDisable(GL_SCISSOR_TEST);
          if (m_last_blend_state.write_mask != 0xf)
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
          glClearBufferfv(GL_COLOR, 0, color.data());
          if (m_last_blend_state.write_mask != 0xf)
          {
            glColorMask(m_last_blend_state.write_r, m_last_blend_state.write_g, m_last_blend_state.write_b,
                        m_last_blend_state.write_a);
          }
          glEnable(GL_SCISSOR_TEST);
        }

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_current_fbo);
      }
    }
    break;

    case GPUTexture::State::Dirty:
      break;

    default:
      UnreachableCode();
      break;
  }
}

void OpenGLDevice::CommitRTClearInFB(OpenGLTexture* tex, u32 idx)
{
  switch (tex->GetState())
  {
    case GPUTexture::State::Invalidated:
    {
      const GLenum attachment = GL_COLOR_ATTACHMENT0 + idx;
      if (glInvalidateFramebuffer)
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
      tex->SetState(GPUTexture::State::Dirty);
    }
    break;

    case GPUTexture::State::Cleared:
    {
      const auto color = tex->GetUNormClearColor();
      glDisable(GL_SCISSOR_TEST);
      if (m_last_blend_state.write_mask != 0xf)
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearBufferfv(GL_COLOR, static_cast<GLint>(idx), color.data());
      if (m_last_blend_state.write_mask != 0xf)
      {
        glColorMask(m_last_blend_state.write_r, m_last_blend_state.write_g, m_last_blend_state.write_b,
                    m_last_blend_state.write_a);
      }
      glEnable(GL_SCISSOR_TEST);
      tex->SetState(GPUTexture::State::Dirty);
    }
    break;

    case GPUTexture::State::Dirty:
      break;

    default:
      UnreachableCode();
      break;
  }
}

void OpenGLDevice::CommitDSClearInFB(OpenGLTexture* tex)
{
  switch (tex->GetState())
  {
    case GPUTexture::State::Invalidated:
    {
      const GLenum attachment = GL_DEPTH_ATTACHMENT;
      if (glInvalidateFramebuffer)
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
      tex->SetState(GPUTexture::State::Dirty);
    }
    break;

    case GPUTexture::State::Cleared:
    {
      const float depth = tex->GetClearDepth();
      glDisable(GL_SCISSOR_TEST);
      if (!m_last_depth_state.depth_write)
        glDepthMask(GL_TRUE);
      glClearBufferfv(GL_DEPTH, 0, &depth);
      if (!m_last_depth_state.depth_write)
        glDepthMask(GL_FALSE);
      glEnable(GL_SCISSOR_TEST);
      tex->SetState(GPUTexture::State::Dirty);
    }
    break;

    case GPUTexture::State::Dirty:
      break;

    default:
      UnreachableCode();
      break;
  }
}

//////////////////////////////////////////////////////////////////////////

OpenGLTextureBuffer::OpenGLTextureBuffer(Format format, u32 size_in_elements,
                                         std::unique_ptr<OpenGLStreamBuffer> buffer, GLuint texture_id)
  : GPUTextureBuffer(format, size_in_elements), m_buffer(std::move(buffer)), m_texture_id(texture_id)
{
}

OpenGLTextureBuffer::~OpenGLTextureBuffer()
{
  OpenGLDevice& dev = OpenGLDevice::GetInstance();
  if (m_texture_id != 0)
  {
    dev.UnbindTexture(m_texture_id);
    glDeleteTextures(1, &m_texture_id);
  }
  else if (dev.GetFeatures().texture_buffers_emulated_with_ssbo && m_buffer)
  {
    dev.UnbindSSBO(m_buffer->GetGLBufferId());
  }
}

bool OpenGLTextureBuffer::CreateBuffer()
{
  const bool use_ssbo = OpenGLDevice::GetInstance().GetFeatures().texture_buffers_emulated_with_ssbo;

  const GLenum target = (use_ssbo ? GL_SHADER_STORAGE_BUFFER : GL_TEXTURE_BUFFER);
  m_buffer = OpenGLStreamBuffer::Create(target, GetSizeInBytes());
  if (!m_buffer)
    return false;

  if (!use_ssbo)
  {
    glGetError();
    glGenTextures(1, &m_texture_id);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR)
    {
      ERROR_LOG("Failed to create texture for buffer: 0x{:X}", err);
      return false;
    }

    OpenGLDevice::BindUpdateTextureUnit();
    glBindTexture(GL_TEXTURE_BUFFER, m_texture_id);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, m_buffer->GetGLBufferId());
  }

  m_buffer->Unbind();

  return true;
}

void* OpenGLTextureBuffer::Map(u32 required_elements)
{
  const u32 esize = GetElementSize(m_format);
  const auto map = m_buffer->Map(esize, esize * required_elements);
  m_current_position = map.index_aligned;
  return map.pointer;
}

void OpenGLTextureBuffer::Unmap(u32 used_elements)
{
  const u32 size = used_elements * GetElementSize(m_format);
  GPUDevice::GetStatistics().buffer_streamed += size;
  GPUDevice::GetStatistics().num_uploads++;
  m_buffer->Unmap(size);
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void OpenGLTextureBuffer::SetDebugName(std::string_view name)
{
  if (glObjectLabel)
  {
    glObjectLabel(GL_TEXTURE, m_buffer->GetGLBufferId(), static_cast<GLsizei>(name.length()),
                  static_cast<const GLchar*>(name.data()));
  }
}

#endif

std::unique_ptr<GPUTextureBuffer> OpenGLDevice::CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                    u32 size_in_elements, Error* error)
{
  const bool use_ssbo = OpenGLDevice::GetInstance().GetFeatures().texture_buffers_emulated_with_ssbo;
  const u32 buffer_size = GPUTextureBuffer::GetElementSize(format) * size_in_elements;

  if (use_ssbo)
  {
    GLint64 max_ssbo_size = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_size);
    if (static_cast<GLint64>(buffer_size) > max_ssbo_size)
    {
      Error::SetStringFmt(error, "Buffer size of {} not supported, max is {}", buffer_size, max_ssbo_size);
      return {};
    }
  }

  const GLenum target = (use_ssbo ? GL_SHADER_STORAGE_BUFFER : GL_TEXTURE_BUFFER);
  std::unique_ptr<OpenGLStreamBuffer> buffer = OpenGLStreamBuffer::Create(target, buffer_size, error);
  if (!buffer)
    return {};
  buffer->Unbind();

  GLuint texture_id = 0;
  if (!use_ssbo)
  {
    glGetError();
    glGenTextures(1, &texture_id);
    if (const GLenum err = glGetError(); err != GL_NO_ERROR)
    {
      Error::SetStringFmt(error, "Failed to create texture for buffer: 0x{:X}", err);
      return {};
    }

    OpenGLDevice::BindUpdateTextureUnit();
    glBindTexture(GL_TEXTURE_BUFFER, texture_id);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, buffer->GetGLBufferId());
  }

  return std::unique_ptr<GPUTextureBuffer>(
    new OpenGLTextureBuffer(format, size_in_elements, std::move(buffer), texture_id));
}

OpenGLDownloadTexture::OpenGLDownloadTexture(u32 width, u32 height, GPUTexture::Format format, bool imported,
                                             GLuint buffer_id, u8* cpu_buffer, const u8* map_ptr, u32 map_pitch)
  : GPUDownloadTexture(width, height, format, imported), m_buffer_id(buffer_id), m_cpu_buffer(cpu_buffer)
{
  m_map_pointer = map_ptr;
  m_current_pitch = map_pitch;
}

OpenGLDownloadTexture::~OpenGLDownloadTexture()
{
  if (m_buffer_id != 0)
  {
    if (m_sync)
      glDeleteSync(m_sync);

    if (m_map_pointer)
    {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_id);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    glDeleteBuffers(1, &m_buffer_id);
  }
  else if (m_cpu_buffer && !m_is_imported)
  {
    Common::AlignedFree(m_cpu_buffer);
  }
}

std::unique_ptr<OpenGLDownloadTexture> OpenGLDownloadTexture::Create(u32 width, u32 height, GPUTexture::Format format,
                                                                     void* memory, size_t memory_size, u32 memory_pitch,
                                                                     Error* error)
{
  const u32 buffer_pitch =
    memory ? memory_pitch :
             Common::AlignUpPow2(GPUTexture::CalcUploadPitch(format, width), TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  const u32 buffer_size = memory ? static_cast<u32>(memory_size) : (height * buffer_pitch);

  const bool use_buffer_storage = (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage) &&
                                  !memory && OpenGLDevice::ShouldUsePBOsForDownloads();
  if (use_buffer_storage)
  {
    GLuint buffer_id;
    glGenBuffers(1, &buffer_id);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_id);

    const u32 flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const u32 map_flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT;

    if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
      glBufferStorage(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, flags);
    else if (GLAD_GL_EXT_buffer_storage)
      glBufferStorageEXT(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, flags);

    u8* buffer_map = static_cast<u8*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, buffer_size, map_flags));

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (!buffer_map)
    {
      Error::SetStringView(error, "Failed to map persistent download buffer");
      glDeleteBuffers(1, &buffer_id);
      return {};
    }

    return std::unique_ptr<OpenGLDownloadTexture>(
      new OpenGLDownloadTexture(width, height, format, false, buffer_id, nullptr, buffer_map, buffer_pitch));
  }

  // Fallback to glReadPixels() + CPU buffer.
  const bool imported = (memory != nullptr);
  u8* cpu_buffer =
    imported ? static_cast<u8*>(memory) : static_cast<u8*>(Common::AlignedMalloc(buffer_size, VECTOR_ALIGNMENT));
  if (!cpu_buffer) [[unlikely]]
  {
    Error::SetStringView(error, "Failed to get client-side memory pointer.");
    return {};
  }

  return std::unique_ptr<OpenGLDownloadTexture>(
    new OpenGLDownloadTexture(width, height, format, imported, 0, cpu_buffer, cpu_buffer, buffer_pitch));
}

void OpenGLDownloadTexture::CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width,
                                            u32 height, u32 src_layer, u32 src_level, bool use_transfer_pitch)
{
  OpenGLTexture* const srcgl = static_cast<OpenGLTexture*>(src);
  OpenGLDevice& dev = OpenGLDevice::GetInstance();

  DebugAssert(srcgl->GetFormat() == m_format);
  DebugAssert(src_level < srcgl->GetLevels());
  DebugAssert((src_x + width) <= srcgl->GetMipWidth(src_level) && (src_y + height) <= srcgl->GetMipHeight(src_level));
  DebugAssert((dst_x + width) <= m_width && (dst_y + height) <= m_height);
  DebugAssert((dst_x == 0 && dst_y == 0) || !use_transfer_pitch);
  DebugAssert(!m_is_imported || !use_transfer_pitch);

  dev.CommitClear(srcgl);

  u32 copy_offset, copy_size, copy_rows;
  if (!m_is_imported)
    m_current_pitch = GetTransferPitch(use_transfer_pitch ? width : m_width, TEXTURE_UPLOAD_PITCH_ALIGNMENT);
  GetTransferSize(dst_x, dst_y, width, height, m_current_pitch, &copy_offset, &copy_size, &copy_rows);
  dev.GetStatistics().num_downloads++;

  GLint alignment;
  if (m_current_pitch & 1)
    alignment = 1;
  else if (m_current_pitch & 2)
    alignment = 2;
  else
    alignment = 4;

  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  glPixelStorei(GL_PACK_ROW_LENGTH, GPUTexture::CalcUploadRowLengthFromPitch(m_format, m_current_pitch));

  if (!m_cpu_buffer)
  {
    // Read to PBO.
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer_id);
  }

  const auto [gl_internal_format, gl_format, gl_type] =
    OpenGLTexture::GetPixelFormatMapping(srcgl->GetFormat(), dev.IsGLES());
  if (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_get_texture_sub_image)
  {
    glGetTextureSubImage(srcgl->GetGLId(), src_level, src_x, src_y, 0, width, height, 1, gl_format, gl_type,
                         m_current_pitch * height, m_cpu_buffer + copy_offset);
  }
  else
  {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, dev.m_read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcgl->GetGLId(), 0);

    glReadPixels(src_x, src_y, width, height, gl_format, gl_type, m_cpu_buffer + copy_offset);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }

  if (m_cpu_buffer)
  {
    // If using CPU buffers, we never need to flush.
    m_needs_flush = false;
  }
  else
  {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // Create a sync object so we know when the GPU is done copying.
    if (m_sync)
      glDeleteSync(m_sync);

    m_sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    m_needs_flush = true;
  }

  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

bool OpenGLDownloadTexture::Map(u32 x, u32 y, u32 width, u32 height)
{
  // Either always mapped, or CPU buffer.
  return true;
}

void OpenGLDownloadTexture::Unmap()
{
  // Either always mapped, or CPU buffer.
}

void OpenGLDownloadTexture::Flush()
{
  // If we're using CPU buffers, we did the readback synchronously...
  if (!m_needs_flush || !m_sync)
    return;

  m_needs_flush = false;

  glClientWaitSync(m_sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
  glDeleteSync(m_sync);
  m_sync = {};
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void OpenGLDownloadTexture::SetDebugName(std::string_view name)
{
  if (name.empty())
    return;

  if (glObjectLabel)
    glObjectLabel(GL_BUFFER, m_buffer_id, static_cast<GLsizei>(name.length()), name.data());
}

#endif

std::unique_ptr<GPUDownloadTexture>
OpenGLDevice::CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format, Error* error /* = nullptr */)
{
  return OpenGLDownloadTexture::Create(width, height, format, nullptr, 0, 0, error);
}

std::unique_ptr<GPUDownloadTexture> OpenGLDevice::CreateDownloadTexture(u32 width, u32 height,
                                                                        GPUTexture::Format format, void* memory,
                                                                        size_t memory_size, u32 memory_stride,
                                                                        Error* error /* = nullptr */)
{
  // not _really_ memory importing, but PBOs are broken on Intel....
  return OpenGLDownloadTexture::Create(width, height, format, memory, memory_size, memory_stride, error);
}
