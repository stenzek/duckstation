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

LOG_CHANNEL(GPUDevice);

// We need to synchronize instance creation because of adapter enumeration from the UI thread.
static std::mutex s_instance_mutex;

static constexpr std::array<float, 4> s_clear_color = {};
static constexpr GPUTexture::Format s_swap_chain_format = GPUTexture::Format::RGBA8;

void SetD3DDebugObjectName(ID3D11DeviceChild* obj, std::string_view name)
{
#ifdef ENABLE_GPU_OBJECT_NAMES
  // WKPDID_D3DDebugObjectName
  static constexpr GUID guid = {0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};

  UINT existing_data_size;
  HRESULT hr = obj->GetPrivateData(guid, &existing_data_size, nullptr);
  if (SUCCEEDED(hr) && existing_data_size > 0)
    return;

  obj->SetPrivateData(guid, static_cast<UINT>(name.length()), name.data());
#endif
}

D3D11Device::D3D11Device()
{
  m_render_api = RenderAPI::D3D11;
  m_features.exclusive_fullscreen = true; // set so the caller can pass a mode to CreateDeviceAndSwapChain()
}

D3D11Device::~D3D11Device()
{
  // Should all be torn down by now.
  Assert(!m_device);
}

bool D3D11Device::CreateDeviceAndMainSwapChain(std::string_view adapter, CreateFlags create_flags, const WindowInfo& wi,
                                               GPUVSyncMode vsync_mode, bool allow_present_throttle,
                                               const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                               std::optional<bool> exclusive_fullscreen_control, Error* error)
{
  std::unique_lock lock(s_instance_mutex);

  UINT d3d_create_flags = 0;
  if (m_debug_device)
    d3d_create_flags |= D3D11_CREATE_DEVICE_DEBUG;

  m_dxgi_factory = D3DCommon::CreateFactory(m_debug_device, error);
  if (!m_dxgi_factory)
    return false;

  ComPtr<IDXGIAdapter1> dxgi_adapter = D3DCommon::GetAdapterByName(m_dxgi_factory.Get(), adapter);
  m_max_feature_level = D3DCommon::GetDeviceMaxFeatureLevel(dxgi_adapter.Get());

  static constexpr std::array<D3D_FEATURE_LEVEL, 4> requested_feature_levels = {
    {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0}};

  ComPtr<ID3D11Device> temp_device;
  ComPtr<ID3D11DeviceContext> temp_context;
  HRESULT hr;
  if (!D3DCommon::CreateD3D11Device(dxgi_adapter.Get(), d3d_create_flags, requested_feature_levels.data(),
                                    static_cast<UINT>(requested_feature_levels.size()), &temp_device, nullptr,
                                    &temp_context, error))
  {
    return false;
  }
  else if (FAILED(hr = temp_device.As(&m_device)) || FAILED(hr = temp_context.As(&m_context)))
  {
    Error::SetHResult(error, "Failed to get D3D11.1 device: ", hr);
    return false;
  }

  // just in case the max query failed, apparently happens for some people...
  m_max_feature_level = std::max(m_max_feature_level, m_device->GetFeatureLevel());

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

#ifdef ENABLE_GPU_OBJECT_NAMES
  if (m_debug_device)
    m_context.As(&m_annotation);
#endif

  ComPtr<IDXGIDevice> dxgi_device;
  GPUDriverType driver_type = GPUDriverType::Unknown;
  if (SUCCEEDED(m_device.As(&dxgi_device)) &&
      SUCCEEDED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.GetAddressOf()))))
    INFO_LOG("D3D Adapter: {}", D3DCommon::GetAdapterName(dxgi_adapter.Get(), &driver_type));
  else
    ERROR_LOG("Failed to obtain D3D adapter name.");
  INFO_LOG("Max device feature level: {}",
           D3DCommon::GetFeatureLevelString(D3DCommon::GetRenderAPIVersionForFeatureLevel(m_max_feature_level)));

  SetDriverType(driver_type);
  SetFeatures(create_flags);

  if (!wi.IsSurfaceless())
  {
    m_main_swap_chain = CreateSwapChain(wi, vsync_mode, allow_present_throttle, exclusive_fullscreen_mode,
                                        exclusive_fullscreen_control, error);
    if (!m_main_swap_chain)
      return false;
  }

  if (!CreateBuffers(error))
    return false;

  return true;
}

