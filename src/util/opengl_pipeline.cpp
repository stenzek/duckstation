// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_pipeline.h"
#include "compress_helpers.h"
#include "opengl_device.h"
#include "opengl_stream_buffer.h"
#include "shadergen.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/hash_combine.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <cerrno>

LOG_CHANNEL(GPUDevice);

namespace {

struct PipelineDiskCacheFooter
{
  u32 version;
  u32 num_programs;
  char driver_vendor[128];
  char driver_renderer[128];
  char driver_version[128];
};
static_assert(sizeof(PipelineDiskCacheFooter) == (sizeof(u32) * 2 + 128 * 3));

struct PipelineDiskCacheIndexEntry
{
  OpenGLPipeline::ProgramCacheKey key;
  u32 format;
  u32 offset;
  u32 uncompressed_size;
  u32 compressed_size;
};
static_assert(sizeof(PipelineDiskCacheIndexEntry) == 112); // No padding

struct VAMapping
{
  GLenum type;
  GLboolean normalized;
  GLboolean integer;
};

} // namespace

static constexpr const std::array<VAMapping, static_cast<u8>(GPUPipeline::VertexAttribute::Type::MaxCount)>
  s_vao_format_mapping = {{
    {GL_FLOAT, GL_FALSE, GL_FALSE},         // Float
    {GL_UNSIGNED_BYTE, GL_FALSE, GL_TRUE},  // UInt8
    {GL_BYTE, GL_FALSE, GL_TRUE},           // SInt8
    {GL_UNSIGNED_BYTE, GL_TRUE, GL_FALSE},  // UNorm8
    {GL_UNSIGNED_SHORT, GL_FALSE, GL_TRUE}, // UInt16
    {GL_SHORT, GL_FALSE, GL_TRUE},          // SInt16
    {GL_UNSIGNED_SHORT, GL_TRUE, GL_FALSE}, // UNorm16
    {GL_UNSIGNED_INT, GL_FALSE, GL_TRUE},   // UInt32
    {GL_INT, GL_FALSE, GL_TRUE},            // SInt32
  }};

static GLenum GetGLShaderType(GPUShaderStage stage)
{
  static constexpr std::array<GLenum, static_cast<u32>(GPUShaderStage::MaxCount)> mapping = {{
    GL_VERTEX_SHADER,   // Vertex
    GL_FRAGMENT_SHADER, // Fragment
    GL_GEOMETRY_SHADER, // Geometry
    GL_COMPUTE_SHADER,  // Compute
  }};

  return mapping[static_cast<u32>(stage)];
}

static void FillFooter(PipelineDiskCacheFooter* footer, u32 version)
{
  footer->version = version;
  footer->num_programs = 0;
  StringUtil::Strlcpy(footer->driver_vendor, reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                      std::size(footer->driver_vendor));
  StringUtil::Strlcpy(footer->driver_renderer, reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                      std::size(footer->driver_renderer));
  StringUtil::Strlcpy(footer->driver_version, reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                      std::size(footer->driver_version));
}

OpenGLShader::OpenGLShader(GPUShaderStage stage, const GPUShaderCache::CacheIndexKey& key, std::string source)
  : GPUShader(stage), m_key(key), m_source(std::move(source))
{
}

OpenGLShader::~OpenGLShader()
{
  if (m_id.has_value())
    glDeleteShader(m_id.value());
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void OpenGLShader::SetDebugName(std::string_view name)
{
  if (glObjectLabel)
  {
    if (m_id.has_value())
    {
      m_debug_name = {};
      glObjectLabel(GL_SHADER, m_id.value(), static_cast<GLsizei>(name.length()),
                    static_cast<const GLchar*>(name.data()));
    }
    else
    {
      m_debug_name = name;
    }
  }
}

#endif

bool OpenGLShader::Compile(Error* error)
{
  if (m_compile_tried)
  {
    if (!m_id.has_value()) [[unlikely]]
    {
      Error::SetStringView(error, "Shader previously failed to compile.");
      return false;
    }

    return true;
  }

  m_compile_tried = true;

  glGetError();

  GLuint shader = glCreateShader(GetGLShaderType(m_stage));
  if (GLenum err = glGetError(); err != GL_NO_ERROR)
  {
    ERROR_LOG("glCreateShader() failed: {}", err);
    OpenGLDevice::SetErrorObject(error, "glCreateShader() failed: ", err);
    return false;
  }

  const GLchar* string = m_source.data();
  const GLint length = static_cast<GLint>(m_source.length());
  glShaderSource(shader, 1, &string, &length);
  glCompileShader(shader);

  GLint status = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

  GLint info_log_length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0)
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetShaderInfoLog(shader, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      ERROR_LOG("Shader compiled with warnings:\n{}", info_log);
    }
    else
    {
      ERROR_LOG("Shader failed to compile:\n{}", info_log);
      Error::SetStringFmt(error, "Shader failed to compile:\n{}", info_log);
      GPUDevice::DumpBadShader(m_source, info_log);
      glDeleteShader(shader);
      return false;
    }
  }

  m_id = shader;

#ifdef ENABLE_GPU_OBJECT_NAMES
  if (glObjectLabel && !m_debug_name.empty())
  {
    glObjectLabel(GL_SHADER, shader, static_cast<GLsizei>(m_debug_name.length()),
                  static_cast<const GLchar*>(m_debug_name.data()));
    m_debug_name = {};
  }
