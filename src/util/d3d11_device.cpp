// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "d3d11_device.h"
#include "core/host.h" // TODO: Remove me
#include "d3d11_pipeline.h"
#include "d3d11_texture.h"
#include "d3d_common.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <array>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

Log_SetChannel(D3D11Device);

// We need to synchronize instance creation because of adapter enumeration from the UI thread.
static std::mutex s_instance_mutex;

static constexpr std::array<float, 4> s_clear_color = {};
static constexpr GPUTexture::Format s_swap_chain_format = GPUTexture::Format::RGBA8;

void SetD3DDebugObjectName(ID3D11DeviceChild* obj, std::string_view name)
{
#ifdef _DEBUG
  // WKPDID_D3DDebugObjectName
  static constexpr GUID guid = {0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};

  UINT existing_data_size;
  HRESULT hr = obj->GetPrivateData(guid, &existing_data_size, nullptr);
  if (SUCCEEDED(hr) && existing_data_size > 0)
    return;

  obj->SetPrivateData(guid, static_cast<UINT>(name.length()), name.data());
#endif
}

D3D11Device::D3D11Device() = default;

D3D11Device::~D3D11Device()
{
  // Should all be torn down by now.
  Assert(!m_device);
}

bool D3D11Device::HasSurface() const
{
  return static_cast<bool>(m_swap_chain);
}

bool D3D11Device::CreateDevice(std::string_view adapter, std::optional<bool> exclusive_fullscreen_control,
                               FeatureMask disabled_features, Error* error)
{
  std::unique_lock lock(s_instance_mutex);

  UINT create_flags = 0;
  if (m_debug_device)
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;

  m_dxgi_factory = D3DCommon::CreateFactory(m_debug_device, error);
  if (!m_dxgi_factory)
    return false;

  ComPtr<IDXGIAdapter1> dxgi_adapter = D3DCommon::GetAdapterByName(m_dxgi_factory.Get(), adapter);
  m_max_feature_level = D3DCommon::GetDeviceMaxFeatureLevel(dxgi_adapter.Get());

  static constexpr std::array<D3D_FEATURE_LEVEL, 4> requested_feature_levels = {
    {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0}};

  ComPtr<ID3D11Device> temp_device;
  ComPtr<ID3D11DeviceContext> temp_context;
  HRESULT hr =
    D3D11CreateDevice(dxgi_adapter.Get(), dxgi_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      create_flags, requested_feature_levels.data(), static_cast<UINT>(requested_feature_levels.size()),
                      D3D11_SDK_VERSION, temp_device.GetAddressOf(), nullptr, temp_context.GetAddressOf());

  if (FAILED(hr))
  {
    Error::SetHResult(error, "Failed to create D3D device: ", hr);
    return false;
  }
  else if (FAILED(hr = temp_device.As(&m_device)) || FAILED(hr = temp_context.As(&m_context)))
  {
    Error::SetHResult(error, "Failed to get D3D11.1 device: ", hr);
    return false;
  }

  // we re-grab these later, see below
  dxgi_adapter.Reset();
  temp_context.Reset();
  temp_device.Reset();

  if (m_debug_device && IsDebuggerPresent())
  {
    ComPtr<ID3D11InfoQueue> info;
    hr = m_device.As(&info);
    if (SUCCEEDED(hr))
    {
      info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
      info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, TRUE);
    }
  }

#ifdef _DEBUG
  if (m_debug_device)
    m_context.As(&m_annotation);
#endif

  ComPtr<IDXGIDevice> dxgi_device;
  if (SUCCEEDED(m_device.As(&dxgi_device)) &&
      SUCCEEDED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.GetAddressOf()))))
    INFO_LOG("D3D Adapter: {}", D3DCommon::GetAdapterName(dxgi_adapter.Get()));
  else
    ERROR_LOG("Failed to obtain D3D adapter name.");
  INFO_LOG("Max device feature level: {}",
           D3DCommon::GetFeatureLevelString(D3DCommon::GetRenderAPIVersionForFeatureLevel(m_max_feature_level)));

  BOOL allow_tearing_supported = false;
  hr = m_dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing_supported,
                                           sizeof(allow_tearing_supported));
  m_allow_tearing_supported = (SUCCEEDED(hr) && allow_tearing_supported == TRUE);

  SetFeatures(disabled_features);

  if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSwapChain())
  {
    Error::SetStringView(error, "Failed to create swap chain");
    return false;
  }

  if (!CreateBuffers())
  {
    Error::SetStringView(error, "Failed to create buffers");
    return false;
  }

  return true;
}

void D3D11Device::DestroyDevice()
{
  std::unique_lock lock(s_instance_mutex);

  DestroyBuffers();
  m_context.Reset();
  m_device.Reset();
}

