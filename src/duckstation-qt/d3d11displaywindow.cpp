#include "d3d11displaywindow.h"
#include "common/assert.h"
#include "common/d3d11/shader_compiler.h"
#include "common/log.h"
#include <array>
#include <dxgi1_5.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
Log_SetChannel(D3D11DisplayWindow);

class D3D11DisplayWindowTexture : public HostDisplayTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11DisplayWindowTexture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv, u32 width,
                            u32 height, bool dynamic)
    : m_texture(std::move(texture)), m_srv(std::move(srv)), m_width(width), m_height(height), m_dynamic(dynamic)
  {
  }
  ~D3D11DisplayWindowTexture() override = default;

  void* GetHandle() const override { return m_srv.Get(); }
  u32 GetWidth() const override { return m_width; }
  u32 GetHeight() const override { return m_height; }

  ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }
  ID3D11ShaderResourceView* GetD3DSRV() const { return m_srv.Get(); }
  bool IsDynamic() const { return m_dynamic; }

  static std::unique_ptr<D3D11DisplayWindowTexture> Create(ID3D11Device* device, u32 width, u32 height,
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

    return std::make_unique<D3D11DisplayWindowTexture>(std::move(texture), std::move(srv), width, height, dynamic);
  }

private:
  ComPtr<ID3D11Texture2D> m_texture;
  ComPtr<ID3D11ShaderResourceView> m_srv;
  u32 m_width;
  u32 m_height;
  bool m_dynamic;
};

D3D11DisplayWindow::D3D11DisplayWindow(QtHostInterface* host_interface, QWindow* parent)
  : QtDisplayWindow(host_interface, parent)
{
}

D3D11DisplayWindow::~D3D11DisplayWindow() = default;

HostDisplay* D3D11DisplayWindow::getHostDisplayInterface()
{
  return this;
}

HostDisplay::RenderAPI D3D11DisplayWindow::GetRenderAPI() const
{
  return HostDisplay::RenderAPI::D3D11;
}

void* D3D11DisplayWindow::GetRenderDevice() const
{
  return m_device.Get();
}

void* D3D11DisplayWindow::GetRenderContext() const
{
  return m_context.Get();
}

void* D3D11DisplayWindow::GetRenderWindow() const
{
  return const_cast<QWindow*>(static_cast<const QWindow*>(this));
}

void D3D11DisplayWindow::ChangeRenderWindow(void* new_window)
{
  Panic("Not supported");
}

std::unique_ptr<HostDisplayTexture> D3D11DisplayWindow::CreateTexture(u32 width, u32 height, const void* data,
                                                                      u32 data_stride, bool dynamic)
{
  return D3D11DisplayWindowTexture::Create(m_device.Get(), width, height, data, data_stride, dynamic);
}

void D3D11DisplayWindow::UpdateTexture(HostDisplayTexture* texture, u32 x, u32 y, u32 width, u32 height,
                                       const void* data, u32 data_stride)
{
  D3D11DisplayWindowTexture* d3d11_texture = static_cast<D3D11DisplayWindowTexture*>(texture);
  if (!d3d11_texture->IsDynamic())
  {
    const CD3D11_BOX dst_box(x, y, 0, x + width, y + height, 1);
    m_context->UpdateSubresource(d3d11_texture->GetD3DTexture(), 0, &dst_box, data, data_stride, data_stride * height);
  }
  else
  {
    D3D11_MAPPED_SUBRESOURCE sr;
    HRESULT hr = m_context->Map(d3d11_texture->GetD3DTexture(), 0, D3D11_MAP_WRITE_DISCARD, 0, &sr);
    if (FAILED(hr))
      Panic("Failed to map dynamic host display texture");

    char* dst_ptr = static_cast<char*>(sr.pData) + (y * sr.RowPitch) + (x * sizeof(u32));
    const char* src_ptr = static_cast<const char*>(data);
    if (sr.RowPitch == data_stride)
    {
      std::memcpy(dst_ptr, src_ptr, data_stride * height);
    }
    else
    {
      for (u32 row = 0; row < height; row++)
      {
        std::memcpy(dst_ptr, src_ptr, width * sizeof(u32));
        src_ptr += data_stride;
        dst_ptr += sr.RowPitch;
      }
    }

    m_context->Unmap(d3d11_texture->GetD3DTexture(), 0);
  }
}

