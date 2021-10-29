#include "d3d12_host_display.h"
#include "common/assert.h"
#include "common/d3d11/shader_compiler.h"
#include "common/d3d12/context.h"
#include "common/d3d12/util.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/host_interface.h"
#include "core/settings.h"
#include "display_ps.hlsl.h"
#include "display_vs.hlsl.h"
#include "frontend-common/postprocessing_shadergen.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <array>
#include <dxgi1_5.h>
Log_SetChannel(D3D12HostDisplay);

namespace FrontendCommon {

static constexpr std::array<DXGI_FORMAT, static_cast<u32>(HostDisplayPixelFormat::Count)>
  s_display_pixel_format_mapping = {{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                                     DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM}};

class D3D12HostDisplayTexture : public HostDisplayTexture
{
public:
  D3D12HostDisplayTexture(D3D12::Texture texture) : m_texture(std::move(texture)) {}
  ~D3D12HostDisplayTexture() override = default;

  void* GetHandle() const override { return const_cast<D3D12::Texture*>(&m_texture); }
  u32 GetWidth() const override { return m_texture.GetWidth(); }
  u32 GetHeight() const override { return m_texture.GetHeight(); }
  u32 GetLayers() const override { return 1; }
  u32 GetLevels() const override { return 1; }
  u32 GetSamples() const override { return m_texture.GetSamples(); }

  HostDisplayPixelFormat GetFormat() const override
  {
    for (u32 i = 0; i < static_cast<u32>(s_display_pixel_format_mapping.size()); i++)
    {
      if (m_texture.GetFormat() == s_display_pixel_format_mapping[i])
        return static_cast<HostDisplayPixelFormat>(i);
    }

    return HostDisplayPixelFormat::Count;
  }

  const D3D12::Texture& GetTexture() const { return m_texture; }
  D3D12::Texture& GetTexture() { return m_texture; }

private:
  D3D12::Texture m_texture;
};

D3D12HostDisplay::D3D12HostDisplay() = default;

D3D12HostDisplay::~D3D12HostDisplay()
{
  AssertMsg(!g_d3d12_context, "Context should have been destroyed by now");
  AssertMsg(!m_swap_chain, "Swap chain should have been destroyed by now");
}

HostDisplay::RenderAPI D3D12HostDisplay::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::D3D12;
}

void* D3D12HostDisplay::GetRenderDevice() const
{
  return g_d3d12_context->GetDevice();
}

void* D3D12HostDisplay::GetRenderContext() const
{
  return g_d3d12_context.get();
}

bool D3D12HostDisplay::HasRenderDevice() const
{
  return static_cast<bool>(g_d3d12_context);
}

bool D3D12HostDisplay::HasRenderSurface() const
{
  return static_cast<bool>(m_swap_chain);
}

std::unique_ptr<HostDisplayTexture> D3D12HostDisplay::CreateTexture(u32 width, u32 height, u32 layers, u32 levels,
                                                                    u32 samples, HostDisplayPixelFormat format,
                                                                    const void* data, u32 data_stride,
                                                                    bool dynamic /* = false */)
{
  if (layers != 1)
    return {};

  const DXGI_FORMAT dxgi_format = s_display_pixel_format_mapping[static_cast<u32>(format)];
  D3D12::Texture tex;
  if (!tex.Create(width, height, samples, dxgi_format, dxgi_format, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                  D3D12_RESOURCE_FLAG_NONE))
  {
    return {};
  }

  if (data && !tex.LoadData(0, 0, width, height, data, data_stride))
    return {};

  return std::make_unique<D3D12HostDisplayTexture>(std::move(tex));
}

void D3D12HostDisplay::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                     const void* texture_data, u32 texture_data_stride)
{
  static_cast<D3D12HostDisplayTexture*>(texture)->GetTexture().LoadData(x, y, width, height, texture_data,
                                                                        texture_data_stride);
}

