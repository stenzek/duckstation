// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_device.h"

#include "common/windows_headers.h"

#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

class D3D12Device;

class D3D12Shader final : public GPUShader
{
  friend D3D12Device;

public:
  using Bytecode = std::vector<u8>;

  ~D3D12Shader() override;

  ALWAYS_INLINE const Bytecode& GetBytecode() const { return m_bytecode; }
  ALWAYS_INLINE D3D12_SHADER_BYTECODE GetD3DBytecode() const { return {m_bytecode.data(), m_bytecode.size()}; }
  ALWAYS_INLINE const u8* GetBytecodeData() const { return m_bytecode.data(); }
  ALWAYS_INLINE u32 GetBytecodeSize() const { return static_cast<u32>(m_bytecode.size()); }

  void SetDebugName(std::string_view name) override;

private:
  D3D12Shader(GPUShaderStage stage, Bytecode bytecode);

  Bytecode m_bytecode;
};

class D3D12Pipeline final : public GPUPipeline
{
  friend D3D12Device;

public:
  ~D3D12Pipeline() override;

  ALWAYS_INLINE ID3D12PipelineState* GetPipeline() const { return m_pipeline.Get(); }
  ALWAYS_INLINE Layout GetLayout() const { return m_layout; }
  ALWAYS_INLINE D3D12_PRIMITIVE_TOPOLOGY GetTopology() const { return m_topology; }
  ALWAYS_INLINE u32 GetVertexStride() const { return m_vertex_stride; }
  ALWAYS_INLINE u32 GetBlendConstants() const { return m_blend_constants; }
  ALWAYS_INLINE const std::array<float, 4>& GetBlendConstantsF() const { return m_blend_constants_f; }
  ALWAYS_INLINE bool HasVertexStride() const { return (m_vertex_stride > 0); }

  void SetDebugName(std::string_view name) override;

  static std::string GetPipelineName(const GraphicsConfig& config);

private:
  D3D12Pipeline(Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline, Layout layout, D3D12_PRIMITIVE_TOPOLOGY topology,
                u32 vertex_stride, u32 blend_constants);

  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
  Layout m_layout;
  D3D12_PRIMITIVE_TOPOLOGY m_topology;
  u32 m_vertex_stride;
  u32 m_blend_constants;
  std::array<float, 4> m_blend_constants_f;
};
