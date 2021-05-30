#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace D3D11 {
class Texture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  Texture();
  Texture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv, ComPtr<ID3D11RenderTargetView> rtv);
  ~Texture();

  ALWAYS_INLINE ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* GetD3DSRV() const { return m_srv.Get(); }
  ALWAYS_INLINE ID3D11RenderTargetView* GetD3DRTV() const { return m_rtv.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* const* GetD3DSRVArray() const { return m_srv.GetAddressOf(); }
  ALWAYS_INLINE ID3D11RenderTargetView* const* GetD3DRTVArray() const { return m_rtv.GetAddressOf(); }

  ALWAYS_INLINE u16 GetWidth() const { return m_width; }
  ALWAYS_INLINE u16 GetHeight() const { return m_height; }
  ALWAYS_INLINE u16 GetLayers() const { return m_layers; }
  ALWAYS_INLINE u8 GetLevels() const { return m_levels; }
  ALWAYS_INLINE u8 GetSamples() const { return m_samples; }
  ALWAYS_INLINE bool IsMultisampled() const { return m_samples > 1; }
  ALWAYS_INLINE DXGI_FORMAT GetFormat() const { return GetDesc().Format; }
  D3D11_TEXTURE2D_DESC GetDesc() const;

  ALWAYS_INLINE operator ID3D11Texture2D*() const { return m_texture.Get(); }
  ALWAYS_INLINE operator ID3D11ShaderResourceView*() const { return m_srv.Get(); }
  ALWAYS_INLINE operator ID3D11RenderTargetView*() const { return m_rtv.Get(); }
  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_texture); }

  bool Create(ID3D11Device* device, u32 width, u32 height, u32 layers, u32 levels, u32 samples, DXGI_FORMAT format,
              u32 bind_flags, const void* initial_data = nullptr, u32 initial_data_stride = 0, bool dynamic = false);
  bool Adopt(ID3D11Device* device, ComPtr<ID3D11Texture2D> texture);

  void Destroy();

private:
  ComPtr<ID3D11Texture2D> m_texture;
  ComPtr<ID3D11ShaderResourceView> m_srv;
  ComPtr<ID3D11RenderTargetView> m_rtv;
  u16 m_width;
  u16 m_height;
  u16 m_layers;
  u8 m_levels;
  u8 m_samples;
};
} // namespace D3D11