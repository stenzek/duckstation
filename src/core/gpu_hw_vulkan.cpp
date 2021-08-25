#include "gpu_hw_vulkan.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/scope_guard.h"
#include "common/state_wrapper.h"
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

GPURenderer GPU_HW_Vulkan::GetRendererType() const
{
  return GPURenderer::HardwareVulkan;
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

void GPU_HW_Vulkan::Reset(bool clear_vram)
{
  GPU_HW::Reset(clear_vram);

  EndRenderPass();

  if (clear_vram)
    ClearFramebuffer();
}

bool GPU_HW_Vulkan::DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display)
{
  if (host_texture)
  {
    EndRenderPass();

    const VkImageCopy ic{{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                         {0, 0, 0},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                         {0, 0, 0},
                         {m_vram_texture.GetWidth(), m_vram_texture.GetHeight(), 1u}};

    VkCommandBuffer buf = g_vulkan_context->GetCurrentCommandBuffer();
    const Vulkan::Util::DebugScope debugScope(buf, "GPU_HW_Vulkan::DoState");

    if (sw.IsReading())
    {
      Vulkan::Texture* tex = static_cast<Vulkan::Texture*>((*host_texture)->GetHandle());
      if (tex->GetWidth() != m_vram_texture.GetWidth() || tex->GetHeight() != m_vram_texture.GetHeight() ||
          tex->GetSamples() != m_vram_texture.GetSamples())
      {
        return false;
      }

      const VkImageLayout old_tex_layout = tex->GetLayout();
      const VkImageLayout old_vram_layout = m_vram_texture.GetLayout();
      tex->TransitionToLayout(buf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      m_vram_texture.TransitionToLayout(buf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      vkCmdCopyImage(g_vulkan_context->GetCurrentCommandBuffer(), tex->GetImage(), tex->GetLayout(),
                     m_vram_texture.GetImage(), m_vram_texture.GetLayout(), 1, &ic);
      m_vram_texture.TransitionToLayout(buf, old_vram_layout);
      tex->TransitionToLayout(buf, old_tex_layout);
    }
    else
    {
      HostDisplayTexture* htex = *host_texture;
      if (!htex || htex->GetWidth() != m_vram_texture.GetWidth() || htex->GetHeight() != m_vram_texture.GetHeight() ||
          htex->GetSamples() != static_cast<u32>(m_vram_texture.GetSamples()))
      {
        delete htex;

        htex = m_host_display
                 ->CreateTexture(m_vram_texture.GetWidth(), m_vram_texture.GetHeight(), 1, 1,
                                 m_vram_texture.GetSamples(), HostDisplayPixelFormat::RGBA8, nullptr, 0, false)
                 .release();
        *host_texture = htex;
        if (!htex)
          return false;
      }

      Vulkan::Texture* tex = static_cast<Vulkan::Texture*>(htex->GetHandle());
      if (tex->GetWidth() != m_vram_texture.GetWidth() || tex->GetHeight() != m_vram_texture.GetHeight() ||
          tex->GetSamples() != m_vram_texture.GetSamples())
      {
        return false;
      }

      const VkImageLayout old_vram_layout = m_vram_texture.GetLayout();
      tex->TransitionToLayout(buf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      m_vram_texture.TransitionToLayout(buf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      vkCmdCopyImage(g_vulkan_context->GetCurrentCommandBuffer(), m_vram_texture.GetImage(), m_vram_texture.GetLayout(),
                     tex->GetImage(), tex->GetLayout(), 1, &ic);
      m_vram_texture.TransitionToLayout(buf, old_vram_layout);
      tex->TransitionToLayout(buf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
  }

  return GPU_HW::DoState(sw, host_texture, update_display);
}

void GPU_HW_Vulkan::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();

  EndRenderPass();

  if (m_host_display->GetDisplayTextureHandle() == &m_vram_texture)
  {
    m_vram_texture.TransitionToLayout(g_vulkan_context->GetCurrentCommandBuffer(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  // this is called at the end of the frame, so the UBO is associated with the previous command buffer.
  m_batch_ubo_dirty = true;
}

void GPU_HW_Vulkan::RestoreGraphicsAPIState()
{
  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::RestoreGraphicsAPIState");
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

  // Everything should be finished executing before recreating resources.
  m_host_display->ClearDisplayTexture();
  g_vulkan_context->ExecuteCommandBuffer(true);

  if (framebuffer_changed)
    CreateFramebuffer();

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
    ExecuteCommandBuffer(false, true);
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
    ExecuteCommandBuffer(false, true);
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
  m_supports_adaptive_downsampling = true;

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

  Vulkan::Util::SafeDestroyPipelineLayout(m_downsample_pipeline_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_downsample_composite_descriptor_set_layout);
  Vulkan::Util::SafeDestroyPipelineLayout(m_downsample_composite_pipeline_layout);

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
  Vulkan::Util::SafeDestroySampler(m_trilinear_sampler);
}

void GPU_HW_Vulkan::BeginRenderPass(VkRenderPass render_pass, VkFramebuffer framebuffer, u32 x, u32 y, u32 width,
                                    u32 height, const VkClearValue* clear_value /* = nullptr */)
{
  DebugAssert(m_current_render_pass == VK_NULL_HANDLE);

  const VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                    nullptr,
                                    render_pass,
                                    framebuffer,
                                    {{static_cast<s32>(x), static_cast<s32>(y)}, {width, height}},
                                    (clear_value ? 1u : 0u),
                                    clear_value};
  Vulkan::Util::BeginDebugScope(g_vulkan_context->GetCurrentCommandBuffer(), "GPU_HW_Vulkan::BeginRenderPass");
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
  Vulkan::Util::EndDebugScope(g_vulkan_context->GetCurrentCommandBuffer());
  m_current_render_pass = VK_NULL_HANDLE;
}

void GPU_HW_Vulkan::ExecuteCommandBuffer(bool wait_for_completion, bool restore_state)
{
  EndRenderPass();
  g_vulkan_context->ExecuteCommandBuffer(wait_for_completion);
  m_batch_ubo_dirty = true;
  if (restore_state)
    RestoreGraphicsAPIState();
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

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_batch_descriptor_set_layout,
                              "Batch Descriptor Set Layout");

  // textures start at 1
  dslbuilder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_single_sampler_descriptor_set_layout = dslbuilder.Create(device);
  if (m_single_sampler_descriptor_set_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_single_sampler_descriptor_set_layout,
                              "Single Sampler Descriptor Set Layout");

  if (m_use_ssbos_for_vram_writes)
    dslbuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  else
    dslbuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_vram_write_descriptor_set_layout = dslbuilder.Create(device);
  if (m_vram_write_descriptor_set_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_write_descriptor_set_layout,
                              "VRAM Write Descriptor Set Layout");

  Vulkan::PipelineLayoutBuilder plbuilder;
  plbuilder.AddDescriptorSet(m_batch_descriptor_set_layout);
  m_batch_pipeline_layout = plbuilder.Create(device);
  if (m_batch_pipeline_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_batch_pipeline_layout, "Batch Pipeline Layout");

  plbuilder.AddDescriptorSet(m_single_sampler_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_single_sampler_pipeline_layout = plbuilder.Create(device);
  if (m_single_sampler_pipeline_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_single_sampler_pipeline_layout,
                              "Single Sampler Pipeline Layout");

  plbuilder.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_no_samplers_pipeline_layout = plbuilder.Create(device);
  if (m_no_samplers_pipeline_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_no_samplers_pipeline_layout,
                              "No Samplers Pipeline Layout");

  plbuilder.AddDescriptorSet(m_vram_write_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_vram_write_pipeline_layout = plbuilder.Create(device);
  if (m_vram_write_pipeline_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_write_pipeline_layout,
                              "VRAM Write Pipeline Layout");

  plbuilder.AddDescriptorSet(m_single_sampler_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_downsample_pipeline_layout = plbuilder.Create(device);
  if (m_downsample_pipeline_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_downsample_pipeline_layout,
                              "Downsample Pipeline Layout");

  dslbuilder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  dslbuilder.AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_downsample_composite_descriptor_set_layout = dslbuilder.Create(device);
  if (m_downsample_composite_descriptor_set_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(),

                              m_downsample_composite_descriptor_set_layout,
                              "Downsample Composite Descriptor Set Layout");

  plbuilder.AddDescriptorSet(m_downsample_composite_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, MAX_PUSH_CONSTANTS_SIZE);
  m_downsample_composite_pipeline_layout = plbuilder.Create(device);
  if (m_downsample_composite_pipeline_layout == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_downsample_composite_pipeline_layout,
                              "Downsample Composite Pipeline Layout");

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
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_point_sampler, "Point Sampler");

  sbuilder.SetLinearSampler(false, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  sbuilder.SetAddressMode(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                          VK_SAMPLER_ADDRESS_MODE_REPEAT);
  m_linear_sampler = sbuilder.Create(device);
  if (m_linear_sampler == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_linear_sampler, "Linear Sampler");

  sbuilder.SetLinearSampler(true, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  m_trilinear_sampler = sbuilder.Create(device);
  if (m_trilinear_sampler == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_trilinear_sampler, "Trilinear Sampler");

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
      !m_display_texture.Create(
        ((m_downsample_mode == GPUDownsampleMode::Adaptive) ? VRAM_WIDTH : GPU_MAX_DISPLAY_WIDTH) * m_resolution_scale,
        GPU_MAX_DISPLAY_HEIGHT * m_resolution_scale, 1, 1, texture_format, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
          VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      !m_vram_readback_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, 1, 1, texture_format, VK_SAMPLE_COUNT_1_BIT,
                                      VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
      !m_vram_readback_staging_texture.Create(Vulkan::StagingBuffer::Type::Readback, texture_format, VRAM_WIDTH / 2,
                                              VRAM_HEIGHT))
  {
    return false;
  }

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_texture.GetImage(), "VRAM Texture");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_texture.GetView(), "VRAM Texture View");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_texture.GetDeviceMemory(), "VRAM Texture Memory");

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_depth_texture.GetImage(), "VRAM Depth Texture");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_depth_texture.GetView(), "VRAM Depth Texture View");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_depth_texture.GetDeviceMemory(),
                              "VRAM Depth Texture Memory");

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_read_texture.GetImage(), "VRAM Read Texture");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_read_texture.GetView(), "VRAM Read Texture View");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_read_texture.GetDeviceMemory(),
                              "VRAM Read Texture Memory");

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_display_texture.GetImage(), "Display Texture");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_display_texture.GetView(), "Display Texture View");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_display_texture.GetDeviceMemory(),
                              "Display Texture Memory");

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_readback_texture.GetImage(),
                              "VRAM Readback Texture");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_readback_texture.GetView(),
                              "VRAM Readback Texture View");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_readback_texture.GetDeviceMemory(),
                              "VRAM Readback Texture Memory");

  m_vram_render_pass =
    g_vulkan_context->GetRenderPass(texture_format, depth_format, samples, VK_ATTACHMENT_LOAD_OP_LOAD);
  m_vram_update_depth_render_pass =
    g_vulkan_context->GetRenderPass(VK_FORMAT_UNDEFINED, depth_format, samples, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  m_display_load_render_pass = g_vulkan_context->GetRenderPass(
    m_display_texture.GetFormat(), VK_FORMAT_UNDEFINED, m_display_texture.GetSamples(), VK_ATTACHMENT_LOAD_OP_LOAD);
  m_display_discard_render_pass =
    g_vulkan_context->GetRenderPass(m_display_texture.GetFormat(), VK_FORMAT_UNDEFINED, m_display_texture.GetSamples(),
                                    VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  m_vram_readback_render_pass =
    g_vulkan_context->GetRenderPass(m_vram_readback_texture.GetFormat(), VK_FORMAT_UNDEFINED,
                                    m_vram_readback_texture.GetSamples(), VK_ATTACHMENT_LOAD_OP_DONT_CARE);

  if (m_vram_render_pass == VK_NULL_HANDLE || m_vram_update_depth_render_pass == VK_NULL_HANDLE ||
      m_display_load_render_pass == VK_NULL_HANDLE || m_vram_readback_render_pass == VK_NULL_HANDLE)
  {
    return false;
  }

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_render_pass, "VRAM Render Pass");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_update_depth_render_pass,
                              "VRAM Update Depth Render Pass");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_display_load_render_pass, "Display Load Render Pass");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_readback_render_pass, "VRAM Readback Render Pass");

  // vram framebuffer has both colour and depth
  Vulkan::FramebufferBuilder fbb;
  fbb.AddAttachment(m_vram_texture.GetView());
  fbb.AddAttachment(m_vram_depth_texture.GetView());
  fbb.SetRenderPass(m_vram_render_pass);
  fbb.SetSize(m_vram_texture.GetWidth(), m_vram_texture.GetHeight(), m_vram_texture.GetLayers());
  m_vram_framebuffer = fbb.Create(g_vulkan_context->GetDevice());
  if (m_vram_framebuffer == VK_NULL_HANDLE)
    return false;
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_framebuffer, "VRAM Framebuffer");

  m_vram_update_depth_framebuffer = m_vram_depth_texture.CreateFramebuffer(m_vram_update_depth_render_pass);
  m_vram_readback_framebuffer = m_vram_readback_texture.CreateFramebuffer(m_vram_readback_render_pass);
  m_display_framebuffer = m_display_texture.CreateFramebuffer(m_display_load_render_pass);
  if (m_vram_update_depth_framebuffer == VK_NULL_HANDLE || m_vram_readback_framebuffer == VK_NULL_HANDLE ||
      m_display_framebuffer == VK_NULL_HANDLE)
  {
    return false;
  }
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_update_depth_framebuffer,
                              "VRAM Update Depth Framebuffer");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_readback_framebuffer, "VRAM Readback Framebuffer");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_display_framebuffer, "Display Framebuffer");

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::CreateFramebuffer");

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  m_vram_depth_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  m_vram_read_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  Vulkan::DescriptorSetUpdateBuilder dsubuilder;

  m_batch_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_batch_descriptor_set_layout);
  m_vram_copy_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_single_sampler_descriptor_set_layout);
  m_vram_read_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_single_sampler_descriptor_set_layout);
  m_display_descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_single_sampler_descriptor_set_layout);
  if (m_batch_descriptor_set == VK_NULL_HANDLE || m_vram_copy_descriptor_set == VK_NULL_HANDLE ||
      m_vram_read_descriptor_set == VK_NULL_HANDLE || m_display_descriptor_set == VK_NULL_HANDLE)
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
  dsubuilder.AddCombinedImageSamplerDescriptorWrite(m_display_descriptor_set, 1, m_display_texture.GetView(),
                                                    m_point_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  dsubuilder.Update(g_vulkan_context->GetDevice());

  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
  {
    const u32 levels = GetAdaptiveDownsamplingMipLevels();

    if (!m_downsample_texture.Create(texture_width, texture_height, levels, 1, texture_format, VK_SAMPLE_COUNT_1_BIT,
                                     VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !m_downsample_weight_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, 1, 1, VK_FORMAT_R8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                            VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
    {
      return false;
    }

    m_downsample_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    m_downsample_render_pass = g_vulkan_context->GetRenderPass(m_downsample_texture.GetFormat(), VK_FORMAT_UNDEFINED,
                                                               VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR);
    m_downsample_weight_render_pass = g_vulkan_context->GetRenderPass(
      m_downsample_weight_texture.GetFormat(), VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR);
    if (m_downsample_render_pass == VK_NULL_HANDLE || m_downsample_weight_render_pass == VK_NULL_HANDLE)
      return false;

    m_downsample_weight_framebuffer = m_downsample_weight_texture.CreateFramebuffer(m_downsample_weight_render_pass);
    if (m_downsample_weight_framebuffer == VK_NULL_HANDLE)
      return false;

    m_downsample_mip_views.resize(levels);
    for (u32 i = 0; i < levels; i++)
    {
      SmoothMipView& mv = m_downsample_mip_views[i];

      const VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         nullptr,
                                         0,
                                         m_downsample_texture.GetImage(),
                                         VK_IMAGE_VIEW_TYPE_2D,
                                         m_downsample_texture.GetFormat(),
                                         {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                         {VK_IMAGE_ASPECT_COLOR_BIT, i, 1u, 0u, 1u}};
      VkResult res = vkCreateImageView(g_vulkan_context->GetDevice(), &vci, nullptr, &mv.image_view);
      if (res != VK_SUCCESS)
      {
        LOG_VULKAN_ERROR(res, "vkCreateImageView() for smooth mip failed: ");
        return false;
      }

      mv.descriptor_set = g_vulkan_context->AllocateGlobalDescriptorSet(m_single_sampler_descriptor_set_layout);
      if (mv.descriptor_set == VK_NULL_HANDLE)
        return false;

      dsubuilder.AddCombinedImageSamplerDescriptorWrite(m_downsample_mip_views[i].descriptor_set, 1,
                                                        m_downsample_mip_views[i].image_view, m_point_sampler,
                                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      fbb.AddAttachment(mv.image_view);
      fbb.SetRenderPass(m_downsample_render_pass);
      fbb.SetSize(texture_width >> i, texture_height >> i, 1);
      mv.framebuffer = fbb.Create(g_vulkan_context->GetDevice());
      if (mv.framebuffer == VK_NULL_HANDLE)
        return false;
    }

    m_downsample_composite_descriptor_set =
      g_vulkan_context->AllocateGlobalDescriptorSet(m_downsample_composite_descriptor_set_layout);
    if (m_downsample_composite_descriptor_set_layout == VK_NULL_HANDLE)
      return false;

    dsubuilder.AddCombinedImageSamplerDescriptorWrite(m_downsample_composite_descriptor_set, 1,
                                                      m_downsample_texture.GetView(), m_trilinear_sampler,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dsubuilder.AddCombinedImageSamplerDescriptorWrite(m_downsample_composite_descriptor_set, 2,
                                                      m_downsample_weight_texture.GetView(), m_linear_sampler,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    dsubuilder.Update(g_vulkan_context->GetDevice());
  }
  else if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    if (!m_downsample_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, 1, 1, texture_format, VK_SAMPLE_COUNT_1_BIT,
                                     VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
      return false;
    }

    m_downsample_render_pass = g_vulkan_context->GetRenderPass(m_downsample_texture.GetFormat(), VK_FORMAT_UNDEFINED,
                                                               VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR);

    m_downsample_mip_views.resize(1);
    m_downsample_mip_views[0].framebuffer = m_downsample_texture.CreateFramebuffer(m_downsample_render_pass);
    if (m_downsample_mip_views[0].framebuffer == VK_NULL_HANDLE)
      return false;
  }

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
  const VkClearDepthStencilValue cds = {m_pgxp_depth_buffer ? 1.0f : 0.0f};
  static constexpr VkImageSubresourceRange csrr = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  static constexpr VkImageSubresourceRange dsrr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u};
  vkCmdClearColorImage(cmdbuf, m_vram_texture.GetImage(), m_vram_texture.GetLayout(), &cc, 1u, &csrr);
  vkCmdClearDepthStencilImage(cmdbuf, m_vram_depth_texture.GetImage(), m_vram_depth_texture.GetLayout(), &cds, 1u,
                              &dsrr);

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  m_vram_depth_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  m_last_depth_z = 1.0f;

  SetFullVRAMDirtyRectangle();
}

void GPU_HW_Vulkan::DestroyFramebuffer()
{
  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_downsample_composite_descriptor_set);

  for (SmoothMipView& mv : m_downsample_mip_views)
  {
    Vulkan::Util::SafeFreeGlobalDescriptorSet(mv.descriptor_set);
    Vulkan::Util::SafeDestroyImageView(mv.image_view);
    Vulkan::Util::SafeDestroyFramebuffer(mv.framebuffer);
  }
  m_downsample_mip_views.clear();
  m_downsample_texture.Destroy(false);
  Vulkan::Util::SafeDestroyFramebuffer(m_downsample_weight_framebuffer);
  m_downsample_weight_texture.Destroy(false);

  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_batch_descriptor_set);
  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_vram_copy_descriptor_set);
  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_vram_read_descriptor_set);
  Vulkan::Util::SafeFreeGlobalDescriptorSet(m_display_descriptor_set);

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
  if (!m_vertex_stream_buffer.Create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VERTEX_BUFFER_SIZE))
    return false;

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vertex_stream_buffer.GetBuffer(),
                              "Vertex Stream Buffer");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vertex_stream_buffer.GetDeviceMemory(),
                              "Vertex Stream Buffer Memory");
  return true;
}

