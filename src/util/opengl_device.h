// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"
#include "gpu_framebuffer_manager.h"
#include "gpu_shader_cache.h"
#include "opengl_context.h"
#include "opengl_loader.h"
#include "opengl_pipeline.h"
#include "opengl_texture.h"

#include "common/file_system.h"

#include <memory>
#include <string_view>
#include <tuple>

class OpenGLPipeline;
class OpenGLStreamBuffer;
class OpenGLTexture;
class OpenGLDownloadTexture;

class OpenGLDevice final : public GPUDevice
{
  friend OpenGLTexture;
  friend OpenGLDownloadTexture;

public:
  OpenGLDevice();
  ~OpenGLDevice();

  ALWAYS_INLINE static OpenGLDevice& GetInstance() { return *static_cast<OpenGLDevice*>(g_gpu_device.get()); }
  ALWAYS_INLINE static OpenGLStreamBuffer* GetTextureStreamBuffer()
  {
    return GetInstance().m_texture_stream_buffer.get();
  }
  ALWAYS_INLINE static bool IsGLES() { return GetInstance().m_gl_context->IsGLES(); }
  ALWAYS_INLINE static OpenGLContext* GetContext() { return GetInstance().m_gl_context.get(); }
  static void BindUpdateTextureUnit();
  static bool ShouldUsePBOsForDownloads();
  static void SetErrorObject(Error* errptr, std::string_view prefix, GLenum glerr);

  std::string GetDriverInfo() const override;

  void FlushCommands() override;
  void WaitForGPUIdle() override;

  std::unique_ptr<GPUSwapChain> CreateSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode,
                                                bool allow_present_throttle,
                                                const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                                std::optional<bool> exclusive_fullscreen_control,
                                                Error* error) override;
  bool SwitchToSurfacelessRendering(Error* error) override;
  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Type type, GPUTexture::Format format, GPUTexture::Flags flags,
                                            const void* data = nullptr, u32 data_stride = 0,
                                            Error* error = nullptr) override;
  std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config, Error* error = nullptr) override;
  std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements,
                                                        Error* error = nullptr) override;

  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                            Error* error = nullptr) override;
  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                            void* memory, size_t memory_size, u32 memory_stride,
                                                            Error* error = nullptr) override;

  bool SupportsTextureFormat(GPUTexture::Format format) const override;
  void CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                         u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width, u32 height) override;
  void ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level, GPUTexture* src,
                            u32 src_x, u32 src_y, u32 width, u32 height) override;
  void ClearRenderTarget(GPUTexture* t, u32 c) override;
  void ClearDepth(GPUTexture* t, float d) override;
  void InvalidateRenderTarget(GPUTexture* t) override;

  std::unique_ptr<GPUShader> CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                    Error* error) override;
  std::unique_ptr<GPUShader> CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
                                                    std::string_view source, const char* entry_point,
                                                    DynamicHeapArray<u8>* out_binary, Error* error) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::GraphicsConfig& config, Error* error) override;
  std::unique_ptr<GPUPipeline> CreatePipeline(const GPUPipeline::ComputeConfig& config, Error* error) override;

#ifdef ENABLE_GPU_OBJECT_NAMES
  void PushDebugGroup(const char* name) override;
  void PopDebugGroup() override;
  void InsertDebugMessage(const char* msg) override;
