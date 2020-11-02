#include "vulkan_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/scope_guard.h"
#include "common/vulkan/builders.h"
#include "common/vulkan/context.h"
#include "common/vulkan/shader_cache.h"
#include "common/vulkan/staging_texture.h"
#include "common/vulkan/stream_buffer.h"
#include "common/vulkan/swap_chain.h"
#include "common/vulkan/util.h"
#include <array>
#ifdef WITH_IMGUI
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#endif
#ifndef LIBRETRO
#include "postprocessing_shadergen.h"
#endif
Log_SetChannel(VulkanHostDisplay);

namespace FrontendCommon {

class VulkanHostDisplayTexture : public HostDisplayTexture
{
public:
  VulkanHostDisplayTexture(Vulkan::Texture texture, Vulkan::StagingTexture staging_texture)
    : m_texture(std::move(texture)), m_staging_texture(std::move(staging_texture))
  {
  }
  ~VulkanHostDisplayTexture() override = default;

  void* GetHandle() const override { return const_cast<Vulkan::Texture*>(&m_texture); }
  u32 GetWidth() const override { return m_texture.GetWidth(); }
  u32 GetHeight() const override { return m_texture.GetHeight(); }

  const Vulkan::Texture& GetTexture() const { return m_texture; }
  Vulkan::Texture& GetTexture() { return m_texture; }
  Vulkan::StagingTexture& GetStagingTexture() { return m_staging_texture; }

  static std::unique_ptr<VulkanHostDisplayTexture> Create(u32 width, u32 height, const void* data, u32 data_stride,
                                                          bool dynamic)
  {
    static constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    Vulkan::Texture texture;
    if (!texture.Create(width, height, 1, 1, format, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D,
                        VK_IMAGE_TILING_OPTIMAL, usage))
    {
      return {};
    }

    Vulkan::StagingTexture staging_texture;
    if (data || dynamic)
    {
      if (!staging_texture.Create(dynamic ? Vulkan::StagingBuffer::Type::Mutable : Vulkan::StagingBuffer::Type::Upload,
                                  format, width, height))
      {
        return {};
      }
    }

    texture.TransitionToLayout(g_vulkan_context->GetCurrentCommandBuffer(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (data)
    {
      staging_texture.WriteTexels(0, 0, width, height, data, data_stride);
      staging_texture.CopyToTexture(g_vulkan_context->GetCurrentCommandBuffer(), 0, 0, texture, 0, 0, 0, 0, width,
                                    height);
    }
    else
    {
      // clear it instead so we don't read uninitialized data (and keep the validation layer happy!)
      static constexpr VkClearColorValue ccv = {};
      static constexpr VkImageSubresourceRange isr = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
      vkCmdClearColorImage(g_vulkan_context->GetCurrentCommandBuffer(), texture.GetImage(), texture.GetLayout(), &ccv,
                           1u, &isr);
    }

    texture.TransitionToLayout(g_vulkan_context->GetCurrentCommandBuffer(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // don't need to keep the staging texture around if we're not dynamic
    if (!dynamic)
      staging_texture.Destroy(true);

    return std::make_unique<VulkanHostDisplayTexture>(std::move(texture), std::move(staging_texture));
  }

private:
  Vulkan::Texture m_texture;
  Vulkan::StagingTexture m_staging_texture;
};

VulkanHostDisplay::VulkanHostDisplay() = default;

VulkanHostDisplay::~VulkanHostDisplay()
{
  AssertMsg(!g_vulkan_context, "Context should have been destroyed by now");
  AssertMsg(!m_swap_chain, "Swap chain should have been destroyed by now");
}

HostDisplay::RenderAPI VulkanHostDisplay::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::Vulkan;
}

void* VulkanHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* VulkanHostDisplay::GetRenderContext() const
{
  return nullptr;
}

bool VulkanHostDisplay::ChangeRenderWindow(const WindowInfo& new_wi)
{
  if (new_wi.type == WindowInfo::Type::Surfaceless)
  {
    g_vulkan_context->ExecuteCommandBuffer(true);
    m_swap_chain.reset();
    return true;
  }

  WindowInfo wi_copy(new_wi);
  VkSurfaceKHR surface = Vulkan::SwapChain::CreateVulkanSurface(g_vulkan_context->GetVulkanInstance(), wi_copy);
  if (surface == VK_NULL_HANDLE)
  {
    Log_ErrorPrintf("Failed to create new surface for swap chain");
    return false;
  }

  m_swap_chain = Vulkan::SwapChain::Create(wi_copy, surface, false);
  if (!m_swap_chain)
  {
    Log_ErrorPrintf("Failed to create swap chain");
    return false;
  }

  m_window_info = wi_copy;
  m_window_info.surface_width = m_swap_chain->GetWidth();
  m_window_info.surface_height = m_swap_chain->GetHeight();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
  {
    ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
    ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);
  }
#endif

  return true;
}

void VulkanHostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  g_vulkan_context->WaitForGPUIdle();

  if (!m_swap_chain->ResizeSwapChain(new_window_width, new_window_height))
    Panic("Failed to resize swap chain");

  m_window_info.surface_width = m_swap_chain->GetWidth();
  m_window_info.surface_height = m_swap_chain->GetHeight();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
  {
    ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
    ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);
  }
#endif
}

bool VulkanHostDisplay::SupportsFullscreen() const
{
  return false;
}

bool VulkanHostDisplay::IsFullscreen()
{
  return false;
}

bool VulkanHostDisplay::SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate)
{
  return false;
}

