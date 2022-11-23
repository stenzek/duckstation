#include "d3d11_host_display.h"
#include "common/assert.h"
#include "common/d3d11/shader_cache.h"
#include "common/d3d11/shader_compiler.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common_host.h"
#include "core/host_settings.h"
#include "core/settings.h"
#include "core/shader_cache_version.h"
#include "display_ps.hlsl.h"
#include "display_ps_alpha.hlsl.h"
#include "display_vs.hlsl.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "postprocessing_shadergen.h"
#include <array>
#include <dxgi1_5.h>
Log_SetChannel(D3D11HostDisplay);

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static constexpr std::array<float, 4> s_clear_color = {};

D3D11HostDisplay::D3D11HostDisplay() = default;

D3D11HostDisplay::~D3D11HostDisplay()
{
  DestroyStagingBuffer();
  DestroyResources();
  DestroyRenderSurface();
  m_context.Reset();
  m_device.Reset();
}

RenderAPI D3D11HostDisplay::GetRenderAPI() const
{
  return RenderAPI::D3D11;
}

void* D3D11HostDisplay::GetRenderDevice() const
{
  return m_device.Get();
}

void* D3D11HostDisplay::GetRenderContext() const
{
  return m_context.Get();
}

bool D3D11HostDisplay::HasRenderDevice() const
{
  return static_cast<bool>(m_device);
}

bool D3D11HostDisplay::HasRenderSurface() const
{
  return static_cast<bool>(m_swap_chain);
}

std::unique_ptr<GPUTexture> D3D11HostDisplay::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                            GPUTexture::Format format, const void* data,
                                                            u32 data_stride, bool dynamic /* = false */)
{
  std::unique_ptr<D3D11::Texture> tex(std::make_unique<D3D11::Texture>());
  if (!tex->Create(m_device.Get(), width, height, layers, levels, samples, format, D3D11_BIND_SHADER_RESOURCE, data,
                   data_stride, dynamic))
  {
    tex.reset();
  }

  return tex;
}

bool D3D11HostDisplay::BeginTextureUpdate(GPUTexture* texture, u32 width, u32 height, void** out_buffer, u32* out_pitch)
{
  D3D11::Texture* tex = static_cast<D3D11::Texture*>(texture);
  if (!tex->IsDynamic() || tex->GetWidth() != width || tex->GetHeight() != height)
    return false;

  D3D11_MAPPED_SUBRESOURCE sr;
  HRESULT hr = m_context->Map(tex->GetD3DTexture(), 0, D3D11_MAP_WRITE_DISCARD, 0, &sr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map pixels texture failed: %08X", hr);
    return false;
  }

  *out_buffer = sr.pData;
  *out_pitch = sr.RowPitch;
  return true;
}

void D3D11HostDisplay::EndTextureUpdate(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height)
{
  D3D11::Texture* tex = static_cast<D3D11::Texture*>(texture);
  m_context->Unmap(tex->GetD3DTexture(), 0);
}

bool D3D11HostDisplay::UpdateTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, const void* data,
                                     u32 pitch)
{
  D3D11::Texture* tex = static_cast<D3D11::Texture*>(texture);
  if (tex->IsDynamic())
    return HostDisplay::UpdateTexture(texture, x, y, width, height, data, pitch);

  const CD3D11_BOX dst_box(x, y, 0, x + width, y + height, 1);
  m_context->UpdateSubresource(tex->GetD3DTexture(), 0, &dst_box, data, pitch, pitch * height);
  return true;
}

bool D3D11HostDisplay::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                       u32 out_data_stride)
{
  const D3D11::Texture* tex = static_cast<const D3D11::Texture*>(texture);
  if (!CheckStagingBufferSize(width, height, tex->GetDXGIFormat()))
    return false;

  const CD3D11_BOX box(static_cast<LONG>(x), static_cast<LONG>(y), 0, static_cast<LONG>(x + width),
                       static_cast<LONG>(y + height), 1);
  m_context->CopySubresourceRegion(m_readback_staging_texture.Get(), 0, 0, 0, 0, tex->GetD3DTexture(), 0, &box);

  D3D11_MAPPED_SUBRESOURCE sr;
  HRESULT hr = m_context->Map(m_readback_staging_texture.Get(), 0, D3D11_MAP_READ, 0, &sr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map() failed with HRESULT %08X", hr);
    return false;
  }

  const u32 copy_size = tex->GetPixelSize() * width;
  StringUtil::StrideMemCpy(out_data, out_data_stride, sr.pData, sr.RowPitch, copy_size, height);
  m_context->Unmap(m_readback_staging_texture.Get(), 0);
  return true;
}

bool D3D11HostDisplay::CheckStagingBufferSize(u32 width, u32 height, DXGI_FORMAT format)
{
  if (m_readback_staging_texture_width >= width && m_readback_staging_texture_width >= height &&
      m_readback_staging_texture_format == format)
    return true;

  DestroyStagingBuffer();

  CD3D11_TEXTURE2D_DESC desc(format, width, height, 1, 1, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
  HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_readback_staging_texture.ReleaseAndGetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateTexture2D() failed with HRESULT %08X", hr);
    return false;
  }

  return true;
}

void D3D11HostDisplay::DestroyStagingBuffer()
{
  m_readback_staging_texture.Reset();
  m_readback_staging_texture_width = 0;
  m_readback_staging_texture_height = 0;
  m_readback_staging_texture_format = DXGI_FORMAT_UNKNOWN;
}

