// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "d3d11_texture.h"
#include "d3d11_device.h"
#include "d3d_common.h"

// #include "common/align.h"
// #include "common/assert.h"
// #include "common/file_system.h"
#include "common/log.h"
// #include "common/path.h"
// #include "common/rectangle.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <array>

Log_SetChannel(D3D11Device);

std::unique_ptr<GPUTexture> D3D11Device::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                       GPUTexture::Type type, GPUTexture::Format format,
                                                       const void* data, u32 data_stride)
{
  return D3D11Texture::Create(m_device.Get(), width, height, layers, levels, samples, type, format, data, data_stride);
}

bool D3D11Device::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                  u32 out_data_stride)
{
  const D3D11Texture* tex = static_cast<const D3D11Texture*>(texture);
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

  s_stats.num_downloads++;

  const u32 copy_size = tex->GetPixelSize() * width;
  StringUtil::StrideMemCpy(out_data, out_data_stride, sr.pData, sr.RowPitch, copy_size, height);
  m_context->Unmap(m_readback_staging_texture.Get(), 0);
  return true;
}

bool D3D11Device::CheckStagingBufferSize(u32 width, u32 height, DXGI_FORMAT format)
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

void D3D11Device::DestroyStagingBuffer()
{
  m_readback_staging_texture.Reset();
  m_readback_staging_texture_width = 0;
  m_readback_staging_texture_height = 0;
  m_readback_staging_texture_format = DXGI_FORMAT_UNKNOWN;
}

bool D3D11Device::SupportsTextureFormat(GPUTexture::Format format) const
{
  const DXGI_FORMAT dfmt = D3DCommon::GetFormatMapping(format).resource_format;
  if (dfmt == DXGI_FORMAT_UNKNOWN)
    return false;

  UINT support = 0;
  const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
  return (SUCCEEDED(m_device->CheckFormatSupport(dfmt, &support)) && ((support & required) == required));
}

D3D11Sampler::D3D11Sampler(ComPtr<ID3D11SamplerState> ss) : m_ss(std::move(ss))
{
}

D3D11Sampler::~D3D11Sampler() = default;

void D3D11Sampler::SetDebugName(const std::string_view& name)
{
  SetD3DDebugObjectName(m_ss.Get(), name);
}

std::unique_ptr<GPUSampler> D3D11Device::CreateSampler(const GPUSampler::Config& config)
{
  static constexpr std::array<D3D11_TEXTURE_ADDRESS_MODE, static_cast<u8>(GPUSampler::AddressMode::MaxCount)> ta = {{
    D3D11_TEXTURE_ADDRESS_WRAP,   // Repeat
    D3D11_TEXTURE_ADDRESS_CLAMP,  // ClampToEdge
    D3D11_TEXTURE_ADDRESS_BORDER, // ClampToBorder
    D3D11_TEXTURE_ADDRESS_MIRROR, // MirrorRepeat
  }};
  static constexpr u8 filter_count = static_cast<u8>(GPUSampler::Filter::MaxCount);
  static constexpr D3D11_FILTER filters[filter_count][filter_count][filter_count] = {
    {
      {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT},
      {D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT},
    },
    {
      {D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR, D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR},
      {D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR, D3D11_FILTER_MIN_MAG_MIP_LINEAR},
    }};

  D3D11_SAMPLER_DESC desc = {};
  desc.AddressU = ta[static_cast<u8>(config.address_u.GetValue())];
  desc.AddressV = ta[static_cast<u8>(config.address_v.GetValue())];
  desc.AddressW = ta[static_cast<u8>(config.address_w.GetValue())];
  std::memcpy(desc.BorderColor, RGBA8ToFloat(config.border_color).data(), sizeof(desc.BorderColor));
  desc.MinLOD = static_cast<float>(config.min_lod);
  desc.MaxLOD = static_cast<float>(config.max_lod);

  if (config.anisotropy > 1)
  {
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = config.anisotropy;
  }
  else
  {
    desc.Filter = filters[static_cast<u8>(config.mip_filter.GetValue())][static_cast<u8>(config.min_filter.GetValue())]
                         [static_cast<u8>(config.mag_filter.GetValue())];
    desc.MaxAnisotropy = 1;
  }

  ComPtr<ID3D11SamplerState> ss;
  const HRESULT hr = m_device->CreateSamplerState(&desc, ss.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateSamplerState() failed: %08X", hr);
    return {};
  }

  return std::unique_ptr<GPUSampler>(new D3D11Sampler(std::move(ss)));
}

D3D11Texture::D3D11Texture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                           ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv,
                           ComPtr<ID3D11View> rtv_dsv)
  : GPUTexture(static_cast<u16>(width), static_cast<u16>(height), static_cast<u8>(layers), static_cast<u8>(levels),
               static_cast<u8>(samples), type, format),
    m_texture(std::move(texture)), m_srv(std::move(srv)), m_rtv_dsv(std::move(rtv_dsv))
{
}