void VulkanHostDisplay::DestroyRenderSurface()
{
  m_window_info = {};
  m_swap_chain.reset();
}

std::unique_ptr<HostDisplayTexture> VulkanHostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                     u32 data_stride, bool dynamic)
{
  return VulkanHostDisplayTexture::Create(width, height, data, data_stride, dynamic);
}

void VulkanHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                      const void* data, u32 data_stride)
{
  VulkanHostDisplayTexture* vk_texture = static_cast<VulkanHostDisplayTexture*>(texture);

  Vulkan::StagingTexture* staging_texture;
  if (vk_texture->GetStagingTexture().IsValid())
  {
    staging_texture = &vk_texture->GetStagingTexture();
  }
  else
  {
    // TODO: This should use a stream buffer instead for speed.
    if (m_upload_staging_texture.IsValid())
      m_upload_staging_texture.Flush();

    if ((m_upload_staging_texture.GetWidth() < width || m_upload_staging_texture.GetHeight() < height) &&
        !m_upload_staging_texture.Create(Vulkan::StagingBuffer::Type::Upload, VK_FORMAT_R8G8B8A8_UNORM, width, height))
    {
      Panic("Failed to create upload staging texture");
    }

    staging_texture = &m_upload_staging_texture;
  }

  staging_texture->WriteTexels(0, 0, width, height, data, data_stride);
  staging_texture->CopyToTexture(0, 0, vk_texture->GetTexture(), x, y, 0, 0, width, height);
}

bool VulkanHostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                        u32 out_data_stride)
{
  Vulkan::Texture* texture = static_cast<Vulkan::Texture*>(const_cast<void*>(texture_handle));

  if ((m_readback_staging_texture.GetWidth() < width || m_readback_staging_texture.GetHeight() < height) &&
      !m_readback_staging_texture.Create(Vulkan::StagingBuffer::Type::Readback, VK_FORMAT_R8G8B8A8_UNORM, width,
                                         height))
  {
    return false;
  }

  m_readback_staging_texture.CopyFromTexture(*texture, x, y, 0, 0, 0, 0, width, height);
  m_readback_staging_texture.ReadTexels(0, 0, width, height, out_data, out_data_stride);
  return true;
}

void VulkanHostDisplay::SetVSync(bool enabled)
{
  if (!m_swap_chain)
    return;

  // This swap chain should not be used by the current buffer, thus safe to destroy.
  g_vulkan_context->WaitForGPUIdle();
  m_swap_chain->SetVSync(enabled);
}

bool VulkanHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device)
{
  if (!Vulkan::Context::Create(adapter_name, &wi, &m_swap_chain, debug_device, false))
  {
    Log_ErrorPrintf("Failed to create Vulkan context");
    return false;
  }

  m_window_info = wi;
  if (m_swap_chain)
  {
    m_window_info.surface_width = m_swap_chain->GetWidth();
    m_window_info.surface_height = m_swap_chain->GetHeight();
  }

  return true;
}

bool VulkanHostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device)
{
  Vulkan::ShaderCache::Create(shader_cache_directory, debug_device);

  if (!CreateResources())
    return false;

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext() && !CreateImGuiContext())
    return false;
#endif

  return true;
}

bool VulkanHostDisplay::HasRenderDevice() const
{
  return static_cast<bool>(g_vulkan_context);
}

