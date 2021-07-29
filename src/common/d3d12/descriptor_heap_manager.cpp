// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "descriptor_heap_manager.h"
#include "../assert.h"
#include "../log.h"
#include "context.h"
Log_SetChannel(DescriptorHeapManager);

namespace D3D12 {
DescriptorHeapManager::DescriptorHeapManager() = default;
DescriptorHeapManager::~DescriptorHeapManager() = default;

bool DescriptorHeapManager::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors,
                                   bool shader_visible)
{
  D3D12_DESCRIPTOR_HEAP_DESC desc = {type, static_cast<UINT>(num_descriptors),
                                     shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE :
                                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE};

  HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptor_heap));
  AssertMsg(SUCCEEDED(hr), "Create descriptor heap");
  if (FAILED(hr))
    return false;

  m_heap_base_cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  m_heap_base_gpu = m_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
  m_num_descriptors = num_descriptors;
  m_descriptor_increment_size = device->GetDescriptorHandleIncrementSize(type);

  // Set all slots to unallocated (1)
  const u32 bitset_count = num_descriptors / BITSET_SIZE + (((num_descriptors % BITSET_SIZE) != 0) ? 1 : 0);
  m_free_slots.resize(bitset_count);
  for (BitSetType& bs : m_free_slots)
    bs.flip();

  return true;
}

void DescriptorHeapManager::Destroy()
{
  for (BitSetType& bs : m_free_slots)
    Assert(bs.all());

  m_num_descriptors = 0;
  m_descriptor_increment_size = 0;
  m_heap_base_cpu = {};
  m_heap_base_gpu = {};
  m_descriptor_heap.Reset();
  m_free_slots.clear();
}

bool DescriptorHeapManager::Allocate(DescriptorHandle* handle, u32 count /* = 1 */)
{
  // Start past the temporary slots, no point in searching those.
  for (u32 group = 0; group < m_free_slots.size(); group++)
  {
    BitSetType& bs = m_free_slots[group];
    if (bs.none())
      continue;

    u32 bit = 0;
    for (; bit < BITSET_SIZE; bit++)
    {
      if (bs[bit])
      {
        u32 offset;
        for (offset = 0; offset < count; offset++)
        {
          if (!bs[bit + offset])
            break;
        }

        if (offset == count)
          break;
      }
    }

    u32 index = group * BITSET_SIZE + bit;
    for (u32 offset = 0; offset < count; offset++)
      bs[bit + offset] = false;

    handle->index = index;
    handle->cpu_handle.ptr = m_heap_base_cpu.ptr + index * m_descriptor_increment_size;
    handle->gpu_handle.ptr = m_heap_base_gpu.ptr + index * m_descriptor_increment_size;
    return true;
  }

  Panic("Out of fixed descriptors");
  return false;
}

void DescriptorHeapManager::Free(u32 index, u32 count /* = 1 */)
{
  Assert(index < m_num_descriptors);

  for (u32 i = 0; i < count; i++, index++)
  {
    u32 group = index / BITSET_SIZE;
    u32 bit = index % BITSET_SIZE;
    m_free_slots[group][bit] = true;
  }
}

void DescriptorHeapManager::Free(DescriptorHandle* handle, u32 count /* = 1 */)
{
  if (handle->index == DescriptorHandle::INVALID_INDEX)
    return;

  Free(handle->index, count);
  handle->Clear();
}

} // namespace D3D12