bool GPU_HW_Vulkan::CreateUniformBuffer()
{
  if (!m_uniform_stream_buffer.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, UNIFORM_BUFFER_SIZE))
    return false;

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_uniform_stream_buffer.GetBuffer(),
                              "Uniform Stream Buffer");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_uniform_stream_buffer.GetDeviceMemory(),
                              "Uniform Stream Buffer Memory");
  return true;
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

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_texture_stream_buffer.GetBuffer(),
                              "Texture Stream Buffer");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_texture_stream_buffer.GetDeviceMemory(),
                              "Texture Stream Buffer Memory");

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_write_descriptor_set, "VRAM Write Descriptor Set");

  return true;
}

bool GPU_HW_Vulkan::CompilePipelines()
{
  VkDevice device = g_vulkan_context->GetDevice();
  VkPipelineCache pipeline_cache = g_vulkan_shader_cache->GetPipelineCache();

  GPU_HW_ShaderGen shadergen(m_host_display->GetRenderAPI(), m_resolution_scale, m_multisamples, m_per_sample_shading,
                             m_true_color, m_scaled_dithering, m_texture_filtering, m_using_uv_limits,
                             m_pgxp_depth_buffer, m_supports_dual_source_blend);

  ShaderCompileProgressTracker progress("Compiling Pipelines", 2 + (4 * 9 * 2 * 2) + (3 * 4 * 5 * 9 * 2 * 2) + 1 + 2 +
                                                                 (2 * 2) + 2 + 1 + 1 + (2 * 3) + 1);

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
    progress.Increment();
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
          progress.Increment();
        }
      }
    }
  }

  Vulkan::GraphicsPipelineBuilder gpbuilder;

  // [depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  for (u8 depth_test = 0; depth_test < 3; depth_test++)
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
              static constexpr std::array<VkCompareOp, 3> depth_test_values = {
                VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL};
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
              gpbuilder.SetDepthState(true, true, depth_test_values[depth_test]);
              gpbuilder.SetNoBlendingState();
              gpbuilder.SetMultisamples(m_multisamples, m_per_sample_shading);

              if ((static_cast<GPUTransparencyMode>(transparency_mode) != GPUTransparencyMode::Disabled &&
                   (static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                    static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque)) ||
                  m_texture_filtering != GPUTextureFilter::Nearest)
              {
                if (m_supports_dual_source_blend)
                {
                  gpbuilder.SetBlendAttachment(
                    0, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC1_ALPHA,
                    (static_cast<GPUTransparencyMode>(transparency_mode) ==
                       GPUTransparencyMode::BackgroundMinusForeground &&
                     static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                     static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                      VK_BLEND_OP_REVERSE_SUBTRACT :
                      VK_BLEND_OP_ADD,
                    VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
                }
                else
                {
                  const float factor = (static_cast<GPUTransparencyMode>(transparency_mode) ==
                                        GPUTransparencyMode::HalfBackgroundPlusHalfForeground) ?
                                         0.5f :
                                         1.0f;
                  gpbuilder.SetBlendAttachment(
                    0, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_ALPHA,
                    (static_cast<GPUTransparencyMode>(transparency_mode) ==
                       GPUTransparencyMode::BackgroundMinusForeground &&
                     static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::TransparencyDisabled &&
                     static_cast<BatchRenderMode>(render_mode) != BatchRenderMode::OnlyOpaque) ?
                      VK_BLEND_OP_REVERSE_SUBTRACT :
                      VK_BLEND_OP_ADD,
                    VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
                  gpbuilder.SetBlendConstants(0.0f, 0.0f, 0.0f, factor);
                }
              }

              gpbuilder.SetDynamicViewportAndScissorState();

              VkPipeline pipeline = gpbuilder.Create(device, pipeline_cache);
              if (pipeline == VK_NULL_HANDLE)
                return false;

              m_batch_pipelines[depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing] =
                pipeline;
              progress.Increment();
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
  VkShaderModule uv_quad_vertex_shader = g_vulkan_shader_cache->GetVertexShader(shadergen.GenerateUVQuadVertexShader());
  if (uv_quad_vertex_shader == VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), uv_quad_vertex_shader, nullptr);
    return false;
  }

  progress.Increment();

  Common::ScopeGuard fullscreen_quad_vertex_shader_guard([&fullscreen_quad_vertex_shader, &uv_quad_vertex_shader]() {
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fullscreen_quad_vertex_shader, nullptr);
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), uv_quad_vertex_shader, nullptr);
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
  for (u8 wrapped = 0; wrapped < 2; wrapped++)
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(
        shadergen.GenerateVRAMFillFragmentShader(ConvertToBoolUnchecked(wrapped), ConvertToBoolUnchecked(interlaced)));
      if (fs == VK_NULL_HANDLE)
        return false;

      gpbuilder.SetPipelineLayout(m_no_samplers_pipeline_layout);
      gpbuilder.SetFragmentShader(fs);
      gpbuilder.SetDepthState(true, true, VK_COMPARE_OP_ALWAYS);

      m_vram_fill_pipelines[wrapped][interlaced] = gpbuilder.Create(device, pipeline_cache, false);
      vkDestroyShaderModule(device, fs, nullptr);
      if (m_vram_fill_pipelines[wrapped][interlaced] == VK_NULL_HANDLE)
        return false;

      progress.Increment();
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

      progress.Increment();
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

      progress.Increment();
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
    Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_update_depth_pipeline,
                                "VRAM Update Depth Pipeline");

    progress.Increment();
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
    Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_vram_readback_pipeline, "VRAM Read Pipeline");

    progress.Increment();
  }

  gpbuilder.Clear();

  // Display
  {
    gpbuilder.SetRenderPass(m_display_load_render_pass, 0);
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

        progress.Increment();
      }
    }
  }

  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
  {
    gpbuilder.Clear();
    gpbuilder.SetRenderPass(m_downsample_render_pass, 0);
    gpbuilder.SetPipelineLayout(m_downsample_pipeline_layout);
    gpbuilder.SetVertexShader(uv_quad_vertex_shader);
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetDynamicViewportAndScissorState();

    VkShaderModule fs =
      g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateAdaptiveDownsampleMipFragmentShader(true));
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetFragmentShader(fs);
    m_downsample_first_pass_pipeline = gpbuilder.Create(device, pipeline_cache, false);
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fs, nullptr);
    if (m_downsample_first_pass_pipeline == VK_NULL_HANDLE)
      return false;
    Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_downsample_first_pass_pipeline,
                                "Downsample First Pass Pipeline");

    fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateAdaptiveDownsampleMipFragmentShader(false));
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetFragmentShader(fs);
    m_downsample_mid_pass_pipeline = gpbuilder.Create(device, pipeline_cache, false);
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fs, nullptr);
    if (m_downsample_mid_pass_pipeline == VK_NULL_HANDLE)
      return false;
    Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_downsample_mid_pass_pipeline,
                                "Downsample Mid Pass Pipeline");

    fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateAdaptiveDownsampleBlurFragmentShader());
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetFragmentShader(fs);
    gpbuilder.SetRenderPass(m_downsample_weight_render_pass, 0);
    m_downsample_blur_pass_pipeline = gpbuilder.Create(device, pipeline_cache, false);
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fs, nullptr);
    if (m_downsample_blur_pass_pipeline == VK_NULL_HANDLE)
      return false;
    Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_downsample_blur_pass_pipeline,
                                "Downsample Blur Pass Pipeline");

    fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateAdaptiveDownsampleCompositeFragmentShader());
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetFragmentShader(fs);
    gpbuilder.SetPipelineLayout(m_downsample_composite_pipeline_layout);
    gpbuilder.SetRenderPass(m_display_load_render_pass, 0);
    m_downsample_composite_pass_pipeline = gpbuilder.Create(device, pipeline_cache, false);
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fs, nullptr);
    if (m_downsample_composite_pass_pipeline == VK_NULL_HANDLE)
      return false;

    Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_downsample_composite_pass_pipeline,
                                "Downsample Composite Pass Pipeline");
  }
  else if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    gpbuilder.Clear();
    gpbuilder.SetRenderPass(m_downsample_render_pass, 0);
    gpbuilder.SetPipelineLayout(m_single_sampler_pipeline_layout);
    gpbuilder.SetVertexShader(fullscreen_quad_vertex_shader);
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetDynamicViewportAndScissorState();

    VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(shadergen.GenerateBoxSampleDownsampleFragmentShader());
    if (fs == VK_NULL_HANDLE)
      return false;

    gpbuilder.SetFragmentShader(fs);
    m_downsample_first_pass_pipeline = gpbuilder.Create(device, pipeline_cache, false);
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fs, nullptr);
    if (m_downsample_first_pass_pipeline == VK_NULL_HANDLE)
      return false;

    Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_downsample_first_pass_pipeline,
                                "Downsample First Pass Pipeline");
  }

  progress.Increment();