void D3D11Device::SetFeatures(FeatureMask disabled_features)
{
  const D3D_FEATURE_LEVEL feature_level = m_device->GetFeatureLevel();

  m_render_api = RenderAPI::D3D11;
  m_render_api_version = D3DCommon::GetRenderAPIVersionForFeatureLevel(feature_level);
  m_max_texture_size = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  m_max_multisamples = 1;
  for (u32 multisamples = 2; multisamples < D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; multisamples++)
  {
    UINT num_quality_levels;
    if (SUCCEEDED(
          m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, multisamples, &num_quality_levels)) &&
        num_quality_levels > 0)
    {
      m_max_multisamples = multisamples;
    }
  }

  m_features.dual_source_blend = !(disabled_features & FEATURE_MASK_DUAL_SOURCE_BLEND);
  m_features.framebuffer_fetch = false;
  m_features.per_sample_shading = (feature_level >= D3D_FEATURE_LEVEL_10_1);
  m_features.noperspective_interpolation = true;
  m_features.texture_copy_to_self = false;
  m_features.supports_texture_buffers = !(disabled_features & FEATURE_MASK_TEXTURE_BUFFERS);
  m_features.texture_buffers_emulated_with_ssbo = false;
  m_features.feedback_loops = false;
  m_features.geometry_shaders = !(disabled_features & FEATURE_MASK_GEOMETRY_SHADERS);
  m_features.partial_msaa_resolve = false;
  m_features.memory_import = false;
  m_features.explicit_present = false;
  m_features.timed_present = false;
  m_features.gpu_timing = true;
  m_features.shader_cache = true;
  m_features.pipeline_cache = false;
  m_features.prefer_unused_textures = false;
  m_features.raster_order_views = false;
  if (!(disabled_features & FEATURE_MASK_RASTER_ORDER_VIEWS))
  {
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 data = {};
    m_features.raster_order_views =
      (SUCCEEDED(m_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &data, sizeof(data))) &&
       data.ROVsSupported);
  }
}

u32 D3D11Device::GetSwapChainBufferCount() const
{
  // With vsync off, we only need two buffers. Same for blocking vsync.
  // With triple buffering, we need three.
  return (m_vsync_mode == GPUVSyncMode::Mailbox) ? 3 : 2;
}