D3D11Texture::~D3D11Texture()
{
  D3D11Device::GetInstance().UnbindTexture(this);
  m_rtv_dsv.Reset();
  m_srv.Reset();
  m_texture.Reset();
}

D3D11_TEXTURE2D_DESC D3D11Texture::GetDesc() const
{
  D3D11_TEXTURE2D_DESC desc;
  m_texture->GetDesc(&desc);
  return desc;
}

void D3D11Texture::CommitClear(ID3D11DeviceContext1* context)
{
  if (m_state == GPUTexture::State::Dirty)
    return;

  if (IsDepthStencil())
  {
    if (m_state == GPUTexture::State::Invalidated)
      context->DiscardView(GetD3DDSV());
    else
      context->ClearDepthStencilView(GetD3DDSV(), D3D11_CLEAR_DEPTH, GetClearDepth(), 0);
  }
  else if (IsRenderTarget())
  {
    if (m_state == GPUTexture::State::Invalidated)
      context->DiscardView(GetD3DRTV());
    else
      context->ClearRenderTargetView(GetD3DRTV(), GetUNormClearColor().data());
  }

  m_state = GPUTexture::State::Dirty;
}

bool D3D11Texture::Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer /*= 0*/,
                          u32 level /*= 0*/)
{
  if (m_type == Type::DynamicTexture)
  {
    void* map;
    u32 map_stride;
    if (!Map(&map, &map_stride, x, y, width, height, layer, level))
      return false;

    StringUtil::StrideMemCpy(map, map_stride, data, pitch, GetPixelSize() * width, height);
    Unmap();
    return true;
  }

  const CD3D11_BOX box(static_cast<LONG>(x), static_cast<LONG>(y), 0, static_cast<LONG>(x + width),
                       static_cast<LONG>(y + height), 1);
  const u32 srnum = D3D11CalcSubresource(level, layer, m_levels);

  ID3D11DeviceContext1* context = D3D11Device::GetD3DContext();
  CommitClear(context);

  GPUDevice::GetStatistics().buffer_streamed += height * pitch;
  GPUDevice::GetStatistics().num_uploads++;

  context->UpdateSubresource(m_texture.Get(), srnum, &box, data, pitch, 0);
  m_state = GPUTexture::State::Dirty;
  return true;
}

bool D3D11Texture::Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer /*= 0*/,
                       u32 level /*= 0*/)
{
  if (m_type != Type::DynamicTexture || (x + width) > GetMipWidth(level) || (y + height) > GetMipHeight(level) ||
      layer > m_layers || level > m_levels)
  {
    return false;
  }

  const bool discard = (width == m_width && height == m_height);
  const u32 srnum = D3D11CalcSubresource(level, layer, m_levels);

  ID3D11DeviceContext1* context = D3D11Device::GetD3DContext();
  CommitClear(context);

  D3D11_MAPPED_SUBRESOURCE sr;
  HRESULT hr = context->Map(m_texture.Get(), srnum, discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_READ_WRITE, 0, &sr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map pixels texture failed: %08X", hr);
    return false;
  }

  GPUDevice::GetStatistics().buffer_streamed += height * sr.RowPitch;
  GPUDevice::GetStatistics().num_uploads++;

  *map = static_cast<u8*>(sr.pData) + (y * sr.RowPitch) + (x * GetPixelSize());
  *map_stride = sr.RowPitch;
  m_mapped_subresource = srnum;
  m_state = GPUTexture::State::Dirty;
  return true;
}

void D3D11Texture::Unmap()
{
  D3D11Device::GetD3DContext()->Unmap(m_texture.Get(), m_mapped_subresource);
  m_mapped_subresource = 0;
}

void D3D11Texture::SetDebugName(const std::string_view& name)
{
  SetD3DDebugObjectName(m_texture.Get(), name);
}

DXGI_FORMAT D3D11Texture::GetDXGIFormat() const
{
  return D3DCommon::GetFormatMapping(m_format).resource_format;
}