#undef UPDATE_PROGRESS

  return true;
}

void GPU_HW_Vulkan::DestroyPipelines()
{
  m_batch_pipelines.enumerate(Vulkan::Util::SafeDestroyPipeline);

  m_vram_fill_pipelines.enumerate(Vulkan::Util::SafeDestroyPipeline);

  for (VkPipeline& p : m_vram_write_pipelines)
    Vulkan::Util::SafeDestroyPipeline(p);

  for (VkPipeline& p : m_vram_copy_pipelines)
    Vulkan::Util::SafeDestroyPipeline(p);

  Vulkan::Util::SafeDestroyPipeline(m_vram_readback_pipeline);
  Vulkan::Util::SafeDestroyPipeline(m_vram_update_depth_pipeline);

  Vulkan::Util::SafeDestroyPipeline(m_downsample_first_pass_pipeline);
  Vulkan::Util::SafeDestroyPipeline(m_downsample_mid_pass_pipeline);
  Vulkan::Util::SafeDestroyPipeline(m_downsample_blur_pass_pipeline);
  Vulkan::Util::SafeDestroyPipeline(m_downsample_composite_pass_pipeline);

  m_display_pipelines.enumerate(Vulkan::Util::SafeDestroyPipeline);
}

void GPU_HW_Vulkan::DrawBatchVertices(BatchRenderMode render_mode, u32 base_vertex, u32 num_vertices)
{
  BeginVRAMRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::DrawBatchVertices: [%u,%u)", base_vertex,
                                            base_vertex + num_vertices);

  // [depth_test][render_mode][texture_mode][transparency_mode][dithering][interlacing]
  const u8 depth_test = m_batch.use_depth_buffer ? static_cast<u8>(2) : BoolToUInt8(m_batch.check_mask_before_draw);
  VkPipeline pipeline =
    m_batch_pipelines[depth_test][static_cast<u8>(render_mode)][static_cast<u8>(m_batch.texture_mode)][static_cast<u8>(
      m_batch.transparency_mode)][BoolToUInt8(m_batch.dithering)][BoolToUInt8(m_batch.interlacing)];

  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdDraw(cmdbuf, num_vertices, 1, base_vertex, 0);
}

