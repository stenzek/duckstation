#include "d3d11displaywidget.h"
#include "common/assert.h"
#include "common/d3d11/shader_compiler.h"
#include "common/log.h"
#include "frontend-common/display_ps.hlsl.h"
#include "frontend-common/display_vs.hlsl.h"
#include <array>
#include <dxgi1_5.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
Log_SetChannel(D3D11DisplayWidget);

class D3D11DisplayWidgetTexture : public HostDisplayTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11DisplayWidgetTexture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv, u32 width,
                            u32 height, bool dynamic)
    : m_texture(std::move(texture)), m_srv(std::move(srv)), m_width(width), m_height(height), m_dynamic(dynamic)
  {
  }
  ~D3D11DisplayWidgetTexture() override = default;

  void* GetHandle() const override { return m_srv.Get(); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }
  ID3D11ShaderResourceView* GetD3DSRV() const { return m_srv.Get(); }
  bool IsDynamic() const { return m_dynamic; }

  static std::unique_ptr<D3D11DisplayWidgetTexture> Create(ID3D11Device* device, u32 width, u32 height,
                                                           const void* data, u32 data_stride, bool dynamic)
  {
    const CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1, D3D11_BIND_SHADER_RESOURCE,
                                     dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT,
                                     dynamic ? D3D11_CPU_ACCESS_WRITE : 0, 1, 0, 0);
    const D3D11_SUBRESOURCE_DATA srd{data, data_stride, data_stride * height};
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&desc, data ? &srd : nullptr, texture.GetAddressOf());
    if (FAILED(hr))
      return {};

    const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(D3D11_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0,
                                                    1);
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, srv.GetAddressOf());
    if (FAILED(hr))
      return {};

    return std::make_unique<D3D11DisplayWidgetTexture>(std::move(texture), std::move(srv), width, height, dynamic);
  }

private:
  ComPtr<ID3D11Texture2D> m_texture;
  ComPtr<ID3D11ShaderResourceView> m_srv;
  u32 m_width;
  u32 m_height;
  bool m_dynamic;
};

D3D11DisplayWidget::D3D11DisplayWidget(QtHostInterface* host_interface, QWidget* parent)
  : QtDisplayWidget(host_interface, parent)
{
}

D3D11DisplayWidget::~D3D11DisplayWidget() = default;

HostDisplay* D3D11DisplayWidget::getHostDisplayInterface()
{
  return this;
}

HostDisplay::RenderAPI D3D11DisplayWidget::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::D3D11;
}

void* D3D11DisplayWidget::GetRenderDevice() const
{
  return m_device.Get();
}

void* D3D11DisplayWidget::GetRenderContext() const
{
  return m_context.Get();
}

void* D3D11DisplayWidget::GetRenderWindow() const
{
  return const_cast<QWidget*>(static_cast<const QWidget*>(this));
}

void D3D11DisplayWidget::ChangeRenderWindow(void* new_window)
{
  Panic("Not supported");
}

std::unique_ptr<HostDisplayTexture> D3D11DisplayWidget::CreateTexture(u32 width, u32 height, const void* initial_data,
                                                                      u32 initial_data_stride, bool dynamic)
{
  return D3D11DisplayWidgetTexture::Create(m_device.Get(), width, height, initial_data, initial_data_stride, dynamic);
}

void D3D11DisplayWidget::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                       const void* texture_data, u32 texture_data_stride)
{
  D3D11DisplayWidgetTexture* d3d11_texture = static_cast<D3D11DisplayWidgetTexture*>(texture);
  if (!d3d11_texture->IsDynamic())
  {
    const CD3D11_BOX dst_box(x, y, 0, x + width, y + height, 1);
    m_context->UpdateSubresource(d3d11_texture->GetD3DTexture(), 0, &dst_box, texture_data, texture_data_stride,
                                 texture_data_stride * height);
  }
  else
  {
    D3D11_MAPPED_SUBRESOURCE sr;
    HRESULT hr = m_context->Map(d3d11_texture->GetD3DTexture(), 0, D3D11_MAP_WRITE_DISCARD, 0, &sr);
    if (FAILED(hr))
      Panic("Failed to map dynamic host display texture");

    char* dst_ptr = static_cast<char*>(sr.pData) + (y * sr.RowPitch) + (x * sizeof(u32));
    const char* src_ptr = static_cast<const char*>(texture_data);
    if (sr.RowPitch == texture_data_stride)
    {
      std::memcpy(dst_ptr, src_ptr, texture_data_stride * height);
    }
    else
    {
      for (u32 row = 0; row < height; row++)
      {
        std::memcpy(dst_ptr, src_ptr, width * sizeof(u32));
        src_ptr += texture_data_stride;
        dst_ptr += sr.RowPitch;
      }
    }

    m_context->Unmap(d3d11_texture->GetD3DTexture(), 0);
  }
}

