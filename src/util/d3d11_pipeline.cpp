// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "d3d11_pipeline.h"
#include "d3d11_device.h"
#include "d3d_common.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/hash_combine.h"

#include "fmt/format.h"

#include <array>
#include <malloc.h>

D3D11Shader::D3D11Shader(GPUShaderStage stage, Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader,
                         std::vector<u8> bytecode)
  : GPUShader(stage), m_shader(std::move(shader)), m_bytecode(std::move(bytecode))
{
}

D3D11Shader::~D3D11Shader() = default;

ID3D11VertexShader* D3D11Shader::GetVertexShader() const
{
  DebugAssert(m_stage == GPUShaderStage::Vertex);
  return static_cast<ID3D11VertexShader*>(m_shader.Get());
}

ID3D11PixelShader* D3D11Shader::GetPixelShader() const
{
  DebugAssert(m_stage == GPUShaderStage::Fragment);
  return static_cast<ID3D11PixelShader*>(m_shader.Get());
}

ID3D11GeometryShader* D3D11Shader::GetGeometryShader() const
{
  DebugAssert(m_stage == GPUShaderStage::Geometry);
  return static_cast<ID3D11GeometryShader*>(m_shader.Get());
}

ID3D11ComputeShader* D3D11Shader::GetComputeShader() const
{
  DebugAssert(m_stage == GPUShaderStage::Compute);
  return static_cast<ID3D11ComputeShader*>(m_shader.Get());
}

void D3D11Shader::SetDebugName(std::string_view name)
{
  SetD3DDebugObjectName(m_shader.Get(), name);
}

std::unique_ptr<GPUShader> D3D11Device::CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                               Error* error)
{
  ComPtr<ID3D11DeviceChild> shader;
  std::vector<u8> bytecode;
  HRESULT hr;
  switch (stage)
  {
    case GPUShaderStage::Vertex:
      hr = m_device->CreateVertexShader(data.data(), data.size(), nullptr,
                                        reinterpret_cast<ID3D11VertexShader**>(shader.GetAddressOf()));
      bytecode.resize(data.size());
      std::memcpy(bytecode.data(), data.data(), data.size());
      break;

    case GPUShaderStage::Fragment:
      hr = m_device->CreatePixelShader(data.data(), data.size(), nullptr,
                                       reinterpret_cast<ID3D11PixelShader**>(shader.GetAddressOf()));
      break;

    case GPUShaderStage::Geometry:
      hr = m_device->CreateGeometryShader(data.data(), data.size(), nullptr,
                                          reinterpret_cast<ID3D11GeometryShader**>(shader.GetAddressOf()));
      break;

    case GPUShaderStage::Compute:
      hr = m_device->CreateComputeShader(data.data(), data.size(), nullptr,
                                         reinterpret_cast<ID3D11ComputeShader**>(shader.GetAddressOf()));
      break;

    default:
      UnreachableCode();
      hr = S_FALSE;
      break;
  }

  if (FAILED(hr) || !shader)
  {
    Error::SetHResult(error, "Create[Typed]Shader() failed: ", hr);
    return {};
  }

  return std::unique_ptr<GPUShader>(new D3D11Shader(stage, std::move(shader), std::move(bytecode)));
}

std::unique_ptr<GPUShader> D3D11Device::CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
                                                               std::string_view source, const char* entry_point,
                                                               DynamicHeapArray<u8>* out_binary, Error* error)
{
  const u32 shader_model = D3DCommon::GetShaderModelForFeatureLevelNumber(m_render_api_version);
  if (language != GPUShaderLanguage::HLSL)
  {
    return TranspileAndCreateShaderFromSource(stage, language, source, entry_point, GPUShaderLanguage::HLSL,
                                              shader_model, out_binary, error);
  }

  std::optional<DynamicHeapArray<u8>> bytecode =
    D3DCommon::CompileShader(shader_model, m_debug_device, stage, source, entry_point, error);
  if (!bytecode.has_value())
    return {};

  std::unique_ptr<GPUShader> ret = CreateShaderFromBinary(stage, bytecode.value(), error);
  if (ret && out_binary)
    *out_binary = std::move(bytecode.value());

  return ret;
}

