// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"
#include "gpu_shader_cache.h"
#include "opengl_loader.h"

class OpenGLDevice;

class OpenGLShader final : public GPUShader
{
  friend OpenGLDevice;

public:
  ~OpenGLShader() override;

  void SetDebugName(std::string_view name) override;

  bool Compile(Error* error);

  ALWAYS_INLINE GLuint GetGLId() const { return m_id.value(); }
  ALWAYS_INLINE const GPUShaderCache::CacheIndexKey& GetKey() const { return m_key; }
  ALWAYS_INLINE const std::string& GetSource() const { return m_source; }

private:
  OpenGLShader(GPUShaderStage stage, const GPUShaderCache::CacheIndexKey& key, std::string source);

  GPUShaderCache::CacheIndexKey m_key;
  std::string m_source;
  std::optional<GLuint> m_id;
  bool m_compile_tried = false;

#ifdef _DEBUG
  std::string m_debug_name;
#endif
};

class OpenGLPipeline final : public GPUPipeline
{
  friend OpenGLDevice;

public:
  static constexpr u32 MAX_VERTEX_ATTRIBUTES = 7;

  struct VertexArrayCacheKey
  {
    VertexAttribute vertex_attributes[MAX_VERTEX_ATTRIBUTES];
    u32 vertex_attribute_stride;
    u32 num_vertex_attributes;

    bool operator==(const VertexArrayCacheKey& rhs) const;
    bool operator!=(const VertexArrayCacheKey& rhs) const;
  };
  struct VertexArrayCacheItem
  {
    GLuint vao_id;
    u32 reference_count;
  };
  struct VertexArrayCacheKeyHash
  {
    size_t operator()(const VertexArrayCacheKey& k) const;
  };
  using VertexArrayCache = std::unordered_map<VertexArrayCacheKey, VertexArrayCacheItem, VertexArrayCacheKeyHash>;

  struct ProgramCacheKey
  {
    u64 vs_hash_low, vs_hash_high;
    u64 gs_hash_low, gs_hash_high;
    u64 fs_hash_low, fs_hash_high;
    u32 vs_length;
    u32 gs_length;
    u32 fs_length;
    VertexArrayCacheKey va_key;

    bool operator==(const ProgramCacheKey& rhs) const;
    bool operator!=(const ProgramCacheKey& rhs) const;
  };
  static_assert(sizeof(ProgramCacheKey) == 96); // Has no padding
  struct ProgramCacheKeyHash
  {
    size_t operator()(const ProgramCacheKey& k) const;
  };
  struct ProgramCacheItem
  {
    GLuint program_id;
    u32 reference_count;
    GLenum file_format;
    u32 file_offset;
    u32 file_compressed_size;
    u32 file_uncompressed_size;
  };
  using ProgramCache = std::unordered_map<ProgramCacheKey, ProgramCacheItem, ProgramCacheKeyHash>;

  static ProgramCacheKey GetProgramCacheKey(const GraphicsConfig& plconfig);

  ~OpenGLPipeline() override;

  ALWAYS_INLINE GLuint GetProgram() const { return m_program; }
  ALWAYS_INLINE VertexArrayCache::const_iterator GetVAO() const { return m_vao; }
  ALWAYS_INLINE const RasterizationState& GetRasterizationState() const { return m_rasterization_state; }
  ALWAYS_INLINE const DepthState& GetDepthState() const { return m_depth_state; }
  ALWAYS_INLINE const BlendState& GetBlendState() const { return m_blend_state; }
  ALWAYS_INLINE GLenum GetTopology() const { return m_topology; }

  void SetDebugName(std::string_view name) override;

private:
  OpenGLPipeline(const ProgramCacheKey& key, GLuint program, VertexArrayCache::const_iterator vao,
                 const RasterizationState& rs, const DepthState& ds, const BlendState& bs, GLenum topology);

  ProgramCacheKey m_key;
  VertexArrayCache::const_iterator m_vao;
  GLuint m_program;
  BlendState m_blend_state;
  RasterizationState m_rasterization_state;
  DepthState m_depth_state;
  GLenum m_topology;
};
