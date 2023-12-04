// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "d3d12_device.h"
#include "d3d12_builders.h"
#include "d3d12_pipeline.h"
#include "d3d12_stream_buffer.h"
#include "d3d12_texture.h"
#include "d3d_common.h"

#include "core/host.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bitutils.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/scoped_guard.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "D3D12MemAlloc.h"
#include "fmt/format.h"

#include <limits>
#include <mutex>

Log_SetChannel(D3D12Device);

// Tweakables
enum : u32
{
  MAX_DRAW_CALLS_PER_FRAME = 2048,
  MAX_DESCRIPTORS_PER_FRAME = 32768,
  MAX_SAMPLERS_PER_FRAME = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE,
  MAX_DESCRIPTOR_SETS_PER_FRAME = MAX_DRAW_CALLS_PER_FRAME,

  MAX_PERSISTENT_DESCRIPTORS = 2048,
  MAX_PERSISTENT_RTVS = 512,
  MAX_PERSISTENT_DSVS = 128,
  MAX_PERSISTENT_SAMPLERS = 512,

  VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
  INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
  VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
  FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
  TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024,

  // UNIFORM_PUSH_CONSTANTS_STAGES = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
  UNIFORM_PUSH_CONSTANTS_SIZE = 128,

  MAX_UNIFORM_BUFFER_SIZE = 1024,
};

// We need to synchronize instance creation because of adapter enumeration from the UI thread.
static std::mutex s_instance_mutex;

static constexpr D3D12_CLEAR_VALUE s_present_clear_color = {DXGI_FORMAT_R8G8B8A8_UNORM, {{0.0f, 0.0f, 0.0f, 1.0f}}};
static constexpr GPUTexture::Format s_swap_chain_format = GPUTexture::Format::RGBA8;

// We just need to keep this alive, never reference it.
static std::vector<u8> s_pipeline_cache_data;

#ifdef _DEBUG
#include "WinPixEventRuntime/pix3.h"
static u32 s_debug_scope_depth = 0;
#endif

D3D12Device::D3D12Device()
{
#ifdef _DEBUG
  s_debug_scope_depth = 0;
#endif
}

D3D12Device::~D3D12Device()
{
  Assert(!m_device);
  Assert(s_pipeline_cache_data.empty());
}

D3D12Device::ComPtr<ID3DBlob> D3D12Device::SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc)
{
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error_blob;
  const HRESULT hr =
    D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error_blob.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("D3D12SerializeRootSignature() failed: %08X", hr);
    if (error_blob)
      Log_ErrorPrintf("%s", error_blob->GetBufferPointer());

    return {};
  }

  return blob;
}

D3D12Device::ComPtr<ID3D12RootSignature> D3D12Device::CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc)
{
  ComPtr<ID3DBlob> blob = SerializeRootSignature(desc);
  if (!blob)
    return {};

  ComPtr<ID3D12RootSignature> rs;
  const HRESULT hr =
    m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(rs.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateRootSignature() failed: %08X", hr);
    return {};
  }

  return rs;
}

bool D3D12Device::CreateDevice(const std::string_view& adapter, bool threaded_presentation,
                               FeatureMask disabled_features)
{
  std::unique_lock lock(s_instance_mutex);

  m_dxgi_factory = D3DCommon::CreateFactory(m_debug_device);
  if (!m_dxgi_factory)
    return false;

  m_adapter = D3DCommon::GetAdapterByName(m_dxgi_factory.Get(), adapter);

  HRESULT hr;

  // Enabling the debug layer will fail if the Graphics Tools feature is not installed.
  if (m_debug_device)
  {
    ComPtr<ID3D12Debug> debug12;
    hr = D3D12GetDebugInterface(IID_PPV_ARGS(debug12.GetAddressOf()));
    if (SUCCEEDED(hr))
    {
      debug12->EnableDebugLayer();
    }
    else
    {
      Log_ErrorPrintf("Debug layer requested but not available.");
      m_debug_device = false;
    }
  }

  // Create the actual device.
  m_feature_level = D3D_FEATURE_LEVEL_11_0;
  hr = D3D12CreateDevice(m_adapter.Get(), m_feature_level, IID_PPV_ARGS(&m_device));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create D3D12 device: %08X", hr);
    return false;
  }

  if (!m_adapter)
  {
    const LUID luid(m_device->GetAdapterLuid());
    if (FAILED(m_dxgi_factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(m_adapter.GetAddressOf()))))
      Log_ErrorPrintf("Failed to get lookup adapter by device LUID");
  }

  if (m_debug_device)
  {
    ComPtr<ID3D12InfoQueue> info_queue;
    if (SUCCEEDED(m_device.As(&info_queue)))
    {
      if (IsDebuggerPresent())
      {
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
      }

      D3D12_INFO_QUEUE_FILTER filter = {};
      std::array<D3D12_MESSAGE_ID, 6> id_list{
        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
        D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_TYPE_MISMATCH,
        D3D12_MESSAGE_ID_DRAW_EMPTY_SCISSOR_RECTANGLE,
        D3D12_MESSAGE_ID_LOADPIPELINE_NAMENOTFOUND,
      };
      filter.DenyList.NumIDs = static_cast<UINT>(id_list.size());
      filter.DenyList.pIDList = id_list.data();
      info_queue->PushStorageFilter(&filter);
    }
  }

  const D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                                               D3D12_COMMAND_QUEUE_FLAG_NONE, 0u};
  hr = m_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_command_queue));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create command queue: %08X", hr);
    return false;
  }

  D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
  allocatorDesc.pDevice = m_device.Get();
  allocatorDesc.pAdapter = m_adapter.Get();
  allocatorDesc.Flags =
    D3D12MA::ALLOCATOR_FLAG_SINGLETHREADED |
    D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED /* | D3D12MA::ALLOCATOR_FLAG_ALWAYS_COMMITTED*/;

  hr = D3D12MA::CreateAllocator(&allocatorDesc, m_allocator.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("D3D12MA::CreateAllocator() failed with HRESULT %08X", hr);
    return false;
  }

  hr = m_device->CreateFence(m_completed_fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create fence: %08X", hr);
    return false;
  }

  m_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (m_fence_event == NULL)
  {
    Log_ErrorPrintf("Failed to create fence event: %08X", GetLastError());
    return false;
  }

  SetFeatures(disabled_features);

  if (!CreateCommandLists() || !CreateDescriptorHeaps())
    return false;

  if (!m_window_info.IsSurfaceless() && !CreateSwapChain())
    return false;

  if (!CreateRootSignatures() || !CreateBuffers())
    return false;

  CreateTimestampQuery();
  return true;
}

void D3D12Device::DestroyDevice()
{
  std::unique_lock lock(s_instance_mutex);

  // Toss command list if we're recording...
  if (InRenderPass())
    EndRenderPass();

  WaitForGPUIdle();

  DestroyDeferredObjects(m_current_fence_value);
  DestroyDownloadBuffer();
  DestroySamplers();
  DestroyTimestampQuery();
  DestroyBuffers();
  DestroyDescriptorHeaps();
  DestroyRootSignatures();
  DestroySwapChain();
  DestroyCommandLists();

  m_pipeline_library.Reset();
  std::vector<u8>().swap(s_pipeline_cache_data);
  m_fence.Reset();
  if (m_fence_event != NULL)
  {
    CloseHandle(m_fence_event);
    m_fence_event = NULL;
  }

  m_allocator.Reset();
  m_command_queue.Reset();
  m_device.Reset();
  m_adapter.Reset();
  m_dxgi_factory.Reset();
}

bool D3D12Device::ReadPipelineCache(const std::string& filename)
{
  std::optional<std::vector<u8>> data;

  auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "rb");
  if (fp)
    data = FileSystem::ReadBinaryFile(fp.get());

  const HRESULT hr =
    m_device->CreatePipelineLibrary(data.has_value() ? data->data() : nullptr, data.has_value() ? data->size() : 0,
                                    IID_PPV_ARGS(m_pipeline_library.ReleaseAndGetAddressOf()));
  if (FAILED(hr))
    Log_WarningPrintf("CreatePipelineLibrary() failed with HRESULT %08X, pipeline caching will not be available.", hr);
  else if (data.has_value())
    s_pipeline_cache_data = std::move(data.value());

  return SUCCEEDED(hr);
}

bool D3D12Device::GetPipelineCacheData(DynamicHeapArray<u8>* data)
{
  if (!m_pipeline_library)
    return false;

  const size_t size = m_pipeline_library->GetSerializedSize();
  if (size == 0)
  {
    Log_WarningPrintf("Empty serialized pipeline state returned.");
    return false;
  }

  data->resize(size);
  const HRESULT hr = m_pipeline_library->Serialize(data->data(), data->size());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Serialize() failed with HRESULT %08X", hr);
    data->deallocate();
    return false;
  }

  return true;
}

