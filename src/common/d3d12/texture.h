#pragma once
#include "../gpu_texture.h"
#include "../windows_headers.h"
#include "descriptor_heap_manager.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace D3D12 {

class StreamBuffer;

class Texture final : public GPUTexture
{
public:
  template<typename T>
  using ComPtr = Microsoft::WRL::ComPtr<T>;

  Texture();
  Texture(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);
  Texture(Texture&& texture);
  Texture(const Texture&) = delete;
  ~Texture();

  static DXGI_FORMAT GetDXGIFormat(Format format);
  static Format LookupBaseFormat(DXGI_FORMAT dformat);

  ALWAYS_INLINE ID3D12Resource* GetResource() const { return m_resource.Get(); }
  ALWAYS_INLINE const DescriptorHandle& GetSRVDescriptor() const { return m_srv_descriptor; }
  ALWAYS_INLINE const DescriptorHandle& GetRTVOrDSVDescriptor() const { return m_rtv_or_dsv_descriptor; }
  ALWAYS_INLINE D3D12_RESOURCE_STATES GetState() const { return m_state; }
  ALWAYS_INLINE DXGI_FORMAT GetDXGIFormat() const { return GetDXGIFormat(m_format); }

  ALWAYS_INLINE operator ID3D12Resource*() const { return m_resource.Get(); }
  ALWAYS_INLINE operator bool() const { return static_cast<bool>(m_resource); }

  bool IsValid() const override;

  bool Create(u32 width, u32 height, u32 layers, u32 levels, u32 samples, DXGI_FORMAT format, DXGI_FORMAT srv_format, DXGI_FORMAT rtv_format,
              DXGI_FORMAT dsv_format, D3D12_RESOURCE_FLAGS flags);
  bool Adopt(ComPtr<ID3D12Resource> texture, DXGI_FORMAT srv_format, DXGI_FORMAT rtv_format, DXGI_FORMAT dsv_format,
             D3D12_RESOURCE_STATES state);

  D3D12_RESOURCE_DESC GetDesc() const;

  void Destroy(bool defer = true);

  void TransitionToState(D3D12_RESOURCE_STATES state) const;

  Texture& operator=(const Texture&) = delete;
  Texture& operator=(Texture&& texture);

  bool BeginStreamUpdate(u32 x, u32 y, u32 width, u32 height, void** out_data, u32* out_data_pitch);
  void EndStreamUpdate(u32 x, u32 y, u32 width, u32 height);

  bool LoadData(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch);

  static void CopyToUploadBuffer(const void* src_data, u32 src_pitch, u32 height, void* dst_data, u32 dst_pitch);
  void CopyFromBuffer(u32 x, u32 y, u32 width, u32 height, u32 pitch, ID3D12Resource* buffer, u32 buffer_offset);

private:
  static bool CreateSRVDescriptor(ID3D12Resource* resource, DXGI_FORMAT format, bool multisampled,
                                  DescriptorHandle* dh);
  static bool CreateRTVDescriptor(ID3D12Resource* resource, DXGI_FORMAT format, bool multisampled,
                                  DescriptorHandle* dh);
  static bool CreateDSVDescriptor(ID3D12Resource* resource, DXGI_FORMAT format, bool multisampled,
                                  DescriptorHandle* dh);

  ComPtr<ID3D12Resource> m_resource;
  DescriptorHandle m_srv_descriptor = {};
  DescriptorHandle m_rtv_or_dsv_descriptor = {};

  mutable D3D12_RESOURCE_STATES m_state = D3D12_RESOURCE_STATE_COMMON;

  bool m_is_depth_view = false;
};

} // namespace D3D12