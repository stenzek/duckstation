// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "d3d11_stream_buffer.h"
#include "gpu_device.h"

#include "common/windows_headers.h"

#include <d3d11_1.h>
#include <wrl/client.h>

class D3D11Device;

class D3D11Sampler final : public GPUSampler
{
  friend D3D11Device;

  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

public:
  ~D3D11Sampler() override;

  ALWAYS_INLINE ID3D11SamplerState* GetSamplerState() const { return m_ss.Get(); }
  ALWAYS_INLINE ID3D11SamplerState* const* GetSamplerStateArray() const { return m_ss.GetAddressOf(); }

  void SetDebugName(const std::string_view& name) override;

private:
  D3D11Sampler(ComPtr<ID3D11SamplerState> ss);

  ComPtr<ID3D11SamplerState> m_ss;
};

class D3D11Texture final : public GPUTexture
{
  friend D3D11Device;

public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  ~D3D11Texture();

  ALWAYS_INLINE ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* GetD3DSRV() const { return m_srv.Get(); }
  ALWAYS_INLINE ID3D11View* GetRTVOrDSV() const { return m_rtv_dsv.Get(); }
  ALWAYS_INLINE ID3D11RenderTargetView* GetD3DRTV() const
  {
    return static_cast<ID3D11RenderTargetView*>(m_rtv_dsv.Get());
  }
  ALWAYS_INLINE ID3D11DepthStencilView* GetD3DDSV() const
  {
    return static_cast<ID3D11DepthStencilView*>(m_rtv_dsv.Get());
  }
  ALWAYS_INLINE ID3D11ShaderResourceView* const* GetD3DSRVArray() const { return m_srv.GetAddressOf(); }
  ALWAYS_INLINE ID3D11RenderTargetView* const* GetD3DRTVArray() const
  {
    return reinterpret_cast<ID3D11RenderTargetView* const*>(m_rtv_dsv.GetAddressOf());
  }
  DXGI_FORMAT GetDXGIFormat() const;

  ALWAYS_INLINE operator ID3D11Texture2D*() const { return m_texture.Get(); }
  ALWAYS_INLINE operator ID3D11ShaderResourceView*() const { return m_srv.Get(); }
  ALWAYS_INLINE operator ID3D11RenderTargetView*() const
  {
    return static_cast<ID3D11RenderTargetView*>(m_rtv_dsv.Get());
  }
  ALWAYS_INLINE operator ID3D11DepthStencilView*() const
  {
    return static_cast<ID3D11DepthStencilView*>(m_rtv_dsv.Get());
  }
  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_texture); }

  static std::unique_ptr<D3D11Texture> Create(ID3D11Device* device, u32 width, u32 height, u32 layers, u32 levels,
                                              u32 samples, Type type, Format format, const void* initial_data = nullptr,
                                              u32 initial_data_stride = 0);

  D3D11_TEXTURE2D_DESC GetDesc() const;
  void CommitClear(ID3D11DeviceContext1* context);

  bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0, u32 level = 0) override;
  bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) override;
  void Unmap() override;

  void SetDebugName(const std::string_view& name) override;

private:
  D3D11Texture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
               ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv, ComPtr<ID3D11View> rtv_dsv);

  ComPtr<ID3D11Texture2D> m_texture;
  ComPtr<ID3D11ShaderResourceView> m_srv;
  ComPtr<ID3D11View> m_rtv_dsv;
  u32 m_mapped_subresource = 0;
};

class D3D11TextureBuffer final : public GPUTextureBuffer
{
public:
  D3D11TextureBuffer(Format format, u32 size_in_elements);
  ~D3D11TextureBuffer() override;

  ALWAYS_INLINE ID3D11Buffer* GetBuffer() const { return m_buffer.GetD3DBuffer(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* GetSRV() const { return m_srv.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* const* GetSRVArray() const { return m_srv.GetAddressOf(); }

  bool CreateBuffer(ID3D11Device* device);

  // Inherited via GPUTextureBuffer
  void* Map(u32 required_elements) override;
  void Unmap(u32 used_elements) override;

  void SetDebugName(const std::string_view& name) override;

private:
  D3D11StreamBuffer m_buffer;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
};

class D3D11DownloadTexture final : public GPUDownloadTexture
{
public:
  ~D3D11DownloadTexture() override;

  static std::unique_ptr<D3D11DownloadTexture> Create(u32 width, u32 height, GPUTexture::Format format);

  void CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height,
                       u32 src_layer, u32 src_level, bool use_transfer_pitch) override;

  bool Map(u32 x, u32 y, u32 width, u32 height) override;
  void Unmap() override;

  void Flush() override;

  void SetDebugName(std::string_view name) override;

private:
  D3D11DownloadTexture(Microsoft::WRL::ComPtr<ID3D11Texture2D> tex, u32 width, u32 height, GPUTexture::Format format);

  Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
};
