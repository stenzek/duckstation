#pragma once

#include "../types.h"
#include <optional>
#include <string_view>
#include <vector>

namespace Vulkan::ShaderCompiler {

// Shader types
enum class Type
{
  Vertex,
  Geometry,
  Fragment,
  Compute
};

void DeinitializeGlslang();

// SPIR-V compiled code type
using SPIRVCodeType = u32;
using SPIRVCodeVector = std::vector<SPIRVCodeType>;

// Compile a vertex shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileVertexShader(std::string_view source_code);

// Compile a geometry shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileGeometryShader(std::string_view source_code);

// Compile a fragment shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileFragmentShader(std::string_view source_code);

// Compile a compute shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileComputeShader(std::string_view source_code);

std::optional<SPIRVCodeVector> CompileShader(Type type, std::string_view source_code, bool debug);

} // namespace Vulkan::ShaderCompiler
