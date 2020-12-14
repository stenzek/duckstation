#include "gpu_hw_vulkan.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/scope_guard.h"
#include "common/timer.h"
#include "common/vulkan/builders.h"
#include "common/vulkan/context.h"
#include "common/vulkan/shader_cache.h"
#include "common/vulkan/util.h"
#include "gpu_hw_shadergen.h"
#include "host_display.h"
#include "host_interface.h"
#include "system.h"
Log_SetChannel(GPU_HW_Vulkan);

GPU_HW_Vulkan::GPU_HW_Vulkan() = default;

GPU_HW_Vulkan::~GPU_HW_Vulkan()
{
  if (m_host_display)
  {
    m_host_display->ClearDisplayTexture();
    ResetGraphicsAPIState();
  }

  DestroyResources();
}

bool GPU_HW_Vulkan::Initialize(HostDisplay* host_display)
{
  if (host_display->GetRenderAPI() != HostDisplay::RenderAPI::Vulkan)
  {
    Log_ErrorPrintf("Host render API is incompatible");
    return false;
  }

  Assert(g_vulkan_shader_cache);
  SetCapabilities();

  if (!GPU_HW::Initialize(host_display))
    return false;

  if (!CreatePipelineLayouts())
  {
    Log_ErrorPrintf("Failed to create pipeline layouts");
    return false;
  }

  if (!CreateSamplers())
  {
    Log_ErrorPrintf("Failed to create samplers");
    return false;
  }

  if (!CreateVertexBuffer())
  {
    Log_ErrorPrintf("Failed to create vertex buffer");
    return false;
  }

  if (!CreateUniformBuffer())
  {
    Log_ErrorPrintf("Failed to create uniform buffer");
    return false;
  }

  if (!CreateTextureBuffer())
  {
    Log_ErrorPrintf("Failed to create texture buffer");
    return false;
  }

  if (!CreateFramebuffer())
  {
    Log_ErrorPrintf("Failed to create framebuffer");
    return false;
  }

  if (!CompilePipelines())
  {
    Log_ErrorPrintf("Failed to compile pipelines");
    return false;
  }

  UpdateDepthBufferFromMaskBit();
  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_Vulkan::Reset()
{
  GPU_HW::Reset();

  EndRenderPass();
  ClearFramebuffer();
}

void GPU_HW_Vulkan::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();

  EndRenderPass();
}

void GPU_HW_Vulkan::RestoreGraphicsAPIState()
{
  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkDeviceSize vertex_buffer_offset = 0;
  vkCmdBindVertexBuffers(cmdbuf, 0, 1, m_vertex_stream_buffer.GetBufferPointer(), &vertex_buffer_offset);
  Vulkan::Util::SetViewport(cmdbuf, 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
  vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_batch_pipeline_layout, 0, 1,
                          &m_batch_descriptor_set, 1, &m_current_uniform_buffer_offset);
  SetScissorFromDrawingArea();
}

void GPU_HW_Vulkan::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  bool framebuffer_changed, shaders_changed;
  UpdateHWSettings(&framebuffer_changed, &shaders_changed);

  if (framebuffer_changed)
  {
    RestoreGraphicsAPIState();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    ResetGraphicsAPIState();
  }

  if (framebuffer_changed)
    CreateFramebuffer();

  // Everything should be finished executing before recreating resources.
  m_host_display->ClearDisplayTexture();
  g_vulkan_context->ExecuteCommandBuffer(true);

  if (shaders_changed)
  {
    // clear it since we draw a loading screen and it's not in the correct state
    DestroyPipelines();
    CompilePipelines();
  }

  // this has to be done here, because otherwise we're using destroyed pipelines in the same cmdbuffer
  if (framebuffer_changed)
  {
    RestoreGraphicsAPIState();
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_ptr, false, false);
    UpdateDepthBufferFromMaskBit();
    UpdateDisplay();
    ResetGraphicsAPIState();
  }
}

void GPU_HW_Vulkan::MapBatchVertexPointer(u32 required_vertices)
{
  DebugAssert(!m_batch_start_vertex_ptr);

  const u32 required_space = required_vertices * sizeof(BatchVertex);
  if (!m_vertex_stream_buffer.ReserveMemory(required_space, sizeof(BatchVertex)))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes in vertex stream buffer", required_space);
    EndRenderPass();
    g_vulkan_context->ExecuteCommandBuffer(false);
    RestoreGraphicsAPIState();
    if (!m_vertex_stream_buffer.ReserveMemory(required_space, sizeof(BatchVertex)))
      Panic("Failed to reserve vertex stream buffer memory");
  }

  m_batch_start_vertex_ptr = static_cast<BatchVertex*>(m_vertex_stream_buffer.GetCurrentHostPointer());
  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
  m_batch_end_vertex_ptr = m_batch_start_vertex_ptr + (m_vertex_stream_buffer.GetCurrentSpace() / sizeof(BatchVertex));
  m_batch_base_vertex = m_vertex_stream_buffer.GetCurrentOffset() / sizeof(BatchVertex);
}

void GPU_HW_Vulkan::UnmapBatchVertexPointer(u32 used_vertices)
{
  DebugAssert(m_batch_start_vertex_ptr);
  if (used_vertices > 0)
    m_vertex_stream_buffer.CommitMemory(used_vertices * sizeof(BatchVertex));

  m_batch_start_vertex_ptr = nullptr;
  m_batch_end_vertex_ptr = nullptr;
  m_batch_current_vertex_ptr = nullptr;
}

void GPU_HW_Vulkan::UploadUniformBuffer(const void* data, u32 data_size)
{
  const u32 alignment = static_cast<u32>(g_vulkan_context->GetUniformBufferAlignment());
  if (!m_uniform_stream_buffer.ReserveMemory(data_size, alignment))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes in uniform stream buffer", data_size);
    EndRenderPass();
    g_vulkan_context->ExecuteCommandBuffer(false);
    RestoreGraphicsAPIState();
    if (!m_uniform_stream_buffer.ReserveMemory(data_size, alignment))
      Panic("Failed to reserve uniform stream buffer memory");
  }

  m_current_uniform_buffer_offset = m_uniform_stream_buffer.GetCurrentOffset();
  std::memcpy(m_uniform_stream_buffer.GetCurrentHostPointer(), data, data_size);
  m_uniform_stream_buffer.CommitMemory(data_size);

  vkCmdBindDescriptorSets(g_vulkan_context->GetCurrentCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_batch_pipeline_layout, 0, 1, &m_batch_descriptor_set, 1, &m_current_uniform_buffer_offset);
}

