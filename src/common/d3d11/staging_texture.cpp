#include "staging_texture.h"
#include "../assert.h"
#include "../log.h"
Log_SetChannel(D3D11);

namespace D3D11 {

StagingTexture::StagingTexture() : m_width(0), m_height(0) {}

StagingTexture::~StagingTexture()
{
  Destroy();
}

bool StagingTexture::Create(ID3D11Device* device, u32 width, u32 height, DXGI_FORMAT format, bool for_uploading)
{
  CD3D11_TEXTURE2D_DESC desc(format, width, height, 1, 1, 0, for_uploading ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_STAGING,
                             for_uploading ? D3D11_CPU_ACCESS_WRITE : D3D11_CPU_ACCESS_READ, 1, 0, 0);

  ComPtr<ID3D11Texture2D> texture;
  const HRESULT tex_hr = device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf());
  if (FAILED(tex_hr))
  {
    Log_ErrorPrintf("Create texture failed: 0x%08X", tex_hr);
    return false;
  }

  m_texture = std::move(texture);
  m_width = desc.Width;
  m_height = desc.Height;
  m_format = desc.Format;
  return true;
}

void StagingTexture::Destroy()
{
  Assert(!IsMapped());
  m_texture.Reset();
}

bool StagingTexture::Map(ID3D11DeviceContext* context, bool writing)
{
  Assert(!IsMapped());
  const HRESULT hr = context->Map(m_texture.Get(), 0, writing ? D3D11_MAP_WRITE : D3D11_MAP_READ, 0, &m_map);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map staging texture failed: 0x%08X", hr);
    return false;
  }

  return true;
}

void StagingTexture::Unmap(ID3D11DeviceContext* context)
{
  Assert(IsMapped());
  context->Unmap(m_texture.Get(), 0);
  m_map = {};
}

void StagingTexture::CopyToTexture(ID3D11DeviceContext* context, u32 src_x, u32 src_y, ID3D11Resource* dst_texture,
                                   u32 dst_subresource, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  DebugAssert((src_x + width) <= m_width && (src_y + height) <= m_height);

  const CD3D11_BOX box(static_cast<LONG>(src_x), static_cast<LONG>(src_y), 0, static_cast<LONG>(src_x + width),
                       static_cast<LONG>(src_y + height), 1);
  context->CopySubresourceRegion(dst_texture, dst_subresource, dst_x, dst_y, 0, m_texture.Get(), 0, &box);
}

void StagingTexture::CopyFromTexture(ID3D11DeviceContext* context, ID3D11Resource* src_texture, u32 src_subresource,
                                     u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  DebugAssert((dst_x + width) <= m_width && (dst_y + height) <= m_height);

  const CD3D11_BOX box(static_cast<LONG>(src_x), static_cast<LONG>(src_y), 0, static_cast<LONG>(src_x + width),
                       static_cast<LONG>(src_y + height), 1);
  context->CopySubresourceRegion(m_texture.Get(), 0, dst_x, dst_y, 0, src_texture, src_subresource, &box);
}

bool AutoStagingTexture::EnsureSize(ID3D11DeviceContext* context, u32 width, u32 height, DXGI_FORMAT format,
                                    bool for_uploading)
{
  if (m_texture && m_width >= width && m_height >= height && m_format == format)
    return true;

  ComPtr<ID3D11Device> device;
  context->GetDevice(device.GetAddressOf());

  CD3D11_TEXTURE2D_DESC new_desc(format, width, height, 1, 1, 0,
                                 for_uploading ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_STAGING,
                                 for_uploading ? D3D11_CPU_ACCESS_WRITE : D3D11_CPU_ACCESS_READ, 1, 0, 0);
  ComPtr<ID3D11Texture2D> new_texture;
  HRESULT hr = device->CreateTexture2D(&new_desc, nullptr, new_texture.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Create texture failed: 0x%08X", hr);
    return false;
  }

  m_texture = std::move(new_texture);
  m_width = width;
  m_height = height;
  m_format = format;
  return true;
}

void AutoStagingTexture::CopyFromTexture(ID3D11DeviceContext* context, ID3D11Resource* src_texture, u32 src_subresource,
                                         u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  if (!EnsureSize(context, width, height, m_format, false))
    return;

  StagingTexture::CopyFromTexture(context, src_texture, src_subresource, src_x, src_y, dst_x, dst_y, width, height);
}

} // namespace D3D11