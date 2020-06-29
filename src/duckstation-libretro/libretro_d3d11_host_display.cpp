#include "libretro_d3d11_host_display.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/d3d11/shader_compiler.h"
#include "common/log.h"
#include "libretro_host_interface.h"
Log_SetChannel(D3D11HostDisplay);

#define HAVE_D3D11
#include "libretro_d3d.h"

LibretroD3D11HostDisplay::LibretroD3D11HostDisplay() = default;

LibretroD3D11HostDisplay::~LibretroD3D11HostDisplay() = default;

void LibretroD3D11HostDisplay::SetVSync(bool enabled)
{
  // The libretro frontend controls this.
  Log_DevPrintf("Ignoring SetVSync(%u)", BoolToUInt32(enabled));
}

bool LibretroD3D11HostDisplay::RequestHardwareRendererContext(retro_hw_render_callback* cb)
{
  cb->cache_context = true;
  cb->bottom_left_origin = false;
  cb->context_type = RETRO_HW_CONTEXT_DIRECT3D;
  cb->version_major = 11;
  cb->version_minor = 0;

  return g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb);
}

bool LibretroD3D11HostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name,
                                                  bool debug_device)
{
  retro_hw_render_interface* ri = nullptr;
  if (!g_retro_environment_callback(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &ri))
  {
    Log_ErrorPrint("Failed to get HW render interface");
    return false;
  }
  else if (ri->interface_type != RETRO_HW_RENDER_INTERFACE_D3D11 ||
           ri->interface_version != RETRO_HW_RENDER_INTERFACE_D3D11_VERSION)
  {
    Log_ErrorPrint("Unexpected HW interface - type %u version %u", static_cast<unsigned>(ri->interface_type),
                   static_cast<unsigned>(ri->interface_version));
    return false;
  }

  const retro_hw_render_interface_d3d11* d3d11_ri = reinterpret_cast<const retro_hw_render_interface_d3d11*>(ri);
  if (!d3d11_ri->device || !d3d11_ri->context)
  {
    Log_ErrorPrintf("Missing D3D device or context");
    return false;
  }

  m_device = d3d11_ri->device;
  m_context = d3d11_ri->context;
  return CreateResources();
}

void LibretroD3D11HostDisplay::DestroyRenderDevice()
{
  DestroyResources();
  m_framebuffer.Destroy();
  m_context.Reset();
  m_device.Reset();
}

void LibretroD3D11HostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  m_window_info.surface_width = static_cast<u32>(new_window_width);
  m_window_info.surface_height = static_cast<u32>(new_window_height);
}

bool LibretroD3D11HostDisplay::Render()
{
  // TODO: Skip framebuffer when offset is (0,0).
  if (!CheckFramebufferSize(m_display_texture_width, m_display_texture_height))
    return false;

  // Ensure we're not currently bound.
  ID3D11ShaderResourceView* null_srv = nullptr;
  m_context->PSSetShaderResources(0, 1, &null_srv);
  m_context->OMSetRenderTargets(1u, m_framebuffer.GetD3DRTVArray(), nullptr);

  if (HasDisplayTexture())
  {
    RenderDisplay(0, 0, m_display_texture_width, m_display_texture_height, m_display_texture_handle,
                  m_display_texture_width, m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                  m_display_texture_view_width, m_display_texture_view_height, m_display_linear_filtering);
  }

  if (HasSoftwareCursor())
  {
    const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
    RenderSoftwareCursor(left, top, width, height, m_cursor_texture.get());
  }

  // NOTE: libretro frontend expects the data bound to PS SRV slot 0.
  m_context->OMSetRenderTargets(0, nullptr, nullptr);
  m_context->PSSetShaderResources(0, 1, m_framebuffer.GetD3DSRVArray());
  g_retro_video_refresh_callback(RETRO_HW_FRAME_BUFFER_VALID, m_display_texture_width, m_display_texture_height, 0);
  return true;
}

bool LibretroD3D11HostDisplay::CheckFramebufferSize(u32 width, u32 height)
{
  if (m_framebuffer.GetWidth() >= width && m_framebuffer.GetHeight() >= height)
    return true;

  const u32 rounded_width = Common::AlignUpPow2(width, 1024);
  const u32 rounded_height = Common::AlignUpPow2(height, 512);
  return m_framebuffer.Create(m_device.Get(), rounded_width, rounded_height, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
}
