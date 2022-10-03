#pragma once
#include "../gpu_texture.h"
#include "../windows_headers.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace D3D11 {

class Texture final : public GPUTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  Texture();
  Texture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv, ComPtr<ID3D11RenderTargetView> rtv);
  ~Texture();

  static DXGI_FORMAT GetDXGIFormat(Format format);
  static Format LookupBaseFormat(DXGI_FORMAT dformat);

  ALWAYS_INLINE ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* GetD3DSRV() const { return m_srv.Get(); }
  ALWAYS_INLINE ID3D11RenderTargetView* GetD3DRTV() const { return m_rtv.Get(); }
  ALWAYS_INLINE ID3D11ShaderResourceView* const* GetD3DSRVArray() const { return m_srv.GetAddressOf(); }
  ALWAYS_INLINE ID3D11RenderTargetView* const* GetD3DRTVArray() const { return m_rtv.GetAddressOf(); }
  ALWAYS_INLINE DXGI_FORMAT GetDXGIFormat() const { return GetDXGIFormat(m_format); }
  ALWAYS_INLINE bool IsDynamic() const { return m_dynamic; }

  ALWAYS_INLINE operator ID3D11Texture2D*() const { return m_texture.Get(); }
  ALWAYS_INLINE operator ID3D11ShaderResourceView*() const { return m_srv.Get(); }
  ALWAYS_INLINE operator ID3D11RenderTargetView*() const { return m_rtv.Get(); }
  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_texture); }

  D3D11_TEXTURE2D_DESC GetDesc() const;

  bool IsValid() const override;

  bool Create(ID3D11Device* device, u32 width, u32 height, u32 layers, u32 levels, u32 samples, Format format,
              u32 bind_flags, const void* initial_data = nullptr, u32 initial_data_stride = 0, bool dynamic = false);
  bool Adopt(ID3D11Device* device, ComPtr<ID3D11Texture2D> texture);

  void Destroy();

private:
  ComPtr<ID3D11Texture2D> m_texture;
  ComPtr<ID3D11ShaderResourceView> m_srv;
  ComPtr<ID3D11RenderTargetView> m_rtv;
  bool m_dynamic = false;
};

} // namespace D3D11