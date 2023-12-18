// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum class GPUShaderStage : u8;

namespace SPIRVCompiler {

enum CompileOptions
{
  DebugInfo = (1 << 0),
  VulkanRules = (1 << 1),
};

// SPIR-V compiled code type
using SPIRVCodeType = u32;
using SPIRVCodeVector = std::vector<SPIRVCodeType>;

// Compile a vertex shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileVertexShader(std::string_view source_code, u32 options);

// Compile a fragment shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileFragmentShader(std::string_view source_code, u32 options);

// Compile a geometry shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileGeometryShader(std::string_view source_code, u32 options);

// Compile a compute shader to SPIR-V.
std::optional<SPIRVCodeVector> CompileComputeShader(std::string_view source_code, u32 options);

std::optional<SPIRVCodeVector> CompileShader(GPUShaderStage stage, std::string_view source_code, u32 options);

#ifdef __APPLE__

// Converts a SPIR-V shader into MSL.
std::optional<std::string> CompileSPIRVToMSL(GPUShaderStage stage, std::span<const SPIRVCodeType> spv);

#endif

} // namespace SPIRVCompiler