bool D3D12Device::CreateCommandLists()
{
  for (u32 i = 0; i < NUM_COMMAND_LISTS; i++)
  {
    CommandList& res = m_command_lists[i];
    HRESULT hr;

    for (u32 j = 0; j < 2; j++)
    {
      hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(res.command_allocators[j].GetAddressOf()));
      if (FAILED(hr))
      {
        Log_ErrorPrintf("CreateCommandAllocator() failed: %08X", hr);
        return false;
      }

      hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, res.command_allocators[j].Get(), nullptr,
                                       IID_PPV_ARGS(res.command_lists[j].GetAddressOf()));
      if (FAILED(hr))
      {
        Log_ErrorPrintf("CreateCommandList() failed: %08X", hr);
        return false;
      }

      // Close the command lists, since the first thing we do is reset them.
      hr = res.command_lists[j]->Close();
      if (FAILED(hr))
      {
        Log_ErrorPrintf("Close() failed: %08X", hr);
        return false;
      }
    }

    if (!res.descriptor_allocator.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         MAX_DESCRIPTORS_PER_FRAME))
    {
      Log_ErrorPrintf("Failed to create per frame descriptor allocator");
      return false;
    }

    if (!res.sampler_allocator.Create(m_device.Get(), MAX_SAMPLERS_PER_FRAME))
    {
      Log_ErrorPrintf("Failed to create per frame sampler allocator");
      return false;
    }
  }

  MoveToNextCommandList();
  return true;
}

void D3D12Device::MoveToNextCommandList()
{
  m_current_command_list = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
  m_current_fence_value++;

  // We may have to wait if this command list hasn't finished on the GPU.
  CommandList& res = m_command_lists[m_current_command_list];
  WaitForFence(res.fence_counter);
  res.fence_counter = m_current_fence_value;
  res.init_list_used = false;

  // Begin command list.
  res.command_allocators[1]->Reset();
  res.command_lists[1]->Reset(res.command_allocators[1].Get(), nullptr);
  res.descriptor_allocator.Reset();
  if (res.sampler_allocator.ShouldReset())
    res.sampler_allocator.Reset();

  if (res.has_timestamp_query)
  {
    // readback timestamp from the last time this cmdlist was used.
    // we don't need to worry about disjoint in dx12, the frequency is reliable within a single cmdlist.
    const u32 offset = (m_current_command_list * (sizeof(u64) * NUM_TIMESTAMP_QUERIES_PER_CMDLIST));
    const D3D12_RANGE read_range = {offset, offset + (sizeof(u64) * NUM_TIMESTAMP_QUERIES_PER_CMDLIST)};
    void* map;
    HRESULT hr = m_timestamp_query_buffer->Map(0, &read_range, &map);
    if (SUCCEEDED(hr))
    {
      u64 timestamps[2];
      std::memcpy(timestamps, static_cast<const u8*>(map) + offset, sizeof(timestamps));
      m_accumulated_gpu_time +=
        static_cast<float>(static_cast<double>(timestamps[1] - timestamps[0]) / m_timestamp_frequency);

      const D3D12_RANGE write_range = {};
      m_timestamp_query_buffer->Unmap(0, &write_range);
    }
    else
    {
      Log_WarningPrintf("Map() for timestamp query failed: %08X", hr);
    }
  }

  res.has_timestamp_query = m_gpu_timing_enabled;
  if (m_gpu_timing_enabled)
  {
    res.command_lists[1]->EndQuery(m_timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                   m_current_command_list * NUM_TIMESTAMP_QUERIES_PER_CMDLIST);
  }

  ID3D12DescriptorHeap* heaps[2] = {res.descriptor_allocator.GetDescriptorHeap(),
                                    res.sampler_allocator.GetDescriptorHeap()};
  res.command_lists[1]->SetDescriptorHeaps(static_cast<UINT>(std::size(heaps)), heaps);

  m_allocator->SetCurrentFrameIndex(static_cast<UINT>(m_current_fence_value));
  InvalidateCachedState();
}

void D3D12Device::DestroyCommandLists()
{
  for (CommandList& resources : m_command_lists)
  {
    resources.descriptor_allocator.Destroy();
    resources.sampler_allocator.Destroy();
    for (u32 i = 0; i < 2; i++)
    {
      resources.command_lists[i].Reset();
      resources.command_allocators[i].Reset();
    }
  }
}

bool D3D12Device::CreateDescriptorHeaps()
{
  if (!m_descriptor_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                        MAX_PERSISTENT_DESCRIPTORS, false) ||
      !m_rtv_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_PERSISTENT_RTVS, false) ||
      !m_dsv_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MAX_PERSISTENT_DSVS, false) ||
      !m_sampler_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, MAX_PERSISTENT_SAMPLERS,
                                     false))
  {
    return false;
  }

  // Allocate null SRV descriptor for unbound textures.
  static constexpr D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc = {
    DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};

  if (!m_descriptor_heap_manager.Allocate(&m_null_srv_descriptor))
  {
    Log_ErrorPrint("Failed to allocate null descriptor");
    return false;
  }

  m_device->CreateShaderResourceView(nullptr, &null_srv_desc, m_null_srv_descriptor.cpu_handle);

  // Same for samplers.
  m_point_sampler = GetSampler(GPUSampler::GetNearestConfig());
  for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
    m_current_samplers[i] = m_point_sampler;
  return true;
}

void D3D12Device::DestroyDescriptorHeaps()
{
  if (m_null_srv_descriptor)
    m_descriptor_heap_manager.Free(&m_null_srv_descriptor);
  m_sampler_heap_manager.Destroy();
  m_dsv_heap_manager.Destroy();
  m_rtv_heap_manager.Destroy();
  m_descriptor_heap_manager.Destroy();
}

ID3D12GraphicsCommandList4* D3D12Device::GetInitCommandList()
{
  CommandList& res = m_command_lists[m_current_command_list];
  if (!res.init_list_used)
  {
    HRESULT hr = res.command_allocators[0]->Reset();
    AssertMsg(SUCCEEDED(hr), "Reset init command allocator failed");

    hr = res.command_lists[0]->Reset(res.command_allocators[0].Get(), nullptr);
    AssertMsg(SUCCEEDED(hr), "Reset init command list failed");
    res.init_list_used = true;
  }

  return res.command_lists[0].Get();
}

void D3D12Device::SubmitCommandList(bool wait_for_completion)
{
  CommandList& res = m_command_lists[m_current_command_list];
  HRESULT hr;

  if (res.has_timestamp_query)
  {
    // write the timestamp back at the end of the cmdlist
    res.command_lists[1]->EndQuery(m_timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                   (m_current_command_list * NUM_TIMESTAMP_QUERIES_PER_CMDLIST) + 1);
    res.command_lists[1]->ResolveQueryData(m_timestamp_query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                           m_current_command_list * NUM_TIMESTAMP_QUERIES_PER_CMDLIST,
                                           NUM_TIMESTAMP_QUERIES_PER_CMDLIST, m_timestamp_query_buffer.Get(),
                                           m_current_command_list * (sizeof(u64) * NUM_TIMESTAMP_QUERIES_PER_CMDLIST));
  }

  // TODO: error handling
  if (res.init_list_used)
  {
    hr = res.command_lists[0]->Close();
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Closing init command list failed with HRESULT %08X", hr);
      Panic("TODO cannot continue");
    }
  }

  // Close and queue command list.
  hr = res.command_lists[1]->Close();
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Closing main command list failed with HRESULT %08X", hr);
    Panic("TODO cannot continue");
  }

  if (res.init_list_used)
  {
    const std::array<ID3D12CommandList*, 2> execute_lists{res.command_lists[0].Get(), res.command_lists[1].Get()};
    m_command_queue->ExecuteCommandLists(static_cast<UINT>(execute_lists.size()), execute_lists.data());
  }
  else
  {
    const std::array<ID3D12CommandList*, 1> execute_lists{res.command_lists[1].Get()};
    m_command_queue->ExecuteCommandLists(static_cast<UINT>(execute_lists.size()), execute_lists.data());
  }

  // Update fence when GPU has completed.
  hr = m_command_queue->Signal(m_fence.Get(), res.fence_counter);
  DebugAssertMsg(SUCCEEDED(hr), "Signal fence");

  MoveToNextCommandList();

  if (wait_for_completion)
    WaitForFence(res.fence_counter);
}

void D3D12Device::SubmitCommandList(bool wait_for_completion, const char* reason, ...)
{
  std::va_list ap;
  va_start(ap, reason);
  const std::string reason_str(StringUtil::StdStringFromFormatV(reason, ap));
  va_end(ap);

  Log_WarningPrintf("Executing command buffer due to '%s'", reason_str.c_str());
  SubmitCommandList(wait_for_completion);
}

void D3D12Device::SubmitCommandListAndRestartRenderPass(const char* reason)
{
  if (InRenderPass())
    EndRenderPass();

  D3D12Pipeline* pl = m_current_pipeline;
  SubmitCommandList(false, "%s", reason);

  SetPipeline(pl);
  BeginRenderPass();
}

void D3D12Device::WaitForFence(u64 fence)
{
  if (m_completed_fence_value >= fence)
    return;

  // Try non-blocking check.
  m_completed_fence_value = m_fence->GetCompletedValue();
  if (m_completed_fence_value < fence)
  {
    // Fall back to event.
    HRESULT hr = m_fence->SetEventOnCompletion(fence, m_fence_event);
    AssertMsg(SUCCEEDED(hr), "Set fence event on completion");
    WaitForSingleObject(m_fence_event, INFINITE);
    m_completed_fence_value = m_fence->GetCompletedValue();
  }

  // Release resources for as many command lists which have completed.
  DestroyDeferredObjects(m_completed_fence_value);
}

