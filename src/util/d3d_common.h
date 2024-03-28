// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "gpu_device.h"

#include "common/heap_array.h"
#include "common/types.h"
#include "common/windows_headers.h"

#include <d3dcommon.h>
#include <dxgiformat.h>
#include <optional>
#include <string>
#include <vector>
#include <wrl/client.h>

class Error;

struct IDXGIFactory5;
struct IDXGIAdapter1;
struct IDXGIOutput;
struct DXGI_MODE_DESC;

namespace D3DCommon {
// returns string representation of feature level
const char* GetFeatureLevelString(D3D_FEATURE_LEVEL feature_level);
const char* GetFeatureLevelShaderModelString(D3D_FEATURE_LEVEL feature_level);

// returns max feature level of a device
D3D_FEATURE_LEVEL GetDeviceMaxFeatureLevel(IDXGIAdapter1* adapter);

// create a dxgi factory
Microsoft::WRL::ComPtr<IDXGIFactory5> CreateFactory(bool debug, Error* error);

// returns a list of all adapter names
std::vector<std::string> GetAdapterNames(IDXGIFactory5* factory);

// returns a list of fullscreen modes for the specified adapter
std::vector<std::string> GetFullscreenModes(IDXGIFactory5* factory, const std::string_view& adapter_name);

// returns the fullscreen mode to use for the specified dimensions
bool GetRequestedExclusiveFullscreenModeDesc(IDXGIFactory5* factory, const RECT& window_rect, u32 width, u32 height,
                                             float refresh_rate, DXGI_FORMAT format, DXGI_MODE_DESC* fullscreen_mode,
                                             IDXGIOutput** output);

// get an adapter based on name
Microsoft::WRL::ComPtr<IDXGIAdapter1> GetAdapterByName(IDXGIFactory5* factory, const std::string_view& name);

// returns the first adapter in the system
Microsoft::WRL::ComPtr<IDXGIAdapter1> GetFirstAdapter(IDXGIFactory5* factory);

// returns the adapter specified in the configuration, or the default
Microsoft::WRL::ComPtr<IDXGIAdapter1> GetChosenOrFirstAdapter(IDXGIFactory5* factory, const std::string_view& name);

// returns a utf-8 string of the specified adapter's name
std::string GetAdapterName(IDXGIAdapter1* adapter);

// returns the driver version from the registry as a string
std::string GetDriverVersionFromLUID(const LUID& luid);

std::optional<DynamicHeapArray<u8>> CompileShader(D3D_FEATURE_LEVEL feature_level, bool debug_device,
                                                  GPUShaderStage stage, const std::string_view& source,
                                                  const char* entry_point);

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