bool D3D12HostDisplay::DownloadTexture(const void* texture_handle, HostDisplayPixelFormat texture_format, u32 x, u32 y,
                                       u32 width, u32 height, void* out_data, u32 out_data_stride)
{
  const D3D12::Texture* texture = static_cast<const D3D12::Texture*>(texture_handle);

  if (!m_readback_staging_texture.EnsureSize(width, height, texture->GetFormat(), false))
    return false;

  const D3D12_RESOURCE_STATES old_state = texture->GetState();
  texture->TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_readback_staging_texture.CopyFromTexture(texture->GetResource(), 0, x, y, 0, 0, width, height);
  texture->TransitionToState(old_state);

  return m_readback_staging_texture.ReadPixels(0, 0, width, height, out_data, out_data_stride);
}

bool D3D12HostDisplay::SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const
{
  const DXGI_FORMAT dfmt = s_display_pixel_format_mapping[static_cast<u32>(format)];
  if (dfmt == DXGI_FORMAT_UNKNOWN)
    return false;

  return g_d3d12_context->SupportsTextureFormat(dfmt);
}

bool D3D12HostDisplay::BeginSetDisplayPixels(HostDisplayPixelFormat format, u32 width, u32 height, void** out_buffer,
                                             u32* out_pitch)
{
  ClearDisplayTexture();

  const DXGI_FORMAT dxgi_format = s_display_pixel_format_mapping[static_cast<u32>(format)];
  if (m_display_pixels_texture.GetWidth() < width || m_display_pixels_texture.GetHeight() < height ||
      m_display_pixels_texture.GetFormat() != dxgi_format)
  {
    if (!m_display_pixels_texture.Create(width, height, 1, dxgi_format, dxgi_format, DXGI_FORMAT_UNKNOWN,
                                         DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE))
    {
      return false;
    }
  }

  if (!m_display_pixels_texture.BeginStreamUpdate(0, 0, width, height, out_buffer, out_pitch))
    return false;

  SetDisplayTexture(&m_display_pixels_texture, format, m_display_pixels_texture.GetWidth(),
                    m_display_pixels_texture.GetHeight(), 0, 0, static_cast<u32>(width), static_cast<u32>(height));
  return true;
}

void D3D12HostDisplay::EndSetDisplayPixels()
{
  m_display_pixels_texture.EndStreamUpdate(0, 0, static_cast<u32>(m_display_texture_view_width),
                                           static_cast<u32>(m_display_texture_view_height));
}

bool D3D12HostDisplay::GetHostRefreshRate(float* refresh_rate)
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

void D3D12HostDisplay::SetVSync(bool enabled)
{
  m_vsync = enabled;
}

bool D3D12HostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                                          bool threaded_presentation)
{
  ComPtr<IDXGIFactory> temp_dxgi_factory;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(temp_dxgi_factory.GetAddressOf()));
#else
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(temp_dxgi_factory.GetAddressOf()));
#endif

  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create DXGI factory: 0x%08X", hr);
    return false;
  }

  u32 adapter_index;
  if (!adapter_name.empty())
  {
    AdapterAndModeList adapter_info(GetAdapterAndModeList(temp_dxgi_factory.Get()));
    for (adapter_index = 0; adapter_index < static_cast<u32>(adapter_info.adapter_names.size()); adapter_index++)
    {
      if (adapter_name == adapter_info.adapter_names[adapter_index])
        break;
    }
    if (adapter_index == static_cast<u32>(adapter_info.adapter_names.size()))
    {
      Log_WarningPrintf("Could not find adapter '%*s', using first (%s)", static_cast<int>(adapter_name.size()),
                        adapter_name.data(), adapter_info.adapter_names[0].c_str());
      adapter_index = 0;
    }
  }
  else
  {
    Log_InfoPrintf("No adapter selected, using first.");
    adapter_index = 0;
  }

  if (!D3D12::Context::Create(temp_dxgi_factory.Get(), adapter_index, debug_device))
    return false;

  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create D3D device: 0x%08X", hr);
    return false;
  }

  m_dxgi_factory = std::move(temp_dxgi_factory);

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
  return true;
}

