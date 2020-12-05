#include "texture.h"
#include "../log.h"
Log_SetChannel(D3D11);

namespace D3D11 {

Texture::Texture() : m_width(0), m_height(0), m_samples(0) {}

Texture::Texture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv,
                 ComPtr<ID3D11RenderTargetView> rtv)
  : m_texture(std::move(texture)), m_srv(std::move(srv)), m_rtv(std::move(rtv))
{
  const D3D11_TEXTURE2D_DESC desc = GetDesc();
  m_width = desc.Width;
  m_height = desc.Height;
  m_samples = desc.SampleDesc.Count;
}

Texture::~Texture()
{
  Destroy();
}

D3D11_TEXTURE2D_DESC Texture::GetDesc() const
{
  D3D11_TEXTURE2D_DESC desc;
  m_texture->GetDesc(&desc);
  return desc;
}

bool Texture::Create(ID3D11Device* device, u32 width, u32 height, u32 samples, DXGI_FORMAT format, u32 bind_flags,
                     const void* initial_data /* = nullptr */, u32 initial_data_stride /* = 0 */, bool dynamic)
{
  CD3D11_TEXTURE2D_DESC desc(format, width, height, 1, 1, bind_flags,
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
    Log_ErrorPrintf("Create texture failed: 0x%08X", tex_hr);
    return false;
  }

  ComPtr<ID3D11ShaderResourceView> srv;
  if (bind_flags & D3D11_BIND_SHADER_RESOURCE)
  {
    const D3D11_SRV_DIMENSION srv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(srv_dimension, desc.Format, 0, desc.MipLevels, 0, desc.ArraySize);
    const HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, srv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create SRV for adopted texture failed: 0x%08X", hr);
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
      Log_ErrorPrintf("Create RTV for adopted texture failed: 0x%08X", hr);
      return false;
    }
  }

  m_texture = std::move(texture);
  m_srv = std::move(srv);
  m_rtv = std::move(rtv);
  m_width = desc.Width;
  m_height = desc.Height;
  m_samples = desc.SampleDesc.Count;
  return true;
}

bool Texture::Adopt(ID3D11Device* device, ComPtr<ID3D11Texture2D> texture)
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
  m_width = desc.Width;
  m_height = desc.Height;
  m_samples = desc.SampleDesc.Count;
  return true;
}

void Texture::Destroy()
{
  m_rtv.Reset();
  m_srv.Reset();
  m_texture.Reset();
  m_width = 0;
  m_height = 0;
  m_samples = 0;
}

} // namespace D3D11