void GPU_HW_Vulkan::SetScissorFromDrawingArea()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);
  const Vulkan::Util::DebugScope debugScope(g_vulkan_context->GetCurrentCommandBuffer(),
                                            "GPU_HW_Vulkan::SetScissorFromDrawingArea: {%u,%u} {%u,%u}", left, top,
                                            right, bottom);
  Vulkan::Util::SetScissor(g_vulkan_context->GetCurrentCommandBuffer(), left, top, right - left, bottom - top);
}

void GPU_HW_Vulkan::ClearDisplay()
{
  GPU_HW::ClearDisplay();
  EndRenderPass();

  m_host_display->ClearDisplayTexture();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::ClearDisplay");
  m_display_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  static const VkClearColorValue cc = {{0.0f, 0.0f, 0.0f, 1.0f}};
  static const VkImageSubresourceRange srr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmdbuf, m_display_texture.GetImage(), m_display_texture.GetLayout(), &cc, 1, &srr);
}

void GPU_HW_Vulkan::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();
  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::UpdateDisplay");

  if (g_settings.debugging.show_vram)
  {
    if (IsUsingMultisampling())
    {
      if (m_vram_dirty_rect.Intersects(
            Common::Rectangle<u32>::FromExtents(m_crtc_state.display_vram_left, m_crtc_state.display_vram_top,
                                                m_crtc_state.display_vram_width, m_crtc_state.display_vram_height)))
      {
        UpdateVRAMReadTexture();
      }

      m_host_display->SetDisplayTexture(&m_vram_read_texture, HostDisplayPixelFormat::RGBA8,
                                        m_vram_read_texture.GetWidth(), m_vram_read_texture.GetHeight(), 0, 0,
                                        m_vram_read_texture.GetWidth(), m_vram_read_texture.GetHeight());
    }
    else
    {
      m_host_display->SetDisplayTexture(&m_vram_texture, HostDisplayPixelFormat::RGBA8, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight(), 0, 0, m_vram_texture.GetWidth(),
                                        m_vram_texture.GetHeight());
    }
    m_host_display->SetDisplayParameters(VRAM_WIDTH, VRAM_HEIGHT, 0, 0, VRAM_WIDTH, VRAM_HEIGHT,
                                         static_cast<float>(VRAM_WIDTH) / static_cast<float>(VRAM_HEIGHT));
  }
  else
  {
    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         GetDisplayAspectRatio());

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
      if (IsUsingDownsampling())
      {
        DownsampleFramebuffer(m_vram_texture, scaled_vram_offset_x, scaled_vram_offset_y, scaled_display_width,
                              scaled_display_height);
      }
      else
      {
        m_host_display->SetDisplayTexture(&m_vram_texture, HostDisplayPixelFormat::RGBA8, m_vram_texture.GetWidth(),
                                          m_vram_texture.GetHeight(), scaled_vram_offset_x, scaled_vram_offset_y,
                                          scaled_display_width, scaled_display_height);
      }
    }
    else
    {
      EndRenderPass();

      const u32 reinterpret_field_offset = (interlaced != InterlacedRenderMode::None) ? GetInterlacedDisplayField() : 0;
      const u32 reinterpret_start_x = m_crtc_state.regs.X * resolution_scale;
      const u32 reinterpret_crop_left = (m_crtc_state.display_vram_left - m_crtc_state.regs.X) * resolution_scale;
      const u32 uniforms[4] = {reinterpret_start_x, scaled_vram_offset_y + reinterpret_field_offset,
                               reinterpret_crop_left, reinterpret_field_offset};

      m_display_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      Assert(scaled_display_width <= m_display_texture.GetWidth() &&
             scaled_display_height <= m_display_texture.GetHeight());

      BeginRenderPass((interlaced != InterlacedRenderMode::None) ? m_display_load_render_pass :
                                                                   m_display_discard_render_pass,
                      m_display_framebuffer, 0, 0, scaled_display_width, scaled_display_height);

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

      if (IsUsingDownsampling())
      {
        DownsampleFramebuffer(m_display_texture, 0, 0, scaled_display_width, scaled_display_height);
      }
      else
      {
        m_host_display->SetDisplayTexture(&m_display_texture, HostDisplayPixelFormat::RGBA8,
                                          m_display_texture.GetWidth(), m_display_texture.GetHeight(), 0, 0,
                                          scaled_display_width, scaled_display_height);
        RestoreGraphicsAPIState();
      }
    }
  }
}

