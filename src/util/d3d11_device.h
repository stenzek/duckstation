// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "d3d11_stream_buffer.h"
#include "gpu_device.h"

#include "common/windows_headers.h"

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

class D3D11Pipeline;
class D3D11Shader;
class D3D11Texture;
class D3D11TextureBuffer;

class D3D11Device final : public GPUDevice
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11Device();
  ~D3D11Device();

  ALWAYS_INLINE static D3D11Device& GetInstance() { return *static_cast<D3D11Device*>(g_gpu_device.get()); }
  ALWAYS_INLINE static ID3D11Device1* GetD3DDevice() { return GetInstance().m_device.Get(); }
  ALWAYS_INLINE static ID3D11DeviceContext1* GetD3DContext() { return GetInstance().m_context.Get(); }
  ALWAYS_INLINE static IDXGIFactory5* GetDXGIFactory() { return GetInstance().m_dxgi_factory.Get(); }
  ALWAYS_INLINE static D3D_FEATURE_LEVEL GetMaxFeatureLevel() { return GetInstance().m_max_feature_level; }

  std::string GetDriverInfo() const override;

  void FlushCommands() override;
  void WaitForGPUIdle() override;

  std::unique_ptr<GPUSwapChain> CreateSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode,
                                                bool allow_present_throttle,
                                                const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                                std::optional<bool> exclusive_fullscreen_control,
                                                Error* error) override;
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
                        GPUPipeline::RenderPassFlag flags = GPUPipeline::NoRenderPassFlags) override;
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

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  PresentResult BeginPresent(GPUSwapChain* swap_chain, u32 clear_color) override;
  void EndPresent(GPUSwapChain* swap_chain, bool explicit_present, u64 present_time) override;
  void SubmitPresent(GPUSwapChain* swap_chain) override;

  void UnbindPipeline(D3D11Pipeline* pl);
  void UnbindTexture(D3D11Texture* tex);

protected:
  bool CreateDeviceAndMainSwapChain(std::string_view adapter, CreateFlags create_flags, const WindowInfo& wi,
                                    GPUVSyncMode vsync_mode, bool allow_present_throttle,
                                    const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                    std::optional<bool> exclusive_fullscreen_control, Error* error) override;
  void DestroyDevice() override;