void D3D11DisplayWidget::SetVSync(bool enabled)
{
  m_vsync = enabled;
}

bool D3D11DisplayWidget::hasDeviceContext() const
{
  return static_cast<bool>(m_device);
}

bool D3D11DisplayWidget::createDeviceContext(QThread* worker_thread, bool debug_device)
{
  UINT create_flags = 0;
  if (debug_device)
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags, nullptr, 0,
                                 D3D11_SDK_VERSION, m_device.GetAddressOf(), nullptr, m_context.GetAddressOf());

  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create D3D device: 0x%08X", hr);
    return false;
  }

  if (debug_device)
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
  ComPtr<IDXGIAdapter> dxgi_adapter;
  if (FAILED(m_device.As(&dxgi_device)) || FAILED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.GetAddressOf()))) ||
      FAILED(dxgi_adapter->GetParent(IID_PPV_ARGS(m_dxgi_factory.GetAddressOf()))))
  {
    Log_WarningPrint("Failed to get parent adapter/device/factory");
    return false;
  }

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

  if (!createSwapChain(reinterpret_cast<HWND>(winId())))
    return false;

  if (!QtDisplayWidget::createDeviceContext(worker_thread, debug_device))
  {
    m_swap_chain.Reset();
    m_context.Reset();
    m_device.Reset();
  }

  return true;
}

bool D3D11DisplayWidget::initializeDeviceContext(bool debug_device)
{
  if (!createSwapChainRTV())
    return false;

  if (!QtDisplayWidget::initializeDeviceContext(debug_device))
    return false;

  return true;
}

void D3D11DisplayWidget::destroyDeviceContext()
{
  QtDisplayWidget::destroyDeviceContext();
  m_swap_chain.Reset();
  m_context.Reset();
  m_device.Reset();
}

bool D3D11DisplayWidget::createSwapChain(HWND hwnd)
{
  HRESULT hr;

  DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
  swap_chain_desc.BufferDesc.Width = m_window_width;
  swap_chain_desc.BufferDesc.Height = m_window_height;
  swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferCount = 3;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.OutputWindow = hwnd;
  swap_chain_desc.Windowed = TRUE;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  if (m_allow_tearing_supported)
    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  hr = m_dxgi_factory->CreateSwapChain(m_device.Get(), &swap_chain_desc, m_swap_chain.GetAddressOf());
  if (FAILED(hr))
  {
    Log_WarningPrintf("Failed to create a flip-discard swap chain, trying discard.");
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swap_chain_desc.Flags = 0;
    m_allow_tearing_supported = false;

    hr = m_dxgi_factory->CreateSwapChain(m_device.Get(), &swap_chain_desc, m_swap_chain.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("CreateSwapChain failed: 0x%08X", hr);
      return false;
    }
  }

  hr = m_dxgi_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES);
  if (FAILED(hr))
    Log_WarningPrintf("MakeWindowAssociation() to disable ALT+ENTER failed");

  return true;
}

void D3D11DisplayWidget::windowResized(s32 new_window_width, s32 new_window_height)
{
  QtDisplayWidget::windowResized(new_window_width, new_window_height);
  HostDisplay::WindowResized(new_window_width, new_window_height);

  if (!m_swap_chain)
    return;

  m_swap_chain_rtv.Reset();

  HRESULT hr = m_swap_chain->ResizeBuffers(0, new_window_width, new_window_height, DXGI_FORMAT_UNKNOWN,
                                           m_allow_tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr))
    Log_ErrorPrintf("ResizeBuffers() failed: 0x%08X", hr);

  if (!createSwapChainRTV())
    Panic("Failed to recreate swap chain RTV after resize");
}

bool D3D11DisplayWidget::createSwapChainRTV()
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

  return true;
}

