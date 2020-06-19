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
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <array>
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

bool VulkanHostDisplay::RecreateSwapChain(const WindowInfo& new_wi)
{
  Assert(!m_swap_chain);

  VkSurfaceKHR surface = Vulkan::SwapChain::CreateVulkanSurface(g_vulkan_context->GetVulkanInstance(), new_wi);
  if (surface == VK_NULL_HANDLE)
  {
    Log_ErrorPrintf("Failed to create new surface for swap chain");
    return false;
  }

  m_swap_chain = Vulkan::SwapChain::Create(new_wi, surface, false);
  if (!m_swap_chain)
  {
    Log_ErrorPrintf("Failed to create swap chain");
    return false;
  }

  return true;
}

void VulkanHostDisplay::ResizeSwapChain(u32 new_width, u32 new_height)
{
  g_vulkan_context->WaitForGPUIdle();

  if (!m_swap_chain->ResizeSwapChain(new_width, new_height))
    Panic("Failed to resize swap chain");

  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_swap_chain->GetWidth());
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_swap_chain->GetHeight());
}

void VulkanHostDisplay::DestroySwapChain()
{
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
  // This swap chain should not be used by the current buffer, thus safe to destroy.
  g_vulkan_context->WaitForGPUIdle();
  m_swap_chain->SetVSync(enabled);
}

bool VulkanHostDisplay::CreateContextAndSwapChain(const WindowInfo& wi, std::string_view gpu_name, bool debug_device)
{
  if (!Vulkan::Context::Create(gpu_name, &wi, &m_swap_chain, debug_device, false))
  {
    Log_ErrorPrintf("Failed to create Vulkan context");
    return false;
  }

  return true;
}

void VulkanHostDisplay::CreateShaderCache(std::string_view shader_cache_directory, bool debug_shaders)
{
  Vulkan::ShaderCache::Create(shader_cache_directory, debug_shaders);
}

bool VulkanHostDisplay::HasContext() const
{
  return static_cast<bool>(g_vulkan_context);
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

  static constexpr char display_fragment_shader[] = R"(
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

  VkShaderModule vertex_shader = g_vulkan_shader_cache->GetVertexShader(fullscreen_quad_vertex_shader);
  if (vertex_shader == VK_NULL_HANDLE)
    return false;

  VkShaderModule fragment_shader = g_vulkan_shader_cache->GetFragmentShader(display_fragment_shader);
  if (fragment_shader == VK_NULL_HANDLE)
    return false;

  Vulkan::GraphicsPipelineBuilder gpbuilder;
  gpbuilder.SetVertexShader(vertex_shader);
  gpbuilder.SetFragmentShader(fragment_shader);
  gpbuilder.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  gpbuilder.SetNoCullRasterizationState();
  gpbuilder.SetNoDepthTestState();
  gpbuilder.SetNoBlendingState();
  gpbuilder.SetDynamicViewportAndScissorState();
  gpbuilder.SetPipelineLayout(m_pipeline_layout);
  gpbuilder.SetRenderPass(m_swap_chain->GetClearRenderPass(), 0);

  m_display_pipeline = gpbuilder.Create(device, pipeline_cache, false);
  if (m_display_pipeline == VK_NULL_HANDLE)
    return false;

  gpbuilder.SetBlendAttachment(0, true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                               VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD);
  m_software_cursor_pipeline = gpbuilder.Create(device, pipeline_cache, false);
  if (m_software_cursor_pipeline == VK_NULL_HANDLE)
    return false;

  // don't need these anymore
  vkDestroyShaderModule(device, vertex_shader, nullptr);
  vkDestroyShaderModule(device, fragment_shader, nullptr);

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
  m_readback_staging_texture.Destroy(false);
  m_upload_staging_texture.Destroy(false);

  Vulkan::Util::SafeDestroyPipeline(m_display_pipeline);
  Vulkan::Util::SafeDestroyPipeline(m_software_cursor_pipeline);
  Vulkan::Util::SafeDestroyPipelineLayout(m_pipeline_layout);
  Vulkan::Util::SafeDestroyDescriptorSetLayout(m_descriptor_set_layout);
  Vulkan::Util::SafeDestroySampler(m_point_sampler);
  Vulkan::Util::SafeDestroySampler(m_linear_sampler);
}

void VulkanHostDisplay::DestroyImGuiContext()
{
  ImGui_ImplVulkan_Shutdown();
}

void VulkanHostDisplay::DestroyContext()
{
  if (!g_vulkan_context)
    return;

  g_vulkan_context->WaitForGPUIdle();
  Vulkan::Context::Destroy();
}

void VulkanHostDisplay::DestroyShaderCache()
{
  Vulkan::ShaderCache::Destroy();
}