void GPU_HW_Vulkan::ReadVRAM(u32 x, u32 y, u32 width, u32 height)
{
  if (IsUsingSoftwareRendererForReadbacks())
  {
    ReadSoftwareRendererVRAM(x, y, width, height);
    return;
  }

  // Get bounds with wrap-around handled.
  const Common::Rectangle<u32> copy_rect = GetVRAMTransferBounds(x, y, width, height);
  const u32 encoded_width = (copy_rect.GetWidth() + 1) / 2;
  const u32 encoded_height = copy_rect.GetHeight();

  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::ReadVRAM: %u %u %ux%u", x, y, width, height);

  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_vram_readback_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  // Work around Mali driver bug: set full framebuffer size for render area. The GPU crashes with a page fault if we use
  // the actual size we're rendering to...
  const u32 rp_width = std::max<u32>(16, encoded_width);
  const u32 rp_height = std::max<u32>(16, encoded_height);
  BeginRenderPass(m_vram_readback_render_pass, m_vram_readback_framebuffer, 0, 0, rp_width, rp_height);

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
  ExecuteCommandBuffer(true, true);
  m_vram_readback_staging_texture.ReadTexels(0, 0, encoded_width, encoded_height,
                                             &m_vram_shadow[copy_rect.top * VRAM_WIDTH + copy_rect.left],
                                             VRAM_WIDTH * sizeof(u16));
}

