// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"
#include "gpu_framebuffer_manager.h"
#include "gpu_shader_cache.h"
#include "opengl_context.h"
#include "opengl_loader.h"
#include "opengl_pipeline.h"
#include "opengl_texture.h"

#include "common/rectangle.h"

#include <cstdio>
#include <memory>
#include <tuple>

class OpenGLPipeline;
class OpenGLStreamBuffer;
class OpenGLTexture;

class OpenGLDevice final : public GPUDevice
{
public:
  OpenGLDevice();
  ~OpenGLDevice();

  ALWAYS_INLINE static OpenGLDevice& GetInstance() { return *static_cast<OpenGLDevice*>(g_gpu_device.get()); }
  ALWAYS_INLINE static OpenGLStreamBuffer* GetTextureStreamBuffer()
  {
    return GetInstance().m_texture_stream_buffer.get();
  }
  ALWAYS_INLINE static bool IsGLES() { return GetInstance().m_gl_context->IsGLES(); }
  static void BindUpdateTextureUnit();

  RenderAPI GetRenderAPI() const override;

  bool HasSurface() const override;
  void DestroySurface() override;

  bool UpdateWindow() override;
  void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) override;

  std::string GetDriverInfo() const override;

  AdapterAndModeList GetAdapterAndModeList() override;

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Type type, GPUTexture::Format format,
                                            const void* data = nullptr, u32 data_stride = 0) override;
  std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config) override;
  std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements) override;

  bool DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                       u32 out_data_stride) override;
  bool SupportsTextureFormat(GPUTexture::Format format) const override;
  void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                         u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) override;
  void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                            u32 src_x, u32 src_y, u32 width, u32 height) override;
  void ClearRenderTarget(GPUTexture* t, u32 c) override;
  void ClearDepth(GPUTexture* t, float d) override;
  void InvalidateRenderTarget(GPUTexture* t) override;

  std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data) override;
  std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                    const char* entry_point, DynamicHeapArray<u8>* out_binary) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config) override;

  void PushDebugGroup(const char* name) override;
  void PopDebugGroup() override;
  void InsertDebugMessage(const char* msg) override;

  void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                       u32* map_base_vertex) override;
  void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) override;
  void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) override;
  void UnmapIndexBuffer(u32 used_index_count) override;
  void PushUniformBuffer(const void* data, u32 data_size) override;
  void* MapUniformBuffer(u32 size) override;
  void UnmapUniformBuffer(u32 size) override;
  void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds) override;
  void SetPipeline(GPUPipeline* pipeline) override;
  void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) override;
  void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) override;
  void SetViewport(s32 x, s32 y, s32 width, s32 height) override;
  void SetScissor(s32 x, s32 y, s32 width, s32 height) override;
  void Draw(u32 vertex_count, u32 base_vertex) override;
  void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) override;

  void SetVSync(bool enabled) override;

  bool BeginPresent(bool skip_present) override;
  void EndPresent() override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void CommitClear(OpenGLTexture* tex);
  void CommitRTClearInFB(OpenGLTexture* tex, u32 idx);
  void CommitDSClearInFB(OpenGLTexture* tex);

  GLuint LookupProgramCache(const OpenGLPipeline::ProgramCacheKey& key, const GPUPipeline::GraphicsConfig& plconfig);
  GLuint CompileProgram(const GPUPipeline::GraphicsConfig& plconfig);
  void PostLinkProgram(const GPUPipeline::GraphicsConfig& plconfig, GLuint program_id);
  void UnrefProgram(const OpenGLPipeline::ProgramCacheKey& key);

  OpenGLPipeline::VertexArrayCache::const_iterator LookupVAOCache(const OpenGLPipeline::VertexArrayCacheKey& key);
  GLuint CreateVAO(std::span<const GPUPipeline::VertexAttribute> attributes, u32 stride);
  void UnrefVAO(const OpenGLPipeline::VertexArrayCacheKey& key);

  void SetActiveTexture(u32 slot);
  void UnbindTexture(GLuint id);
  void UnbindTexture(OpenGLTexture* tex);
  void UnbindSSBO(GLuint id);
  void UnbindSampler(GLuint id);
  void UnbindPipeline(const OpenGLPipeline* pl);