bool D3D11HostDisplay::SupportsTextureFormat(GPUTexture::Format format) const
{
  const DXGI_FORMAT dfmt = D3D11::Texture::GetDXGIFormat(format);
  if (dfmt == DXGI_FORMAT_UNKNOWN)
    return false;

  UINT support = 0;
  const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
  return (SUCCEEDED(m_device->CheckFormatSupport(dfmt, &support)) && ((support & required) == required));
}

bool D3D11HostDisplay::GetHostRefreshRate(float* refresh_rate)
{
  if (m_swap_chain && IsFullscreen())
  {
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(m_swap_chain->GetDesc(&desc)) && desc.BufferDesc.RefreshRate.Numerator > 0 &&
        desc.BufferDesc.RefreshRate.Denominator > 0)
    {
      Log_InfoPrintf("using fs rr: %u %u", desc.BufferDesc.RefreshRate.Numerator,
                     desc.BufferDesc.RefreshRate.Denominator);
      *refresh_rate = static_cast<float>(desc.BufferDesc.RefreshRate.Numerator) /
                      static_cast<float>(desc.BufferDesc.RefreshRate.Denominator);
      return true;
    }
  }

  return HostDisplay::GetHostRefreshRate(refresh_rate);
}

void D3D11HostDisplay::SetVSync(bool enabled)
{
  m_vsync = enabled;
}

bool D3D11HostDisplay::CreateRenderDevice(const WindowInfo& wi)
{
  UINT create_flags = 0;
  if (g_settings.gpu_use_debug_device)
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;

  ComPtr<IDXGIFactory> temp_dxgi_factory;
  HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(temp_dxgi_factory.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create DXGI factory: 0x%08X", hr);
    return false;
  }

  u32 adapter_index;
  if (!g_settings.gpu_adapter.empty())
  {
    AdapterAndModeList adapter_info(GetAdapterAndModeList(temp_dxgi_factory.Get()));
    for (adapter_index = 0; adapter_index < static_cast<u32>(adapter_info.adapter_names.size()); adapter_index++)
    {
      if (g_settings.gpu_adapter == adapter_info.adapter_names[adapter_index])
        break;
    }
    if (adapter_index == static_cast<u32>(adapter_info.adapter_names.size()))
    {
      Log_WarningPrintf("Could not find adapter '%s', using first (%s)", g_settings.gpu_adapter.c_str(),
                        adapter_info.adapter_names[0].c_str());
      adapter_index = 0;
    }
  }
  else
  {
    Log_InfoPrintf("No adapter selected, using first.");
    adapter_index = 0;
  }

  ComPtr<IDXGIAdapter> dxgi_adapter;
  hr = temp_dxgi_factory->EnumAdapters(adapter_index, dxgi_adapter.GetAddressOf());
  if (FAILED(hr))
    Log_WarningPrintf("Failed to enumerate adapter %u, using default", adapter_index);

  static constexpr std::array<D3D_FEATURE_LEVEL, 3> requested_feature_levels = {
    {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0}};

  hr =
    D3D11CreateDevice(dxgi_adapter.Get(), dxgi_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      create_flags, requested_feature_levels.data(), static_cast<UINT>(requested_feature_levels.size()),
                      D3D11_SDK_VERSION, m_device.GetAddressOf(), nullptr, m_context.GetAddressOf());

  // we re-grab these later, see below
  dxgi_adapter.Reset();
  temp_dxgi_factory.Reset();

  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create D3D device: 0x%08X", hr);
    return false;
  }

  if (g_settings.gpu_use_debug_device && IsDebuggerPresent())
  {
    ComPtr<ID3D11InfoQueue> info;
    hr = m_device.As(&info);
    if (SUCCEEDED(hr))
    {
      info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
      info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, TRUE);
    }
  }

  // we need the specific factory for the device, otherwise MakeWindowAssociation() is flaky.
  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(m_device.As(&dxgi_device)) || FAILED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.GetAddressOf()))) ||
      FAILED(dxgi_adapter->GetParent(IID_PPV_ARGS(m_dxgi_factory.GetAddressOf()))))
  {
    Log_WarningPrint("Failed to get parent adapter/device/factory");
    return false;
  }
  ComPtr<IDXGIDevice1> dxgi_device1;
  if (SUCCEEDED(dxgi_device.As(&dxgi_device1)))
    dxgi_device1->SetMaximumFrameLatency(1);

  DXGI_ADAPTER_DESC adapter_desc;
  if (SUCCEEDED(dxgi_adapter->GetDesc(&adapter_desc)))
  {
    char adapter_name_buffer[128];
    const int name_length =
      WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, static_cast<int>(std::wcslen(adapter_desc.Description)),
                          adapter_name_buffer, countof(adapter_name_buffer), 0, nullptr);
    if (name_length >= 0)
    {
      adapter_name_buffer[name_length] = 0;
      Log_InfoPrintf("D3D Adapter: %s", adapter_name_buffer);
    }
  }

  m_allow_tearing_supported = false;
  ComPtr<IDXGIFactory5> dxgi_factory5;
  hr = m_dxgi_factory.As(&dxgi_factory5);
  if (SUCCEEDED(hr))
  {
    BOOL allow_tearing_supported = false;
    hr = dxgi_factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing_supported,
                                            sizeof(allow_tearing_supported));
    if (SUCCEEDED(hr))
      m_allow_tearing_supported = (allow_tearing_supported == TRUE);
  }

  m_window_info = wi;

  if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSwapChain(nullptr))
  {
    m_window_info = {};
    return false;
  }

  return true;
}

bool D3D11HostDisplay::InitializeRenderDevice()
{
  if (!CreateResources())
    return false;

  return true;
}

