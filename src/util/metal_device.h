// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

// Macro hell. These have to come first.
#include <AppKit/AppKit.h>
#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>

#ifndef __OBJC__
#error This file needs to be compiled with Objective C++.
#endif

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

#include "gpu_device.h"
#include "metal_stream_buffer.h"
#include "window_info.h"

#include "common/rectangle.h"
#include "common/timer.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class MetalDevice;
class MetalPipeline;
class MetalTexture;

class MetalSampler final : public GPUSampler
{
  friend MetalDevice;

public:
  ~MetalSampler() override;

  ALWAYS_INLINE id<MTLSamplerState> GetSamplerState() const { return m_ss; }

  void SetDebugName(const std::string_view& name) override;

private:
  MetalSampler(id<MTLSamplerState> ss);

  id<MTLSamplerState> m_ss;
};

class MetalShader final : public GPUShader
{
  friend MetalDevice;

public:
  ~MetalShader() override;

  ALWAYS_INLINE id<MTLLibrary> GetLibrary() const { return m_library; }
  ALWAYS_INLINE id<MTLFunction> GetFunction() const { return m_function; }

  void SetDebugName(const std::string_view& name) override;

private:
  MetalShader(GPUShaderStage stage, id<MTLLibrary> library, id<MTLFunction> function);

  id<MTLLibrary> m_library;
  id<MTLFunction> m_function;
};

class MetalPipeline final : public GPUPipeline
{
  friend MetalDevice;

public:
  ~MetalPipeline() override;

  ALWAYS_INLINE id<MTLRenderPipelineState> GetPipelineState() const { return m_pipeline; }
  ALWAYS_INLINE id<MTLDepthStencilState> GetDepthState() const { return m_depth; }
  ALWAYS_INLINE MTLCullMode GetCullMode() const { return m_cull_mode; }
  ALWAYS_INLINE MTLPrimitiveType GetPrimitive() const { return m_primitive; }

  void SetDebugName(const std::string_view& name) override;

private:
  MetalPipeline(id<MTLRenderPipelineState> pipeline, id<MTLDepthStencilState> depth, MTLCullMode cull_mode,
                MTLPrimitiveType primitive);

  id<MTLRenderPipelineState> m_pipeline;
  id<MTLDepthStencilState> m_depth;
  MTLCullMode m_cull_mode;
  MTLPrimitiveType m_primitive;
};

class MetalTexture final : public GPUTexture
{
  friend MetalDevice;

public:
  ~MetalTexture();

  ALWAYS_INLINE id<MTLTexture> GetMTLTexture() const { return m_texture; }

  bool Create(id<MTLDevice> device, u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type,
              Format format, const void* initial_data = nullptr, u32 initial_data_stride = 0);

  bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0, u32 level = 0) override;
  bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) override;
  void Unmap() override;

  void MakeReadyForSampling() override;

  void SetDebugName(const std::string_view& name) override;

  // Call when the texture is bound to the pipeline, or read from in a copy.
  ALWAYS_INLINE void SetUseFenceCounter(u64 counter) { m_use_fence_counter = counter; }

private:
  MetalTexture(id<MTLTexture> texture, u16 width, u16 height, u8 layers, u8 levels, u8 samples, Type type,
               Format format);

  id<MTLTexture> m_texture;

  // Contains the fence counter when the texture was last used.
  // When this matches the current fence counter, the texture was used this command buffer.
  u64 m_use_fence_counter = 0;

  u16 m_map_x = 0;
  u16 m_map_y = 0;
  u16 m_map_width = 0;
  u16 m_map_height = 0;
  u8 m_map_layer = 0;
  u8 m_map_level = 0;
};

class MetalDownloadTexture final : public GPUDownloadTexture
{
public:
  ~MetalDownloadTexture() override;

  static std::unique_ptr<MetalDownloadTexture> Create(u32 width, u32 height, GPUTexture::Format format, void* memory,
                                                      size_t memory_size, u32 memory_stride);

  void CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height,
                       u32 src_layer, u32 src_level, bool use_transfer_pitch) override;

  bool Map(u32 x, u32 y, u32 width, u32 height) override;
  void Unmap() override;

  void Flush() override;

  void SetDebugName(std::string_view name) override;

private:
  MetalDownloadTexture(u32 width, u32 height, GPUTexture::Format format, u8* import_buffer, size_t buffer_offset,
                       id<MTLBuffer> buffer, const u8* map_ptr, u32 map_pitch);

  size_t m_buffer_offset = 0;
  id<MTLBuffer> m_buffer = nil;

  u64 m_copy_fence_counter = 0;
};

