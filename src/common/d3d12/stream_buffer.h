// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "../types.h"
#include "../windows_headers.h"
#include <d3d12.h>
#include <deque>
#include <utility>
#include <wrl/client.h>

namespace D3D12 {
class StreamBuffer
{
public:
  StreamBuffer();
  ~StreamBuffer();

  bool Create(u32 size);

  ALWAYS_INLINE bool IsValid() const { return static_cast<bool>(m_buffer); }
  ALWAYS_INLINE ID3D12Resource* GetBuffer() const { return m_buffer.Get(); }
  ALWAYS_INLINE D3D12_GPU_VIRTUAL_ADDRESS GetGPUPointer() const { return m_gpu_pointer; }
  ALWAYS_INLINE void* GetHostPointer() const { return m_host_pointer; }
  ALWAYS_INLINE void* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
  ALWAYS_INLINE D3D12_GPU_VIRTUAL_ADDRESS GetCurrentGPUPointer() const { return m_gpu_pointer + m_current_offset; }
  ALWAYS_INLINE u32 GetSize() const { return m_size; }
  ALWAYS_INLINE u32 GetCurrentOffset() const { return m_current_offset; }
  ALWAYS_INLINE u32 GetCurrentSpace() const { return m_current_space; }

  bool ReserveMemory(u32 num_bytes, u32 alignment);
  void CommitMemory(u32 final_num_bytes);

  void Destroy(bool defer = true);

private:
  void UpdateCurrentFencePosition();
  void UpdateGPUPosition();

  // Waits for as many fences as needed to allocate num_bytes bytes from the buffer.
  bool WaitForClearSpace(u32 num_bytes);

  u32 m_size = 0;
  u32 m_current_offset = 0;
  u32 m_current_space = 0;
  u32 m_current_gpu_position = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
  D3D12_GPU_VIRTUAL_ADDRESS m_gpu_pointer = {};
  u8* m_host_pointer = nullptr;

  // List of fences and the corresponding positions in the buffer
  std::deque<std::pair<u64, u32>> m_tracked_fences;
};

} // namespace D3D12