bool D3D11Device::CreateSwapChain()
{
  if (m_window_info.type != WindowInfo::Type::Win32)
    return false;

  const DXGI_FORMAT dxgi_format = D3DCommon::GetFormatMapping(s_swap_chain_format).resource_format;

  const HWND window_hwnd = reinterpret_cast<HWND>(m_window_info.window_handle);
  RECT client_rc{};
  GetClientRect(window_hwnd, &client_rc);

  DXGI_MODE_DESC fullscreen_mode = {};
  ComPtr<IDXGIOutput> fullscreen_output;
  if (Host::IsFullscreen())
  {
    u32 fullscreen_width, fullscreen_height;
    float fullscreen_refresh_rate;
    m_is_exclusive_fullscreen =
      GetRequestedExclusiveFullscreenMode(&fullscreen_width, &fullscreen_height, &fullscreen_refresh_rate) &&
      D3DCommon::GetRequestedExclusiveFullscreenModeDesc(m_dxgi_factory.Get(), client_rc, fullscreen_width,
                                                         fullscreen_height, fullscreen_refresh_rate, dxgi_format,
                                                         &fullscreen_mode, fullscreen_output.GetAddressOf());

    // Using mailbox-style no-allow-tearing causes tearing in exclusive fullscreen.
    if (m_vsync_mode == GPUVSyncMode::Mailbox && m_is_exclusive_fullscreen)
    {
      WARNING_LOG("Using FIFO instead of Mailbox vsync due to exclusive fullscreen.");
      m_vsync_mode = GPUVSyncMode::FIFO;
    }
  }
  else
  {
    m_is_exclusive_fullscreen = false;
  }

  m_using_flip_model_swap_chain =
    !Host::GetBoolSettingValue("Display", "UseBlitSwapChain", false) || m_is_exclusive_fullscreen;

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
  swap_chain_desc.Width = static_cast<u32>(client_rc.right - client_rc.left);
  swap_chain_desc.Height = static_cast<u32>(client_rc.bottom - client_rc.top);
  swap_chain_desc.Format = dxgi_format;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferCount = GetSwapChainBufferCount();
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.SwapEffect = m_using_flip_model_swap_chain ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;

  m_using_allow_tearing = (m_allow_tearing_supported && m_using_flip_model_swap_chain && !m_is_exclusive_fullscreen);
  if (m_using_allow_tearing)
    swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  HRESULT hr = S_OK;

  if (m_is_exclusive_fullscreen)
  {
    DXGI_SWAP_CHAIN_DESC1 fs_sd_desc = swap_chain_desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc = {};

    fs_sd_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    fs_sd_desc.Width = fullscreen_mode.Width;
    fs_sd_desc.Height = fullscreen_mode.Height;
    fs_desc.RefreshRate = fullscreen_mode.RefreshRate;
    fs_desc.ScanlineOrdering = fullscreen_mode.ScanlineOrdering;
    fs_desc.Scaling = fullscreen_mode.Scaling;
    fs_desc.Windowed = FALSE;

    VERBOSE_LOG("Creating a {}x{} exclusive fullscreen swap chain", fs_sd_desc.Width, fs_sd_desc.Height);
    hr = m_dxgi_factory->CreateSwapChainForHwnd(m_device.Get(), window_hwnd, &fs_sd_desc, &fs_desc,
                                                fullscreen_output.Get(), m_swap_chain.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
      WARNING_LOG("Failed to create fullscreen swap chain, trying windowed.");
      m_is_exclusive_fullscreen = false;
      m_using_allow_tearing = m_allow_tearing_supported && m_using_flip_model_swap_chain;
    }
  }

  if (!m_is_exclusive_fullscreen)
  {
    VERBOSE_LOG("Creating a {}x{} {} windowed swap chain", swap_chain_desc.Width, swap_chain_desc.Height,
                m_using_flip_model_swap_chain ? "flip-discard" : "discard");
    hr = m_dxgi_factory->CreateSwapChainForHwnd(m_device.Get(), window_hwnd, &swap_chain_desc, nullptr, nullptr,
                                                m_swap_chain.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) && m_using_flip_model_swap_chain)
  {
    WARNING_LOG("Failed to create a flip-discard swap chain, trying discard.");
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swap_chain_desc.Flags = 0;
    m_using_flip_model_swap_chain = false;
    m_using_allow_tearing = false;

    hr = m_dxgi_factory->CreateSwapChainForHwnd(m_device.Get(), window_hwnd, &swap_chain_desc, nullptr, nullptr,
                                                m_swap_chain.ReleaseAndGetAddressOf());
    if (FAILED(hr)) [[unlikely]]
    {
      ERROR_LOG("CreateSwapChainForHwnd failed: 0x{:08X}", static_cast<unsigned>(hr));
      return false;
    }
  }

  // we need the specific factory for the device, otherwise MakeWindowAssociation() is flaky.
  ComPtr<IDXGIFactory> parent_factory;
  if (FAILED(m_swap_chain->GetParent(IID_PPV_ARGS(parent_factory.GetAddressOf()))) ||
      FAILED(parent_factory->MakeWindowAssociation(window_hwnd, DXGI_MWA_NO_WINDOW_CHANGES)))
  {
    WARNING_LOG("MakeWindowAssociation() to disable ALT+ENTER failed");
  }

  if (!CreateSwapChainRTV())
  {
    DestroySwapChain();
    return false;
  }

  // Render a frame as soon as possible to clear out whatever was previously being displayed.
  m_context->ClearRenderTargetView(m_swap_chain_rtv.Get(), s_clear_color.data());
  m_swap_chain->Present(0, m_using_allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
  return true;
}

bool D3D11Device::CreateSwapChainRTV()
{
  ComPtr<ID3D11Texture2D> backbuffer;
  HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
  if (FAILED(hr)) [[unlikely]]
  {
    ERROR_LOG("GetBuffer for RTV failed: 0x{:08X}", static_cast<unsigned>(hr));
    return false;
  }

  D3D11_TEXTURE2D_DESC backbuffer_desc;
  backbuffer->GetDesc(&backbuffer_desc);

  CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(D3D11_RTV_DIMENSION_TEXTURE2D, backbuffer_desc.Format, 0, 0,
                                          backbuffer_desc.ArraySize);
  hr = m_device->CreateRenderTargetView(backbuffer.Get(), &rtv_desc, m_swap_chain_rtv.ReleaseAndGetAddressOf());
  if (FAILED(hr)) [[unlikely]]
  {
    ERROR_LOG("CreateRenderTargetView for swap chain failed: 0x{:08X}", static_cast<unsigned>(hr));
    m_swap_chain_rtv.Reset();
    return false;
  }

  m_window_info.surface_width = backbuffer_desc.Width;
  m_window_info.surface_height = backbuffer_desc.Height;
  m_window_info.surface_format = s_swap_chain_format;
  VERBOSE_LOG("Swap chain buffer size: {}x{}", m_window_info.surface_width, m_window_info.surface_height);

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
  }

  return true;
}

void D3D11Device::DestroySwapChain()
{
  if (!m_swap_chain)
    return;

  m_swap_chain_rtv.Reset();

  // switch out of fullscreen before destroying
  BOOL is_fullscreen;
  if (SUCCEEDED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) && is_fullscreen)
    m_swap_chain->SetFullscreenState(FALSE, nullptr);

  m_swap_chain.Reset();
  m_is_exclusive_fullscreen = false;
}