#endif

  return true;
}

std::unique_ptr<GPUShader> OpenGLDevice::CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                                Error* error)
{
  // Not supported.. except spir-v maybe? but no point really...
  Error::SetStringView(error, "Not supported.");
  return {};
}

std::unique_ptr<GPUShader> OpenGLDevice::CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
                                                                std::string_view source, const char* entry_point,
                                                                DynamicHeapArray<u8>* out_binary, Error* error)
{
  const GPUShaderLanguage expected_language = IsGLES() ? GPUShaderLanguage::GLSLES : GPUShaderLanguage::GLSL;
  if (language != expected_language)
  {
    return TranspileAndCreateShaderFromSource(
      stage, language, source, entry_point, expected_language,
      ShaderGen::GetGLSLVersion(IsGLES() ? RenderAPI::OpenGLES : RenderAPI::OpenGL), out_binary, error);
  }

  if (std::strcmp(entry_point, "main") != 0)
  {
    ERROR_LOG("Entry point must be 'main', but got '{}' instead.", entry_point);
    Error::SetStringFmt(error, "Entry point must be 'main', but got '{}' instead.", entry_point);
    return {};
  }

  return std::unique_ptr<GPUShader>(
    new OpenGLShader(stage, GPUShaderCache::GetCacheKey(stage, language, source, entry_point), std::string(source)));
}

//////////////////////////////////////////////////////////////////////////

bool OpenGLPipeline::VertexArrayCacheKey::operator==(const VertexArrayCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}

bool OpenGLPipeline::VertexArrayCacheKey::operator!=(const VertexArrayCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

size_t OpenGLPipeline::VertexArrayCacheKeyHash::operator()(const VertexArrayCacheKey& k) const
{
  std::size_t h = 0;
  hash_combine(h, k.num_vertex_attributes, k.vertex_attribute_stride);
  for (const VertexAttribute& va : k.vertex_attributes)
    hash_combine(h, va.key);
  return h;
}

bool OpenGLPipeline::ProgramCacheKey::operator==(const ProgramCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) == 0);
}

bool OpenGLPipeline::ProgramCacheKey::operator!=(const ProgramCacheKey& rhs) const
{
  return (std::memcmp(this, &rhs, sizeof(*this)) != 0);
}

size_t OpenGLPipeline::ProgramCacheKeyHash::operator()(const ProgramCacheKey& k) const
{
  // TODO: maybe use xxhash here...
  std::size_t h = 0;
  hash_combine(h, k.va_key.num_vertex_attributes, k.va_key.vertex_attribute_stride);
  for (const VertexAttribute& va : k.va_key.vertex_attributes)
    hash_combine(h, va.key);
  hash_combine(h, k.vs_hash_low, k.vs_hash_high, k.vs_length);
  hash_combine(h, k.fs_hash_low, k.fs_hash_high, k.fs_length);
  hash_combine(h, k.gs_hash_low, k.gs_hash_high, k.gs_length);
  return h;
}

OpenGLPipeline::ProgramCacheKey OpenGLPipeline::GetProgramCacheKey(const GraphicsConfig& plconfig)
{
  Assert(plconfig.input_layout.vertex_attributes.size() <= MAX_VERTEX_ATTRIBUTES);

  const GPUShaderCache::CacheIndexKey& vs_key = static_cast<const OpenGLShader*>(plconfig.vertex_shader)->GetKey();
  const GPUShaderCache::CacheIndexKey& fs_key = static_cast<const OpenGLShader*>(plconfig.fragment_shader)->GetKey();
  const GPUShaderCache::CacheIndexKey* gs_key =
    plconfig.geometry_shader ? &static_cast<const OpenGLShader*>(plconfig.geometry_shader)->GetKey() : nullptr;

  ProgramCacheKey ret;
  ret.vs_hash_low = vs_key.source_hash_low;
  ret.vs_hash_high = vs_key.source_hash_high;
  ret.vs_length = vs_key.source_length;
  ret.fs_hash_low = fs_key.source_hash_low;
  ret.fs_hash_high = fs_key.source_hash_high;
  ret.fs_length = fs_key.source_length;
  ret.gs_hash_low = gs_key ? gs_key->source_hash_low : 0;
  ret.gs_hash_high = gs_key ? gs_key->source_hash_high : 0;
  ret.gs_length = gs_key ? gs_key->source_length : 0;

  std::memset(ret.va_key.vertex_attributes, 0, sizeof(ret.va_key.vertex_attributes));
  ret.va_key.vertex_attribute_stride = 0;
  ret.va_key.num_vertex_attributes = static_cast<u32>(plconfig.input_layout.vertex_attributes.size());

  if (ret.va_key.num_vertex_attributes > 0)
  {
    std::memcpy(ret.va_key.vertex_attributes, plconfig.input_layout.vertex_attributes.data(),
                sizeof(VertexAttribute) * ret.va_key.num_vertex_attributes);
    ret.va_key.vertex_attribute_stride = plconfig.input_layout.vertex_stride;
  }

  return ret;
}