void D3D11Device::DestroyDevice()
{
  std::unique_lock lock(s_instance_mutex);

  DestroyBuffers();
  m_main_swap_chain.reset();
  m_context.Reset();
  m_device.Reset();
}

void D3D11Device::SetFeatures(CreateFlags create_flags)
{
  const D3D_FEATURE_LEVEL feature_level = m_device->GetFeatureLevel();

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
      m_max_multisamples = static_cast<u16>(multisamples);
    }
  }

  m_features.dual_source_blend = !HasCreateFlag(create_flags, CreateFlags::DisableDualSourceBlend);
  m_features.framebuffer_fetch = false;
  m_features.per_sample_shading = (feature_level >= D3D_FEATURE_LEVEL_10_1);
  m_features.noperspective_interpolation = true;
  m_features.texture_copy_to_self = false;
  m_features.texture_buffers = !HasCreateFlag(create_flags, CreateFlags::DisableTextureBuffers);
  m_features.texture_buffers_emulated_with_ssbo = false;
  m_features.feedback_loops = false;
  m_features.geometry_shaders = !HasCreateFlag(create_flags, CreateFlags::DisableGeometryShaders);
  m_features.compute_shaders =
    (!HasCreateFlag(create_flags, CreateFlags::DisableComputeShaders) && feature_level >= D3D_FEATURE_LEVEL_11_0);
  m_features.partial_msaa_resolve = false;
  m_features.memory_import = false;
  m_features.exclusive_fullscreen = true;
  m_features.explicit_present = false;
  m_features.timed_present = false;
  m_features.gpu_timing = true;
  m_features.shader_cache = true;
  m_features.pipeline_cache = false;
  m_features.prefer_unused_textures = false;
  m_features.raster_order_views = false;
  if (!HasCreateFlag(create_flags, CreateFlags::DisableRasterOrderViews))
  {
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 data = {};
    m_features.raster_order_views =
      (SUCCEEDED(m_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &data, sizeof(data))) &&
       data.ROVsSupported);
  }

  m_features.dxt_textures =
    (!HasCreateFlag(create_flags, CreateFlags::DisableCompressedTextures) &&
     (SupportsTextureFormat(GPUTexture::Format::BC1) && SupportsTextureFormat(GPUTexture::Format::BC2) &&
      SupportsTextureFormat(GPUTexture::Format::BC3)));
  m_features.bptc_textures = (!HasCreateFlag(create_flags, CreateFlags::DisableCompressedTextures) &&
                              SupportsTextureFormat(GPUTexture::Format::BC7));
}

D3D11SwapChain::D3D11SwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode, bool allow_present_throttle,
                               const GPUDevice::ExclusiveFullscreenMode* fullscreen_mode)
  : GPUSwapChain(wi, vsync_mode, allow_present_throttle)
{
  if (fullscreen_mode)
    InitializeExclusiveFullscreenMode(fullscreen_mode);
}

D3D11SwapChain::~D3D11SwapChain()
{
  m_swap_chain_rtv.Reset();
  DestroySwapChain();
}