void GPU_HW_Vulkan::SetCapabilities()
{
  const u32 max_texture_size = g_vulkan_context->GetDeviceLimits().maxImageDimension2D;
  const u32 max_texture_scale = max_texture_size / VRAM_WIDTH;
  Log_InfoPrintf("Max texture size: %ux%u", max_texture_size, max_texture_size);
  m_max_resolution_scale = max_texture_scale;

  VkImageFormatProperties color_properties = {};
  vkGetPhysicalDeviceImageFormatProperties(g_vulkan_context->GetPhysicalDevice(), VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &color_properties);
  VkImageFormatProperties depth_properties = {};
  vkGetPhysicalDeviceImageFormatProperties(g_vulkan_context->GetPhysicalDevice(), VK_FORMAT_D32_SFLOAT,
                                           VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &depth_properties);
  const VkSampleCountFlags combined_properties =
    g_vulkan_context->GetDeviceProperties().limits.framebufferColorSampleCounts &
    g_vulkan_context->GetDeviceProperties().limits.framebufferDepthSampleCounts & color_properties.sampleCounts &
    depth_properties.sampleCounts;
  if (combined_properties & VK_SAMPLE_COUNT_64_BIT)
    m_max_multisamples = 64;
  else if (combined_properties & VK_SAMPLE_COUNT_32_BIT)
    m_max_multisamples = 32;
  else if (combined_properties & VK_SAMPLE_COUNT_16_BIT)
    m_max_multisamples = 16;
  else if (combined_properties & VK_SAMPLE_COUNT_8_BIT)
    m_max_multisamples = 8;
  else if (combined_properties & VK_SAMPLE_COUNT_4_BIT)
    m_max_multisamples = 4;
  else if (combined_properties & VK_SAMPLE_COUNT_2_BIT)
    m_max_multisamples = 2;
  else
    m_max_multisamples = 1;

  m_supports_dual_source_blend = g_vulkan_context->GetDeviceFeatures().dualSrcBlend;
  m_supports_per_sample_shading = g_vulkan_context->GetDeviceFeatures().sampleRateShading;
  Log_InfoPrintf("Dual-source blend: %s", m_supports_dual_source_blend ? "supported" : "not supported");
  Log_InfoPrintf("Per-sample shading: %s", m_supports_per_sample_shading ? "supported" : "not supported");
  Log_InfoPrintf("Max multisamples: %u", m_max_multisamples);

#ifdef __APPLE__
  // Partial texture buffer uploads appear to be broken in macOS/MoltenVK.
  m_use_ssbos_for_vram_writes = true;
#else
  const u32 max_texel_buffer_elements = g_vulkan_context->GetDeviceLimits().maxTexelBufferElements;
  Log_InfoPrintf("Max texel buffer elements: %u", max_texel_buffer_elements);
  if (max_texel_buffer_elements < (VRAM_WIDTH * VRAM_HEIGHT))
  {
    Log_WarningPrintf("Texel buffer elements insufficient, using shader storage buffers instead.");
    m_use_ssbos_for_vram_writes = true;
  }
#endif
}

void GPU_HW_Vulkan::DestroyResources()
{
  // Everything should be finished executing before recreating resources.
  if (g_vulkan_context)
    g_vulkan_context->ExecuteCommandBuffer(true);

  DestroyFramebuffer();
  DestroyPipelines();

  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_vram_write_descriptor_set);
  Vulkan::Util::SafeDestroyBufferView(m_texture_stream_buffer_view);

  m_vertex_stream_buffer.Destroy(false);
  m_uniform_stream_buffer.Destroy(false);
  m_texture_stream_buffer.Destroy(false);

  Vulkan::Util::SafeDestroyPipelineLayout(m_vram_write_pipeline_layout);
  Vulkan::Util::SafeDestroyPipelineLayout(m_single_sampler_pipeline_layout);
  Vulkan::Util::SafeDestroyPipelineLayout(m_no_samplers_pipeline_layout);
  Vulkan::Util::SafeDestroyPipelineLayout(m_batch_pipeline_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_vram_write_descriptor_set_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_single_sampler_descriptor_set_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_batch_descriptor_set_layout);
  Vulkan::Util::SafeDestroySampler(m_point_sampler);
  Vulkan::Util::SafeDestroySampler(m_linear_sampler);
}

void GPU_HW_Vulkan::BeginRenderPass(VkRenderPass render_pass, VkFramebuffer framebuffer, u32 x, u32 y, u32 width,
                                    u32 height)
{
  DebugAssert(m_current_render_pass == VK_NULL_HANDLE);

  const VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                    nullptr,
                                    render_pass,
                                    framebuffer,
                                    {{static_cast<s32>(x), static_cast<s32>(y)}, {width, height}},
                                    0u,
                                    nullptr};
  vkCmdBeginRenderPass(g_vulkan_context->GetCurrentCommandBuffer(), &bi, VK_SUBPASS_CONTENTS_INLINE);
  m_current_render_pass = render_pass;
}