GLuint OpenGLDevice::LookupProgramCache(const OpenGLPipeline::ProgramCacheKey& key,
                                        const GPUPipeline::GraphicsConfig& plconfig, Error* error)
{
  auto it = m_program_cache.find(key);
  if (it != m_program_cache.end() && it->second.program_id == 0 && it->second.file_uncompressed_size > 0)
  {
    it->second.program_id = CreateProgramFromPipelineCache(it->second, plconfig);
    if (it->second.program_id == 0)
    {
      ERROR_LOG("Failed to create program from binary.");
      m_program_cache.erase(it);
      it = m_program_cache.end();
      DiscardPipelineCache();
    }
  }

  if (it != m_program_cache.end())
  {
    if (it->second.program_id != 0)
      it->second.reference_count++;

    return it->second.program_id;
  }

  const GLuint program_id = CompileProgram(plconfig, error);
  if (program_id == 0)
  {
    // Compile failed, don't add to map, it just gets confusing.
    return 0;
  }

  OpenGLPipeline::ProgramCacheItem item;
  item.program_id = program_id;
  item.reference_count = 1;
  item.file_format = 0;
  item.file_offset = 0;
  item.file_uncompressed_size = 0;
  item.file_compressed_size = 0;
  if (m_pipeline_disk_cache_file)
    AddToPipelineCache(&item);

  m_program_cache.emplace(key, item);
  return item.program_id;
}

GLuint OpenGLDevice::CompileProgram(const GPUPipeline::GraphicsConfig& plconfig, Error* error)
{
  OpenGLShader* vertex_shader = static_cast<OpenGLShader*>(plconfig.vertex_shader);
  OpenGLShader* fragment_shader = static_cast<OpenGLShader*>(plconfig.fragment_shader);
  OpenGLShader* geometry_shader = static_cast<OpenGLShader*>(plconfig.geometry_shader);
  if (!vertex_shader || !fragment_shader || !vertex_shader->Compile(error) || !fragment_shader->Compile(error) ||
      (geometry_shader && !geometry_shader->Compile(error))) [[unlikely]]
  {
    return 0;
  }

  glGetError();
  const GLuint program_id = glCreateProgram();
  if (const GLenum err = glGetError(); err != GL_NO_ERROR) [[unlikely]]
  {
    SetErrorObject(error, "glCreateProgram() failed: ", err);
    return 0;
  }

  if (m_pipeline_disk_cache_file)
    glProgramParameteri(program_id, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

  Assert(plconfig.vertex_shader && plconfig.fragment_shader);
  glAttachShader(program_id, vertex_shader->GetGLId());
  glAttachShader(program_id, fragment_shader->GetGLId());
  if (geometry_shader)
    glAttachShader(program_id, geometry_shader->GetGLId());

  if (!ShaderGen::UseGLSLBindingLayout())
  {
    static constexpr std::array<const char*, static_cast<u8>(GPUPipeline::VertexAttribute::Semantic::MaxCount)>
      semantic_vars = {{
        "a_pos", // Position
        "a_tex", // TexCoord
        "a_col", // Color
      }};

    for (u32 i = 0; i < static_cast<u32>(plconfig.input_layout.vertex_attributes.size()); i++)
    {
      const GPUPipeline::VertexAttribute& va = plconfig.input_layout.vertex_attributes[i];
      if (va.semantic == GPUPipeline::VertexAttribute::Semantic::Position && va.semantic_index == 0)
      {
        glBindAttribLocation(program_id, i, "a_pos");
      }
      else
      {
        glBindAttribLocation(program_id, i,
                             TinyString::from_format("{}{}", semantic_vars[static_cast<u8>(va.semantic.GetValue())],
                                                     static_cast<u8>(va.semantic_index)));
      }
    }

    // Output colour is implicit in GLES.
    const bool is_gles = m_gl_context->IsGLES();
    if (!is_gles)
      glBindFragDataLocation(program_id, 0, "o_col0");

    if (m_features.dual_source_blend)
    {
      if (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended)
      {
        if (is_gles)
          glBindFragDataLocationIndexed(program_id, 0, 0, "o_col0");
        glBindFragDataLocationIndexed(program_id, 1, 0, "o_col1");
      }
      else if (GLAD_GL_EXT_blend_func_extended)
      {
        if (is_gles)
          glBindFragDataLocationIndexedEXT(program_id, 0, 0, "o_col1");
        glBindFragDataLocationIndexedEXT(program_id, 1, 0, "o_col1");
      }
    }
  }

  glLinkProgram(program_id);

  GLint status = GL_FALSE;
  glGetProgramiv(program_id, GL_LINK_STATUS, &status);

  GLint info_log_length = 0;
  glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &info_log_length);

  if (status == GL_FALSE || info_log_length > 0) [[unlikely]]
  {
    std::string info_log;
    info_log.resize(info_log_length + 1);
    glGetProgramInfoLog(program_id, info_log_length, &info_log_length, &info_log[0]);

    if (status == GL_TRUE)
    {
      ERROR_LOG("Program linked with warnings:\n{}", info_log);
    }
    else
    {
      {
        std::stringstream ss;
        ss << "########## VERTEX SHADER ##########\n";
        ss << vertex_shader->GetSource();
        if (geometry_shader)
        {
          ss << "\n########## GEOMETRY SHADER ##########\n";
          ss << geometry_shader->GetSource();
        }
        ss << "\n########## FRAGMENT SHADER ##########\n";
        ss << fragment_shader->GetSource();
        ss << "\n#####################################\n";
        DumpBadShader(std::move(ss).str(), info_log);
      }

      ERROR_LOG("Program failed to link:\n{}", info_log);
      Error::SetStringFmt(error, "Program failed to link:\n{}", info_log);
      glDeleteProgram(program_id);
      return 0;
    }
  }

  PostLinkProgram(plconfig, program_id);

  return program_id;
}