bool D3D11HostDisplay::MakeRenderContextCurrent()
{
  return true;
}

bool D3D11HostDisplay::DoneRenderContextCurrent()
{
  return true;
}

bool D3D11HostDisplay::CreateSwapChain(const DXGI_MODE_DESC* fullscreen_mode)
{
  HRESULT hr;

  if (m_window_info.type != WindowInfo::Type::Win32)
    return false;

  m_using_flip_model_swap_chain = fullscreen_mode || !Host::GetBoolSettingValue("Display", "UseBlitSwapChain", false);

  const HWND window_hwnd = reinterpret_cast<HWND>(m_window_info.window_handle);
  RECT client_rc{};
  GetClientRect(window_hwnd, &client_rc);
  const u32 width = static_cast<u32>(client_rc.right - client_rc.left);
  const u32 height = static_cast<u32>(client_rc.bottom - client_rc.top);

  DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
  swap_chain_desc.BufferDesc.Width = width;
  swap_chain_desc.BufferDesc.Height = height;
  swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferCount = 2;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.OutputWindow = window_hwnd;
  swap_chain_desc.Windowed = TRUE;
  swap_chain_desc.SwapEffect = m_using_flip_model_swap_chain ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;

  m_using_allow_tearing = (m_allow_tearing_supported && m_using_flip_model_swap_chain && !fullscreen_mode);
  if (m_using_allow_tearing)
    swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  if (fullscreen_mode)
  {
    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swap_chain_desc.Windowed = FALSE;
    swap_chain_desc.BufferDesc = *fullscreen_mode;
  }

  Log_InfoPrintf("Creating a %dx%d %s %s swap chain", swap_chain_desc.BufferDesc.Width,
                 swap_chain_desc.BufferDesc.Height, m_using_flip_model_swap_chain ? "flip-discard" : "discard",
                 swap_chain_desc.Windowed ? "windowed" : "full-screen");

  hr = m_dxgi_factory->CreateSwapChain(m_device.Get(), &swap_chain_desc, m_swap_chain.GetAddressOf());
  if (FAILED(hr) && m_using_flip_model_swap_chain)
  {
    Log_WarningPrintf("Failed to create a flip-discard swap chain, trying discard.");
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swap_chain_desc.Flags = 0;
    m_using_flip_model_swap_chain = false;
    m_using_allow_tearing = false;

    hr = m_dxgi_factory->CreateSwapChain(m_device.Get(), &swap_chain_desc, m_swap_chain.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("CreateSwapChain failed: 0x%08X", hr);
      return false;
    }
  }

  ComPtr<IDXGIFactory> dxgi_factory;
  hr = m_swap_chain->GetParent(IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
  if (SUCCEEDED(hr))
  {
    hr = dxgi_factory->MakeWindowAssociation(swap_chain_desc.OutputWindow, DXGI_MWA_NO_WINDOW_CHANGES);
    if (FAILED(hr))
      Log_WarningPrintf("MakeWindowAssociation() to disable ALT+ENTER failed");
  }

  return CreateSwapChainRTV();
}

bool D3D11HostDisplay::CreateSwapChainRTV()
{
  ComPtr<ID3D11Texture2D> backbuffer;
  HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("GetBuffer for RTV failed: 0x%08X", hr);
    return false;
  }

  D3D11_TEXTURE2D_DESC backbuffer_desc;
  backbuffer->GetDesc(&backbuffer_desc);

  CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(D3D11_RTV_DIMENSION_TEXTURE2D, backbuffer_desc.Format, 0, 0,
                                          backbuffer_desc.ArraySize);
  hr = m_device->CreateRenderTargetView(backbuffer.Get(), &rtv_desc, m_swap_chain_rtv.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateRenderTargetView for swap chain failed: 0x%08X", hr);
    return false;
  }

  m_window_info.surface_width = backbuffer_desc.Width;
  m_window_info.surface_height = backbuffer_desc.Height;
  Log_InfoPrintf("Swap chain buffer size: %ux%u", m_window_info.surface_width, m_window_info.surface_height);

  if (m_window_info.type == WindowInfo::Type::Win32)
  {
    BOOL fullscreen = FALSE;
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(m_swap_chain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen &&
        SUCCEEDED(m_swap_chain->GetDesc(&desc)))
    {
      m_window_info.surface_refresh_rate = static_cast<float>(desc.BufferDesc.RefreshRate.Numerator) /
                                           static_cast<float>(desc.BufferDesc.RefreshRate.Denominator);
    }
    else
    {
      m_window_info.surface_refresh_rate = 0.0f;
    }
  }

  return true;
}

bool D3D11HostDisplay::ChangeRenderWindow(const WindowInfo& new_wi)
{
  DestroyRenderSurface();

  m_window_info = new_wi;
  return CreateSwapChain(nullptr);
}

void D3D11HostDisplay::DestroyRenderSurface()
{
  m_window_info.SetSurfaceless();
  if (IsFullscreen())
    SetFullscreen(false, 0, 0, 0.0f);

  m_swap_chain_rtv.Reset();
  m_swap_chain.Reset();
}

void D3D11HostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  if (!m_swap_chain)
    return;

  m_swap_chain_rtv.Reset();

  HRESULT hr = m_swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                           m_using_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr))
    Log_ErrorPrintf("ResizeBuffers() failed: 0x%08X", hr);

  if (!CreateSwapChainRTV())
    Panic("Failed to recreate swap chain RTV after resize");
}