bool VulkanHostDisplay::HasRenderSurface() const
{
  return static_cast<bool>(m_swap_chain);
}

VkRenderPass VulkanHostDisplay::GetRenderPassForDisplay() const
{
  return m_swap_chain->GetClearRenderPass();
}

bool VulkanHostDisplay::CreateResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
#version 450 core

layout(push_constant) uniform PushConstants {
  uniform vec4 u_src_rect;
};

layout(location = 0) out vec2 v_tex0;

void main()
{
  vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  gl_Position = vec4(pos * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
  gl_Position.y = -gl_Position.y;
}
)";

  static constexpr char display_fragment_shader_src[] = R"(
#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(location = 0) in vec2 v_tex0;
layout(location = 0) out vec4 o_col0;

void main()
{
  o_col0 = vec4(texture(samp0, v_tex0).rgb, 1.0);
}
)";

  static constexpr char cursor_fragment_shader_src[] = R"(
#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(location = 0) in vec2 v_tex0;
layout(location = 0) out vec4 o_col0;

void main()
{
  o_col0 = texture(samp0, v_tex0);
}
)";

  VkDevice device = g_vulkan_context->GetDevice();
  VkPipelineCache pipeline_cache = g_vulkan_shader_cache->GetPipelineCache();

  Vulkan::DescriptorSetLayoutBuilder dslbuilder;
  dslbuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_descriptor_set_layout = dslbuilder.Create(device);
  if (m_descriptor_set_layout == VK_NULL_HANDLE)
    return false;

  Vulkan::PipelineLayoutBuilder plbuilder;
  plbuilder.AddDescriptorSet(m_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants));
  m_pipeline_layout = plbuilder.Create(device);
  if (m_pipeline_layout == VK_NULL_HANDLE)
    return false;

#ifndef LIBRETRO

  dslbuilder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_post_process_descriptor_set_layout = dslbuilder.Create(device);
  if (m_post_process_descriptor_set_layout == VK_NULL_HANDLE)
    return false;

  plbuilder.AddDescriptorSet(m_post_process_descriptor_set_layout);
  plbuilder.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             PostProcessingShader::PUSH_CONSTANT_SIZE_THRESHOLD);
  m_post_process_pipeline_layout = plbuilder.Create(device);
  if (m_post_process_pipeline_layout == VK_NULL_HANDLE)
    return false;

  dslbuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
  dslbuilder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
  m_post_process_ubo_descriptor_set_layout = dslbuilder.Create(device);
  if (m_post_process_ubo_descriptor_set_layout == VK_NULL_HANDLE)
    return false;

  plbuilder.AddDescriptorSet(m_post_process_ubo_descriptor_set_layout);
  m_post_process_ubo_pipeline_layout = plbuilder.Create(device);
  if (m_post_process_ubo_pipeline_layout == VK_NULL_HANDLE)
    return false;

#endif

  VkShaderModule vertex_shader = g_vulkan_shader_cache->GetVertexShader(fullscreen_quad_vertex_shader);
  if (vertex_shader == VK_NULL_HANDLE)
    return false;

  VkShaderModule display_fragment_shader = g_vulkan_shader_cache->GetFragmentShader(display_fragment_shader_src);
  VkShaderModule cursor_fragment_shader = g_vulkan_shader_cache->GetFragmentShader(cursor_fragment_shader_src);
  if (display_fragment_shader == VK_NULL_HANDLE || cursor_fragment_shader == VK_NULL_HANDLE)
    return false;

  Vulkan::GraphicsPipelineBuilder gpbuilder;
  gpbuilder.SetVertexShader(vertex_shader);
  gpbuilder.SetFragmentShader(display_fragment_shader);
  gpbuilder.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  gpbuilder.SetNoCullRasterizationState();
  gpbuilder.SetNoDepthTestState();
  gpbuilder.SetNoBlendingState();
  gpbuilder.SetDynamicViewportAndScissorState();
  gpbuilder.SetPipelineLayout(m_pipeline_layout);
  gpbuilder.SetRenderPass(GetRenderPassForDisplay(), 0);

  m_display_pipeline = gpbuilder.Create(device, pipeline_cache, false);
  if (m_display_pipeline == VK_NULL_HANDLE)
    return false;

  gpbuilder.SetFragmentShader(cursor_fragment_shader);
  gpbuilder.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                               VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
  m_cursor_pipeline = gpbuilder.Create(device, pipeline_cache, false);
  if (m_cursor_pipeline == VK_NULL_HANDLE)
    return false;

  // don't need these anymore
  vkDestroyShaderModule(device, vertex_shader, nullptr);
  vkDestroyShaderModule(device, display_fragment_shader, nullptr);
  vkDestroyShaderModule(device, cursor_fragment_shader, nullptr);

  Vulkan::SamplerBuilder sbuilder;
  sbuilder.SetPointSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  m_point_sampler = sbuilder.Create(device, true);
  if (m_point_sampler == VK_NULL_HANDLE)
    return false;

  sbuilder.SetLinearSampler(false, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
  m_linear_sampler = sbuilder.Create(device);
  if (m_linear_sampler == VK_NULL_HANDLE)
    return false;

  return true;
}

