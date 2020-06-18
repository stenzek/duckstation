#include "vulkanhostdisplay.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui.h"
#include "qtdisplaywidget.h"
Log_SetChannel(VulkanHostDisplay);

VulkanHostDisplay::VulkanHostDisplay(QtHostInterface* host_interface) : QtHostDisplay(host_interface) {}

VulkanHostDisplay::~VulkanHostDisplay() = default;

HostDisplay::RenderAPI VulkanHostDisplay::GetRenderAPI() const
{
  return m_vulkan_display.GetRenderAPI();
}

void* VulkanHostDisplay::GetRenderDevice() const
{
  return m_vulkan_display.GetRenderDevice();
}

void* VulkanHostDisplay::GetRenderContext() const
{
  return m_vulkan_display.GetRenderContext();
}

std::unique_ptr<HostDisplayTexture> VulkanHostDisplay::CreateTexture(u32 width, u32 height, const void* initial_data,
                                                                     u32 initial_data_stride, bool dynamic)
{
  return m_vulkan_display.CreateTexture(width, height, initial_data, initial_data_stride, dynamic);
}

void VulkanHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                      const void* texture_data, u32 texture_data_stride)
{
  m_vulkan_display.UpdateTexture(texture, x, y, width, height, texture_data, texture_data_stride);
}

bool VulkanHostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                        u32 out_data_stride)
{
  return m_vulkan_display.DownloadTexture(texture_handle, x, y, width, height, out_data, out_data_stride);
}

void VulkanHostDisplay::SetVSync(bool enabled)
{
  m_vulkan_display.SetVSync(enabled);
}

bool VulkanHostDisplay::hasDeviceContext() const
{
  return m_vulkan_display.HasContext();
}

bool VulkanHostDisplay::createDeviceContext(bool debug_device)
{
  std::optional<WindowInfo> wi = getWindowInfo();
  if (!wi || !m_vulkan_display.CreateContextAndSwapChain(wi.value(), debug_device))
    return false;

  m_window_width = static_cast<s32>(m_vulkan_display.GetSwapChainWidth());
  m_window_height = static_cast<s32>(m_vulkan_display.GetSwapChainHeight());
  return true;
}

bool VulkanHostDisplay::initializeDeviceContext(std::string_view shader_cache_directory, bool debug_device)
{
  m_vulkan_display.CreateShaderCache(shader_cache_directory, debug_device);

  return QtHostDisplay::initializeDeviceContext(shader_cache_directory, debug_device);
}

bool VulkanHostDisplay::activateDeviceContext()
{
  return true;
}

void VulkanHostDisplay::deactivateDeviceContext() {}

void VulkanHostDisplay::destroyDeviceContext()
{
  QtHostDisplay::destroyDeviceContext();
  m_vulkan_display.DestroyShaderCache();
  m_vulkan_display.DestroySwapChain();
  m_vulkan_display.DestroyContext();
}

bool VulkanHostDisplay::recreateSurface()
{
  std::optional<WindowInfo> wi = getWindowInfo();
  if (!wi.has_value())
    return false;

  if (!m_vulkan_display.RecreateSwapChain(wi.value()))
    return false;

  m_window_width = static_cast<s32>(m_vulkan_display.GetSwapChainWidth());
  m_window_height = static_cast<s32>(m_vulkan_display.GetSwapChainHeight());
  return true;
}

void VulkanHostDisplay::destroySurface()
{
  m_vulkan_display.DestroySwapChain();
}

void VulkanHostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  QtHostDisplay::WindowResized(new_window_width, new_window_height);

  m_vulkan_display.ResizeSwapChain(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  m_window_width = static_cast<s32>(m_vulkan_display.GetSwapChainWidth());
  m_window_height = static_cast<s32>(m_vulkan_display.GetSwapChainHeight());
}

bool VulkanHostDisplay::createDeviceResources()
{
  if (!QtHostDisplay::createDeviceResources())
    return false;

  return m_vulkan_display.CreateResources();
}

void VulkanHostDisplay::destroyDeviceResources()
{
  QtHostDisplay::destroyDeviceResources();
  m_vulkan_display.DestroyResources();
}

bool VulkanHostDisplay::createImGuiContext()
{
  if (!QtHostDisplay::createImGuiContext() || !m_vulkan_display.CreateImGuiContext())
    return false;

  ImGui::NewFrame();
  return true;
}

void VulkanHostDisplay::destroyImGuiContext()
{
  m_vulkan_display.DestroyImGuiContext();
  QtHostDisplay::destroyImGuiContext();
}

void VulkanHostDisplay::Render()
{
  if (!m_vulkan_display.HasSwapChain() || !m_vulkan_display.BeginRender())
    return;

  if (HasDisplayTexture())
  {
    const auto [left, top, width, height] = CalculateDrawRect(m_window_width, m_window_height, m_display_top_margin);
    m_vulkan_display.RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width,
                                   m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                                   m_display_texture_view_width, m_display_texture_view_height,
                                   m_display_linear_filtering);
  }

  m_vulkan_display.RenderImGui();

  if (HasSoftwareCursor())
  {
    const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
    m_vulkan_display.RenderSoftwareCursor(left, top, width, height, m_cursor_texture.get());
  }

  m_vulkan_display.EndRenderAndPresent();
}