#endif

  void MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                       u32* map_base_vertex) override;
  void UnmapVertexBuffer(u32 vertex_size, u32 vertex_count) override;
  void MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index) override;
  void UnmapIndexBuffer(u32 used_index_count) override;
  void* MapUniformBuffer(u32 size) override;
  void UnmapUniformBuffer(u32 size) override;
  void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                        GPUPipeline::RenderPassFlag feedback_loop = GPUPipeline::NoRenderPassFlags) override;
  void SetPipeline(GPUPipeline* pipeline) override;
  void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) override;
  void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) override;
  void SetViewport(const GSVector4i rc) override;
  void SetScissor(const GSVector4i rc) override;
  void Draw(u32 vertex_count, u32 base_vertex) override;
  void DrawWithPushConstants(u32 vertex_count, u32 base_vertex, const void* push_constants,
                             u32 push_constants_size) override;
  void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) override;
  void DrawIndexedWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex, const void* push_constants,
                                    u32 push_constants_size) override;
  void Dispatch(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x, u32 group_size_y,
                u32 group_size_z) override;
  void DispatchWithPushConstants(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x, u32 group_size_y,
                                 u32 group_size_z, const void* push_constants, u32 push_constants_size) override;

  PresentResult BeginPresent(GPUSwapChain* swap_chain, u32 clear_color) override;
  void EndPresent(GPUSwapChain* swap_chain, bool explicit_present, u64 present_time) override;
  void SubmitPresent(GPUSwapChain* swap_chain) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void CommitClear(OpenGLTexture* tex);
  void CommitRTClearInFB(OpenGLTexture* tex, u32 idx);
  void CommitDSClearInFB(OpenGLTexture* tex);

  GLuint LookupProgramCache(const OpenGLPipeline::ProgramCacheKey& key, const GPUPipeline::GraphicsConfig& plconfig,
                            Error* error);
  GLuint CompileProgram(const GPUPipeline::GraphicsConfig& plconfig, Error* error);
  void PostLinkProgram(const GPUPipeline::GraphicsConfig& plconfig, GLuint program_id);
  void UnrefProgram(const OpenGLPipeline::ProgramCacheKey& key);

  OpenGLPipeline::VertexArrayCache::const_iterator LookupVAOCache(const OpenGLPipeline::VertexArrayCacheKey& key,
                                                                  Error* error);
  GLuint CreateVAO(std::span<const GPUPipeline::VertexAttribute> attributes, u32 stride, Error* error);
  void UnrefVAO(const OpenGLPipeline::VertexArrayCacheKey& key);

  void SetActiveTexture(u32 slot);
  void UnbindTexture(GLuint id);
  void UnbindTexture(OpenGLTexture* tex);
  void UnbindSSBO(GLuint id);
  void UnbindSampler(GLuint id);
  void UnbindPipeline(const OpenGLPipeline* pl);

  void RenderBlankFrame();

protected:
  bool CreateDeviceAndMainSwapChain(std::string_view adapter, CreateFlags create_flags, const WindowInfo& wi,
                                    GPUVSyncMode vsync_mode, bool allow_present_throttle,
                                    const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                    std::optional<bool> exclusive_fullscreen_control, Error* error) override;
  void DestroyDevice() override;

  bool OpenPipelineCache(const std::string& path, Error* error) override;
  bool CreatePipelineCache(const std::string& path, Error* error) override;
  bool ClosePipelineCache(const std::string& path, Error* error) override;

private:
  static constexpr u8 NUM_TIMESTAMP_QUERIES = 3;

  static constexpr GLenum UPDATE_TEXTURE_UNIT = GL_TEXTURE8;

  static constexpr u32 VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;
  static constexpr u32 INDEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024;
  static constexpr u32 TEXTURE_STREAM_BUFFER_SIZE = 16 * 1024 * 1024;

  bool CheckFeatures(CreateFlags create_flags);
  bool CreateBuffers();
  void DestroyBuffers();

  s32 IsRenderTargetBound(const GPUTexture* tex) const;
  static GLuint CreateFramebuffer(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds, u32 flags);
  static void DestroyFramebuffer(GLuint fbo);

  void PushUniformBuffer(const void* data, u32 data_size);
  void UpdateViewport();
  void UpdateScissor();

  void CreateTimestampQueries();
  void DestroyTimestampQueries();
  void PopTimestampQuery();
  void StartTimestampQuery();
  void EndTimestampQuery();

  GLuint CreateProgramFromPipelineCache(const OpenGLPipeline::ProgramCacheItem& it,
                                        const GPUPipeline::GraphicsConfig& plconfig);
  void AddToPipelineCache(OpenGLPipeline::ProgramCacheItem* it);
  bool DiscardPipelineCache();

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
  GSVector4i m_last_viewport = {};
  GSVector4i m_last_scissor = GSVector4i::cxpr(0, 0, 1, 1);

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
#ifdef HAS_POSIX_FILE_LOCK
  FileSystem::POSIXLock m_pipeline_disk_cache_file_lock;
#endif
  u32 m_pipeline_disk_cache_data_end = 0;
  bool m_pipeline_disk_cache_changed = false;

  bool m_disable_pbo = false;
  bool m_disable_async_download = false;
};

class OpenGLSwapChain : public GPUSwapChain
{
public:
  OpenGLSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode, bool allow_present_throttle,
                  OpenGLContext::SurfaceHandle surface_handle);
  ~OpenGLSwapChain() override;

  ALWAYS_INLINE OpenGLContext::SurfaceHandle GetSurfaceHandle() const { return m_surface_handle; }

  bool ResizeBuffers(u32 new_width, u32 new_height, float new_scale, Error* error) override;
  bool SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle, Error* error) override;

  static bool SetSwapInterval(OpenGLContext* ctx, GPUVSyncMode mode, Error* error);

private:
  OpenGLContext::SurfaceHandle m_surface_handle;
};
