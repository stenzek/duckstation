#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <array>
#include <d3d12.h>
#include <wrl/client.h>

namespace D3D12 {

class ShaderCache;

static inline void ResourceBarrier(ID3D12GraphicsCommandList* cmdlist, ID3D12Resource* resource,
                                   D3D12_RESOURCE_STATES from_state, D3D12_RESOURCE_STATES to_state)
{
  const D3D12_RESOURCE_BARRIER barrier = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                          D3D12_RESOURCE_BARRIER_FLAG_NONE,
                                          {{resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from_state, to_state}}};
  cmdlist->ResourceBarrier(1, &barrier);
}

static inline void SetViewport(ID3D12GraphicsCommandList* cmdlist, int x, int y, int width, int height,
                               float min_depth = 0.0f, float max_depth = 1.0f)
{
  const D3D12_VIEWPORT vp{static_cast<float>(x),
                          static_cast<float>(y),
                          static_cast<float>(width),
                          static_cast<float>(height),
                          min_depth,
                          max_depth};
  cmdlist->RSSetViewports(1, &vp);
}

static inline void SetScissor(ID3D12GraphicsCommandList* cmdlist, int x, int y, int width, int height)
{
  const D3D12_RECT r{x, y, x + width, y + height};
  cmdlist->RSSetScissorRects(1, &r);
}

static inline void SetViewportAndScissor(ID3D12GraphicsCommandList* cmdlist, int x, int y, int width, int height,
                                         float min_depth = 0.0f, float max_depth = 1.0f)
{
  SetViewport(cmdlist, x, y, width, height, min_depth, max_depth);
  SetScissor(cmdlist, x, y, width, height);
}

u32 GetTexelSize(DXGI_FORMAT format);

void SetDefaultSampler(D3D12_SAMPLER_DESC* desc);

#ifdef _DEBUG

void SetObjectName(ID3D12Object* object, const char* name);
void SetObjectNameFormatted(ID3D12Object* object, const char* format, ...);

#else

static inline void SetObjectName(ID3D12Object* object, const char* name) {}
static inline void SetObjectNameFormatted(ID3D12Object* object, const char* format, ...) {}

#endif

class RootSignatureBuilder
{
public:
  enum : u32
  {
    MAX_PARAMETERS = 16,
    MAX_DESCRIPTOR_RANGES = 16
  };

  RootSignatureBuilder();

  void Clear();

  Microsoft::WRL::ComPtr<ID3D12RootSignature> Create(bool clear = true);

  void SetInputAssemblerFlag();

  u32 Add32BitConstants(u32 shader_reg, u32 num_values, D3D12_SHADER_VISIBILITY visibility);
  u32 AddCBVParameter(u32 shader_reg, D3D12_SHADER_VISIBILITY visibility);
  u32 AddSRVParameter(u32 shader_reg, D3D12_SHADER_VISIBILITY visibility);
  u32 AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE rt, u32 start_shader_reg, u32 num_shader_regs,
                         D3D12_SHADER_VISIBILITY visibility);

private:
  D3D12_ROOT_SIGNATURE_DESC m_desc{};
  std::array<D3D12_ROOT_PARAMETER, MAX_PARAMETERS> m_params{};
  std::array<D3D12_DESCRIPTOR_RANGE, MAX_DESCRIPTOR_RANGES> m_descriptor_ranges{};
  u32 m_num_descriptor_ranges = 0;
};

class GraphicsPipelineBuilder
{
public:
  enum : u32
  {
    MAX_VERTEX_ATTRIBUTES = 16,
  };

  GraphicsPipelineBuilder();

  ~GraphicsPipelineBuilder() = default;

  void Clear();

  Microsoft::WRL::ComPtr<ID3D12PipelineState> Create(ID3D12Device* device, bool clear = true);
  Microsoft::WRL::ComPtr<ID3D12PipelineState> Create(ID3D12Device* device, ShaderCache& cache, bool clear = true);

  void SetRootSignature(ID3D12RootSignature* rs);

  void SetVertexShader(const void* data, u32 data_size);
  void SetGeometryShader(const void* data, u32 data_size);
  void SetPixelShader(const void* data, u32 data_size);

  void SetVertexShader(ID3DBlob* blob);
  void SetGeometryShader(ID3DBlob* blob);
  void SetPixelShader(ID3DBlob* blob);

  void AddVertexAttribute(const char* semantic_name, u32 semantic_index, DXGI_FORMAT format, u32 buffer, u32 offset);

  void SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE type);

  void SetRasterizationState(D3D12_FILL_MODE polygon_mode, D3D12_CULL_MODE cull_mode, bool front_face_ccw);

  void SetMultisamples(u32 multisamples);

  void SetNoCullRasterizationState();

  void SetDepthState(bool depth_test, bool depth_write, D3D12_COMPARISON_FUNC compare_op);

  void SetNoDepthTestState();

  void SetBlendState(u32 rt, bool blend_enable, D3D12_BLEND src_factor, D3D12_BLEND dst_factor, D3D12_BLEND_OP op,
                     D3D12_BLEND alpha_src_factor, D3D12_BLEND alpha_dst_factor, D3D12_BLEND_OP alpha_op,
                     u8 write_mask = D3D12_COLOR_WRITE_ENABLE_ALL);

  void SetNoBlendingState();

  void ClearRenderTargets();

  void SetRenderTarget(u32 rt, DXGI_FORMAT format);

  void ClearDepthStencilFormat();

  void SetDepthStencilFormat(DXGI_FORMAT format);

private:
  D3D12_GRAPHICS_PIPELINE_STATE_DESC m_desc{};
  std::array<D3D12_INPUT_ELEMENT_DESC, MAX_VERTEX_ATTRIBUTES> m_input_elements{};
};

} // namespace D3D12