void D3D12Device::WaitForGPUIdle()
{
  u32 index = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
  for (u32 i = 0; i < (NUM_COMMAND_LISTS - 1); i++)
  {
    WaitForFence(m_command_lists[index].fence_counter);
    index = (index + 1) % NUM_COMMAND_LISTS;
  }
}

bool D3D12Device::CreateTimestampQuery()
{
  constexpr u32 QUERY_COUNT = NUM_TIMESTAMP_QUERIES_PER_CMDLIST * NUM_COMMAND_LISTS;
  constexpr u32 BUFFER_SIZE = sizeof(u64) * QUERY_COUNT;

  const D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, QUERY_COUNT, 0u};
  HRESULT hr = m_device->CreateQueryHeap(&desc, IID_PPV_ARGS(m_timestamp_query_heap.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateQueryHeap() for timestamp failed with %08X", hr);
    m_features.gpu_timing = false;
    return false;
  }

  const D3D12MA::ALLOCATION_DESC allocation_desc = {D3D12MA::ALLOCATION_FLAG_NONE, D3D12_HEAP_TYPE_READBACK,
                                                    D3D12_HEAP_FLAG_NONE, nullptr, nullptr};
  const D3D12_RESOURCE_DESC resource_desc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                             0,
                                             BUFFER_SIZE,
                                             1,
                                             1,
                                             1,
                                             DXGI_FORMAT_UNKNOWN,
                                             {1, 0},
                                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                             D3D12_RESOURCE_FLAG_NONE};
  hr = m_allocator->CreateResource(&allocation_desc, &resource_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                   m_timestamp_query_allocation.GetAddressOf(),
                                   IID_PPV_ARGS(m_timestamp_query_buffer.GetAddressOf()));
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateResource() for timestamp failed with %08X", hr);
    m_features.gpu_timing = false;
    return false;
  }

  u64 frequency;
  hr = m_command_queue->GetTimestampFrequency(&frequency);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("GetTimestampFrequency() failed: %08X", hr);
    m_features.gpu_timing = false;
    return false;
  }

  m_timestamp_frequency = static_cast<double>(frequency) / 1000.0;
  return true;
}

void D3D12Device::DestroyTimestampQuery()
{
  m_timestamp_query_buffer.Reset();
  m_timestamp_query_allocation.Reset();
  m_timestamp_query_heap.Reset();
}

float D3D12Device::GetAndResetAccumulatedGPUTime()
{
  const float time = m_accumulated_gpu_time;
  m_accumulated_gpu_time = 0.0f;
  return time;
}

bool D3D12Device::SetGPUTimingEnabled(bool enabled)
{
  m_gpu_timing_enabled = enabled && m_features.gpu_timing;
  return (enabled == m_gpu_timing_enabled);
}

void D3D12Device::DeferObjectDestruction(ComPtr<ID3D12Object> resource)
{
  DebugAssert(resource);
  m_cleanup_resources.emplace_back(GetCurrentFenceValue(),
                                   std::pair<D3D12MA::Allocation*, ID3D12Object*>(nullptr, resource.Detach()));
}

void D3D12Device::DeferResourceDestruction(ComPtr<D3D12MA::Allocation> allocation, ComPtr<ID3D12Resource> resource)
{
  DebugAssert(allocation && resource);
  m_cleanup_resources.emplace_back(
    GetCurrentFenceValue(), std::pair<D3D12MA::Allocation*, ID3D12Object*>(allocation.Detach(), resource.Detach()));
}

void D3D12Device::DeferDescriptorDestruction(D3D12DescriptorHeapManager& heap, D3D12DescriptorHandle* descriptor)
{
  DebugAssert(descriptor->index != D3D12DescriptorHandle::INVALID_INDEX);
  m_cleanup_descriptors.emplace_back(GetCurrentFenceValue(),
                                     std::pair<D3D12DescriptorHeapManager*, D3D12DescriptorHandle>(&heap, *descriptor));
  descriptor->Clear();
}

void D3D12Device::DestroyDeferredObjects(u64 fence_value)
{
  while (!m_cleanup_descriptors.empty())
  {
    auto& it = m_cleanup_descriptors.front();
    if (it.first > fence_value)
      break;

    it.second.first->Free(it.second.second.index);
    m_cleanup_descriptors.pop_front();
  }

  while (!m_cleanup_resources.empty())
  {
    auto& it = m_cleanup_resources.front();
    if (it.first > fence_value)
      break;

    it.second.second->Release();
    if (it.second.first)
      it.second.first->Release();
    m_cleanup_resources.pop_front();
  }
}

void D3D12Device::GetAdapterAndModeList(AdapterAndModeList* ret, IDXGIFactory5* factory)
{
  ret->adapter_names = D3DCommon::GetAdapterNames(factory);
  ret->fullscreen_modes = D3DCommon::GetFullscreenModes(factory, {});
}

GPUDevice::AdapterAndModeList D3D12Device::StaticGetAdapterAndModeList()
{
  AdapterAndModeList ret;
  std::unique_lock lock(s_instance_mutex);

  // Device shouldn't be torn down since we have the lock.
  if (g_gpu_device && g_gpu_device->GetRenderAPI() == RenderAPI::D3D12)
  {
    GetAdapterAndModeList(&ret, D3D12Device::GetInstance().m_dxgi_factory.Get());
  }
  else
  {
    ComPtr<IDXGIFactory5> factory = D3DCommon::CreateFactory(false);
    if (factory)
      GetAdapterAndModeList(&ret, factory.Get());
  }

  return ret;
}

GPUDevice::AdapterAndModeList D3D12Device::GetAdapterAndModeList()
{
  AdapterAndModeList ret;
  GetAdapterAndModeList(&ret, m_dxgi_factory.Get());
  return ret;
}

RenderAPI D3D12Device::GetRenderAPI() const
{
  return RenderAPI::D3D12;
}

bool D3D12Device::HasSurface() const
{
  return static_cast<bool>(m_swap_chain);
}

bool D3D12Device::CreateSwapChain()
{
  if (m_window_info.type != WindowInfo::Type::Win32)
    return false;

  const D3DCommon::DXGIFormatMapping& fm = D3DCommon::GetFormatMapping(s_swap_chain_format);

  const HWND window_hwnd = reinterpret_cast<HWND>(m_window_info.window_handle);
  RECT client_rc{};
  GetClientRect(window_hwnd, &client_rc);

  DXGI_MODE_DESC fullscreen_mode = {};
  ComPtr<IDXGIOutput> fullscreen_output;
  if (Host::IsFullscreen())
  {
    u32 fullscreen_width, fullscreen_height;
    float fullscreen_refresh_rate;
    m_is_exclusive_fullscreen =
      GetRequestedExclusiveFullscreenMode(&fullscreen_width, &fullscreen_height, &fullscreen_refresh_rate) &&
      D3DCommon::GetRequestedExclusiveFullscreenModeDesc(m_dxgi_factory.Get(), client_rc, fullscreen_width,
                                                         fullscreen_height, fullscreen_refresh_rate, fm.resource_format,
                                                         &fullscreen_mode, fullscreen_output.GetAddressOf());
  }
  else
  {
    m_is_exclusive_fullscreen = false;
  }

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
  swap_chain_desc.Width = static_cast<u32>(client_rc.right - client_rc.left);
  swap_chain_desc.Height = static_cast<u32>(client_rc.bottom - client_rc.top);
  swap_chain_desc.Format = fm.resource_format;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.BufferCount = 3;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  m_using_allow_tearing = (m_allow_tearing_supported && !m_is_exclusive_fullscreen);
  if (m_using_allow_tearing)
    swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  HRESULT hr = S_OK;

  if (m_is_exclusive_fullscreen)
  {
    DXGI_SWAP_CHAIN_DESC1 fs_sd_desc = swap_chain_desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc = {};

    fs_sd_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    fs_sd_desc.Width = fullscreen_mode.Width;
    fs_sd_desc.Height = fullscreen_mode.Height;
    fs_desc.RefreshRate = fullscreen_mode.RefreshRate;
    fs_desc.ScanlineOrdering = fullscreen_mode.ScanlineOrdering;
    fs_desc.Scaling = fullscreen_mode.Scaling;
    fs_desc.Windowed = FALSE;

    Log_VerbosePrintf("Creating a %dx%d exclusive fullscreen swap chain", fs_sd_desc.Width, fs_sd_desc.Height);
    hr = m_dxgi_factory->CreateSwapChainForHwnd(m_command_queue.Get(), window_hwnd, &fs_sd_desc, &fs_desc,
                                                fullscreen_output.Get(), m_swap_chain.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
      Log_WarningPrint("Failed to create fullscreen swap chain, trying windowed.");
      m_is_exclusive_fullscreen = false;
      m_using_allow_tearing = m_allow_tearing_supported;
    }
  }

  if (!m_is_exclusive_fullscreen)
  {
    Log_VerbosePrintf("Creating a %dx%d windowed swap chain", swap_chain_desc.Width, swap_chain_desc.Height);
    hr = m_dxgi_factory->CreateSwapChainForHwnd(m_command_queue.Get(), window_hwnd, &swap_chain_desc, nullptr, nullptr,
                                                m_swap_chain.ReleaseAndGetAddressOf());
  }

  hr = m_dxgi_factory->MakeWindowAssociation(window_hwnd, DXGI_MWA_NO_WINDOW_CHANGES);
  if (FAILED(hr))
    Log_WarningPrint("MakeWindowAssociation() to disable ALT+ENTER failed");

  if (!CreateSwapChainRTV())
  {
    DestroySwapChain();
    return false;
  }

  // Render a frame as soon as possible to clear out whatever was previously being displayed.
  RenderBlankFrame();
  return true;
}

