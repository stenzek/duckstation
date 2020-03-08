#pragma once
#include "../types.h"
#include "../windows_headers.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace D3D11 {
class StreamBuffer
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  StreamBuffer();
  StreamBuffer(ComPtr<ID3D11Buffer> buffer);
  ~StreamBuffer();

  ALWAYS_INLINE ID3D11Buffer* GetD3DBuffer() const { return m_buffer.Get(); }
  ALWAYS_INLINE ID3D11Buffer* const* GetD3DBufferArray() const { return m_buffer.GetAddressOf(); }
  ALWAYS_INLINE u32 GetSize() const { return m_size; }
  ALWAYS_INLINE u32 GetPosition() const { return m_position; }

  bool Create(ID3D11Device* device, D3D11_BIND_FLAG bind_flags, u32 size);
  void Adopt(ComPtr<ID3D11Buffer> buffer);
  void Release();
  
  struct MappingResult
  {
    void* pointer;
    u32 buffer_offset;
    u32 index_aligned; // offset / alignment, suitable for base vertex
    u32 space_aligned; // remaining space / alignment
  };

  MappingResult Map(ID3D11DeviceContext* context, u32 alignment, u32 min_size);
  void Unmap(ID3D11DeviceContext* context, u32 used_size);

private:
  ComPtr<ID3D11Buffer> m_buffer;
  u32 m_size;
  u32 m_position;
  bool m_use_map_no_overwrite = false;
};
} // namespace GL