protected:
  bool CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                    std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                    Error* error) override;
  void DestroyDevice() override;

  bool ReadPipelineCache(const std::string& filename) override;
  bool GetPipelineCacheData(DynamicHeapArray<u8>* data) override;

private:
  static constexpr u8 NUM_TIMESTAMP_QUERIES = 3;

  static constexpr GLenum UPDATE_TEXTURE_UNIT = GL_TEXTURE8;

  static constexpr u32 VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;
  static constexpr u32 INDEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024;
  static constexpr u32 TEXTURE_STREAM_BUFFER_SIZE = 16 * 1024 * 1024;

  bool CheckFeatures(bool* buggy_pbo, FeatureMask disabled_features);
  bool CreateBuffers(bool buggy_pbo);
  void DestroyBuffers();

  void SetSwapInterval();
  void RenderBlankFrame();

  s32 IsRenderTargetBound(const GPUTexture* tex) const;
  static GLuint CreateFramebuffer(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags);
  static void DestroyFramebuffer(GLuint fbo);

  void UpdateViewport();
  void UpdateScissor();

  void CreateTimestampQueries();
  void DestroyTimestampQueries();
  void PopTimestampQuery();
  void KickTimestampQuery();

  GLuint CreateProgramFromPipelineCache(const OpenGLPipeline::ProgramCacheItem& it,
                                        const GPUPipeline::GraphicsConfig& plconfig);
  void AddToPipelineCache(OpenGLPipeline::ProgramCacheItem* it);
  bool DiscardPipelineCache();
  void ClosePipelineCache();

  void ApplyRasterizationState(GPUPipeline::RasterizationState rs);
  void ApplyDepthState(GPUPipeline::DepthState ds);
  void ApplyBlendState(GPUPipeline::BlendState bs);

  void SetVertexBufferOffsets(u32 base_vertex);

  std::unique_ptr<OpenGLContext> m_gl_context;

  std::unique_ptr<OpenGLStreamBuffer> m_vertex_buffer;
  std::unique_ptr<OpenGLStreamBuffer> m_index_buffer;
  std::unique_ptr<OpenGLStreamBuffer> m_uniform_buffer;
  std::unique_ptr<OpenGLStreamBuffer> m_texture_stream_buffer;

  // TODO: pass in file instead of blob for pipeline cache
  OpenGLPipeline::VertexArrayCache m_vao_cache;
  OpenGLPipeline::ProgramCache m_program_cache;
  GPUFramebufferManager<GLuint, CreateFramebuffer, DestroyFramebuffer> m_framebuffer_manager;

  // VAO cache - fixed max as key
  OpenGLPipeline::VertexArrayCache::const_iterator m_last_vao = m_vao_cache.cend();
  GPUPipeline::BlendState m_last_blend_state = {};
  GPUPipeline::RasterizationState m_last_rasterization_state = {};
  GPUPipeline::DepthState m_last_depth_state = {};
  GLuint m_uniform_buffer_alignment = 1;
  GLuint m_last_program = 0;
  u32 m_last_texture_unit = 0;
  std::array<std::pair<GLuint, GLuint>, MAX_TEXTURE_SAMPLERS> m_last_samplers = {};
  GLuint m_last_ssbo = 0;
  Common::Rectangle<s32> m_last_viewport{0, 0, 1, 1};
  Common::Rectangle<s32> m_last_scissor{0, 0, 1, 1};

  // Misc framebuffers
  GLuint m_read_fbo = 0;
  GLuint m_write_fbo = 0;

  GLuint m_current_fbo = 0;
  u32 m_num_current_render_targets = 0;
  std::array<OpenGLTexture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  OpenGLTexture* m_current_depth_target = nullptr;

  OpenGLPipeline* m_current_pipeline = nullptr;

  std::array<GLuint, NUM_TIMESTAMP_QUERIES> m_timestamp_queries = {};
  float m_accumulated_gpu_time = 0.0f;
  u8 m_read_timestamp_query = 0;
  u8 m_write_timestamp_query = 0;
  u8 m_waiting_timestamp_queries = 0;
  bool m_timestamp_query_started = false;

  std::FILE* m_pipeline_disk_cache_file = nullptr;
  std::string m_pipeline_disk_cache_filename;
  u32 m_pipeline_disk_cache_data_end = 0;
  bool m_pipeline_disk_cache_changed = false;
};