void OpenGLDevice::PostLinkProgram(const GPUPipeline::GraphicsConfig& plconfig, GLuint program_id)
{
  if (!ShaderGen::UseGLSLBindingLayout())
  {
    const GLint ubo_location = glGetUniformBlockIndex(program_id, "UBOBlock");
    if (ubo_location >= 0)
      glUniformBlockBinding(program_id, ubo_location, 0);
    const GLint push_constant_location = glGetUniformBlockIndex(program_id, "PushConstants");
    if (push_constant_location >= 0)
      glUniformBlockBinding(program_id, push_constant_location, 1);

    glUseProgram(program_id);

    // Texture buffer is zero here, so we have to bump it.
    const u32 num_textures = std::max<u32>(GetActiveTexturesForLayout(plconfig.layout), 1);
    for (u32 i = 0; i < num_textures; i++)
    {
      const GLint samp_location = glGetUniformLocation(program_id, TinyString::from_format("samp{}", i));
      if (samp_location >= 0)
        glUniform1i(samp_location, i);
    }

    glUseProgram(m_last_program);
  }
}

void OpenGLDevice::UnrefProgram(const OpenGLPipeline::ProgramCacheKey& key)
{
  auto it = m_program_cache.find(key);
  Assert(it != m_program_cache.end() && it->second.program_id != 0 && it->second.reference_count > 0);

  if ((--it->second.reference_count) > 0)
    return;

  if (m_last_program == it->second.program_id)
  {
    m_last_program = 0;
    glUseProgram(0);
  }

  glDeleteProgram(it->second.program_id);
  it->second.program_id = 0;

  // If it's not in the pipeline cache, we need to remove it completely, otherwise we won't recreate it.
  if (it->second.file_uncompressed_size == 0)
    m_program_cache.erase(it);
}

OpenGLPipeline::VertexArrayCache::const_iterator
OpenGLDevice::LookupVAOCache(const OpenGLPipeline::VertexArrayCacheKey& key, Error* error)
{
  OpenGLPipeline::VertexArrayCache::iterator it = m_vao_cache.find(key);
  if (it != m_vao_cache.end())
  {
    it->second.reference_count++;
    return it;
  }

  OpenGLPipeline::VertexArrayCacheItem item;
  item.vao_id =
    CreateVAO(std::span<const GPUPipeline::VertexAttribute>(key.vertex_attributes, key.num_vertex_attributes),
              key.vertex_attribute_stride, error);
  if (item.vao_id == 0)
    return m_vao_cache.cend();

  item.reference_count = 1;
  return m_vao_cache.emplace(key, item).first;
}

GLuint OpenGLDevice::CreateVAO(std::span<const GPUPipeline::VertexAttribute> attributes, u32 stride, Error* error)
{
  glGetError();
  GLuint vao;
  glGenVertexArrays(1, &vao);
  if (const GLenum err = glGetError(); err != GL_NO_ERROR)
  {
    ERROR_LOG("Failed to create vertex array object: {}", err);
    SetErrorObject(error, "glGenVertexArrays() failed: ", err);
    return 0;
  }

  glBindVertexArray(vao);
  m_vertex_buffer->Bind();
  m_index_buffer->Bind();

  for (u32 i = 0; i < static_cast<u32>(attributes.size()); i++)
  {
    const GPUPipeline::VertexAttribute& va = attributes[i];
    const VAMapping& m = s_vao_format_mapping[static_cast<u8>(va.type.GetValue())];
    const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(va.offset.GetValue()));
    glEnableVertexAttribArray(i);
    if (m.integer)
      glVertexAttribIPointer(i, va.components, m.type, stride, ptr);
    else
      glVertexAttribPointer(i, va.components, m.type, m.normalized, stride, ptr);
  }

  if (m_last_vao != m_vao_cache.cend())
    glBindVertexArray(m_last_vao->second.vao_id);

  return vao;
}

void OpenGLDevice::SetVertexBufferOffsets(u32 base_vertex)
{
  const OpenGLPipeline::VertexArrayCacheKey& va = m_last_vao->first;
  const u32 stride = va.vertex_attribute_stride;
  const u32 base_vertex_start = base_vertex * stride;

  if (glBindVertexBuffer) [[likely]]
  {
    for (u32 i = 0; i < va.num_vertex_attributes; i++)
    {
      glBindVertexBuffer(i, m_vertex_buffer->GetGLBufferId(), base_vertex_start + va.vertex_attributes[i].offset,
                         static_cast<GLsizei>(stride));
    }
  }
  else
  {
    for (u32 i = 0; i < va.num_vertex_attributes; i++)
    {
      const GPUPipeline::VertexAttribute& attrib = va.vertex_attributes[i];
      const void* ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(base_vertex_start + attrib.offset));
      const VAMapping& m = s_vao_format_mapping[static_cast<u8>(attrib.type.GetValue())];
      if (m.integer)
        glVertexAttribIPointer(i, attrib.components, m.type, stride, ptr);
      else
        glVertexAttribPointer(i, attrib.components, m.type, m.normalized, stride, ptr);
    }
  }
}