bool D3D11SwapChain::InitializeExclusiveFullscreenMode(const GPUDevice::ExclusiveFullscreenMode* mode)
{
  const D3DCommon::DXGIFormatMapping& fm = D3DCommon::GetFormatMapping(s_swap_chain_format);

  const HWND window_hwnd = reinterpret_cast<HWND>(m_window_info.window_handle);
  RECT client_rc{};
  GetClientRect(window_hwnd, &client_rc);

  // Little bit messy...
  HRESULT hr;
  ComPtr<IDXGIDevice> dxgi_dev;
  if (FAILED((hr = D3D11Device::GetD3DDevice()->QueryInterface(IID_PPV_ARGS(dxgi_dev.GetAddressOf())))))
  {
    ERROR_LOG("Failed to get DXGIDevice from D3D device: {:08X}", static_cast<unsigned>(hr));
    return false;
  }
  ComPtr<IDXGIAdapter> dxgi_adapter;
  if (FAILED((hr = dxgi_dev->GetAdapter(dxgi_adapter.GetAddressOf()))))
  {
    ERROR_LOG("Failed to get DXGIAdapter from DXGIDevice: {:08X}", static_cast<unsigned>(hr));
    return false;
  }

  m_fullscreen_mode = D3DCommon::GetRequestedExclusiveFullscreenModeDesc(
    dxgi_adapter.Get(), client_rc, mode, fm.resource_format, m_fullscreen_output.GetAddressOf());
  return m_fullscreen_mode.has_value();
}

u32 D3D11SwapChain::GetNewBufferCount(GPUVSyncMode vsync_mode)
{
  // With vsync off, we only need two buffers. Same for blocking vsync.
  // With triple buffering, we need three.
  return (vsync_mode == GPUVSyncMode::Mailbox) ? 3 : 2;
}

bool D3D11SwapChain::CreateSwapChain(Error* error)
{
  const D3DCommon::DXGIFormatMapping& fm = D3DCommon::GetFormatMapping(s_swap_chain_format);

  const HWND window_hwnd = reinterpret_cast<HWND>(m_window_info.window_handle);
  RECT client_rc{};
  GetClientRect(window_hwnd, &client_rc);

  // Using mailbox-style no-allow-tearing causes tearing in exclusive fullscreen.
  if (IsExclusiveFullscreen() && m_vsync_mode == GPUVSyncMode::Mailbox)
  {
    WARNING_LOG("Using FIFO instead of Mailbox vsync due to exclusive fullscreen.");
    m_vsync_mode = GPUVSyncMode::FIFO;
  }

  m_using_flip_model_swap_chain =
    !Host::GetBoolSettingValue("Display", "UseBlitSwapChain", false) || IsExclusiveFullscreen();

  IDXGIFactory5* const dxgi_factory = D3D11Device::GetDXGIFactory();
  ID3D11Device1* const d3d_device = D3D11Device::GetD3DDevice();

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
  swap_chain_desc.Width = static_cast<u32>(client_rc.right - client_rc.left);
  swap_chain_desc.Height = static_cast<u32>(client_rc.bottom - client_rc.top);
  swap_chain_desc.Format = fm.resource_format;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferCount = GetNewBufferCount(m_vsync_mode);
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.SwapEffect = m_using_flip_model_swap_chain ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;

  HRESULT hr = S_OK;

  if (IsExclusiveFullscreen())
  {
    DXGI_SWAP_CHAIN_DESC1 fs_sd_desc = swap_chain_desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc = {};

    fs_sd_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    fs_sd_desc.Width = m_fullscreen_mode->Width;
    fs_sd_desc.Height = m_fullscreen_mode->Height;
    fs_desc.RefreshRate = m_fullscreen_mode->RefreshRate;
    fs_desc.ScanlineOrdering = m_fullscreen_mode->ScanlineOrdering;
    fs_desc.Scaling = m_fullscreen_mode->Scaling;
    fs_desc.Windowed = FALSE;

    VERBOSE_LOG("Creating a {}x{} exclusive fullscreen swap chain", fs_sd_desc.Width, fs_sd_desc.Height);
    hr = dxgi_factory->CreateSwapChainForHwnd(d3d_device, window_hwnd, &fs_sd_desc, &fs_desc, m_fullscreen_output.Get(),
                                              m_swap_chain.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
      WARNING_LOG("Failed to create fullscreen swap chain, trying windowed.");
      m_fullscreen_output.Reset();
      m_fullscreen_mode.reset();
      m_using_allow_tearing = (m_using_flip_model_swap_chain && D3DCommon::SupportsAllowTearing(dxgi_factory));
    }
  }

  if (!IsExclusiveFullscreen())
  {
    VERBOSE_LOG("Creating a {}x{} {} windowed swap chain", swap_chain_desc.Width, swap_chain_desc.Height,
                m_using_flip_model_swap_chain ? "flip-discard" : "discard");
    m_using_allow_tearing = (m_using_flip_model_swap_chain && !IsExclusiveFullscreen() &&
                             D3DCommon::SupportsAllowTearing(D3D11Device::GetDXGIFactory()));
    if (m_using_allow_tearing)
      swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    hr = dxgi_factory->CreateSwapChainForHwnd(d3d_device, window_hwnd, &swap_chain_desc, nullptr, nullptr,
                                              m_swap_chain.ReleaseAndGetAddressOf());
  }

  if (FAILED(hr) && m_using_flip_model_swap_chain)
  {
    WARNING_LOG("Failed to create a flip-discard swap chain, trying discard.");
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swap_chain_desc.Flags = 0;
    m_using_flip_model_swap_chain = false;
    m_using_allow_tearing = false;

    hr = dxgi_factory->CreateSwapChainForHwnd(d3d_device, window_hwnd, &swap_chain_desc, nullptr, nullptr,
                                              m_swap_chain.ReleaseAndGetAddressOf());
    if (FAILED(hr)) [[unlikely]]
    {
      Error::SetHResult(error, "CreateSwapChainForHwnd() failed: ", hr);
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

  return true;
}

void D3D11SwapChain::DestroySwapChain()
{
  if (!m_swap_chain)
    return;

  // switch out of fullscreen before destroying
  BOOL is_fullscreen;
  if (SUCCEEDED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) && is_fullscreen)
    m_swap_chain->SetFullscreenState(FALSE, nullptr);

  m_swap_chain.Reset();
}

bool D3D11SwapChain::CreateRTV(Error* error)
{
  ComPtr<ID3D11Texture2D> backbuffer;
  HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "GetBuffer() failed: ", hr);
    return false;
  }

  D3D11_TEXTURE2D_DESC backbuffer_desc;
  backbuffer->GetDesc(&backbuffer_desc);

  CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(D3D11_RTV_DIMENSION_TEXTURE2D, backbuffer_desc.Format, 0, 0,
                                          backbuffer_desc.ArraySize);
  hr = D3D11Device::GetD3DDevice()->CreateRenderTargetView(backbuffer.Get(), &rtv_desc,
                                                           m_swap_chain_rtv.ReleaseAndGetAddressOf());
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "CreateRenderTargetView(): ", hr);
    m_swap_chain_rtv.Reset();
    return false;
  }

  m_window_info.surface_width = static_cast<u16>(backbuffer_desc.Width);
  m_window_info.surface_height = static_cast<u16>(backbuffer_desc.Height);
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