bool D3D12Device::CreateSwapChainRTV()
{
  DXGI_SWAP_CHAIN_DESC swap_chain_desc;
  HRESULT hr = m_swap_chain->GetDesc(&swap_chain_desc);
  if (FAILED(hr))
    return false;

  const D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {swap_chain_desc.BufferDesc.Format, D3D12_RTV_DIMENSION_TEXTURE2D, {}};

  for (u32 i = 0; i < swap_chain_desc.BufferCount; i++)
  {
    ComPtr<ID3D12Resource> backbuffer;
    hr = m_swap_chain->GetBuffer(i, IID_PPV_ARGS(backbuffer.GetAddressOf()));
    if (FAILED(hr))
    {
      Log_ErrorPrintf("GetBuffer for RTV failed: 0x%08X", hr);
      DestroySwapChainRTVs();
      return false;
    }

    D3D12::SetObjectNameFormatted(backbuffer.Get(), "Swap Chain Buffer #%u", i);

    D3D12DescriptorHandle rtv;
    if (!m_rtv_heap_manager.Allocate(&rtv))
    {
      Log_ErrorPrintf("Failed to allocate RTV handle");
      DestroySwapChainRTVs();
      return false;
    }

    m_device->CreateRenderTargetView(backbuffer.Get(), &rtv_desc, rtv);
    m_swap_chain_buffers.emplace_back(std::move(backbuffer), rtv);
  }

  m_window_info.surface_width = swap_chain_desc.BufferDesc.Width;
  m_window_info.surface_height = swap_chain_desc.BufferDesc.Height;
  m_window_info.surface_format = s_swap_chain_format;
  Log_VerbosePrintf("Swap chain buffer size: %ux%u", m_window_info.surface_width, m_window_info.surface_height);

  if (m_window_info.type == WindowInfo::Type::Win32)
  {
    BOOL fullscreen = FALSE;
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(m_swap_chain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen &&
        SUCCEEDED(m_swap_chain->GetDesc(&desc)))
    {
      m_window_info.surface_refresh_rate = static_cast<float>(desc.BufferDesc.RefreshRate.Numerator) /
                                           static_cast<float>(desc.BufferDesc.RefreshRate.Denominator);
    }
    else
    {
      m_window_info.surface_refresh_rate = 0.0f;
    }
  }

  m_current_swap_chain_buffer = 0;
  return true;
}

void D3D12Device::DestroySwapChainRTVs()
{
  // Runtime gets cranky if we don't submit the current buffer...
  if (InRenderPass())
    EndRenderPass();
  SubmitCommandList(true);

  for (auto it = m_swap_chain_buffers.rbegin(); it != m_swap_chain_buffers.rend(); ++it)
  {
    m_rtv_heap_manager.Free(it->second.index);
    it->first.Reset();
  }
  m_swap_chain_buffers.clear();
  m_current_swap_chain_buffer = 0;
}

void D3D12Device::DestroySwapChain()
{
  if (!m_swap_chain)
    return;

  DestroySwapChainRTVs();

  // switch out of fullscreen before destroying
  BOOL is_fullscreen;
  if (SUCCEEDED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) && is_fullscreen)
    m_swap_chain->SetFullscreenState(FALSE, nullptr);

  m_swap_chain.Reset();
  m_is_exclusive_fullscreen = false;
}

void D3D12Device::RenderBlankFrame()
{
  if (InRenderPass())
    EndRenderPass();

  auto& swap_chain_buf = m_swap_chain_buffers[m_current_swap_chain_buffer];
  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();
  m_current_swap_chain_buffer = ((m_current_swap_chain_buffer + 1) % static_cast<u32>(m_swap_chain_buffers.size()));
  D3D12Texture::TransitionSubresourceToState(cmdlist, swap_chain_buf.first.Get(), 0, D3D12_RESOURCE_STATE_COMMON,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET);
  cmdlist->ClearRenderTargetView(swap_chain_buf.second, s_present_clear_color.Color, 0, nullptr);
  D3D12Texture::TransitionSubresourceToState(cmdlist, swap_chain_buf.first.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             D3D12_RESOURCE_STATE_PRESENT);
  SubmitCommandList(false);
  m_swap_chain->Present(0, m_using_allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

bool D3D12Device::UpdateWindow()
{
  WaitForGPUIdle();
  DestroySwapChain();

  if (!AcquireWindow(false))
    return false;

  if (m_window_info.IsSurfaceless())
    return true;

  if (!CreateSwapChain())
  {
    Log_ErrorPrintf("Failed to create swap chain on updated window");
    return false;
  }

  RenderBlankFrame();
  return true;
}

void D3D12Device::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
  if (!m_swap_chain)
    return;

  m_window_info.surface_scale = new_window_scale;

  if (m_window_info.surface_width == static_cast<u32>(new_window_width) &&
      m_window_info.surface_height == static_cast<u32>(new_window_height))
  {
    return;
  }

  DestroySwapChainRTVs();

  HRESULT hr = m_swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                           m_using_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  if (FAILED(hr))
    Log_ErrorPrintf("ResizeBuffers() failed: 0x%08X", hr);

  if (!CreateSwapChainRTV())
    Panic("Failed to recreate swap chain RTV after resize");
}

void D3D12Device::DestroySurface()
{
  DestroySwapChainRTVs();
  DestroySwapChain();
}

bool D3D12Device::SupportsTextureFormat(GPUTexture::Format format) const
{
  constexpr u32 required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;

  const DXGI_FORMAT dfmt = D3DCommon::GetFormatMapping(format).resource_format;
  if (dfmt == DXGI_FORMAT_UNKNOWN)
    return false;

  D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {dfmt, {}, {}};
  return SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
         (support.Support1 & required) == required;
}

std::string D3D12Device::GetDriverInfo() const
{
  std::string ret = fmt::format("{} ({})\n", D3DCommon::GetFeatureLevelString(m_feature_level),
                                D3DCommon::GetFeatureLevelShaderModelString(m_feature_level));

  DXGI_ADAPTER_DESC desc;
  if (m_adapter && SUCCEEDED(m_adapter->GetDesc(&desc)))
  {
    fmt::format_to(std::back_inserter(ret), "VID: 0x{:04X} PID: 0x{:04X}\n", desc.VendorId, desc.DeviceId);
    ret += StringUtil::WideStringToUTF8String(desc.Description);
    ret += "\n";

    const std::string driver_version(D3DCommon::GetDriverVersionFromLUID(desc.AdapterLuid));
    if (!driver_version.empty())
    {
      ret += "Driver Version: ";
      ret += driver_version;
    }
  }

  return ret;
}

void D3D12Device::SetVSync(bool enabled)
{
  m_vsync_enabled = enabled;
}

bool D3D12Device::BeginPresent(bool frame_skip)
{
  if (InRenderPass())
    EndRenderPass();

  if (frame_skip)
    return false;

  // If we're running surfaceless, kick the command buffer so we don't run out of descriptors.
  if (!m_swap_chain)
  {
    SubmitCommandList(false);
    TrimTexturePool();
    return false;
  }

  // TODO: Check if the device was lost.

  // Check if we lost exclusive fullscreen. If so, notify the host, so it can switch to windowed mode.
  // This might get called repeatedly if it takes a while to switch back, that's the host's problem.
  BOOL is_fullscreen;
  if (m_is_exclusive_fullscreen &&
      (FAILED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) || !is_fullscreen))
  {
    Host::RunOnCPUThread([]() { Host::SetFullscreen(false); });
    TrimTexturePool();
    return false;
  }

  BeginSwapChainRenderPass();
  return true;
}

void D3D12Device::EndPresent()
{
  DebugAssert(InRenderPass() && m_num_current_render_targets == 0 && !m_current_depth_target);
  EndRenderPass();

  const auto& swap_chain_buf = m_swap_chain_buffers[m_current_swap_chain_buffer];
  m_current_swap_chain_buffer = ((m_current_swap_chain_buffer + 1) % static_cast<u32>(m_swap_chain_buffers.size()));

  ID3D12GraphicsCommandList* cmdlist = GetCommandList();
  D3D12Texture::TransitionSubresourceToState(cmdlist, swap_chain_buf.first.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             D3D12_RESOURCE_STATE_PRESENT);

  SubmitCommandList(false);

  if (!m_vsync_enabled && m_using_allow_tearing)
    m_swap_chain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
  else
    m_swap_chain->Present(static_cast<UINT>(m_vsync_enabled), 0);

  TrimTexturePool();
}