void VulkanHostDisplay::DestroyResources()
{
#ifndef LIBRETRO
  Vulkan::Util::SafeDestroyPipelineLayout(m_post_process_pipeline_layout);
  Vulkan::Util::SafeDestroyPipelineLayout(m_post_process_ubo_pipeline_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_post_process_descriptor_set_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_post_process_ubo_descriptor_set_layout);
  m_post_processing_input_texture.Destroy(false);
  Vulkan::Util::SafeDestroyFramebuffer(m_post_processing_input_framebuffer);
  m_post_processing_stages.clear();
  m_post_processing_ubo.Destroy(true);
  m_post_processing_chain.ClearStages();
#endif

  m_readback_staging_texture.Destroy(false);
  m_upload_staging_texture.Destroy(false);

  Vulkan::Util::SafeDestroyPipeline(m_display_pipeline);
  Vulkan::Util::SafeDestroyPipeline(m_cursor_pipeline);
  Vulkan::Util::SafeDestroyPipelineLayout(m_pipeline_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_descriptor_set_layout);
  Vulkan::Util::SafeDestroySampler(m_point_sampler);
  Vulkan::Util::SafeDestroySampler(m_linear_sampler);
}

void VulkanHostDisplay::DestroyImGuiContext()
{
#ifdef WITH_IMGUI
  ImGui_ImplVulkan_Shutdown();
#endif
}

void VulkanHostDisplay::DestroyRenderDevice()
{
  if (!g_vulkan_context)
    return;

  g_vulkan_context->WaitForGPUIdle();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
    DestroyImGuiContext();
#endif

  DestroyResources();

  Vulkan::ShaderCache::Destroy();
  DestroyRenderSurface();
  Vulkan::Context::Destroy();
}

bool VulkanHostDisplay::MakeRenderContextCurrent()
{
  return true;
}

bool VulkanHostDisplay::DoneRenderContextCurrent()
{
  return true;
}

bool VulkanHostDisplay::CreateImGuiContext()
{
#ifdef WITH_IMGUI
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);

  ImGui_ImplVulkan_InitInfo vii = {};
  vii.Instance = g_vulkan_context->GetVulkanInstance();
  vii.PhysicalDevice = g_vulkan_context->GetPhysicalDevice();
  vii.Device = g_vulkan_context->GetDevice();
  vii.QueueFamily = g_vulkan_context->GetGraphicsQueueFamilyIndex();
  vii.Queue = g_vulkan_context->GetGraphicsQueue();
  vii.PipelineCache = g_vulkan_shader_cache->GetPipelineCache();
  vii.MinImageCount = m_swap_chain->GetImageCount();
  vii.ImageCount = m_swap_chain->GetImageCount();
  vii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

  if (!ImGui_ImplVulkan_Init(&vii, m_swap_chain->GetClearRenderPass()) ||
      !ImGui_ImplVulkan_CreateFontsTexture(g_vulkan_context->GetCurrentCommandBuffer()))
  {
    return false;
  }

  ImGui_ImplVulkan_NewFrame();
#endif

  return true;
}