bool D3D11SwapChain::ResizeBuffers(u32 new_width, u32 new_height, float new_scale, Error* error)
{
  m_window_info.surface_scale = new_scale;
  if (m_window_info.surface_width == new_width && m_window_info.surface_height == new_height)
    return true;

  m_swap_chain_rtv.Reset();

  HRESULT hr = m_swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                           m_using_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr)) [[unlikely]]
  {
    Error::SetHResult(error, "ResizeBuffers() failed: ", hr);
    return false;
  }

  return CreateRTV(error);
}

bool D3D11SwapChain::SetVSyncMode(GPUVSyncMode mode, bool allow_present_throttle, Error* error)
{
  m_allow_present_throttle = allow_present_throttle;

  // Using mailbox-style no-allow-tearing causes tearing in exclusive fullscreen.
  if (mode == GPUVSyncMode::Mailbox && IsExclusiveFullscreen())
  {
    WARNING_LOG("Using FIFO instead of Mailbox vsync due to exclusive fullscreen.");
    mode = GPUVSyncMode::FIFO;
  }

  if (m_vsync_mode == mode)
    return true;

  const u32 old_buffer_count = GetNewBufferCount(m_vsync_mode);
  const u32 new_buffer_count = GetNewBufferCount(mode);
  m_vsync_mode = mode;
  if (old_buffer_count == new_buffer_count)
    return true;

  // Buffer count change => needs recreation.
  m_swap_chain_rtv.Reset();
  DestroySwapChain();
  return CreateSwapChain(error) && CreateRTV(error);
}

