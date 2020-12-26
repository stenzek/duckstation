#include "libretro_vulkan_host_display.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/vulkan/builders.h"
#include "common/vulkan/context.h"
#include "common/vulkan/shader_cache.h"
#include "common/vulkan/util.h"
#include "libretro_host_interface.h"
#include "vulkan_loader.h"
Log_SetChannel(LibretroVulkanHostDisplay);

LibretroVulkanHostDisplay::LibretroVulkanHostDisplay() = default;

LibretroVulkanHostDisplay::~LibretroVulkanHostDisplay() = default;

void LibretroVulkanHostDisplay::SetVSync(bool enabled)
{
  // The libretro frontend controls this.
  Log_DevPrintf("Ignoring SetVSync(%u)", BoolToUInt32(enabled));
}

static bool RetroCreateVulkanDevice(struct retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
                                    VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                                    const char** required_device_extensions, unsigned num_required_device_extensions,
                                    const char** required_device_layers, unsigned num_required_device_layers,
                                    const VkPhysicalDeviceFeatures* required_features)
{
  // We need some module functions.
  vkGetInstanceProcAddr = get_instance_proc_addr;
  if (!Vulkan::LoadVulkanInstanceFunctions(instance))
  {
    Log_ErrorPrintf("Failed to load Vulkan instance functions");
    Vulkan::ResetVulkanLibraryFunctionPointers();
    return false;
  }

  if (gpu == VK_NULL_HANDLE)
  {
    Vulkan::Context::GPUList gpus = Vulkan::Context::EnumerateGPUs(instance);
    if (gpus.empty())
    {
      g_libretro_host_interface.ReportError("No GPU provided and none available, cannot create device");
      Vulkan::ResetVulkanLibraryFunctionPointers();
      return false;
    }

    Log_InfoPrintf("No GPU provided, using first/default");
    gpu = gpus[0];
  }

  if (!Vulkan::Context::CreateFromExistingInstance(
        instance, gpu, surface, false, false, false, required_device_extensions, num_required_device_extensions,
        required_device_layers, num_required_device_layers, required_features))
  {
    Vulkan::ResetVulkanLibraryFunctionPointers();
    return false;
  }

  context->gpu = g_vulkan_context->GetPhysicalDevice();
  context->device = g_vulkan_context->GetDevice();
  context->queue = g_vulkan_context->GetGraphicsQueue();
  context->queue_family_index = g_vulkan_context->GetGraphicsQueueFamilyIndex();
  context->presentation_queue = g_vulkan_context->GetPresentQueue();
  context->presentation_queue_family_index = g_vulkan_context->GetPresentQueueFamilyIndex();
  return true;
}

static retro_hw_render_context_negotiation_interface_vulkan s_vulkan_context_negotiation_interface = {
  RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,         // interface_type
  RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION, // interface_version
  nullptr,                                                      // get_application_info
  RetroCreateVulkanDevice,                                      // create_device
  nullptr                                                       // destroy_device
};

bool LibretroVulkanHostDisplay::RequestHardwareRendererContext(retro_hw_render_callback* cb)
{
  cb->cache_context = false;
  cb->bottom_left_origin = false;
  cb->context_type = RETRO_HW_CONTEXT_VULKAN;
  return g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb) &&
         g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
                                      &s_vulkan_context_negotiation_interface);
}

bool LibretroVulkanHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name,
                                                   bool debug_device, bool threaded_presentation)
{
  retro_hw_render_interface* ri = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &ri))
  {
    Log_ErrorPrint("Failed to get HW render interface");
    return false;
  }
  else if (ri->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
           ri->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
  {
    Log_ErrorPrintf("Unexpected HW interface - type %u version %u", static_cast<unsigned>(ri->interface_type),
                    static_cast<unsigned>(ri->interface_version));
    return false;
  }

  if (!g_vulkan_context)
  {
    Log_ErrorPrintf("Vulkan context was not negotiated/created");
    return false;
  }

  // TODO: Grab queue? it should be the same
  m_ri = reinterpret_cast<retro_hw_render_interface_vulkan*>(ri);
  return true;
}

void LibretroVulkanHostDisplay::DestroyRenderDevice()
{
  VulkanHostDisplay::DestroyRenderDevice();
  Vulkan::ResetVulkanLibraryFunctionPointers();
}

bool LibretroVulkanHostDisplay::CreateResources()
{
  m_frame_render_pass = g_vulkan_context->GetRenderPass(FRAMEBUFFER_FORMAT, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT,
                                                        VK_ATTACHMENT_LOAD_OP_CLEAR);
  if (m_frame_render_pass == VK_NULL_HANDLE)
    return false;

  return VulkanHostDisplay::CreateResources();
}

void LibretroVulkanHostDisplay::DestroyResources()
{
  VulkanHostDisplay::DestroyResources();
  Vulkan::Util::SafeDestroyFramebuffer(m_frame_framebuffer);
  m_frame_texture.Destroy();
  Vulkan::ShaderCompiler::DeinitializeGlslang();
}

VkRenderPass LibretroVulkanHostDisplay::GetRenderPassForDisplay() const
{
  return m_frame_render_pass;
}

void LibretroVulkanHostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  m_window_info.surface_width = static_cast<u32>(new_window_width);
  m_window_info.surface_height = static_cast<u32>(new_window_height);
}

bool LibretroVulkanHostDisplay::ChangeRenderWindow(const WindowInfo& new_wi)
{
  // re-query hardware render interface - in vulkan, things get recreated without us being notified
  retro_hw_render_interface* ri = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &ri))
  {
    Log_ErrorPrint("Failed to get HW render interface");
    return false;
  }
  else if (ri->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
           ri->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
  {
    Log_ErrorPrintf("Unexpected HW interface - type %u version %u", static_cast<unsigned>(ri->interface_type),
                    static_cast<unsigned>(ri->interface_version));
    return false;
  }

  retro_hw_render_interface_vulkan* vri = reinterpret_cast<retro_hw_render_interface_vulkan*>(ri);
  if (vri != m_ri)
  {
    Log_WarningPrintf("HW render interface pointer changed without us being notified, this might cause issues?");
    m_ri = vri;
  }

  return true;
}

bool LibretroVulkanHostDisplay::Render()
{
  const u32 resolution_scale = g_libretro_host_interface.GetResolutionScale();
  const u32 display_width = static_cast<u32>(m_display_width) * resolution_scale;
  const u32 display_height = static_cast<u32>(m_display_height) * resolution_scale;
  if (display_width == 0 || display_height == 0 || !CheckFramebufferSize(display_width, display_height))
    return false;

  VkCommandBuffer cmdbuffer = g_vulkan_context->GetCurrentCommandBuffer();
  m_frame_texture.OverrideImageLayout(m_frame_view.image_layout);
  m_frame_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  const VkClearValue clear_value = {};
  const VkRenderPassBeginInfo rp = {
    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,  nullptr, m_frame_render_pass, m_frame_framebuffer,
    {{0, 0}, {display_width, display_height}}, 1u,      &clear_value};
  vkCmdBeginRenderPass(cmdbuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

  if (HasDisplayTexture())
  {
    const auto [left, top, width, height] = CalculateDrawRect(display_width, display_height, 0, false);
    RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width, m_display_texture_height,
                  m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                  m_display_texture_view_height, m_display_linear_filtering);
  }

  if (HasSoftwareCursor())
  {
    // TODO: Scale mouse x/y
    const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect(m_mouse_position_x, m_mouse_position_y);
    RenderSoftwareCursor(left, top, width, height, m_cursor_texture.get());
  }

  vkCmdEndRenderPass(cmdbuffer);
  m_frame_texture.TransitionToLayout(cmdbuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_frame_view.image_layout = m_frame_texture.GetLayout();
  m_ri->set_image(m_ri->handle, &m_frame_view, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);

  // TODO: We can't use this because it doesn't support passing fences...
  // m_ri.set_command_buffers(m_ri.handle, 1, &cmdbuffer);
  m_ri->lock_queue(m_ri->handle);
  g_vulkan_context->SubmitCommandBuffer();
  m_ri->unlock_queue(m_ri->handle);
  g_vulkan_context->MoveToNextCommandBuffer();

  g_retro_video_refresh_callback(RETRO_HW_FRAME_BUFFER_VALID, display_width, display_height, 0);
  return true;
}

bool LibretroVulkanHostDisplay::CheckFramebufferSize(u32 width, u32 height)
{
  static constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  static constexpr VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
  static constexpr VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;

  if (m_frame_texture.GetWidth() == width && m_frame_texture.GetHeight() == height)
    return true;

  g_vulkan_context->DeferFramebufferDestruction(m_frame_framebuffer);
  m_frame_texture.Destroy(true);

  if (!m_frame_texture.Create(width, height, 1, 1, FRAMEBUFFER_FORMAT, VK_SAMPLE_COUNT_1_BIT, view_type, tiling, usage))
    return false;

  VkCommandBuffer cmdbuf = g_vulkan_context->GetCurrentCommandBuffer();
  m_frame_texture.TransitionToLayout(cmdbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  static constexpr VkClearColorValue cc = {};
  static constexpr VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmdbuf, m_frame_texture.GetImage(), m_frame_texture.GetLayout(), &cc, 1, &range);

  Vulkan::FramebufferBuilder fbb;
  fbb.SetRenderPass(m_frame_render_pass);
  fbb.AddAttachment(m_frame_texture.GetView());
  fbb.SetSize(width, height, 1);
  m_frame_framebuffer = fbb.Create(g_vulkan_context->GetDevice(), false);
  if (m_frame_framebuffer == VK_NULL_HANDLE)
    return false;

  m_frame_view = {};
  m_frame_view.image_view = m_frame_texture.GetView();
  m_frame_view.image_layout = m_frame_texture.GetLayout();
  m_frame_view.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  m_frame_view.create_info.image = m_frame_texture.GetImage();
  m_frame_view.create_info.viewType = view_type;
  m_frame_view.create_info.format = FRAMEBUFFER_FORMAT;
  m_frame_view.create_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                         VK_COMPONENT_SWIZZLE_A};
  m_frame_view.create_info.subresourceRange = range;
  return true;
}