bool D3D11Device::UpdateWindow()
{
  DestroySwapChain();

  if (!AcquireWindow(false))
    return false;

  if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSwapChain())
  {
    ERROR_LOG("Failed to create swap chain on updated window");
    return false;
  }

  return true;
}

void D3D11Device::DestroySurface()
{
  DestroySwapChain();
}

void D3D11Device::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
  if (!m_swap_chain || m_is_exclusive_fullscreen)
    return;

  m_window_info.surface_scale = new_window_scale;

  if (m_window_info.surface_width == static_cast<u32>(new_window_width) &&
      m_window_info.surface_height == static_cast<u32>(new_window_height))
  {
    return;
  }

  m_swap_chain_rtv.Reset();

  HRESULT hr = m_swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                           m_using_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr)) [[unlikely]]
    ERROR_LOG("ResizeBuffers() failed: 0x{:08X}", static_cast<unsigned>(hr));

  if (!CreateSwapChainRTV())
    Panic("Failed to recreate swap chain RTV after resize");
}

bool D3D11Device::SupportsExclusiveFullscreen() const
{
  return true;
}

std::string D3D11Device::GetDriverInfo() const
{
  std::string ret = fmt::format("{} (Shader Model {})\n", D3DCommon::GetFeatureLevelString(m_render_api_version),
                                D3DCommon::GetShaderModelForFeatureLevelNumber(m_render_api_version));

  ComPtr<IDXGIDevice> dxgi_dev;
  if (SUCCEEDED(m_device.As(&dxgi_dev)))
  {
    ComPtr<IDXGIAdapter> dxgi_adapter;
    if (SUCCEEDED(dxgi_dev->GetAdapter(dxgi_adapter.GetAddressOf())))
    {
      DXGI_ADAPTER_DESC desc;
      if (SUCCEEDED(dxgi_adapter->GetDesc(&desc)))
      {
        fmt::format_to(std::back_inserter(ret), "VID: 0x{:04X} PID: 0x{:04X}\n", desc.VendorId, desc.DeviceId);
        ret += StringUtil::WideStringToUTF8String(desc.Description);
        ret += "\n";

        const std::string driver_version(D3DCommon::GetDriverVersionFromLUID(desc.AdapterLuid));
        if (!driver_version.empty())
        {
          ret += "Driver Version: ";
          ret += driver_version;
        }
      }
    }
  }

  return ret;
}

void D3D11Device::ExecuteAndWaitForGPUIdle()
{
  m_context->Flush();
}

bool D3D11Device::CreateBuffers()
{
  if (!m_vertex_buffer.Create(D3D11_BIND_VERTEX_BUFFER, VERTEX_BUFFER_SIZE, VERTEX_BUFFER_SIZE) ||
      !m_index_buffer.Create(D3D11_BIND_INDEX_BUFFER, INDEX_BUFFER_SIZE, INDEX_BUFFER_SIZE) ||
      !m_uniform_buffer.Create(D3D11_BIND_CONSTANT_BUFFER, MIN_UNIFORM_BUFFER_SIZE, MAX_UNIFORM_BUFFER_SIZE))
  {
    ERROR_LOG("Failed to create vertex/index/uniform buffers.");
    return false;
  }

  // Index buffer never changes :)
  m_context->IASetIndexBuffer(m_index_buffer.GetD3DBuffer(), DXGI_FORMAT_R16_UINT, 0);
  return true;
}

void D3D11Device::DestroyBuffers()
{
  m_uniform_buffer.Destroy();
  m_vertex_buffer.Destroy();
  m_index_buffer.Destroy();
}

void D3D11Device::CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                    GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                    u32 height)
{
  DebugAssert(src_level < src->GetLevels() && src_layer < src->GetLayers());
  DebugAssert((src_x + width) <= src->GetMipWidth(src_level));
  DebugAssert((src_y + height) <= src->GetMipHeight(src_level));
  DebugAssert(dst_level < dst->GetLevels() && dst_layer < dst->GetLayers());
  DebugAssert((dst_x + width) <= dst->GetMipWidth(dst_level));
  DebugAssert((dst_y + height) <= dst->GetMipHeight(dst_level));

  D3D11Texture* dst11 = static_cast<D3D11Texture*>(dst);
  D3D11Texture* src11 = static_cast<D3D11Texture*>(src);

  if (dst11->IsRenderTargetOrDepthStencil())
  {
    if (src11->GetState() == GPUTexture::State::Cleared)
    {
      if (src11->GetWidth() == dst11->GetWidth() && src11->GetHeight() == dst11->GetHeight())
      {
        // pass clear through
        dst11->m_state = src11->m_state;
        dst11->m_clear_value = src11->m_clear_value;
        return;
      }
    }
    else if (dst_x == 0 && dst_y == 0 && width == dst11->GetMipWidth(dst_level) &&
             height == dst11->GetMipHeight(dst_level))
    {
      m_context->DiscardView(dst11->GetRTVOrDSV());
      dst11->SetState(GPUTexture::State::Dirty);
    }

    dst11->CommitClear(m_context.Get());
  }

  src11->CommitClear(m_context.Get());

  s_stats.num_copies++;

  const CD3D11_BOX src_box(static_cast<LONG>(src_x), static_cast<LONG>(src_y), 0, static_cast<LONG>(src_x + width),
                           static_cast<LONG>(src_y + height), 1);
  m_context->CopySubresourceRegion(dst11->GetD3DTexture(), D3D11CalcSubresource(dst_level, dst_layer, dst->GetLevels()),
                                   dst_x, dst_y, 0, src11->GetD3DTexture(),
                                   D3D11CalcSubresource(src_level, src_layer, src->GetLevels()), &src_box);
}