void GPU_HW_Vulkan::BeginVRAMRenderPass()
{
  if (m_current_render_pass == m_vram_render_pass)
    return;

  EndRenderPass();
  BeginRenderPass(m_vram_render_pass, m_vram_framebuffer, 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
}

void GPU_HW_Vulkan::EndRenderPass()
{
  if (m_current_render_pass == VK_NULL_HANDLE)
    return;

  vkCmdEndRenderPass(g_vulkan_context->GetCurrentCommandBuffer());
  m_current_render_pass = VK_NULL_HANDLE;
}

bool GPU_HW_Vulkan::CreatePipelineLayouts()
{
  VkDevice device = g_vulkan_context->GetDevice();

  Vulkan::DescriptorSetLayoutBuilder dslbuilder;
  dslbuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
  dslbuilder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_batch_descriptor_set_layout = dslbuilder.Create(device);
  if (m_batch_descriptor_set_layout == VK_NULL_HANDLE)
    return false;

  // textures start at 1
  dslbuilder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_single_sampler_descriptor_set_layout = dslbuilder.Create(device);
  if (m_single_sampler_descriptor_set_layout == VK_NULL_HANDLE)
    return false;

  if (m_use_ssbos_for_vram_writes)
    dslbuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  else
    dslbuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_vram_write_descriptor_set_layout = dslbuilder.Create(device);
  if (m_vram_write_descriptor_set_layout == VK_NULL_HANDLE)
    return false;

  Vulkan::PipelineLayoutBuilder plbuilder;
  plbuilder.AddDescriptorSet(m_batch_descriptor_set_layout);
  m_batch_pipeline_layout = plbuilder.Create(device);
  if (m_batch_pipeline_layout == VK_NULL_HANDLE)
    return false;

  plbuilder.AddDescriptorSet(m_single_sampler_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_single_sampler_pipeline_layout = plbuilder.Create(device);
  if (m_single_sampler_pipeline_layout == VK_NULL_HANDLE)
    return false;

  plbuilder.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_no_samplers_pipeline_layout = plbuilder.Create(device);
  if (m_no_samplers_pipeline_layout == VK_NULL_HANDLE)
    return false;

  plbuilder.AddDescriptorSet(m_vram_write_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_vram_write_pipeline_layout = plbuilder.Create(device);
  if (m_vram_write_pipeline_layout == VK_NULL_HANDLE)
    return false;

  return true;
}

bool GPU_HW_Vulkan::CreateSamplers()
{
  VkDevice device = g_vulkan_context->GetDevice();

  Vulkan::SamplerBuilder sbuilder;
  sbuilder.SetPointSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  sbuilder.SetAddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                          VK_SAMPLER_ADDRESS_MODE_REPEAT);
  m_point_sampler = sbuilder.Create(device);
  if (m_point_sampler == VK_NULL_HANDLE)
    return false;

  sbuilder.SetLinearSampler(false, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  sbuilder.SetAddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                          VK_SAMPLER_ADDRESS_MODE_REPEAT);
  m_linear_sampler = sbuilder.Create(device);
  if (m_linear_sampler == VK_NULL_HANDLE)
    return false;

  return true;
}

bool GPU_HW_Vulkan::CreateFramebuffer()
{
  DestroyFramebuffer();

  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;
  const VkFormat texture_format = VK_FORMAT_R8G8B8A8_UNORM;
  const VkFormat depth_format = VK_FORMAT_D16_UNORM;
  const VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>(m_multisamples);

  if (!m_vram_texture.Create(texture_width, texture_height, 1, 1, texture_format, samples, VK_IMAGE_VIEW_TYPE_2D,
                             VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      !m_vram_depth_texture.Create(texture_width, texture_height, 1, 1, depth_format, samples, VK_IMAGE_VIEW_TYPE_2D,
                                   VK_IMAGE_TILING_OPTIMAL,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      !m_vram_read_texture.Create(texture_width, texture_height, 1, 1, texture_format, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      !m_display_texture.Create(texture_width, texture_height, 1, 1, texture_format, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      !m_vram_readback_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, 1, 1, texture_format, VK_SAMPLE_COUNT_1_BIT,
                                      VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      !m_vram_readback_staging_texture.Create(Vulkan::StagingBuffer::Type::Readback, texture_format, VRAM_WIDTH / 2,
                                              VRAM_HEIGHT))
  {
    return false;
  }

  m_vram_render_pass =
    g_vulkan_context->GetRenderPass(texture_format, depth_format, samples, VK_ATTACHMENT_LOAD_OP_LOAD);
  m_vram_update_depth_render_pass =
    g_vulkan_context->GetRenderPass(VK_FORMAT_UNDEFINED, depth_format, samples, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  m_display_render_pass = g_vulkan_context->GetRenderPass(m_display_texture.GetFormat(), VK_FORMAT_UNDEFINED,
                                                          m_display_texture.GetSamples(), VK_ATTACHMENT_LOAD_OP_LOAD);
  m_vram_readback_render_pass =
    g_vulkan_context->GetRenderPass(m_vram_readback_texture.GetFormat(), VK_FORMAT_UNDEFINED,
                                    m_vram_readback_texture.GetSamples(), VK_ATTACHMENT_LOAD_OP_DONT_CARE);

  if (m_vram_render_pass == VK_NULL_HANDLE || m_vram_update_depth_render_pass == VK_NULL_HANDLE ||
      m_display_render_pass == VK_NULL_HANDLE || m_vram_readback_render_pass == VK_NULL_HANDLE)
  {
    return false;
  }

  // vram framebuffer has both colour and depth
  {
    Vulkan::FramebufferBuilder fbb;
    fbb.AddAttachment(m_vram_texture.GetView());
    fbb.AddAttachment(m_vram_depth_texture.GetView());
    fbb.SetRenderPass(m_vram_render_pass);
    fbb.SetSize(m_vram_texture.GetWidth(), m_vram_texture.GetHeight(), m_vram_texture.GetLayers());
    m_vram_framebuffer = fbb.Create(g_vulkan_context->GetDevice());
    if (m_vram_framebuffer == VK_NULL_HANDLE)
      return false;
  }

  m_vram_update_depth_framebuffer = m_vram_depth_texture.CreateFramebuffer(m_vram_update_depth_render_pass);
  m_vram_readback_framebuffer = m_vram_readback_texture.CreateFramebuffer(m_vram_readback_render_pass);
  m_display_framebuffer = m_display_texture.CreateFramebuffer(m_display_render_pass);
  if (m_vram_update_depth_framebuffer == VK_NULL_HANDLE || m_vram_readback_framebuffer == VK_NULL_HANDLE ||
      m_display_framebuffer == VK_NULL_HANDLE)
  {
    return false;
  }

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  m_vram_depth_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  m_vram_read_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  Vulkan::DescriptorSetUpdateBuilder dsubuilder;

  m_batch_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_batch_descriptor_set_layout);
  m_vram_copy_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_single_sampler_descriptor_set_layout);
  m_vram_read_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_single_sampler_descriptor_set_layout);
  if (m_batch_descriptor_set == VK_NULL_HANDLE || m_vram_copy_descriptor_set == VK_NULL_HANDLE ||
      m_vram_read_descriptor_set == VK_NULL_HANDLE)
  {
    return false;
  }

  dsubuilder.AddBufferDescriptorWrite(m_batch_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                      m_uniform_stream_buffer.GetBuffer(), 0, sizeof(BatchUBOData));
  dsubuilder.AddCombinedImageSamplerDescriptorWrite(m_batch_descriptor_set, 1, m_vram_read_texture.GetView(),
                                                    m_point_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  dsubuilder.AddCombinedImageSamplerDescriptorWrite(m_vram_copy_descriptor_set, 1, m_vram_read_texture.GetView(),
                                                    m_point_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  dsubuilder.AddCombinedImageSamplerDescriptorWrite(m_vram_read_descriptor_set, 1, m_vram_texture.GetView(),
                                                    m_point_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  dsubuilder.Update(g_vulkan_context->GetDevice());

  ClearDisplay();
  SetFullVRAMDirtyRectangle();
  return true;
}

void GPU_HW_Vulkan::ClearFramebuffer()
{
  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  m_vram_depth_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  static constexpr VkClearColorValue cc = {};
  static constexpr VkImageSubresourceRange csrr = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  static constexpr VkClearDepthStencilValue cds = {};
  static constexpr VkImageSubresourceRange dsrr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u};
  vkCmdClearColorImage(cmdbuf, m_vram_texture.GetImage(), m_vram_texture.GetLayout(), &cc, 1u, &csrr);
  vkCmdClearDepthStencilImage(cmdbuf, m_vram_depth_texture.GetImage(), m_vram_depth_texture.GetLayout(), &cds, 1u,
                              &dsrr);

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  m_vram_depth_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

  SetFullVRAMDirtyRectangle();
}

void GPU_HW_Vulkan::DestroyFramebuffer()
{
  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_batch_descriptor_set);
  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_vram_copy_descriptor_set);
  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_vram_read_descriptor_set);

  Vulkan::Util::SafeDestroyFramebuffer(m_vram_framebuffer);
  Vulkan::Util::SafeDestroyFramebuffer(m_vram_update_depth_framebuffer);
  Vulkan::Util::SafeDestroyFramebuffer(m_vram_readback_framebuffer);
  Vulkan::Util::SafeDestroyFramebuffer(m_display_framebuffer);

  m_vram_read_texture.Destroy(false);
  m_vram_depth_texture.Destroy(false);
  m_vram_texture.Destroy(false);
  m_vram_readback_texture.Destroy(false);
  m_display_texture.Destroy(false);
  m_vram_readback_staging_texture.Destroy(false);
}

bool GPU_HW_Vulkan::CreateVertexBuffer()
{
  return m_vertex_stream_buffer.Create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VERTEX_BUFFER_SIZE);
}

bool GPU_HW_Vulkan::CreateUniformBuffer()
{
  return m_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, UNIFORM_BUFFER_SIZE);
}

bool GPU_HW_Vulkan::CreateTextureBuffer()
{
  if (m_use_ssbos_for_vram_writes)
  {
    if (!m_texture_stream_buffer.Create(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VRAM_UPDATE_TEXTURE_BUFFER_SIZE))
      return false;

    m_vram_write_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_vram_write_descriptor_set_layout);
    if (m_vram_write_descriptor_set == VK_NULL_HANDLE)
      return false;

    Vulkan::DescriptorSetUpdateBuilder dsubuilder;
    dsubuilder.AddBufferDescriptorWrite(m_vram_write_descriptor_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        m_texture_stream_buffer.GetBuffer(), 0,
                                        m_texture_stream_buffer.GetCurrentSize());
    dsubuilder.Update(g_vulkan_context->GetDevice());
    return true;
  }
  else
  {
    if (!m_texture_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VRAM_UPDATE_TEXTURE_BUFFER_SIZE))
      return false;

    Vulkan::BufferViewBuilder bvbuilder;
    bvbuilder.Set(m_texture_stream_buffer.GetBuffer(), VK_FORMAT_R16_UINT, 0, m_texture_stream_buffer.GetCurrentSize());
    m_texture_stream_buffer_view = bvbuilder.Create(g_vulkan_context->GetDevice());
    if (m_texture_stream_buffer_view == VK_NULL_HANDLE)
      return false;

    m_vram_write_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_vram_write_descriptor_set_layout);
    if (m_vram_write_descriptor_set == VK_NULL_HANDLE)
      return false;

    Vulkan::DescriptorSetUpdateBuilder dsubuilder;
    dsubuilder.AddBufferViewDescriptorWrite(m_vram_write_descriptor_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                            m_texture_stream_buffer_view);
    dsubuilder.Update(g_vulkan_context->GetDevice());
  }

  return true;
}