void OpenGLDevice::UnrefVAO(const OpenGLPipeline::VertexArrayCacheKey& key)
{
  auto it = m_vao_cache.find(key);
  Assert(it != m_vao_cache.end() && it->second.reference_count > 0);

  if ((--it->second.reference_count) > 0)
    return;

  if (m_last_vao == it)
  {
    m_last_vao = m_vao_cache.cend();
    glBindVertexArray(0);
  }

  glDeleteVertexArrays(1, &it->second.vao_id);
  m_vao_cache.erase(it);
}

OpenGLPipeline::OpenGLPipeline(const ProgramCacheKey& key, GLuint program, VertexArrayCache::const_iterator vao,
                               const RasterizationState& rs, const DepthState& ds, const BlendState& bs,
                               GLenum topology)
  : m_key(key), m_vao(vao), m_program(program), m_blend_state(bs), m_rasterization_state(rs), m_depth_state(ds),
    m_topology(topology)
{
}

OpenGLPipeline::~OpenGLPipeline()
{
  OpenGLDevice& dev = OpenGLDevice::GetInstance();
  dev.UnbindPipeline(this);
  dev.UnrefProgram(m_key);
  dev.UnrefVAO(m_key.va_key);
}

#ifdef ENABLE_GPU_OBJECT_NAMES

void OpenGLPipeline::SetDebugName(std::string_view name)
{
  if (glObjectLabel)
    glObjectLabel(GL_PROGRAM, m_program, static_cast<u32>(name.length()), name.data());
}

#endif

std::unique_ptr<GPUPipeline> OpenGLDevice::CreatePipeline(const GPUPipeline::GraphicsConfig& config, Error* error)
{
  const OpenGLPipeline::ProgramCacheKey pkey = OpenGLPipeline::GetProgramCacheKey(config);

  const GLuint program_id = LookupProgramCache(pkey, config, error);
  if (program_id == 0)
    return {};

  const OpenGLPipeline::VertexArrayCache::const_iterator vao = LookupVAOCache(pkey.va_key, error);
  if (vao == m_vao_cache.cend())
  {
    UnrefProgram(pkey);
    return {};
  }

  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::Primitive::MaxCount)> primitives = {{
    GL_POINTS,         // Points
    GL_LINES,          // Lines
    GL_TRIANGLES,      // Triangles
    GL_TRIANGLE_STRIP, // TriangleStrips
  }};

  return std::unique_ptr<GPUPipeline>(new OpenGLPipeline(pkey, program_id, vao, config.rasterization, config.depth,
                                                         config.blend, primitives[static_cast<u8>(config.primitive)]));
}

ALWAYS_INLINE_RELEASE void OpenGLDevice::ApplyRasterizationState(GPUPipeline::RasterizationState rs)
{
  if (m_last_rasterization_state == rs)
    return;

  // Only one thing, no need to check.
  if (rs.cull_mode == GPUPipeline::CullMode::None)
  {
    glDisable(GL_CULL_FACE);
  }
  else
  {
    glEnable(GL_CULL_FACE);
    glCullFace((rs.cull_mode == GPUPipeline::CullMode::Front) ? GL_FRONT : GL_BACK);
  }

  m_last_rasterization_state = rs;
}

ALWAYS_INLINE_RELEASE void OpenGLDevice::ApplyDepthState(GPUPipeline::DepthState ds)
{
  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::DepthFunc::MaxCount)> func_mapping = {{
    GL_NEVER,   // Never
    GL_ALWAYS,  // Always
    GL_LESS,    // Less
    GL_LEQUAL,  // LessEqual
    GL_GREATER, // Greater
    GL_GEQUAL,  // GreaterEqual
    GL_EQUAL,   // Equal
  }};

  if (m_last_depth_state == ds)
    return;

  (ds.depth_test != GPUPipeline::DepthFunc::Always || ds.depth_write) ? glEnable(GL_DEPTH_TEST) :
                                                                        glDisable(GL_DEPTH_TEST);
  glDepthFunc(func_mapping[static_cast<u8>(ds.depth_test.GetValue())]);
  if (m_last_depth_state.depth_write != ds.depth_write)
    glDepthMask(ds.depth_write);

  m_last_depth_state = ds;
}