bool D3D12HostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                                              bool threaded_presentation)
{
  if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSwapChain(nullptr))
    return false;

  if (!CreateResources())
    return false;

  return true;
}

void D3D12HostDisplay::DestroyRenderDevice()
{
  g_d3d12_context->ExecuteCommandList(true);

  DestroyResources();
  DestroyRenderSurface();
  if (g_d3d12_context)
    g_d3d12_context->Destroy();
}

bool D3D12HostDisplay::MakeRenderContextCurrent()
{
  return true;
}

bool D3D12HostDisplay::DoneRenderContextCurrent()
{
  return true;
}

bool D3D12HostDisplay::CreateSwapChain(const DXGI_MODE_DESC* fullscreen_mode)
{
  HRESULT hr;

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  if (m_window_info.type != WindowInfo::Type::Win32)
    return false;

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
  swap_chain_desc.BufferCount = 3;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.OutputWindow = window_hwnd;
  swap_chain_desc.Windowed = TRUE;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  m_using_allow_tearing = (m_allow_tearing_supported && !fullscreen_mode);
  if (m_using_allow_tearing)
    swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  if (fullscreen_mode)
  {
    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swap_chain_desc.Windowed = FALSE;
    swap_chain_desc.BufferDesc = *fullscreen_mode;
  }

  Log_InfoPrintf("Creating a %dx%d %s swap chain", swap_chain_desc.BufferDesc.Width, swap_chain_desc.BufferDesc.Height,
                 swap_chain_desc.Windowed ? "windowed" : "full-screen");

  hr =
    m_dxgi_factory->CreateSwapChain(g_d3d12_context->GetCommandQueue(), &swap_chain_desc, m_swap_chain.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateSwapChain failed: 0x%08X", hr);
    return false;
  }

  hr = m_dxgi_factory->MakeWindowAssociation(swap_chain_desc.OutputWindow, DXGI_MWA_NO_WINDOW_CHANGES);
  if (FAILED(hr))
    Log_WarningPrintf("MakeWindowAssociation() to disable ALT+ENTER failed");
#else
  if (m_window_info.type != WindowInfo::Type::WinRT)
    return false;

  ComPtr<IDXGIFactory2> factory2;
  hr = m_dxgi_factory.As(&factory2);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to get DXGI factory: %08X", hr);
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
  swap_chain_desc.Width = m_window_info.surface_width;
  swap_chain_desc.Height = m_window_info.surface_height;
  swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferCount = 3;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  m_using_allow_tearing = (m_allow_tearing_supported && !fullscreen_mode);
  if (m_using_allow_tearing)
    swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  ComPtr<IDXGISwapChain1> swap_chain1;
  hr = factory2->CreateSwapChainForCoreWindow(g_d3d12_context->GetCommandQueue(),
                                              static_cast<IUnknown*>(m_window_info.window_handle), &swap_chain_desc,
                                              nullptr, swap_chain1.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateSwapChainForCoreWindow failed: 0x%08X", hr);
    return false;
  }

  m_swap_chain = swap_chain1;
#endif

  return CreateSwapChainRTV();
}

bool D3D12HostDisplay::CreateSwapChainRTV()
{
  DXGI_SWAP_CHAIN_DESC swap_chain_desc;
  HRESULT hr = m_swap_chain->GetDesc(&swap_chain_desc);
  if (FAILED(hr))
    return false;

  for (u32 i = 0; i < swap_chain_desc.BufferCount; i++)
  {
    ComPtr<ID3D12Resource> backbuffer;
    hr = m_swap_chain->GetBuffer(i, IID_PPV_ARGS(backbuffer.GetAddressOf()));
    if (FAILED(hr))
    {
      Log_ErrorPrintf("GetBuffer for RTV failed: 0x%08X", hr);
      return false;
    }

    D3D12::Texture tex;
    if (!tex.Adopt(std::move(backbuffer), DXGI_FORMAT_UNKNOWN, swap_chain_desc.BufferDesc.Format, DXGI_FORMAT_UNKNOWN,
                   D3D12_RESOURCE_STATE_PRESENT))
    {
      return false;
    }

    m_swap_chain_buffers.push_back(std::move(tex));
  }

  m_window_info.surface_width = swap_chain_desc.BufferDesc.Width;
  m_window_info.surface_height = swap_chain_desc.BufferDesc.Height;
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

  m_current_swap_chain_buffer = 0;
  return true;
}