bool D3D11DisplayWidget::createDeviceResources()
{
  HRESULT hr;

  m_display_vertex_shader =
    D3D11::ShaderCompiler::CreateVertexShader(m_device.Get(), s_display_vs_bytecode, sizeof(s_display_vs_bytecode));
  m_display_pixel_shader =
    D3D11::ShaderCompiler::CreatePixelShader(m_device.Get(), s_display_ps_bytecode, sizeof(s_display_ps_bytecode));
  if (!m_display_vertex_shader || !m_display_pixel_shader)
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

  CD3D11_SAMPLER_DESC sampler_desc = CD3D11_SAMPLER_DESC(CD3D11_DEFAULT());
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  hr = m_device->CreateSamplerState(&sampler_desc, m_point_sampler.GetAddressOf());
  if (FAILED(hr))
    return false;

  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  hr = m_device->CreateSamplerState(&sampler_desc, m_linear_sampler.GetAddressOf());
  if (FAILED(hr))
    return false;

  return true;
}

void D3D11DisplayWidget::destroyDeviceResources()
{
  QtDisplayWidget::destroyDeviceResources();

  m_display_uniform_buffer.Release();
  m_swap_chain_rtv.Reset();
  m_linear_sampler.Reset();
  m_point_sampler.Reset();
  m_display_pixel_shader.Reset();
  m_display_vertex_shader.Reset();
  m_display_blend_state.Reset();
  m_display_depth_stencil_state.Reset();
  m_display_rasterizer_state.Reset();
}

bool D3D11DisplayWidget::createImGuiContext()
{
  if (!QtDisplayWidget::createImGuiContext())
    return false;

  if (!ImGui_ImplDX11_Init(m_device.Get(), m_context.Get()))
    return false;

  ImGui_ImplDX11_NewFrame();
  ImGui::NewFrame();
  return true;
}

void D3D11DisplayWidget::destroyImGuiContext()
{
  ImGui_ImplDX11_Shutdown();
  QtDisplayWidget::destroyImGuiContext();
}

void D3D11DisplayWidget::Render()
{
  static constexpr std::array<float, 4> clear_color = {};
  m_context->ClearRenderTargetView(m_swap_chain_rtv.Get(), clear_color.data());
  m_context->OMSetRenderTargets(1, m_swap_chain_rtv.GetAddressOf(), nullptr);

  renderDisplay();

  ImGui::Render();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

  if (!m_vsync && m_allow_tearing_supported)
    m_swap_chain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
  else
    m_swap_chain->Present(BoolToUInt32(m_vsync), 0);

  ImGui::NewFrame();
  ImGui_ImplDX11_NewFrame();
}

void D3D11DisplayWidget::renderDisplay()
{
  if (!m_display_texture_handle)
    return;

  auto [vp_left, vp_top, vp_width, vp_height] = CalculateDrawRect();

  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_context->VSSetShader(m_display_vertex_shader.Get(), nullptr, 0);
  m_context->PSSetShader(m_display_pixel_shader.Get(), nullptr, 0);
  m_context->PSSetShaderResources(0, 1, reinterpret_cast<ID3D11ShaderResourceView**>(&m_display_texture_handle));
  m_context->PSSetSamplers(
    0, 1, m_display_linear_filtering ? m_linear_sampler.GetAddressOf() : m_point_sampler.GetAddressOf());

  const float uniforms[4] = {
    (static_cast<float>(m_display_texture_view_x) + 0.25f) / static_cast<float>(m_display_texture_width),
    (static_cast<float>(m_display_texture_view_y) + 0.25f) / static_cast<float>(m_display_texture_height),
    (static_cast<float>(m_display_texture_view_width) - 0.5f) / static_cast<float>(m_display_texture_width),
    (static_cast<float>(m_display_texture_view_height) - 0.5f) / static_cast<float>(m_display_texture_height)};
  const auto map = m_display_uniform_buffer.Map(m_context.Get(), sizeof(uniforms), sizeof(uniforms));
  std::memcpy(map.pointer, uniforms, sizeof(uniforms));
  m_display_uniform_buffer.Unmap(m_context.Get(), sizeof(uniforms));
  m_context->VSSetConstantBuffers(0, 1, m_display_uniform_buffer.GetD3DBufferArray());

  const CD3D11_VIEWPORT vp(static_cast<float>(vp_left), static_cast<float>(vp_top), static_cast<float>(vp_width),
                           static_cast<float>(vp_height));
  m_context->RSSetViewports(1, &vp);
  m_context->RSSetState(m_display_rasterizer_state.Get());
  m_context->OMSetDepthStencilState(m_display_depth_stencil_state.Get(), 0);
  m_context->OMSetBlendState(m_display_blend_state.Get(), nullptr, 0xFFFFFFFFu);

  m_context->Draw(3, 0);
}