void D3D11Device::ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                       GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height)
{
  DebugAssert((src_x + width) <= src->GetWidth());
  DebugAssert((src_y + height) <= src->GetHeight());
  DebugAssert(src->IsMultisampled());
  DebugAssert(dst_level < dst->GetLevels() && dst_layer < dst->GetLayers());
  DebugAssert((dst_x + width) <= dst->GetMipWidth(dst_level));
  DebugAssert((dst_y + height) <= dst->GetMipHeight(dst_level));
  DebugAssert(!dst->IsMultisampled() && src->IsMultisampled());

  s_stats.num_copies++;

  // DX11 can't resolve partial rects.
  Assert(src_x == 0 && src_y == 0 && width == src->GetWidth() && height == src->GetHeight() && dst_x == 0 &&
         dst_y == 0 && width == dst->GetMipWidth(dst_level) && height == dst->GetMipHeight(dst_level));

  D3D11Texture* dst11 = static_cast<D3D11Texture*>(dst);
  D3D11Texture* src11 = static_cast<D3D11Texture*>(src);

  src11->CommitClear(m_context.Get());
  dst11->CommitClear(m_context.Get());

  m_context->ResolveSubresource(dst11->GetD3DTexture(), D3D11CalcSubresource(dst_level, dst_layer, dst->GetLevels()),
                                src11->GetD3DTexture(), 0, dst11->GetDXGIFormat());
}

bool D3D11Device::IsRenderTargetBound(const D3D11Texture* tex) const
{
  if (tex->IsRenderTarget() || tex->IsRWTexture())
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
        return true;
    }
  }

  return false;
}

void D3D11Device::ClearRenderTarget(GPUTexture* t, u32 c)
{
  D3D11Texture* const T = static_cast<D3D11Texture*>(t);
  GPUDevice::ClearRenderTarget(T, c);
  if (IsRenderTargetBound(T))
    T->CommitClear(m_context.Get());
}

void D3D11Device::ClearDepth(GPUTexture* t, float d)
{
  D3D11Texture* const T = static_cast<D3D11Texture*>(t);
  GPUDevice::ClearDepth(T, d);
  if (T == m_current_depth_target)
    T->CommitClear(m_context.Get());
}

void D3D11Device::InvalidateRenderTarget(GPUTexture* t)
{
  D3D11Texture* const T = static_cast<D3D11Texture*>(t);
  GPUDevice::InvalidateRenderTarget(T);
  if (T->IsDepthStencil() ? (m_current_depth_target == T) : IsRenderTargetBound(T))
    T->CommitClear(m_context.Get());
}

void D3D11Device::SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle)
{
  m_allow_present_throttle = allow_present_throttle;

  // Using mailbox-style no-allow-tearing causes tearing in exclusive fullscreen.
  if (mode == GPUVSyncMode::Mailbox && m_is_exclusive_fullscreen)
  {
    WARNING_LOG("Using FIFO instead of Mailbox vsync due to exclusive fullscreen.");
    mode = GPUVSyncMode::FIFO;
  }

  if (m_vsync_mode == mode)
    return;

  const u32 old_buffer_count = GetSwapChainBufferCount();
  m_vsync_mode = mode;
  if (!m_swap_chain)
    return;

  if (GetSwapChainBufferCount() != old_buffer_count)
  {
    DestroySwapChain();
    if (!CreateSwapChain())
      Panic("Failed to recreate swap chain after vsync change.");
  }
}

