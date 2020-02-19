#include "shader_compiler.h"
#include "../log.h"
#include "../string_util.h"
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

  std::string error_string;
  if (error_blob)
  {
    error_string.append(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
    error_blob.Reset();
  }

  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to compile '%s':\n%s", target, error_string.c_str());

    std::ofstream ofs(StringUtil::StdStringFromFormat("bad_shader_%u.txt", s_next_bad_shader_id++).c_str(),
                      std::ofstream::out | std::ofstream::binary);
    if (ofs.is_open())
    {
      ofs << code;
      ofs << "\n\nCompile as " << target << " failed: " << hr << "\n";
      ofs.write(error_string.c_str(), error_string.size());
      ofs.close();
    }

    return {};
  }

  if (!error_string.empty())
    Log_WarningPrintf("'%s' compiled with warnings:\n%s", target, error_string.c_str());

  return blob;
}

ComPtr<ID3D11VertexShader> CompileAndCreateVertexShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Vertex, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  return CreateVertexShader(device, blob.Get());
}

ComPtr<ID3D11GeometryShader> CompileAndCreateGeometryShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Geometry, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  return CreateGeometryShader(device, blob.Get());
}

ComPtr<ID3D11PixelShader> CompileAndCreatePixelShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Pixel, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  return CreatePixelShader(device, blob.Get());
}

ComPtr<ID3D11ComputeShader> CompileAndCreateComputeShader(ID3D11Device* device, std::string_view code, bool debug)
{
  ComPtr<ID3DBlob> blob = CompileShader(Type::Compute, device->GetFeatureLevel(), std::move(code), debug);
  if (!blob)
    return {};

  return CreateComputeShader(device, blob.Get());
}

ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11VertexShader> shader;
  const HRESULT hr = device->CreateVertexShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create vertex shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreateVertexShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                            const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

ComPtr<ID3D11GeometryShader> CreateGeometryShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11GeometryShader> shader;
  const HRESULT hr = device->CreateGeometryShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create geometry shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11GeometryShader> CreateGeometryShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreateGeometryShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                              const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11PixelShader> shader;
  const HRESULT hr = device->CreatePixelShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create pixel shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreatePixelShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                           const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11ComputeShader> shader;
  const HRESULT hr = device->CreateComputeShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create compute shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreateComputeShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                             const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

} // namespace D3D11::ShaderCompiler