bool VulkanHostDisplay::Render()
{
  VkResult res = m_swap_chain->AcquireNextImage();
  if (res != VK_SUCCESS)
  {
    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
    {
      ResizeRenderWindow(0, 0);
      res = m_swap_chain->AcquireNextImage();
    }

    // This can happen when multiple resize events happen in quick succession.
    // In this case, just wait until the next frame to try again.
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
    {
      // Still submit the command buffer, otherwise we'll end up with several frames waiting.
      LOG_VULKAN_ERROR(res, "vkAcquireNextImageKHR() failed: ");
      g_vulkan_context->ExecuteCommandBuffer(false);
      return false;
    }
  }

  VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();
  Vulkan::Texture& swap_chain_texture = m_swap_chain->GetCurrentTexture();

  // Swap chain images start in undefined
  swap_chain_texture.OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  swap_chain_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  RenderDisplay();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
    RenderImGui();
#endif

  RenderSoftwareCursor();

  vkCmdEndRenderPass(cmdbuffer);

  swap_chain_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  g_vulkan_context->SubmitCommandBuffer(m_swap_chain->GetImageAvailableSemaphore(),
                                        m_swap_chain->GetRenderingFinishedSemaphore(), m_swap_chain->GetSwapChain(),
                                        m_swap_chain->GetCurrentImageIndex());
  g_vulkan_context->MoveToNextCommandBuffer();

#ifdef WITH_IMGUI
  if (ImGui::GetCurrentContext())
    ImGui_ImplVulkan_NewFrame();
#endif

  return true;
}

void VulkanHostDisplay::BeginSwapChainRenderPass(VkFramebuffer framebuffer)
{
  const VkClearValue clear_value = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
  const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                    nullptr,
                                    m_swap_chain->GetClearRenderPass(),
                                    framebuffer,
                                    {{0, 0}, {m_swap_chain->GetWidth(), m_swap_chain->GetHeight()}},
                                    1u,
                                    &clear_value};
  vkCmdBeginRenderPass(g_vulkan_context->GetCurrentCommandBuffer(), &rp, VK_SUBPASS_CONTENTS_INLINE);
}

void VulkanHostDisplay::RenderDisplay()
{
  if (!HasDisplayTexture())
  {
    BeginSwapChainRenderPass(m_swap_chain->GetCurrentFramebuffer());
    return;
  }

  const auto [left, top, width, height] = CalculateDrawRect(GetWindowWidth(), GetWindowHeight(), m_display_top_margin);

#ifndef LIBRETRO
  if (!m_post_processing_chain.IsEmpty())
  {
    ApplyPostProcessingChain(left, top, width, height, m_display_texture_handle, m_display_texture_width,
                             m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                             m_display_texture_view_width, m_display_texture_view_height);
    return;
  }
#endif

  BeginSwapChainRenderPass(m_swap_chain->GetCurrentFramebuffer());
  RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width, m_display_texture_height,
                m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                m_display_texture_view_height, m_display_linear_filtering);
}

void VulkanHostDisplay::RenderDisplay(s32 left, s32 top, s32 width, s32 height, void* texture_handle, u32 texture_width,
                                      s32 texture_height, s32 texture_view_x, s32 texture_view_y,
                                      s32 texture_view_width, s32 texture_view_height, bool linear_filter)
{
  VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();

  VkDescriptorSet ds = g_vulkan_context->AllocateDescriptorSet(m_descriptor_set_layout);
  if (ds == VK_NULL_HANDLE)
  {
    Log_ErrorPrintf("Skipping rendering display because of no descriptor set");
    return;
  }

  {
    const Vulkan::Texture* vktex = static_cast<Vulkan::Texture*>(texture_handle);
    Vulkan::DescriptorSetUpdateBuilder dsupdate;
    dsupdate.AddCombinedImageSamplerDescriptorWrite(
      ds, 0, vktex->GetView(), linear_filter ? m_linear_sampler : m_point_sampler, vktex->GetLayout());
    dsupdate.Update(g_vulkan_context->GetDevice());
  }

  const PushConstants pc{static_cast<float>(texture_view_x) / static_cast<float>(texture_width),
                         static_cast<float>(texture_view_y) / static_cast<float>(texture_height),
                         (static_cast<float>(texture_view_width) - 0.5f) / static_cast<float>(texture_width),
                         (static_cast<float>(texture_view_height) - 0.5f) / static_cast<float>(texture_height)};

  vkCmdBindPipeline(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_display_pipeline);
  vkCmdPushConstants(cmdbuffer, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
  vkCmdBindDescriptorSets(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &ds, 0, nullptr);
  Vulkan::Util::SetViewportAndScissor(cmdbuffer, left, top, width, height);
  vkCmdDraw(cmdbuffer, 3, 1, 0, 0);
}

void VulkanHostDisplay::RenderImGui()
{
#ifdef WITH_IMGUI
  ImGui::Render();
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), g_vulkan_context->GetCurrentCommandBuffer());
#endif
}

void VulkanHostDisplay::RenderSoftwareCursor()
{
  if (!HasSoftwareCursor())
    return;

  const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
  RenderSoftwareCursor(left, top, width, height, m_cursor_texture.get());
}

