#include "d3d11hostdisplay.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui.h"
#include "qtdisplaywidget.h"
Log_SetChannel(D3D11HostDisplay);

D3D11HostDisplay::D3D11HostDisplay(QtHostInterface* host_interface) : QtHostDisplay(host_interface) {}

D3D11HostDisplay::~D3D11HostDisplay() = default;

HostDisplay::RenderAPI D3D11HostDisplay::GetRenderAPI() const
{
  return m_interface.GetRenderAPI();
}

void* D3D11HostDisplay::GetRenderDevice() const
{
  return m_interface.GetRenderDevice();
}

void* D3D11HostDisplay::GetRenderContext() const
{
  return m_interface.GetRenderContext();
}

std::unique_ptr<HostDisplayTexture> D3D11HostDisplay::CreateTexture(u32 width, u32 height, const void* initial_data,
                                                                    u32 initial_data_stride, bool dynamic)
{
  return m_interface.CreateTexture(width, height, initial_data, initial_data_stride, dynamic);
}

void D3D11HostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                     const void* texture_data, u32 texture_data_stride)
{
  m_interface.UpdateTexture(texture, x, y, width, height, texture_data, texture_data_stride);
}

bool D3D11HostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                       u32 out_data_stride)
{
  return m_interface.DownloadTexture(texture_handle, x, y, width, height, out_data, out_data_stride);
}

void D3D11HostDisplay::SetVSync(bool enabled)
{
  m_interface.SetVSync(enabled);
}

bool D3D11HostDisplay::shouldUseFlipModelSwapChain() const
{
  // For some reason DXGI gets stuck waiting for some kernel object when the Qt window has a parent (render-to-main) on
  // some computers, unless the window is completely occluded. The legacy swap chain mode does not have this problem.
  return m_widget->parent() == nullptr;
}

bool D3D11HostDisplay::hasDeviceContext() const
{
  return m_interface.HasContext();
}

bool D3D11HostDisplay::createDeviceContext(const QString& adapter_name, bool debug_device)
{
  std::optional<WindowInfo> wi = getWindowInfo();
  if (!wi || !m_interface.CreateContextAndSwapChain(wi.value(), adapter_name.toStdString(),
                                                    shouldUseFlipModelSwapChain(), debug_device))
  {
    return false;
  }

  m_window_width = static_cast<s32>(m_interface.GetSwapChainWidth());
  m_window_height = static_cast<s32>(m_interface.GetSwapChainHeight());
  return true;
}

bool D3D11HostDisplay::initializeDeviceContext(std::string_view shader_cache_directory, bool debug_device)
{
  return QtHostDisplay::initializeDeviceContext(shader_cache_directory, debug_device);
}

bool D3D11HostDisplay::activateDeviceContext()
{
  return true;
}

void D3D11HostDisplay::deactivateDeviceContext() {}

void D3D11HostDisplay::destroyDeviceContext()
{
  QtHostDisplay::destroyDeviceContext();
  m_interface.DestroySwapChain();
  m_interface.DestroyContext();
}

bool D3D11HostDisplay::recreateSurface()
{
  std::optional<WindowInfo> wi = getWindowInfo();
  if (!wi.has_value())
    return false;

  if (!m_interface.RecreateSwapChain(wi.value(), shouldUseFlipModelSwapChain()))
    return false;

  m_window_width = static_cast<s32>(m_interface.GetSwapChainWidth());
  m_window_height = static_cast<s32>(m_interface.GetSwapChainHeight());
  return true;
}

void D3D11HostDisplay::destroySurface()
{
  m_interface.DestroySwapChain();
}

void D3D11HostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  QtHostDisplay::WindowResized(new_window_width, new_window_height);

  m_interface.ResizeSwapChain(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  m_window_width = static_cast<s32>(m_interface.GetSwapChainWidth());
  m_window_height = static_cast<s32>(m_interface.GetSwapChainHeight());
}

bool D3D11HostDisplay::createDeviceResources()
{
  if (!QtHostDisplay::createDeviceResources())
    return false;

  return m_interface.CreateResources();
}

void D3D11HostDisplay::destroyDeviceResources()
{
  QtHostDisplay::destroyDeviceResources();
  m_interface.DestroyResources();
}

bool D3D11HostDisplay::createImGuiContext()
{
  if (!QtHostDisplay::createImGuiContext() || !m_interface.CreateImGuiContext())
    return false;

  ImGui::NewFrame();
  return true;
}

void D3D11HostDisplay::destroyImGuiContext()
{
  m_interface.DestroyImGuiContext();
  QtHostDisplay::destroyImGuiContext();
}

void D3D11HostDisplay::Render()
{
  if (!m_interface.HasSwapChain() || !m_interface.BeginRender())
    return;

  if (HasDisplayTexture())
  {
    const auto [left, top, width, height] = CalculateDrawRect(m_window_width, m_window_height, m_display_top_margin);
    m_interface.RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width,
                              m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                              m_display_texture_view_width, m_display_texture_view_height, m_display_linear_filtering);
  }

  m_interface.RenderImGui();

  if (HasSoftwareCursor())
  {
    const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
    m_interface.RenderSoftwareCursor(left, top, width, height, m_cursor_texture.get());
  }

  m_interface.EndRenderAndPresent();
}