bool GPU_HW_Vulkan::CompilePipelines()
{
  VkDevice device = g_vulkan_context->GetDevice();
  VkPipelineCache pipeline_cache = g_vulkan_shader_cache->GetPipelineCache();

  GPU_HW_ShaderGen shadergen(m_host_display->GetRenderAPI(), m_resolution_scale, m_multisamples, m_per_sample_shading,
                             m_true_color, m_scaled_dithering, m_texture_filtering, m_using_uv_limits,
                             m_supports_dual_source_blend);

  Common::Timer compile_time;
  const int progress_total = 2 + (4 * 9 * 2 * 2) + (2 * 4 * 5 * 9 * 2 * 2) + 1 + 2 + 2 + 2 + 2 + (2 * 3);
  int progress_value = 0;
#define UPDATE_PROGRESS()                                                                                              \
  do                                                                                                                   \
  {                                                                                                                    \
    progress_value++;                                                                                                  \
    if (compile_time.GetTimeSeconds() >= 1.0f)                                                                         \
    {                                                                                                                  \
      compile_time.Reset();                                                                                            \
      g_host_interface->DisplayLoadingScreen("Compiling Shaders", 0, progress_total, progress_value);                  \
    }                                                                                                                  \
  } while (0)

  // vertex shaders - [textured]
  // fragment shaders - [render_mode][texture_mode][dithering][interlacing]
  DimensionalArray<VkShaderModule, 2> batch_vertex_shaders{};
  DimensionalArray<VkShaderModule, 2, 2, 9, 4> batch_fragment_shaders{};
  Common::ScopeGuard batch_shader_guard([&batch_vertex_shaders, &batch_fragment_shaders]() {
    batch_vertex_shaders.enumerate(Vulkan::Util::SafeDestroyShaderModule);
    batch_fragment_shaders.enumerate(Vulkan::Util::SafeDestroyShaderModule);
  });

  for (u8 textured = 0; textured < 2; textured++)
  {
    const std::string vs = shadergen.GenerateBatchVertexShader(ConvertToBoolUnchecked(textured));
    VkShaderModule shader = g_vulkan_shader_cache->GetVertexShader(vs);
    if (shader == VK_NULL_HANDLE)
      return false;

    batch_vertex_shaders[textured] = shader;
    UPDATE_PROGRESS();
  }

  for (u8 render_mode = 0; render_mode < 4; render_mode++)
  {
    for (u8 texture_mode = 0; texture_mode < 9; texture_mode++)
    {
      for (u8 dithering = 0; dithering < 2; dithering++)
      {
        for (u8 interlacing = 0; interlacing < 2; interlacing++)
        {
          const std::string fs = shadergen.GenerateBatchFragmentShader(
            static_cast<BatchRenderMode>(render_mode), static_cast<GPUTextureMode>(texture_mode),
            ConvertToBoolUnchecked(dithering), ConvertToBoolUnchecked(interlacing));

          VkShaderModule shader = g_vulkan_shader_cache->GetFragmentShader(fs);
          if (shader == VK_NULL_HANDLE)
            return false;

          batch_fragment_shaders[render_mode][texture_mode][dithering][interlacing] = shader;
          UPDATE_PROGRESS();
        }
      }
    }
  }

  Vulkan::GraphicsPipelineBuilder gpbuilder;

  // [depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  for (u8 depth_test = 0; depth_test < 2; depth_test++)
  {
    for (u8 render_mode = 0; render_mode < 4; render_mode++)
    {
      for (u8 transparency_mode = 0; transparency_mode < 5; transparency_mode++)
      {
        for (u8 texture_mode = 0; texture_mode < 9; texture_mode++)
        {
          for (u8 dithering = 0; dithering < 2; dithering++)
          {
            for (u8 interlacing = 0; interlacing < 2; interlacing++)
            {
              const bool textured = (static_cast<GPUTextureMode>(texture_mode) != GPUTextureMode::Disabled);

              gpbuilder.SetPipelineLayout(m_batch_pipeline_layout);
              gpbuilder.SetRenderPass(m_vram_render_pass, 0);

              gpbuilder.AddVertexBuffer(0, sizeof(BatchVertex), VK_VERTEX_INPUT_RATE_VERTEX);
              gpbuilder.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BatchVertex, x));
              gpbuilder.AddVertexAttribute(1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(BatchVertex, color));
              if (textured)
              {
                gpbuilder.AddVertexAttribute(2, 0, VK_FORMAT_R32_UINT, offsetof(BatchVertex, u));
                gpbuilder.AddVertexAttribute(3, 0, VK_FORMAT_R32_UINT, offsetof(BatchVertex, texpage));
                if (m_using_uv_limits)
                  gpbuilder.AddVertexAttribute(4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(BatchVertex, uv_limits));
              }

              gpbuilder.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
              gpbuilder.SetVertexShader(batch_vertex_shaders[BoolToUInt8(textured)]);
              gpbuilder.SetFragmentShader(batch_fragment_shaders[render_mode][texture_mode][dithering][interlacing]);

              gpbuilder.SetRasterizationState(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
              gpbuilder.SetDepthState(true, true,
                                      (depth_test != 0) ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS);
              gpbuilder.SetNoBlendingState();
              gpbuilder.SetMultisamples(m_multisamples, m_per_sample_shading);

              if ((static_cast<GPUTransparencyMode>(transparency_mode) != GPUTransparencyMode::Disabled &&
                   (static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                    static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque)) ||
                  m_texture_filtering != GPUTextureFilter::Nearest)
              {
                gpbuilder.SetBlendAttachment(
                  0, true, VK_BLEND_FACTOR_ONE,
                  m_supports_dual_source_blend ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA,
                  (static_cast<GPUTransparencyMode>(transparency_mode) ==
                     GPUTransparencyMode::BackgroundMinusForeground &&
                   static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                   static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                    VK_BLEND_OP_REVERSE_SUBTRACT :
                    VK_BLEND_OP_ADD,
                  VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
              }

              gpbuilder.SetDynamicViewportAndScissorState();

              VkPipeline pipeline = gpbuilder.Create(device, pipeline_cache);
              if (pipeline == VK_NULL_HANDLE)
                return false;

              m_batch_pipelines[depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing] =
                pipeline;
              UPDATE_PROGRESS();
            }
          }
        }
      }
    }
  }

  batch_shader_guard.Exit();

  VkShaderModule fullscreen_quad_vertex_shader =
    g_vulkan_shader_cache->GetVertexShader(shadergen.GenerateScreenQuadVertexShader());
  if (fullscreen_quad_vertex_shader == VK_NULL_HANDLE)
    return false;

  UPDATE_PROGRESS();

  Common::ScopeGuard fullscreen_quad_vertex_shader_guard([&fullscreen_quad_vertex_shader]() {
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fullscreen_quad_vertex_shader, nullptr);
  });

  // common state
  gpbuilder.SetRenderPass(m_vram_render_pass, 0);
  gpbuilder.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  gpbuilder.SetNoCullRasterizationState();
  gpbuilder.SetNoDepthTestState();
  gpbuilder.SetNoBlendingState();
  gpbuilder.SetDynamicViewportAndScissorState();
  gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader);
  gpbuilder.SetMultisamples(m_multisamples, false);

  // VRAM fill
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(
        (interlaced == 0) ? shadergen.GenerateFillFragmentShader() : shadergen.GenerateInterlacedFillFragmentShader());
      if (fs == VK_NULL_HANDLE)
        return false;

      gpbuilder.SetPipelineLayout(m_no_samplers_pipeline_layout);
      gpbuilder.SetFragmentShader(fs);
      gpbuilder.SetDepthState(true, true, VK_COMPARE_OP_ALWAYS);

      m_vram_fill_pipelines[interlaced] = gpbuilder.Create(device, pipeline_cache, false);
      vkDestroyShaderModule(device, fs, nullptr);
      if (m_vram_fill_pipelines[interlaced] == VK_NULL_HANDLE)
        return false;

      UPDATE_PROGRESS();
    }
  }

  // VRAM copy
  {
    VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateVRAMCopyFragmentShader());
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetPipelineLayout(m_single_sampler_pipeline_layout);
    gpbuilder.SetFragmentShader(fs);
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      gpbuilder.SetDepthState((depth_test != 0), true,
                              (depth_test != 0) ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS);

      m_vram_copy_pipelines[depth_test] = gpbuilder.Create(device, pipeline_cache, false);
      if (m_vram_copy_pipelines[depth_test] == VK_NULL_HANDLE)
      {
        vkDestroyShaderModule(device, fs, nullptr);
        return false;
      }

      UPDATE_PROGRESS();
    }

    vkDestroyShaderModule(device, fs, nullptr);
  }

  // VRAM write
  {
    VkShaderModule fs =
      g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateVRAMWriteFragmentShader(m_use_ssbos_for_vram_writes));
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetPipelineLayout(m_vram_write_pipeline_layout);
    gpbuilder.SetFragmentShader(fs);
    for (u8 depth_test = 0; depth_test < 2; depth_test++)
    {
      gpbuilder.SetDepthState(true, true, (depth_test != 0) ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS);
      m_vram_write_pipelines[depth_test] = gpbuilder.Create(device, pipeline_cache, false);
      if (m_vram_write_pipelines[depth_test] == VK_NULL_HANDLE)
      {
        vkDestroyShaderModule(device, fs, nullptr);
        return false;
      }

      UPDATE_PROGRESS();
    }

    vkDestroyShaderModule(device, fs, nullptr);
  }

  // VRAM update depth
  {
    VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateVRAMUpdateDepthFragmentShader());
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetRenderPass(m_vram_update_depth_render_pass, 0);
    gpbuilder.SetPipelineLayout(m_single_sampler_pipeline_layout);
    gpbuilder.SetFragmentShader(fs);
    gpbuilder.SetDepthState(true, true, VK_COMPARE_OP_ALWAYS);
    gpbuilder.SetBlendAttachment(0, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                 VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0);

    m_vram_update_depth_pipeline = gpbuilder.Create(device, pipeline_cache, false);
    vkDestroyShaderModule(device, fs, nullptr);
    if (m_vram_update_depth_pipeline == VK_NULL_HANDLE)
      return false;

    UPDATE_PROGRESS();
  }

  gpbuilder.Clear();

  // VRAM read
  {
    VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateVRAMReadFragmentShader());
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetRenderPass(m_vram_readback_render_pass, 0);
    gpbuilder.SetPipelineLayout(m_single_sampler_pipeline_layout);
    gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader);
    gpbuilder.SetFragmentShader(fs);
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetDynamicViewportAndScissorState();

    m_vram_readback_pipeline = gpbuilder.Create(device, pipeline_cache, false);
    vkDestroyShaderModule(device, fs, nullptr);
    if (m_vram_readback_pipeline == VK_NULL_HANDLE)
      return false;

    UPDATE_PROGRESS();
  }

  gpbuilder.Clear();

  // Display
  {
    gpbuilder.SetRenderPass(m_display_render_pass, 0);
    gpbuilder.SetPipelineLayout(m_single_sampler_pipeline_layout);
    gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader);
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetDynamicViewportAndScissorState();

    for (u8 depth_24 = 0; depth_24 < 2; depth_24++)
    {
      for (u8 interlace_mode = 0; interlace_mode < 3; interlace_mode++)
      {
        VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateDisplayFragmentShader(
          ConvertToBoolUnchecked(depth_24), static_cast<InterlacedRenderMode>(interlace_mode), m_chroma_smoothing));
        if (fs == VK_NULL_HANDLE)
          return false;

        gpbuilder.SetFragmentShader(fs);

        m_display_pipelines[depth_24][interlace_mode] = gpbuilder.Create(device, pipeline_cache, false);
        vkDestroyShaderModule(device, fs, nullptr);
        if (m_display_pipelines[depth_24][interlace_mode] == VK_NULL_HANDLE)
          return false;

        UPDATE_PROGRESS();
      }
    }
  }

