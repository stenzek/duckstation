// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "d3d12_descriptor_heap_manager.h"
#include "d3d12_stream_buffer.h"
#include "gpu_device.h"
#include "gpu_texture.h"

#include <d3d12.h>
#include <limits>
#include <memory>

namespace D3D12MA {
class Allocation;
}

class D3D12Device;

class D3D12Texture final : public GPUTexture
{
  friend D3D12Device;

public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  ~D3D12Texture() override;

  void Destroy(bool defer);

  ALWAYS_INLINE const D3D12DescriptorHandle& GetSRVDescriptor() const { return m_srv_descriptor; }
  ALWAYS_INLINE const D3D12DescriptorHandle& GetWriteDescriptor() const { return m_write_descriptor; }
  ALWAYS_INLINE const D3D12DescriptorHandle& GetUAVDescriptor() const { return m_uav_descriptor; }
  ALWAYS_INLINE D3D12_RESOURCE_STATES GetResourceState() const { return m_resource_state; }
  ALWAYS_INLINE DXGI_FORMAT GetDXGIFormat() const { return m_dxgi_format; }
  ALWAYS_INLINE ID3D12Resource* GetResource() const { return m_resource.Get(); }

  bool Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer = 0, u32 level = 0) override;
  bool Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer = 0, u32 level = 0) override;
  void Unmap() override;
  void GenerateMipmaps() override;
  void MakeReadyForSampling() override;

#ifdef ENABLE_GPU_OBJECT_NAMES
  void SetDebugName(std::string_view name) override;
#endif

  void TransitionToState(D3D12_RESOURCE_STATES state);
  void CommitClear();
  void CommitClear(ID3D12GraphicsCommandList* cmdlist);

  static u32 CalculateSubresource(u32 layer, u32 level, u32 num_levels);
  u32 CalculateSubresource(u32 layer, u32 level) const;

  void TransitionToState(ID3D12GraphicsCommandList* cmdlist, D3D12_RESOURCE_STATES state);
  void TransitionSubresourceToState(ID3D12GraphicsCommandList* cmdlist, u32 layer, u32 level,
                                    D3D12_RESOURCE_STATES before_state, D3D12_RESOURCE_STATES after_state) const;
  void TransitionSubresourceToState(ID3D12GraphicsCommandList* cmdlist, u32 subresource,
                                    D3D12_RESOURCE_STATES before_state, D3D12_RESOURCE_STATES after_state) const;
  static void TransitionSubresourceToState(ID3D12GraphicsCommandList* cmdlist, ID3D12Resource* resource,
                                           u32 subresource, D3D12_RESOURCE_STATES before_state,
                                           D3D12_RESOURCE_STATES after_state);

  // Call when the texture is bound to the pipeline, or read from in a copy.
  ALWAYS_INLINE void SetUseFenceValue(u64 counter) { m_use_fence_counter = counter; }

private:
  enum class WriteDescriptorType : u8
  {
    None,
    RTV,
    DSV
  };

  D3D12Texture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format, Flags flags,
               DXGI_FORMAT dxgi_format, ComPtr<ID3D12Resource> resource, ComPtr<D3D12MA::Allocation> allocation,
               const D3D12DescriptorHandle& srv_descriptor, const D3D12DescriptorHandle& write_descriptor,
               const D3D12DescriptorHandle& uav_descriptor, WriteDescriptorType wdtype,
               D3D12_RESOURCE_STATES resource_state);

  ID3D12GraphicsCommandList4* GetCommandBufferForUpdate();
  ID3D12Resource* AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 width, u32 height,
                                              u32 buffer_size) const;
  void ActuallyCommitClear(ID3D12GraphicsCommandList* cmdlist);

  ComPtr<ID3D12Resource> m_resource;
  ComPtr<D3D12MA::Allocation> m_allocation;

  D3D12DescriptorHandle m_srv_descriptor = {};
  D3D12DescriptorHandle m_write_descriptor = {};
  D3D12DescriptorHandle m_uav_descriptor = {};

  DXGI_FORMAT m_dxgi_format = DXGI_FORMAT_UNKNOWN;
  D3D12_RESOURCE_STATES m_resource_state = D3D12_RESOURCE_STATE_COMMON;
  WriteDescriptorType m_write_descriptor_type = WriteDescriptorType::None;

  // Contains the fence counter when the texture was last used.
  // When this matches the current fence counter, the texture was used this command buffer.
  u64 m_use_fence_counter = 0;

  u16 m_map_x = 0;
  u16 m_map_y = 0;
  u16 m_map_width = 0;
  u16 m_map_height = 0;
  u8 m_map_layer = 0;
  u8 m_map_level = 0;
};

class D3D12Sampler final : public GPUSampler
{
  friend D3D12Device;

public:
  ~D3D12Sampler() override;

  ALWAYS_INLINE const D3D12DescriptorHandle& GetDescriptor() const { return m_descriptor; }

#ifdef ENABLE_GPU_OBJECT_NAMES
  void SetDebugName(std::string_view name) override;
#endif

  static D3D12_SAMPLER_DESC GetD3DSamplerDesc(const GPUSampler::Config& config);

private:
  D3D12Sampler(D3D12DescriptorHandle descriptor);

  D3D12DescriptorHandle m_descriptor;
};

class D3D12TextureBuffer final : public GPUTextureBuffer
{
  friend D3D12Device;

public:
  D3D12TextureBuffer(Format format, u32 size_in_elements);
  ~D3D12TextureBuffer() override;

  ALWAYS_INLINE const D3D12DescriptorHandle& GetDescriptor() const { return m_descriptor; }

  bool Create(D3D12Device& dev, Error* error);
  void Destroy(bool defer);

  // Inherited via GPUTextureBuffer
  void* Map(u32 required_elements) override;
  void Unmap(u32 used_elements) override;

#ifdef ENABLE_GPU_OBJECT_NAMES
  void SetDebugName(std::string_view name) override;
#endif

private:
  D3D12StreamBuffer m_buffer;
  D3D12DescriptorHandle m_descriptor;
};

class D3D12DownloadTexture final : public GPUDownloadTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  ~D3D12DownloadTexture() override;

  static std::unique_ptr<D3D12DownloadTexture> Create(u32 width, u32 height, GPUTexture::Format format, Error* error);

  void CopyFromTexture(u32 dst_x, u32 dst_y, GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height,
                       u32 src_layer, u32 src_level, bool use_transfer_pitch) override;

  bool Map(u32 x, u32 y, u32 width, u32 height) override;
  void Unmap() override;

  void Flush() override;

#ifdef ENABLE_GPU_OBJECT_NAMES
  void SetDebugName(std::string_view name) override;
#endif

private:
  D3D12DownloadTexture(u32 width, u32 height, GPUTexture::Format format, ComPtr<D3D12MA::Allocation> allocation,
                       ComPtr<ID3D12Resource> buffer, size_t buffer_size);

  ComPtr<D3D12MA::Allocation> m_allocation;
  ComPtr<ID3D12Resource> m_buffer;

  u64 m_copy_fence_value = 0;
  size_t m_buffer_size = 0;
};