#ifdef _DEBUG
static UINT64 Palette(float phase, const std::array<float, 3>& a, const std::array<float, 3>& b,
                      const std::array<float, 3>& c, const std::array<float, 3>& d)
{
  std::array<float, 3> result;
  result[0] = a[0] + b[0] * std::cos(6.28318f * (c[0] * phase + d[0]));
  result[1] = a[1] + b[1] * std::cos(6.28318f * (c[1] * phase + d[1]));
  result[2] = a[2] + b[2] * std::cos(6.28318f * (c[2] * phase + d[2]));

  return PIX_COLOR(static_cast<BYTE>(std::clamp(result[0] * 255.0f, 0.0f, 255.0f)),
                   static_cast<BYTE>(std::clamp(result[1] * 255.0f, 0.0f, 255.0f)),
                   static_cast<BYTE>(std::clamp(result[2] * 255.0f, 0.0f, 255.0f)));
}
#endif

void D3D12Device::PushDebugGroup(const char* name)
{
#ifdef _DEBUG
  if (!m_debug_device)
    return;

  const UINT64 color = Palette(static_cast<float>(++s_debug_scope_depth), {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f},
                               {1.0f, 1.0f, 0.5f}, {0.8f, 0.90f, 0.30f});
  PIXBeginEvent(GetCommandList(), color, "%s", name);
#endif
}

void D3D12Device::PopDebugGroup()
{
#ifdef _DEBUG
  if (!m_debug_device)
    return;

  s_debug_scope_depth = (s_debug_scope_depth == 0) ? 0 : (s_debug_scope_depth - 1u);
  PIXEndEvent(GetCommandList());
#endif
}

void D3D12Device::InsertDebugMessage(const char* msg)
{
#ifdef _DEBUG
  if (!m_debug_device)
    return;

  PIXSetMarker(GetCommandList(), PIX_COLOR(0, 0, 0), "%s", msg);
#endif
}

void D3D12Device::SetFeatures(FeatureMask disabled_features)
{
  m_max_texture_size = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  m_max_multisamples = 1;
  for (u32 multisamples = 2; multisamples < D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; multisamples++)
  {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS fd = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(multisamples),
                                                        D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0u};

    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &fd, sizeof(fd))) &&
        fd.NumQualityLevels > 0)
    {
      m_max_multisamples = multisamples;
    }
  }

  m_features.dual_source_blend = !(disabled_features & FEATURE_MASK_DUAL_SOURCE_BLEND);
  m_features.framebuffer_fetch = false;
  m_features.per_sample_shading = true;
  m_features.noperspective_interpolation = true;
  m_features.texture_copy_to_self =
    /*!(disabled_features & FEATURE_MASK_TEXTURE_COPY_TO_SELF)*/ false; // TODO: Support with Enhanced Barriers
  m_features.supports_texture_buffers = !(disabled_features & FEATURE_MASK_TEXTURE_BUFFERS);
  m_features.texture_buffers_emulated_with_ssbo = false;
  m_features.geometry_shaders = !(disabled_features & FEATURE_MASK_GEOMETRY_SHADERS);
  m_features.partial_msaa_resolve = true;
  m_features.gpu_timing = true;
  m_features.shader_cache = true;
  m_features.pipeline_cache = true;

  BOOL allow_tearing_supported = false;
  HRESULT hr = m_dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing_supported,
                                                   sizeof(allow_tearing_supported));
  m_allow_tearing_supported = (SUCCEEDED(hr) && allow_tearing_supported == TRUE);
}

void D3D12Device::CopyTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                    GPUTexture* src, u32 src_x, u32 src_y, u32 src_layer, u32 src_level, u32 width,
                                    u32 height)
{
  D3D12Texture* const S = static_cast<D3D12Texture*>(src);
  D3D12Texture* const D = static_cast<D3D12Texture*>(dst);

  if (S->GetState() == GPUTexture::State::Cleared)
  {
    // source is cleared. if destination is a render target, we can carry the clear forward
    if (D->IsRenderTargetOrDepthStencil())
    {
      if (dst_level == 0 && dst_x == 0 && dst_y == 0 && width == D->GetWidth() && height == D->GetHeight())
      {
        // pass it forward if we're clearing the whole thing
        if (S->IsDepthStencil())
          D->SetClearDepth(S->GetClearDepth());
        else
          D->SetClearColor(S->GetClearColor());

        return;
      }

      if (D->GetState() == GPUTexture::State::Cleared)
      {
        // destination is cleared, if it's the same colour and rect, we can just avoid this entirely
        if (D->IsDepthStencil())
        {
          if (D->GetClearDepth() == S->GetClearDepth())
            return;
        }
        else
        {
          if (D->GetClearColor() == S->GetClearColor())
            return;
        }
      }
    }

    // commit the clear to the source first, then do normal copy
    S->CommitClear();
  }

  // if the destination has been cleared, and we're not overwriting the whole thing, commit the clear first
  // (the area outside of where we're copying to)
  if (D->GetState() == GPUTexture::State::Cleared &&
      (dst_level != 0 || dst_x != 0 || dst_y != 0 || width != D->GetWidth() || height != D->GetHeight()))
  {
    D->CommitClear();
  }

  // *now* we can do a normal image copy.
  if (InRenderPass())
    EndRenderPass();

  S->TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  S->SetUseFenceValue(GetCurrentFenceValue());

  D->TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);
  D->SetUseFenceValue(GetCurrentFenceValue());

  D3D12_TEXTURE_COPY_LOCATION srcloc;
  srcloc.pResource = S->GetResource();
  srcloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcloc.SubresourceIndex = S->CalculateSubresource(src_layer, src_level);

  D3D12_TEXTURE_COPY_LOCATION dstloc;
  dstloc.pResource = D->GetResource();
  dstloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dstloc.SubresourceIndex = D->CalculateSubresource(dst_layer, dst_level);

  const D3D12_BOX srcbox{static_cast<UINT>(src_x),         static_cast<UINT>(src_y),          0u,
                         static_cast<UINT>(src_x + width), static_cast<UINT>(src_y + height), 1u};
  GetCommandList()->CopyTextureRegion(&dstloc, dst_x, dst_y, 0, &srcloc, &srcbox);

  D->SetState(GPUTexture::State::Dirty);
}