#undef UPDATE_PROGRESS

  return true;
}

void GPU_HW_Vulkan::DestroyPipelines()
{
  m_batch_pipelines.enumerate(Vulkan::Util::SafeDestroyPipeline);

  for (VkPipeline& p : m_vram_fill_pipelines)
    Vulkan::Util::SafeDestroyPipeline(p);

  for (VkPipeline& p : m_vram_write_pipelines)
    Vulkan::Util::SafeDestroyPipeline(p);

  for (VkPipeline& p : m_vram_copy_pipelines)
    Vulkan::Util::SafeDestroyPipeline(p);

  Vulkan::Util::SafeDestroyPipeline(m_vram_readback_pipeline);
  Vulkan::Util::SafeDestroyPipeline(m_vram_update_depth_pipeline);

  m_display_pipelines.enumerate(Vulkan::Util::SafeDestroyPipeline);
}

void GPU_HW_Vulkan::DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices)
{
  BeginVRAMRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();

  // [primitive][depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  VkPipeline pipeline =
    m_batch_pipelines[BoolToUInt8(m_batch.check_mask_before_draw)][static_cast<u8>(render_mode)]
                     [static_cast<u8>(m_batch.texture_mode)][static_cast<u8>(m_batch.transparency_mode)]
                     [BoolToUInt8(m_batch.dithering)][BoolToUInt8(m_batch.interlacing)];

  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdDraw(cmdbuf, num_vertices, 1, base_vertex, 0);
}