ALWAYS_INLINE_RELEASE void OpenGLDevice::ApplyBlendState(GPUPipeline::BlendState bs)
{
  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::BlendFunc::MaxCount)> blend_mapping = {{
    GL_ZERO,                     // Zero
    GL_ONE,                      // One
    GL_SRC_COLOR,                // SrcColor
    GL_ONE_MINUS_SRC_COLOR,      // InvSrcColor
    GL_DST_COLOR,                // DstColor
    GL_ONE_MINUS_DST_COLOR,      // InvDstColor
    GL_SRC_ALPHA,                // SrcAlpha
    GL_ONE_MINUS_SRC_ALPHA,      // InvSrcAlpha
    GL_SRC1_ALPHA,               // SrcAlpha1
    GL_ONE_MINUS_SRC1_ALPHA,     // InvSrcAlpha1
    GL_DST_ALPHA,                // DstAlpha
    GL_ONE_MINUS_DST_ALPHA,      // InvDstAlpha
    GL_CONSTANT_COLOR,           // ConstantColor
    GL_ONE_MINUS_CONSTANT_COLOR, // InvConstantColor
  }};

  static constexpr std::array<GLenum, static_cast<u32>(GPUPipeline::BlendOp::MaxCount)> op_mapping = {{
    GL_FUNC_ADD,              // Add
    GL_FUNC_SUBTRACT,         // Subtract
    GL_FUNC_REVERSE_SUBTRACT, // ReverseSubtract
    GL_MIN,                   // Min
    GL_MAX,                   // Max
  }};

  // TODO: driver bugs

  if (bs == m_last_blend_state)
    return;

  if (bs.enable != m_last_blend_state.enable)
    bs.enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);

  if (bs.enable)
  {
    if (bs.blend_factors != m_last_blend_state.blend_factors)
    {
      glBlendFuncSeparate(blend_mapping[static_cast<u8>(bs.src_blend.GetValue())],
                          blend_mapping[static_cast<u8>(bs.dst_blend.GetValue())],
                          blend_mapping[static_cast<u8>(bs.src_alpha_blend.GetValue())],
                          blend_mapping[static_cast<u8>(bs.dst_alpha_blend.GetValue())]);
    }

    if (bs.blend_ops != m_last_blend_state.blend_ops)
    {
      glBlendEquationSeparate(op_mapping[static_cast<u8>(bs.blend_op.GetValue())],
                              op_mapping[static_cast<u8>(bs.alpha_blend_op.GetValue())]);
    }

    if (bs.constant != m_last_blend_state.constant)
      glBlendColor(bs.GetConstantRed(), bs.GetConstantGreen(), bs.GetConstantBlue(), bs.GetConstantAlpha());
  }
  else
  {
    // Keep old values for blend options to potentially avoid calls when re-enabling.
    bs.blend_factors.SetValue(m_last_blend_state.blend_factors);
    bs.blend_ops.SetValue(m_last_blend_state.blend_ops);
    bs.constant.SetValue(m_last_blend_state.constant);
  }

  if (bs.write_mask != m_last_blend_state.write_mask)
    glColorMask(bs.write_r, bs.write_g, bs.write_b, bs.write_a);

  m_last_blend_state = bs;
}

void OpenGLDevice::SetPipeline(GPUPipeline* pipeline)
{
  if (m_current_pipeline == pipeline)
    return;

  OpenGLPipeline* const P = static_cast<OpenGLPipeline*>(pipeline);
  m_current_pipeline = P;

  ApplyRasterizationState(P->GetRasterizationState());
  ApplyDepthState(P->GetDepthState());
  ApplyBlendState(P->GetBlendState());

  if (m_last_vao != P->GetVAO())
  {
    m_last_vao = P->GetVAO();
    glBindVertexArray(m_last_vao->second.vao_id);
  }
  if (m_last_program != P->GetProgram())
  {
    m_last_program = P->GetProgram();
    glUseProgram(m_last_program);
  }
}

bool OpenGLDevice::OpenPipelineCache(const std::string& path, Error* error)
{
  DebugAssert(!m_pipeline_disk_cache_file);

  auto fp = FileSystem::OpenManagedCFile(path.c_str(), "r+b", error);
  if (!fp)
    return false;

#ifdef HAS_POSIX_FILE_LOCK
  // Unix doesn't prevent concurrent write access, need to explicitly lock it.
  FileSystem::POSIXLock fp_lock(fp.get(), false, error);
  if (!fp_lock.IsLocked())
  {
    Error::AddPrefix(error, "Failed to lock cache file: ");
    return false;
  }
#endif

  // Read footer.
  const s64 size = FileSystem::FSize64(fp.get());
  if (size < static_cast<s64>(sizeof(PipelineDiskCacheFooter)) ||
      size >= static_cast<s64>(std::numeric_limits<u32>::max()))
  {
    Error::SetStringView(error, "Invalid cache file size.");
    return false;
  }

  PipelineDiskCacheFooter file_footer;
  if (FileSystem::FSeek64(fp.get(), size - sizeof(PipelineDiskCacheFooter), SEEK_SET) != 0 ||
      std::fread(&file_footer, sizeof(file_footer), 1, fp.get()) != 1)
  {
    Error::SetStringView(error, "Invalid cache file footer.");
    return false;
  }

  PipelineDiskCacheFooter expected_footer;
  FillFooter(&expected_footer, m_shader_cache.GetVersion());

  if (file_footer.version != expected_footer.version ||
      std::strncmp(file_footer.driver_vendor, expected_footer.driver_vendor, std::size(file_footer.driver_vendor)) !=
        0 ||
      std::strncmp(file_footer.driver_renderer, expected_footer.driver_renderer,
                   std::size(file_footer.driver_renderer)) != 0 ||
      std::strncmp(file_footer.driver_version, expected_footer.driver_version, std::size(file_footer.driver_version)) !=
        0)
  {
    Error::SetStringView(error, "Cache does not match expected driver/version.");
    return false;
  }

  m_pipeline_disk_cache_data_end = static_cast<u32>(size) - sizeof(PipelineDiskCacheFooter) -
                                   (sizeof(PipelineDiskCacheIndexEntry) * file_footer.num_programs);
  if (m_pipeline_disk_cache_data_end < 0 ||
      FileSystem::FSeek64(fp.get(), m_pipeline_disk_cache_data_end, SEEK_SET) != 0)
  {
    Error::SetStringView(error, "Failed to seek to start of index entries.");
    return false;
  }

  // Read entries.
  for (u32 i = 0; i < file_footer.num_programs; i++)
  {
    PipelineDiskCacheIndexEntry entry;
    if (std::fread(&entry, sizeof(entry), 1, fp.get()) != 1 ||
        (static_cast<s64>(entry.offset) + static_cast<s64>(entry.compressed_size)) >= size)
    {
      Error::SetStringView(error, "Failed to read disk cache entry.");
      m_program_cache.clear();
      return false;
    }

    if (m_program_cache.find(entry.key) != m_program_cache.end())
    {
      Error::SetStringView(error, "Duplicate program in disk cache.");
      m_program_cache.clear();
      return false;
    }

    OpenGLPipeline::ProgramCacheItem pitem;
    pitem.program_id = 0;
    pitem.reference_count = 0;
    pitem.file_format = entry.format;
    pitem.file_offset = entry.offset;
    pitem.file_uncompressed_size = entry.uncompressed_size;
    pitem.file_compressed_size = entry.compressed_size;
    m_program_cache.emplace(entry.key, pitem);
  }

  VERBOSE_LOG("Read {} programs from disk cache.", m_program_cache.size());
  m_pipeline_disk_cache_file = fp.release();
#ifdef HAS_POSIX_FILE_LOCK
  m_pipeline_disk_cache_file_lock = std::move(fp_lock);
#endif
  return true;
}