private:
  using BlendStateMapKey = std::pair<u64, u32>;
  struct BlendStateMapHash
  {
    size_t operator()(const BlendStateMapKey& key) const;
  };
  using RasterizationStateMap = std::unordered_map<u8, ComPtr<ID3D11RasterizerState>>;
  using DepthStateMap = std::unordered_map<u8, ComPtr<ID3D11DepthStencilState>>;
  using BlendStateMap = std::unordered_map<BlendStateMapKey, ComPtr<ID3D11BlendState>, BlendStateMapHash>;
  using InputLayoutMap =
    std::unordered_map<GPUPipeline::InputLayout, ComPtr<ID3D11InputLayout>, GPUPipeline::InputLayoutHash>;

  static constexpr u32 VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;
  static constexpr u32 INDEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 MAX_UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024;
  static constexpr u32 MIN_UNIFORM_BUFFER_SIZE = 16;
  static constexpr u32 UNIFORM_BUFFER_ALIGNMENT = 256;
  static constexpr u32 UNIFORM_BUFFER_ALIGNMENT_DISCARD = 16;
  static constexpr u32 PUSH_CONSTANT_BUFFER_SIZE = 128;
  static constexpr u8 NUM_TIMESTAMP_QUERIES = 3;

  void SetFeatures(CreateFlags create_flags);

  bool CreateBuffers(Error* error);
  void DestroyBuffers();
  void BindUniformBuffer(u32 offset, u32 size);
  void PushUniformBuffer(const void* data, u32 data_size);
  void UnbindComputePipeline();

  bool IsRenderTargetBound(const D3D11Texture* tex) const;

  ComPtr<ID3D11RasterizerState> GetRasterizationState(const GPUPipeline::RasterizationState& rs, Error* error);
  ComPtr<ID3D11DepthStencilState> GetDepthState(const GPUPipeline::DepthState& ds, Error* error);
  ComPtr<ID3D11BlendState> GetBlendState(const GPUPipeline::BlendState& bs, u32 num_rts, Error* error);
  ComPtr<ID3D11InputLayout> GetInputLayout(const GPUPipeline::InputLayout& il, const D3D11Shader* vs, Error* error);

  bool CreateTimestampQueries();
  void DestroyTimestampQueries();
  void PopTimestampQuery();
  void EndTimestampQuery();
  void StartTimestampQuery();

  ComPtr<ID3D11Device1> m_device;
  ComPtr<ID3D11DeviceContext1> m_context;
  ComPtr<ID3DUserDefinedAnnotation> m_annotation;

  ComPtr<IDXGIFactory5> m_dxgi_factory;

  RasterizationStateMap m_rasterization_states;
  DepthStateMap m_depth_states;
  BlendStateMap m_blend_states;
  InputLayoutMap m_input_layouts;

  D3D_FEATURE_LEVEL m_max_feature_level = D3D_FEATURE_LEVEL_10_0;

  D3D11StreamBuffer m_vertex_buffer;
  D3D11StreamBuffer m_index_buffer;
  D3D11StreamBuffer m_uniform_buffer;
  ComPtr<ID3D11Buffer> m_push_constant_buffer;

  D3D11Pipeline* m_current_pipeline = nullptr;
  std::array<D3D11Texture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  u32 m_num_current_render_targets = 0;
  GPUPipeline::RenderPassFlag m_current_render_pass_flags = GPUPipeline::NoRenderPassFlags;
  D3D11Texture* m_current_depth_target = nullptr;

  ID3D11InputLayout* m_current_input_layout = nullptr;
  ID3D11VertexShader* m_current_vertex_shader = nullptr;
  ID3D11GeometryShader* m_current_geometry_shader = nullptr;
  ID3D11PixelShader* m_current_pixel_shader = nullptr;
  ID3D11ComputeShader* m_current_compute_shader = nullptr;
  ID3D11RasterizerState* m_current_rasterizer_state = nullptr;
  ID3D11DepthStencilState* m_current_depth_state = nullptr;
  ID3D11BlendState* m_current_blend_state = nullptr;
  D3D_PRIMITIVE_TOPOLOGY m_current_primitive_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  u32 m_current_vertex_stride = 0;
  u32 m_current_blend_factor = 0;

  std::array<ID3D11ShaderResourceView*, MAX_TEXTURE_SAMPLERS> m_current_textures = {};
  std::array<ID3D11SamplerState*, MAX_TEXTURE_SAMPLERS> m_current_samplers = {};

  std::array<std::array<ComPtr<ID3D11Query>, 3>, NUM_TIMESTAMP_QUERIES> m_timestamp_queries = {};
  u8 m_read_timestamp_query = 0;
  u8 m_write_timestamp_query = 0;
  u8 m_waiting_timestamp_queries = 0;
  bool m_timestamp_query_started = false;
  float m_accumulated_gpu_time = 0.0f;
};

class D3D11SwapChain : public GPUSwapChain
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  friend D3D11Device;

  D3D11SwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode, bool allow_present_throttle,
                 const GPUDevice::ExclusiveFullscreenMode* fullscreen_mode);
  ~D3D11SwapChain() override;

  ALWAYS_INLINE IDXGISwapChain1* GetSwapChain() const { return m_swap_chain.Get(); }
  ALWAYS_INLINE ID3D11RenderTargetView* GetRTV() const { return m_swap_chain_rtv.Get(); }
  ALWAYS_INLINE ID3D11RenderTargetView* const* GetRTVArray() const { return m_swap_chain_rtv.GetAddressOf(); }
  ALWAYS_INLINE bool IsUsingAllowTearing() const { return m_using_allow_tearing; }

  bool ResizeBuffers(u32 new_width, u32 new_height, float new_scale, Error* error) override;
  bool SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle, Error* error) override;
  bool IsExclusiveFullscreen() const override;

private:
  static u32 GetNewBufferCount(GPUVSyncMode vsync_mode);

  bool InitializeExclusiveFullscreenMode(const GPUDevice::ExclusiveFullscreenMode* mode);

  bool CreateSwapChain(Error* error);
  bool CreateRTV(Error* error);

  void DestroySwapChain();

  ComPtr<IDXGISwapChain1> m_swap_chain;
  ComPtr<ID3D11RenderTargetView> m_swap_chain_rtv;

  ComPtr<IDXGIOutput> m_fullscreen_output;
  std::optional<DXGI_MODE_DESC> m_fullscreen_mode;

  bool m_using_flip_model_swap_chain = true;
  bool m_using_allow_tearing = false;
};

void SetD3DDebugObjectName(ID3D11DeviceChild* obj, std::string_view name);
