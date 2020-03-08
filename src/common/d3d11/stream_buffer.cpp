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
  const HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Creating buffer failed: 0x%08X", hr);
    return false;
  }

  m_buffer = std::move(buffer);
  m_size = size;
  m_position = 0;
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
  if ((m_position + min_size) >= m_size)
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