D3D11Pipeline::D3D11Pipeline(ComPtr<ID3D11RasterizerState> rs, ComPtr<ID3D11DepthStencilState> ds,
                             ComPtr<ID3D11BlendState> bs, ComPtr<ID3D11InputLayout> il, ComPtr<ID3D11VertexShader> vs,
                             ComPtr<ID3D11GeometryShader> gs, ComPtr<ID3D11PixelShader> ps,
                             D3D11_PRIMITIVE_TOPOLOGY topology, u32 vertex_stride, u32 blend_factor)
  : m_rs(std::move(rs)), m_ds(std::move(ds)), m_bs(std::move(bs)), m_il(std::move(il)), m_vs(std::move(vs)),
    m_gs(std::move(gs)), m_ps(std::move(ps)), m_topology(topology), m_vertex_stride(vertex_stride),
    m_blend_factor(blend_factor), m_blend_factor_float(GPUDevice::RGBA8ToFloat(blend_factor))
{
}

D3D11Pipeline::~D3D11Pipeline()
{
  D3D11Device::GetInstance().UnbindPipeline(this);
}

void D3D11Pipeline::SetDebugName(std::string_view name)
{
  // can't label this directly
}

D3D11Device::ComPtr<ID3D11RasterizerState> D3D11Device::GetRasterizationState(const GPUPipeline::RasterizationState& rs,
                                                                              Error* error)
{
  ComPtr<ID3D11RasterizerState> drs;

  const auto it = m_rasterization_states.find(rs.key);
  if (it != m_rasterization_states.end())
  {
    drs = it->second;
    return drs;
  }

  static constexpr std::array<D3D11_CULL_MODE, static_cast<u32>(GPUPipeline::CullMode::MaxCount)> cull_mapping = {{
    D3D11_CULL_NONE,  // None
    D3D11_CULL_FRONT, // Front
    D3D11_CULL_BACK,  // Back
  }};

  D3D11_RASTERIZER_DESC desc = {};
  desc.FillMode = D3D11_FILL_SOLID;
  desc.CullMode = cull_mapping[static_cast<u8>(rs.cull_mode.GetValue())];
  desc.ScissorEnable = TRUE;
  // desc.MultisampleEnable ???

  HRESULT hr = m_device->CreateRasterizerState(&desc, drs.GetAddressOf());
  if (FAILED(hr)) [[unlikely]]
    Error::SetHResult(error, "CreateRasterizerState() failed: ", hr);
  else
    m_rasterization_states.emplace(rs.key, drs);

  return drs;
}

D3D11Device::ComPtr<ID3D11DepthStencilState> D3D11Device::GetDepthState(const GPUPipeline::DepthState& ds, Error* error)
{
  ComPtr<ID3D11DepthStencilState> dds;

  const auto it = m_depth_states.find(ds.key);
  if (it != m_depth_states.end())
  {
    dds = it->second;
    return dds;
  }

  static constexpr std::array<D3D11_COMPARISON_FUNC, static_cast<u32>(GPUPipeline::DepthFunc::MaxCount)> func_mapping =
    {{
      D3D11_COMPARISON_NEVER,         // Never
      D3D11_COMPARISON_ALWAYS,        // Always
      D3D11_COMPARISON_LESS,          // Less
      D3D11_COMPARISON_LESS_EQUAL,    // LessEqual
      D3D11_COMPARISON_GREATER,       // Greater
      D3D11_COMPARISON_GREATER_EQUAL, // GreaterEqual
      D3D11_COMPARISON_EQUAL,         // Equal
    }};

  D3D11_DEPTH_STENCIL_DESC desc = {};
  desc.DepthEnable = ds.depth_test != GPUPipeline::DepthFunc::Always || ds.depth_write;
  desc.DepthFunc = func_mapping[static_cast<u8>(ds.depth_test.GetValue())];
  desc.DepthWriteMask = ds.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;

  HRESULT hr = m_device->CreateDepthStencilState(&desc, dds.GetAddressOf());
  if (FAILED(hr)) [[unlikely]]
    Error::SetHResult(error, "CreateDepthStencilState() failed: ", hr);
  else
    m_depth_states.emplace(ds.key, dds);

  return dds;
}

