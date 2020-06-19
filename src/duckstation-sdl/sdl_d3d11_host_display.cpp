#include "sdl_d3d11_host_display.h"
#include "imgui_impl_sdl.h"
#include "sdl_util.h"
#include <SDL_syswm.h>
#include <array>
#include <dxgi1_5.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

SDLD3D11HostDisplay::SDLD3D11HostDisplay(SDL_Window* window) : m_window(window)
{
  SDL_GetWindowSize(window, &m_window_width, &m_window_height);
}

SDLD3D11HostDisplay::~SDLD3D11HostDisplay()
{
  if (m_window)
    SDL_DestroyWindow(m_window);
}

std::unique_ptr<HostDisplay> SDLD3D11HostDisplay::Create(SDL_Window* window, bool debug_device)
{
  std::unique_ptr<SDLD3D11HostDisplay> display = std::make_unique<SDLD3D11HostDisplay>(window);
  if (!display->Initialize(debug_device))
    return {};

  return display;
}

HostDisplay::RenderAPI SDLD3D11HostDisplay::GetRenderAPI() const
{
  return m_interface.GetRenderAPI();
}

void* SDLD3D11HostDisplay::GetRenderDevice() const
{
  return m_interface.GetRenderDevice();
}

void* SDLD3D11HostDisplay::GetRenderContext() const
{
  return m_interface.GetRenderContext();
}

void SDLD3D11HostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  m_interface.ResizeSwapChain(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  HostDisplay::WindowResized(static_cast<s32>(m_interface.GetSwapChainWidth()),
                             static_cast<s32>(m_interface.GetSwapChainHeight()));
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_interface.GetSwapChainWidth());
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_interface.GetSwapChainHeight());
}

std::unique_ptr<HostDisplayTexture> SDLD3D11HostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                       u32 data_stride, bool dynamic)
{
  return m_interface.CreateTexture(width, height, data, data_stride, dynamic);
}

void SDLD3D11HostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                        const void* data, u32 data_stride)
{
  m_interface.UpdateTexture(texture, x, y, width, height, data, data_stride);
}

bool SDLD3D11HostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height,
                                          void* out_data, u32 out_data_stride)
{
  return m_interface.DownloadTexture(texture_handle, x, y, width, height, out_data, out_data_stride);
}

void SDLD3D11HostDisplay::SetVSync(bool enabled)
{
  m_interface.SetVSync(enabled);
}

bool SDLD3D11HostDisplay::Initialize(bool debug_device)
{
  std::optional<WindowInfo> wi = SDLUtil::GetWindowInfoForSDLWindow(m_window);
  if (!wi.has_value())
    return false;

  if (!m_interface.CreateContextAndSwapChain(wi.value(), true, debug_device))
    return false;

  if (!m_interface.CreateResources())
    return false;

  if (!m_interface.CreateImGuiContext() || !ImGui_ImplSDL2_InitForVulkan(m_window))
    return false;

  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui::NewFrame();
  return true;
}

void SDLD3D11HostDisplay::Render()
{
  if (!m_interface.BeginRender())
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

  ImGui_ImplSDL2_NewFrame(m_window);
}
