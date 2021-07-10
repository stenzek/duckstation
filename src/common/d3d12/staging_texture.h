#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <cstring>
#include <d3d12.h>
#include <wrl/client.h>

namespace D3D12 {
class StagingTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  StagingTexture();
  ~StagingTexture();

  ALWAYS_INLINE ID3D12Resource* GetD3DResource() const { return m_resource.Get(); }

  ALWAYS_INLINE u32 GetWidth() const { return m_width; }
  ALWAYS_INLINE u32 GetHeight() const { return m_height; }
  ALWAYS_INLINE DXGI_FORMAT GetFormat() const { return m_format; }
  ALWAYS_INLINE bool IsMapped() const { return m_mapped_pointer != nullptr; }
  ALWAYS_INLINE const void* GetMapPointer() const { return m_mapped_pointer; }

  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_resource); }

  bool Create(u32 width, u32 height, DXGI_FORMAT format, bool for_uploading);
  void Destroy(bool defer = true);

  bool Map(bool writing);
  void Unmap();
  void Flush();

  void CopyToTexture(u32 src_x, u32 src_y, ID3D12Resource* dst_texture, u32 dst_subresource, u32 dst_x, u32 dst_y,
                     u32 width, u32 height);
  void CopyFromTexture(ID3D12Resource* src_texture, u32 src_subresource, u32 src_x, u32 src_y, u32 dst_x, u32 dst_y,
                       u32 width, u32 height);


  bool ReadPixels(u32 x, u32 y, u32 width, u32 height, void* data, u32 row_pitch);

  bool WritePixels(u32 x, u32 y, u32 width, u32 height, const void* data, u32 row_pitch);

  bool EnsureSize(u32 width, u32 height, DXGI_FORMAT format, bool for_uploading);

protected:
  ComPtr<ID3D12Resource> m_resource;
  u32 m_width;
  u32 m_height;
  DXGI_FORMAT m_format;
  u32 m_texel_size;
  u32 m_row_pitch;
  u32 m_buffer_size;

  void* m_mapped_pointer = nullptr;
  u64 m_completed_fence = 0;
  bool m_mapped_for_write = false;
  bool m_needs_flush = false;
};

} // namespace D3D12