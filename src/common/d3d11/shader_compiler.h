#pragma once
#include "YBaseLib/Windows/WindowsHeaders.h"
#include <d3d11.h>
#include <string_view>
#include <type_traits>
#include <wrl/client.h>

namespace D3D11::ShaderCompiler {
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

enum class Type
{
  Vertex,
  Pixel,
  Compute
};

ComPtr<ID3DBlob> CompileShader(Type type, D3D_FEATURE_LEVEL feature_level, std::string_view code, bool debug);

ComPtr<ID3D11VertexShader> CompileAndCreateVertexShader(ID3D11Device* device, std::string_view code, bool debug);
ComPtr<ID3D11PixelShader> CompileAndCreatePixelShader(ID3D11Device* device, std::string_view code, bool debug);
ComPtr<ID3D11ComputeShader> CompileAndCreateComputeShader(ID3D11Device* device, std::string_view code, bool debug);

}; // namespace D3D11::ShaderCompiler
