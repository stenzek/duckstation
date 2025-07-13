// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "gpu_device.h"

#include "common/heap_array.h"
#include "common/types.h"
#include "common/windows_headers.h"

#include <d3dcommon.h>
#include <dxgiformat.h>
#include <dxgitype.h>
#include <optional>
#include <string>
#include <vector>
#include <wrl/client.h>

class Error;

enum class GPUDriverType : u16;

struct D3D12_ROOT_SIGNATURE_DESC;

struct IDXGIFactory5;
struct IDXGIAdapter;
struct IDXGIAdapter1;
struct IDXGIOutput;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D12Debug;
struct ID3D12Device1;

namespace D3DCommon {

// returns string representation of feature level
const char* GetFeatureLevelString(u32 render_api_version);
u32 GetRenderAPIVersionForFeatureLevel(D3D_FEATURE_LEVEL feature_level);
D3D_FEATURE_LEVEL GetFeatureLevelForNumber(u32 render_api_version);
u32 GetShaderModelForFeatureLevelNumber(u32 render_api_version);

// returns max feature level of a device
D3D_FEATURE_LEVEL GetDeviceMaxFeatureLevel(IDXGIAdapter1* adapter);

// create a dxgi factory
Microsoft::WRL::ComPtr<IDXGIFactory5> CreateFactory(bool debug, Error* error);
bool SupportsAllowTearing(IDXGIFactory5* factory);

// create a D3D device
bool CreateD3D11Device(IDXGIAdapter* adapter, UINT create_flags, const D3D_FEATURE_LEVEL* feature_levels,
                       UINT num_feature_levels, Microsoft::WRL::ComPtr<ID3D11Device>* device,
                       D3D_FEATURE_LEVEL* out_feature_level,
                       Microsoft::WRL::ComPtr<ID3D11DeviceContext>* immediate_context, Error* error);

// D3D12 functions
bool GetD3D12DebugInterface(Microsoft::WRL::ComPtr<ID3D12Debug>* debug, Error* error);
bool CreateD3D12Device(IDXGIAdapter* adapter, D3D_FEATURE_LEVEL feature_level,
                       Microsoft::WRL::ComPtr<ID3D12Device1>* device, Error* error);
Microsoft::WRL::ComPtr<ID3DBlob> SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc, Error* error);

// returns a list of all adapter names
GPUDevice::AdapterInfoList GetAdapterInfoList();

// returns the fullscreen mode to use for the specified dimensions
std::optional<DXGI_MODE_DESC>
GetRequestedExclusiveFullscreenModeDesc(IDXGIAdapter* adapter, const RECT& window_rect,
                                        const GPUDevice::ExclusiveFullscreenMode* requested_fullscreen_mode,
                                        DXGI_FORMAT format, IDXGIOutput** output);

// get an adapter based on name
Microsoft::WRL::ComPtr<IDXGIAdapter1> GetAdapterByName(IDXGIFactory5* factory, std::string_view name);

// returns the first adapter in the system
Microsoft::WRL::ComPtr<IDXGIAdapter1> GetFirstAdapter(IDXGIFactory5* factory);

// returns the adapter specified in the configuration, or the default
Microsoft::WRL::ComPtr<IDXGIAdapter1> GetChosenOrFirstAdapter(IDXGIFactory5* factory, std::string_view name);

// returns a utf-8 string of the specified adapter's name
std::string GetAdapterName(IDXGIAdapter1* adapter, GPUDriverType* out_driver_type = nullptr);

// returns the driver version from the registry as a string
std::string GetDriverVersionFromLUID(const LUID& luid);

std::optional<DynamicHeapArray<u8>> CompileShader(u32 shader_model, bool debug_device, GPUShaderStage stage,
                                                  std::string_view source, const char* entry_point, Error* error);

struct DXGIFormatMapping
{
  DXGI_FORMAT resource_format;
  DXGI_FORMAT srv_format;
  DXGI_FORMAT rtv_format;
  DXGI_FORMAT dsv_format;
};
const DXGIFormatMapping& GetFormatMapping(GPUTexture::Format format);
GPUTexture::Format GetFormatForDXGIFormat(DXGI_FORMAT format);

} // namespace D3DCommon
