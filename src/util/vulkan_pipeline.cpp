// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "vulkan_pipeline.h"
#include "spirv_compiler.h"
#include "vulkan_builders.h"
#include "vulkan_device.h"

#include "common/assert.h"
#include "common/log.h"

Log_SetChannel(VulkanDevice);

VulkanShader::VulkanShader(GPUShaderStage stage, VkShaderModule mod) : GPUShader(stage), m_module(mod)
{
}

VulkanShader::~VulkanShader()
{
  vkDestroyShaderModule(VulkanDevice::GetInstance().GetVulkanDevice(), m_module, nullptr);
}

void VulkanShader::SetDebugName(const std::string_view& name)
{
  Vulkan::SetObjectName(VulkanDevice::GetInstance().GetVulkanDevice(), m_module, name);
}

std::unique_ptr<GPUShader> VulkanDevice::CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data)
{
  VkShaderModule mod;

  const VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, data.size(),
                                       reinterpret_cast<const u32*>(data.data())};
  VkResult res = vkCreateShaderModule(m_device, &ci, nullptr, &mod);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateShaderModule() failed: ");
    return {};
  }

  return std::unique_ptr<GPUShader>(new VulkanShader(stage, mod));
}

std::unique_ptr<GPUShader> VulkanDevice::CreateShaderFromSource(GPUShaderStage stage, const std::string_view& source,
                                                                const char* entry_point,
                                                                DynamicHeapArray<u8>* out_binary)
{
  if (std::strcmp(entry_point, "main") != 0)
  {
    Log_ErrorPrintf("Entry point must be 'main', but got '%s' instead.", entry_point);
    return {};
  }

  const u32 options = (m_debug_device ? SPIRVCompiler::DebugInfo : 0) | SPIRVCompiler::VulkanRules;

  std::optional<SPIRVCompiler::SPIRVCodeVector> spirv = SPIRVCompiler::CompileShader(stage, source, options);
  if (!spirv.has_value())
  {
    Log_ErrorPrintf("Failed to compile shader to SPIR-V.");
    return {};
  }

  const size_t spirv_size = spirv->size() * sizeof(SPIRVCompiler::SPIRVCodeType);
  if (out_binary)
  {
    out_binary->resize(spirv_size);
    std::memcpy(out_binary->data(), spirv->data(), spirv_size);
  }

  return CreateShaderFromBinary(stage, std::span<const u8>(reinterpret_cast<const u8*>(spirv->data()), spirv_size));
}

//////////////////////////////////////////////////////////////////////////

VulkanPipeline::VulkanPipeline(VkPipeline pipeline, Layout layout)
  : GPUPipeline(), m_pipeline(pipeline), m_layout(layout)
{
}

VulkanPipeline::~VulkanPipeline()
{
  VulkanDevice::GetInstance().DeferPipelineDestruction(m_pipeline);
}

void VulkanPipeline::SetDebugName(const std::string_view& name)
{
  Vulkan::SetObjectName(VulkanDevice::GetInstance().GetVulkanDevice(), m_pipeline, name);
}

