// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "d3d12_texture.h"
#include "d3d12_builders.h"
#include "d3d12_device.h"
#include "d3d_common.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/string_util.h"

#include "D3D12MemAlloc.h"

Log_SetChannel(D3D12Device);

D3D12Texture::D3D12Texture(u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type, Format format,
                           DXGI_FORMAT dxgi_format, ComPtr<ID3D12Resource> resource,
                           ComPtr<D3D12MA::Allocation> allocation, const D3D12DescriptorHandle& srv_descriptor,
                           const D3D12DescriptorHandle& write_descriptor, const D3D12DescriptorHandle& uav_descriptor,
                           WriteDescriptorType wdtype, D3D12_RESOURCE_STATES resource_state)
  : GPUTexture(static_cast<u16>(width), static_cast<u16>(height), static_cast<u8>(layers), static_cast<u8>(levels),
               static_cast<u8>(samples), type, format),
    m_resource(std::move(resource)), m_allocation(std::move(allocation)), m_srv_descriptor(srv_descriptor),
    m_write_descriptor(write_descriptor), m_uav_descriptor(uav_descriptor), m_dxgi_format(dxgi_format),
    m_resource_state(resource_state), m_write_descriptor_type(wdtype)
{
}

D3D12Texture::~D3D12Texture()
{
  Destroy(true);
}