GPUDevice::PresentResult D3D11Device::BeginPresent(u32 clear_color)
{
  if (!m_swap_chain)
  {
    // Note: Really slow on Intel...
    m_context->Flush();
    TrimTexturePool();
    return PresentResult::SkipPresent;
  }

  // Check if we lost exclusive fullscreen. If so, notify the host, so it can switch to windowed mode.
  // This might get called repeatedly if it takes a while to switch back, that's the host's problem.
  BOOL is_fullscreen;
  if (m_is_exclusive_fullscreen &&
      (FAILED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) || !is_fullscreen))
  {
    Host::SetFullscreen(false);
    TrimTexturePool();
    return PresentResult::SkipPresent;
  }

  // When using vsync, the time here seems to include the time for the buffer to become available.
  // This blows our our GPU usage number considerably, so read the timestamp before the final blit
  // in this configuration. It does reduce accuracy a little, but better than seeing 100% all of
  // the time, when it's more like a couple of percent.
  if (m_vsync_mode == GPUVSyncMode::FIFO && m_gpu_timing_enabled)
    PopTimestampQuery();

  m_context->ClearRenderTargetView(m_swap_chain_rtv.Get(), GSVector4::rgba32(clear_color).F32);
  m_context->OMSetRenderTargets(1, m_swap_chain_rtv.GetAddressOf(), nullptr);
  s_stats.num_render_passes++;
  m_num_current_render_targets = 0;
  m_current_render_pass_flags = GPUPipeline::NoRenderPassFlags;
  std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
  m_current_depth_target = nullptr;
  return PresentResult::OK;
}

void D3D11Device::EndPresent(bool explicit_present, u64 present_time)
{
  DebugAssert(!explicit_present && present_time == 0);
  DebugAssert(m_num_current_render_targets == 0 && !m_current_depth_target);

  if (m_vsync_mode != GPUVSyncMode::FIFO && m_gpu_timing_enabled)
    PopTimestampQuery();

  const UINT sync_interval = static_cast<UINT>(m_vsync_mode == GPUVSyncMode::FIFO);
  const UINT flags = (m_vsync_mode == GPUVSyncMode::Disabled && m_using_allow_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
  m_swap_chain->Present(sync_interval, flags);

  if (m_gpu_timing_enabled)
    KickTimestampQuery();

  TrimTexturePool();
}

void D3D11Device::SubmitPresent()
{
  Panic("Not supported by this API.");
}

bool D3D11Device::CreateTimestampQueries()
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

void D3D11Device::DestroyTimestampQueries()
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

void D3D11Device::PopTimestampQuery()
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
      VERBOSE_LOG("GPU timing disjoint, resetting.");
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
      else
      {
        // Data not ready yet.
        break;
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

void D3D11Device::KickTimestampQuery()
{
  if (m_timestamp_query_started || !m_timestamp_queries[0][0] || m_waiting_timestamp_queries == NUM_TIMESTAMP_QUERIES)
    return;

  m_context->Begin(m_timestamp_queries[m_write_timestamp_query][0].Get());
  m_context->End(m_timestamp_queries[m_write_timestamp_query][1].Get());
  m_timestamp_query_started = true;
}

bool D3D11Device::SetGPUTimingEnabled(bool enabled)
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

float D3D11Device::GetAndResetAccumulatedGPUTime()
{
  const float value = m_accumulated_gpu_time;
  m_accumulated_gpu_time = 0.0f;
  return value;
}

void D3D11Device::PushDebugGroup(const char* name)
{
#ifdef _DEBUG
  if (!m_annotation)
    return;

  m_annotation->BeginEvent(StringUtil::UTF8StringToWideString(name).c_str());
#endif
}

void D3D11Device::PopDebugGroup()
{
#ifdef _DEBUG
  if (!m_annotation)
    return;

  m_annotation->EndEvent();
#endif
}

void D3D11Device::InsertDebugMessage(const char* msg)
{
#ifdef _DEBUG
  if (!m_annotation)
    return;

  m_annotation->SetMarker(StringUtil::UTF8StringToWideString(msg).c_str());
#endif
}

void D3D11Device::MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                                  u32* map_base_vertex)
{
  const auto res = m_vertex_buffer.Map(m_context.Get(), vertex_size, vertex_size * vertex_count);
  *map_ptr = res.pointer;
  *map_space = res.space_aligned;
  *map_base_vertex = res.index_aligned;
}

void D3D11Device::UnmapVertexBuffer(u32 vertex_size, u32 vertex_count)
{
  const u32 upload_size = vertex_size * vertex_count;
  s_stats.buffer_streamed += upload_size;
  m_vertex_buffer.Unmap(m_context.Get(), upload_size);
}

void D3D11Device::MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index)
{
  const auto res = m_index_buffer.Map(m_context.Get(), sizeof(DrawIndex), sizeof(DrawIndex) * index_count);
  *map_ptr = static_cast<DrawIndex*>(res.pointer);
  *map_space = res.space_aligned;
  *map_base_index = res.index_aligned;
}

void D3D11Device::UnmapIndexBuffer(u32 used_index_count)
{
  s_stats.buffer_streamed += sizeof(DrawIndex) * used_index_count;
  m_index_buffer.Unmap(m_context.Get(), sizeof(DrawIndex) * used_index_count);
}