void D3D11DisplayWindow::SetDisplayTexture(void* texture, s32 offset_x, s32 offset_y, s32 width, s32 height,
                                           u32 texture_width, u32 texture_height, float aspect_ratio)
{
  m_display_srv = static_cast<ID3D11ShaderResourceView*>(texture);
  m_display_offset_x = offset_x;
  m_display_offset_y = offset_y;
  m_display_width = width;
  m_display_height = height;
  m_display_texture_width = texture_width;
  m_display_texture_height = texture_height;
  m_display_aspect_ratio = aspect_ratio;
  m_display_texture_changed = true;
}

void D3D11DisplayWindow::SetDisplayLinearFiltering(bool enabled)
{
  m_display_linear_filtering = enabled;
}

void D3D11DisplayWindow::SetDisplayTopMargin(int height)
{
  m_display_top_margin = height;
}

void D3D11DisplayWindow::SetVSync(bool enabled)
{
  m_vsync = enabled;
}

std::tuple<u32, u32> D3D11DisplayWindow::GetWindowSize() const
{
  const QSize s = size();
  return std::make_tuple(static_cast<u32>(s.width()), static_cast<u32>(s.height()));
}

void D3D11DisplayWindow::WindowResized() {}

void D3D11DisplayWindow::onWindowResized(int width, int height)
{
  QtDisplayWindow::onWindowResized(width, height);

  if (!m_swap_chain)
    return;

  m_swap_chain_rtv.Reset();

  HRESULT hr = m_swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr))
    Log_ErrorPrintf("ResizeBuffers() failed: 0x%08X", hr);

  if (!createSwapChainRTV())
    Panic("Failed to recreate swap chain RTV after resize");
}

bool D3D11DisplayWindow::createDeviceContext(QThread* worker_thread)
{
  const bool debug = false;

  ComPtr<IDXGIFactory> dxgi_factory;
  HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create DXGI factory: 0x%08X", hr);
    return false;
  }

  m_allow_tearing_supported = false;
  ComPtr<IDXGIFactory5> dxgi_factory5;
  hr = dxgi_factory.As(&dxgi_factory5);
  if (SUCCEEDED(hr))
  {
    BOOL allow_tearing_supported = false;
    hr = dxgi_factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing_supported,
                                            sizeof(allow_tearing_supported));
    if (SUCCEEDED(hr))
      m_allow_tearing_supported = (allow_tearing_supported == TRUE);
  }

  UINT create_flags = 0;
  if (debug)
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;

  hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags, nullptr, 0, D3D11_SDK_VERSION,
                         m_device.GetAddressOf(), nullptr, m_context.GetAddressOf());

  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create D3D device: 0x%08X", hr);
    return false;
  }

  DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
  swap_chain_desc.BufferDesc.Width = m_window_width;
  swap_chain_desc.BufferDesc.Height = m_window_height;
  swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferCount = 3;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.OutputWindow = reinterpret_cast<HWND>(winId());
  swap_chain_desc.Windowed = TRUE;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  if (m_allow_tearing_supported)
    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  hr = dxgi_factory->CreateSwapChain(m_device.Get(), &swap_chain_desc, m_swap_chain.GetAddressOf());
  if (FAILED(hr))
  {
    Log_WarningPrintf("Failed to create a flip-discard swap chain, trying discard.");
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swap_chain_desc.Flags = 0;
    m_allow_tearing_supported = false;

    hr = dxgi_factory->CreateSwapChain(m_device.Get(), &swap_chain_desc, m_swap_chain.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("CreateSwapChain failed: 0x%08X", hr);
      return false;
    }
  }

  if (debug)
  {
    ComPtr<ID3D11InfoQueue> info;
    hr = m_device.As(&info);
    if (SUCCEEDED(hr))
    {
      info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
      info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, TRUE);
    }
  }

  if (!QtDisplayWindow::createDeviceContext(worker_thread))
  {
    m_swap_chain.Reset();
    m_context.Reset();
    m_device.Reset();
  }

  return true;
}

