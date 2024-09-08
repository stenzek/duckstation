// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "d3d12_pipeline.h"
#include "d3d12_builders.h"
#include "d3d12_device.h"
#include "d3d_common.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/sha1_digest.h"
#include "common/string_util.h"

#include <d3dcompiler.h>

Log_SetChannel(D3D12Device);

D3D12Shader::D3D12Shader(GPUShaderStage stage, Bytecode bytecode) : GPUShader(stage), m_bytecode(std::move(bytecode))
{
}

D3D12Shader::~D3D12Shader() = default;

void D3D12Shader::SetDebugName(std::string_view name)
{
}

std::unique_ptr<GPUShader> D3D12Device::CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                               Error* error)
{
  // Can't do much at this point.
  std::vector bytecode(data.begin(), data.end());
  return std::unique_ptr<GPUShader>(new D3D12Shader(stage, std::move(bytecode)));
}

std::unique_ptr<GPUShader> D3D12Device::CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
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

//////////////////////////////////////////////////////////////////////////

D3D12Pipeline::D3D12Pipeline(Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline, Layout layout,
                             D3D12_PRIMITIVE_TOPOLOGY topology, u32 vertex_stride, u32 blend_constants)
  : GPUPipeline(), m_pipeline(std::move(pipeline)), m_layout(layout), m_topology(topology),
    m_vertex_stride(vertex_stride), m_blend_constants(blend_constants),
    m_blend_constants_f(GPUDevice::RGBA8ToFloat(blend_constants))
{
}

D3D12Pipeline::~D3D12Pipeline()
{
  D3D12Device::GetInstance().DeferObjectDestruction(std::move(m_pipeline));
}

void D3D12Pipeline::SetDebugName(std::string_view name)
{
  D3D12::SetObjectName(m_pipeline.Get(), name);
}

std::string D3D12Pipeline::GetPipelineName(const GraphicsConfig& config)
{
  SHA1Digest hash;
  hash.Update(&config.layout, sizeof(config.layout));
  hash.Update(&config.primitive, sizeof(config.primitive));
  if (!config.input_layout.vertex_attributes.empty())
  {
    hash.Update(config.input_layout.vertex_attributes.data(),
                sizeof(VertexAttribute) * static_cast<u32>(config.input_layout.vertex_attributes.size()));
    hash.Update(&config.input_layout.vertex_stride, sizeof(config.input_layout.vertex_stride));
  }
  hash.Update(&config.rasterization.key, sizeof(config.rasterization.key));
  hash.Update(&config.depth.key, sizeof(config.depth.key));
  hash.Update(&config.blend.key, sizeof(config.blend.key));
  if (const D3D12Shader* shader = static_cast<const D3D12Shader*>(config.vertex_shader))
    hash.Update(shader->GetBytecodeData(), shader->GetBytecodeSize());
  if (const D3D12Shader* shader = static_cast<const D3D12Shader*>(config.fragment_shader))
    hash.Update(shader->GetBytecodeData(), shader->GetBytecodeSize());
  if (const D3D12Shader* shader = static_cast<const D3D12Shader*>(config.geometry_shader))
    hash.Update(shader->GetBytecodeData(), shader->GetBytecodeSize());
  hash.Update(&config.color_formats, sizeof(config.color_formats));
  hash.Update(&config.depth_format, sizeof(config.depth_format));
  hash.Update(&config.samples, sizeof(config.samples));
  hash.Update(&config.per_sample_shading, sizeof(config.per_sample_shading));

  u8 digest[SHA1Digest::DIGEST_SIZE];
  hash.Final(digest);
  return SHA1Digest::DigestToString(digest);
}