bool D3D11HostDisplay::SupportsFullscreen() const
{
  return true;
}

bool D3D11HostDisplay::IsFullscreen()
{
  BOOL is_fullscreen = FALSE;
  return (m_swap_chain && SUCCEEDED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) && is_fullscreen);
}

bool D3D11HostDisplay::SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate)
{
  if (!m_swap_chain)
    return false;

  BOOL is_fullscreen = FALSE;
  HRESULT hr = m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr);
  if (!fullscreen)
  {
    // leaving fullscreen
    if (is_fullscreen)
      return SUCCEEDED(m_swap_chain->SetFullscreenState(FALSE, nullptr));
    else
      return true;
  }

  IDXGIOutput* output;
  if (FAILED(hr = m_swap_chain->GetContainingOutput(&output)))
    return false;

  DXGI_SWAP_CHAIN_DESC current_desc;
  hr = m_swap_chain->GetDesc(&current_desc);
  if (FAILED(hr))
    return false;

  DXGI_MODE_DESC new_mode = current_desc.BufferDesc;
  new_mode.Width = width;
  new_mode.Height = height;
  new_mode.RefreshRate.Numerator = static_cast<UINT>(std::floor(refresh_rate * 1000.0f));
  new_mode.RefreshRate.Denominator = 1000u;

  DXGI_MODE_DESC closest_mode;
  if (FAILED(hr = output->FindClosestMatchingMode(&new_mode, &closest_mode, nullptr)) ||
      new_mode.Format != current_desc.BufferDesc.Format)
  {
    Log_ErrorPrintf("Failed to find closest matching mode, hr=%08X", hr);
    return false;
  }

  if (new_mode.Width == current_desc.BufferDesc.Width && new_mode.Height == current_desc.BufferDesc.Height &&
      new_mode.RefreshRate.Numerator == current_desc.BufferDesc.RefreshRate.Numerator &&
      new_mode.RefreshRate.Denominator == current_desc.BufferDesc.RefreshRate.Denominator)
  {
    Log_InfoPrintf("Fullscreen mode already set");
    return true;
  }

  m_swap_chain_rtv.Reset();
  m_swap_chain.Reset();

  if (!CreateSwapChain(&closest_mode))
  {
    Log_ErrorPrintf("Failed to create a fullscreen swap chain");
    if (!CreateSwapChain(nullptr))
      Panic("Failed to recreate windowed swap chain");

    return false;
  }

  return true;
}