void GPU_HW_Vulkan::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  if (IsUsingSoftwareRendererForReadbacks())
    FillSoftwareRendererVRAM(x, y, width, height, color);

  GPU_HW::FillVRAM(x, y, width, height, color);

  BeginVRAMRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::FillVRAM: {%u,%u} %ux%u %08x", x, y, width, height,
                                            color);

  const VRAMFillUBOData uniforms = GetVRAMFillUBOData(x, y, width, height, color);
  vkCmdPushConstants(cmdbuf, m_no_samplers_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uniforms),
                     &uniforms);
  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_vram_fill_pipelines[BoolToUInt8(IsVRAMFillOversized(x, y, width, height))]
                                         [BoolToUInt8(IsInterlacedRenderingEnabled())]);

  const Common::Rectangle<u32> bounds(GetVRAMTransferBounds(x, y, width, height));
  Vulkan::Util::SetViewportAndScissor(cmdbuf, bounds.left * m_resolution_scale, bounds.top * m_resolution_scale,
                                      bounds.GetWidth() * m_resolution_scale, bounds.GetHeight() * m_resolution_scale);
  vkCmdDraw(cmdbuf, 3, 1, 0, 0);

  RestoreGraphicsAPIState();
}

void GPU_HW_Vulkan::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, bool set_mask, bool check_mask)
{
  if (IsUsingSoftwareRendererForReadbacks())
    UpdateSoftwareRendererVRAM(x, y, width, height, data, set_mask, check_mask);

  const Common::Rectangle<u32> bounds = GetVRAMTransferBounds(x, y, width, height);
  GPU_HW::UpdateVRAM(bounds.left, bounds.top, bounds.GetWidth(), bounds.GetHeight(), data, set_mask, check_mask);

  if (!check_mask)
  {
    const TextureReplacementTexture* rtex = g_texture_replacements.GetVRAMWriteReplacement(width, height, data);
    if (rtex && BlitVRAMReplacementTexture(rtex, x * m_resolution_scale, y * m_resolution_scale,
                                           width * m_resolution_scale, height * m_resolution_scale))
    {
      return;
    }
  }

  const u32 data_size = width * height * sizeof(u16);
  const u32 alignment = std::max<u32>(sizeof(u32), static_cast<u32>(m_use_ssbos_for_vram_writes ?
                                                                      g_vulkan_context->GetStorageBufferAlignment() :
                                                                      g_vulkan_context->GetTexelBufferAlignment()));
  if (!m_texture_stream_buffer.ReserveMemory(data_size, alignment))
  {
    Log_PerfPrintf("Executing command buffer while waiting for %u bytes in stream buffer", data_size);
    ExecuteCommandBuffer(false, true);
    if (!m_texture_stream_buffer.ReserveMemory(data_size, alignment))
    {
      Panic("Failed to allocate space in stream buffer for VRAM write");
      return;
    }
  }

  const u32 start_index = m_texture_stream_buffer.GetCurrentOffset() / sizeof(u16);
  std::memcpy(m_texture_stream_buffer.GetCurrentHostPointer(), data, data_size);
  m_texture_stream_buffer.CommitMemory(data_size);

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::UpdateVRAM: {%u,%u} %ux%u", x, y, width, height);

  BeginVRAMRenderPass();

  const VRAMWriteUBOData uniforms = GetVRAMWriteUBOData(x, y, width, height, start_index, set_mask, check_mask);
  vkCmdPushConstants(cmdbuf, m_vram_write_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uniforms),
                     &uniforms);
  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_vram_write_pipelines[BoolToUInt8(check_mask && !m_pgxp_depth_buffer)]);
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
  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::CopyVRAM: {%u, %u} {%u, %u} %ux%u", src_x, src_y,
                                            dst_x, dst_y, width, height);
  if (IsUsingSoftwareRendererForReadbacks())
    CopySoftwareRendererVRAM(src_x, src_y, dst_x, dst_y, width, height);

  if (UseVRAMCopyShader(src_x, src_y, dst_x, dst_y, width, height) || IsUsingMultisampling())
  {
    const Common::Rectangle<u32> src_bounds = GetVRAMTransferBounds(src_x, src_y, width, height);
    const Common::Rectangle<u32> dst_bounds = GetVRAMTransferBounds(dst_x, dst_y, width, height);
    if (m_vram_dirty_rect.Intersects(src_bounds))
      UpdateVRAMReadTexture();
    IncludeVRAMDirtyRectangle(dst_bounds);

    const VRAMCopyUBOData uniforms(GetVRAMCopyUBOData(src_x, src_y, dst_x, dst_y, width, height));
    const Common::Rectangle<u32> dst_bounds_scaled(dst_bounds * m_resolution_scale);

    BeginVRAMRenderPass();

    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_vram_copy_pipelines[BoolToUInt8(m_GPUSTAT.check_mask_before_draw && !m_pgxp_depth_buffer)]);
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
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::UpdateVRAMReadTexture");
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
  if (m_pgxp_depth_buffer)
    return;

  EndRenderPass();
  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::UpdateDepthBufferFromMaskBit");
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