void GPU_HW_Vulkan::SetScissorFromDrawingArea()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);

  Vulkan::Util::SetScissor(g_vulkan_context->GetCurrentCommandBuffer(), left, top, right - left, bottom - top);
}

void GPU_HW_Vulkan::ClearDisplay()
{
  GPU_HW::ClearDisplay();
  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  m_display_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  static const VkClearColorValue cc = {{0.0f, 0.0f, 0.0f, 1.0f}};
  static const VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmdbuf, m_display_texture.GetImage(), m_display_texture.GetLayout(), &cc, 1, &srr);
}

void GPU_HW_Vulkan::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();
  EndRenderPass();

  if (g_settings.debugging.show_vram)
  {
    if (IsUsingMultisampling())
    {
      UpdateVRAMReadTexture();
      m_host_display->SetDisplayTexture(&m_vram_read_texture, HostDisplayPixelFormat::RGBA8,
                                        m_vram_read_texture.GetWidth(), m_vram_read_texture.GetHeight(), 0, 0,
                                        m_vram_read_texture.GetWidth(), m_vram_read_texture.GetHeight());
    }
    else
    {
      m_vram_texture.TransitionToLayout(g_vulkan_context->GetCurrentCommandBuffer(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      m_host_display->SetDisplayTexture(&m_vram_texture, HostDisplayPixelFormat::RGBA8, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight(), 0, 0, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight());
    }
    m_host_display->SetDisplayParameters(VRAM_WIDTH, VRAM_HEIGHT, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                                         static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT));
  }
  else
  {
    const u32 resolution_scale = m_GPUSTAT.display_area_color_depth_24 ? 1 : m_resolution_scale;
    const u32 vram_offset_x = m_crtc_state.display_vram_left;
    const u32 vram_offset_y = m_crtc_state.display_vram_top;
    const u32 scaled_vram_offset_x = vram_offset_x * resolution_scale;
    const u32 scaled_vram_offset_y = vram_offset_y * resolution_scale;
    const u32 display_width = m_crtc_state.display_vram_width;
    const u32 display_height = m_crtc_state.display_vram_height;
    const u32 scaled_display_width = display_width * resolution_scale;
    const u32 scaled_display_height = display_height * resolution_scale;
    const InterlacedRenderMode interlaced = GetInterlacedRenderMode();

    if (IsDisplayDisabled())
    {
      m_host_display->ClearDisplayTexture();
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && interlaced == InterlacedRenderMode::None &&
             !IsUsingMultisampling() && (scaled_vram_offset_x + scaled_display_width) <= m_vram_texture.GetWidth() &&
             (scaled_vram_offset_y + scaled_display_height) <= m_vram_texture.GetHeight())
    {
      m_vram_texture.TransitionToLayout(g_vulkan_context->GetCurrentCommandBuffer(),
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      m_host_display->SetDisplayTexture(&m_vram_texture, HostDisplayPixelFormat::RGBA8, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight(), scaled_vram_offset_x, scaled_vram_offset_y,
                                        scaled_display_width, scaled_display_height);
    }
    else
    {
      EndRenderPass();

      const u32 reinterpret_field_offset = (interlaced != InterlacedRenderMode::None) ? GetInterlacedDisplayField() : 0;
      const u32 reinterpret_start_x = m_crtc_state.regs.X * resolution_scale;
      const u32 reinterpret_crop_left = (m_crtc_state.display_vram_left - m_crtc_state.regs.X) * resolution_scale;
      const u32 uniforms[4] = {reinterpret_start_x, scaled_vram_offset_y + reinterpret_field_offset,
                               reinterpret_crop_left, reinterpret_field_offset};

      VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
      m_display_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      BeginRenderPass(m_display_render_pass, m_display_framebuffer, 0, 0, scaled_display_width, scaled_display_height);

      vkCmdBindPipeline(
        cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_display_pipelines[BoolToUInt8(m_GPUSTAT.display_area_color_depth_24)][static_cast<u8>(interlaced)]);
      vkCmdPushConstants(cmdbuf, m_single_sampler_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uniforms),
                         uniforms);
      vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_single_sampler_pipeline_layout, 0, 1,
                              &m_vram_read_descriptor_set, 0, nullptr);
      Vulkan::Util::SetViewportAndScissor(cmdbuf, 0, 0, scaled_display_width, scaled_display_height);
      vkCmdDraw(cmdbuf, 3, 1, 0, 0);

      EndRenderPass();

      m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      m_display_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      m_host_display->SetDisplayTexture(&m_display_texture, HostDisplayPixelFormat::RGBA8, m_display_texture.GetWidth(),
                                        m_display_texture.GetHeight(), 0, 0, scaled_display_width,
                                        scaled_display_height);

      RestoreGraphicsAPIState();
    }

    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         GetDisplayAspectRatio());
  }
}