bool D3D11HostDisplay::CreateResources()
{
  HRESULT hr;

  m_display_vertex_shader =
    D3D11::ShaderCompiler::CreateVertexShader(m_device.Get(), s_display_vs_bytecode, sizeof(s_display_vs_bytecode));
  m_display_pixel_shader =
    D3D11::ShaderCompiler::CreatePixelShader(m_device.Get(), s_display_ps_bytecode, sizeof(s_display_ps_bytecode));
  m_display_alpha_pixel_shader = D3D11::ShaderCompiler::CreatePixelShader(m_device.Get(), s_display_ps_alpha_bytecode,
                                                                          sizeof(s_display_ps_alpha_bytecode));
  if (!m_display_vertex_shader || !m_display_pixel_shader || !m_display_alpha_pixel_shader)
    return false;

  if (!m_display_uniform_buffer.Create(m_device.Get(), D3D11_BIND_CONSTANT_BUFFER, DISPLAY_UNIFORM_BUFFER_SIZE))
    return false;

  CD3D11_RASTERIZER_DESC rasterizer_desc = CD3D11_RASTERIZER_DESC(CD3D11_DEFAULT());
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  hr = m_device->CreateRasterizerState(&rasterizer_desc, m_display_rasterizer_state.GetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_DEPTH_STENCIL_DESC depth_stencil_desc = CD3D11_DEPTH_STENCIL_DESC(CD3D11_DEFAULT());
  depth_stencil_desc.DepthEnable = FALSE;
  depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  hr = m_device->CreateDepthStencilState(&depth_stencil_desc, m_display_depth_stencil_state.GetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_BLEND_DESC blend_desc = CD3D11_BLEND_DESC(CD3D11_DEFAULT());
  hr = m_device->CreateBlendState(&blend_desc, m_display_blend_state.GetAddressOf());
  if (FAILED(hr))
    return false;

  blend_desc.RenderTarget[0] = {TRUE,
                                D3D11_BLEND_SRC_ALPHA,
                                D3D11_BLEND_INV_SRC_ALPHA,
                                D3D11_BLEND_OP_ADD,
                                D3D11_BLEND_ONE,
                                D3D11_BLEND_ZERO,
                                D3D11_BLEND_OP_ADD,
                                D3D11_COLOR_WRITE_ENABLE_ALL};
  hr = m_device->CreateBlendState(&blend_desc, m_software_cursor_blend_state.GetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_SAMPLER_DESC sampler_desc = CD3D11_SAMPLER_DESC(CD3D11_DEFAULT());
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  hr = m_device->CreateSamplerState(&sampler_desc, m_point_sampler.GetAddressOf());
  if (FAILED(hr))
    return false;

  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  hr = m_device->CreateSamplerState(&sampler_desc, m_linear_sampler.GetAddressOf());
  if (FAILED(hr))
    return false;

  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
  sampler_desc.BorderColor[0] = 0.0f;
  sampler_desc.BorderColor[1] = 0.0f;
  sampler_desc.BorderColor[2] = 0.0f;
  sampler_desc.BorderColor[3] = 1.0f;
  hr = m_device->CreateSamplerState(&sampler_desc, m_border_sampler.GetAddressOf());
  if (FAILED(hr))
    return false;

  return true;
}

void D3D11HostDisplay::DestroyResources()
{
  m_post_processing_chain.ClearStages();
  m_post_processing_input_texture.Destroy();
  m_post_processing_stages.clear();

  m_display_uniform_buffer.Release();
  m_border_sampler.Reset();
  m_linear_sampler.Reset();
  m_point_sampler.Reset();
  m_display_alpha_pixel_shader.Reset();
  m_display_pixel_shader.Reset();
  m_display_vertex_shader.Reset();
  m_display_blend_state.Reset();
  m_display_depth_stencil_state.Reset();
  m_display_rasterizer_state.Reset();
}

bool D3D11HostDisplay::CreateImGuiContext()
{
  return ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
}

void D3D11HostDisplay::DestroyImGuiContext()
{
  ImGui_ImplDX11_Shutdown();
}

bool D3D11HostDisplay::UpdateImGuiFontTexture()
{
  ImGui_ImplDX11_CreateFontsTexture();
  return true;
}

bool D3D11HostDisplay::Render(bool skip_present)
{
  if (skip_present || !m_swap_chain)
  {
    if (ImGui::GetCurrentContext())
      ImGui::Render();

    return false;
  }

  // When using vsync, the time here seems to include the time for the buffer to become available.
  // This blows our our GPU usage number considerably, so read the timestamp before the final blit
  // in this configuration. It does reduce accuracy a little, but better than seeing 100% all of
  // the time, when it's more like a couple of percent.
  if (m_vsync && m_gpu_timing_enabled)
    PopTimestampQuery();

  RenderDisplay();

  if (ImGui::GetCurrentContext())
    RenderImGui();

  RenderSoftwareCursor();

  if (!m_vsync && m_gpu_timing_enabled)
    PopTimestampQuery();

  if (!m_vsync && m_using_allow_tearing)
    m_swap_chain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
  else
    m_swap_chain->Present(BoolToUInt32(m_vsync), 0);

  if (m_gpu_timing_enabled)
    KickTimestampQuery();

  return true;
}

bool D3D11HostDisplay::RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                        GPUTexture::Format* out_format)
{
  static constexpr GPUTexture::Format hdformat = GPUTexture::Format::RGBA8;

  D3D11::Texture render_texture;
  if (!render_texture.Create(m_device.Get(), width, height, 1, 1, 1, hdformat, D3D11_BIND_RENDER_TARGET))
    return false;

  static constexpr std::array<float, 4> clear_color = {};
  m_context->ClearRenderTargetView(render_texture.GetD3DRTV(), clear_color.data());
  m_context->OMSetRenderTargets(1, render_texture.GetD3DRTVArray(), nullptr);

  if (HasDisplayTexture())
  {
    const auto [left, top, draw_width, draw_height] = CalculateDrawRect(width, height);

    if (!m_post_processing_chain.IsEmpty())
    {
      ApplyPostProcessingChain(render_texture.GetD3DRTV(), left, top, draw_width, draw_height,
                               static_cast<D3D11::Texture*>(m_display_texture), m_display_texture_view_x,
                               m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                               width, height);
    }
    else
    {
      RenderDisplay(left, top, draw_width, draw_height, static_cast<D3D11::Texture*>(m_display_texture),
                    m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                    m_display_texture_view_height, IsUsingLinearFiltering());
    }
  }

  m_context->OMSetRenderTargets(0, nullptr, nullptr);

  const u32 stride = GPUTexture::GetPixelSize(hdformat) * width;
  out_pixels->resize(width * height);
  if (!DownloadTexture(&render_texture, 0, 0, width, height, out_pixels->data(), stride))
    return false;

  *out_stride = stride;
  *out_format = hdformat;
  return true;
}

void D3D11HostDisplay::RenderImGui()
{
  ImGui::Render();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void D3D11HostDisplay::RenderDisplay()
{
  const auto [left, top, width, height] = CalculateDrawRect(GetWindowWidth(), GetWindowHeight());

  if (HasDisplayTexture() && !m_post_processing_chain.IsEmpty())
  {
    ApplyPostProcessingChain(m_swap_chain_rtv.Get(), left, top, width, height,
                             static_cast<D3D11::Texture*>(m_display_texture), m_display_texture_view_x,
                             m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                             GetWindowWidth(), GetWindowHeight());
    return;
  }

  m_context->ClearRenderTargetView(m_swap_chain_rtv.Get(), s_clear_color.data());
  m_context->OMSetRenderTargets(1, m_swap_chain_rtv.GetAddressOf(), nullptr);

  if (!HasDisplayTexture())
    return;

  RenderDisplay(left, top, width, height, static_cast<D3D11::Texture*>(m_display_texture), m_display_texture_view_x,
                m_display_texture_view_y, m_display_texture_view_width, m_display_texture_view_height,
                IsUsingLinearFiltering());
}

void D3D11HostDisplay::RenderDisplay(s32 left, s32 top, s32 width, s32 height, D3D11::Texture* texture,
                                     s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                     s32 texture_view_height, bool linear_filter)
{
  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_context->VSSetShader(m_display_vertex_shader.Get(), nullptr, 0);
  m_context->PSSetShader(m_display_pixel_shader.Get(), nullptr, 0);
  m_context->PSSetShaderResources(0, 1, texture->GetD3DSRVArray());
  m_context->PSSetSamplers(0, 1, linear_filter ? m_linear_sampler.GetAddressOf() : m_point_sampler.GetAddressOf());

  const bool linear = IsUsingLinearFiltering();
  const float position_adjust = linear ? 0.5f : 0.0f;
  const float size_adjust = linear ? 1.0f : 0.0f;
  const float uniforms[4] = {
    (static_cast<float>(texture_view_x) + position_adjust) / static_cast<float>(texture->GetWidth()),
    (static_cast<float>(texture_view_y) + position_adjust) / static_cast<float>(texture->GetHeight()),
    (static_cast<float>(texture_view_width) - size_adjust) / static_cast<float>(texture->GetWidth()),
    (static_cast<float>(texture_view_height) - size_adjust) / static_cast<float>(texture->GetHeight())};
  const auto map = m_display_uniform_buffer.Map(m_context.Get(), m_display_uniform_buffer.GetSize(), sizeof(uniforms));
  std::memcpy(map.pointer, uniforms, sizeof(uniforms));
  m_display_uniform_buffer.Unmap(m_context.Get(), sizeof(uniforms));
  m_context->VSSetConstantBuffers(0, 1, m_display_uniform_buffer.GetD3DBufferArray());

  const CD3D11_VIEWPORT vp(static_cast<float>(left), static_cast<float>(top), static_cast<float>(width),
                           static_cast<float>(height));
  m_context->RSSetViewports(1, &vp);
  m_context->RSSetState(m_display_rasterizer_state.Get());
  m_context->OMSetDepthStencilState(m_display_depth_stencil_state.Get(), 0);
  m_context->OMSetBlendState(m_display_blend_state.Get(), nullptr, 0xFFFFFFFFu);

  m_context->Draw(3, 0);
}

void D3D11HostDisplay::RenderSoftwareCursor()
{
  if (!HasSoftwareCursor())
    return;

  const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
  RenderSoftwareCursor(left, top, width, height, m_cursor_texture.get());
}

void D3D11HostDisplay::RenderSoftwareCursor(s32 left, s32 top, s32 width, s32 height, GPUTexture* texture_handle)
{
  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_context->VSSetShader(m_display_vertex_shader.Get(), nullptr, 0);
  m_context->PSSetShader(m_display_alpha_pixel_shader.Get(), nullptr, 0);
  m_context->PSSetShaderResources(0, 1, static_cast<D3D11::Texture*>(texture_handle)->GetD3DSRVArray());
  m_context->PSSetSamplers(0, 1, m_linear_sampler.GetAddressOf());

  const float uniforms[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  const auto map = m_display_uniform_buffer.Map(m_context.Get(), m_display_uniform_buffer.GetSize(), sizeof(uniforms));
  std::memcpy(map.pointer, uniforms, sizeof(uniforms));
  m_display_uniform_buffer.Unmap(m_context.Get(), sizeof(uniforms));
  m_context->VSSetConstantBuffers(0, 1, m_display_uniform_buffer.GetD3DBufferArray());

  const CD3D11_VIEWPORT vp(static_cast<float>(left), static_cast<float>(top), static_cast<float>(width),
                           static_cast<float>(height));
  m_context->RSSetViewports(1, &vp);
  m_context->RSSetState(m_display_rasterizer_state.Get());
  m_context->OMSetDepthStencilState(m_display_depth_stencil_state.Get(), 0);
  m_context->OMSetBlendState(m_software_cursor_blend_state.Get(), nullptr, 0xFFFFFFFFu);

  m_context->Draw(3, 0);
}

HostDisplay::AdapterAndModeList D3D11HostDisplay::StaticGetAdapterAndModeList()
{
  ComPtr<IDXGIFactory> dxgi_factory;
  HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
  if (FAILED(hr))
    return {};

  return GetAdapterAndModeList(dxgi_factory.Get());
}

HostDisplay::AdapterAndModeList D3D11HostDisplay::GetAdapterAndModeList(IDXGIFactory* dxgi_factory)
{
  AdapterAndModeList adapter_info;
  ComPtr<IDXGIAdapter> current_adapter;
  while (SUCCEEDED(dxgi_factory->EnumAdapters(static_cast<UINT>(adapter_info.adapter_names.size()),
                                              current_adapter.ReleaseAndGetAddressOf())))
  {
    DXGI_ADAPTER_DESC adapter_desc;
    std::string adapter_name;
    if (SUCCEEDED(current_adapter->GetDesc(&adapter_desc)))
    {
      char adapter_name_buffer[128];
      const int name_length = WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description,
                                                  static_cast<int>(std::wcslen(adapter_desc.Description)),
                                                  adapter_name_buffer, countof(adapter_name_buffer), 0, nullptr);
      if (name_length >= 0)
        adapter_name.assign(adapter_name_buffer, static_cast<size_t>(name_length));
      else
        adapter_name.assign("(Unknown)");
    }
    else
    {
      adapter_name.assign("(Unknown)");
    }

    if (adapter_info.fullscreen_modes.empty())
    {
      ComPtr<IDXGIOutput> output;
      if (SUCCEEDED(current_adapter->EnumOutputs(0, &output)))
      {
        UINT num_modes = 0;
        if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num_modes, nullptr)))
        {
          std::vector<DXGI_MODE_DESC> modes(num_modes);
          if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num_modes, modes.data())))
          {
            for (const DXGI_MODE_DESC& mode : modes)
            {
              adapter_info.fullscreen_modes.push_back(GetFullscreenModeString(
                mode.Width, mode.Height,
                static_cast<float>(mode.RefreshRate.Numerator) / static_cast<float>(mode.RefreshRate.Denominator)));
            }
          }
        }
      }
    }

    // handle duplicate adapter names
    if (std::any_of(adapter_info.adapter_names.begin(), adapter_info.adapter_names.end(),
                    [&adapter_name](const std::string& other) { return (adapter_name == other); }))
    {
      std::string original_adapter_name = std::move(adapter_name);

      u32 current_extra = 2;
      do
      {
        adapter_name = StringUtil::StdStringFromFormat("%s (%u)", original_adapter_name.c_str(), current_extra);
        current_extra++;
      } while (std::any_of(adapter_info.adapter_names.begin(), adapter_info.adapter_names.end(),
                           [&adapter_name](const std::string& other) { return (adapter_name == other); }));
    }

    adapter_info.adapter_names.push_back(std::move(adapter_name));
  }

  return adapter_info;
}