std::unique_ptr<GPUTexture> D3D12Device::CreateTexture(u32 width, u32 height, u32 layers, u32 levels, u32 samples,
                                                       GPUTexture::Type type, GPUTexture::Format format,
                                                       const void* data /* = nullptr */, u32 data_stride /* = 0 */)
{
  if (!GPUTexture::ValidateConfig(width, height, layers, levels, samples, type, format))
    return {};

  const D3DCommon::DXGIFormatMapping& fm = D3DCommon::GetFormatMapping(format);

  const DXGI_FORMAT uav_format = (type == GPUTexture::Type::RWTexture) ? fm.resource_format : DXGI_FORMAT_UNKNOWN;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = static_cast<u8>(levels);
  desc.Format = fm.resource_format;
  desc.SampleDesc.Count = samples;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

  D3D12MA::ALLOCATION_DESC allocationDesc = {};
  allocationDesc.Flags = D3D12MA::ALLOCATION_FLAG_WITHIN_BUDGET;
  allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_CLEAR_VALUE optimized_clear_value = {};
  D3D12_RESOURCE_STATES state;

  switch (type)
  {
    case GPUTexture::Type::Texture:
    case GPUTexture::Type::DynamicTexture:
    {
      desc.Flags = D3D12_RESOURCE_FLAG_NONE;
      state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    break;

    case GPUTexture::Type::RenderTarget:
    {
      // RT's tend to be larger, so we'll keep them committed for speed.
      DebugAssert(levels == 1);
      allocationDesc.Flags |= D3D12MA::ALLOCATION_FLAG_COMMITTED;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      optimized_clear_value.Format = fm.rtv_format;
      state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    break;

    case GPUTexture::Type::DepthStencil:
    {
      DebugAssert(levels == 1);
      allocationDesc.Flags |= D3D12MA::ALLOCATION_FLAG_COMMITTED;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      optimized_clear_value.Format = fm.dsv_format;
      state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    break;

    case GPUTexture::Type::RWTexture:
    {
      DebugAssert(levels == 1);
      allocationDesc.Flags |= D3D12MA::ALLOCATION_FLAG_COMMITTED;
      state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    break;

    default:
      return {};
  }

  if (uav_format != DXGI_FORMAT_UNKNOWN)
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  ComPtr<ID3D12Resource> resource;
  ComPtr<D3D12MA::Allocation> allocation;
  HRESULT hr = m_allocator->CreateResource(
    &allocationDesc, &desc, state,
    (type == GPUTexture::Type::RenderTarget || type == GPUTexture::Type::DepthStencil) ? &optimized_clear_value :
                                                                                         nullptr,
    allocation.GetAddressOf(), IID_PPV_ARGS(resource.GetAddressOf()));
  if (FAILED(hr))
  {
    // OOM isn't fatal.
    if (hr != E_OUTOFMEMORY)
      Log_ErrorPrintf("Create texture failed: 0x%08X", hr);

    return {};
  }

  D3D12DescriptorHandle srv_descriptor, write_descriptor, uav_descriptor;
  D3D12Texture::WriteDescriptorType write_descriptor_type = D3D12Texture::WriteDescriptorType::None;
  if (fm.srv_format != DXGI_FORMAT_UNKNOWN)
  {
    if (!CreateSRVDescriptor(resource.Get(), layers, levels, samples, fm.srv_format, &srv_descriptor))
      return {};
  }

  switch (type)
  {
    case GPUTexture::Type::RenderTarget:
    {
      write_descriptor_type = D3D12Texture::WriteDescriptorType::RTV;
      if (!CreateRTVDescriptor(resource.Get(), samples, fm.rtv_format, &write_descriptor))
      {
        m_descriptor_heap_manager.Free(&srv_descriptor);
        return {};
      }
    }
    break;

    case GPUTexture::Type::DepthStencil:
    {
      write_descriptor_type = D3D12Texture::WriteDescriptorType::DSV;
      if (!CreateDSVDescriptor(resource.Get(), samples, fm.dsv_format, &write_descriptor))
      {
        m_descriptor_heap_manager.Free(&srv_descriptor);
        return {};
      }
    }
    break;

    default:
      break;
  }

  if (uav_format != DXGI_FORMAT_UNKNOWN &&
      !CreateUAVDescriptor(resource.Get(), samples, fm.dsv_format, &uav_descriptor))
  {
    m_descriptor_heap_manager.Free(&write_descriptor);
    m_descriptor_heap_manager.Free(&srv_descriptor);
    return {};
  }

  std::unique_ptr<D3D12Texture> tex(new D3D12Texture(
    width, height, layers, levels, samples, type, format, fm.resource_format, std::move(resource),
    std::move(allocation), srv_descriptor, write_descriptor, uav_descriptor, write_descriptor_type, state));

  if (data)
  {
    tex->Update(0, 0, width, height, data, data_stride);
    tex->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  return tex;
}

bool D3D12Device::CreateSRVDescriptor(ID3D12Resource* resource, u32 layers, u32 levels, u32 samples, DXGI_FORMAT format,
                                      D3D12DescriptorHandle* dh)
{
  if (!m_descriptor_heap_manager.Allocate(dh))
  {
    Log_ErrorPrint("Failed to allocate SRV descriptor");
    return false;
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = format;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

  if (layers > 1)
  {
    if (samples > 1)
    {
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
      desc.Texture2DMSArray = {0u, layers};
    }
    else
    {
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      desc.Texture2DArray = {0u, levels, 0u, layers, 0u, 0.0f};
    }
  }
  else
  {
    if (samples > 1)
    {
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    }
    else
    {
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      desc.Texture2D = {0u, levels, 0u, 0.0f};
    }
  }

  m_device->CreateShaderResourceView(resource, &desc, dh->cpu_handle);
  return true;
}

bool D3D12Device::CreateRTVDescriptor(ID3D12Resource* resource, u32 samples, DXGI_FORMAT format,
                                      D3D12DescriptorHandle* dh)
{
  if (!m_rtv_heap_manager.Allocate(dh))
  {
    Log_ErrorPrint("Failed to allocate SRV descriptor");
    return false;
  }

  const D3D12_RENDER_TARGET_VIEW_DESC desc = {
    format, (samples > 1) ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D, {}};
  m_device->CreateRenderTargetView(resource, &desc, dh->cpu_handle);
  return true;
}

bool D3D12Device::CreateDSVDescriptor(ID3D12Resource* resource, u32 samples, DXGI_FORMAT format,
                                      D3D12DescriptorHandle* dh)
{
  if (!m_dsv_heap_manager.Allocate(dh))
  {
    Log_ErrorPrint("Failed to allocate SRV descriptor");
    return false;
  }

  const D3D12_DEPTH_STENCIL_VIEW_DESC desc = {
    format, (samples > 1) ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAG_NONE, {}};
  m_device->CreateDepthStencilView(resource, &desc, dh->cpu_handle);
  return true;
}

bool D3D12Device::CreateUAVDescriptor(ID3D12Resource* resource, u32 samples, DXGI_FORMAT format,
                                      D3D12DescriptorHandle* dh)
{
  if (!m_descriptor_heap_manager.Allocate(dh))
  {
    Log_ErrorPrint("Failed to allocate UAV descriptor");
    return false;
  }

  DebugAssert(samples == 1);
  const D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {format, D3D12_UAV_DIMENSION_TEXTURE2D, {}};
  m_device->CreateUnorderedAccessView(resource, nullptr, &desc, dh->cpu_handle);
  return true;
}

void D3D12Texture::Destroy(bool defer)
{
  D3D12Device& dev = D3D12Device::GetInstance();
  dev.UnbindTexture(this);

  if (defer)
  {
    dev.DeferDescriptorDestruction(dev.GetDescriptorHeapManager(), &m_srv_descriptor);

    switch (m_write_descriptor_type)
    {
      case WriteDescriptorType::RTV:
        dev.DeferDescriptorDestruction(dev.GetRTVHeapManager(), &m_write_descriptor);
        break;
      case WriteDescriptorType::DSV:
        dev.DeferDescriptorDestruction(dev.GetDSVHeapManager(), &m_write_descriptor);
        break;
      case WriteDescriptorType::None:
      default:
        break;
    }

    if (m_uav_descriptor)
      dev.DeferDescriptorDestruction(dev.GetDescriptorHeapManager(), &m_uav_descriptor);

    dev.DeferResourceDestruction(std::move(m_allocation), std::move(m_resource));
  }
  else
  {
    dev.GetDescriptorHeapManager().Free(&m_srv_descriptor);

    switch (m_write_descriptor_type)
    {
      case WriteDescriptorType::RTV:
        dev.GetRTVHeapManager().Free(&m_write_descriptor);
        break;
      case WriteDescriptorType::DSV:
        dev.GetDSVHeapManager().Free(&m_write_descriptor);
        break;
      case WriteDescriptorType::None:
      default:
        break;
    }

    if (m_uav_descriptor)
      dev.GetDescriptorHeapManager().Free(&m_uav_descriptor);

    m_resource.Reset();
    m_allocation.Reset();
  }

  m_write_descriptor_type = WriteDescriptorType::None;
}

ID3D12GraphicsCommandList4* D3D12Texture::GetCommandBufferForUpdate()
{
  D3D12Device& dev = D3D12Device::GetInstance();
  if ((m_type != Type::Texture && m_type != Type::DynamicTexture) || m_use_fence_counter == dev.GetCurrentFenceValue())
  {
    // Console.WriteLn("Texture update within frame, can't use do beforehand");
    if (dev.InRenderPass())
      dev.EndRenderPass();
    return dev.GetCommandList();
  }

  return dev.GetInitCommandList();
}

void D3D12Texture::CopyTextureDataForUpload(void* dst, const void* src, u32 width, u32 height, u32 pitch,
                                            u32 upload_pitch) const
{
  StringUtil::StrideMemCpy(dst, upload_pitch, src, pitch, GetPixelSize() * width, height);
}

ID3D12Resource* D3D12Texture::AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 width,
                                                          u32 height) const
{
  const u32 size = upload_pitch * height;
  ComPtr<ID3D12Resource> resource;
  ComPtr<D3D12MA::Allocation> allocation;

  const D3D12MA::ALLOCATION_DESC allocation_desc = {D3D12MA::ALLOCATION_FLAG_NONE, D3D12_HEAP_TYPE_UPLOAD,
                                                    D3D12_HEAP_FLAG_NONE, nullptr, nullptr};
  const D3D12_RESOURCE_DESC resource_desc = {
    D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    D3D12_RESOURCE_FLAG_NONE};
  HRESULT hr = D3D12Device::GetInstance().GetAllocator()->CreateResource(
    &allocation_desc, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, allocation.GetAddressOf(),
    IID_PPV_ARGS(resource.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateResource() failed with %08X", hr);
    return nullptr;
  }

  void* map_ptr;
  hr = resource->Map(0, nullptr, &map_ptr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map() failed with %08X", hr);
    return nullptr;
  }

  CopyTextureDataForUpload(map_ptr, data, width, height, pitch, upload_pitch);

  const D3D12_RANGE write_range = {0, size};
  resource->Unmap(0, &write_range);

  // Immediately queue it for freeing after the command buffer finishes, since it's only needed for the copy.
  // This adds the reference needed to keep the buffer alive.
  ID3D12Resource* ret = resource.Get();
  D3D12Device::GetInstance().DeferResourceDestruction(std::move(allocation), std::move(resource));
  return ret;
}

bool D3D12Texture::Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer, u32 level)
{
  DebugAssert(layer < m_layers && level < m_levels);
  DebugAssert((x + width) <= GetMipWidth(level) && (y + height) <= GetMipHeight(level));

  D3D12Device& dev = D3D12Device::GetInstance();
  D3D12StreamBuffer& sbuffer = dev.GetTextureUploadBuffer();

  const u32 upload_pitch = Common::AlignUpPow2<u32>(pitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 required_size = height * upload_pitch;

  D3D12_TEXTURE_COPY_LOCATION srcloc;
  srcloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  srcloc.PlacedFootprint.Footprint.Width = width;
  srcloc.PlacedFootprint.Footprint.Height = height;
  srcloc.PlacedFootprint.Footprint.Depth = 1;
  srcloc.PlacedFootprint.Footprint.Format = m_dxgi_format;
  srcloc.PlacedFootprint.Footprint.RowPitch = upload_pitch;

  // If the texture is larger than half our streaming buffer size, use a separate buffer.
  // Otherwise allocation will either fail, or require lots of cmdbuffer submissions.
  if (required_size > (sbuffer.GetSize() / 2))
  {
    srcloc.pResource = AllocateUploadStagingBuffer(data, pitch, upload_pitch, width, height);
    if (!srcloc.pResource)
      return false;

    srcloc.PlacedFootprint.Offset = 0;
  }
  else
  {
    if (!sbuffer.ReserveMemory(required_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
    {
      D3D12Device::GetInstance().SubmitCommandList(false, "While waiting for %u bytes in texture upload buffer",
                                                   required_size);
      if (!sbuffer.ReserveMemory(required_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
      {
        Log_ErrorPrintf("Failed to reserve texture upload memory (%u bytes).", required_size);
        return false;
      }
    }

    srcloc.pResource = sbuffer.GetBuffer();
    srcloc.PlacedFootprint.Offset = sbuffer.GetCurrentOffset();
    CopyTextureDataForUpload(sbuffer.GetCurrentHostPointer(), data, width, height, pitch, upload_pitch);
    sbuffer.CommitMemory(required_size);
  }

  ID3D12GraphicsCommandList4* cmdlist = GetCommandBufferForUpdate();

  // if we're an rt and have been cleared, and the full rect isn't being uploaded, do the clear
  if (m_type == Type::RenderTarget)
  {
    if (x != 0 || y != 0 || width != m_width || height != m_height)
      CommitClear(cmdlist);
    else
      m_state = State::Dirty;
  }

  GPUDevice::GetStatistics().buffer_streamed += required_size;
  GPUDevice::GetStatistics().num_uploads++;

  // first time the texture is used? don't leave it undefined
  if (m_resource_state == D3D12_RESOURCE_STATE_COMMON)
    TransitionToState(cmdlist, D3D12_RESOURCE_STATE_COPY_DEST);
  else if (m_resource_state != D3D12_RESOURCE_STATE_COPY_DEST)
    TransitionSubresourceToState(cmdlist, layer, level, m_resource_state, D3D12_RESOURCE_STATE_COPY_DEST);

  D3D12_TEXTURE_COPY_LOCATION dstloc;
  dstloc.pResource = m_resource.Get();
  dstloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dstloc.SubresourceIndex = layer;

  const D3D12_BOX srcbox{0u, 0u, 0u, width, height, 1u};
  cmdlist->CopyTextureRegion(&dstloc, x, y, 0, &srcloc, &srcbox);

  if (m_resource_state != D3D12_RESOURCE_STATE_COPY_DEST)
    TransitionSubresourceToState(cmdlist, layer, level, D3D12_RESOURCE_STATE_COPY_DEST, m_resource_state);

  return true;
}

bool D3D12Texture::Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer, u32 level)
{
  // TODO: linear textures for dynamic?
  if ((x + width) > GetMipWidth(level) || (y + height) > GetMipHeight(level) || layer > m_layers || level > m_levels)
  {
    return false;
  }

  D3D12Device& dev = D3D12Device::GetInstance();
  if (m_state == State::Cleared && (x != 0 || y != 0 || width != m_width || height != m_height))
    CommitClear(GetCommandBufferForUpdate());

  // see note in Update() for the reason why.
  const u32 aligned_pitch = Common::AlignUpPow2(width * GetPixelSize(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 req_size = height * aligned_pitch;
  D3D12StreamBuffer& buffer = dev.GetTextureUploadBuffer();
  if (req_size >= (buffer.GetSize() / 2))
    return false;

  if (!buffer.ReserveMemory(req_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
  {
    dev.SubmitCommandList(false, "While waiting for %u bytes in texture upload buffer", req_size);
    if (!buffer.ReserveMemory(req_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
      Panic("Failed to reserve texture upload memory");
  }

  // map for writing
  *map = buffer.GetCurrentHostPointer();
  *map_stride = aligned_pitch;
  m_map_x = static_cast<u16>(x);
  m_map_y = static_cast<u16>(y);
  m_map_width = static_cast<u16>(width);
  m_map_height = static_cast<u16>(height);
  m_map_layer = static_cast<u8>(layer);
  m_map_level = static_cast<u8>(level);
  m_state = State::Dirty;
  return true;
}

void D3D12Texture::Unmap()
{
  D3D12Device& dev = D3D12Device::GetInstance();
  D3D12StreamBuffer& sb = dev.GetTextureUploadBuffer();
  const u32 aligned_pitch = Common::AlignUpPow2(m_map_width * GetPixelSize(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 req_size = m_map_height * aligned_pitch;
  const u32 offset = sb.GetCurrentOffset();
  sb.CommitMemory(req_size);

  GPUDevice::GetStatistics().buffer_streamed += req_size;
  GPUDevice::GetStatistics().num_uploads++;

  ID3D12GraphicsCommandList4* cmdlist = GetCommandBufferForUpdate();

  // first time the texture is used? don't leave it undefined
  if (m_resource_state == D3D12_RESOURCE_STATE_COMMON)
    TransitionToState(cmdlist, D3D12_RESOURCE_STATE_COPY_DEST);
  else if (m_resource_state != D3D12_RESOURCE_STATE_COPY_DEST)
    TransitionSubresourceToState(cmdlist, m_map_layer, m_map_level, m_resource_state, D3D12_RESOURCE_STATE_COPY_DEST);

  D3D12_TEXTURE_COPY_LOCATION srcloc;
  srcloc.pResource = sb.GetBuffer();
  srcloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  srcloc.PlacedFootprint.Offset = offset;
  srcloc.PlacedFootprint.Footprint.Width = m_map_width;
  srcloc.PlacedFootprint.Footprint.Height = m_map_height;
  srcloc.PlacedFootprint.Footprint.Depth = 1;
  srcloc.PlacedFootprint.Footprint.Format = m_dxgi_format;
  srcloc.PlacedFootprint.Footprint.RowPitch = aligned_pitch;

  D3D12_TEXTURE_COPY_LOCATION dstloc;
  dstloc.pResource = m_resource.Get();
  dstloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dstloc.SubresourceIndex = m_map_level;

  const D3D12_BOX srcbox{0u, 0u, 0u, m_map_width, m_map_height, 1};
  cmdlist->CopyTextureRegion(&dstloc, m_map_x, m_map_y, 0, &srcloc, &srcbox);

  if (m_resource_state != D3D12_RESOURCE_STATE_COPY_DEST)
    TransitionSubresourceToState(cmdlist, m_map_layer, m_map_level, D3D12_RESOURCE_STATE_COPY_DEST, m_resource_state);

  m_map_x = 0;
  m_map_y = 0;
  m_map_width = 0;
  m_map_height = 0;
  m_map_layer = 0;
  m_map_level = 0;
}

void D3D12Texture::CommitClear()
{
  if (m_state != GPUTexture::State::Cleared)
    return;

  D3D12Device& dev = D3D12Device::GetInstance();
  if (dev.InRenderPass())
    dev.EndRenderPass();

  ActuallyCommitClear(dev.GetCommandList());
}

void D3D12Texture::CommitClear(ID3D12GraphicsCommandList* cmdlist)
{
  if (m_state != GPUTexture::State::Cleared)
    return;

  ActuallyCommitClear(cmdlist);
}

void D3D12Texture::ActuallyCommitClear(ID3D12GraphicsCommandList* cmdlist)
{
  if (IsDepthStencil())
  {
    TransitionToState(cmdlist, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdlist->ClearDepthStencilView(GetWriteDescriptor(), D3D12_CLEAR_FLAG_DEPTH, m_clear_value.depth, 0, 0, nullptr);
  }
  else
  {
    TransitionToState(cmdlist, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdlist->ClearRenderTargetView(GetWriteDescriptor(), D3D12Device::RGBA8ToFloat(m_clear_value.color).data(), 0,
                                   nullptr);
  }

  SetState(State::Dirty);
}

void D3D12Texture::SetDebugName(const std::string_view& name)
{
  D3D12::SetObjectName(m_resource.Get(), name);
}

u32 D3D12Texture::CalculateSubresource(u32 layer, u32 level, u32 num_levels)
{
  // D3D11CalcSubresource
  return level + layer * num_levels;
}

u32 D3D12Texture::CalculateSubresource(u32 layer, u32 level) const
{
  return CalculateSubresource(layer, level, m_levels);
}

void D3D12Texture::TransitionToState(D3D12_RESOURCE_STATES state)
{
  TransitionToState(D3D12Device::GetInstance().GetCommandList(), state);
}

void D3D12Texture::TransitionToState(ID3D12GraphicsCommandList* cmdlist, D3D12_RESOURCE_STATES state)
{
  if (m_resource_state == state)
    return;

  const D3D12_RESOURCE_STATES prev_state = m_resource_state;
  m_resource_state = state;

  const D3D12_RESOURCE_BARRIER barrier = {
    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    D3D12_RESOURCE_BARRIER_FLAG_NONE,
    {{m_resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, prev_state, state}}};
  cmdlist->ResourceBarrier(1, &barrier);
}

void D3D12Texture::TransitionSubresourceToState(ID3D12GraphicsCommandList* cmdlist, u32 layer, u32 level,
                                                D3D12_RESOURCE_STATES before_state,
                                                D3D12_RESOURCE_STATES after_state) const
{
  TransitionSubresourceToState(cmdlist, m_resource.Get(), CalculateSubresource(layer, level), before_state,
                               after_state);
}

void D3D12Texture::TransitionSubresourceToState(ID3D12GraphicsCommandList* cmdlist, u32 subresource,
                                                D3D12_RESOURCE_STATES before_state,
                                                D3D12_RESOURCE_STATES after_state) const
{
  TransitionSubresourceToState(cmdlist, m_resource.Get(), subresource, before_state, after_state);
}

void D3D12Texture::TransitionSubresourceToState(ID3D12GraphicsCommandList* cmdlist, ID3D12Resource* resource,
                                                u32 subresource, D3D12_RESOURCE_STATES before_state,
                                                D3D12_RESOURCE_STATES after_state)
{
  const D3D12_RESOURCE_BARRIER barrier = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                          D3D12_RESOURCE_BARRIER_FLAG_NONE,
                                          {{resource, subresource, before_state, after_state}}};
  cmdlist->ResourceBarrier(1, &barrier);
}

void D3D12Texture::MakeReadyForSampling()
{
  if (m_resource_state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    return;

  D3D12Device& dev = D3D12Device::GetInstance();
  if (dev.InRenderPass())
    dev.EndRenderPass();

  TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool D3D12Device::DownloadTexture(GPUTexture* texture, u32 x, u32 y, u32 width, u32 height, void* out_data,
                                  u32 out_data_stride)
{
  D3D12Texture* T = static_cast<D3D12Texture*>(texture);
  T->CommitClear();

  const u32 pitch = Common::AlignUp(width * T->GetPixelSize(), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 size = pitch * height;
  const u32 subresource = 0;
  if (!CheckDownloadBufferSize(size))
  {
    Log_ErrorPrintf("Can't read back %ux%u", width, height);
    return false;
  }

  if (InRenderPass())
    EndRenderPass();

  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();

  D3D12_TEXTURE_COPY_LOCATION srcloc;
  srcloc.pResource = T->GetResource();
  srcloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcloc.SubresourceIndex = subresource;

  D3D12_TEXTURE_COPY_LOCATION dstloc;
  dstloc.pResource = m_download_buffer.Get();
  dstloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstloc.PlacedFootprint.Offset = 0;
  dstloc.PlacedFootprint.Footprint.Format = T->GetDXGIFormat();
  dstloc.PlacedFootprint.Footprint.Width = width;
  dstloc.PlacedFootprint.Footprint.Height = height;
  dstloc.PlacedFootprint.Footprint.Depth = 1;
  dstloc.PlacedFootprint.Footprint.RowPitch = pitch;

  const D3D12_RESOURCE_STATES old_layout = T->GetResourceState();
  if (old_layout != D3D12_RESOURCE_STATE_COPY_SOURCE)
    T->TransitionSubresourceToState(cmdlist, subresource, old_layout, D3D12_RESOURCE_STATE_COPY_SOURCE);

  // TODO: Rules for depth buffers here?
  const D3D12_BOX srcbox{static_cast<UINT>(x),         static_cast<UINT>(y),          0u,
                         static_cast<UINT>(x + width), static_cast<UINT>(y + height), 1u};
  cmdlist->CopyTextureRegion(&dstloc, 0, 0, 0, &srcloc, &srcbox);

  if (old_layout != D3D12_RESOURCE_STATE_COPY_SOURCE)
    T->TransitionSubresourceToState(cmdlist, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE, old_layout);

  SubmitCommandList(true);

  u8* map_pointer;
  const D3D12_RANGE read_range{0u, size};
  const HRESULT hr = m_download_buffer->Map(0, &read_range, reinterpret_cast<void**>(const_cast<u8**>(&map_pointer)));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map() failed with HRESULT %08X", hr);
    return false;
  }

  StringUtil::StrideMemCpy(out_data, out_data_stride, map_pointer, pitch, width * T->GetPixelSize(), height);
  m_download_buffer->Unmap(0, nullptr);
  return true;
}

bool D3D12Device::CheckDownloadBufferSize(u32 required_size)
{
  if (m_download_buffer_size >= required_size)
    return true;

  DestroyDownloadBuffer();

  D3D12MA::ALLOCATION_DESC allocation_desc = {};
  allocation_desc.HeapType = D3D12_HEAP_TYPE_READBACK;

  const D3D12_RESOURCE_DESC resource_desc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                             0,
                                             required_size,
                                             1,
                                             1,
                                             1,
                                             DXGI_FORMAT_UNKNOWN,
                                             {1, 0},
                                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                             D3D12_RESOURCE_FLAG_NONE};

  HRESULT hr = m_allocator->CreateResource(&allocation_desc, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           m_download_buffer_allocation.ReleaseAndGetAddressOf(),
                                           IID_PPV_ARGS(m_download_buffer.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateResource() failed with HRESULT %08X", hr);
    return false;
  }

  return true;
}

void D3D12Device::DestroyDownloadBuffer()
{
  if (!m_download_buffer)
    return;

  m_download_buffer.Reset();
  m_download_buffer_allocation.Reset();
  m_download_buffer_size = 0;
}

D3D12Sampler::D3D12Sampler(D3D12DescriptorHandle descriptor) : m_descriptor(descriptor)
{
}

D3D12Sampler::~D3D12Sampler()
{
  // Cleaned up by main class.
}

void D3D12Sampler::SetDebugName(const std::string_view& name)
{
}

D3D12DescriptorHandle D3D12Device::GetSampler(const GPUSampler::Config& config)
{
  const auto it = m_sampler_map.find(config.key);
  if (it != m_sampler_map.end())
    return it->second;

  static constexpr std::array<D3D12_TEXTURE_ADDRESS_MODE, static_cast<u8>(GPUSampler::AddressMode::MaxCount)> ta = {{
    D3D12_TEXTURE_ADDRESS_MODE_WRAP,   // Repeat
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // ClampToEdge
    D3D12_TEXTURE_ADDRESS_MODE_BORDER, // ClampToBorder
    D3D12_TEXTURE_ADDRESS_MODE_MIRROR, // MirrorRepeat
  }};

  static constexpr u8 filter_count = static_cast<u8>(GPUSampler::Filter::MaxCount);
  static constexpr D3D12_FILTER filters[filter_count][filter_count][filter_count] = {
    {
      {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT},
      {D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT},
    },
    {
      {D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR, D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR},
      {D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR, D3D12_FILTER_MIN_MAG_MIP_LINEAR},
    }};

  D3D12_SAMPLER_DESC desc = {};
  desc.AddressU = ta[static_cast<u8>(config.address_u.GetValue())];
  desc.AddressV = ta[static_cast<u8>(config.address_v.GetValue())];
  desc.AddressW = ta[static_cast<u8>(config.address_w.GetValue())];
  std::memcpy(desc.BorderColor, RGBA8ToFloat(config.border_color).data(), sizeof(desc.BorderColor));
  desc.MinLOD = static_cast<float>(config.min_lod);
  desc.MaxLOD = static_cast<float>(config.max_lod);

  if (config.anisotropy > 1)
  {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = config.anisotropy;
  }
  else
  {
    desc.Filter = filters[static_cast<u8>(config.mip_filter.GetValue())][static_cast<u8>(config.min_filter.GetValue())]
                         [static_cast<u8>(config.mag_filter.GetValue())];
    desc.MaxAnisotropy = 1;
  }

  D3D12DescriptorHandle handle;
  if (m_sampler_heap_manager.Allocate(&handle))
    m_device->CreateSampler(&desc, handle);

  m_sampler_map.emplace(config.key, handle);
  return handle;
}

void D3D12Device::DestroySamplers()
{
  for (auto& it : m_sampler_map)
  {
    if (it.second)
      m_sampler_heap_manager.Free(&it.second);
  }
  m_sampler_map.clear();
}

std::unique_ptr<GPUSampler> D3D12Device::CreateSampler(const GPUSampler::Config& config)
{
  const D3D12DescriptorHandle handle = GetSampler(config);
  if (!handle)
    return {};

  return std::unique_ptr<GPUSampler>(new D3D12Sampler(std::move(handle)));
}

D3D12TextureBuffer::D3D12TextureBuffer(Format format, u32 size_in_elements) : GPUTextureBuffer(format, size_in_elements)
{
}

D3D12TextureBuffer::~D3D12TextureBuffer()
{
  Destroy(true);
}

bool D3D12TextureBuffer::Create(D3D12Device& dev)
{
  static constexpr std::array<DXGI_FORMAT, static_cast<u8>(GPUTextureBuffer::Format::MaxCount)> format_mapping = {{
    DXGI_FORMAT_R16_UINT, // R16UI
  }};

  if (!m_buffer.Create(GetSizeInBytes()))
    return false;

  if (!dev.GetDescriptorHeapManager().Allocate(&m_descriptor))
    return {};

  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {format_mapping[static_cast<u8>(m_format)],
                                          D3D12_SRV_DIMENSION_BUFFER,
                                          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                          {}};
  desc.Buffer.NumElements = m_size_in_elements;
  dev.GetDevice()->CreateShaderResourceView(m_buffer.GetBuffer(), &desc, m_descriptor);
  return true;
}

void D3D12TextureBuffer::Destroy(bool defer)
{
  D3D12Device& dev = D3D12Device::GetInstance();
  if (m_descriptor)
  {
    if (defer)
      dev.DeferDescriptorDestruction(dev.GetDescriptorHeapManager(), &m_descriptor);
    else
      dev.GetDescriptorHeapManager().Free(&m_descriptor);
  }
}

void* D3D12TextureBuffer::Map(u32 required_elements)
{
  const u32 esize = GetElementSize(m_format);
  const u32 req_size = esize * required_elements;
  if (!m_buffer.ReserveMemory(req_size, esize))
  {
    D3D12Device::GetInstance().SubmitCommandListAndRestartRenderPass("out of space in texture buffer");
    if (!m_buffer.ReserveMemory(req_size, esize))
      Panic("Failed to allocate texture buffer space.");
  }

  m_current_position = m_buffer.GetCurrentOffset() / esize;
  return m_buffer.GetCurrentHostPointer();
}

void D3D12TextureBuffer::Unmap(u32 used_elements)
{
  const u32 size = GetElementSize(m_format) * used_elements;
  GPUDevice::GetStatistics().buffer_streamed += size;
  GPUDevice::GetStatistics().num_uploads++;
  m_buffer.CommitMemory(size);
}

void D3D12TextureBuffer::SetDebugName(const std::string_view& name)
{
  D3D12::SetObjectName(m_buffer.GetBuffer(), name);
}

std::unique_ptr<GPUTextureBuffer> D3D12Device::CreateTextureBuffer(GPUTextureBuffer::Format format,
                                                                   u32 size_in_elements)
{

  std::unique_ptr<D3D12TextureBuffer> tb = std::make_unique<D3D12TextureBuffer>(format, size_in_elements);
  if (!tb->Create(*this))
    tb.reset();

  return tb;
}
