// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "d3d_common.h"
#include "gpu_device.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/rectangle.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

Log_SetChannel(D3DCommon);

const char* D3DCommon::GetFeatureLevelString(D3D_FEATURE_LEVEL feature_level)
{
  static constexpr std::array<std::tuple<D3D_FEATURE_LEVEL, const char*>, 11> feature_level_names = {{
    {D3D_FEATURE_LEVEL_1_0_CORE, "D3D_FEATURE_LEVEL_1_0_CORE"},
    {D3D_FEATURE_LEVEL_9_1, "D3D_FEATURE_LEVEL_9_1"},
    {D3D_FEATURE_LEVEL_9_2, "D3D_FEATURE_LEVEL_9_2"},
    {D3D_FEATURE_LEVEL_9_3, "D3D_FEATURE_LEVEL_9_3"},
    {D3D_FEATURE_LEVEL_10_0, "D3D_FEATURE_LEVEL_10_0"},
    {D3D_FEATURE_LEVEL_10_1, "D3D_FEATURE_LEVEL_10_1"},
    {D3D_FEATURE_LEVEL_11_0, "D3D_FEATURE_LEVEL_11_0"},
    {D3D_FEATURE_LEVEL_11_1, "D3D_FEATURE_LEVEL_11_1"},
    {D3D_FEATURE_LEVEL_12_0, "D3D_FEATURE_LEVEL_12_0"},
    {D3D_FEATURE_LEVEL_12_1, "D3D_FEATURE_LEVEL_12_1"},
    {D3D_FEATURE_LEVEL_12_2, "D3D_FEATURE_LEVEL_12_2"},
  }};

  for (const auto& [fl, name] : feature_level_names)
  {
    if (fl == feature_level)
      return name;
  }

  return "D3D_FEATURE_LEVEL_UNKNOWN";
}

const char* D3DCommon::GetFeatureLevelShaderModelString(D3D_FEATURE_LEVEL feature_level)
{
  static constexpr std::array<std::tuple<D3D_FEATURE_LEVEL, const char*>, 4> feature_level_names = {{
    {D3D_FEATURE_LEVEL_10_0, "sm40"},
    {D3D_FEATURE_LEVEL_10_1, "sm41"},
    {D3D_FEATURE_LEVEL_11_0, "sm50"},
    {D3D_FEATURE_LEVEL_11_1, "sm51"},
  }};

  for (const auto& [fl, name] : feature_level_names)
  {
    if (fl == feature_level)
      return name;
  }

  return "unk";
}

D3D_FEATURE_LEVEL D3DCommon::GetDeviceMaxFeatureLevel(IDXGIAdapter1* adapter)
{
  static constexpr std::array requested_feature_levels = {
    D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

  D3D_FEATURE_LEVEL max_supported_level = requested_feature_levels.back();
  HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 requested_feature_levels.data(), static_cast<UINT>(requested_feature_levels.size()),
                                 D3D11_SDK_VERSION, nullptr, &max_supported_level, nullptr);
  if (FAILED(hr))
    Log_WarningFmt("D3D11CreateDevice() for getting max feature level failed: 0x{:08X}", static_cast<unsigned>(hr));

  return max_supported_level;
}

Microsoft::WRL::ComPtr<IDXGIFactory5> D3DCommon::CreateFactory(bool debug, Error* error)
{
  UINT flags = 0;
  if (debug)
    flags |= DXGI_CREATE_FACTORY_DEBUG;

  Microsoft::WRL::ComPtr<IDXGIFactory5> factory;
  const HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr))
    Error::SetHResult(error, "Failed to create DXGI factory: ", hr);

  return factory;
}

static std::string FixupDuplicateAdapterNames(const std::vector<std::string>& adapter_names, std::string adapter_name)
{
  if (std::any_of(adapter_names.begin(), adapter_names.end(),
                  [&adapter_name](const std::string& other) { return (adapter_name == other); }))
  {
    std::string original_adapter_name = std::move(adapter_name);

    u32 current_extra = 2;
    do
    {
      adapter_name = fmt::format("{} ({})", original_adapter_name.c_str(), current_extra);
      current_extra++;
    } while (std::any_of(adapter_names.begin(), adapter_names.end(),
                         [&adapter_name](const std::string& other) { return (adapter_name == other); }));
  }

  return adapter_name;
}