bool VulkanHostDisplay::CreateImGuiContext()
{
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_swap_chain->GetWidth());
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_swap_chain->GetHeight());

  ImGui_ImplVulkan_InitInfo vii = {};
  vii.Instance = g_vulkan_context->GetVulkanInstance();
  vii.PhysicalDevice = g_vulkan_context->GetPhysicalDevice();
  vii.Device = g_vulkan_context->GetDevice();
  vii.QueueFamily = g_vulkan_context->GetGraphicsQueueFamilyIndex();
  vii.Queue = g_vulkan_context->GetGraphicsQueue();
  vii.PipelineCache = g_vulkan_shader_cache->GetPipelineCache();
  vii.DescriptorPool = g_vulkan_context->GetGlobalDescriptorPool();
  vii.MinImageCount = m_swap_chain->GetImageCount();
  vii.ImageCount = m_swap_chain->GetImageCount();
  vii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

  if (!ImGui_ImplVulkan_Init(&vii, m_swap_chain->GetClearRenderPass()) ||
      !ImGui_ImplVulkan_CreateFontsTexture(g_vulkan_context->GetCurrentCommandBuffer()))
  {
    return false;
  }

  ImGui_ImplVulkan_NewFrame();
  return true;
}

bool VulkanHostDisplay::BeginRender()
{
  VkResult res = m_swap_chain->AcquireNextImage();
  if (res != VK_SUCCESS)
  {
    if (res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_OUT_OF_DATE_KHR)
    {
      ResizeSwapChain(0, 0);
      res = m_swap_chain->AcquireNextImage();
    }

    if (res != VK_SUCCESS)
    {
      Panic("Failed to acquire swap chain image");
      return false;
    }
  }

  VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();
  Vulkan::Texture& swap_chain_texture = m_swap_chain->GetCurrentTexture();

  // Swap chain images start in undefined
  swap_chain_texture.OverrideImageLayout(VK_IMAGE_LAYOUT_UNDEFINED);
  swap_chain_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  const VkClearValue clear_value = {};

  const VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                    nullptr,
                                    m_swap_chain->GetClearRenderPass(),
                                    m_swap_chain->GetCurrentFramebuffer(),
                                    {{0, 0}, {m_swap_chain->GetWidth(), m_swap_chain->GetHeight()}},
                                    1u,
                                    &clear_value};
  vkCmdBeginRenderPass(cmdbuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
  return true;
}

void VulkanHostDisplay::EndRenderAndPresent()
{
  VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();
  Vulkan::Texture& swap_chain_texture = m_swap_chain->GetCurrentTexture();

  vkCmdEndRenderPass(cmdbuffer);

  swap_chain_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  g_vulkan_context->SubmitCommandBuffer(m_swap_chain->GetImageAvailableSemaphore(),
                                        m_swap_chain->GetRenderingFinishedSemaphore(), m_swap_chain->GetSwapChain(),
                                        m_swap_chain->GetCurrentImageIndex());
  g_vulkan_context->MoveToNextCommandBuffer();

  ImGui::NewFrame();
  ImGui_ImplVulkan_NewFrame();
}

void VulkanHostDisplay::RenderDisplay(s32 left, s32 top, s32 width, s32 height, void* texture_handle, u32 texture_width,
                                      u32 texture_height, u32 texture_view_x, u32 texture_view_y,
                                      u32 texture_view_width, u32 texture_view_height, bool linear_filter)
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
  ImGui::Render();
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), g_vulkan_context->GetCurrentCommandBuffer());
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
    dsupdate.AddImageDescriptorWrite(ds, 0, static_cast<VulkanHostDisplayTexture*>(texture)->GetTexture().GetView());
    dsupdate.AddSamplerDescriptorWrite(ds, 0, m_linear_sampler);
    dsupdate.Update(g_vulkan_context->GetDevice());
  }

  const PushConstants pc{0.0f, 0.0f, 1.0f, 1.0f};
  vkCmdBindPipeline(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_display_pipeline);
  vkCmdPushConstants(cmdbuffer, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
  vkCmdBindDescriptorSets(cmdbuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline_layout, 0, 1, &ds, 0, nullptr);
  Vulkan::Util::SetViewportAndScissor(cmdbuffer, left, top, width, height);
  vkCmdDraw(cmdbuffer, 3, 1, 0, 0);
}

std::vector<std::string> VulkanHostDisplay::EnumerateAdapterNames()
{
  if (Vulkan::LoadVulkanLibrary())
  {
    Common::ScopeGuard lib_guard([]() { Vulkan::UnloadVulkanLibrary(); });

    VkInstance instance = Vulkan::Context::CreateVulkanInstance(false, false, false);
    if (instance != VK_NULL_HANDLE)
    {
      Common::ScopeGuard instance_guard([&instance]() { vkDestroyInstance(instance, nullptr); });

      if (Vulkan::LoadVulkanInstanceFunctions(instance))
      {
        Vulkan::Context::GPUNameList gpus = Vulkan::Context::EnumerateGPUNames(instance);
        if (!gpus.empty())
          return gpus;
      }
    }
  }

  return {"(Default)"};
}

} // namespace FrontendCommon