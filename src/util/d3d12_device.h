// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "d3d12_descriptor_heap_manager.h"
#include "d3d12_stream_buffer.h"
#include "gpu_device.h"
#include "gpu_texture.h"

#include "common/windows_headers.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <d3d12.h>
#include <deque>
#include <dxgi1_5.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

class D3D12Framebuffer;
class D3D12Pipeline;
class D3D12SwapChain;
class D3D12Texture;
class D3D12TextureBuffer;

namespace D3D12MA {
class Allocator;
}

class D3D12Device final : public GPUDevice
{
public:
  friend D3D12Texture;

  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  enum : u32
  {
    NUM_COMMAND_LISTS = 3,

    /// Start/End timestamp queries.
    NUM_TIMESTAMP_QUERIES_PER_CMDLIST = 2,
  };

public:
  D3D12Device();
  ~D3D12Device() override;

  RenderAPI GetRenderAPI() const override;

  bool HasSurface() const override;

  bool UpdateWindow() override;
  void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) override;

  static AdapterAndModeList StaticGetAdapterAndModeList();
  AdapterAndModeList GetAdapterAndModeList() override;
  void DestroySurface() override;

  std::string GetDriverInfo() const override;

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

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void SetVSync(bool enabled) override;

  bool BeginPresent(bool skip_present) override;
  void EndPresent() override;

  // Global state accessors
  ALWAYS_INLINE static D3D12Device& GetInstance() { return *static_cast<D3D12Device*>(g_gpu_device.get()); }
  ALWAYS_INLINE IDXGIAdapter1* GetAdapter() const { return m_adapter.Get(); }
  ALWAYS_INLINE ID3D12Device1* GetDevice() const { return m_device.Get(); }
  ALWAYS_INLINE ID3D12CommandQueue* GetCommandQueue() const { return m_command_queue.Get(); }
  ALWAYS_INLINE D3D12MA::Allocator* GetAllocator() const { return m_allocator.Get(); }

  void WaitForGPUIdle();

  // Descriptor manager access.
  D3D12DescriptorHeapManager& GetDescriptorHeapManager() { return m_descriptor_heap_manager; }
  D3D12DescriptorHeapManager& GetRTVHeapManager() { return m_rtv_heap_manager; }
  D3D12DescriptorHeapManager& GetDSVHeapManager() { return m_dsv_heap_manager; }
  D3D12DescriptorHeapManager& GetSamplerHeapManager() { return m_sampler_heap_manager; }
  const D3D12DescriptorHandle& GetNullSRVDescriptor() const { return m_null_srv_descriptor; }

  // These command buffers are allocated per-frame. They are valid until the command buffer
  // is submitted, after that you should call these functions again.
  ALWAYS_INLINE ID3D12GraphicsCommandList4* GetCommandList() const
  {
    return m_command_lists[m_current_command_list].command_lists[1].Get();
  }
  ALWAYS_INLINE D3D12StreamBuffer& GetTextureUploadBuffer() { return m_texture_upload_buffer; }
  ID3D12GraphicsCommandList4* GetInitCommandList();

  // Root signature access.
  ComPtr<ID3DBlob> SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc);
  ComPtr<ID3D12RootSignature> CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc);

  /// Fence value for current command list.
  u64 GetCurrentFenceValue() const { return m_current_fence_value; }

  /// Last "completed" fence.
  u64 GetCompletedFenceValue() const { return m_completed_fence_value; }

  // Schedule a d3d12 resource for destruction later on. This will occur when the command buffer
  // is next re-used, and the GPU has finished working with the specified resource.
  void DeferObjectDestruction(ComPtr<ID3D12Object> resource);
  void DeferResourceDestruction(ComPtr<D3D12MA::Allocation> allocation, ComPtr<ID3D12Resource> resource);
  void DeferDescriptorDestruction(D3D12DescriptorHeapManager& heap, D3D12DescriptorHandle* descriptor);

  // Wait for a fence to be completed.
  // Also invokes callbacks for completion.
  void WaitForFence(u64 fence_counter);

  /// Ends any render pass, executes the command buffer, and invalidates cached state.
  void SubmitCommandList(bool wait_for_completion);
  void SubmitCommandList(bool wait_for_completion, const char* reason, ...);
  void SubmitCommandListAndRestartRenderPass(const char* reason);

  void UnbindPipeline(D3D12Pipeline* pl);
  void UnbindTexture(D3D12Texture* tex);
  void UnbindTextureBuffer(D3D12TextureBuffer* buf);