HostDisplay::AdapterAndModeList D3D11HostDisplay::GetAdapterAndModeList()
{
  return GetAdapterAndModeList(m_dxgi_factory.Get());
}

bool D3D11HostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  if (config.empty())
  {
    m_post_processing_input_texture.Destroy();
    m_post_processing_stages.clear();
    m_post_processing_chain.ClearStages();
    return true;
  }

  if (!m_post_processing_chain.CreateFromString(config))
    return false;

  m_post_processing_stages.clear();

  D3D11::ShaderCache shader_cache;
  shader_cache.Open(EmuFolders::Cache, m_device->GetFeatureLevel(), SHADER_CACHE_VERSION,
                    g_settings.gpu_use_debug_device);

  FrontendCommon::PostProcessingShaderGen shadergen(RenderAPI::D3D11, true);
  u32 max_ubo_size = 0;

  for (u32 i = 0; i < m_post_processing_chain.GetStageCount(); i++)
  {
    const FrontendCommon::PostProcessingShader& shader = m_post_processing_chain.GetShaderStage(i);
    const std::string vs = shadergen.GeneratePostProcessingVertexShader(shader);
    const std::string ps = shadergen.GeneratePostProcessingFragmentShader(shader);

    PostProcessingStage stage;
    stage.uniforms_size = shader.GetUniformsSize();
    stage.vertex_shader = shader_cache.GetVertexShader(m_device.Get(), vs);
    stage.pixel_shader = shader_cache.GetPixelShader(m_device.Get(), ps);
    if (!stage.vertex_shader || !stage.pixel_shader)
    {
      Log_ErrorPrintf("Failed to compile one or more post-processing shaders, disabling.");
      m_post_processing_stages.clear();
      m_post_processing_chain.ClearStages();
      return false;
    }

    max_ubo_size = std::max(max_ubo_size, stage.uniforms_size);
    m_post_processing_stages.push_back(std::move(stage));
  }

  if (m_display_uniform_buffer.GetSize() < max_ubo_size &&
      !m_display_uniform_buffer.Create(m_device.Get(), D3D11_BIND_CONSTANT_BUFFER, max_ubo_size))
  {
    Log_ErrorPrintf("Failed to allocate %u byte constant buffer for postprocessing", max_ubo_size);
    m_post_processing_stages.clear();
    m_post_processing_chain.ClearStages();
    return false;
  }

  m_post_processing_timer.Reset();
  return true;
}