void D3D12HostDisplay::DestroySwapChainRTVs()
{
  for (D3D12::Texture& buffer : m_swap_chain_buffers)
    buffer.Destroy(false);
  m_swap_chain_buffers.clear();
  m_current_swap_chain_buffer = 0;
}

bool D3D12HostDisplay::ChangeRenderWindow(const WindowInfo& new_wi)
{
  DestroyRenderSurface();

  m_window_info = new_wi;
  return CreateSwapChain(nullptr);
}

void D3D12HostDisplay::DestroyRenderSurface()
{
  // For some reason if we don't execute the command list here, the swap chain is in use.. not sure where.
  g_d3d12_context->ExecuteCommandList(true);

  if (IsFullscreen())
    SetFullscreen(false, 0, 0, 0.0f);

  DestroySwapChainRTVs();
  m_swap_chain.Reset();
}

void D3D12HostDisplay::ResizeRenderWindow(s32 new_window_width, s32 new_window_height)
{
  if (!m_swap_chain)
    return;

  // For some reason if we don't execute the command list here, the swap chain is in use.. not sure where.
  g_d3d12_context->ExecuteCommandList(true);

  DestroySwapChainRTVs();

  HRESULT hr = m_swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                           m_using_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr))
    Log_ErrorPrintf("ResizeBuffers() failed: 0x%08X", hr);

  if (!CreateSwapChainRTV())
    Panic("Failed to recreate swap chain RTV after resize");
}

bool D3D12HostDisplay::SupportsFullscreen() const
{
  return true;
}

bool D3D12HostDisplay::IsFullscreen()
{
  BOOL is_fullscreen = FALSE;
  return (m_swap_chain && SUCCEEDED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) && is_fullscreen);
}

bool D3D12HostDisplay::SetFullscreen(bool fullscreen, u32 width, u32 height, float refresh_rate)
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

  if (new_mode.Width == current_desc.BufferDesc.Width && new_mode.Height == current_desc.BufferDesc.Width &&
      new_mode.RefreshRate.Numerator == current_desc.BufferDesc.RefreshRate.Numerator &&
      new_mode.RefreshRate.Denominator == current_desc.BufferDesc.RefreshRate.Denominator)
  {
    Log_InfoPrintf("Fullscreen mode already set");
    return true;
  }

  g_d3d12_context->ExecuteCommandList(true);
  DestroySwapChainRTVs();
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

HostDisplay::AdapterAndModeList D3D12HostDisplay::GetAdapterAndModeList()
{
  return GetAdapterAndModeList(m_dxgi_factory.Get());
}

