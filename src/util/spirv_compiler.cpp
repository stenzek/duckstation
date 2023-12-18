// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "spirv_compiler.h"
#include "gpu_device.h"

#include "core/settings.h" // TODO: Remove me

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <cstring>
#include <memory>
Log_SetChannel(SPIRVCompiler);

// glslang includes
#include "SPIRV/GlslangToSpv.h"
#include "StandAlone/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"

#ifdef __APPLE__
#include "spirv-cross/spirv_cross.hpp"
#include "spirv-cross/spirv_msl.hpp"
#endif

namespace SPIRVCompiler {
static std::optional<SPIRVCodeVector> CompileShaderToSPV(EShLanguage stage, const char* stage_filename,
                                                         std::string_view source, u32 options);

static unsigned s_next_bad_shader_id = 1;
} // namespace SPIRVCompiler

std::optional<SPIRVCompiler::SPIRVCodeVector>
SPIRVCompiler::CompileShaderToSPV(EShLanguage stage, const char* stage_filename, std::string_view source, u32 options)
{
  static bool glslang_initialized = false;
  if (!glslang_initialized)
  {
    if (!glslang::InitializeProcess())
    {
      Panic("Failed to initialize glslang shader compiler");
      return std::nullopt;
    }

    std::atexit(&glslang::FinalizeProcess);
    glslang_initialized = true;
  }

  std::unique_ptr<glslang::TShader> shader = std::make_unique<glslang::TShader>(stage);
  std::unique_ptr<glslang::TProgram> program;
  glslang::TShader::ForbidIncluder includer;
  const EProfile profile = ECoreProfile;
  const EShMessages messages =
    static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules | ((options & VulkanRules) ? EShMsgVulkanRules : 0) |
                             ((options & DebugInfo) ? EShMsgDebugInfo : 0));
  const int default_version = 450;

  std::string full_source_code;
  const char* pass_source_code = source.data();
  int pass_source_code_length = static_cast<int>(source.size());
  shader->setStringsWithLengths(&pass_source_code, &pass_source_code_length, 1);

  auto DumpBadShader = [&](const char* msg) {
    const std::string filename =
      Path::Combine(EmuFolders::DataRoot, fmt::format("bad_shader_{}.txt", s_next_bad_shader_id++));
    Log_ErrorPrintf("CompileShaderToSPV: %s, writing to %s", msg, filename.c_str());

    auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "wb");
    if (fp)
    {
      std::fwrite(source.data(), source.size(), 1, fp.get());
      std::fprintf(fp.get(), "\n\n%s\n", msg);
      std::fprintf(fp.get(), "Shader Info Log:\n%s\n%s\n", shader->getInfoLog(), shader->getInfoDebugLog());
      if (program)
        std::fprintf(fp.get(), "Program Info Log:%s\n%s\n", program->getInfoLog(), program->getInfoDebugLog());
    }
  };

  if (!shader->parse(&glslang::DefaultTBuiltInResource, default_version, profile, false, true, messages, includer))
  {
    DumpBadShader("Failed to parse shader");
    return std::nullopt;
  }

  // Even though there's only a single shader, we still need to link it to generate SPV
  program = std::make_unique<glslang::TProgram>();
  program->addShader(shader.get());
  if (!program->link(messages))
  {
    DumpBadShader("Failed to link program");
    return std::nullopt;
  }

  glslang::TIntermediate* intermediate = program->getIntermediate(static_cast<EShLanguage>(stage));
  if (!intermediate)
  {
    DumpBadShader("Failed to generate SPIR-V");
    return std::nullopt;
  }

  SPIRVCodeVector out_code;
  spv::SpvBuildLogger logger;
  glslang::SpvOptions spvoptions;
  spvoptions.generateDebugInfo = (options & DebugInfo) != 0;
  glslang::GlslangToSpv(*intermediate, out_code, &logger, &spvoptions);

  // Write out messages
  if (std::strlen(shader->getInfoLog()) > 0)
    Log_WarningPrintf("Shader info log: %s", shader->getInfoLog());
  if (std::strlen(shader->getInfoDebugLog()) > 0)
    Log_WarningPrintf("Shader debug info log: %s", shader->getInfoDebugLog());
  if (std::strlen(program->getInfoLog()) > 0)
    Log_WarningPrintf("Program info log: %s", program->getInfoLog());
  if (std::strlen(program->getInfoDebugLog()) > 0)
    Log_WarningPrintf("Program debug info log: %s", program->getInfoDebugLog());
  std::string spv_messages = logger.getAllMessages();
  if (!spv_messages.empty())
    Log_WarningPrintf("SPIR-V conversion messages: %s", spv_messages.c_str());

  return out_code;
}

std::optional<SPIRVCompiler::SPIRVCodeVector> SPIRVCompiler::CompileVertexShader(std::string_view source_code,
                                                                                 u32 options)
{
  return CompileShaderToSPV(EShLangVertex, "vs", source_code, options);
}

std::optional<SPIRVCompiler::SPIRVCodeVector> SPIRVCompiler::CompileFragmentShader(std::string_view source_code,
                                                                                   u32 options)
{
  return CompileShaderToSPV(EShLangFragment, "ps", source_code, options);
}

std::optional<SPIRVCompiler::SPIRVCodeVector> SPIRVCompiler::CompileGeometryShader(std::string_view source_code,
                                                                                   u32 options)
{
  return CompileShaderToSPV(EShLangGeometry, "gs", source_code, options);
}

std::optional<SPIRVCompiler::SPIRVCodeVector> SPIRVCompiler::CompileComputeShader(std::string_view source_code,
                                                                                  u32 options)
{
  return CompileShaderToSPV(EShLangCompute, "cs", source_code, options);
}

std::optional<SPIRVCompiler::SPIRVCodeVector> SPIRVCompiler::CompileShader(GPUShaderStage type,
                                                                           std::string_view source_code, u32 options)
{
  switch (type)
  {
    case GPUShaderStage::Vertex:
      return CompileShaderToSPV(EShLangVertex, "vs", source_code, options);

    case GPUShaderStage::Fragment:
      return CompileShaderToSPV(EShLangFragment, "ps", source_code, options);

    case GPUShaderStage::Geometry:
      return CompileShaderToSPV(EShLangGeometry, "gs", source_code, options);

    case GPUShaderStage::Compute:
      return CompileShaderToSPV(EShLangCompute, "cs", source_code, options);

    default:
      return std::nullopt;
  }
}

#ifdef __APPLE__

std::optional<std::string> SPIRVCompiler::CompileSPIRVToMSL(GPUShaderStage stage, std::span<const SPIRVCodeType> spv)
{
  spirv_cross::CompilerMSL compiler(spv.data(), spv.size());

  spirv_cross::CompilerMSL::Options options = compiler.get_msl_options();
  options.pad_fragment_output_components = true;

  if (stage == GPUShaderStage::Fragment)
  {
    for (u32 i = 0; i < GPUDevice::MAX_TEXTURE_SAMPLERS; i++)
    {
      spirv_cross::MSLResourceBinding rb;
      rb.stage = spv::ExecutionModelFragment;
      rb.desc_set = 1;
      rb.binding = i;
      rb.count = 1;
      rb.msl_texture = i;
      rb.msl_sampler = i;
      rb.msl_buffer = i;
      compiler.add_msl_resource_binding(rb);
    }
  }

  compiler.set_msl_options(options);

  std::string msl = compiler.compile();
  return (msl.empty()) ? std::optional<std::string>() : std::optional<std::string>(std::move(msl));
}

#endif
