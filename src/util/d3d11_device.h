// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

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
  ALWAYS_INLINE static ID3D11Device* GetD3DDevice() { return GetInstance().m_device.Get(); }
  ALWAYS_INLINE static ID3D11DeviceContext1* GetD3DContext() { return GetInstance().m_context.Get(); }

  RenderAPI GetRenderAPI() const override;

  bool HasSurface() const override;

  bool UpdateWindow() override;
  void ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale) override;
  bool SupportsExclusiveFullscreen() const override;
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
                                                    const char* entry_point, DynamicHeapArray<u8>* binary) override;
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

  bool GetHostRefreshRate(float* refresh_rate) override;

  bool SetGPUTimingEnabled(bool enabled) override;
  float GetAndResetAccumulatedGPUTime() override;

  void SetVSync(bool enabled) override;

  bool BeginPresent(bool skip_present) override;
  void EndPresent() override;

  void UnbindPipeline(D3D11Pipeline* pl);
  void UnbindTexture(D3D11Texture* tex);

  static AdapterAndModeList StaticGetAdapterAndModeList();

protected:
  bool CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                    std::optional<bool> exclusive_fullscreen_control, FeatureMask disabled_features,
                    Error* error) override;
  void DestroyDevice() override;

private:
  using RasterizationStateMap = std::unordered_map<u8, ComPtr<ID3D11RasterizerState>>;
  using DepthStateMap = std::unordered_map<u8, ComPtr<ID3D11DepthStencilState>>;
  using BlendStateMap = std::unordered_map<u64, ComPtr<ID3D11BlendState>>;
  using InputLayoutMap =
    std::unordered_map<GPUPipeline::InputLayout, ComPtr<ID3D11InputLayout>, GPUPipeline::InputLayoutHash>;

  static constexpr u32 VERTEX_BUFFER_SIZE = 8 * 1024 * 1024;
  static constexpr u32 INDEX_BUFFER_SIZE = 4 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_SIZE = 2 * 1024 * 1024;
  static constexpr u32 UNIFORM_BUFFER_ALIGNMENT = 256;
  static constexpr u8 NUM_TIMESTAMP_QUERIES = 3;

  static void GetAdapterAndModeList(AdapterAndModeList* ret, IDXGIFactory5* factory);

  void SetFeatures(FeatureMask disabled_features);

  bool CheckStagingBufferSize(u32 width, u32 height, DXGI_FORMAT format);
  void DestroyStagingBuffer();

  bool CreateSwapChain();
  bool CreateSwapChainRTV();
  void DestroySwapChain();

  bool CreateBuffers();
  void DestroyBuffers();

  bool IsRenderTargetBound(const GPUTexture* tex) const;

  ComPtr<ID3D11RasterizerState> GetRasterizationState(const GPUPipeline::RasterizationState& rs);
  ComPtr<ID3D11DepthStencilState> GetDepthState(const GPUPipeline::DepthState& ds);
  ComPtr<ID3D11BlendState> GetBlendState(const GPUPipeline::BlendState& bs);
  ComPtr<ID3D11InputLayout> GetInputLayout(const GPUPipeline::InputLayout& il, const D3D11Shader* vs);

  bool CreateTimestampQueries();
  void DestroyTimestampQueries();
  void PopTimestampQuery();
  void KickTimestampQuery();

  ComPtr<ID3D11Device1> m_device;
  ComPtr<ID3D11DeviceContext1> m_context;
  ComPtr<ID3DUserDefinedAnnotation> m_annotation;

  ComPtr<IDXGIFactory5> m_dxgi_factory;
  ComPtr<IDXGISwapChain1> m_swap_chain;
  ComPtr<ID3D11RenderTargetView> m_swap_chain_rtv;

  RasterizationStateMap m_rasterization_states;
  DepthStateMap m_depth_states;
  BlendStateMap m_blend_states;
  InputLayoutMap m_input_layouts;

  ComPtr<ID3D11Texture2D> m_readback_staging_texture;
  DXGI_FORMAT m_readback_staging_texture_format = DXGI_FORMAT_UNKNOWN;
  u32 m_readback_staging_texture_width = 0;
  u32 m_readback_staging_texture_height = 0;

  bool m_allow_tearing_supported = false;
  bool m_using_flip_model_swap_chain = true;
  bool m_using_allow_tearing = false;
  bool m_is_exclusive_fullscreen = false;

  D3D11StreamBuffer m_vertex_buffer;
  D3D11StreamBuffer m_index_buffer;
  D3D11StreamBuffer m_uniform_buffer;

  D3D11Pipeline* m_current_pipeline = nullptr;
  std::array<D3D11Texture*, MAX_RENDER_TARGETS> m_current_render_targets = {};
  u32 m_num_current_render_targets = 0;
  D3D11Texture* m_current_depth_target = nullptr;

  ID3D11InputLayout* m_current_input_layout = nullptr;
  ID3D11VertexShader* m_current_vertex_shader = nullptr;
  ID3D11GeometryShader* m_current_geometry_shader = nullptr;
  ID3D11PixelShader* m_current_pixel_shader = nullptr;
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

void SetD3DDebugObjectName(ID3D11DeviceChild* obj, const std::string_view& name);