bool D3D11HostDisplay::CheckPostProcessingRenderTargets(u32 target_width, u32 target_height)
{
  DebugAssert(!m_post_processing_stages.empty());

  const GPUTexture::Format format = GPUTexture::Format::RGBA8;
  const u32 bind_flags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  if (m_post_processing_input_texture.GetWidth() != target_width ||
      m_post_processing_input_texture.GetHeight() != target_height)
  {
    if (!m_post_processing_input_texture.Create(m_device.Get(), target_width, target_height, 1, 1, 1, format,
                                                bind_flags))
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
      if (!pps.output_texture.Create(m_device.Get(), target_width, target_height, 1, 1, 1, format, bind_flags))
        return false;
    }
  }

  return true;
}

void D3D11HostDisplay::ApplyPostProcessingChain(ID3D11RenderTargetView* final_target, s32 final_left, s32 final_top,
                                                s32 final_width, s32 final_height, D3D11::Texture* texture,
                                                s32 texture_view_x, s32 texture_view_y, s32 texture_view_width,
                                                s32 texture_view_height, u32 target_width, u32 target_height)
{
  if (!CheckPostProcessingRenderTargets(target_width, target_height))
  {
    RenderDisplay(final_left, final_top, final_width, final_height, texture, texture_view_x, texture_view_y,
                  texture_view_width, texture_view_height, IsUsingLinearFiltering());
    return;
  }

  // downsample/upsample - use same viewport for remainder
  m_context->ClearRenderTargetView(m_post_processing_input_texture.GetD3DRTV(), s_clear_color.data());
  m_context->OMSetRenderTargets(1, m_post_processing_input_texture.GetD3DRTVArray(), nullptr);
  RenderDisplay(final_left, final_top, final_width, final_height, texture, texture_view_x, texture_view_y,
                texture_view_width, texture_view_height, IsUsingLinearFiltering());

  const s32 orig_texture_width = texture_view_width;
  const s32 orig_texture_height = texture_view_height;
  texture = &m_post_processing_input_texture;
  texture_view_x = final_left;
  texture_view_y = final_top;
  texture_view_width = final_width;
  texture_view_height = final_height;

  const u32 final_stage = static_cast<u32>(m_post_processing_stages.size()) - 1u;
  for (u32 i = 0; i < static_cast<u32>(m_post_processing_stages.size()); i++)
  {
    PostProcessingStage& pps = m_post_processing_stages[i];
    ID3D11RenderTargetView* rtv = (i == final_stage) ? final_target : pps.output_texture.GetD3DRTV();
    m_context->ClearRenderTargetView(rtv, s_clear_color.data());
    m_context->OMSetRenderTargets(1, &rtv, nullptr);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(pps.vertex_shader.Get(), nullptr, 0);
    m_context->PSSetShader(pps.pixel_shader.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, texture->GetD3DSRVArray());
    m_context->PSSetSamplers(0, 1, m_border_sampler.GetAddressOf());

    const auto map =
      m_display_uniform_buffer.Map(m_context.Get(), m_display_uniform_buffer.GetSize(), pps.uniforms_size);
    m_post_processing_chain.GetShaderStage(i).FillUniformBuffer(
      map.pointer, texture->GetWidth(), texture->GetHeight(), texture_view_x, texture_view_y, texture_view_width,
      texture_view_height, GetWindowWidth(), GetWindowHeight(), orig_texture_width, orig_texture_height,
      static_cast<float>(m_post_processing_timer.GetTimeSeconds()));
    m_display_uniform_buffer.Unmap(m_context.Get(), pps.uniforms_size);
    m_context->VSSetConstantBuffers(0, 1, m_display_uniform_buffer.GetD3DBufferArray());
    m_context->PSSetConstantBuffers(0, 1, m_display_uniform_buffer.GetD3DBufferArray());

    m_context->Draw(3, 0);

    if (i != final_stage)
      texture = &pps.output_texture;
  }

  ID3D11ShaderResourceView* null_srv = nullptr;
  m_context->PSSetShaderResources(0, 1, &null_srv);
}

