// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "context.h"
#include "../assert.h"
#include "../log.h"
#include "../scope_guard.h"
#include <algorithm>
#include <array>
#include <dxgi1_2.h>
#include <queue>
#include <vector>
Log_SetChannel(D3D12::Context);

std::unique_ptr<D3D12::Context> g_d3d12_context;

namespace D3D12 {

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

// Private D3D12 state
static HMODULE s_d3d12_library;
static PFN_D3D12_CREATE_DEVICE s_d3d12_create_device;
static PFN_D3D12_GET_DEBUG_INTERFACE s_d3d12_get_debug_interface;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE s_d3d12_serialize_root_signature;

static bool LoadD3D12Library()
{
  if ((s_d3d12_library = LoadLibrary("d3d12.dll")) == nullptr ||
      (s_d3d12_create_device =
         reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(s_d3d12_library, "D3D12CreateDevice"))) == nullptr ||
      (s_d3d12_get_debug_interface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
         GetProcAddress(s_d3d12_library, "D3D12GetDebugInterface"))) == nullptr ||
      (s_d3d12_serialize_root_signature = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
         GetProcAddress(s_d3d12_library, "D3D12SerializeRootSignature"))) == nullptr)
  {
    Log_ErrorPrintf("d3d12.dll could not be loaded.");
    s_d3d12_create_device = nullptr;
    s_d3d12_get_debug_interface = nullptr;
    s_d3d12_serialize_root_signature = nullptr;
    if (s_d3d12_library)
      FreeLibrary(s_d3d12_library);
    s_d3d12_library = nullptr;
    return false;
  }

  return true;
}

static void UnloadD3D12Library()
{
  s_d3d12_serialize_root_signature = nullptr;
  s_d3d12_get_debug_interface = nullptr;
  s_d3d12_create_device = nullptr;
  if (s_d3d12_library)
  {
    FreeLibrary(s_d3d12_library);
    s_d3d12_library = nullptr;
  }
}

#else

static const PFN_D3D12_CREATE_DEVICE s_d3d12_create_device = D3D12CreateDevice;
static const PFN_D3D12_GET_DEBUG_INTERFACE s_d3d12_get_debug_interface = D3D12GetDebugInterface;
static const PFN_D3D12_SERIALIZE_ROOT_SIGNATURE s_d3d12_serialize_root_signature = D3D12SerializeRootSignature;

static bool LoadD3D12Library()
{
  return true;
}

static void UnloadD3D12Library() {}

#endif

Context::Context() = default;

Context::~Context()
{
  DestroyResources();
}

Context::ComPtr<ID3DBlob> Context::SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc)
{
  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error_blob;
  const HRESULT hr = s_d3d12_serialize_root_signature(desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(),
                                                      error_blob.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("D3D12SerializeRootSignature() failed: %08X", hr);
    if (error_blob)
      Log_ErrorPrintf("%s", error_blob->GetBufferPointer());

    return {};
  }

  return blob;
}

D3D12::Context::ComPtr<ID3D12RootSignature> Context::CreateRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc)
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

bool Context::SupportsTextureFormat(DXGI_FORMAT format)
{
  constexpr u32 required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;

  D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
  return SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
         (support.Support1 & required) == required;
}

bool Context::Create(IDXGIFactory* dxgi_factory, u32 adapter_index, bool enable_debug_layer)
{
  Assert(!g_d3d12_context);

  if (!LoadD3D12Library())
    return false;

  g_d3d12_context.reset(new Context());
  if (!g_d3d12_context->CreateDevice(dxgi_factory, adapter_index, enable_debug_layer) ||
      !g_d3d12_context->CreateCommandQueue() || !g_d3d12_context->CreateFence() ||
      !g_d3d12_context->CreateDescriptorHeaps() || !g_d3d12_context->CreateCommandLists() ||
      !g_d3d12_context->CreateTextureStreamBuffer())
  {
    Destroy();
    return false;
  }

  return true;
}

void Context::Destroy()
{
  if (g_d3d12_context)
    g_d3d12_context.reset();

  UnloadD3D12Library();
}

