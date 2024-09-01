// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/hash_combine.h"
#include "common/types.h"
#include "common/windows_headers.h"

#include <bitset>
#include <cstring>
#include <d3d12.h>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

class Error;

// This class provides an abstraction for D3D12 descriptor heaps.
struct D3D12DescriptorHandle final
{
  enum : u32
  {
    INVALID_INDEX = 0xFFFFFFFF
  };

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle{};
  u32 index = INVALID_INDEX;

  ALWAYS_INLINE operator bool() const { return index != INVALID_INDEX; }

  ALWAYS_INLINE operator D3D12_CPU_DESCRIPTOR_HANDLE() const { return cpu_handle; }
  ALWAYS_INLINE operator D3D12_GPU_DESCRIPTOR_HANDLE() const { return gpu_handle; }

  ALWAYS_INLINE bool operator==(const D3D12DescriptorHandle& rhs) const { return (index == rhs.index); }
  ALWAYS_INLINE bool operator!=(const D3D12DescriptorHandle& rhs) const { return (index != rhs.index); }
  ALWAYS_INLINE bool operator<(const D3D12DescriptorHandle& rhs) const { return (index < rhs.index); }
  ALWAYS_INLINE bool operator<=(const D3D12DescriptorHandle& rhs) const { return (index <= rhs.index); }
  ALWAYS_INLINE bool operator>(const D3D12DescriptorHandle& rhs) const { return (index > rhs.index); }
  ALWAYS_INLINE bool operator>=(const D3D12DescriptorHandle& rhs) const { return (index >= rhs.index); }

  ALWAYS_INLINE void Clear()
  {
    cpu_handle = {};
    gpu_handle = {};
    index = INVALID_INDEX;
  }
};

class D3D12DescriptorHeapManager final
{
public:
  D3D12DescriptorHeapManager();
  ~D3D12DescriptorHeapManager();

  ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptor_heap.Get(); }
  u32 GetDescriptorIncrementSize() const { return m_descriptor_increment_size; }

  bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors, bool shader_visible,
              Error* error);
  void Destroy();

  bool Allocate(D3D12DescriptorHandle* handle);
  void Free(D3D12DescriptorHandle* handle);
  void Free(u32 index);

private:
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
  u32 m_num_descriptors = 0;
  u32 m_descriptor_increment_size = 0;
  bool m_shader_visible = false;

  D3D12_CPU_DESCRIPTOR_HANDLE m_heap_base_cpu = {};
  D3D12_GPU_DESCRIPTOR_HANDLE m_heap_base_gpu = {};

  static constexpr u32 BITSET_SIZE = 1024;
  using BitSetType = std::bitset<BITSET_SIZE>;
  std::vector<BitSetType> m_free_slots = {};
};

class D3D12DescriptorAllocator
{
public:
  D3D12DescriptorAllocator();
  ~D3D12DescriptorAllocator();

  ALWAYS_INLINE ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptor_heap.Get(); }
  ALWAYS_INLINE u32 GetDescriptorIncrementSize() const { return m_descriptor_increment_size; }

  bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors, Error* error);
  void Destroy();

  bool Allocate(u32 num_handles, D3D12DescriptorHandle* out_base_handle);
  void Reset();

private:
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
  u32 m_descriptor_increment_size = 0;
  u32 m_num_descriptors = 0;
  u32 m_current_offset = 0;

  D3D12_CPU_DESCRIPTOR_HANDLE m_heap_base_cpu = {};
  D3D12_GPU_DESCRIPTOR_HANDLE m_heap_base_gpu = {};
};

template<u32 NumSamplers>
class D3D12GroupedSamplerAllocator : private D3D12DescriptorAllocator
{
  struct Key
  {
    u32 idx[NumSamplers];

    ALWAYS_INLINE bool operator==(const Key& rhs) const { return (std::memcmp(idx, rhs.idx, sizeof(idx)) == 0); }
    ALWAYS_INLINE bool operator!=(const Key& rhs) const { return (std::memcmp(idx, rhs.idx, sizeof(idx)) != 0); }
  };

  struct KeyHash
  {
    ALWAYS_INLINE std::size_t operator()(const Key& key) const
    {
      size_t seed = 0;
      for (u32 i : key.idx)
        hash_combine(seed, i);
      return seed;
    }
  };

public:
  D3D12GroupedSamplerAllocator();
  ~D3D12GroupedSamplerAllocator();

