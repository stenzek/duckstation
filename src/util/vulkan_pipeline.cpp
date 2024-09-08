// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "vulkan_pipeline.h"
#include "vulkan_builders.h"
#include "vulkan_device.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/heap_array.h"
#include "common/log.h"

Log_SetChannel(VulkanDevice);

VulkanShader::VulkanShader(GPUShaderStage stage, VkShaderModule mod) : GPUShader(stage), m_module(mod)
{
}

VulkanShader::~VulkanShader()
{
  vkDestroyShaderModule(VulkanDevice::GetInstance().GetVulkanDevice(), m_module, nullptr);
}

void VulkanShader::SetDebugName(std::string_view name)
{
  Vulkan::SetObjectName(VulkanDevice::GetInstance().GetVulkanDevice(), m_module, name);
}

std::unique_ptr<GPUShader> VulkanDevice::CreateShaderFromBinary(GPUShaderStage stage, std::span<const u8> data,
                                                                Error* error)
{
  VkShaderModule mod;

  // In the very rare event that the pointer isn't word-aligned...
  DynamicHeapArray<u32> data_copy;
  if (Common::IsAlignedPow2(reinterpret_cast<uintptr_t>(data.data()), 4)) [[unlikely]]
  {
    data_copy.resize((data.size() + (sizeof(u32) - 1)) / sizeof(u32));
    std::memcpy(data_copy.data(), data.data(), data.size());
    data = std::span<const u8>(reinterpret_cast<const u8*>(data_copy.data()), data.size());
  }

  const VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, data.size(),
                                       reinterpret_cast<const u32*>(data.data())};
  VkResult res = vkCreateShaderModule(m_device, &ci, nullptr, &mod);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateShaderModule() failed: ");
    Error::SetStringFmt(error, "vkCreateShaderModule() failed: {}", Vulkan::VkResultToString(res));
    return {};
  }

  return std::unique_ptr<GPUShader>(new VulkanShader(stage, mod));
}

std::unique_ptr<GPUShader> VulkanDevice::CreateShaderFromSource(GPUShaderStage stage, GPUShaderLanguage language,
                                                                std::string_view source, const char* entry_point,
                                                                DynamicHeapArray<u8>* out_binary, Error* error)
{
  if (language == GPUShaderLanguage::SPV)
  {
    // Optimize the SPIR-V if we're not using a debug device.
    std::optional<DynamicHeapArray<u8>> optimized_spv;
    if (!m_debug_device)
    {
      Error optimize_error;
      optimized_spv = GPUDevice::OptimizeVulkanSpv(
        std::span<const u8>(reinterpret_cast<const u8*>(source.data()), source.size()), &optimize_error);
      if (!optimized_spv.has_value())
        WARNING_LOG("Failed to optimize SPIR-V: {}", optimize_error.GetDescription());
      else
        source = std::string_view(reinterpret_cast<const char*>(optimized_spv->data()), optimized_spv->size());
    }

    if (out_binary)
      out_binary->assign(reinterpret_cast<const u8*>(source.data()), source.length());

    return CreateShaderFromBinary(
      stage, std::span<const u8>(reinterpret_cast<const u8*>(source.data()), source.length()), error);
  }

  DynamicHeapArray<u8> local_binary;
  DynamicHeapArray<u8>* dest_binary = out_binary ? out_binary : &local_binary;
  if (!CompileGLSLShaderToVulkanSpv(stage, language, source, entry_point, !m_debug_device,
                                    m_optional_extensions.vk_khr_shader_non_semantic_info, dest_binary, error))
  {
    return {};
  }

  AssertMsg((dest_binary->size() % 4) == 0, "Compile result should be 4 byte aligned.");

  return CreateShaderFromBinary(stage, dest_binary->cspan(), error);
}

//////////////////////////////////////////////////////////////////////////

VulkanPipeline::VulkanPipeline(VkPipeline pipeline, Layout layout, u8 vertices_per_primitive,
                               RenderPassFlag render_pass_flags)
  : GPUPipeline(), m_pipeline(pipeline), m_layout(layout), m_vertices_per_primitive(vertices_per_primitive),
    m_render_pass_flags(render_pass_flags)
{
}

VulkanPipeline::~VulkanPipeline()
{
  VulkanDevice::GetInstance().DeferPipelineDestruction(m_pipeline);
}

void VulkanPipeline::SetDebugName(std::string_view name)
{
  Vulkan::SetObjectName(VulkanDevice::GetInstance().GetVulkanDevice(), m_pipeline, name);
}

std::unique_ptr<GPUPipeline> VulkanDevice::CreatePipeline(const GPUPipeline::GraphicsConfig& config, Error* error)
{
  static constexpr std::array<std::pair<VkPrimitiveTopology, u32>, static_cast<u32>(GPUPipeline::Primitive::MaxCount)>
    primitives = {{
      {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 1},     // Points
      {VK_PRIMITIVE_TOPOLOGY_LINE_LIST, 2},      // Lines
      {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 3},  // Triangles
      {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 3}, // TriangleStrips
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

  const auto [vk_topology, vertices_per_primitive] = primitives[static_cast<u8>(config.primitive)];
  gpb.SetPrimitiveTopology(vk_topology);

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

  gpb.SetPipelineLayout(m_pipeline_layouts[static_cast<size_t>(GetPipelineLayoutType(config.render_pass_flags))]
                                          [static_cast<size_t>(config.layout)]);

  if (m_optional_extensions.vk_khr_dynamic_rendering && (m_optional_extensions.vk_khr_dynamic_rendering_local_read ||
                                                         !(config.render_pass_flags & GPUPipeline::ColorFeedbackLoop)))
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

    if (config.render_pass_flags & GPUPipeline::ColorFeedbackLoop)
    {
      DebugAssert(m_optional_extensions.vk_khr_dynamic_rendering_local_read &&
                  config.color_formats[0] != GPUTexture::Format::Unknown);
      gpb.AddDynamicRenderingInputAttachment(0);
    }
  }
  else
  {
    const VkRenderPass render_pass = GetRenderPass(config);
    DebugAssert(render_pass != VK_NULL_HANDLE);
    gpb.SetRenderPass(render_pass, 0);
  }

  const VkPipeline pipeline = gpb.Create(m_device, m_pipeline_cache, false, error);
  if (!pipeline)
    return {};

  return std::unique_ptr<GPUPipeline>(
    new VulkanPipeline(pipeline, config.layout, static_cast<u8>(vertices_per_primitive), config.render_pass_flags));
}