bool D3D11SwapChain::IsExclusiveFullscreen() const
{
  return m_fullscreen_mode.has_value();
}

std::unique_ptr<GPUSwapChain> D3D11Device::CreateSwapChain(const WindowInfo& wi, GPUVSyncMode vsync_mode,
                                                           bool allow_present_throttle,
                                                           const ExclusiveFullscreenMode* exclusive_fullscreen_mode,
                                                           std::optional<bool> exclusive_fullscreen_control,
                                                           Error* error)
{
  std::unique_ptr<D3D11SwapChain> ret;
  if (wi.type != WindowInfo::Type::Win32)
  {
    Error::SetStringView(error, "Cannot create a swap chain on non-win32 window.");
    return ret;
  }

  ret = std::make_unique<D3D11SwapChain>(wi, vsync_mode, allow_present_throttle, exclusive_fullscreen_mode);
  if (ret->CreateSwapChain(error) && ret->CreateRTV(error))
  {
    // Render a frame as soon as possible to clear out whatever was previously being displayed.
    m_context->ClearRenderTargetView(ret->GetRTV(), s_clear_color.data());
    ret->GetSwapChain()->Present(0, ret->IsUsingAllowTearing() ? DXGI_PRESENT_ALLOW_TEARING : 0);
  }
  else
  {
    ret.reset();
  }

  return ret;
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

void D3D11Device::FlushCommands()
{
  m_context->Flush();
  EndTimestampQuery();
  TrimTexturePool();
}

void D3D11Device::WaitForGPUIdle()
{
  m_context->Flush();
  EndTimestampQuery();
  TrimTexturePool();
}

bool D3D11Device::CreateBuffers(Error* error)
{
  if (!m_vertex_buffer.Create(D3D11_BIND_VERTEX_BUFFER, VERTEX_BUFFER_SIZE, VERTEX_BUFFER_SIZE, error) ||
      !m_index_buffer.Create(D3D11_BIND_INDEX_BUFFER, INDEX_BUFFER_SIZE, INDEX_BUFFER_SIZE, error) ||
      !m_uniform_buffer.Create(D3D11_BIND_CONSTANT_BUFFER, MIN_UNIFORM_BUFFER_SIZE, MAX_UNIFORM_BUFFER_SIZE, error))
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
  if (tex->IsRenderTarget() || tex->HasFlag(GPUTexture::Flags::AllowBindAsImage))
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

GPUDevice::PresentResult D3D11Device::BeginPresent(GPUSwapChain* swap_chain, u32 clear_color)
{
  D3D11SwapChain* const SC = static_cast<D3D11SwapChain*>(swap_chain);

  // Check if we lost exclusive fullscreen. If so, notify the host, so it can switch to windowed mode.
  // This might get called repeatedly if it takes a while to switch back, that's the host's problem.
  BOOL is_fullscreen;
  if (SC->IsExclusiveFullscreen() &&
      (FAILED(SC->GetSwapChain()->GetFullscreenState(&is_fullscreen, nullptr)) || !is_fullscreen))
  {
    TrimTexturePool();
    return PresentResult::ExclusiveFullscreenLost;
  }

  // The time here seems to include the time for the buffer to become available.
  // This blows our our GPU usage number considerably, so read the timestamp before the final blit
  // in this configuration. It does reduce accuracy a little, but better than seeing 100% all of
  // the time, when it's more like a couple of percent.
  if (SC == m_main_swap_chain.get() && m_gpu_timing_enabled)
  {
    PopTimestampQuery();
    EndTimestampQuery();
  }

  m_context->ClearRenderTargetView(SC->GetRTV(), GSVector4::unorm8(clear_color).F32);

  // Ugh, have to clear out any UAV bindings...
  if (m_current_render_pass_flags & GPUPipeline::BindRenderTargetsAsImages && !m_current_compute_shader)
    m_context->OMSetRenderTargetsAndUnorderedAccessViews(1, SC->GetRTVArray(), nullptr, 0, 0, nullptr, nullptr);
  else
    m_context->OMSetRenderTargets(1, SC->GetRTVArray(), nullptr);
  if (m_current_compute_shader)
    UnbindComputePipeline();
  s_stats.num_render_passes++;
  m_num_current_render_targets = 0;
  m_current_render_pass_flags = GPUPipeline::NoRenderPassFlags;
  std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
  m_current_depth_target = nullptr;
  return PresentResult::OK;
}

void D3D11Device::EndPresent(GPUSwapChain* swap_chain, bool explicit_present, u64 present_time)
{
  D3D11SwapChain* const SC = static_cast<D3D11SwapChain*>(swap_chain);
  DebugAssert(!explicit_present && present_time == 0);
  DebugAssert(m_num_current_render_targets == 0 && !m_current_depth_target);

  const UINT sync_interval = static_cast<UINT>(SC->GetVSyncMode() == GPUVSyncMode::FIFO);
  const UINT flags =
    (SC->GetVSyncMode() == GPUVSyncMode::Disabled && SC->IsUsingAllowTearing()) ? DXGI_PRESENT_ALLOW_TEARING : 0;
  SC->GetSwapChain()->Present(sync_interval, flags);

  if (m_gpu_timing_enabled)
    StartTimestampQuery();

  TrimTexturePool();
}

void D3D11Device::SubmitPresent(GPUSwapChain* swap_chain)
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

  StartTimestampQuery();
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
}

void D3D11Device::EndTimestampQuery()
{
  if (m_timestamp_query_started)
  {
    m_context->End(m_timestamp_queries[m_write_timestamp_query][2].Get());
    m_context->End(m_timestamp_queries[m_write_timestamp_query][0].Get());
    m_write_timestamp_query = (m_write_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
    m_timestamp_query_started = false;
    m_waiting_timestamp_queries++;
  }
}

void D3D11Device::StartTimestampQuery()
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

    StartTimestampQuery();
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

#ifdef ENABLE_GPU_OBJECT_NAMES

void D3D11Device::PushDebugGroup(const char* name)
{
  if (!m_annotation)
    return;

  m_annotation->BeginEvent(StringUtil::UTF8StringToWideString(name).c_str());
}

void D3D11Device::PopDebugGroup()
{
  if (!m_annotation)
    return;

  m_annotation->EndEvent();
}

void D3D11Device::InsertDebugMessage(const char* msg)
{
  if (!m_annotation)
    return;

  m_annotation->SetMarker(StringUtil::UTF8StringToWideString(msg).c_str());
}

#endif

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

  BindUniformBuffer(res.index_aligned * UNIFORM_BUFFER_ALIGNMENT, req_size);
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

  BindUniformBuffer(pos, req_size);
}

void D3D11Device::BindUniformBuffer(u32 offset, u32 size)
{
  if (m_uniform_buffer.IsUsingMapNoOverwrite())
  {
    const UINT first_constant = offset / 16u;
    const UINT num_constants = size / 16u;
    if (m_current_compute_shader)
    {
      m_context->CSSetConstantBuffers1(0, 1, m_uniform_buffer.GetD3DBufferArray(), &first_constant, &num_constants);
    }
    else
    {
      m_context->VSSetConstantBuffers1(0, 1, m_uniform_buffer.GetD3DBufferArray(), &first_constant, &num_constants);
      m_context->PSSetConstantBuffers1(0, 1, m_uniform_buffer.GetD3DBufferArray(), &first_constant, &num_constants);
    }
  }
  else
  {
    DebugAssert(offset == 0);
    if (m_current_compute_shader)
    {
      m_context->CSSetConstantBuffers(0, 1, m_uniform_buffer.GetD3DBufferArray());
    }
    else
    {
      m_context->VSSetConstantBuffers(0, 1, m_uniform_buffer.GetD3DBufferArray());
      m_context->PSSetConstantBuffers(0, 1, m_uniform_buffer.GetD3DBufferArray());
    }
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

    if (!m_current_compute_shader)
    {
      m_context->OMSetRenderTargetsAndUnorderedAccessViews(
        0, nullptr, m_current_depth_target ? m_current_depth_target->GetD3DDSV() : nullptr, 0,
        m_num_current_render_targets, uavs.data(), nullptr);
    }
    else
    {
      m_context->CSSetUnorderedAccessViews(0, m_num_current_render_targets, uavs.data(), nullptr);
    }
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
              !((texture->IsRenderTarget() || texture->HasFlag(GPUTexture::Flags::AllowBindAsImage)) &&
                IsRenderTargetBound(static_cast<D3D11Texture*>(texture))) ||
              !(texture->IsDepthStencil() &&
                (!m_current_depth_target || m_current_depth_target != static_cast<D3D11Texture*>(texture))));

  if (m_current_textures[slot] != T)
  {
    m_current_textures[slot] = T;
    m_context->PSSetShaderResources(slot, 1, &T);
    if (m_current_compute_shader)
      m_context->CSSetShaderResources(slot, 1, &T);
  }
  if (m_current_samplers[slot] != S)
  {
    m_current_samplers[slot] = S;
    m_context->PSSetSamplers(slot, 1, &S);
    if (m_current_compute_shader)
      m_context->CSSetSamplers(slot, 1, &S);
  }
}

void D3D11Device::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  ID3D11ShaderResourceView* B = buffer ? static_cast<D3D11TextureBuffer*>(buffer)->GetSRV() : nullptr;
  if (m_current_textures[slot] != B)
  {
    m_current_textures[slot] = B;

    // Compute doesn't support texture buffers, yet...
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

  if (tex->IsRenderTarget() || tex->HasFlag(GPUTexture::Flags::AllowBindAsImage))
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
      {
        DEV_LOG("Unbinding current RT");
        SetRenderTargets(nullptr, 0, m_current_depth_target);
        break;
      }
    }
  }
  else if (tex->IsDepthStencil() && m_current_depth_target == tex)
  {
    DEV_LOG("Unbinding current DS");
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
  DebugAssert(!m_vertex_buffer.IsMapped() && !m_index_buffer.IsMapped() && !m_current_compute_shader);
  s_stats.num_draws++;
  m_context->Draw(vertex_count, base_vertex);
}

