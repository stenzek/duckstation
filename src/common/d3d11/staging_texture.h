#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <cstring>
#include <d3d11.h>
#include <wrl/client.h>

namespace D3D11 {
class StagingTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  StagingTexture();
  ~StagingTexture();

  ALWAYS_INLINE ID3D11Texture2D* GetD3DTexture() const { return m_texture.Get(); }

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE DXGI_FORMAT GetFormat() const { return m_format; }
  ALWAYS_INLINE bool IsMapped() const { return m_map.pData != nullptr; }
  ALWAYS_INLINE const D3D11_MAPPED_SUBRESOURCE& GetMappedSubresource() const { return m_map; }

  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_texture); }

  bool Create(ID3D11Device* device, u32 width, u32 height, DXGI_FORMAT format, bool for_uploading);
  void Destroy();

  bool Map(ID3D11DeviceContext* context, bool writing);
  void Unmap(ID3D11DeviceContext* context);

  void CopyToTexture(ID3D11DeviceContext* context, u32 src_x, u32 src_y, ID3D11Resource* dst_texture,
                     u32 dst_subresource, u32 dst_x, u32 dst_y, u32 width, u32 height);
  void CopyFromTexture(ID3D11DeviceContext* context, ID3D11Resource* src_texture, u32 src_subresource, u32 src_x,
                       u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height);

  template<typename T>
  T ReadPixel(u32 x, u32 y)
  {
    T pixel;
    std::memcpy(&pixel, static_cast<u8*>(m_map.pData) + (y * m_map.RowPitch) + x, sizeof(T));
    return pixel;
  }

  template<typename T>
  void WritePixel(u32 x, u32 y, T pixel)
  {
    std::memcpy(static_cast<u8*>(m_map.pData) + (y * m_map.RowPitch) + x, &pixel, sizeof(T));
  }

  template<typename T>
  void ReadPixels(u32 x, u32 y, u32 width, u32 height, u32 stride, T* data)
  {
    const u8* src_ptr = static_cast<u8*>(m_map.pData) + (y * m_map.RowPitch) + (x * sizeof(T));
    u8* dst_ptr = reinterpret_cast<u8*>(data);
    if (m_map.RowPitch != (sizeof(T) * stride))
    {
      for (u32 row = 0; row < height; row++)
      {
        std::memcpy(dst_ptr, src_ptr, sizeof(T) * width);
        src_ptr += m_map.RowPitch;
        dst_ptr += sizeof(T) * stride;
      }
    }
    else
    {
      std::memcpy(dst_ptr, src_ptr, (sizeof(T) * stride) * height);
    }
  }

  template<typename T>
  bool ReadPixels(ID3D11DeviceContext* context, u32 x, u32 y, u32 width, u32 height, u32 stride, T* data)
  {
    const bool was_mapped = IsMapped();
    if (!was_mapped && !Map(context, false))
      return false;

    ReadPixels<T>(x, y, width, height, stride, data);
    if (!was_mapped)
      Unmap(context);

    return true;
  }

  template<typename T>
  void WritePixels(u32 x, u32 y, u32 width, u32 height, u32 stride, const T* data)
  {
    const u8* src_ptr = reinterpret_cast<const u8*>(data);
    u8* dst_ptr = static_cast<u8*>(m_map.pData) + (y * m_map.RowPitch) + (x * sizeof(T));
    if (m_map.RowPitch != (sizeof(T) * stride))
    {
      for (u32 row = 0; row < height; row++)
      {
        std::memcpy(dst_ptr, src_ptr, sizeof(T) * width);
        src_ptr += sizeof(T) * stride;
        dst_ptr += m_map.RowPitch;
      }
    }
    else
    {
      std::memcpy(dst_ptr, src_ptr, (sizeof(T) * stride) * height);
    }
  }

  template<typename T>
  bool WritePixels(ID3D11DeviceContext* context, u32 x, u32 y, u32 width, u32 height, u32 stride, const T* data)
  {
    const bool was_mapped = IsMapped();
    if (!was_mapped && !Map(context, true))
      return false;

    WritePixels<T>(context, x, y, width, height, stride, data);
    if (!was_mapped)
      Unmap(context);

    return true;
  }

protected:
  ComPtr<ID3D11Texture2D> m_texture;
  u32 m_width;
  u32 m_height;
  DXGI_FORMAT m_format;

  D3D11_MAPPED_SUBRESOURCE m_map = {};
};

class AutoStagingTexture : public StagingTexture
{
public:
  bool EnsureSize(ID3D11DeviceContext* context, u32 width, u32 height, DXGI_FORMAT format, bool for_uploading);

  void CopyFromTexture(ID3D11DeviceContext* context, ID3D11Resource* src_texture, u32 src_subresource, u32 src_x,
                       u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height);

  template<typename T>
  bool WritePixels(ID3D11DeviceContext* context, u32 x, u32 y, u32 width, u32 height, u32 stride, const T* data)
  {
    if (!EnsureSize(context, width, height, m_format, true))
      return false;

    const bool was_mapped = IsMapped();
    if (!was_mapped && !Map(context, true))
      return false;

    WritePixels<T>(context, x, y, width, height, stride, data);

    if (!was_mapped)
      Unmap(context);

    return true;
  }
};
} // namespace D3D11