std::unique_ptr<D3D11Texture> D3D11Texture::Create(ID3D11Device* device, u32 width, u32 height, u32 layers, u32 levels,
                                                   u32 samples, Type type, Format format,
                                                   const void* initial_data /* = nullptr */,
                                                   u32 initial_data_stride /* = 0 */)
{
  if (!ValidateConfig(width, height, layers, layers, samples, type, format))
    return nullptr;

  u32 bind_flags = 0;
  D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
  u32 cpu_access = 0;
  switch (type)
  {
    case Type::RenderTarget:
      bind_flags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      break;
    case Type::DepthStencil:
      bind_flags = D3D11_BIND_DEPTH_STENCIL; // | D3D11_BIND_SHADER_RESOURCE;
      break;
    case Type::Texture:
      bind_flags = D3D11_BIND_SHADER_RESOURCE;
      break;
    case Type::DynamicTexture:
      bind_flags = D3D11_BIND_SHADER_RESOURCE;
      usage = D3D11_USAGE_DYNAMIC;
      cpu_access = D3D11_CPU_ACCESS_WRITE;
      break;
    case Type::RWTexture:
      bind_flags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
      break;
    default:
      break;
  }

  const D3DCommon::DXGIFormatMapping& fm = D3DCommon::GetFormatMapping(format);

  CD3D11_TEXTURE2D_DESC desc(fm.resource_format, width, height, layers, levels, bind_flags, usage, cpu_access, samples,
                             0, 0);

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
    return nullptr;
  }

  if (initial_data)
  {
    GPUDevice::GetStatistics().buffer_streamed += height * initial_data_stride;
    GPUDevice::GetStatistics().num_uploads++;
  }

  ComPtr<ID3D11ShaderResourceView> srv;
  if (bind_flags & D3D11_BIND_SHADER_RESOURCE)
  {
    const D3D11_SRV_DIMENSION srv_dimension =
      (desc.SampleDesc.Count > 1) ?
        D3D11_SRV_DIMENSION_TEXTURE2DMS :
        (desc.ArraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D);
    const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(srv_dimension, fm.srv_format, 0, desc.MipLevels, 0, desc.ArraySize);
    const HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, srv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create SRV for texture failed: 0x%08X", hr);
      return nullptr;
    }
  }

  ComPtr<ID3D11View> rtv_dsv;
  if (bind_flags & D3D11_BIND_RENDER_TARGET)
  {
    const D3D11_RTV_DIMENSION rtv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    const CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(rtv_dimension, fm.rtv_format, 0, 0, desc.ArraySize);
    ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, rtv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create RTV for texture failed: 0x%08X", hr);
      return nullptr;
    }

    rtv_dsv = std::move(rtv);
  }
  else if (bind_flags & D3D11_BIND_DEPTH_STENCIL)
  {
    const D3D11_DSV_DIMENSION dsv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    const CD3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc(dsv_dimension, fm.dsv_format, 0, 0, desc.ArraySize);
    ComPtr<ID3D11DepthStencilView> dsv;
    const HRESULT hr = device->CreateDepthStencilView(texture.Get(), &dsv_desc, dsv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create DSV for texture failed: 0x%08X", hr);
      return nullptr;
    }

    rtv_dsv = std::move(dsv);
  }

  return std::unique_ptr<D3D11Texture>(new D3D11Texture(width, height, layers, levels, samples, type, format,
                                                        std::move(texture), std::move(srv), std::move(rtv_dsv)));
}

D3D11TextureBuffer::D3D11TextureBuffer(Format format, u32 size_in_elements) : GPUTextureBuffer(format, size_in_elements)
{
}

D3D11TextureBuffer::~D3D11TextureBuffer() = default;

bool D3D11TextureBuffer::CreateBuffer(ID3D11Device* device)
{
  if (!m_buffer.Create(device, D3D11_BIND_SHADER_RESOURCE, GetSizeInBytes()))
    return false;

  static constexpr std::array<DXGI_FORMAT, static_cast<u32>(Format::MaxCount)> dxgi_formats = {{
    DXGI_FORMAT_R16_UINT,
  }};

  CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(m_buffer.GetD3DBuffer(), dxgi_formats[static_cast<u32>(m_format)], 0,
                                            m_size_in_elements);
  const HRESULT hr = device->CreateShaderResourceView(m_buffer.GetD3DBuffer(), &srv_desc, m_srv.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateShaderResourceView() failed: %08X", hr);
    return false;
  }

  return true;
}

void* D3D11TextureBuffer::Map(u32 required_elements)
{
  const u32 esize = GetElementSize(m_format);
  const auto res = m_buffer.Map(D3D11Device::GetD3DContext(), esize, esize * required_elements);
  m_current_position = res.index_aligned;
  return res.pointer;
}

void D3D11TextureBuffer::Unmap(u32 used_elements)
{
  const u32 size = used_elements * GetElementSize(m_format);
  GPUDevice::GetStatistics().buffer_streamed += size;
  GPUDevice::GetStatistics().num_uploads++;
  m_buffer.Unmap(D3D11Device::GetD3DContext(), size);
}

void D3D11TextureBuffer::SetDebugName(const std::string_view& name)
{
  SetD3DDebugObjectName(m_buffer.GetD3DBuffer(), name);
}

std::unique_ptr<GPUTextureBuffer> D3D11Device::CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                   u32 size_in_elements)
{
  std::unique_ptr<D3D11TextureBuffer> tb = std::make_unique<D3D11TextureBuffer>(format, size_in_elements);
  if (!tb->CreateBuffer(m_device.Get()))
    tb.reset();

  return tb;
}