std::vector<std::string> D3DCommon::GetAdapterNames(IDXGIFactory5* factory)
{
  std::vector<std::string> adapter_names;

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (u32 index = 0;; index++)
  {
    const HRESULT hr = factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf());
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;

    if (FAILED(hr))
    {
      Log_ErrorPrintf("IDXGIFactory2::EnumAdapters() returned %08X", hr);
      continue;
    }

    adapter_names.push_back(FixupDuplicateAdapterNames(adapter_names, GetAdapterName(adapter.Get())));
  }

  return adapter_names;
}

std::vector<std::string> D3DCommon::GetFullscreenModes(IDXGIFactory5* factory, const std::string_view& adapter_name)
{
  std::vector<std::string> modes;
  HRESULT hr;

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter = GetChosenOrFirstAdapter(factory, adapter_name);
  if (!adapter)
    return modes;

  Microsoft::WRL::ComPtr<IDXGIOutput> output;
  if (FAILED(hr = adapter->EnumOutputs(0, &output)))
  {
    Log_ErrorPrintf("EnumOutputs() failed: %08X", hr);
    return modes;
  }

  UINT num_modes = 0;
  if (FAILED(hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num_modes, nullptr)))
  {
    Log_ErrorPrintf("GetDisplayModeList() failed: %08X", hr);
    return modes;
  }

  std::vector<DXGI_MODE_DESC> dmodes(num_modes);
  if (FAILED(hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num_modes, dmodes.data())))
  {
    Log_ErrorPrintf("GetDisplayModeList() (2) failed: %08X", hr);
    return modes;
  }

  for (const DXGI_MODE_DESC& mode : dmodes)
  {
    modes.push_back(GPUDevice::GetFullscreenModeString(mode.Width, mode.Height,
                                                       static_cast<float>(mode.RefreshRate.Numerator) /
                                                         static_cast<float>(mode.RefreshRate.Denominator)));
  }

  return modes;
}