void GPU_HW_Vulkan::ClearDepthBuffer()
{
  EndRenderPass();

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::ClearDepthBuffer");
  m_vram_depth_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  static const VkClearDepthStencilValue cds = {1.0f};
  static constexpr VkImageSubresourceRange dsrr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u};
  vkCmdClearDepthStencilImage(cmdbuf, m_vram_depth_texture.GetImage(), m_vram_depth_texture.GetLayout(), &cds, 1u,
                              &dsrr);

  m_vram_depth_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  m_last_depth_z = 1.0f;
}

bool GPU_HW_Vulkan::CreateTextureReplacementStreamBuffer()
{
  if (m_texture_replacment_stream_buffer.IsValid())
    return true;

  if (!m_texture_replacment_stream_buffer.Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_REPLACEMENT_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate texture replacement streaming buffer");
    return false;
  }

  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_texture_replacment_stream_buffer.GetBuffer(),
                              "Texture Replacement Stream Buffer");
  Vulkan::Util::SetObjectName(g_vulkan_context->GetDevice(), m_texture_replacment_stream_buffer.GetDeviceMemory(),
                              "Texture Replacement Stream Buffer Memory");

  return true;
}

bool GPU_HW_Vulkan::BlitVRAMReplacementTexture(const TextureReplacementTexture* tex, u32 dst_x, u32 dst_y, u32 width,
                                               u32 height)
{
  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::BlitVRAMReplacementTexture: {%u,%u} %ux%u", dst_x,
                                            dst_y, width, height);
  if (!CreateTextureReplacementStreamBuffer())
    return false;

  if (m_vram_write_replacement_texture.GetWidth() < tex->GetWidth() ||
      m_vram_write_replacement_texture.GetHeight() < tex->GetHeight())
  {
    if (!m_vram_write_replacement_texture.Create(tex->GetWidth(), tex->GetHeight(), 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                                 VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    {
      Log_ErrorPrint("Failed to create VRAM write replacement texture");
      return false;
    }
  }

  const u32 required_size = tex->GetWidth() * tex->GetHeight() * sizeof(u32);
  const u32 alignment = static_cast<u32>(g_vulkan_context->GetBufferImageGranularity());
  if (!m_texture_replacment_stream_buffer.ReserveMemory(required_size, alignment))
  {
    Log_PerfPrint("Executing command buffer while waiting for texture replacement buffer space");
    ExecuteCommandBuffer(false, true);
    if (!m_texture_replacment_stream_buffer.ReserveMemory(required_size, alignment))
    {
      Log_ErrorPrintf("Failed to allocate %u bytes from texture replacement streaming buffer", required_size);
      return false;
    }
  }

  // upload to buffer
  const u32 buffer_offset = m_texture_replacment_stream_buffer.GetCurrentOffset();
  std::memcpy(m_texture_replacment_stream_buffer.GetCurrentHostPointer(), tex->GetPixels(), required_size);
  m_texture_replacment_stream_buffer.CommitMemory(required_size);

  // buffer -> texture
  m_vram_write_replacement_texture.UpdateFromBuffer(cmdbuf, 0, 0, 0, 0, tex->GetWidth(), tex->GetHeight(),
                                                    m_texture_replacment_stream_buffer.GetBuffer(), buffer_offset);

  // texture -> vram
  const VkImageBlit blit = {
    {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
    {
      {0, 0, 0},
      {static_cast<int32_t>(tex->GetWidth()), static_cast<int32_t>(tex->GetHeight()), 1},
    },
    {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
    {{static_cast<int32_t>(dst_x), static_cast<int32_t>(dst_y), 0},
     {static_cast<int32_t>(dst_x + width), static_cast<int32_t>(dst_y + height), 1}},
  };
  m_vram_write_replacement_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdBlitImage(cmdbuf, m_vram_write_replacement_texture.GetImage(), m_vram_write_replacement_texture.GetLayout(),
                 m_vram_texture.GetImage(), m_vram_texture.GetLayout(), 1, &blit, VK_FILTER_LINEAR);
  m_vram_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  return true;
}

void GPU_HW_Vulkan::DownsampleFramebuffer(Vulkan::Texture& source, u32 left, u32 top, u32 width, u32 height)
{
  if (m_downsample_mode == GPUDownsampleMode::Adaptive)
    DownsampleFramebufferAdaptive(source, left, top, width, height);
  else
    DownsampleFramebufferBoxFilter(source, left, top, width, height);
}

void GPU_HW_Vulkan::DownsampleFramebufferBoxFilter(Vulkan::Texture& source, u32 left, u32 top, u32 width, u32 height)
{
  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope debugScope(cmdbuf, "GPU_HW_Vulkan::DownsampleFramebufferBoxFilter: {%u,%u} %ux%u",
                                            left, top, width, height);
  source.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_downsample_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  Assert(&source == &m_vram_texture || &source == &m_display_texture);
  VkDescriptorSet ds = (&source == &m_vram_texture) ? m_vram_read_descriptor_set : m_display_descriptor_set;

  const u32 ds_left = left / m_resolution_scale;
  const u32 ds_top = top / m_resolution_scale;
  const u32 ds_width = width / m_resolution_scale;
  const u32 ds_height = height / m_resolution_scale;

  static constexpr VkClearValue clear_color = {};
  BeginRenderPass(m_downsample_render_pass, m_downsample_mip_views[0].framebuffer, ds_left, ds_top, ds_width, ds_height,
                  &clear_color);
  Vulkan::Util::SetViewportAndScissor(cmdbuf, ds_left, ds_top, ds_width, ds_height);
  vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsample_first_pass_pipeline);
  vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_single_sampler_pipeline_layout, 0, 1, &ds, 0,
                          nullptr);
  vkCmdDraw(cmdbuf, 3, 1, 0, 0);
  EndRenderPass();

  m_downsample_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  RestoreGraphicsAPIState();

  m_host_display->SetDisplayTexture(&m_downsample_texture, HostDisplayPixelFormat::RGBA8,
                                    m_downsample_texture.GetWidth(), m_downsample_texture.GetHeight(), ds_left, ds_top,
                                    ds_width, ds_height);
}

void GPU_HW_Vulkan::DownsampleFramebufferAdaptive(Vulkan::Texture& source, u32 left, u32 top, u32 width, u32 height)
{
  const VkImageCopy copy{{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                         {static_cast<s32>(left), static_cast<s32>(top), 0},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
                         {static_cast<s32>(left), static_cast<s32>(top), 0},
                         {width, height, 1u}};

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  const Vulkan::Util::DebugScope outer_scope(cmdbuf, "Downsample Framebuffer Adaptive:");

  source.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  m_downsample_texture.TransitionSubresourcesToLayout(cmdbuf, 0, 1, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyImage(cmdbuf, source.GetImage(), source.GetLayout(), m_downsample_texture.GetImage(),
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  m_downsample_texture.TransitionSubresourcesToLayout(cmdbuf, 0, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // creating mip chain
  const u32 levels = m_downsample_texture.GetLevels();
  for (u32 level = 1; level < levels; level++)
  {
    const Vulkan::Util::DebugScope mip_scope(cmdbuf, "Generate Mip: %u", level);
    m_downsample_texture.TransitionSubresourcesToLayout(
      cmdbuf, level, 1, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    static constexpr VkClearValue clear_color = {};
    BeginRenderPass(m_downsample_render_pass, m_downsample_mip_views[level].framebuffer, 0, 0,
                    m_downsample_texture.GetMipWidth(level), m_downsample_texture.GetMipHeight(level), &clear_color);
    Vulkan::Util::SetViewportAndScissor(cmdbuf, left >> level, top >> level, width >> level, height >> level);
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      (level == 1) ? m_downsample_first_pass_pipeline : m_downsample_mid_pass_pipeline);
    vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsample_pipeline_layout, 0, 1,
                            &m_downsample_mip_views[level - 1].descriptor_set, 0, nullptr);

    const SmoothingUBOData ubo = GetSmoothingUBO(level, left, top, width, height, m_downsample_texture.GetWidth(),
                                                 m_downsample_texture.GetHeight());
    vkCmdPushConstants(cmdbuf, m_downsample_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(ubo), &ubo);

    vkCmdDraw(cmdbuf, 3, 1, 0, 0);

    EndRenderPass();

    m_downsample_texture.TransitionSubresourcesToLayout(
      cmdbuf, level, 1, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  // blur pass at lowest resolution
  {
    const Vulkan::Util::DebugScope blur_scope(cmdbuf, "Blur Pass at lowest resolution");
    const u32 last_level = levels - 1;

    m_downsample_weight_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    static constexpr VkClearValue clear_color = {};
    BeginRenderPass(m_downsample_weight_render_pass, m_downsample_weight_framebuffer, 0, 0,
                    m_downsample_texture.GetMipWidth(last_level), m_downsample_texture.GetMipHeight(last_level),
                    &clear_color);
    Vulkan::Util::SetViewportAndScissor(cmdbuf, left >> last_level, top >> last_level, width >> last_level,
                                        height >> last_level);
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsample_blur_pass_pipeline);
    vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsample_pipeline_layout, 0, 1,
                            &m_downsample_mip_views[last_level].descriptor_set, 0, nullptr);

    const SmoothingUBOData ubo = GetSmoothingUBO(last_level, left, top, width, height, m_downsample_texture.GetWidth(),
                                                 m_downsample_texture.GetHeight());
    vkCmdPushConstants(cmdbuf, m_downsample_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(ubo), &ubo);

    vkCmdDraw(cmdbuf, 3, 1, 0, 0);
    EndRenderPass();

    m_downsample_weight_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  // resolve pass
  {
    const Vulkan::Util::DebugScope resolve_scope(cmdbuf, "Resolve pass");
    m_display_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    BeginRenderPass(m_display_load_render_pass, m_display_framebuffer, left, top, width, height);
    Vulkan::Util::SetViewportAndScissor(cmdbuf, left, top, width, height);
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsample_composite_pass_pipeline);
    vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsample_composite_pipeline_layout, 0, 1,
                            &m_downsample_composite_descriptor_set, 0, nullptr);
    vkCmdDraw(cmdbuf, 3, 1, 0, 0);
    EndRenderPass();
    m_display_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  RestoreGraphicsAPIState();

  m_host_display->SetDisplayTexture(&m_display_texture, HostDisplayPixelFormat::RGBA8, m_display_texture.GetWidth(),
                                    m_display_texture.GetHeight(), left, top, width, height);
}

std::unique_ptr<GPU> GPU::CreateHardwareVulkanRenderer()
{
  return std::make_unique<GPU_HW_Vulkan>();
}