size_t D3D11Device::BlendStateMapHash::operator()(const BlendStateMapKey& key) const
{
  size_t h = std::hash<u64>()(key.first);
  hash_combine(h, key.second);
  return h;
}

D3D11Device::ComPtr<ID3D11BlendState> D3D11Device::GetBlendState(const GPUPipeline::BlendState& bs, u32 num_rts, Error* error)
{
  ComPtr<ID3D11BlendState> dbs;

  const std::pair<u64, u32> key(bs.key, num_rts);
  const auto it = m_blend_states.find(key);
  if (it != m_blend_states.end())
  {
    dbs = it->second;
    return dbs;
  }

  static constexpr std::array<D3D11_BLEND, static_cast<u32>(GPUPipeline::BlendFunc::MaxCount)> blend_mapping = {{
    D3D11_BLEND_ZERO,             // Zero
    D3D11_BLEND_ONE,              // One
    D3D11_BLEND_SRC_COLOR,        // SrcColor
    D3D11_BLEND_INV_SRC_COLOR,    // InvSrcColor
    D3D11_BLEND_DEST_COLOR,       // DstColor
    D3D11_BLEND_INV_DEST_COLOR,   // InvDstColor
    D3D11_BLEND_SRC_ALPHA,        // SrcAlpha
    D3D11_BLEND_INV_SRC_ALPHA,    // InvSrcAlpha
    D3D11_BLEND_SRC1_ALPHA,       // SrcAlpha1
    D3D11_BLEND_INV_SRC1_ALPHA,   // InvSrcAlpha1
    D3D11_BLEND_DEST_ALPHA,       // DstAlpha
    D3D11_BLEND_INV_DEST_ALPHA,   // InvDstAlpha
    D3D11_BLEND_BLEND_FACTOR,     // ConstantColor
    D3D11_BLEND_INV_BLEND_FACTOR, // InvConstantColor
  }};

  static constexpr std::array<D3D11_BLEND_OP, static_cast<u32>(GPUPipeline::BlendOp::MaxCount)> op_mapping = {{
    D3D11_BLEND_OP_ADD,          // Add
    D3D11_BLEND_OP_SUBTRACT,     // Subtract
    D3D11_BLEND_OP_REV_SUBTRACT, // ReverseSubtract
    D3D11_BLEND_OP_MIN,          // Min
    D3D11_BLEND_OP_MAX,          // Max
  }};

  D3D11_BLEND_DESC blend_desc = {};
  for (u32 i = 0; i < num_rts; i++)
  {
    D3D11_RENDER_TARGET_BLEND_DESC& tgt_desc = blend_desc.RenderTarget[i];
    tgt_desc.BlendEnable = bs.enable;
    tgt_desc.RenderTargetWriteMask = bs.write_mask;
    if (bs.enable)
    {
      tgt_desc.SrcBlend = blend_mapping[static_cast<u8>(bs.src_blend.GetValue())];
      tgt_desc.DestBlend = blend_mapping[static_cast<u8>(bs.dst_blend.GetValue())];
      tgt_desc.BlendOp = op_mapping[static_cast<u8>(bs.blend_op.GetValue())];
      tgt_desc.SrcBlendAlpha = blend_mapping[static_cast<u8>(bs.src_alpha_blend.GetValue())];
      tgt_desc.DestBlendAlpha = blend_mapping[static_cast<u8>(bs.dst_alpha_blend.GetValue())];
      tgt_desc.BlendOpAlpha = op_mapping[static_cast<u8>(bs.alpha_blend_op.GetValue())];
    }
  }

  HRESULT hr = m_device->CreateBlendState(&blend_desc, dbs.GetAddressOf());
  if (FAILED(hr)) [[unlikely]]
    Error::SetHResult(error, "CreateBlendState() failed: ", hr);
  else
    m_blend_states.emplace(key, dbs);

  return dbs;
}