bool OpenGLDevice::CreatePipelineCache(const std::string& path, Error* error)
{
#ifndef HAS_POSIX_FILE_LOCK
  m_pipeline_disk_cache_file = FileSystem::OpenCFile(path.c_str(), "w+b", error);
  if (!m_pipeline_disk_cache_file)
    return false;
#else
  // Manually truncate it, that way we don't blow away another process's file on Linux.
  m_pipeline_disk_cache_file = FileSystem::OpenCFile(path.c_str(), "a+b", error);
  if (!m_pipeline_disk_cache_file || !FileSystem::FSeek64(m_pipeline_disk_cache_file, 0, SEEK_SET, error))
    return false;

  m_pipeline_disk_cache_file_lock = FileSystem::POSIXLock(m_pipeline_disk_cache_file, false, error);
  if (!m_pipeline_disk_cache_file_lock.IsLocked())
  {
    Error::AddPrefix(error, "Failed to lock cache file: ");
    std::fclose(m_pipeline_disk_cache_file);
    m_pipeline_disk_cache_file = nullptr;
    return false;
  }

  if (!FileSystem::FTruncate64(m_pipeline_disk_cache_file, 0, error))
  {
    Error::AddPrefix(error, "Failed to truncate cache file: ");
    m_pipeline_disk_cache_file_lock = {};
    std::fclose(m_pipeline_disk_cache_file);
    m_pipeline_disk_cache_file = nullptr;
  }
#endif

  m_pipeline_disk_cache_data_end = 0;
  m_pipeline_disk_cache_changed = true;
  return true;
}

GLuint OpenGLDevice::CreateProgramFromPipelineCache(const OpenGLPipeline::ProgramCacheItem& it,
                                                    const GPUPipeline::GraphicsConfig& plconfig)
{
  DynamicHeapArray<u8> compressed_data(it.file_compressed_size);

  if (FileSystem::FSeek64(m_pipeline_disk_cache_file, it.file_offset, SEEK_SET) != 0 ||
      std::fread(compressed_data.data(), it.file_compressed_size, 1, m_pipeline_disk_cache_file) != 1) [[unlikely]]
  {
    ERROR_LOG("Failed to read program from disk cache.");
    return 0;
  }

  Error error;
  CompressHelpers::OptionalByteBuffer data = CompressHelpers::DecompressBuffer(
    CompressHelpers::CompressType::Zstandard, CompressHelpers::OptionalByteBuffer(std::move(compressed_data)),
    it.file_uncompressed_size, &error);
  if (!data.has_value())
  {
    ERROR_LOG("Failed to decompress program from disk cache: {}", error.GetDescription());
    return 0;
  }

  glGetError();
  GLuint prog = glCreateProgram();
  if (const GLenum err = glGetError(); err != GL_NO_ERROR) [[unlikely]]
  {
    ERROR_LOG("Failed to create program object: {}", err);
    return 0;
  }

  glProgramBinary(prog, it.file_format, data->data(), it.file_uncompressed_size);

  GLint link_status;
  glGetProgramiv(prog, GL_LINK_STATUS, &link_status);
  if (link_status != GL_TRUE) [[unlikely]]
  {
    ERROR_LOG("Failed to create GL program from binary: status {}, discarding cache.", link_status);
    glDeleteProgram(prog);
    return 0;
  }

  PostLinkProgram(plconfig, prog);

  return prog;
}