std::unique_ptr<GPUPipeline> VulkanDevice::CreatePipeline(const GPUPipeline::GraphicsConfig& config)
{
  static constexpr std::array<VkPrimitiveTopology, static_cast<u32>(GPUPipeline::Primitive::MaxCount)> primitives = {{
    VK_PRIMITIVE_TOPOLOGY_POINT_LIST,     // Points
    VK_PRIMITIVE_TOPOLOGY_LINE_LIST,      // Lines
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,  // Triangles
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, // TriangleStrips
  }};

  static constexpr u32 MAX_COMPONENTS = 4;
  static constexpr const VkFormat format_mapping[static_cast<u8>(
    GPUPipeline::VertexAttribute::Type::MaxCount)][MAX_COMPONENTS] = {
    {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}, // Float
    {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT},                   // UInt8
    {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT},                   // SInt8
    {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},           // UNorm8
    {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT},     // UInt16
    {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT},     // SInt16
    {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM}, // UNorm16
    {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT},     // UInt32
    {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT},     // SInt32
  };

  static constexpr std::array<VkCullModeFlagBits, static_cast<u32>(GPUPipeline::CullMode::MaxCount)> cull_mapping = {{
    VK_CULL_MODE_NONE,      // None
    VK_CULL_MODE_FRONT_BIT, // Front
    VK_CULL_MODE_BACK_BIT,  // Back
  }};

  static constexpr std::array<VkCompareOp, static_cast<u32>(GPUPipeline::DepthFunc::MaxCount)> compare_mapping = {{
    VK_COMPARE_OP_NEVER,            // Never
    VK_COMPARE_OP_ALWAYS,           // Always
    VK_COMPARE_OP_LESS,             // Less
    VK_COMPARE_OP_LESS_OR_EQUAL,    // LessEqual
    VK_COMPARE_OP_GREATER,          // Greater
    VK_COMPARE_OP_GREATER_OR_EQUAL, // GreaterEqual
    VK_COMPARE_OP_EQUAL,            // Equal
  }};

  static constexpr std::array<VkBlendFactor, static_cast<u32>(GPUPipeline::BlendFunc::MaxCount)> blend_mapping = {{
    VK_BLEND_FACTOR_ZERO,                     // Zero
    VK_BLEND_FACTOR_ONE,                      // One
    VK_BLEND_FACTOR_SRC_COLOR,                // SrcColor
    VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,      // InvSrcColor
    VK_BLEND_FACTOR_DST_COLOR,                // DstColor
    VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,      // InvDstColor
    VK_BLEND_FACTOR_SRC_ALPHA,                // SrcAlpha
    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,      // InvSrcAlpha
    VK_BLEND_FACTOR_SRC1_ALPHA,               // SrcAlpha1
    VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,     // InvSrcAlpha1
    VK_BLEND_FACTOR_DST_ALPHA,                // DstAlpha
    VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,      // InvDstAlpha
    VK_BLEND_FACTOR_CONSTANT_COLOR,           // ConstantAlpha
    VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, // InvConstantAlpha
  }};

  static constexpr std::array<VkBlendOp, static_cast<u32>(GPUPipeline::BlendOp::MaxCount)> op_mapping = {{
    VK_BLEND_OP_ADD,              // Add
    VK_BLEND_OP_SUBTRACT,         // Subtract
    VK_BLEND_OP_REVERSE_SUBTRACT, // ReverseSubtract
    VK_BLEND_OP_MIN,              // Min
    VK_BLEND_OP_MAX,              // Max
  }};

  Vulkan::GraphicsPipelineBuilder gpb;
  gpb.SetVertexShader(static_cast<const VulkanShader*>(config.vertex_shader)->GetModule());
  gpb.SetFragmentShader(static_cast<const VulkanShader*>(config.fragment_shader)->GetModule());

  if (config.geometry_shader)
    gpb.SetGeometryShader(static_cast<const VulkanShader*>(config.geometry_shader)->GetModule());

  if (!config.input_layout.vertex_attributes.empty())
  {
    gpb.AddVertexBuffer(0, config.input_layout.vertex_stride);
    for (u32 i = 0; i < static_cast<u32>(config.input_layout.vertex_attributes.size()); i++)
    {
      const GPUPipeline::VertexAttribute& va = config.input_layout.vertex_attributes[i];
      DebugAssert(va.components > 0 && va.components <= MAX_COMPONENTS);
      gpb.AddVertexAttribute(
        i, 0, format_mapping[static_cast<u8>(va.type.GetValue())][static_cast<u8>(va.components.GetValue() - 1)],
        va.offset);
    }
  }

  gpb.SetPrimitiveTopology(primitives[static_cast<u8>(config.primitive)]);

  // Line width?

  gpb.SetRasterizationState(VK_POLYGON_MODE_FILL,
                            cull_mapping[static_cast<u8>(config.rasterization.cull_mode.GetValue())],
                            VK_FRONT_FACE_CLOCKWISE);
  if (config.samples > 1)
    gpb.SetMultisamples(config.samples, config.per_sample_shading);
  gpb.SetDepthState(config.depth.depth_test != GPUPipeline::DepthFunc::Always || config.depth.depth_write,
                    config.depth.depth_write, compare_mapping[static_cast<u8>(config.depth.depth_test.GetValue())]);
  gpb.SetNoStencilState();

  for (u32 i = 0; i < MAX_RENDER_TARGETS; i++)
  {
    if (config.color_formats[i] == GPUTexture::Format::Unknown)
      break;

    gpb.SetBlendAttachment(i, config.blend.enable, blend_mapping[static_cast<u8>(config.blend.src_blend.GetValue())],
                           blend_mapping[static_cast<u8>(config.blend.dst_blend.GetValue())],
                           op_mapping[static_cast<u8>(config.blend.blend_op.GetValue())],
                           blend_mapping[static_cast<u8>(config.blend.src_alpha_blend.GetValue())],
                           blend_mapping[static_cast<u8>(config.blend.dst_alpha_blend.GetValue())],
                           op_mapping[static_cast<u8>(config.blend.alpha_blend_op.GetValue())],
                           config.blend.write_mask);
  }

  const auto blend_constants = config.blend.GetConstantFloatColor();
  gpb.SetBlendConstants(blend_constants[0], blend_constants[1], blend_constants[2], blend_constants[3]);

  gpb.AddDynamicState(VK_DYNAMIC_STATE_VIEWPORT);
  gpb.AddDynamicState(VK_DYNAMIC_STATE_SCISSOR);

  gpb.SetPipelineLayout(m_pipeline_layouts[static_cast<u8>(config.layout)]);

  if (m_optional_extensions.vk_khr_dynamic_rendering)
  {
    gpb.SetDynamicRendering();

    for (u32 i = 0; i < MAX_RENDER_TARGETS; i++)
    {
      if (config.color_formats[i] == GPUTexture::Format::Unknown)
        break;

      gpb.AddDynamicRenderingColorAttachment(
        VulkanDevice::TEXTURE_FORMAT_MAPPING[static_cast<u8>(config.color_formats[i])]);
    }

    if (config.depth_format != GPUTexture::Format::Unknown)
    {
      gpb.SetDynamicRenderingDepthAttachment(VulkanDevice::TEXTURE_FORMAT_MAPPING[static_cast<u8>(config.depth_format)],
                                             VK_FORMAT_UNDEFINED);
    }
  }
  else
  {
    const VkRenderPass render_pass = GetRenderPass(config);
    DebugAssert(render_pass != VK_NULL_HANDLE);
    gpb.SetRenderPass(render_pass, 0);
  }

  const VkPipeline pipeline = gpb.Create(m_device, m_pipeline_cache, false);
  if (!pipeline)
    return {};

  return std::unique_ptr<GPUPipeline>(new VulkanPipeline(pipeline, config.layout));
}