bool Context::CreateDevice(IDXGIFactory* dxgi_factory, u32 adapter_index, bool enable_debug_layer)
{
  ComPtr<IDXGIAdapter> adapter;
  HRESULT hr = dxgi_factory->EnumAdapters(adapter_index, &adapter);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Adapter %u not found, using default", adapter_index);
    adapter = nullptr;
  }
  else
  {
    DXGI_ADAPTER_DESC adapter_desc;
    if (SUCCEEDED(adapter->GetDesc(&adapter_desc)))
    {
      char adapter_name_buffer[128];
      const int name_length = WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description,
                                                  static_cast<int>(std::wcslen(adapter_desc.Description)),
                                                  adapter_name_buffer, countof(adapter_name_buffer), 0, nullptr);
      if (name_length >= 0)
      {
        adapter_name_buffer[name_length] = 0;
        Log_InfoPrintf("D3D Adapter: %s", adapter_name_buffer);
      }
    }
  }

  // Enabling the debug layer will fail if the Graphics Tools feature is not installed.
  if (enable_debug_layer)
  {
    hr = s_d3d12_get_debug_interface(IID_PPV_ARGS(&m_debug_interface));
    if (SUCCEEDED(hr))
    {
      m_debug_interface->EnableDebugLayer();
    }
    else
    {
      Log_ErrorPrintf("Debug layer requested but not available.");
      enable_debug_layer = false;
    }
  }

  // Create the actual device.
  hr = s_d3d12_create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
  AssertMsg(SUCCEEDED(hr), "Create D3D12 device");
  if (FAILED(hr))
    return false;

  if (enable_debug_layer)
  {
    ComPtr<ID3D12InfoQueue> info_queue;
    if (SUCCEEDED(m_device.As(&info_queue)))
    {
      info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
      info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

      D3D12_INFO_QUEUE_FILTER filter = {};
      std::array<D3D12_MESSAGE_ID, 5> id_list{
        D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
        D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
        D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_TYPE_MISMATCH,
        D3D12_MESSAGE_ID_DRAW_EMPTY_SCISSOR_RECTANGLE,
      };
      filter.DenyList.NumIDs = static_cast<UINT>(id_list.size());
      filter.DenyList.pIDList = id_list.data();
      info_queue->PushStorageFilter(&filter);
    }
  }

  return true;
}

bool Context::CreateCommandQueue()
{
  const D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                                               D3D12_COMMAND_QUEUE_FLAG_NONE};
  HRESULT hr = m_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_command_queue));
  AssertMsg(SUCCEEDED(hr), "Create command queue");
  return SUCCEEDED(hr);
}

bool Context::CreateFence()
{
  HRESULT hr = m_device->CreateFence(m_completed_fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  AssertMsg(SUCCEEDED(hr), "Create fence");
  if (FAILED(hr))
    return false;

  m_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  AssertMsg(m_fence_event != NULL, "Create fence event");
  if (!m_fence_event)
    return false;

  return true;
}

bool Context::CreateDescriptorHeaps()
{
  static constexpr size_t MAX_SRVS = 16384;
  static constexpr size_t MAX_RTVS = 8192;
  static constexpr size_t MAX_DSVS = 128;
  static constexpr size_t MAX_SAMPLERS = 128;

  if (!m_descriptor_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MAX_SRVS, true) ||
      !m_rtv_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_RTVS, false) ||
      !m_dsv_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MAX_DSVS, false) ||
      !m_sampler_heap_manager.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, MAX_SAMPLERS, true))
  {
    return false;
  }

  m_gpu_descriptor_heaps[0] = m_descriptor_heap_manager.GetDescriptorHeap();
  m_gpu_descriptor_heaps[1] = m_sampler_heap_manager.GetDescriptorHeap();

  // Allocate null SRV descriptor for unbound textures.
  constexpr D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D,
                                                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};

  if (!m_descriptor_heap_manager.Allocate(&m_null_srv_descriptor))
  {
    Panic("Failed to allocate null descriptor");
    return false;
  }

  m_device->CreateShaderResourceView(nullptr, &null_srv_desc, m_null_srv_descriptor.cpu_handle);
  return true;
}

bool Context::CreateCommandLists()
{
  for (u32 i = 0; i < NUM_COMMAND_LISTS; i++)
  {
    CommandListResources& res = m_command_lists[i];
    HRESULT hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(res.command_allocator.GetAddressOf()));
    AssertMsg(SUCCEEDED(hr), "Create command allocator");
    if (FAILED(hr))
      return false;

    hr = m_device->CreateCommandList(1, D3D12_COMMAND_LIST_TYPE_DIRECT, res.command_allocator.Get(), nullptr,
                                     IID_PPV_ARGS(res.command_list.GetAddressOf()));
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Failed to create command list: %08X", hr);
      return false;
    }

    // Close the command list, since the first thing we do is reset them.
    hr = res.command_list->Close();
    AssertMsg(SUCCEEDED(hr), "Closing new command list failed");
    if (FAILED(hr))
      return false;
  }

  MoveToNextCommandList();
  return true;
}

