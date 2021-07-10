// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "../types.h"
#include "../windows_headers.h"
#include <bitset>
#include <d3d12.h>
#include <map>
#include <vector>
#include <wrl/client.h>

namespace D3D12 {
// This class provides an abstraction for D3D12 descriptor heaps.
struct DescriptorHandle final
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

  ALWAYS_INLINE void Clear()
  {
    cpu_handle = {};
    gpu_handle = {};
    index = INVALID_INDEX;
  }
};

class DescriptorHeapManager final
{
public:
  DescriptorHeapManager();
  ~DescriptorHeapManager();

  ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptor_heap.Get(); }
  u32 GetDescriptorIncrementSize() const { return m_descriptor_increment_size; }

  bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors, bool shader_visible);
  void Destroy();

  bool Allocate(DescriptorHandle* handle);
  void Free(DescriptorHandle* handle);
  void Free(u32 index);

private:
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
  u32 m_num_descriptors = 0;
  u32 m_descriptor_increment_size = 0;

  D3D12_CPU_DESCRIPTOR_HANDLE m_heap_base_cpu = {};
  D3D12_GPU_DESCRIPTOR_HANDLE m_heap_base_gpu = {};

  static constexpr u32 BITSET_SIZE = 1024;
  using BitSetType = std::bitset<BITSET_SIZE>;
  std::vector<BitSetType> m_free_slots = {};
};

} // namespace D3D12