bool D3D11DisplayWindow::initializeDeviceContext()
{
  if (!createSwapChainRTV())
    return false;

  if (!QtDisplayWindow::initializeDeviceContext())
    return false;

  return true;
}

void D3D11DisplayWindow::destroyDeviceContext()
{
  QtDisplayWindow::destroyDeviceContext();
  m_swap_chain.Reset();
  m_context.Reset();
  m_device.Reset();
}

bool D3D11DisplayWindow::createSwapChainRTV()
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

bool D3D11DisplayWindow::createDeviceResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
cbuffer UBOBlock : register(b0)
{
  float4 u_src_rect;
};

void main(in uint vertex_id : SV_VertexID,
          out float2 v_tex0 : TEXCOORD0,
          out float4 o_pos : SV_Position)
{
  float2 pos = float2(float((vertex_id << 1) & 2u), float(vertex_id & 2u));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  o_pos = float4(pos * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

  static constexpr char display_pixel_shader[] = R"(
Texture2D samp0 : register(t0);
SamplerState samp0_ss : register(s0);

void main(in float2 v_tex0 : TEXCOORD0,
          out float4 o_col0 : SV_Target)
{
  o_col0 = samp0.Sample(samp0_ss, v_tex0);
}
)";

  HRESULT hr;

  m_display_vertex_shader =
    D3D11::ShaderCompiler::CompileAndCreateVertexShader(m_device.Get(), fullscreen_quad_vertex_shader, false);
  m_display_pixel_shader =
    D3D11::ShaderCompiler::CompileAndCreatePixelShader(m_device.Get(), display_pixel_shader, false);
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

void D3D11DisplayWindow::destroyDeviceResources()
{
  QtDisplayWindow::destroyDeviceResources();

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

bool D3D11DisplayWindow::createImGuiContext()
{
  if (!QtDisplayWindow::createImGuiContext())
    return false;

  if (!ImGui_ImplDX11_Init(m_device.Get(), m_context.Get()))
    return false;

  ImGui_ImplDX11_NewFrame();
  ImGui::NewFrame();
  return true;
}

void D3D11DisplayWindow::destroyImGuiContext()
{
  ImGui_ImplDX11_Shutdown();
  QtDisplayWindow::destroyImGuiContext();
}

void D3D11DisplayWindow::Render()
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

void D3D11DisplayWindow::renderDisplay()
{
  if (!m_display_srv)
    return;

  // - 20 for main menu padding
  auto [vp_left, vp_top, vp_width, vp_height] =
    CalculateDrawRect(m_window_width, std::max(m_window_height - m_display_top_margin, 1), m_display_aspect_ratio);
  vp_top += m_display_top_margin;

  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_context->VSSetShader(m_display_vertex_shader.Get(), nullptr, 0);
  m_context->PSSetShader(m_display_pixel_shader.Get(), nullptr, 0);
  m_context->PSSetShaderResources(0, 1, &m_display_srv);
  m_context->PSSetSamplers(
    0, 1, m_display_linear_filtering ? m_linear_sampler.GetAddressOf() : m_point_sampler.GetAddressOf());

  const float uniforms[4] = {static_cast<float>(m_display_offset_x) / static_cast<float>(m_display_texture_width),
                             static_cast<float>(m_display_offset_y) / static_cast<float>(m_display_texture_height),
                             static_cast<float>(m_display_width) / static_cast<float>(m_display_texture_width),
                             static_cast<float>(m_display_height) / static_cast<float>(m_display_texture_height)};
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