bool D3DCommon::GetRequestedExclusiveFullscreenModeDesc(IDXGIFactory5* factory, const RECT& window_rect, u32 width,
                                                        u32 height, float refresh_rate, DXGI_FORMAT format,
                                                        DXGI_MODE_DESC* fullscreen_mode, IDXGIOutput** output)
{
  // We need to find which monitor the window is located on.
  const Common::Rectangle<s32> client_rc_vec(window_rect.left, window_rect.top, window_rect.right, window_rect.bottom);

  // The window might be on a different adapter to which we are rendering.. so we have to enumerate them all.
  HRESULT hr;
  Microsoft::WRL::ComPtr<IDXGIOutput> first_output, intersecting_output;

  for (u32 adapter_index = 0; !intersecting_output; adapter_index++)
  {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(adapter_index, adapter.GetAddressOf());
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    else if (FAILED(hr))
      continue;

    for (u32 output_index = 0;; output_index++)
    {
      Microsoft::WRL::ComPtr<IDXGIOutput> this_output;
      DXGI_OUTPUT_DESC output_desc;
      hr = adapter->EnumOutputs(output_index, this_output.GetAddressOf());
      if (hr == DXGI_ERROR_NOT_FOUND)
        break;
      else if (FAILED(hr) || FAILED(this_output->GetDesc(&output_desc)))
        continue;

      const Common::Rectangle<s32> output_rc(output_desc.DesktopCoordinates.left, output_desc.DesktopCoordinates.top,
                                             output_desc.DesktopCoordinates.right,
                                             output_desc.DesktopCoordinates.bottom);
      if (!client_rc_vec.Intersects(output_rc))
      {
        intersecting_output = std::move(this_output);
        break;
      }

      // Fallback to the first monitor.
      if (!first_output)
        first_output = std::move(this_output);
    }
  }

  if (!intersecting_output)
  {
    if (!first_output)
    {
      Log_ErrorPrintf("No DXGI output found. Can't use exclusive fullscreen.");
      return false;
    }

    Log_WarningPrint("No DXGI output found for window, using first.");
    intersecting_output = std::move(first_output);
  }

  DXGI_MODE_DESC request_mode = {};
  request_mode.Width = width;
  request_mode.Height = height;
  request_mode.Format = format;
  request_mode.RefreshRate.Numerator = static_cast<UINT>(std::floor(refresh_rate * 1000.0f));
  request_mode.RefreshRate.Denominator = 1000u;

  if (FAILED(hr = intersecting_output->FindClosestMatchingMode(&request_mode, fullscreen_mode, nullptr)) ||
      request_mode.Format != format)
  {
    Log_ErrorPrintf("Failed to find closest matching mode, hr=%08X", hr);
    return false;
  }

  *output = intersecting_output.Get();
  intersecting_output->AddRef();
  return true;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> D3DCommon::GetAdapterByName(IDXGIFactory5* factory, const std::string_view& name)
{
  if (name.empty())
    return {};

  // This might seem a bit odd to cache the names.. but there's a method to the madness.
  // We might have two GPUs with the same name... :)
  std::vector<std::string> adapter_names;

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (u32 index = 0;; index++)
  {
    const HRESULT hr = factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf());
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;

    if (FAILED(hr))
    {
      Log_ErrorPrintf("IDXGIFactory2::EnumAdapters() returned %08X");
      continue;
    }

    std::string adapter_name = FixupDuplicateAdapterNames(adapter_names, GetAdapterName(adapter.Get()));
    if (adapter_name == name)
    {
      Log_VerbosePrintf("Found adapter '%s'", adapter_name.c_str());
      return adapter;
    }

    adapter_names.push_back(std::move(adapter_name));
  }

  Log_ErrorFmt("Adapter '{}' not found.", name);
  return {};
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> D3DCommon::GetFirstAdapter(IDXGIFactory5* factory)
{
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  HRESULT hr = factory->EnumAdapters1(0, adapter.GetAddressOf());
  if (FAILED(hr))
    Log_ErrorPrintf("IDXGIFactory2::EnumAdapters() for first adapter returned %08X", hr);

  return adapter;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> D3DCommon::GetChosenOrFirstAdapter(IDXGIFactory5* factory,
                                                                         const std::string_view& name)
{
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter = GetAdapterByName(factory, name);
  if (!adapter)
    adapter = GetFirstAdapter(factory);

  return adapter;
}

std::string D3DCommon::GetAdapterName(IDXGIAdapter1* adapter)
{
  std::string ret;

  DXGI_ADAPTER_DESC1 desc;
  HRESULT hr = adapter->GetDesc1(&desc);
  if (SUCCEEDED(hr))
  {
    ret = StringUtil::WideStringToUTF8String(desc.Description);
  }
  else
  {
    Log_ErrorPrintf("IDXGIAdapter1::GetDesc() returned %08X", hr);
  }

  if (ret.empty())
    ret = "(Unknown)";

  return ret;
}

std::string D3DCommon::GetDriverVersionFromLUID(const LUID& luid)
{
  std::string ret;

  HKEY hKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\DirectX", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
  {
    DWORD max_key_len = 0, adapter_count = 0;
    if (RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &adapter_count, &max_key_len, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr) == ERROR_SUCCESS)
    {
      std::vector<WCHAR> current_name(max_key_len + 1);
      for (DWORD i = 0; i < adapter_count; ++i)
      {
        DWORD subKeyLength = static_cast<DWORD>(current_name.size());
        if (RegEnumKeyExW(hKey, i, current_name.data(), &subKeyLength, nullptr, nullptr, nullptr, nullptr) ==
            ERROR_SUCCESS)
        {
          LUID current_luid = {};
          DWORD current_luid_size = sizeof(uint64_t);
          if (RegGetValueW(hKey, current_name.data(), L"AdapterLuid", RRF_RT_QWORD, nullptr, &current_luid,
                           &current_luid_size) == ERROR_SUCCESS &&
              current_luid.HighPart == luid.HighPart && current_luid.LowPart == luid.LowPart)
          {
            LARGE_INTEGER driver_version = {};
            DWORD driver_version_size = sizeof(driver_version);
            if (RegGetValueW(hKey, current_name.data(), L"DriverVersion", RRF_RT_QWORD, nullptr, &driver_version,
                             &driver_version_size) == ERROR_SUCCESS)
            {
              WORD nProduct = HIWORD(driver_version.HighPart);
              WORD nVersion = LOWORD(driver_version.HighPart);
              WORD nSubVersion = HIWORD(driver_version.LowPart);
              WORD nBuild = LOWORD(driver_version.LowPart);
              ret = fmt::format("{}.{}.{}.{}", nProduct, nVersion, nSubVersion, nBuild);
            }
          }
        }
      }
    }

    RegCloseKey(hKey);
  }

  return ret;
}

std::optional<DynamicHeapArray<u8>> D3DCommon::CompileShader(D3D_FEATURE_LEVEL feature_level, bool debug_device,
                                                             GPUShaderStage stage, const std::string_view& source,
                                                             const char* entry_point)
{
  const char* target;
  switch (feature_level)
  {
    case D3D_FEATURE_LEVEL_10_0:
    {
      static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> targets = {
        {"vs_4_0", "ps_4_0", "gs_4_0", "cs_4_0"}};
      target = targets[static_cast<int>(stage)];
    }
    break;

    case D3D_FEATURE_LEVEL_10_1:
    {
      static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> targets = {
        {"vs_4_1", "ps_4_1", "gs_4_0", "cs_4_1"}};
      target = targets[static_cast<int>(stage)];
    }
    break;

    case D3D_FEATURE_LEVEL_11_0:
    {
      static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> targets = {
        {"vs_5_0", "ps_5_0", "gs_5_0", "cs_5_0"}};
      target = targets[static_cast<int>(stage)];
    }
    break;

    case D3D_FEATURE_LEVEL_11_1:
    default:
    {
      static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> targets = {
        {"vs_5_1", "ps_5_1", "gs_5_1", "cs_5_1"}};
      target = targets[static_cast<int>(stage)];
    }
    break;
  }

  static constexpr UINT flags_non_debug = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  static constexpr UINT flags_debug = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;

  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> error_blob;
  const HRESULT hr =
    D3DCompile(source.data(), source.size(), "0", nullptr, nullptr, entry_point, target,
               debug_device ? flags_debug : flags_non_debug, 0, blob.GetAddressOf(), error_blob.GetAddressOf());

  std::string_view error_string;
  if (error_blob)
    error_string =
      std::string_view(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());

  if (FAILED(hr))
  {
    Log_ErrorFmt("Failed to compile '{}':\n{}", target, error_string);
    GPUDevice::DumpBadShader(source, error_string);
    return {};
  }

  if (!error_string.empty())
    Log_WarningFmt("'{}' compiled with warnings:\n{}", target, error_string);

  error_blob.Reset();

  return DynamicHeapArray<u8>(static_cast<const u8*>(blob->GetBufferPointer()), blob->GetBufferSize());
}

static constexpr std::array<D3DCommon::DXGIFormatMapping, static_cast<int>(GPUTexture::Format::MaxCount)>
  s_format_mapping = {{
    // clang-format off
  // d3d_format                    srv_format                      rtv_format                      dsv_format
  {DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN   }, // Unknown
  {DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_UNKNOWN   }, // RGBA8
  {DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_UNKNOWN   }, // BGRA8
  {DXGI_FORMAT_B5G6R5_UNORM,       DXGI_FORMAT_B5G6R5_UNORM,       DXGI_FORMAT_B5G6R5_UNORM,       DXGI_FORMAT_UNKNOWN   }, // RGB565
  {DXGI_FORMAT_B5G5R5A1_UNORM,     DXGI_FORMAT_B5G5R5A1_UNORM,     DXGI_FORMAT_B5G5R5A1_UNORM,     DXGI_FORMAT_UNKNOWN   }, // RGBA5551
  {DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_UNKNOWN   }, // R8
  {DXGI_FORMAT_R16_TYPELESS,       DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_D16_UNORM }, // D16
  {DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_UNKNOWN   }, // R16
  {DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_UNKNOWN   }, // R16I
  {DXGI_FORMAT_R16_UINT,           DXGI_FORMAT_R16_UINT,           DXGI_FORMAT_R16_UINT,           DXGI_FORMAT_UNKNOWN   }, // R16U
  {DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_UNKNOWN   }, // R16F
  {DXGI_FORMAT_R32_SINT,           DXGI_FORMAT_R32_SINT,           DXGI_FORMAT_R32_SINT,           DXGI_FORMAT_UNKNOWN   }, // R32I
  {DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_UNKNOWN   }, // R32U
  {DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_UNKNOWN   }, // R32F
  {DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_UNKNOWN   }, // RG8
  {DXGI_FORMAT_R16G16_UNORM,       DXGI_FORMAT_R16G16_UNORM,       DXGI_FORMAT_R16G16_UNORM,       DXGI_FORMAT_UNKNOWN   }, // RG16
  {DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_UNKNOWN   }, // RG16F
  {DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_UNKNOWN   }, // RG32F
  {DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_UNKNOWN   }, // RGBA16
  {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN   }, // RGBA16F
  {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_UNKNOWN   }, // RGBA32F
  {DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_UNKNOWN   }, // RGB10A2
    // clang-format on
  }};

const D3DCommon::DXGIFormatMapping& D3DCommon::GetFormatMapping(GPUTexture::Format format)
{
  DebugAssert(static_cast<u8>(format) < s_format_mapping.size());
  return s_format_mapping[static_cast<u8>(format)];
}

GPUTexture::Format D3DCommon::GetFormatForDXGIFormat(DXGI_FORMAT format)
{
  for (u32 i = 0; i < static_cast<u32>(GPUTexture::Format::MaxCount); i++)
  {
    if (s_format_mapping[i].resource_format == format)
      return static_cast<GPUTexture::Format>(i);
  }

  return GPUTexture::Format::Unknown;
}
