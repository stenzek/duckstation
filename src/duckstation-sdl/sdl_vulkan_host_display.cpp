#include "sdl_vulkan_host_display.h"
#include "common/assert.h"
#include "common/log.h"
#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_vulkan.h"
#include "sdl_util.h"
#include <SDL_syswm.h>
#include <array>
Log_SetChannel(VulkanHostDisplay);

SDLVulkanHostDisplay::SDLVulkanHostDisplay(SDL_Window* window) : m_window(window)
{
  SDL_GetWindowSize(window, &m_window_width, &m_window_height);
}

SDLVulkanHostDisplay::~SDLVulkanHostDisplay()
{
  ImGui_ImplSDL2_Shutdown();
  m_display.DestroyImGuiContext();
  m_display.DestroyResources();
  m_display.DestroyShaderCache();
  m_display.DestroySwapChain();
  m_display.DestroyContext();

  if (m_window)
    SDL_DestroyWindow(m_window);
}

std::unique_ptr<HostDisplay> SDLVulkanHostDisplay::Create(SDL_Window* window, std::string_view adapter_name,
                                                          std::string_view shader_cache_directory, bool debug_device)
{
  std::unique_ptr<SDLVulkanHostDisplay> display = std::make_unique<SDLVulkanHostDisplay>(window);
  if (!display->Initialize(adapter_name, shader_cache_directory, debug_device))
    return nullptr;

  return display;
}

HostDisplay::RenderAPI SDLVulkanHostDisplay::GetRenderAPI() const
{
  return m_display.GetRenderAPI();
}

void* SDLVulkanHostDisplay::GetRenderDevice() const
{
  return m_display.GetRenderDevice();
}

void* SDLVulkanHostDisplay::GetRenderContext() const
{
  return m_display.GetRenderContext();
}

void SDLVulkanHostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  m_display.ResizeSwapChain(static_cast<u32>(new_window_width), static_cast<u32>(new_window_height));
  HostDisplay::WindowResized(static_cast<s32>(m_display.GetSwapChainWidth()),
                             static_cast<s32>(m_display.GetSwapChainHeight()));
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_display.GetSwapChainWidth());
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_display.GetSwapChainHeight());
}

std::unique_ptr<HostDisplayTexture> SDLVulkanHostDisplay::CreateTexture(u32 width, u32 height, const void* data,
                                                                        u32 data_stride, bool dynamic /*= false*/)
{
  return m_display.CreateTexture(width, height, data, data_stride, dynamic);
}

void SDLVulkanHostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                         const void* data, u32 data_stride)
{
  m_display.UpdateTexture(texture, x, y, width, height, data, data_stride);
}

bool SDLVulkanHostDisplay::DownloadTexture(const void* texture_handle, u32 x, u32 y, u32 width, u32 height,
                                           void* out_data, u32 out_data_stride)
{
  return m_display.DownloadTexture(texture_handle, x, y, width, height, out_data, out_data_stride);
}

void SDLVulkanHostDisplay::SetVSync(bool enabled)
{
  m_display.SetVSync(enabled);
}

bool SDLVulkanHostDisplay::Initialize(std::string_view adapter_name, std::string_view shader_cache_directory,
                                      bool debug_device)
{
  std::optional<WindowInfo> wi = SDLUtil::GetWindowInfoForSDLWindow(m_window);
  if (!wi.has_value())
  {
    Log_ErrorPrintf("Failed to get window info for SDL window");
    return false;
  }

  if (!m_display.CreateContextAndSwapChain(wi.value(), adapter_name, debug_device))
    return false;

  m_display.CreateShaderCache(shader_cache_directory, debug_device);

  if (!m_display.CreateResources())
    return false;

  if (!m_display.CreateImGuiContext() || !ImGui_ImplSDL2_InitForVulkan(m_window))
    return false;

  ImGui_ImplSDL2_NewFrame(m_window);
  ImGui::NewFrame();
  return true;
}

void SDLVulkanHostDisplay::Render()
{
  if (!m_display.BeginRender())
    return;

  if (HasDisplayTexture())
  {
    const auto [left, top, width, height] = CalculateDrawRect(m_window_width, m_window_height, m_display_top_margin);
    m_display.RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width,
                            m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                            m_display_texture_view_width, m_display_texture_view_height, m_display_linear_filtering);
  }

  m_display.RenderImGui();

  if (HasSoftwareCursor())
  {
    const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
    m_display.RenderSoftwareCursor(left, top, width, height, m_cursor_texture.get());
  }

  m_display.EndRenderAndPresent();

  ImGui_ImplSDL2_NewFrame(m_window);
}