void D3D11Device::DrawWithPushConstants(u32 vertex_count, u32 base_vertex, const void* push_constants,
                                        u32 push_constants_size)
{
  PushUniformBuffer(push_constants, push_constants_size);
  Draw(vertex_count, base_vertex);
}

void D3D11Device::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  DebugAssert(!m_vertex_buffer.IsMapped() && !m_index_buffer.IsMapped() && !m_current_compute_shader);
  s_stats.num_draws++;
  m_context->DrawIndexed(index_count, base_index, base_vertex);
}

void D3D11Device::DrawIndexedWithPushConstants(u32 index_count, u32 base_index, u32 base_vertex,
                                               const void* push_constants, u32 push_constants_size)
{
  PushUniformBuffer(push_constants, push_constants_size);
  DrawIndexed(index_count, base_index, base_vertex);
}

void D3D11Device::Dispatch(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x, u32 group_size_y,
                           u32 group_size_z)
{
  DebugAssert(m_current_compute_shader);
  s_stats.num_draws++;

  const u32 groups_x = threads_x / group_size_x;
  const u32 groups_y = threads_y / group_size_y;
  const u32 groups_z = threads_z / group_size_z;
  m_context->Dispatch(groups_x, groups_y, groups_z);
}

void D3D11Device::DispatchWithPushConstants(u32 threads_x, u32 threads_y, u32 threads_z, u32 group_size_x,
                                            u32 group_size_y, u32 group_size_z, const void* push_constants,
                                            u32 push_constants_size)
{
  PushUniformBuffer(push_constants, push_constants_size);
  Dispatch(threads_x, threads_y, threads_z, group_size_x, group_size_y, group_size_z);
}