class MetalTextureBuffer final : public GPUTextureBuffer
{
public:
  MetalTextureBuffer(Format format, u32 size_in_elements);
  ~MetalTextureBuffer() override;

  ALWAYS_INLINE id<MTLBuffer> GetMTLBuffer() const { return m_buffer.GetBuffer(); }

  bool CreateBuffer(id<MTLDevice> device);

  // Inherited via GPUTextureBuffer
  void* Map(u32 required_elements) override;
  void Unmap(u32 used_elements) override;

  void SetDebugName(const std::string_view& name) override;

private:
  MetalStreamBuffer m_buffer;
};

class MetalDevice final : public GPUDevice
{
  friend MetalTexture;
  friend MetalDownloadTexture;

public:
  ALWAYS_INLINE static MetalDevice& GetInstance() { return *static_cast<MetalDevice*>(g_gpu_device.get()); }
  ALWAYS_INLINE id<MTLDevice> GetMTLDevice() { return m_device; }
  ALWAYS_INLINE u64 GetCurrentFenceCounter() { return m_current_fence_counter; }
  ALWAYS_INLINE u64 GetCompletedFenceCounter() { return m_completed_fence_counter; }

  MetalDevice();
  ~MetalDevice();

  RenderAPI GetRenderAPI() const override;

  bool HasSurface() const override;

  bool UpdateWindow() override;
  void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) override;

  AdapterAndModeList GetAdapterAndModeList() override;
  void DestroySurface() override;

  std::string GetDriverInfo() const override;

  std::unique_ptr<GPUTexture> CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                            GPUTexture::Type type, GPUTexture::Format format,
                                            const void* data = nullptr, u32 data_stride = 0) override;
  std::unique_ptr<GPUSampler> CreateSampler(const GPUSampler::Config& config) override;
  std::unique_ptr<GPUTextureBuffer> CreateTextureBuffer(GPUTextureBuffer::Format format, u32 size_in_elements) override;

  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format) override;
  std::unique_ptr<GPUDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GPUTexture::Format format,
                                                            void* memory, size_t memory_size,
                                                            u32 memory_stride) override;

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
                                                    const char* entry_point,
                                                    DynamicHeapArray<u8>* out_binary = nullptr) override;
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
  void SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                        GPUPipeline::RenderPassFlag feedback_loop) override;
  void SetPipeline(GPUPipeline* pipeline) override;
  void SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler) override;
  void SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer) override;
  void SetViewport(s32 x, s32 y, s32 width, s32 height) override;
  void SetScissor(s32 x, s32 y, s32 width, s32 height) override;
  void Draw(u32 vertex_count, u32 base_vertex) override;
  void DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex) override;
  void DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type) override;

  bool GetHostRefreshRate(float* refresh_rate) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void SetVSyncEnabled(bool enabled) override;

  bool BeginPresent(bool skip_present) override;
  void EndPresent(bool explicit_submit) override;
  void SubmitPresent() override;

  void WaitForFenceCounter(u64 counter);

  ALWAYS_INLINE MetalStreamBuffer& GetTextureStreamBuffer() { return m_texture_upload_buffer; }
  id<MTLBlitCommandEncoder> GetBlitEncoder(bool is_inline);

  void SubmitCommandBuffer(bool wait_for_completion = false);
  void SubmitCommandBufferAndRestartRenderPass(const char* reason);

  void CommitClear(MetalTexture* tex);

  void UnbindPipeline(MetalPipeline* pl);
  void UnbindTexture(MetalTexture* tex);
  void UnbindTextureBuffer(MetalTextureBuffer* buf);

  static void DeferRelease(id obj);
  static void DeferRelease(u64 fence_counter, id obj);

  static AdapterAndModeList StaticGetAdapterAndModeList();

protected:
  bool CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                    std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                    Error* error) override;
  void DestroyDevice() override;

