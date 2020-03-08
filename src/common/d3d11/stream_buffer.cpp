#include "stream_buffer.h"
#include "../align.h"
#include "../assert.h"
#include "../log.h"
Log_SetChannel(D3D11);

namespace D3D11 {

StreamBuffer::StreamBuffer() : m_size(0), m_position(0) {}

StreamBuffer::StreamBuffer(ComPtr<ID3D11Buffer> buffer) : m_buffer(std::move(buffer)), m_position(0)
{
  D3D11_BUFFER_DESC desc;
  m_buffer->GetDesc(&desc);
  m_size = desc.ByteWidth;
}

StreamBuffer::~StreamBuffer()
{
  Release();
}

bool StreamBuffer::Create(ID3D11Device* device, D3D11_BIND_FLAG bind_flags, u32 size)
{
  CD3D11_BUFFER_DESC desc(size, bind_flags, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, 0, 0);
  ComPtr<ID3D11Buffer> buffer;
  HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Creating buffer failed: 0x%08X", hr);
    return false;
  }

  m_buffer = std::move(buffer);
  m_size = size;
  m_position = 0;

  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
  if (SUCCEEDED(hr))
  {
    if (bind_flags & D3D11_BIND_CONSTANT_BUFFER)
      m_use_map_no_overwrite = options.MapNoOverwriteOnDynamicConstantBuffer;
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

  return true;
}

void StreamBuffer::Adopt(ComPtr<ID3D11Buffer> buffer)
{
  m_buffer = std::move(buffer);

  D3D11_BUFFER_DESC desc;
  m_buffer->GetDesc(&desc);
  m_size = desc.ByteWidth;
  m_position = 0;
}

void StreamBuffer::Release()
{
  m_buffer.Reset();
}

StreamBuffer::MappingResult StreamBuffer::Map(ID3D11DeviceContext* context, u32 alignment, u32 min_size)
{
  m_position = Common::AlignUp(m_position, alignment);
  if ((m_position + min_size) >= m_size || !m_use_map_no_overwrite)
  {
    // wrap around
    m_position = 0;
  }

  D3D11_MAPPED_SUBRESOURCE sr;
  const D3D11_MAP map_type = (m_position == 0) ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
  const HRESULT hr = context->Map(m_buffer.Get(), 0, map_type, 0, &sr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map failed: 0x%08X (alignment %u, minsize %u, size %u, position %u, map type %u)", hr, alignment,
                    min_size, m_size, m_position, static_cast<u32>(map_type));
    Panic("Map failed");
    return {};
  }

  return MappingResult{static_cast<char*>(sr.pData) + m_position, m_position, m_position / alignment,
                       (m_size - m_position) / alignment};
}

void StreamBuffer::Unmap(ID3D11DeviceContext* context, u32 used_size)
{
  context->Unmap(m_buffer.Get(), 0);
  m_position += used_size;
}

} // namespace D3D11