  using D3D12DescriptorAllocator::GetDescriptorHeap;
  using D3D12DescriptorAllocator::GetDescriptorIncrementSize;

  bool Create(ID3D12Device* device, u32 num_descriptors, Error* error);
  void Destroy();

  bool LookupSingle(ID3D12Device* device, D3D12DescriptorHandle* gpu_handle, const D3D12DescriptorHandle& cpu_handle);
  bool LookupGroup(ID3D12Device* device, D3D12DescriptorHandle* gpu_handle, const D3D12DescriptorHandle* cpu_handles);

  // Clears cache but doesn't reset allocator.
  void InvalidateCache();

  void Reset();
  bool ShouldReset() const;

private:
  std::unordered_map<Key, D3D12DescriptorHandle, KeyHash> m_groups;
};

template<u32 NumSamplers>
D3D12GroupedSamplerAllocator<NumSamplers>::D3D12GroupedSamplerAllocator() = default;

template<u32 NumSamplers>
D3D12GroupedSamplerAllocator<NumSamplers>::~D3D12GroupedSamplerAllocator() = default;

template<u32 NumSamplers>
bool D3D12GroupedSamplerAllocator<NumSamplers>::Create(ID3D12Device* device, u32 num_descriptors, Error* error)
{
  return D3D12DescriptorAllocator::Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, num_descriptors, error);
}

template<u32 NumSamplers>
void D3D12GroupedSamplerAllocator<NumSamplers>::Destroy()
{
  D3D12DescriptorAllocator::Destroy();
}

template<u32 NumSamplers>
void D3D12GroupedSamplerAllocator<NumSamplers>::Reset()
{
  m_groups.clear();
  D3D12DescriptorAllocator::Reset();
}

template<u32 NumSamplers>
void D3D12GroupedSamplerAllocator<NumSamplers>::InvalidateCache()
{
  m_groups.clear();
}

template<u32 NumSamplers>
bool D3D12GroupedSamplerAllocator<NumSamplers>::LookupSingle(ID3D12Device* device, D3D12DescriptorHandle* gpu_handle,
                                                             const D3D12DescriptorHandle& cpu_handle)
{
  Key key;
  key.idx[0] = cpu_handle.index;
  for (u32 i = 1; i < NumSamplers; i++)
    key.idx[i] = 0;

  auto it = m_groups.find(key);
  if (it != m_groups.end())
  {
    *gpu_handle = it->second;
    return true;
  }

  if (!Allocate(1, gpu_handle))
    return false;

  device->CopyDescriptorsSimple(1, *gpu_handle, cpu_handle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  m_groups.emplace(key, *gpu_handle);
  return true;
}

template<u32 NumSamplers>
bool D3D12GroupedSamplerAllocator<NumSamplers>::LookupGroup(ID3D12Device* device, D3D12DescriptorHandle* gpu_handle,
                                                            const D3D12DescriptorHandle* cpu_handles)
{
  Key key;
  for (u32 i = 0; i < NumSamplers; i++)
    key.idx[i] = cpu_handles[i].index;

  auto it = m_groups.find(key);
  if (it != m_groups.end())
  {
    *gpu_handle = it->second;
    return true;
  }

  if (!Allocate(NumSamplers, gpu_handle))
    return false;

  D3D12_CPU_DESCRIPTOR_HANDLE dst_handle = *gpu_handle;
  UINT dst_size = NumSamplers;
  D3D12_CPU_DESCRIPTOR_HANDLE src_handles[NumSamplers];
  UINT src_sizes[NumSamplers];
  for (u32 i = 0; i < NumSamplers; i++)
  {
    src_handles[i] = cpu_handles[i];
    src_sizes[i] = 1;
  }
  device->CopyDescriptors(1, &dst_handle, &dst_size, NumSamplers, src_handles, src_sizes,
                          D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

  m_groups.emplace(key, *gpu_handle);
  return true;
}

template<u32 NumSamplers>
bool D3D12GroupedSamplerAllocator<NumSamplers>::ShouldReset() const
{
  // We only reset the sampler heap if more than half of the descriptors are used.
  // This saves descriptor copying when there isn't a large number of sampler configs per frame.
  return m_groups.size() >= (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE / 2);
}