bool Context::CreateTextureStreamBuffer()
{
  return m_texture_stream_buffer.Create(TEXTURE_UPLOAD_BUFFER_SIZE);
}

void Context::MoveToNextCommandList()
{
  m_current_command_list = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
  m_current_fence_value++;

  // We may have to wait if this command list hasn't finished on the GPU.
  CommandListResources& res = m_command_lists[m_current_command_list];
  WaitForFence(res.ready_fence_value);

  // Begin command list.
  res.command_allocator->Reset();
  res.command_list->Reset(res.command_allocator.Get(), nullptr);
  res.command_list->SetDescriptorHeaps(static_cast<UINT>(m_gpu_descriptor_heaps.size()), m_gpu_descriptor_heaps.data());
  res.ready_fence_value = m_current_fence_value;
}

void Context::ExecuteCommandList(bool wait_for_completion)
{
  CommandListResources& res = m_command_lists[m_current_command_list];

  // Close and queue command list.
  HRESULT hr = res.command_list->Close();
  AssertMsg(SUCCEEDED(hr), "Close command list");
  const std::array<ID3D12CommandList*, 1> execute_lists{res.command_list.Get()};
  m_command_queue->ExecuteCommandLists(static_cast<UINT>(execute_lists.size()), execute_lists.data());

  // Update fence when GPU has completed.
  hr = m_command_queue->Signal(m_fence.Get(), m_current_fence_value);
  AssertMsg(SUCCEEDED(hr), "Signal fence");

  MoveToNextCommandList();
  if (wait_for_completion)
    WaitForFence(res.ready_fence_value);
}

void Context::DeferResourceDestruction(ID3D12Resource* resource)
{
  if (!resource)
    return;

  resource->AddRef();
  m_command_lists[m_current_command_list].pending_resources.push_back(resource);
}

void Context::DeferDescriptorDestruction(DescriptorHeapManager& manager, u32 index)
{
  m_command_lists[m_current_command_list].pending_descriptors.emplace_back(manager, index);
}

void Context::DeferDescriptorDestruction(DescriptorHeapManager& manager, DescriptorHandle* handle)
{
  if (handle->index == DescriptorHandle::INVALID_INDEX)
    return;

  m_command_lists[m_current_command_list].pending_descriptors.emplace_back(manager, handle->index);
  handle->Clear();
}

void Context::DestroyPendingResources(CommandListResources& cmdlist)
{
  for (const auto& dd : cmdlist.pending_descriptors)
    dd.first.Free(dd.second);
  cmdlist.pending_descriptors.clear();

  for (ID3D12Resource* res : cmdlist.pending_resources)
    res->Release();
  cmdlist.pending_resources.clear();
}

void Context::DestroyResources()
{
  ExecuteCommandList(true);

  m_texture_stream_buffer.Destroy(false);
  m_descriptor_heap_manager.Free(&m_null_srv_descriptor);
  m_sampler_heap_manager.Destroy();
  m_dsv_heap_manager.Destroy();
  m_rtv_heap_manager.Destroy();
  m_descriptor_heap_manager.Destroy();
  m_command_lists = {};
  m_current_command_list = 0;
  m_completed_fence_value = 0;
  m_current_fence_value = 0;
  if (m_fence_event)
  {
    CloseHandle(m_fence_event);
    m_fence_event = {};
  }

  m_command_queue.Reset();
  m_debug_interface.Reset();
  m_device.Reset();
}

void Context::WaitForFence(u64 fence)
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
  u32 index = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
  for (u32 i = 0; i < NUM_COMMAND_LISTS; i++)
  {
    CommandListResources& res = m_command_lists[index];
    if (m_completed_fence_value < res.ready_fence_value)
      break;

    DestroyPendingResources(res);
    index = (index + 1) % NUM_COMMAND_LISTS;
  }
}

void Context::WaitForGPUIdle()
{
  u32 index = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
  for (u32 i = 0; i < (NUM_COMMAND_LISTS - 1); i++)
  {
    WaitForFence(m_command_lists[index].ready_fence_value);
    index = (index + 1) % NUM_COMMAND_LISTS;
  }
}
} // namespace D3D12