void VulkanHostDisplay::RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, HostDisplayTexture* texture)
{
  VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();

  VkDescriptorSet ds = g_vulkan_context->AllocateDescriptorSet(m_descriptor_set_layout);
  if (ds == VK_NULL_HANDLE)
  {
    Log_ErrorPrintf("Skipping rendering software cursor because of no descriptor set");
    return;
  }

  {
    Vulkan::DescriptorSetUpdateBuilder dsupdate;
    dsupdate.AddCombinedImageSamplerDescriptorWrite(
      ds, 0, static_cast<VulkanHostDisplayTexture*>(texture)->GetTexture().GetView(), m_linear_sampler);
    dsupdate.Update(g_vulkan_context->GetDevice());
  }

  const PushConstants pc{0.0f, 0.0f, 1.0f, 1.0f};
  vkCmdBindPipeline(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cursor_pipeline);
  vkCmdPushConstants(cmdbuffer, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
  vkCmdBindDescriptorSets(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &ds, 0, nullptr);
  Vulkan::Util::SetViewportAndScissor(cmdbuffer, left, top, width, height);
  vkCmdDraw(cmdbuffer, 3, 1, 0, 0);
}

std::vector<std::string> VulkanHostDisplay::EnumerateAdapterNames()
{
  if (g_vulkan_context)
    return Vulkan::Context::EnumerateGPUNames(g_vulkan_context->GetVulkanInstance());

  if (Vulkan::LoadVulkanLibrary())
  {
    Common::ScopeGuard lib_guard([]() { Vulkan::UnloadVulkanLibrary(); });

    VkInstance instance = Vulkan::Context::CreateVulkanInstance(false, false, false);
    if (instance != VK_NULL_HANDLE)
    {
      Common::ScopeGuard instance_guard([&instance]() { vkDestroyInstance(instance, nullptr); });

      if (Vulkan::LoadVulkanInstanceFunctions(instance))
        return Vulkan::Context::EnumerateGPUNames(instance);
    }
  }

  return {};
}

#ifndef LIBRETRO

VulkanHostDisplay::PostProcessingStage::PostProcessingStage(PostProcessingStage&& move)
  : pipeline(move.pipeline), output_framebuffer(move.output_framebuffer),
    output_texture(std::move(move.output_texture)), uniforms_size(move.uniforms_size)
{
  move.output_framebuffer = VK_NULL_HANDLE;
  move.pipeline = VK_NULL_HANDLE;
  move.uniforms_size = 0;
}

VulkanHostDisplay::PostProcessingStage::~PostProcessingStage()
{
  if (output_framebuffer != VK_NULL_HANDLE)
    g_vulkan_context->DeferFramebufferDestruction(output_framebuffer);

  output_texture.Destroy(true);
  if (pipeline != VK_NULL_HANDLE)
    g_vulkan_context->DeferPipelineDestruction(pipeline);
}

bool VulkanHostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  g_vulkan_context->ExecuteCommandBuffer(true);

  if (config.empty())
  {
    m_post_processing_stages.clear();
    m_post_processing_chain.ClearStages();
    return true;
  }

  if (!m_post_processing_chain.CreateFromString(config))
    return false;

  m_post_processing_stages.clear();

  FrontendCommon::PostProcessingShaderGen shadergen(HostDisplay::RenderAPI::Vulkan, false);
  bool only_use_push_constants = true;

  for (u32 i = 0; i < m_post_processing_chain.GetStageCount(); i++)
  {
    const PostProcessingShader& shader = m_post_processing_chain.GetShaderStage(i);
    const std::string vs = shadergen.GeneratePostProcessingVertexShader(shader);
    const std::string ps = shadergen.GeneratePostProcessingFragmentShader(shader);
    const bool use_push_constants = shader.UsePushConstants();
    only_use_push_constants &= use_push_constants;

    PostProcessingStage stage;
    stage.uniforms_size = shader.GetUniformsSize();

    VkShaderModule vs_mod = g_vulkan_shader_cache->GetVertexShader(vs);
    VkShaderModule fs_mod = g_vulkan_shader_cache->GetFragmentShader(ps);
    if (vs_mod == VK_NULL_HANDLE || fs_mod == VK_NULL_HANDLE)
    {
      Log_ErrorPrintf("Failed to compile one or more post-processing shaders, disabling.");

      if (vs_mod != VK_NULL_HANDLE)
        vkDestroyShaderModule(g_vulkan_context->GetDevice(), vs_mod, nullptr);
      if (fs_mod != VK_NULL_HANDLE)
        vkDestroyShaderModule(g_vulkan_context->GetDevice(), vs_mod, nullptr);

      m_post_processing_stages.clear();
      m_post_processing_chain.ClearStages();
      return false;
    }

    Vulkan::GraphicsPipelineBuilder gpbuilder;
    gpbuilder.SetVertexShader(vs_mod);
    gpbuilder.SetFragmentShader(fs_mod);
    gpbuilder.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    gpbuilder.SetNoCullRasterizationState();
    gpbuilder.SetNoDepthTestState();
    gpbuilder.SetNoBlendingState();
    gpbuilder.SetDynamicViewportAndScissorState();
    gpbuilder.SetPipelineLayout(use_push_constants ? m_post_process_pipeline_layout :
                                                     m_post_process_ubo_pipeline_layout);
    gpbuilder.SetRenderPass(GetRenderPassForDisplay(), 0);

    stage.pipeline = gpbuilder.Create(g_vulkan_context->GetDevice(), g_vulkan_shader_cache->GetPipelineCache());
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), vs_mod, nullptr);
    vkDestroyShaderModule(g_vulkan_context->GetDevice(), fs_mod, nullptr);
    if (!stage.pipeline)
    {
      Log_ErrorPrintf("Failed to compile one or more post-processing pipelines, disabling.");
      m_post_processing_stages.clear();
      m_post_processing_chain.ClearStages();
      return false;
    }

    m_post_processing_stages.push_back(std::move(stage));
  }

  constexpr u32 UBO_SIZE = 1 * 1024 * 1024;
  if (!only_use_push_constants && m_post_processing_ubo.GetCurrentSize() < UBO_SIZE &&
      !m_post_processing_ubo.Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, UBO_SIZE))
  {
    Log_ErrorPrintf("Failed to allocate %u byte uniform buffer for postprocessing", UBO_SIZE);
    m_post_processing_stages.clear();
    m_post_processing_chain.ClearStages();
    return false;
  }

  return true;
}