void D3D11Device::PushUniformBuffer(const void* data, u32 data_size)
{
  const u32 req_align =
    m_uniform_buffer.IsUsingMapNoOverwrite() ? UNIFORM_BUFFER_ALIGNMENT : UNIFORM_BUFFER_ALIGNMENT_DISCARD;
  const u32 req_size = Common::AlignUpPow2(data_size, req_align);
  const auto res = m_uniform_buffer.Map(m_context.Get(), req_align, req_size);
  std::memcpy(res.pointer, data, data_size);
  m_uniform_buffer.Unmap(m_context.Get(), req_size);
  s_stats.buffer_streamed += data_size;

  if (m_uniform_buffer.IsUsingMapNoOverwrite())
  {
    const UINT first_constant = (res.index_aligned * UNIFORM_BUFFER_ALIGNMENT) / 16u;
    const UINT num_constants = req_size / 16u;
    m_context->VSSetConstantBuffers1(0, 1, m_uniform_buffer.GetD3DBufferArray(), &first_constant, &num_constants);
    m_context->PSSetConstantBuffers1(0, 1, m_uniform_buffer.GetD3DBufferArray(), &first_constant, &num_constants);
  }
  else
  {
    DebugAssert(res.index_aligned == 0);
    m_context->VSSetConstantBuffers(0, 1, m_uniform_buffer.GetD3DBufferArray());
    m_context->PSSetConstantBuffers(0, 1, m_uniform_buffer.GetD3DBufferArray());
  }
}

void* D3D11Device::MapUniformBuffer(u32 size)
{
  const u32 req_align =
    m_uniform_buffer.IsUsingMapNoOverwrite() ? UNIFORM_BUFFER_ALIGNMENT : UNIFORM_BUFFER_ALIGNMENT_DISCARD;
  const u32 req_size = Common::AlignUpPow2(size, req_align);
  const auto res = m_uniform_buffer.Map(m_context.Get(), req_align, req_size);
  return res.pointer;
}

void D3D11Device::UnmapUniformBuffer(u32 size)
{
  const u32 pos = m_uniform_buffer.GetPosition();
  const u32 req_align =
    m_uniform_buffer.IsUsingMapNoOverwrite() ? UNIFORM_BUFFER_ALIGNMENT : UNIFORM_BUFFER_ALIGNMENT_DISCARD;
  const u32 req_size = Common::AlignUpPow2(size, req_align);

  m_uniform_buffer.Unmap(m_context.Get(), req_size);
  s_stats.buffer_streamed += size;

  if (m_uniform_buffer.IsUsingMapNoOverwrite())
  {
    const UINT first_constant = pos / 16u;
    const UINT num_constants = req_size / 16u;
    m_context->VSSetConstantBuffers1(0, 1, m_uniform_buffer.GetD3DBufferArray(), &first_constant, &num_constants);
    m_context->PSSetConstantBuffers1(0, 1, m_uniform_buffer.GetD3DBufferArray(), &first_constant, &num_constants);
  }
  else
  {
    DebugAssert(pos == 0);
    m_context->VSSetConstantBuffers(0, 1, m_uniform_buffer.GetD3DBufferArray());
    m_context->PSSetConstantBuffers(0, 1, m_uniform_buffer.GetD3DBufferArray());
  }
}

void D3D11Device::SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds,
                                   GPUPipeline::RenderPassFlag flags)
{
  DebugAssert(
    !(flags & (GPUPipeline::RenderPassFlag::ColorFeedbackLoop | GPUPipeline::RenderPassFlag::SampleDepthBuffer)));

  // Make sure DSV isn't bound.
  D3D11Texture* DS = static_cast<D3D11Texture*>(ds);
  if (DS)
    DS->CommitClear(m_context.Get());

  bool changed =
    (m_num_current_render_targets != num_rts || m_current_depth_target != DS || m_current_render_pass_flags != flags);
  m_current_render_pass_flags = flags;
  m_current_depth_target = DS;
  if (ds)
  {
    const ID3D11ShaderResourceView* srv = static_cast<D3D11Texture*>(ds)->GetD3DSRV();
    for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
    {
      if (m_current_textures[i] && m_current_textures[i] == srv)
      {
        m_current_textures[i] = nullptr;
        m_context->PSSetShaderResources(i, 1, &m_current_textures[i]);
      }
    }
  }

  for (u32 i = 0; i < num_rts; i++)
  {
    D3D11Texture* const RT = static_cast<D3D11Texture*>(rts[i]);
    changed |= m_current_render_targets[i] != RT;
    m_current_render_targets[i] = RT;
    RT->CommitClear(m_context.Get());

    const ID3D11ShaderResourceView* srv = RT->GetD3DSRV();
    for (u32 j = 0; j < MAX_TEXTURE_SAMPLERS; j++)
    {
      if (m_current_textures[j] && m_current_textures[j] == srv)
      {
        m_current_textures[j] = nullptr;
        m_context->PSSetShaderResources(j, 1, &m_current_textures[j]);
      }
    }
  }
  for (u32 i = num_rts; i < m_num_current_render_targets; i++)
    m_current_render_targets[i] = nullptr;
  m_num_current_render_targets = num_rts;
  if (!changed)
    return;

  s_stats.num_render_passes++;

  if (m_current_render_pass_flags & GPUPipeline::BindRenderTargetsAsImages)
  {
    std::array<ID3D11UnorderedAccessView*, MAX_RENDER_TARGETS> uavs;
    for (u32 i = 0; i < m_num_current_render_targets; i++)
      uavs[i] = m_current_render_targets[i]->GetD3DUAV();

    m_context->OMSetRenderTargetsAndUnorderedAccessViews(
      0, nullptr, m_current_depth_target ? m_current_depth_target->GetD3DDSV() : nullptr, 0,
      m_num_current_render_targets, uavs.data(), nullptr);
  }
  else
  {
    std::array<ID3D11RenderTargetView*, MAX_RENDER_TARGETS> rtvs;
    for (u32 i = 0; i < m_num_current_render_targets; i++)
      rtvs[i] = m_current_render_targets[i]->GetD3DRTV();

    m_context->OMSetRenderTargets(m_num_current_render_targets,
                                  (m_num_current_render_targets > 0) ? rtvs.data() : nullptr,
                                  m_current_depth_target ? m_current_depth_target->GetD3DDSV() : nullptr);
  }
}

