// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "d3d11_stream_buffer.h"
#include "d3d11_device.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"

Log_SetChannel(D3D11Device);

D3D11StreamBuffer::D3D11StreamBuffer() : m_size(0), m_position(0)
{
}

D3D11StreamBuffer::D3D11StreamBuffer(ComPtr<ID3D11Buffer> buffer) : m_buffer(std::move(buffer)), m_position(0)
{
  D3D11_BUFFER_DESC desc;
  m_buffer->GetDesc(&desc);
  m_size = desc.ByteWidth;
}

D3D11StreamBuffer::~D3D11StreamBuffer()
{
  Destroy();
}

bool D3D11StreamBuffer::Create(D3D11_BIND_FLAG bind_flags, u32 min_size, u32 max_size)
{
  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  HRESULT hr = D3D11Device::GetD3DDevice()->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
  if (SUCCEEDED(hr))
  {
    if (bind_flags & D3D11_BIND_CONSTANT_BUFFER)
    {
      // Older Intel drivers go absolutely bananas with CPU usage when using offset constant buffers.
      // NVIDIA seems to be okay, I don't know about AMD. So let's be safe and limit it to feature level 12+.
      m_use_map_no_overwrite = options.MapNoOverwriteOnDynamicConstantBuffer;
      if (m_use_map_no_overwrite && D3D11Device::GetMaxFeatureLevel() < D3D_FEATURE_LEVEL_12_0)
      {
        Log_WarningPrint("Ignoring MapNoOverwriteOnDynamicConstantBuffer on driver due to feature level.");
        m_use_map_no_overwrite = false;
      }

      // should be 16 byte aligned
      min_size = Common::AlignUpPow2(min_size, 16);
      max_size = Common::AlignUpPow2(max_size, 16);
    }
    else if (bind_flags & D3D11_BIND_SHADER_RESOURCE)
      m_use_map_no_overwrite = options.MapNoOverwriteOnDynamicBufferSRV;
    else
      m_use_map_no_overwrite = true;

    if (!m_use_map_no_overwrite)
    {
      Log_WarningPrintf("Unable to use MAP_NO_OVERWRITE on buffer with bind flag %u, this may affect performance. "
                        "Update your driver/operating system.",
                        static_cast<unsigned>(bind_flags));
    }
  }
  else
  {
    Log_WarningPrintf("ID3D11Device::CheckFeatureSupport() failed: 0x%08X", hr);
    m_use_map_no_overwrite = false;
  }

  const u32 create_size = m_use_map_no_overwrite ? max_size : min_size;
  const CD3D11_BUFFER_DESC desc(create_size, bind_flags, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, 0, 0);
  ComPtr<ID3D11Buffer> buffer;
  hr = D3D11Device::GetD3DDevice()->CreateBuffer(&desc, nullptr, &buffer);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Creating buffer failed: 0x%08X", hr);
    return false;
  }

  m_buffer = std::move(buffer);
  m_size = create_size;
  m_max_size = max_size;
  m_position = 0;

  return true;
}

void D3D11StreamBuffer::Destroy()
{
  m_buffer.Reset();
}

D3D11StreamBuffer::MappingResult D3D11StreamBuffer::Map(ID3D11DeviceContext1* context, u32 alignment, u32 min_size)
{
  HRESULT hr;
  DebugAssert(!m_mapped);

  m_position = Common::AlignUp(m_position, alignment);
  if ((m_position + min_size) >= m_size || !m_use_map_no_overwrite)
  {
    // wrap around
    m_position = 0;

    // grow buffer if needed
    if (min_size > m_size) [[unlikely]]
    {
      Assert(min_size < m_max_size);

      const u32 new_size = std::min(m_max_size, Common::AlignUp(std::max(m_size * 2, min_size), alignment));
      Log_WarningFmt("Growing buffer from {} bytes to {} bytes", m_size, new_size);

      D3D11_BUFFER_DESC new_desc;
      m_buffer->GetDesc(&new_desc);
      new_desc.ByteWidth = new_size;

      hr = D3D11Device::GetD3DDevice()->CreateBuffer(&new_desc, nullptr, m_buffer.ReleaseAndGetAddressOf());
      if (FAILED(hr))
      {
        Log_ErrorFmt("Creating buffer failed: 0x{:08X}", static_cast<unsigned>(hr));
        Panic("Failed to grow buffer");
      }

      m_size = new_size;
    }
  }

  D3D11_MAPPED_SUBRESOURCE sr;
  const D3D11_MAP map_type = (m_position == 0) ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
  hr = context->Map(m_buffer.Get(), 0, map_type, 0, &sr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map failed: 0x%08X (alignment %u, minsize %u, size %u, position %u, map type %u)", hr, alignment,
                    min_size, m_size, m_position, static_cast<u32>(map_type));
    Panic("Map failed");
  }

  m_mapped = true;
  return MappingResult{static_cast<char*>(sr.pData) + m_position, m_position, m_position / alignment,
                       (m_size - m_position) / alignment};
}

void D3D11StreamBuffer::Unmap(ID3D11DeviceContext1* context, u32 used_size)
{
  DebugAssert(m_mapped);

  context->Unmap(m_buffer.Get(), 0);
  m_position += used_size;
  m_mapped = false;
}