private:
  static constexpr u32 VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;
  static constexpr u32 INDEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_ALIGNMENT = 256;
  static constexpr u32 TEXTURE_STREAM_BUFFER_SIZE = 32 /*16*/ * 1024 * 1024; // TODO reduce after separate allocations
  static constexpr u8 NUM_TIMESTAMP_QUERIES = 3;

  using DepthStateMap = std::unordered_map<u8, id<MTLDepthStencilState>>;

  struct ClearPipelineConfig
  {
    GPUTexture::Format color_formats[MAX_RENDER_TARGETS];
    GPUTexture::Format depth_format;
    u8 samples;
    u8 pad[2];

    bool operator==(const ClearPipelineConfig& c) const { return (std::memcmp(this, &c, sizeof(*this)) == 0); }
    bool operator!=(const ClearPipelineConfig& c) const { return (std::memcmp(this, &c, sizeof(*this)) != 0); }
    bool operator<(const ClearPipelineConfig& c) const { return (std::memcmp(this, &c, sizeof(*this)) < 0); }
  };
  static_assert(sizeof(ClearPipelineConfig) == 8);

  ALWAYS_INLINE NSView* GetWindowView() const { return (__bridge NSView*)m_window_info.window_handle; }

  void SetFeatures(FeatureMask disabled_features);
  bool LoadShaders();

  id<MTLFunction> GetFunctionFromLibrary(id<MTLLibrary> library, NSString* name);
  id<MTLComputePipelineState> CreateComputePipeline(id<MTLFunction> function, NSString* name);
  ClearPipelineConfig GetCurrentClearPipelineConfig() const;
  id<MTLRenderPipelineState> GetClearDepthPipeline(const ClearPipelineConfig& config);

  std::unique_ptr<GPUShader> CreateShaderFromMSL(GPUShaderStage stage, const std::string_view& source,
                                                 const std::string_view& entry_point);

  id<MTLDepthStencilState> GetDepthState(const GPUPipeline::DepthState& ds);

  void CreateCommandBuffer();
  void CommandBufferCompletedOffThread(id<MTLCommandBuffer> buffer, u64 fence_counter);
  void WaitForPreviousCommandBuffers();
  void CleanupObjects();

  ALWAYS_INLINE bool InRenderPass() const { return (m_render_encoder != nil); }
  ALWAYS_INLINE bool IsInlineUploading() const { return (m_inline_upload_encoder != nil); }
  void BeginRenderPass();
  void EndRenderPass();
  void EndInlineUploading();
  void EndAnyEncoding();

  Common::Rectangle<s32> ClampToFramebufferSize(const Common::Rectangle<s32>& rc) const;
  void PreDrawCheck();
  void SetInitialEncoderState();
  void SetViewportInRenderEncoder();
  void SetScissorInRenderEncoder();

  bool CreateLayer();
  void DestroyLayer();
  void RenderBlankFrame();

  bool CreateBuffers();
  void DestroyBuffers();

  bool IsRenderTargetBound(const GPUTexture* tex) const;

  id<MTLDevice> m_device;
  id<MTLCommandQueue> m_queue;

  CAMetalLayer* m_layer = nil;
  id<MTLDrawable> m_layer_drawable = nil;
  MTLRenderPassDescriptor* m_layer_pass_desc = nil;

  std::mutex m_fence_mutex;
  u64 m_current_fence_counter = 0;
  std::atomic<u64> m_completed_fence_counter{0};
  std::deque<std::pair<u64, id>> m_cleanup_objects; // [fence_counter, object]

  DepthStateMap m_depth_states;

  MetalStreamBuffer m_vertex_buffer;
  MetalStreamBuffer m_index_buffer;
  MetalStreamBuffer m_uniform_buffer;
  MetalStreamBuffer m_texture_upload_buffer;

  id<MTLLibrary> m_shaders = nil;
  std::vector<std::pair<std::pair<GPUTexture::Format, GPUTexture::Format>, id<MTLComputePipelineState>>>
    m_resolve_pipelines;
  std::vector<std::pair<ClearPipelineConfig, id<MTLRenderPipelineState>>> m_clear_pipelines;

  id<MTLCommandBuffer> m_upload_cmdbuf = nil;
  id<MTLBlitCommandEncoder> m_upload_encoder = nil;
  id<MTLBlitCommandEncoder> m_inline_upload_encoder = nil;

  id<MTLCommandBuffer> m_render_cmdbuf = nil;
  id<MTLRenderCommandEncoder> m_render_encoder = nil;

  u8 m_num_current_render_targets = 0;
  GPUPipeline::RenderPassFlag m_current_feedback_loop = GPUPipeline::NoRenderPassFlags;
  std::array<MetalTexture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  MetalTexture* m_current_depth_target = nullptr;

  MetalPipeline* m_current_pipeline = nullptr;
  id<MTLDepthStencilState> m_current_depth_state = nil;
  MTLCullMode m_current_cull_mode = MTLCullModeNone;
  u32 m_current_uniform_buffer_position = 0;

  std::array<id<MTLTexture>, MAX_TEXTURE_SAMPLERS> m_current_textures = {};
  std::array<id<MTLSamplerState>, MAX_TEXTURE_SAMPLERS> m_current_samplers = {};
  id<MTLBuffer> m_current_ssbo = nil;
  Common::Rectangle<s32> m_current_viewport = {};
  Common::Rectangle<s32> m_current_scissor = {};

  bool m_vsync_enabled = false;

  double m_accumulated_gpu_time = 0;
  double m_last_gpu_time_end = 0;
};
