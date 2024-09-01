// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"

#include "common/windows_headers.h"

#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <string>
#include <string_view>
#include <vector>
#include <wrl/client.h>

class D3D11Device;

class D3D11Shader final : public GPUShader
{
  friend D3D11Device;

public:
  ~D3D11Shader() override;

  ID3D11VertexShader* GetVertexShader() const;
  ID3D11PixelShader* GetPixelShader() const;
  ID3D11GeometryShader* GetGeometryShader() const;
  ID3D11ComputeShader* GetComputeShader() const;

  ALWAYS_INLINE const std::vector<u8>& GetBytecode() const { return m_bytecode; }

  void SetDebugName(std::string_view name) override;

private:
  D3D11Shader(GPUShaderStage stage, Microsoft::WRL::ComPtr<ID3D11DeviceChild> shader, std::vector<u8> bytecode);

  Microsoft::WRL::ComPtr<ID3D11DeviceChild> m_shader;
  std::vector<u8> m_bytecode; // only for VS
};

class D3D11Pipeline final : public GPUPipeline
{
  friend D3D11Device;

  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

public:
  ~D3D11Pipeline() override;

  void SetDebugName(std::string_view name) override;

  ALWAYS_INLINE ID3D11RasterizerState* GetRasterizerState() const { return m_rs.Get(); }
  ALWAYS_INLINE ID3D11DepthStencilState* GetDepthStencilState() const { return m_ds.Get(); }
  ALWAYS_INLINE ID3D11BlendState* GetBlendState() const { return m_bs.Get(); }
  ALWAYS_INLINE ID3D11InputLayout* GetInputLayout() const { return m_il.Get(); }
  ALWAYS_INLINE ID3D11VertexShader* GetVertexShader() const { return m_vs.Get(); }
  ALWAYS_INLINE ID3D11GeometryShader* GetGeometryShader() const { return m_gs.Get(); }
  ALWAYS_INLINE ID3D11PixelShader* GetPixelShader() const { return m_ps.Get(); }
  ALWAYS_INLINE D3D11_PRIMITIVE_TOPOLOGY GetPrimitiveTopology() const { return m_topology; }
  ALWAYS_INLINE u32 GetVertexStride() const { return m_vertex_stride; }
  ALWAYS_INLINE u32 GetBlendFactor() const { return m_blend_factor; }
  ALWAYS_INLINE const std::array<float, 4>& GetBlendFactorFloat() const { return m_blend_factor_float; }

private:
  D3D11Pipeline(ComPtr<ID3D11RasterizerState> rs, ComPtr<ID3D11DepthStencilState> ds, ComPtr<ID3D11BlendState> bs,
                ComPtr<ID3D11InputLayout> il, ComPtr<ID3D11VertexShader> vs, ComPtr<ID3D11GeometryShader> gs,
                ComPtr<ID3D11PixelShader> ps, D3D11_PRIMITIVE_TOPOLOGY topology, u32 vertex_stride, u32 blend_factor);

  ComPtr<ID3D11RasterizerState> m_rs;
  ComPtr<ID3D11DepthStencilState> m_ds;
  ComPtr<ID3D11BlendState> m_bs;
  ComPtr<ID3D11InputLayout> m_il;
  ComPtr<ID3D11VertexShader> m_vs;
  ComPtr<ID3D11GeometryShader> m_gs;
  ComPtr<ID3D11PixelShader> m_ps;
  D3D11_PRIMITIVE_TOPOLOGY m_topology;
  u32 m_vertex_stride;
  u32 m_blend_factor;
  std::array<float, 4> m_blend_factor_float;
};