protected:
  bool CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                    std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                    Error* error) override;
  void DestroyDevice() override;

  bool ReadPipelineCache(const std::string& filename) override;
  bool GetPipelineCacheData(DynamicHeapArray<u8>* data) override;

private:
  enum DIRTY_FLAG : u32
  {
    DIRTY_FLAG_INITIAL = (1 << 0),
    DIRTY_FLAG_PIPELINE_LAYOUT = (1 << 1),
    DIRTY_FLAG_CONSTANT_BUFFER = (1 << 2),
    DIRTY_FLAG_TEXTURES = (1 << 3),
    DIRTY_FLAG_SAMPLERS = (1 << 3),

    ALL_DIRTY_STATE = DIRTY_FLAG_INITIAL | DIRTY_FLAG_PIPELINE_LAYOUT | DIRTY_FLAG_CONSTANT_BUFFER |
                      DIRTY_FLAG_TEXTURES | DIRTY_FLAG_SAMPLERS,
  };

  struct CommandList
  {
    // [0] - Init (upload) command buffer, [1] - draw command buffer
    std::array<ComPtr<ID3D12CommandAllocator>, 2> command_allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList4>, 2> command_lists;
    D3D12DescriptorAllocator descriptor_allocator;
    D3D12GroupedSamplerAllocator<MAX_TEXTURE_SAMPLERS> sampler_allocator;
    u64 fence_counter = 0;
    bool init_list_used = false;
    bool needs_fence_wait = false;
    bool has_timestamp_query = false;
  };

  using SamplerMap = std::unordered_map<u64, D3D12DescriptorHandle>;

  static void GetAdapterAndModeList(AdapterAndModeList* ret, IDXGIFactory5* factory);

  void SetFeatures(FeatureMask disabled_features);

  bool CreateSwapChain();
  bool CreateSwapChainRTV();
  void DestroySwapChainRTVs();
  void DestroySwapChain();

  bool CreateCommandLists();
  void DestroyCommandLists();
  bool CreateRootSignatures();
  void DestroyRootSignatures();
  bool CreateBuffers();
  void DestroyBuffers();
  bool CreateDescriptorHeaps();
  void DestroyDescriptorHeaps();
  bool CreateTimestampQuery();
  void DestroyTimestampQuery();
  D3D12DescriptorHandle GetSampler(const GPUSampler::Config& config);
  void DestroySamplers();
  void DestroyDeferredObjects(u64 fence_value);

  void RenderBlankFrame();
  void MoveToNextCommandList();

  bool CreateSRVDescriptor(ID3D12Resource* resource, u32 layers, u32 levels, u32 samples, DXGI_FORMAT format,
                           D3D12DescriptorHandle* dh);
  bool CreateRTVDescriptor(ID3D12Resource* resource, u32 samples, DXGI_FORMAT format, D3D12DescriptorHandle* dh);
  bool CreateDSVDescriptor(ID3D12Resource* resource, u32 samples, DXGI_FORMAT format, D3D12DescriptorHandle* dh);
  bool CreateUAVDescriptor(ID3D12Resource* resource, u32 samples, DXGI_FORMAT format, D3D12DescriptorHandle* dh);

  bool IsRenderTargetBound(const GPUTexture* tex) const;

  bool CheckDownloadBufferSize(u32 required_size);
  void DestroyDownloadBuffer();

  /// Set dirty flags on everything to force re-bind at next draw time.
  void InvalidateCachedState();
  void SetVertexBuffer(ID3D12GraphicsCommandList4* cmdlist);
  void SetViewport(ID3D12GraphicsCommandList4* cmdlist);
  void SetScissor(ID3D12GraphicsCommandList4* cmdlist);

  /// Applies any changed state.
  ID3D12RootSignature* GetCurrentRootSignature() const;
  void SetInitialPipelineState();
  void PreDrawCheck();

  void UpdateRootSignature();
  template<GPUPipeline::Layout layout>
  bool UpdateParametersForLayout(u32 dirty);
  bool UpdateRootParameters(u32 dirty);

  // Ends a render pass if we're currently in one.
  // When Bind() is next called, the pass will be restarted.
  void BeginRenderPass();
  void BeginSwapChainRenderPass();
  void EndRenderPass();
  bool InRenderPass();

  ComPtr<IDXGIAdapter1> m_adapter;
  ComPtr<ID3D12Device1> m_device;
  ComPtr<ID3D12CommandQueue> m_command_queue;
  ComPtr<D3D12MA::Allocator> m_allocator;

  ComPtr<ID3D12Fence> m_fence;
  HANDLE m_fence_event = {};
  u64 m_current_fence_value = 0;
  u64 m_completed_fence_value = 0;

  std::array<CommandList, NUM_COMMAND_LISTS> m_command_lists;
  u32 m_current_command_list = NUM_COMMAND_LISTS - 1;
  D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;

  ComPtr<IDXGIFactory5> m_dxgi_factory;
  ComPtr<IDXGISwapChain1> m_swap_chain;
  std::vector<std::pair<ComPtr<ID3D12Resource>, D3D12DescriptorHandle>> m_swap_chain_buffers;
  u32 m_current_swap_chain_buffer = 0;
  bool m_allow_tearing_supported = false;
  bool m_using_allow_tearing = false;
  bool m_is_exclusive_fullscreen = false;

  D3D12DescriptorHeapManager m_descriptor_heap_manager;
  D3D12DescriptorHeapManager m_rtv_heap_manager;
  D3D12DescriptorHeapManager m_dsv_heap_manager;
  D3D12DescriptorHeapManager m_sampler_heap_manager;
  D3D12DescriptorHandle m_null_srv_descriptor;
  D3D12DescriptorHandle m_point_sampler;

  ComPtr<ID3D12QueryHeap> m_timestamp_query_heap;
  ComPtr<ID3D12Resource> m_timestamp_query_buffer;
  ComPtr<D3D12MA::Allocation> m_timestamp_query_allocation;
  double m_timestamp_frequency = 0.0;
  float m_accumulated_gpu_time = 0.0f;

  std::deque<std::pair<u64, std::pair<D3D12MA::Allocation*, ID3D12Object*>>> m_cleanup_resources;
  std::deque<std::pair<u64, std::pair<D3D12DescriptorHeapManager*, D3D12DescriptorHandle>>> m_cleanup_descriptors;

  std::array<ComPtr<ID3D12RootSignature>, static_cast<u8>(GPUPipeline::Layout::MaxCount)> m_root_signatures = {};

  D3D12StreamBuffer m_vertex_buffer;
  D3D12StreamBuffer m_index_buffer;
  D3D12StreamBuffer m_uniform_buffer;
  D3D12StreamBuffer m_texture_upload_buffer;

  u32 m_uniform_buffer_position = 0;
  bool m_in_render_pass = false;

  SamplerMap m_sampler_map;
  ComPtr<ID3D12PipelineLibrary> m_pipeline_library;

  ComPtr<D3D12MA::Allocation> m_download_buffer_allocation;
  ComPtr<ID3D12Resource> m_download_buffer;
  u32 m_download_buffer_size = 0;

  // Which bindings/state has to be updated before the next draw.
  u32 m_dirty_flags = ALL_DIRTY_STATE;

  D3D12Pipeline* m_current_pipeline = nullptr;
  D3D12_PRIMITIVE_TOPOLOGY m_current_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  u32 m_num_current_render_targets = 0;
  std::array<D3D12Texture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  D3D12Texture* m_current_depth_target = nullptr;
  u32 m_current_vertex_stride = 0;
  u32 m_current_blend_constant = 0;
  GPUPipeline::Layout m_current_pipeline_layout = GPUPipeline::Layout::SingleTextureAndPushConstants;

  std::array<D3D12Texture*, MAX_TEXTURE_SAMPLERS> m_current_textures = {};
  std::array<D3D12DescriptorHandle, MAX_TEXTURE_SAMPLERS> m_current_samplers = {};
  D3D12TextureBuffer* m_current_texture_buffer = nullptr;
  Common::Rectangle<s32> m_current_viewport{0, 0, 1, 1};
  Common::Rectangle<s32> m_current_scissor{0, 0, 1, 1};
};