void D3D12Device::ResolveTextureRegion(GPUTexture* dst, u32 dst_x, u32 dst_y, u32 dst_layer, u32 dst_level,
                                       GPUTexture* src, u32 src_x, u32 src_y, u32 width, u32 height)
{
  DebugAssert((src_x + width) <= src->GetWidth());
  DebugAssert((src_y + height) <= src->GetHeight());
  DebugAssert(src->IsMultisampled());
  DebugAssert(dst_level < dst->GetLevels() && dst_layer < dst->GetLayers());
  DebugAssert((dst_x + width) <= dst->GetMipWidth(dst_level));
  DebugAssert((dst_y + height) <= dst->GetMipHeight(dst_level));
  DebugAssert(!dst->IsMultisampled() && src->IsMultisampled());

  if (InRenderPass())
    EndRenderPass();

  D3D12Texture* D = static_cast<D3D12Texture*>(dst);
  D3D12Texture* S = static_cast<D3D12Texture*>(src);
  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();
  const u32 DSR = D->CalculateSubresource(dst_layer, dst_level);

  S->CommitClear(cmdlist);
  D->CommitClear(cmdlist);

  S->TransitionSubresourceToState(cmdlist, 0, S->GetResourceState(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  D->TransitionSubresourceToState(cmdlist, DSR, D->GetResourceState(), D3D12_RESOURCE_STATE_RESOLVE_DEST);

  if (src_x == 0 && src_y == 0 && width == src->GetWidth() && height == src->GetHeight() && dst_x == 0 && dst_y == 0 &&
      width == dst->GetMipWidth(dst_level) && height == dst->GetMipHeight(dst_level))
  {
    cmdlist->ResolveSubresource(D->GetResource(), DSR, S->GetResource(), 0, S->GetDXGIFormat());
  }
  else
  {
    D3D12_RECT src_rc{static_cast<LONG>(src_x), static_cast<LONG>(src_y), static_cast<LONG>(src_x + width),
                      static_cast<LONG>(src_y + height)};
    cmdlist->ResolveSubresourceRegion(D->GetResource(), D->CalculateSubresource(dst_level, dst_layer), dst_x, dst_y,
                                      S->GetResource(), 0, &src_rc, D->GetDXGIFormat(), D3D12_RESOLVE_MODE_AVERAGE);
  }

  S->TransitionSubresourceToState(cmdlist, 0, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, S->GetResourceState());
  D->TransitionSubresourceToState(cmdlist, DSR, D3D12_RESOURCE_STATE_RESOLVE_DEST, D->GetResourceState());
}

void D3D12Device::ClearRenderTarget(GPUTexture* t, u32 c)
{
  GPUDevice::ClearRenderTarget(t, c);
  if (InRenderPass() && IsRenderTargetBound(t))
    EndRenderPass();
}

void D3D12Device::ClearDepth(GPUTexture* t, float d)
{
  GPUDevice::ClearDepth(t, d);
  if (InRenderPass() && m_current_depth_target == t)
    EndRenderPass();
}

void D3D12Device::InvalidateRenderTarget(GPUTexture* t)
{
  GPUDevice::InvalidateRenderTarget(t);
  if (InRenderPass() && (t->IsRenderTarget() ? IsRenderTargetBound(t) : (m_current_depth_target == t)))
    EndRenderPass();
}

bool D3D12Device::CreateBuffers()
{
  if (!m_vertex_buffer.Create(VERTEX_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate vertex buffer");
    return false;
  }

  if (!m_index_buffer.Create(INDEX_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate index buffer");
    return false;
  }

  if (!m_uniform_buffer.Create(VERTEX_UNIFORM_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate uniform buffer");
    return false;
  }

  if (!m_texture_upload_buffer.Create(TEXTURE_BUFFER_SIZE))
  {
    Log_ErrorPrint("Failed to allocate texture upload buffer");
    return false;
  }

  return true;
}

void D3D12Device::DestroyBuffers()
{
  m_texture_upload_buffer.Destroy(false);
  m_uniform_buffer.Destroy(false);
  m_index_buffer.Destroy(false);
  m_vertex_buffer.Destroy(false);
}

void D3D12Device::MapVertexBuffer(u32 vertex_size, u32 vertex_count, void** map_ptr, u32* map_space,
                                  u32* map_base_vertex)
{
  const u32 req_size = vertex_size * vertex_count;
  if (!m_vertex_buffer.ReserveMemory(req_size, vertex_size))
  {
    SubmitCommandListAndRestartRenderPass("out of vertex space");
    if (!m_vertex_buffer.ReserveMemory(req_size, vertex_size))
      Panic("Failed to allocate vertex space");
  }

  *map_ptr = m_vertex_buffer.GetCurrentHostPointer();
  *map_space = m_vertex_buffer.GetCurrentSpace() / vertex_size;
  *map_base_vertex = m_vertex_buffer.GetCurrentOffset() / vertex_size;
}

void D3D12Device::UnmapVertexBuffer(u32 vertex_size, u32 vertex_count)
{
  m_vertex_buffer.CommitMemory(vertex_size * vertex_count);
}

void D3D12Device::MapIndexBuffer(u32 index_count, DrawIndex** map_ptr, u32* map_space, u32* map_base_index)
{
  const u32 req_size = sizeof(DrawIndex) * index_count;
  if (!m_index_buffer.ReserveMemory(req_size, sizeof(DrawIndex)))
  {
    SubmitCommandListAndRestartRenderPass("out of index space");
    if (!m_index_buffer.ReserveMemory(req_size, sizeof(DrawIndex)))
      Panic("Failed to allocate index space");
  }

  *map_ptr = reinterpret_cast<DrawIndex*>(m_index_buffer.GetCurrentHostPointer());
  *map_space = m_index_buffer.GetCurrentSpace() / sizeof(DrawIndex);
  *map_base_index = m_index_buffer.GetCurrentOffset() / sizeof(DrawIndex);
}

void D3D12Device::UnmapIndexBuffer(u32 used_index_count)
{
  m_index_buffer.CommitMemory(sizeof(DrawIndex) * used_index_count);
}

void D3D12Device::PushUniformBuffer(const void* data, u32 data_size)
{
  static constexpr std::array<u8, static_cast<u8>(GPUPipeline::Layout::MaxCount)> push_parameter = {
    0, // SingleTextureAndUBO
    2, // SingleTextureAndPushConstants
    1, // SingleTextureBufferAndPushConstants
    0, // MultiTextureAndUBO
    2, // MultiTextureAndPushConstants
  };

  DebugAssert(data_size < UNIFORM_PUSH_CONSTANTS_SIZE);
  if (m_dirty_flags & DIRTY_FLAG_PIPELINE_LAYOUT)
  {
    m_dirty_flags &= ~DIRTY_FLAG_PIPELINE_LAYOUT;
    UpdateRootSignature();
  }

  GetCommandList()->SetGraphicsRoot32BitConstants(push_parameter[static_cast<u8>(m_current_pipeline_layout)],
                                                  data_size / 4u, data, 0);
}

void* D3D12Device::MapUniformBuffer(u32 size)
{
  const u32 used_space = Common::AlignUpPow2(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  if (!m_uniform_buffer.ReserveMemory(used_space + MAX_UNIFORM_BUFFER_SIZE,
                                      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
  {
    SubmitCommandListAndRestartRenderPass("out of uniform space");
    if (!m_uniform_buffer.ReserveMemory(used_space + MAX_UNIFORM_BUFFER_SIZE,
                                        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT))
      Panic("Failed to allocate uniform space.");
  }

  return m_uniform_buffer.GetCurrentHostPointer();
}

void D3D12Device::UnmapUniformBuffer(u32 size)
{
  m_uniform_buffer_position = m_uniform_buffer.GetCurrentOffset();
  m_uniform_buffer.CommitMemory(size);
  m_dirty_flags |= DIRTY_FLAG_CONSTANT_BUFFER;
}

bool D3D12Device::CreateRootSignatures()
{
  D3D12::RootSignatureBuilder rsb;

  {
    auto& rs = m_root_signatures[static_cast<u8>(GPUPipeline::Layout::SingleTextureAndUBO)];

    rsb.SetInputAssemblerFlag();
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.AddCBVParameter(0, D3D12_SHADER_VISIBILITY_ALL);
    if (!(rs = rsb.Create()))
      return false;
    D3D12::SetObjectName(rs.Get(), "Single Texture + UBO Pipeline Layout");
  }

  {
    auto& rs = m_root_signatures[static_cast<u8>(GPUPipeline::Layout::SingleTextureAndPushConstants)];

    rsb.SetInputAssemblerFlag();
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.Add32BitConstants(0, UNIFORM_PUSH_CONSTANTS_SIZE / sizeof(u32), D3D12_SHADER_VISIBILITY_ALL);
    if (!(rs = rsb.Create()))
      return false;
    D3D12::SetObjectName(rs.Get(), "Single Texture Pipeline Layout");
  }

  {
    auto& rs = m_root_signatures[static_cast<u8>(GPUPipeline::Layout::SingleTextureBufferAndPushConstants)];

    rsb.SetInputAssemblerFlag();
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.Add32BitConstants(0, UNIFORM_PUSH_CONSTANTS_SIZE / sizeof(u32), D3D12_SHADER_VISIBILITY_ALL);
    if (!(rs = rsb.Create()))
      return false;
    D3D12::SetObjectName(rs.Get(), "Single Texture Buffer + UBO Pipeline Layout");
  }

  {
    auto& rs = m_root_signatures[static_cast<u8>(GPUPipeline::Layout::MultiTextureAndUBO)];

    rsb.SetInputAssemblerFlag();
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, MAX_TEXTURE_SAMPLERS, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, MAX_TEXTURE_SAMPLERS, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.AddCBVParameter(0, D3D12_SHADER_VISIBILITY_ALL);
    if (!(rs = rsb.Create()))
      return false;
    D3D12::SetObjectName(rs.Get(), "Multi Texture + UBO Pipeline Layout");
  }

  {
    auto& rs = m_root_signatures[static_cast<u8>(GPUPipeline::Layout::MultiTextureAndPushConstants)];

    rsb.SetInputAssemblerFlag();
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, MAX_TEXTURE_SAMPLERS, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.AddDescriptorTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, MAX_TEXTURE_SAMPLERS, D3D12_SHADER_VISIBILITY_PIXEL);
    rsb.Add32BitConstants(0, UNIFORM_PUSH_CONSTANTS_SIZE / sizeof(u32), D3D12_SHADER_VISIBILITY_ALL);
    if (!(rs = rsb.Create()))
      return false;
    D3D12::SetObjectName(rs.Get(), "Multi Texture Pipeline Layout");
  }

  return true;
}

void D3D12Device::DestroyRootSignatures()
{
  for (auto it = m_root_signatures.rbegin(); it != m_root_signatures.rend(); ++it)
    it->Reset();
}

void D3D12Device::SetRenderTargets(GPUTexture* const* rts, u32 num_rts, GPUTexture* ds)
{
  if (InRenderPass())
    EndRenderPass();

  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();

  m_current_depth_target = static_cast<D3D12Texture*>(ds);
  for (u32 i = 0; i < num_rts; i++)
  {
    D3D12Texture* const dt = static_cast<D3D12Texture*>(rts[i]);
    m_current_render_targets[i] = dt;
    dt->CommitClear(cmdlist);
  }
  for (u32 i = num_rts; i < m_num_current_render_targets; i++)
    m_current_render_targets[i] = nullptr;
  m_num_current_render_targets = num_rts;
}

void D3D12Device::BeginRenderPass()
{
  DebugAssert(!InRenderPass());

  std::array<D3D12_RENDER_PASS_RENDER_TARGET_DESC, MAX_RENDER_TARGETS> rt_desc;
  D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds_desc;

  D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt_desc_p = nullptr;
  D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds_desc_p = nullptr;
  u32 num_rt_descs = 0;

  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();

  if (m_num_current_render_targets > 0 || m_current_depth_target) [[likely]]
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      D3D12Texture* const rt = m_current_render_targets[i];
      rt->TransitionToState(cmdlist, D3D12_RESOURCE_STATE_RENDER_TARGET);
      rt->SetUseFenceValue(GetCurrentFenceValue());

      D3D12_RENDER_PASS_RENDER_TARGET_DESC& desc = rt_desc[i];
      desc.cpuDescriptor = rt->GetWriteDescriptor();
      desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;

      switch (rt->GetState())
      {
        case GPUTexture::State::Cleared:
        {
          desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
          std::memcpy(desc.BeginningAccess.Clear.ClearValue.Color, rt->GetUNormClearColor().data(),
                      sizeof(desc.BeginningAccess.Clear.ClearValue.Color));
          rt->SetState(GPUTexture::State::Dirty);
        }
        break;

        case GPUTexture::State::Invalidated:
        {
          desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
          rt->SetState(GPUTexture::State::Dirty);
        }
        break;

        case GPUTexture::State::Dirty:
        {
          desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        }
        break;

        default:
          UnreachableCode();
          break;
      }
    }

    rt_desc_p = (m_num_current_render_targets > 0) ? rt_desc.data() : nullptr;
    num_rt_descs = m_num_current_render_targets;
    if (m_current_depth_target)
    {
      D3D12Texture* const ds = m_current_depth_target;
      ds->TransitionToState(cmdlist, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      ds->SetUseFenceValue(GetCurrentFenceValue());
      ds_desc_p = &ds_desc;
      ds_desc.cpuDescriptor = ds->GetWriteDescriptor();
      ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
      ds_desc.StencilBeginningAccess = {};
      ds_desc.StencilEndingAccess = {};

      switch (ds->GetState())
      {
        case GPUTexture::State::Cleared:
        {
          ds_desc.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
          ds_desc.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = ds->GetClearDepth();
          ds->SetState(GPUTexture::State::Dirty);
        }
        break;

        case GPUTexture::State::Invalidated:
        {
          ds_desc.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
          ds->SetState(GPUTexture::State::Dirty);
        }
        break;

        case GPUTexture::State::Dirty:
        {
          ds_desc.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        }
        break;

        default:
          UnreachableCode();
          break;
      }

      ds_desc_p = &ds_desc;
    }
  }
  else
  {
    // Re-rendering to swap chain.
    const auto& swap_chain_buf = m_swap_chain_buffers[m_current_swap_chain_buffer];
    rt_desc[0] = {swap_chain_buf.second,
                  {D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE, {}},
                  {D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, {}}};
    rt_desc_p = &rt_desc[0];
    num_rt_descs = 1;
  }

  // All textures should be in shader read only optimal already, but just in case..
  const u32 num_textures = GetActiveTexturesForLayout(m_current_pipeline_layout);
  for (u32 i = 0; i < num_textures; i++)
  {
    if (m_current_textures[i])
      m_current_textures[i]->TransitionToState(cmdlist, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  DebugAssert(rt_desc_p || ds_desc_p);
  cmdlist->BeginRenderPass(num_rt_descs, rt_desc_p, ds_desc_p, D3D12_RENDER_PASS_FLAG_NONE);

  // TODO: Stats
  m_in_render_pass = true;

  // If this is a new command buffer, bind the pipeline and such.
  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    SetInitialPipelineState();
}

void D3D12Device::BeginSwapChainRenderPass()
{
  DebugAssert(!InRenderPass());

  ID3D12GraphicsCommandList4* const cmdlist = GetCommandList();
  const auto& swap_chain_buf = m_swap_chain_buffers[m_current_swap_chain_buffer];

  D3D12Texture::TransitionSubresourceToState(cmdlist, swap_chain_buf.first.Get(), 0, D3D12_RESOURCE_STATE_COMMON,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET);

  // All textures should be in shader read only optimal already, but just in case..
  const u32 num_textures = GetActiveTexturesForLayout(m_current_pipeline_layout);
  for (u32 i = 0; i < num_textures; i++)
  {
    if (m_current_textures[i])
      m_current_textures[i]->TransitionToState(cmdlist, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  }

  const D3D12_RENDER_PASS_RENDER_TARGET_DESC rt_desc = {
    swap_chain_buf.second,
    {D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR, {s_present_clear_color}},
    {D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE, {}}};
  cmdlist->BeginRenderPass(1, &rt_desc, nullptr, D3D12_RENDER_PASS_FLAG_NONE);

  std::memset(m_current_render_targets.data(), 0, sizeof(m_current_render_targets));
  m_num_current_render_targets = 0;
  m_current_depth_target = nullptr;
  m_in_render_pass = true;

  // Clear pipeline, it's likely incompatible.
  m_current_pipeline = nullptr;
}

bool D3D12Device::InRenderPass()
{
  return m_in_render_pass;
}

void D3D12Device::EndRenderPass()
{
  DebugAssert(m_in_render_pass);

  // TODO: stats
  m_in_render_pass = false;

  GetCommandList()->EndRenderPass();
}

void D3D12Device::SetPipeline(GPUPipeline* pipeline)
{
  // First draw? Bind everything.
  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
  {
    m_current_pipeline = static_cast<D3D12Pipeline*>(pipeline);
    if (!m_current_pipeline)
      return;

    SetInitialPipelineState();
    return;
  }
  else if (m_current_pipeline == pipeline)
  {
    return;
  }

  m_current_pipeline = static_cast<D3D12Pipeline*>(pipeline);

  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();
  cmdlist->SetPipelineState(m_current_pipeline->GetPipeline());

  if (D3D12_PRIMITIVE_TOPOLOGY topology = m_current_pipeline->GetTopology(); topology != m_current_topology)
  {
    m_current_topology = topology;
    cmdlist->IASetPrimitiveTopology(topology);
  }

  if (u32 vertex_stride = m_current_pipeline->GetVertexStride();
      vertex_stride > 0 && m_current_vertex_stride != vertex_stride)
  {
    m_current_vertex_stride = vertex_stride;
    SetVertexBuffer(cmdlist);
  }

  // TODO: we don't need to change the blend constant if blending isn't on.
  if (u32 blend_constants = m_current_pipeline->GetBlendConstants(); m_current_blend_constant != blend_constants)
  {
    m_current_blend_constant = blend_constants;
    cmdlist->OMSetBlendFactor(m_current_pipeline->GetBlendConstantsF().data());
  }

  if (GPUPipeline::Layout layout = m_current_pipeline->GetLayout(); m_current_pipeline_layout != layout)
  {
    m_current_pipeline_layout = layout;
    m_dirty_flags |=
      DIRTY_FLAG_PIPELINE_LAYOUT | DIRTY_FLAG_CONSTANT_BUFFER | DIRTY_FLAG_TEXTURES | DIRTY_FLAG_SAMPLERS;
  }
}

void D3D12Device::UnbindPipeline(D3D12Pipeline* pl)
{
  if (m_current_pipeline != pl)
    return;

  m_current_pipeline = nullptr;
}

bool D3D12Device::IsRenderTargetBound(const GPUTexture* tex) const
{
  for (u32 i = 0; i < m_num_current_render_targets; i++)
  {
    if (m_current_render_targets[i] == tex)
      return true;
  }

  return false;
}

void D3D12Device::InvalidateCachedState()
{
  m_dirty_flags = ALL_DIRTY_STATE;
  m_in_render_pass = false;
  m_current_pipeline = nullptr;
  m_current_vertex_stride = 0;
  m_current_blend_constant = 0;
  m_current_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

void D3D12Device::SetInitialPipelineState()
{
  DebugAssert(m_current_pipeline);
  m_dirty_flags &= ~DIRTY_FLAG_INITIAL;

  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();

  m_current_vertex_stride = m_current_pipeline->GetVertexStride();
  SetVertexBuffer(cmdlist);
  const D3D12_INDEX_BUFFER_VIEW ib_view = {m_index_buffer.GetGPUPointer(), m_index_buffer.GetSize(),
                                           DXGI_FORMAT_R16_UINT};
  cmdlist->IASetIndexBuffer(&ib_view);

  cmdlist->SetPipelineState(m_current_pipeline->GetPipeline());
  m_current_pipeline_layout = m_current_pipeline->GetLayout();

  m_current_topology = m_current_pipeline->GetTopology();
  cmdlist->IASetPrimitiveTopology(m_current_topology);

  m_current_blend_constant = m_current_pipeline->GetBlendConstants();
  cmdlist->OMSetBlendFactor(m_current_pipeline->GetBlendConstantsF().data());

  SetViewport(cmdlist);
  SetScissor(cmdlist);
}

void D3D12Device::SetVertexBuffer(ID3D12GraphicsCommandList4* cmdlist)
{
  const D3D12_VERTEX_BUFFER_VIEW vb_view = {m_vertex_buffer.GetGPUPointer(), m_vertex_buffer.GetSize(),
                                            m_current_vertex_stride};
  cmdlist->IASetVertexBuffers(0, 1, &vb_view);
}

void D3D12Device::SetViewport(ID3D12GraphicsCommandList4* cmdlist)
{
  const D3D12_VIEWPORT vp = {static_cast<float>(m_current_viewport.left),
                             static_cast<float>(m_current_viewport.top),
                             static_cast<float>(m_current_viewport.GetWidth()),
                             static_cast<float>(m_current_viewport.GetHeight()),
                             0.0f,
                             1.0f};
  cmdlist->RSSetViewports(1, &vp);
}

void D3D12Device::SetScissor(ID3D12GraphicsCommandList4* cmdlist)
{
  const D3D12_RECT rc = {static_cast<LONG>(m_current_scissor.left), static_cast<LONG>(m_current_scissor.top),
                         static_cast<LONG>(m_current_scissor.right), static_cast<LONG>(m_current_scissor.bottom)};
  cmdlist->RSSetScissorRects(1, &rc);
}

void D3D12Device::SetTextureSampler(u32 slot, GPUTexture* texture, GPUSampler* sampler)
{
  D3D12Texture* T = static_cast<D3D12Texture*>(texture);
  if (m_current_textures[slot] != T)
  {
    m_current_textures[slot] = T;
    m_dirty_flags |= DIRTY_FLAG_TEXTURES;

    if (T)
    {
      T->SetUseFenceValue(GetCurrentFenceValue());
      if (T->GetResourceState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
      {
        if (InRenderPass())
          EndRenderPass();
        T->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
  }

  const D3D12DescriptorHandle& handle =
    sampler ? static_cast<D3D12Sampler*>(sampler)->GetDescriptor() : m_point_sampler;
  if (m_current_samplers[slot] != handle)
  {
    m_current_samplers[slot] = handle;
    m_dirty_flags |= DIRTY_FLAG_SAMPLERS;
  }
}

void D3D12Device::SetTextureBuffer(u32 slot, GPUTextureBuffer* buffer)
{
  DebugAssert(slot == 0);
  if (m_current_texture_buffer == buffer)
    return;

  m_current_texture_buffer = static_cast<D3D12TextureBuffer*>(buffer);
  if (m_current_pipeline_layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
    m_dirty_flags |= DIRTY_FLAG_TEXTURES;
}

void D3D12Device::UnbindTexture(D3D12Texture* tex)
{
  for (u32 i = 0; i < MAX_TEXTURE_SAMPLERS; i++)
  {
    if (m_current_textures[i] == tex)
    {
      m_current_textures[i] = nullptr;
      m_dirty_flags |= DIRTY_FLAG_TEXTURES;
    }
  }

  if (tex->IsRenderTarget())
  {
    for (u32 i = 0; i < m_num_current_render_targets; i++)
    {
      if (m_current_render_targets[i] == tex)
      {
        if (InRenderPass())
          EndRenderPass();
        m_current_render_targets[i] = nullptr;
      }
    }
  }
  else if (tex->IsDepthStencil())
  {
    if (m_current_depth_target == tex)
    {
      if (InRenderPass())
        EndRenderPass();
      m_current_depth_target = nullptr;
    }
  }
}

void D3D12Device::UnbindTextureBuffer(D3D12TextureBuffer* buf)
{
  if (m_current_texture_buffer != buf)
    return;

  m_current_texture_buffer = nullptr;

  if (m_current_pipeline_layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
    m_dirty_flags |= DIRTY_FLAG_TEXTURES;
}

void D3D12Device::SetViewport(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (m_current_viewport == rc)
    return;

  m_current_viewport = rc;

  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    return;

  SetViewport(GetCommandList());
}

void D3D12Device::SetScissor(s32 x, s32 y, s32 width, s32 height)
{
  const Common::Rectangle<s32> rc = Common::Rectangle<s32>::FromExtents(x, y, width, height);
  if (m_current_scissor == rc)
    return;

  m_current_scissor = rc;

  if (m_dirty_flags & DIRTY_FLAG_INITIAL)
    return;

  SetScissor(GetCommandList());
}

void D3D12Device::PreDrawCheck()
{
  // TODO: Flushing cmdbuffer because of descriptor OOM will lose push constants.

  DebugAssert(!(m_dirty_flags & DIRTY_FLAG_INITIAL));
  const u32 dirty = std::exchange(m_dirty_flags, 0);
  if (dirty != 0)
  {
    if (dirty & DIRTY_FLAG_PIPELINE_LAYOUT)
    {
      UpdateRootSignature();
      if (!UpdateRootParameters(dirty))
      {
        SubmitCommandListAndRestartRenderPass("out of descriptors");
        PreDrawCheck();
        return;
      }
    }
    else if (dirty & (DIRTY_FLAG_CONSTANT_BUFFER | DIRTY_FLAG_TEXTURES | DIRTY_FLAG_SAMPLERS))
    {
      if (!UpdateRootParameters(dirty))
      {
        SubmitCommandListAndRestartRenderPass("out of descriptors");
        PreDrawCheck();
        return;
      }
    }
  }

  if (!InRenderPass())
    BeginRenderPass();
}

void D3D12Device::UpdateRootSignature()
{
  GetCommandList()->SetGraphicsRootSignature(m_root_signatures[static_cast<u8>(m_current_pipeline_layout)].Get());
}

template<GPUPipeline::Layout layout>
bool D3D12Device::UpdateParametersForLayout(u32 dirty)
{
  ID3D12GraphicsCommandList4* cmdlist = GetCommandList();

  if constexpr (layout == GPUPipeline::Layout::SingleTextureAndUBO || layout == GPUPipeline::Layout::MultiTextureAndUBO)
  {
    if (dirty & DIRTY_FLAG_CONSTANT_BUFFER)
      cmdlist->SetGraphicsRootConstantBufferView(2, m_uniform_buffer.GetGPUPointer() + m_uniform_buffer_position);
  }

  constexpr u32 num_textures = GetActiveTexturesForLayout(layout);
  if (dirty & DIRTY_FLAG_TEXTURES && num_textures > 0)
  {
    D3D12DescriptorAllocator& allocator = m_command_lists[m_current_command_list].descriptor_allocator;
    D3D12DescriptorHandle gpu_handle;
    if (!allocator.Allocate(num_textures, &gpu_handle))
      return false;

    if constexpr (num_textures == 1)
    {
      m_device->CopyDescriptorsSimple(
        1, gpu_handle, m_current_textures[0] ? m_current_textures[0]->GetSRVDescriptor() : m_null_srv_descriptor,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else
    {
      D3D12_CPU_DESCRIPTOR_HANDLE src_handles[MAX_TEXTURE_SAMPLERS];
      UINT src_sizes[MAX_TEXTURE_SAMPLERS];
      for (u32 i = 0; i < num_textures; i++)
      {
        src_handles[i] = m_current_textures[i] ? m_current_textures[i]->GetSRVDescriptor() : m_null_srv_descriptor;
        src_sizes[i] = 1;
      }
      m_device->CopyDescriptors(1, &gpu_handle.cpu_handle, &num_textures, num_textures, src_handles, src_sizes,
                                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    cmdlist->SetGraphicsRootDescriptorTable(0, gpu_handle);
  }

  if (dirty & DIRTY_FLAG_SAMPLERS && num_textures > 0)
  {
    auto& allocator = m_command_lists[m_current_command_list].sampler_allocator;
    D3D12DescriptorHandle gpu_handle;
    if constexpr (num_textures == 1)
    {
      if (!allocator.LookupSingle(m_device.Get(), &gpu_handle, m_current_samplers[0]))
        return false;
    }
    else
    {
      if (!allocator.LookupGroup(m_device.Get(), &gpu_handle, m_current_samplers.data()))
        return false;
    }

    cmdlist->SetGraphicsRootDescriptorTable(1, gpu_handle);
  }

  if (dirty & DIRTY_FLAG_TEXTURES && layout == GPUPipeline::Layout::SingleTextureBufferAndPushConstants)
  {
    D3D12DescriptorAllocator& allocator = m_command_lists[m_current_command_list].descriptor_allocator;
    D3D12DescriptorHandle gpu_handle;
    if (!allocator.Allocate(1, &gpu_handle))
      return false;

    m_device->CopyDescriptorsSimple(
      1, gpu_handle, m_current_texture_buffer ? m_current_texture_buffer->GetDescriptor() : m_null_srv_descriptor,
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cmdlist->SetGraphicsRootDescriptorTable(0, gpu_handle);
  }

  return true;
}

bool D3D12Device::UpdateRootParameters(u32 dirty)
{
  switch (m_current_pipeline_layout)
  {
    case GPUPipeline::Layout::SingleTextureAndUBO:
      return UpdateParametersForLayout<GPUPipeline::Layout::SingleTextureAndUBO>(dirty);

    case GPUPipeline::Layout::SingleTextureAndPushConstants:
      return UpdateParametersForLayout<GPUPipeline::Layout::SingleTextureAndPushConstants>(dirty);

    case GPUPipeline::Layout::SingleTextureBufferAndPushConstants:
      return UpdateParametersForLayout<GPUPipeline::Layout::SingleTextureBufferAndPushConstants>(dirty);

    case GPUPipeline::Layout::MultiTextureAndUBO:
      return UpdateParametersForLayout<GPUPipeline::Layout::MultiTextureAndUBO>(dirty);

    case GPUPipeline::Layout::MultiTextureAndPushConstants:
      return UpdateParametersForLayout<GPUPipeline::Layout::MultiTextureAndPushConstants>(dirty);

    default:
      UnreachableCode();
  }
}

void D3D12Device::Draw(u32 vertex_count, u32 base_vertex)
{
  PreDrawCheck();
  GetCommandList()->DrawInstanced(vertex_count, 1, base_vertex, 0);
}

void D3D12Device::DrawIndexed(u32 index_count, u32 base_index, u32 base_vertex)
{
  PreDrawCheck();
  GetCommandList()->DrawIndexedInstanced(index_count, 1, base_index, base_vertex, 0);
}
