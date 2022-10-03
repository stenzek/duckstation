#include "texture.h"
#include "../log.h"
#include <array>
Log_SetChannel(D3D11);

static constexpr std::array<DXGI_FORMAT, static_cast<u32>(GPUTexture::Format::Count)> s_dxgi_mapping = {
  {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
   DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_D16_UNORM}};

D3D11::Texture::Texture() = default;

D3D11::Texture::Texture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv,
                        ComPtr<ID3D11RenderTargetView> rtv)
  : m_texture(std::move(texture)), m_srv(std::move(srv)), m_rtv(std::move(rtv))
{
  const D3D11_TEXTURE2D_DESC desc = GetDesc();
  m_width = static_cast<u16>(desc.Width);
  m_height = static_cast<u16>(desc.Height);
  m_layers = static_cast<u8>(desc.ArraySize);
  m_levels = static_cast<u8>(desc.MipLevels);
  m_samples = static_cast<u8>(desc.SampleDesc.Count);
  m_format = LookupBaseFormat(desc.Format);
  m_dynamic = (desc.Usage == D3D11_USAGE_DYNAMIC);
}

D3D11::Texture::~Texture()
{
  Destroy();
}

DXGI_FORMAT D3D11::Texture::GetDXGIFormat(Format format)
{
  return s_dxgi_mapping[static_cast<u8>(format)];
}

GPUTexture::Format D3D11::Texture::LookupBaseFormat(DXGI_FORMAT dformat)
{
  for (u32 i = 0; i < static_cast<u32>(s_dxgi_mapping.size()); i++)
  {
    if (s_dxgi_mapping[i] == dformat)
      return static_cast<Format>(i);
  }
  return GPUTexture::Format::Unknown;
}

D3D11_TEXTURE2D_DESC D3D11::Texture::GetDesc() const
{
  D3D11_TEXTURE2D_DESC desc;
  m_texture->GetDesc(&desc);
  return desc;
}

bool D3D11::Texture::IsValid() const
{
  return static_cast<bool>(m_texture);
}

bool D3D11::Texture::Create(ID3D11Device* device, u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                            Format format, u32 bind_flags, const void* initial_data /* = nullptr */,
                            u32 initial_data_stride /* = 0 */, bool dynamic /* = false */)
{
  if (width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      layers > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION || (layers > 1 && samples > 1))
  {
    Log_ErrorPrintf("Texture bounds (%ux%ux%u, %u mips, %u samples) are too large", width, height, layers, levels,
                    samples);
    return false;
  }

  CD3D11_TEXTURE2D_DESC desc(GetDXGIFormat(format), width, height, layers, levels, bind_flags,
                             dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT, dynamic ? D3D11_CPU_ACCESS_WRITE : 0,
                             samples, 0, 0);

  D3D11_SUBRESOURCE_DATA srd;
  srd.pSysMem = initial_data;
  srd.SysMemPitch = initial_data_stride;
  srd.SysMemSlicePitch = initial_data_stride * height;

  ComPtr<ID3D11Texture2D> texture;
  const HRESULT tex_hr = device->CreateTexture2D(&desc, initial_data ? &srd : nullptr, texture.GetAddressOf());
  if (FAILED(tex_hr))
  {
    Log_ErrorPrintf(
      "Create texture failed: 0x%08X (%ux%u levels:%u samples:%u format:%u bind_flags:%X initial_data:%p)", tex_hr,
      width, height, levels, samples, static_cast<unsigned>(format), bind_flags, initial_data);
    return false;
  }

  ComPtr<ID3D11ShaderResourceView> srv;
  if (bind_flags & D3D11_BIND_SHADER_RESOURCE)
  {
    const D3D11_SRV_DIMENSION srv_dimension =
      (desc.SampleDesc.Count > 1) ?
        D3D11_SRV_DIMENSION_TEXTURE2DMS :
        (desc.ArraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D);
    const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(srv_dimension, desc.Format, 0, desc.MipLevels, 0, desc.ArraySize);
    const HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, srv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create SRV for texture failed: 0x%08X", hr);
      return false;
    }
  }

  ComPtr<ID3D11RenderTargetView> rtv;
  if (bind_flags & D3D11_BIND_RENDER_TARGET)
  {
    const D3D11_RTV_DIMENSION rtv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    const CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(rtv_dimension, desc.Format, 0, 0, desc.ArraySize);
    const HRESULT hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, rtv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create RTV for texture failed: 0x%08X", hr);
      return false;
    }
  }

  m_texture = std::move(texture);
  m_srv = std::move(srv);
  m_rtv = std::move(rtv);
  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_layers = static_cast<u8>(layers);
  m_levels = static_cast<u8>(levels);
  m_samples = static_cast<u8>(samples);
  m_format = format;
  m_dynamic = dynamic;
  return true;
}

bool D3D11::Texture::Adopt(ID3D11Device* device, ComPtr<ID3D11Texture2D> texture)
{
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);

  ComPtr<ID3D11ShaderResourceView> srv;
  if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
  {
    const D3D11_SRV_DIMENSION srv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(srv_dimension, desc.Format, 0, desc.MipLevels, 0, desc.ArraySize);
    const HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, srv.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create SRV for adopted texture failed: 0x%08X", hr);
      return false;
    }
  }

  ComPtr<ID3D11RenderTargetView> rtv;
  if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
  {
    const D3D11_RTV_DIMENSION rtv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    const CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(rtv_dimension, desc.Format, 0, 0, desc.ArraySize);
    const HRESULT hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create RTV for adopted texture failed: 0x%08X", hr);
      return false;
    }
  }

  m_texture = std::move(texture);
  m_srv = std::move(srv);
  m_rtv = std::move(rtv);
  m_width = static_cast<u16>(desc.Width);
  m_height = static_cast<u16>(desc.Height);
  m_layers = static_cast<u8>(desc.ArraySize);
  m_levels = static_cast<u8>(desc.MipLevels);
  m_samples = static_cast<u8>(desc.SampleDesc.Count);
  m_dynamic = (desc.Usage == D3D11_USAGE_DYNAMIC);
  return true;
}

void D3D11::Texture::Destroy()
{
  m_rtv.Reset();
  m_srv.Reset();
  m_texture.Reset();
  m_dynamic = false;
  ClearBaseProperties();
}