bool VulkanHostDisplay::CheckPostProcessingRenderTargets(u32 target_width, u32 target_height)
{
  DebugAssert(!m_post_processing_stages.empty());

  if (m_post_processing_input_texture.GetWidth() != target_width ||
      m_post_processing_input_texture.GetHeight() != target_height)
  {
    if (m_post_processing_input_framebuffer != VK_NULL_HANDLE)
    {
      g_vulkan_context->DeferFramebufferDestruction(m_post_processing_input_framebuffer);
      m_post_processing_input_framebuffer = VK_NULL_HANDLE;
    }

    if (!m_post_processing_input_texture.Create(target_width, target_height, 1, 1, m_swap_chain->GetTextureFormat(),
                                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        (m_post_processing_input_framebuffer =
           m_post_processing_input_texture.CreateFramebuffer(GetRenderPassForDisplay())) == VK_NULL_HANDLE)
    {
      return false;
    }
  }

  const u32 target_count = (static_cast<u32>(m_post_processing_stages.size()) - 1);
  for (u32 i = 0; i < target_count; i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    if (pps.output_texture.GetWidth() != target_width || pps.output_texture.GetHeight() != target_height)
    {
      if (pps.output_framebuffer != VK_NULL_HANDLE)
      {
        g_vulkan_context->DeferFramebufferDestruction(pps.output_framebuffer);
        pps.output_framebuffer = VK_NULL_HANDLE;
      }

      if (!pps.output_texture.Create(target_width, target_height, 1, 1, m_swap_chain->GetTextureFormat(),
                                     VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
          (pps.output_framebuffer = pps.output_texture.CreateFramebuffer(GetRenderPassForDisplay())) == VK_NULL_HANDLE)
      {
        return false;
      }
    }
  }

  return true;
}

void VulkanHostDisplay::ApplyPostProcessingChain(s32 final_left, s32 final_top, s32 final_width, s32 final_height,
                                                 void* texture_handle, u32 texture_width, s32 texture_height,
                                                 s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                                 s32 texture_view_height)
{
  if (!CheckPostProcessingRenderTargets(m_swap_chain->GetWidth(), m_swap_chain->GetHeight()))
  {
    BeginSwapChainRenderPass(m_swap_chain->GetCurrentFramebuffer());
    RenderDisplay(final_left, final_top, final_width, final_height, texture_handle, texture_width, texture_height,
                  texture_view_x, texture_view_y, texture_view_width, texture_view_height, m_display_linear_filtering);
    return;
  }

  // downsample/upsample - use same viewport for remainder
  VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();
  m_post_processing_input_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  BeginSwapChainRenderPass(m_post_processing_input_framebuffer);
  RenderDisplay(final_left, final_top, final_width, final_height, texture_handle, texture_width, texture_height,
                texture_view_x, texture_view_y, texture_view_width, texture_view_height, m_display_linear_filtering);
  vkCmdEndRenderPass(cmdbuffer);
  m_post_processing_input_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  texture_handle = &m_post_processing_input_texture;
  texture_width = m_post_processing_input_texture.GetWidth();
  texture_height = m_post_processing_input_texture.GetHeight();
  texture_view_x = final_left;
  texture_view_y = final_top;
  texture_view_width = final_width;
  texture_view_height = final_height;

  const u32 final_stage = static_cast<u32>(m_post_processing_stages.size()) - 1u;
  for (u32 i = 0; i < static_cast<u32>(m_post_processing_stages.size()); i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    if (i != final_stage)
    {
      pps.output_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      BeginSwapChainRenderPass(pps.output_framebuffer);
    }
    else
    {
      BeginSwapChainRenderPass(m_swap_chain->GetCurrentFramebuffer());
    }

    const bool use_push_constants = m_post_processing_chain.GetShaderStage(i).UsePushConstants();
    VkDescriptorSet ds = g_vulkan_context->AllocateDescriptorSet(
      use_push_constants ? m_post_process_descriptor_set_layout : m_post_process_ubo_descriptor_set_layout);
    if (ds == VK_NULL_HANDLE)
    {
      Log_ErrorPrintf("Skipping rendering display because of no descriptor set");
      return;
    }

    const Vulkan::Texture* vktex = static_cast<Vulkan::Texture*>(texture_handle);
    Vulkan::DescriptorSetUpdateBuilder dsupdate;
    dsupdate.AddCombinedImageSamplerDescriptorWrite(ds, 1, vktex->GetView(), m_point_sampler, vktex->GetLayout());

    if (use_push_constants)
    {
      u8 buffer[PostProcessingShader::PUSH_CONSTANT_SIZE_THRESHOLD];
      Assert(pps.uniforms_size <= sizeof(buffer));
      m_post_processing_chain.GetShaderStage(i).FillUniformBuffer(
        buffer, texture_width, texture_height, texture_view_x, texture_view_y, texture_view_width, texture_view_height,
        texture_width, texture_width, 0.0f);

      vkCmdPushConstants(cmdbuffer, m_post_process_pipeline_layout,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pps.uniforms_size, buffer);

      dsupdate.Update(g_vulkan_context->GetDevice());
      vkCmdBindDescriptorSets(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_post_process_pipeline_layout, 0, 1, &ds, 0,
                              nullptr);
    }
    else
    {
      if (!m_post_processing_ubo.ReserveMemory(pps.uniforms_size,
                                               static_cast<u32>(g_vulkan_context->GetUniformBufferAlignment())))
      {
        Panic("Failed to reserve space in post-processing UBO");
      }

      const u32 offset = m_post_processing_ubo.GetCurrentOffset();
      m_post_processing_chain.GetShaderStage(i).FillUniformBuffer(
        m_post_processing_ubo.GetCurrentHostPointer(), texture_width, texture_height, texture_view_x, texture_view_y,
        texture_view_width, texture_view_height, GetWindowWidth(), GetWindowHeight(), 0.0f);
      m_post_processing_ubo.CommitMemory(pps.uniforms_size);

      dsupdate.AddBufferDescriptorWrite(ds, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                        m_post_processing_ubo.GetBuffer(), 0, pps.uniforms_size);
      dsupdate.Update(g_vulkan_context->GetDevice());
      vkCmdBindDescriptorSets(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_post_process_ubo_pipeline_layout, 0, 1, &ds,
                              1, &offset);
    }

    vkCmdBindPipeline(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pps.pipeline);

    vkCmdDraw(cmdbuffer, 3, 1, 0, 0);

    if (i != final_stage)
    {
      vkCmdEndRenderPass(cmdbuffer);
      pps.output_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      texture_handle = &pps.output_texture;
    }
  }
}

#else // LIBRETRO

bool VulkanHostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  return false;
}

#endif

} // namespace FrontendCommon