bool D3D12HostDisplay::CreateResources()
{
  D3D12::RootSignatureBuilder rsbuilder;
  rsbuilder.AddCBVParameter(0, D3D12_SHADER_VISIBILITY_ALL);
  rsbuilder.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_ALL);
  rsbuilder.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 1, D3D12_SHADER_VISIBILITY_ALL);
  m_display_root_signature = rsbuilder.Create();
  if (!m_display_root_signature)
    return false;

  D3D12::GraphicsPipelineBuilder gpbuilder;
  gpbuilder.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
  gpbuilder.SetRootSignature(m_display_root_signature.Get());
  gpbuilder.SetVertexShader(s_display_vs_bytecode, sizeof(s_display_vs_bytecode));
  gpbuilder.SetPixelShader(s_display_ps_bytecode, sizeof(s_display_ps_bytecode));
  gpbuilder.SetNoCullRasterizationState();
  gpbuilder.SetNoDepthTestState();
  gpbuilder.SetNoBlendingState();
  gpbuilder.SetRenderTarget(0, DXGI_FORMAT_R8G8B8A8_UNORM);
  m_display_pipeline = gpbuilder.Create(g_d3d12_context->GetDevice(), false);
  if (!m_display_pipeline)
    return false;

  gpbuilder.SetBlendState(0, true, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD,
                          D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_COLOR_WRITE_ENABLE_ALL);
  m_software_cursor_pipeline = gpbuilder.Create(g_d3d12_context->GetDevice(), false);
  if (!m_software_cursor_pipeline)
    return false;

  D3D12_SAMPLER_DESC desc = {};
  D3D12::SetDefaultSampler(&desc);
  desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;

  if (!g_d3d12_context->GetSamplerHeapManager().Allocate(&m_point_sampler))
    return false;

  g_d3d12_context->GetDevice()->CreateSampler(&desc, m_point_sampler.cpu_handle);

  desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;

  if (!g_d3d12_context->GetSamplerHeapManager().Allocate(&m_linear_sampler))
    return false;

  g_d3d12_context->GetDevice()->CreateSampler(&desc, m_linear_sampler.cpu_handle);

  if (!m_display_uniform_buffer.Create(DISPLAY_UNIFORM_BUFFER_SIZE))
    return false;

  return true;
}

void D3D12HostDisplay::DestroyResources()
{
  //   m_post_processing_chain.ClearStages();
  //   m_post_processing_input_texture.Destroy();
  //   m_post_processing_stages.clear();

  m_readback_staging_texture.Destroy(false);
  m_display_uniform_buffer.Destroy(false);
  g_d3d12_context->GetSamplerHeapManager().Free(&m_linear_sampler);
  g_d3d12_context->GetSamplerHeapManager().Free(&m_point_sampler);
  m_software_cursor_pipeline.Reset();
  m_display_pipeline.Reset();
  m_display_root_signature.Reset();
}

bool D3D12HostDisplay::CreateImGuiContext()
{
  ImGui::GetIO().DisplaySize.x = static_cast<float>(m_window_info.surface_width);
  ImGui::GetIO().DisplaySize.y = static_cast<float>(m_window_info.surface_height);

  return ImGui_ImplDX12_Init(g_d3d12_context->GetDevice(), D3D12::Context::NUM_COMMAND_LISTS,
                             DXGI_FORMAT_R8G8B8A8_UNORM);
}

void D3D12HostDisplay::DestroyImGuiContext()
{
  g_d3d12_context->WaitForGPUIdle();

  ImGui_ImplDX12_Shutdown();
}

bool D3D12HostDisplay::UpdateImGuiFontTexture()
{
  return ImGui_ImplDX12_CreateFontsTexture();
}

bool D3D12HostDisplay::Render()
{
  if (ShouldSkipDisplayingFrame() || !m_swap_chain)
  {
    if (ImGui::GetCurrentContext())
      ImGui::Render();

    return false;
  }

  static constexpr std::array<float, 4> clear_color = {};
  D3D12::Texture& swap_chain_buf = m_swap_chain_buffers[m_current_swap_chain_buffer];
  m_current_swap_chain_buffer = ((m_current_swap_chain_buffer + 1) % static_cast<u32>(m_swap_chain_buffers.size()));

  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  swap_chain_buf.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmdlist->ClearRenderTargetView(swap_chain_buf.GetRTVOrDSVDescriptor(), clear_color.data(), 0, nullptr);
  cmdlist->OMSetRenderTargets(1, &swap_chain_buf.GetRTVOrDSVDescriptor().cpu_handle, FALSE, nullptr);

  RenderDisplay(cmdlist);

  if (ImGui::GetCurrentContext())
    RenderImGui(cmdlist);

  RenderSoftwareCursor(cmdlist);

  swap_chain_buf.TransitionToState(D3D12_RESOURCE_STATE_PRESENT);
  g_d3d12_context->ExecuteCommandList(false);

  if (!m_vsync && m_using_allow_tearing)
    m_swap_chain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
  else
    m_swap_chain->Present(BoolToUInt32(m_vsync), 0);

  return true;
}

