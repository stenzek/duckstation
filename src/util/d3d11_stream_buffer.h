// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "common/types.h"
#include "common/windows_headers.h"

#include <d3d11_1.h>
#include <wrl/client.h>

class D3D11StreamBuffer
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  D3D11StreamBuffer();
  D3D11StreamBuffer(ComPtr<ID3D11Buffer> buffer);
  ~D3D11StreamBuffer();

  ALWAYS_INLINE ID3D11Buffer* GetD3DBuffer() const { return m_buffer.Get(); }
  ALWAYS_INLINE ID3D11Buffer* const* GetD3DBufferArray() const { return m_buffer.GetAddressOf(); }
  ALWAYS_INLINE u32 GetSize() const { return m_size; }
  ALWAYS_INLINE u32 GetPosition() const { return m_position; }
  ALWAYS_INLINE bool IsMapped() const { return m_mapped; }
  ALWAYS_INLINE bool IsUsingMapNoOverwrite() const { return m_use_map_no_overwrite; }

  bool Create(D3D11_BIND_FLAG bind_flags, u32 min_size, u32 max_size);
  void Destroy();

  struct MappingResult
  {
    void* pointer;
    u32 buffer_offset;
    u32 index_aligned; // offset / alignment, suitable for base vertex
    u32 space_aligned; // remaining space / alignment
  };

  MappingResult Map(ID3D11DeviceContext1* context, u32 alignment, u32 min_size);
  void Unmap(ID3D11DeviceContext1* context, u32 used_size);

private:
  ComPtr<ID3D11Buffer> m_buffer;
  u32 m_size;
  u32 m_max_size;
  u32 m_position;
  bool m_use_map_no_overwrite = false;
  bool m_mapped = false;
};