void GPU_HW_Vulkan::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  // Get bounds with wrap-around handled.
  const Common::Rectangle<u32> copy_rect = GetVRAMTransferBounds(x, y, width, height);
  const u32 encoded_width = (copy_rect.GetWidth() + 1) / 2;
  const u32 encoded_height = copy_rect.GetHeight();

  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_vram_readback_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  // Work around Mali driver bug: set full framebuffer size for render area. The GPU crashes with a page fault if we use
  // the actual size we're rendering to...
  BeginRenderPass(m_vram_readback_render_pass, m_vram_readback_framebuffer, 0, 0, m_vram_readback_texture.GetWidth(),
                  m_vram_readback_texture.GetHeight());

  // Encode the 24-bit texture as 16-bit.
  const u32 uniforms[4] = {copy_rect.left, copy_rect.top, copy_rect.GetWidth(), copy_rect.GetHeight()};
  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vram_readback_pipeline);
  vkCmdPushConstants(cmdbuf, m_single_sampler_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uniforms),
                     uniforms);
  vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_single_sampler_pipeline_layout, 0, 1,
                          &m_vram_read_descriptor_set, 0, nullptr);
  Vulkan::Util::SetViewportAndScissor(cmdbuf, 0, 0, encoded_width, encoded_height);
  vkCmdDraw(cmdbuf, 3, 1, 0, 0);

  EndRenderPass();

  m_vram_readback_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  // Stage the readback.
  m_vram_readback_staging_texture.CopyFromTexture(m_vram_readback_texture, 0, 0, 0, 0, 0, 0, encoded_width,
                                                  encoded_height);

  // And copy it into our shadow buffer (will execute command buffer and stall).
  m_vram_readback_staging_texture.ReadTexels(0, 0, encoded_width, encoded_height,
                                             &m_vram_shadow[copy_rect.top * VRAM_WIDTH + copy_rect.left],
                                             VRAM_WIDTH * sizeof(u16));

  RestoreGraphicsAPIState();
}

void GPU_HW_Vulkan::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  if ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT)
  {
    // CPU round trip if oversized for now.
    Log_WarningPrintf("Oversized VRAM fill (%u-%u, %u-%u), CPU round trip", x, x + width, y, y + height);
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    GPU::FillVRAM(x, y, width, height, color);
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_shadow.data(), false, false);
    return;
  }

  GPU_HW::FillVRAM(x, y, width, height, color);

  x *= m_resolution_scale;
  y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  BeginVRAMRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const VRAMFillUBOData uniforms = GetVRAMFillUBOData(x, y, width, height, color);
  vkCmdPushConstants(cmdbuf, m_no_samplers_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uniforms),
                     &uniforms);
  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_vram_fill_pipelines[BoolToUInt8(IsInterlacedRenderingEnabled())]);
  Vulkan::Util::SetViewportAndScissor(cmdbuf, x, y, width, height);
  vkCmdDraw(cmdbuf, 3, 1, 0, 0);

  RestoreGraphicsAPIState();
}