bool D3D12HostDisplay::RenderScreenshot(u32 width, u32 height, std::vector<u32>* out_pixels, u32* out_stride,
                                        HostDisplayPixelFormat* out_format)
{
  static constexpr DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  static constexpr HostDisplayPixelFormat hdformat = HostDisplayPixelFormat::RGBA8;

  D3D12::Texture render_texture;
  if (!render_texture.Create(width, height, 1, format, DXGI_FORMAT_UNKNOWN, format, DXGI_FORMAT_UNKNOWN,
                             D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
      !m_readback_staging_texture.EnsureSize(width, height, format, false))
  {
    return false;
  }

  static constexpr std::array<float, 4> clear_color = {};
  ID3D12GraphicsCommandList* cmdlist = g_d3d12_context->GetCommandList();
  render_texture.TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmdlist->ClearRenderTargetView(render_texture.GetRTVOrDSVDescriptor(), clear_color.data(), 0, nullptr);
  cmdlist->OMSetRenderTargets(1, &render_texture.GetRTVOrDSVDescriptor().cpu_handle, FALSE, nullptr);

  if (HasDisplayTexture())
  {
    const auto [left, top, draw_width, draw_height] = CalculateDrawRect(width, height, 0);
    RenderDisplay(cmdlist, left, top, draw_width, draw_height, m_display_texture_handle, m_display_texture_width,
                  m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                  m_display_texture_view_width, m_display_texture_view_height, m_display_linear_filtering);
  }

  cmdlist->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

  render_texture.TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  m_readback_staging_texture.CopyFromTexture(render_texture, 0, 0, 0, 0, 0, width, height);

  const u32 stride = sizeof(u32) * width;
  out_pixels->resize(width * height);
  *out_stride = stride;
  *out_format = hdformat;

  return m_readback_staging_texture.ReadPixels(0, 0, width, height, out_pixels->data(), stride);
}

void D3D12HostDisplay::RenderImGui(ID3D12GraphicsCommandList* cmdlist)
{
  ImGui::Render();
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdlist);
}

void D3D12HostDisplay::RenderDisplay(ID3D12GraphicsCommandList* cmdlist)
{
  if (!HasDisplayTexture())
    return;

  const auto [left, top, width, height] = CalculateDrawRect(GetWindowWidth(), GetWindowHeight(), m_display_top_margin);

  //   if (!m_post_processing_chain.IsEmpty())
  //   {
  //     ApplyPostProcessingChain(m_swap_chain_rtv.Get(), left, top, width, height, m_display_texture_handle,
  //                              m_display_texture_width, m_display_texture_height, m_display_texture_view_x,
  //                              m_display_texture_view_y, m_display_texture_view_width,
  //                              m_display_texture_view_height);
  //     return;
  //   }

  RenderDisplay(cmdlist, left, top, width, height, m_display_texture_handle, m_display_texture_width,
                m_display_texture_height, m_display_texture_view_x, m_display_texture_view_y,
                m_display_texture_view_width, m_display_texture_view_height, m_display_linear_filtering);
}

