#include "shader_compiler.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include <array>
#include <d3dcompiler.h>
#include <fstream>
Log_SetChannel(D3D11);

namespace D3D11::ShaderCompiler {

static unsigned s_next_bad_shader_id = 1;

ComPtr<ID3DBlob> CompileShader(Type type, D3D_FEATURE_LEVEL feature_level, std::string_view code, bool debug)
{
  const char* target;
  switch (feature_level)
  {
    case D3D_FEATURE_LEVEL_10_0:
    {
      static constexpr std::array<const char*, 4> targets = {{"vs_4_0", "gs_4_0", "ps_4_0", "cs_4_0"}};
      target = targets[static_cast<int>(type)];
    }
    break;

    case D3D_FEATURE_LEVEL_10_1:
    {
      static constexpr std::array<const char*, 4> targets = {{"vs_4_1", "gs_4_1", "ps_4_1", "cs_4_1"}};
      target = targets[static_cast<int>(type)];
    }
    break;

    case D3D_FEATURE_LEVEL_11_0:
    {
      static constexpr std::array<const char*, 4> targets = {{"vs_5_0", "gs_5_0", "ps_5_0", "cs_5_0"}};
      target = targets[static_cast<int>(type)];
    }
    break;

    case D3D_FEATURE_LEVEL_11_1:
    default:
    {
      static constexpr std::array<const char*, 4> targets = {{"vs_5_1", "gs_5_1", "ps_5_1", "cs_5_1"}};
      target = targets[static_cast<int>(type)];
    }
    break;
  }

  static constexpr UINT flags_non_debug = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  static constexpr UINT flags_debug = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;

  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error_blob;
  const HRESULT hr =
    D3DCompile(code.data(), code.size(), "0", nullptr, nullptr, "main", target, debug ? flags_debug : flags_non_debug,
               0, blob.GetAddressOf(), error_blob.GetAddressOf());

  String error_string;
  if (error_blob)
  {
    error_string.AppendString(static_cast<const char*>(error_blob->GetBufferPointer()),
                              static_cast<uint32>(error_blob->GetBufferSize()));
    error_blob.Reset();
  }

  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to compile '%s':\n%s", target, error_string.GetCharArray());

    std::ofstream ofs(SmallString::FromFormat("bad_shader_%u.txt", s_next_bad_shader_id++),
                      std::ofstream::out | std::ofstream::binary);
    if (ofs.is_open())
    {
      ofs << code;
      ofs << "\n\nCompile as " << target << " failed: " << hr << "\n";
      ofs.write(error_string.GetCharArray(), error_string.GetLength());
      ofs.close();
    }

    return {};
  }

  if (!error_string.IsEmpty())
    Log_WarningPrintf("'%s' compiled with warnings:\n%s", target, error_string.GetCharArray());

  return blob;
}

ComPtr<ID3D11VertexShader> CompileAndCreateVertexShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Vertex, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  ComPtr<ID3D11VertexShader> shader;
  const HRESULT hr =
    device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create vertex shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11GeometryShader> CompileAndCreateGeometryShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Geometry, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  ComPtr<ID3D11GeometryShader> shader;
  const HRESULT hr =
    device->CreateGeometryShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create geometry shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11PixelShader> CompileAndCreatePixelShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Pixel, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  ComPtr<ID3D11PixelShader> shader;
  const HRESULT hr =
    device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create pixel shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11ComputeShader> CompileAndCreateComputeShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Compute, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  ComPtr<ID3D11ComputeShader> shader;
  const HRESULT hr =
    device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create compute shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

} // namespace D3D11::ShaderCompiler