void GPU_HW_Vulkan::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  const Common::Rectangle<u32> bounds = GetVRAMTransferBounds(x, y, width, height);
  GPU_HW::UpdateVRAM(bounds.left, bounds.top, bounds.GetWidth(), bounds.GetHeight(), data, set_mask, check_mask);

  const u32 data_size = width * height * sizeof(u16);
  const u32 alignment = std::max<u32>(sizeof(u16), static_cast<u32>(g_vulkan_context->GetTexelBufferAlignment()));
  if (!m_texture_stream_buffer.ReserveMemory(data_size, alignment))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes in stream buffer", data_size);
    EndRenderPass();
    g_vulkan_context->ExecuteCommandBuffer(false);
    RestoreGraphicsAPIState();
    if (!m_texture_stream_buffer.ReserveMemory(data_size, alignment))
    {
      Panic("Failed to allocate space in stream buffer for VRAM write");
      return;
    }
  }

  const u32 start_index = m_texture_stream_buffer.GetCurrentOffset() / sizeof(u16);
  std::memcpy(m_texture_stream_buffer.GetCurrentHostPointer(), data, data_size);
  m_texture_stream_buffer.CommitMemory(data_size);

  BeginVRAMRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const VRAMWriteUBOData uniforms = GetVRAMWriteUBOData(x, y, width, height, start_index, set_mask, check_mask);
  vkCmdPushConstants(cmdbuf, m_vram_write_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uniforms),
                     &uniforms);
  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vram_write_pipelines[BoolToUInt8(check_mask)]);
  vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vram_write_pipeline_layout, 0, 1,
                          &m_vram_write_descriptor_set, 0, nullptr);

  // the viewport should already be set to the full vram, so just adjust the scissor
  const Common::Rectangle<u32> scaled_bounds = bounds * m_resolution_scale;
  Vulkan::Util::SetScissor(cmdbuf, scaled_bounds.left, scaled_bounds.top, scaled_bounds.GetWidth(),
                           scaled_bounds.GetHeight());
  vkCmdDraw(cmdbuf, 3, 1, 0, 0);

  RestoreGraphicsAPIState();
}

void GPU_HW_Vulkan::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  if (UseVRAMCopyShader(src_x, src_y, dst_x, dst_y, width, height) || IsUsingMultisampling())
  {
    const Common::Rectangle<u32> src_bounds = GetVRAMTransferBounds(src_x, src_y, width, height);
    const Common::Rectangle<u32> dst_bounds = GetVRAMTransferBounds(dst_x, dst_y, width, height);
    if (m_vram_dirty_rect.Intersects(src_bounds))
      UpdateVRAMReadTexture();
    IncludeVRAMDityRectangle(dst_bounds);

    const VRAMCopyUBOData uniforms(GetVRAMCopyUBOData(src_x, src_y, dst_x, dst_y, width, height));
    const Common::Rectangle<u32> dst_bounds_scaled(dst_bounds * m_resolution_scale);

    BeginVRAMRenderPass();

    VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_vram_copy_pipelines[BoolToUInt8(m_GPUSTAT.check_mask_before_draw)]);
    vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_single_sampler_pipeline_layout, 0, 1,
                            &m_vram_copy_descriptor_set, 0, nullptr);
    vkCmdPushConstants(cmdbuf, m_single_sampler_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uniforms),
                       &uniforms);
    Vulkan::Util::SetViewportAndScissor(cmdbuf, dst_bounds_scaled.left, dst_bounds_scaled.top,
                                        dst_bounds_scaled.GetWidth(), dst_bounds_scaled.GetHeight());
    vkCmdDraw(cmdbuf, 3, 1, 0, 0);
    RestoreGraphicsAPIState();

    if (m_GPUSTAT.check_mask_before_draw)
      m_current_depth++;

    return;
  }

  GPU_HW::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);

  src_x *= m_resolution_scale;
  src_y *= m_resolution_scale;
  dst_x *= m_resolution_scale;
  dst_y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_GENERAL);

  const VkImageCopy ic{{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                       {static_cast<s32>(src_x), static_cast<s32>(src_y), 0},
                       {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                       {static_cast<s32>(dst_x), static_cast<s32>(dst_y), 0},
                       {width, height, 1u}};
  vkCmdCopyImage(cmdbuf, m_vram_texture.GetImage(), m_vram_texture.GetLayout(), m_vram_texture.GetImage(),
                 m_vram_texture.GetLayout(), 1, &ic);

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void GPU_HW_Vulkan::UpdateVRAMReadTexture()
{
  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  m_vram_read_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  const auto scaled_rect = m_vram_dirty_rect * m_resolution_scale;

  if (m_vram_texture.GetSamples() > VK_SAMPLE_COUNT_1_BIT)
  {
    const VkImageResolve resolve{{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                                 {static_cast<s32>(scaled_rect.left), static_cast<s32>(scaled_rect.top), 0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                                 {static_cast<s32>(scaled_rect.left), static_cast<s32>(scaled_rect.top), 0},
                                 {scaled_rect.GetWidth(), scaled_rect.GetHeight(), 1u}};
    vkCmdResolveImage(cmdbuf, m_vram_texture.GetImage(), m_vram_texture.GetLayout(), m_vram_read_texture.GetImage(),
                      m_vram_read_texture.GetLayout(), 1, &resolve);
  }
  else
  {
    const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                           {static_cast<s32>(scaled_rect.left), static_cast<s32>(scaled_rect.top), 0},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                           {static_cast<s32>(scaled_rect.left), static_cast<s32>(scaled_rect.top), 0},
                           {scaled_rect.GetWidth(), scaled_rect.GetHeight(), 1u}};

    vkCmdCopyImage(cmdbuf, m_vram_texture.GetImage(), m_vram_texture.GetLayout(), m_vram_read_texture.GetImage(),
                   m_vram_read_texture.GetLayout(), 1u, &copy);
  }

  m_vram_read_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  GPU_HW::UpdateVRAMReadTexture();
}

void GPU_HW_Vulkan::UpdateDepthBufferFromMaskBit()
{
  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  BeginRenderPass(m_vram_update_depth_render_pass, m_vram_update_depth_framebuffer, 0, 0, m_vram_texture.GetWidth(),
                  m_vram_texture.GetHeight());

  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_vram_update_depth_pipeline);
  vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_single_sampler_pipeline_layout, 0, 1,
                          &m_vram_read_descriptor_set, 0, nullptr);
  Vulkan::Util::SetViewportAndScissor(cmdbuf, 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
  vkCmdDraw(cmdbuf, 3, 1, 0, 0);

  EndRenderPass();

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  RestoreGraphicsAPIState();
}

std::unique_ptr<GPU> GPU::CreateHardwareVulkanRenderer()
{
  return std::make_unique<GPU_HW_Vulkan>();
}