void D3D12HostDisplay::RenderDisplay(ID3D12GraphicsCommandList* cmdlist, s32 left, s32 top, s32 width, s32 height,
                                     void* texture_handle, u32 texture_width, s32 texture_height, s32 texture_view_x,
                                     s32 texture_view_y, s32 texture_view_width, s32 texture_view_height,
                                     bool linear_filter)
{
  const float uniforms[4] = {static_cast<float>(texture_view_x) / static_cast<float>(texture_width),
                             static_cast<float>(texture_view_y) / static_cast<float>(texture_height),
                             (static_cast<float>(texture_view_width) - 0.5f) / static_cast<float>(texture_width),
                             (static_cast<float>(texture_view_height) - 0.5f) / static_cast<float>(texture_height)};
  if (!m_display_uniform_buffer.ReserveMemory(sizeof(uniforms), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
    Panic("Failed to reserve UBO space");

  const u32 ubo_offset = m_display_uniform_buffer.GetCurrentOffset();
  std::memcpy(m_display_uniform_buffer.GetCurrentHostPointer(), uniforms, sizeof(uniforms));
  m_display_uniform_buffer.CommitMemory(sizeof(uniforms));

  cmdlist->SetGraphicsRootSignature(m_display_root_signature.Get());
  cmdlist->SetPipelineState(m_display_pipeline.Get());
  cmdlist->SetGraphicsRootConstantBufferView(0, m_display_uniform_buffer.GetGPUPointer() + ubo_offset);
  cmdlist->SetGraphicsRootDescriptorTable(1, reinterpret_cast<D3D12::Texture*>(texture_handle)->GetSRVDescriptor());
  cmdlist->SetGraphicsRootDescriptorTable(2, linear_filter ? m_linear_sampler : m_point_sampler);

  D3D12::SetViewportAndScissor(cmdlist, left, top, width, height);

  cmdlist->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmdlist->DrawInstanced(3, 1, 0, 0);
}

void D3D12HostDisplay::RenderSoftwareCursor(ID3D12GraphicsCommandList* cmdlist)
{
  if (!HasSoftwareCursor())
    return;

  const auto [left, top, width, height] = CalculateSoftwareCursorDrawRect();
  RenderSoftwareCursor(cmdlist, left, top, width, height, m_cursor_texture.get());
}

void D3D12HostDisplay::RenderSoftwareCursor(ID3D12GraphicsCommandList* cmdlist, s32 left, s32 top, s32 width,
                                            s32 height, HostDisplayTexture* texture_handle)
{
  const float uniforms[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  if (!m_display_uniform_buffer.ReserveMemory(sizeof(uniforms), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
    Panic("Failed to reserve UBO space");

  const u32 ubo_offset = m_display_uniform_buffer.GetCurrentOffset();
  std::memcpy(m_display_uniform_buffer.GetCurrentHostPointer(), uniforms, sizeof(uniforms));
  m_display_uniform_buffer.CommitMemory(sizeof(uniforms));

  cmdlist->SetPipelineState(m_display_pipeline.Get());
  cmdlist->SetGraphicsRootConstantBufferView(0, m_display_uniform_buffer.GetGPUPointer() + ubo_offset);
  cmdlist->SetGraphicsRootDescriptorTable(
    1, static_cast<D3D12HostDisplayTexture*>(texture_handle)->GetTexture().GetRTVOrDSVDescriptor());
  cmdlist->SetGraphicsRootDescriptorTable(2, m_linear_sampler);

  D3D12::SetViewportAndScissor(cmdlist, left, top, width, height);

  cmdlist->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmdlist->DrawInstanced(3, 1, 0, 0);
}

HostDisplay::AdapterAndModeList D3D12HostDisplay::StaticGetAdapterAndModeList()
{
  ComPtr<IDXGIFactory> dxgi_factory;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
#else
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
#endif
  if (FAILED(hr))
    return {};

  return GetAdapterAndModeList(dxgi_factory.Get());
}

HostDisplay::AdapterAndModeList D3D12HostDisplay::GetAdapterAndModeList(IDXGIFactory* dxgi_factory)
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
              adapter_info.fullscreen_modes.push_back(StringUtil::StdStringFromFormat(
                "%u x %u @ %f hz", mode.Width, mode.Height,
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

bool D3D12HostDisplay::SetPostProcessingChain(const std::string_view& config)
{
  return false;
}

} // namespace FrontendCommon