void OpenGLDevice::AddToPipelineCache(OpenGLPipeline::ProgramCacheItem* it)
{
  DebugAssert(it->program_id != 0 && it->file_uncompressed_size == 0);
  DebugAssert(m_pipeline_disk_cache_file);

  GLint binary_size = 0;
  glGetProgramiv(it->program_id, GL_PROGRAM_BINARY_LENGTH, &binary_size);
  if (binary_size == 0)
  {
    WARNING_LOG("glGetProgramiv(GL_PROGRAM_BINARY_LENGTH) returned 0");
    return;
  }

  GLenum format = 0;
  DynamicHeapArray<u8> uncompressed_data(binary_size);
  glGetProgramBinary(it->program_id, binary_size, &binary_size, &format, uncompressed_data.data());
  if (binary_size == 0)
  {
    WARNING_LOG("glGetProgramBinary() failed");
    return;
  }
  else if (static_cast<size_t>(binary_size) != uncompressed_data.size()) [[unlikely]]
  {
    WARNING_LOG("Size changed from {} to {} after glGetProgramBinary()", uncompressed_data.size(), binary_size);
  }

  Error error;
  CompressHelpers::OptionalByteBuffer compressed_data =
    CompressHelpers::CompressToBuffer(CompressHelpers::CompressType::Zstandard,
                                      CompressHelpers::OptionalByteBuffer(std::move(uncompressed_data)), -1, &error);
  if (!compressed_data.has_value()) [[unlikely]]
  {
    ERROR_LOG("Failed to compress program: {}", error.GetDescription());
    return;
  }

  DEV_LOG("Program binary retrieved and compressed, {} -> {} bytes, format {}", binary_size, compressed_data->size(),
          format);

  if (FileSystem::FSeek64(m_pipeline_disk_cache_file, m_pipeline_disk_cache_data_end, SEEK_SET) != 0 ||
      std::fwrite(compressed_data->data(), compressed_data->size(), 1, m_pipeline_disk_cache_file) != 1)
  {
    ERROR_LOG("Failed to write binary to disk cache.");
  }

  it->file_format = format;
  it->file_offset = m_pipeline_disk_cache_data_end;
  it->file_uncompressed_size = static_cast<u32>(binary_size);
  it->file_compressed_size = static_cast<u32>(compressed_data->size());
  m_pipeline_disk_cache_data_end += static_cast<u32>(compressed_data->size());
  m_pipeline_disk_cache_changed = true;
}

bool OpenGLDevice::DiscardPipelineCache()
{
  // Remove any other disk cache entries which haven't been loaded.
  for (auto it = m_program_cache.begin(); it != m_program_cache.end();)
  {
    if (it->second.program_id != 0)
    {
      it->second.file_format = 0;
      it->second.file_offset = 0;
      it->second.file_uncompressed_size = 0;
      it->second.file_compressed_size = 0;
      ++it;
      continue;
    }

    it = m_program_cache.erase(it);
  }

  if (!m_pipeline_disk_cache_file)
  {
    // Probably shouldn't get here...
    return false;
  }

  Error error;
  if (!FileSystem::FTruncate64(m_pipeline_disk_cache_file, 0, &error))
  {
    ERROR_LOG("Failed to truncate pipeline cache: {}", error.GetDescription());
#ifdef HAS_POSIX_FILE_LOCK
    m_pipeline_disk_cache_file_lock.Unlock();
#endif
    std::fclose(m_pipeline_disk_cache_file);
    m_pipeline_disk_cache_file = nullptr;
    return false;
  }

  m_pipeline_disk_cache_data_end = 0;
  m_pipeline_disk_cache_changed = true;
  return true;
}

bool OpenGLDevice::ClosePipelineCache(const std::string& filename, Error* error)
{
  const auto close_cache = [this]() {
#ifdef HAS_POSIX_FILE_LOCK
    m_pipeline_disk_cache_file_lock.Unlock();
#endif
    std::fclose(m_pipeline_disk_cache_file);
    m_pipeline_disk_cache_file = nullptr;
  };

  if (!m_pipeline_disk_cache_changed)
  {
    VERBOSE_LOG("Not updating pipeline cache because it has not changed.");
    close_cache();
    return true;
  }

  // Rewrite footer/index entries.
  if (!FileSystem::FSeek64(m_pipeline_disk_cache_file, m_pipeline_disk_cache_data_end, SEEK_SET, error) != 0)
  {
    close_cache();
    return false;
  }

  u32 count = 0;

  for (const auto& it : m_program_cache)
  {
    if (it.second.file_uncompressed_size == 0)
      continue;

    PipelineDiskCacheIndexEntry entry;
    std::memcpy(&entry.key, &it.first, sizeof(entry.key));
    entry.format = it.second.file_format;
    entry.offset = it.second.file_offset;
    entry.compressed_size = it.second.file_compressed_size;
    entry.uncompressed_size = it.second.file_uncompressed_size;

    if (std::fwrite(&entry, sizeof(entry), 1, m_pipeline_disk_cache_file) != 1) [[unlikely]]
    {
      Error::SetErrno(error, "fwrite() for entry failed: ", errno);
      close_cache();
      return false;
    }

    count++;
  }

  PipelineDiskCacheFooter footer;
  FillFooter(&footer, m_shader_cache.GetVersion());
  footer.num_programs = count;

  if (std::fwrite(&footer, sizeof(footer), 1, m_pipeline_disk_cache_file) != 1 ||
      std::fflush(m_pipeline_disk_cache_file) != 0) [[unlikely]]
  {
    Error::SetErrno(error, "fwrite() for footer failed: ", errno);
    close_cache();
    return false;
  }

  close_cache();
  return true;
}