bool D3D11HostDisplay::CreateTimestampQueries()
{
  for (u32 i = 0; i < NUM_TIMESTAMP_QUERIES; i++)
  {
    for (u32 j = 0; j < 3; j++)
    {
      const CD3D11_QUERY_DESC qdesc((j == 0) ? D3D11_QUERY_TIMESTAMP_DISJOINT : D3D11_QUERY_TIMESTAMP);
      const HRESULT hr = m_device->CreateQuery(&qdesc, m_timestamp_queries[i][j].ReleaseAndGetAddressOf());
      if (FAILED(hr))
      {
        m_timestamp_queries = {};
        return false;
      }
    }
  }

  KickTimestampQuery();
  return true;
}

void D3D11HostDisplay::DestroyTimestampQueries()
{
  if (!m_timestamp_queries[0][0])
    return;

  if (m_timestamp_query_started)
    m_context->End(m_timestamp_queries[m_write_timestamp_query][1].Get());

  m_timestamp_queries = {};
  m_read_timestamp_query = 0;
  m_write_timestamp_query = 0;
  m_waiting_timestamp_queries = 0;
  m_timestamp_query_started = 0;
}

void D3D11HostDisplay::PopTimestampQuery()
{
  while (m_waiting_timestamp_queries > 0)
  {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    const HRESULT disjoint_hr = m_context->GetData(m_timestamp_queries[m_read_timestamp_query][0].Get(), &disjoint,
                                                   sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (disjoint_hr != S_OK)
      break;

    if (disjoint.Disjoint)
    {
      Log_VerbosePrintf("GPU timing disjoint, resetting.");
      m_read_timestamp_query = 0;
      m_write_timestamp_query = 0;
      m_waiting_timestamp_queries = 0;
      m_timestamp_query_started = 0;
    }
    else
    {
      u64 start = 0, end = 0;
      const HRESULT start_hr = m_context->GetData(m_timestamp_queries[m_read_timestamp_query][1].Get(), &start,
                                                  sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      const HRESULT end_hr = m_context->GetData(m_timestamp_queries[m_read_timestamp_query][2].Get(), &end, sizeof(end),
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (start_hr == S_OK && end_hr == S_OK)
      {
        const float delta =
          static_cast<float>(static_cast<double>(end - start) / (static_cast<double>(disjoint.Frequency) / 1000.0));
        m_accumulated_gpu_time += delta;
        m_read_timestamp_query = (m_read_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
        m_waiting_timestamp_queries--;
      }
    }
  }

  if (m_timestamp_query_started)
  {
    m_context->End(m_timestamp_queries[m_write_timestamp_query][2].Get());
    m_context->End(m_timestamp_queries[m_write_timestamp_query][0].Get());
    m_write_timestamp_query = (m_write_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_timestamp_query_started = false;
    m_waiting_timestamp_queries++;
  }
}

void D3D11HostDisplay::KickTimestampQuery()
{
  if (m_timestamp_query_started || !m_timestamp_queries[0][0] || m_waiting_timestamp_queries == NUM_TIMESTAMP_QUERIES)
    return;

  m_context->Begin(m_timestamp_queries[m_write_timestamp_query][0].Get());
  m_context->End(m_timestamp_queries[m_write_timestamp_query][1].Get());
  m_timestamp_query_started = true;
}

bool D3D11HostDisplay::SetGPUTimingEnabled(bool enabled)
{
  if (m_gpu_timing_enabled == enabled)
    return true;

  m_gpu_timing_enabled = enabled;
  if (m_gpu_timing_enabled)
  {
    if (!CreateTimestampQueries())
      return false;

    KickTimestampQuery();
    return true;
  }
  else
  {
    DestroyTimestampQueries();
    return true;
  }
}

float D3D11HostDisplay::GetAndResetAccumulatedGPUTime()
{
  const float value = m_accumulated_gpu_time;
  m_accumulated_gpu_time = 0.0f;
  return value;
}