std::unique_ptr<GPUPipeline> D3D12Device::CreatePipeline(const GPUPipeline::GraphicsConfig& config, Error* error)
{
  static constexpr std::array<D3D12_PRIMITIVE_TOPOLOGY, static_cast<u32>(GPUPipeline::Primitive::MaxCount)> primitives =
    {{
      D3D_PRIMITIVE_TOPOLOGY_POINTLIST,     // Points
      D3D_PRIMITIVE_TOPOLOGY_LINELIST,      // Lines
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,  // Triangles
      D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, // TriangleStrips
    }};
  static constexpr std::array<D3D12_PRIMITIVE_TOPOLOGY_TYPE, static_cast<u32>(GPUPipeline::Primitive::MaxCount)>
    primitive_types = {{
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,    // Points
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,     // Lines
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, // Triangles
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, // TriangleStrips
    }};

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

  static constexpr std::array<D3D12_CULL_MODE, static_cast<u32>(GPUPipeline::CullMode::MaxCount)> cull_mapping = {{
    D3D12_CULL_MODE_NONE,  // None
    D3D12_CULL_MODE_FRONT, // Front
    D3D12_CULL_MODE_BACK,  // Back
  }};

  static constexpr std::array<D3D12_COMPARISON_FUNC, static_cast<u32>(GPUPipeline::DepthFunc::MaxCount)>
    compare_mapping = {{
      D3D12_COMPARISON_FUNC_NEVER,         // Never
      D3D12_COMPARISON_FUNC_ALWAYS,        // Always
      D3D12_COMPARISON_FUNC_LESS,          // Less
      D3D12_COMPARISON_FUNC_LESS_EQUAL,    // LessEqual
      D3D12_COMPARISON_FUNC_GREATER,       // Greater
      D3D12_COMPARISON_FUNC_GREATER_EQUAL, // GreaterEqual
      D3D12_COMPARISON_FUNC_EQUAL,         // Equal
    }};

  static constexpr std::array<D3D12_BLEND, static_cast<u32>(GPUPipeline::BlendFunc::MaxCount)> blend_mapping = {{
    D3D12_BLEND_ZERO,             // Zero
    D3D12_BLEND_ONE,              // One
    D3D12_BLEND_SRC_COLOR,        // SrcColor
    D3D12_BLEND_INV_SRC_COLOR,    // InvSrcColor
    D3D12_BLEND_DEST_COLOR,       // DstColor
    D3D12_BLEND_INV_DEST_COLOR,   // InvDstColor
    D3D12_BLEND_SRC_ALPHA,        // SrcAlpha
    D3D12_BLEND_INV_SRC_ALPHA,    // InvSrcAlpha
    D3D12_BLEND_SRC1_ALPHA,       // SrcAlpha1
    D3D12_BLEND_INV_SRC1_ALPHA,   // InvSrcAlpha1
    D3D12_BLEND_DEST_ALPHA,       // DstAlpha
    D3D12_BLEND_INV_DEST_ALPHA,   // InvDstAlpha
    D3D12_BLEND_BLEND_FACTOR,     // ConstantColor
    D3D12_BLEND_INV_BLEND_FACTOR, // InvConstantColor
  }};

  static constexpr std::array<D3D12_BLEND_OP, static_cast<u32>(GPUPipeline::BlendOp::MaxCount)> op_mapping = {{
    D3D12_BLEND_OP_ADD,          // Add
    D3D12_BLEND_OP_SUBTRACT,     // Subtract
    D3D12_BLEND_OP_REV_SUBTRACT, // ReverseSubtract
    D3D12_BLEND_OP_MIN,          // Min
    D3D12_BLEND_OP_MAX,          // Max
  }};

  if (config.render_pass_flags & GPUPipeline::BindRenderTargetsAsImages && !m_features.raster_order_views)
  {
    ERROR_LOG("Attempting to create ROV pipeline without ROV feature.");
    return {};
  }

  D3D12::GraphicsPipelineBuilder gpb;
  gpb.SetRootSignature(m_root_signatures[BoolToUInt8(
    (config.render_pass_flags & GPUPipeline::BindRenderTargetsAsImages))][static_cast<u8>(config.layout)]
                         .Get());
  gpb.SetVertexShader(static_cast<const D3D12Shader*>(config.vertex_shader)->GetBytecodeData(),
                      static_cast<const D3D12Shader*>(config.vertex_shader)->GetBytecodeSize());
  gpb.SetPixelShader(static_cast<const D3D12Shader*>(config.fragment_shader)->GetBytecodeData(),
                     static_cast<const D3D12Shader*>(config.fragment_shader)->GetBytecodeSize());
  if (config.geometry_shader)
  {
    gpb.SetGeometryShader(static_cast<const D3D12Shader*>(config.geometry_shader)->GetBytecodeData(),
                          static_cast<const D3D12Shader*>(config.geometry_shader)->GetBytecodeSize());
  }
  gpb.SetPrimitiveTopologyType(primitive_types[static_cast<u8>(config.primitive)]);

  if (!config.input_layout.vertex_attributes.empty())
  {
    for (u32 i = 0; i < static_cast<u32>(config.input_layout.vertex_attributes.size()); i++)
    {
      const GPUPipeline::VertexAttribute& va = config.input_layout.vertex_attributes[i];
      DebugAssert(va.components > 0 && va.components <= MAX_COMPONENTS);
      gpb.AddVertexAttribute(
        "ATTR", i, format_mapping[static_cast<u8>(va.type.GetValue())][static_cast<u8>(va.components.GetValue() - 1)],
        0, va.offset);
    }
  }

  gpb.SetRasterizationState(D3D12_FILL_MODE_SOLID,
                            cull_mapping[static_cast<u8>(config.rasterization.cull_mode.GetValue())], false);
  if (config.samples > 1)
    gpb.SetMultisamples(config.samples);
  gpb.SetDepthState(config.depth.depth_test != GPUPipeline::DepthFunc::Always || config.depth.depth_write,
                    config.depth.depth_write, compare_mapping[static_cast<u8>(config.depth.depth_test.GetValue())]);
  gpb.SetNoStencilState();

  gpb.SetBlendState(0, config.blend.enable, blend_mapping[static_cast<u8>(config.blend.src_blend.GetValue())],
                    blend_mapping[static_cast<u8>(config.blend.dst_blend.GetValue())],
                    op_mapping[static_cast<u8>(config.blend.blend_op.GetValue())],
                    blend_mapping[static_cast<u8>(config.blend.src_alpha_blend.GetValue())],
                    blend_mapping[static_cast<u8>(config.blend.dst_alpha_blend.GetValue())],
                    op_mapping[static_cast<u8>(config.blend.alpha_blend_op.GetValue())], config.blend.write_mask);

  for (u32 i = 0; i < MAX_RENDER_TARGETS; i++)
  {
    if (config.color_formats[i] != GPUTexture::Format::Unknown)
      gpb.SetRenderTarget(i, D3DCommon::GetFormatMapping(config.color_formats[i]).rtv_format);
  }

  if (config.depth_format != GPUTexture::Format::Unknown)
    gpb.SetDepthStencilFormat(D3DCommon::GetFormatMapping(config.depth_format).dsv_format);

  ComPtr<ID3D12PipelineState> pipeline;
  if (m_pipeline_library)
  {
    const std::wstring name = StringUtil::UTF8StringToWideString(D3D12Pipeline::GetPipelineName(config));
    HRESULT hr =
      m_pipeline_library->LoadGraphicsPipeline(name.c_str(), gpb.GetDesc(), IID_PPV_ARGS(pipeline.GetAddressOf()));
    if (FAILED(hr))
    {
      // E_INVALIDARG = not found.
      if (hr != E_INVALIDARG)
        ERROR_LOG("LoadGraphicsPipeline() failed with HRESULT {:08X}", static_cast<unsigned>(hr));

      // Need to create it normally.
      pipeline = gpb.Create(m_device.Get(), error, false);

      // Store if it wasn't an OOM or something else.
      if (pipeline && hr == E_INVALIDARG)
      {
        hr = m_pipeline_library->StorePipeline(name.c_str(), pipeline.Get());
        if (FAILED(hr))
          ERROR_LOG("StorePipeline() failed with HRESULT {:08X}", static_cast<unsigned>(hr));
      }
    }
  }
  else
  {
    pipeline = gpb.Create(m_device.Get(), error, false);
  }

  if (!pipeline)
    return {};

  return std::unique_ptr<GPUPipeline>(new D3D12Pipeline(
    pipeline, config.layout, primitives[static_cast<u8>(config.primitive)],
    config.input_layout.vertex_attributes.empty() ? 0 : config.input_layout.vertex_stride, config.blend.constant));
}