D3D11Device::ComPtr<ID3D11InputLayout> D3D11Device::GetInputLayout(const GPUPipeline::InputLayout& il,
                                                                   const D3D11Shader* vs, Error* error)
{
  ComPtr<ID3D11InputLayout> dil;
  const auto it = m_input_layouts.find(il);
  if (it != m_input_layouts.end())
  {
    dil = it->second;
    return dil;
  }

  static constexpr u32 MAX_COMPONENTS = 4;
  static constexpr const DXGI_FORMAT
    format_mapping[static_cast<u8>(GPUPipeline::VertexAttribute::Type::MaxCount)][MAX_COMPONENTS] = {
      {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
       DXGI_FORMAT_R32G32B32A32_FLOAT},                                                                       // Float
      {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UINT},           // UInt8
      {DXGI_FORMAT_R8_SINT, DXGI_FORMAT_R8G8_SINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_SINT},           // SInt8
      {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM},        // UNorm8
      {DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16G16_UINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_UINT},    // UInt16
      {DXGI_FORMAT_R16_SINT, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_SINT},    // SInt16
      {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_UNORM}, // UNorm16
      {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32B32A32_UINT},    // UInt32
      {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32G32B32A32_SINT},    // SInt32
    };

  D3D11_INPUT_ELEMENT_DESC* elems =
    static_cast<D3D11_INPUT_ELEMENT_DESC*>(alloca(sizeof(D3D11_INPUT_ELEMENT_DESC) * il.vertex_attributes.size()));
  for (size_t i = 0; i < il.vertex_attributes.size(); i++)
  {
    const GPUPipeline::VertexAttribute& va = il.vertex_attributes[i];
    Assert(va.components > 0 && va.components <= MAX_COMPONENTS);

    D3D11_INPUT_ELEMENT_DESC& elem = elems[i];
    elem.SemanticName = "ATTR";
    elem.SemanticIndex = va.index;
    elem.Format = format_mapping[static_cast<u8>(va.type.GetValue())][va.components - 1];
    elem.InputSlot = 0;
    elem.AlignedByteOffset = va.offset;
    elem.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elem.InstanceDataStepRate = 0;
  }

  HRESULT hr = m_device->CreateInputLayout(elems, static_cast<UINT>(il.vertex_attributes.size()),
                                           vs->GetBytecode().data(), vs->GetBytecode().size(), dil.GetAddressOf());
  if (FAILED(hr)) [[unlikely]]
    Error::SetHResult(error, "CreateInputLayout() failed: ", hr);
  else
    m_input_layouts.emplace(il, dil);

  return dil;
}