void D3D11Device::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  ID3D11ShaderResourceView* T;
  if (texture)
  {
    static_cast<D3D11Texture*>(texture)->CommitClear(m_context.Get());
    T = static_cast<D3D11Texture*>(texture)->GetD3DSRV();
  }
  else
  {
    T = nullptr;
  }

  ID3D11SamplerState* S = sampler ? static_cast<D3D11Sampler*>(sampler)->GetSamplerState() : nullptr;

  // Runtime will null these if we don't...
  DebugAssert(!texture ||
              !((texture->IsRenderTarget() || texture->IsRWTexture()) &&
                IsRenderTargetBound(static_cast<D3D11Texture*>(texture))) ||
              !(texture->IsDepthStencil() &&
                (!m_current_depth_target || m_current_depth_target != static_cast<D3D11Texture*>(texture))));

  if (m_current_textures[slot] != T)
  {
    m_current_textures[slot] = T;
    m_context->PSSetShaderResources(slot, 1, &T);
  }
  if (m_current_samplers[slot] != S)
  {
    m_current_samplers[slot] = S;
    m_context->PSSetSamplers(slot, 1, &S);
  }
}

void D3D11Device::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  ID3D11ShaderResourceView* B = buffer ? static_cast<D3D11TextureBuffer*>(buffer)->GetSRV() : nullptr;
  if (m_current_textures[slot] != B)
  {
    m_current_textures[slot] = B;
    m_context->PSSetShaderResources(slot, 1, &B);
  }
}

void D3D11Device::UnbindTexture(D3D11Texture* tex)
{
  if (const ID3D11ShaderResourceView* srv = tex->GetD3DSRV(); srv)
  {
    for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
    {
      if (m_current_textures[i] == srv)
      {
        m_current_textures[i] = nullptr;
        m_context->PSSetShaderResources(i, 1, &m_current_textures[i]);
      }
    }
  }

  if (tex->IsRenderTarget() || tex->IsRWTexture())
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
      {
        WARNING_LOG("Unbinding current RT");
        SetRenderTargets(nullptr, 0, m_current_depth_target);
        break;
      }
    }
  }
  else if (tex->IsDepthStencil() && m_current_depth_target == tex)
  {
    WARNING_LOG("Unbinding current DS");
    SetRenderTargets(nullptr, 0, nullptr);
  }
}

void D3D11Device::SetViewport(const GSVector4i rc)
{
  const CD3D11_VIEWPORT vp(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(rc.width()),
                           static_cast<float>(rc.height()), 0.0f, 1.0f);
  m_context->RSSetViewports(1, &vp);
}

void D3D11Device::SetScissor(const GSVector4i rc)
{
  alignas(16) D3D11_RECT drc;
  GSVector4i::store<true>(&drc, rc);
  m_context->RSSetScissorRects(1, &drc);
}

void D3D11Device::Draw(u32 vertex_count, u32 base_vertex)
{
  DebugAssert(!m_vertex_buffer.IsMapped() && !m_index_buffer.IsMapped());
  s_stats.num_draws++;
  m_context->Draw(vertex_count, base_vertex);
}

void D3D11Device::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  DebugAssert(!m_vertex_buffer.IsMapped() && !m_index_buffer.IsMapped());
  s_stats.num_draws++;
  m_context->DrawIndexed(index_count, base_index, base_vertex);
}

void D3D11Device::DrawIndexedWithBarrier(u32 index_count, u32 base_index, u32 base_vertex, DrawBarrier type)
{
  Panic("Barriers are not supported");
}