std::unique_ptr<GPUPipeline> D3D11Device::CreatePipeline(const GPUPipeline::GraphicsConfig& config, Error* error)
{
  ComPtr<ID3D11RasterizerState> rs = GetRasterizationState(config.rasterization, error);
  ComPtr<ID3D11DepthStencilState> ds = GetDepthState(config.depth, error);
  ComPtr<ID3D11BlendState> bs = GetBlendState(config.blend, config.GetRenderTargetCount(), error);
  if (!rs || !ds || !bs)
    return {};

  ComPtr<ID3D11InputLayout> il;
  u32 vertex_stride = 0;
  if (!config.input_layout.vertex_attributes.empty())
  {
    il = GetInputLayout(config.input_layout, static_cast<const D3D11Shader*>(config.vertex_shader), error);
    vertex_stride = config.input_layout.vertex_stride;
    if (!il)
      return {};
  }

  static constexpr std::array<D3D11_PRIMITIVE_TOPOLOGY, static_cast<u32>(GPUPipeline::Primitive::MaxCount)> primitives =
    {{
      D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,     // Points
      D3D11_PRIMITIVE_TOPOLOGY_LINELIST,      // Lines
      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,  // Triangles
      D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, // TriangleStrips
    }};

  return std::unique_ptr<GPUPipeline>(new D3D11Pipeline(
    std::move(rs), std::move(ds), std::move(bs), std::move(il),
    static_cast<const D3D11Shader*>(config.vertex_shader)->GetVertexShader(),
    config.geometry_shader ? static_cast<const D3D11Shader*>(config.geometry_shader)->GetGeometryShader() : nullptr,
    static_cast<const D3D11Shader*>(config.fragment_shader)->GetPixelShader(),
    primitives[static_cast<u8>(config.primitive)], vertex_stride, config.blend.constant));
}

void D3D11Device::SetPipeline(GPUPipeline* pipeline)
{
  if (m_current_pipeline == pipeline)
    return;

  D3D11Pipeline* const PL = static_cast<D3D11Pipeline*>(pipeline);
  m_current_pipeline = PL;

  if (ID3D11InputLayout* il = PL->GetInputLayout(); m_current_input_layout != il)
  {
    m_current_input_layout = il;
    m_context->IASetInputLayout(il);
  }

  if (const u32 vertex_stride = PL->GetVertexStride(); m_current_vertex_stride != vertex_stride)
  {
    const UINT offset = 0;
    m_current_vertex_stride = PL->GetVertexStride();
    m_context->IASetVertexBuffers(0, 1, m_vertex_buffer.GetD3DBufferArray(), &m_current_vertex_stride, &offset);
  }

  if (D3D_PRIMITIVE_TOPOLOGY topology = PL->GetPrimitiveTopology(); m_current_primitive_topology != topology)
  {
    m_current_primitive_topology = topology;
    m_context->IASetPrimitiveTopology(topology);
  }

  if (ID3D11VertexShader* vs = PL->GetVertexShader(); m_current_vertex_shader != vs)
  {
    m_current_vertex_shader = vs;
    m_context->VSSetShader(vs, nullptr, 0);
  }

  if (ID3D11GeometryShader* gs = PL->GetGeometryShader(); m_current_geometry_shader != gs)
  {
    m_current_geometry_shader = gs;
    m_context->GSSetShader(gs, nullptr, 0);
  }

  if (ID3D11PixelShader* ps = PL->GetPixelShader(); m_current_pixel_shader != ps)
  {
    m_current_pixel_shader = ps;
    m_context->PSSetShader(ps, nullptr, 0);
  }

  if (ID3D11RasterizerState* rs = PL->GetRasterizerState(); m_current_rasterizer_state != rs)
  {
    m_current_rasterizer_state = rs;
    m_context->RSSetState(rs);
  }

  if (ID3D11DepthStencilState* ds = PL->GetDepthStencilState(); m_current_depth_state != ds)
  {
    m_current_depth_state = ds;
    m_context->OMSetDepthStencilState(ds, 0);
  }

  if (ID3D11BlendState* bs = PL->GetBlendState();
      m_current_blend_state != bs || m_current_blend_factor != PL->GetBlendFactor())
  {
    m_current_blend_state = bs;
    m_current_blend_factor = PL->GetBlendFactor();
    m_context->OMSetBlendState(bs, RGBA8ToFloat(m_current_blend_factor).data(), 0xFFFFFFFFu);
  }
}

void D3D11Device::UnbindPipeline(D3D11Pipeline* pl)
{
  if (m_current_pipeline != pl)
    return;

  // Let the runtime deal with the dead objects...
  m_current_pipeline = nullptr;
}
