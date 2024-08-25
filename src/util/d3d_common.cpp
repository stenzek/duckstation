// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR PolyForm-Strict-1.0.0)

#include "d3d_common.h"
#include "gpu_device.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/gsvector.h"
#include "common/log.h"
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
    {D3D_FEATURE_LEVEL_11_1, "sm50"},
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
    WARNING_LOG("D3D11CreateDevice() for getting max feature level failed: 0x{:08X}", static_cast<unsigned>(hr));

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

static std::string FixupDuplicateAdapterNames(const GPUDevice::AdapterInfoList& adapter_names, std::string adapter_name)
{
  if (std::any_of(adapter_names.begin(), adapter_names.end(),
                  [&adapter_name](const GPUDevice::AdapterInfo& other) { return (adapter_name == other.name); }))
  {
    std::string original_adapter_name = std::move(adapter_name);

    u32 current_extra = 2;
    do
    {
      adapter_name = fmt::format("{} ({})", original_adapter_name.c_str(), current_extra);
      current_extra++;
    } while (
      std::any_of(adapter_names.begin(), adapter_names.end(),
                  [&adapter_name](const GPUDevice::AdapterInfo& other) { return (adapter_name == other.name); }));
  }

  return adapter_name;
}

GPUDevice::AdapterInfoList D3DCommon::GetAdapterInfoList()
{
  GPUDevice::AdapterInfoList adapters;

  Microsoft::WRL::ComPtr<IDXGIFactory5> factory = CreateFactory(false, nullptr);
  if (!factory)
    return adapters;

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (u32 index = 0;; index++)
  {
    HRESULT hr = factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf());
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;

    if (FAILED(hr))
    {
      ERROR_LOG("IDXGIFactory2::EnumAdapters() returned {:08X}", static_cast<unsigned>(hr));
      continue;
    }

    // Unfortunately we can't get any properties such as feature level without creating the device.
    // So just assume a max of the D3D11 max across the board.
    GPUDevice::AdapterInfo ai;
    ai.name = FixupDuplicateAdapterNames(adapters, GetAdapterName(adapter.Get()));
    ai.max_texture_size = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    ai.max_multisamples = 8;
    ai.supports_sample_shading = true;

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(hr = adapter->EnumOutputs(0, output.ReleaseAndGetAddressOf())))
    {
      UINT num_modes = 0;
      if (SUCCEEDED(hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num_modes, nullptr)))
      {
        std::vector<DXGI_MODE_DESC> dmodes(num_modes);
        if (SUCCEEDED(hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num_modes, dmodes.data())))
        {
          for (const DXGI_MODE_DESC& mode : dmodes)
          {
            ai.fullscreen_modes.push_back(GPUDevice::GetFullscreenModeString(
              mode.Width, mode.Height,
              static_cast<float>(mode.RefreshRate.Numerator) / static_cast<float>(mode.RefreshRate.Denominator)));
          }
        }
        else
        {
          ERROR_LOG("GetDisplayModeList() (2) failed: {:08X}", static_cast<unsigned>(hr));
        }
      }
      else
      {
        ERROR_LOG("GetDisplayModeList() failed: {:08X}", static_cast<unsigned>(hr));
      }
    }
    else
    {
      // Adapter may not have any outputs, don't spam the error log in this case.
      if (hr != DXGI_ERROR_NOT_FOUND)
        ERROR_LOG("EnumOutputs() failed: {:08X}", static_cast<unsigned>(hr));
    }

    adapters.push_back(std::move(ai));
  }

  return adapters;
}

bool D3DCommon::GetRequestedExclusiveFullscreenModeDesc(IDXGIFactory5* factory, const RECT& window_rect, u32 width,
                                                        u32 height, float refresh_rate, DXGI_FORMAT format,
                                                        DXGI_MODE_DESC* fullscreen_mode, IDXGIOutput** output)
{
  // We need to find which monitor the window is located on.
  const GSVector4i client_rc_vec(window_rect.left, window_rect.top, window_rect.right, window_rect.bottom);

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

      const GSVector4i output_rc(output_desc.DesktopCoordinates.left, output_desc.DesktopCoordinates.top,
                                 output_desc.DesktopCoordinates.right, output_desc.DesktopCoordinates.bottom);
      if (!client_rc_vec.rintersects(output_rc))
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
      ERROR_LOG("No DXGI output found. Can't use exclusive fullscreen.");
      return false;
    }

    WARNING_LOG("No DXGI output found for window, using first.");
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
    ERROR_LOG("Failed to find closest matching mode, hr={:08X}", static_cast<unsigned>(hr));
    return false;
  }

  *output = intersecting_output.Get();
  intersecting_output->AddRef();
  return true;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> D3DCommon::GetAdapterByName(IDXGIFactory5* factory, std::string_view name)
{
  if (name.empty())
    return {};

  // This might seem a bit odd to cache the names.. but there's a method to the madness.
  // We might have two GPUs with the same name... :)
  GPUDevice::AdapterInfoList adapters;

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (u32 index = 0;; index++)
  {
    const HRESULT hr = factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf());
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;

    if (FAILED(hr))
    {
      ERROR_LOG("IDXGIFactory2::EnumAdapters() returned {:08X}", static_cast<unsigned>(hr));
      continue;
    }

    std::string adapter_name = FixupDuplicateAdapterNames(adapters, GetAdapterName(adapter.Get()));
    if (adapter_name == name)
    {
      VERBOSE_LOG("Found adapter '{}'", adapter_name);
      return adapter;
    }

    GPUDevice::AdapterInfo ai;
    ai.name = std::move(adapter_name);
    adapters.push_back(std::move(ai));
  }

  ERROR_LOG("Adapter '{}' not found.", name);
  return {};
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> D3DCommon::GetFirstAdapter(IDXGIFactory5* factory)
{
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  HRESULT hr = factory->EnumAdapters1(0, adapter.GetAddressOf());
  if (FAILED(hr))
    ERROR_LOG("IDXGIFactory2::EnumAdapters() for first adapter returned {:08X}", static_cast<unsigned>(hr));

  return adapter;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> D3DCommon::GetChosenOrFirstAdapter(IDXGIFactory5* factory, std::string_view name)
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
    ERROR_LOG("IDXGIAdapter1::GetDesc() returned {:08X}", static_cast<unsigned>(hr));
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

u32 D3DCommon::GetShaderModelForFeatureLevel(D3D_FEATURE_LEVEL feature_level)
{
  switch (feature_level)
  {
    case D3D_FEATURE_LEVEL_10_0:
      return 40;

    case D3D_FEATURE_LEVEL_10_1:
      return 41;

    case D3D_FEATURE_LEVEL_11_0:
    case D3D_FEATURE_LEVEL_11_1:
    default:
      return 50;
  }
}

std::optional<DynamicHeapArray<u8>> D3DCommon::CompileShader(u32 shader_model, bool debug_device, GPUShaderStage stage,
                                                             std::string_view source, const char* entry_point,
                                                             Error* error)
{
  const char* target;
  switch (shader_model)
  {
    case 40:
    {
      static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> targets = {
        {"vs_4_0", "ps_4_0", "gs_4_0", "cs_4_0"}};
      target = targets[static_cast<int>(stage)];
    }
    break;

    case 41:
    {
      static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> targets = {
        {"vs_4_1", "ps_4_1", "gs_4_0", "cs_4_1"}};
      target = targets[static_cast<int>(stage)];
    }
    break;

    case 50:
    {
      static constexpr std::array<const char*, static_cast<u32>(GPUShaderStage::MaxCount)> targets = {
        {"vs_5_0", "ps_5_0", "gs_5_0", "cs_5_0"}};
      target = targets[static_cast<int>(stage)];
    }
    break;

    default:
      Error::SetStringFmt(error, "Unknown shader model: {}", shader_model);
      return {};
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
  {
    error_string =
      std::string_view(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
  }

  if (FAILED(hr))
  {
    ERROR_LOG("Failed to compile '{}':\n{}", target, error_string);
    GPUDevice::DumpBadShader(source, error_string);
    Error::SetHResult(error, "D3DCompile() failed: ", hr);
    return {};
  }

  if (!error_string.empty())
    WARNING_LOG("'{}' compiled with warnings:\n{}", target, error_string);

  error_blob.Reset();

  return DynamicHeapArray<u8>(static_cast<const u8*>(blob->GetBufferPointer()), blob->GetBufferSize());
}

static constexpr std::array<D3DCommon::DXGIFormatMapping, static_cast<int>(GPUTexture::Format::MaxCount)>
  s_format_mapping = {{
    // clang-format off
  // d3d_format                    srv_format                           rtv_format                      dsv_format
  {DXGI_FORMAT_UNKNOWN,            DXGI_FORMAT_UNKNOWN,                 DXGI_FORMAT_UNKNOWN,                  DXGI_FORMAT_UNKNOWN               }, // Unknown
  {DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R8G8B8A8_UNORM,          DXGI_FORMAT_R8G8B8A8_UNORM,           DXGI_FORMAT_UNKNOWN               }, // RGBA8
  {DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_B8G8R8A8_UNORM,          DXGI_FORMAT_B8G8R8A8_UNORM,           DXGI_FORMAT_UNKNOWN               }, // BGRA8
  {DXGI_FORMAT_B5G6R5_UNORM,       DXGI_FORMAT_B5G6R5_UNORM,            DXGI_FORMAT_B5G6R5_UNORM,             DXGI_FORMAT_UNKNOWN               }, // RGB565
  {DXGI_FORMAT_B5G5R5A1_UNORM,     DXGI_FORMAT_B5G5R5A1_UNORM,          DXGI_FORMAT_B5G5R5A1_UNORM,           DXGI_FORMAT_UNKNOWN               }, // RGBA5551
  {DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8_UNORM,                DXGI_FORMAT_R8_UNORM,                 DXGI_FORMAT_UNKNOWN               }, // R8
  {DXGI_FORMAT_R16_TYPELESS,       DXGI_FORMAT_R16_UNORM,               DXGI_FORMAT_UNKNOWN,                  DXGI_FORMAT_D16_UNORM             }, // D16
  {DXGI_FORMAT_R24G8_TYPELESS,     DXGI_FORMAT_R24_UNORM_X8_TYPELESS,   DXGI_FORMAT_R24_UNORM_X8_TYPELESS,    DXGI_FORMAT_D24_UNORM_S8_UINT     }, // D24S8
  {DXGI_FORMAT_R32_TYPELESS,       DXGI_FORMAT_R32_FLOAT,               DXGI_FORMAT_R32_FLOAT,                DXGI_FORMAT_D32_FLOAT             }, // D32F
  {DXGI_FORMAT_R32G8X24_TYPELESS,  DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT  }, // D32FS8
  {DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_R16_UNORM,               DXGI_FORMAT_R16_UNORM,                DXGI_FORMAT_UNKNOWN               }, // R16
  {DXGI_FORMAT_R16_SINT,           DXGI_FORMAT_R16_SINT,                DXGI_FORMAT_R16_SINT,                 DXGI_FORMAT_UNKNOWN               }, // R16I
  {DXGI_FORMAT_R16_UINT,           DXGI_FORMAT_R16_UINT,                DXGI_FORMAT_R16_UINT,                 DXGI_FORMAT_UNKNOWN               }, // R16U
  {DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16_FLOAT,               DXGI_FORMAT_R16_FLOAT,                DXGI_FORMAT_UNKNOWN               }, // R16F
  {DXGI_FORMAT_R32_SINT,           DXGI_FORMAT_R32_SINT,                DXGI_FORMAT_R32_SINT,                 DXGI_FORMAT_UNKNOWN               }, // R32I
  {DXGI_FORMAT_R32_UINT,           DXGI_FORMAT_R32_UINT,                DXGI_FORMAT_R32_UINT,                 DXGI_FORMAT_UNKNOWN               }, // R32U
  {DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R32_FLOAT,               DXGI_FORMAT_R32_FLOAT,                DXGI_FORMAT_UNKNOWN               }, // R32F
  {DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_R8G8_UNORM,              DXGI_FORMAT_R8G8_UNORM,               DXGI_FORMAT_UNKNOWN               }, // RG8
  {DXGI_FORMAT_R16G16_UNORM,       DXGI_FORMAT_R16G16_UNORM,            DXGI_FORMAT_R16G16_UNORM,             DXGI_FORMAT_UNKNOWN               }, // RG16
  {DXGI_FORMAT_R16G16_FLOAT,       DXGI_FORMAT_R16G16_FLOAT,            DXGI_FORMAT_R16G16_FLOAT,             DXGI_FORMAT_UNKNOWN               }, // RG16F
  {DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32_FLOAT,            DXGI_FORMAT_R32G32_FLOAT,             DXGI_FORMAT_UNKNOWN               }, // RG32F
  {DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM,      DXGI_FORMAT_R16G16B16A16_UNORM,       DXGI_FORMAT_UNKNOWN               }, // RGBA16
  {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,      DXGI_FORMAT_R16G16B16A16_FLOAT,       DXGI_FORMAT_UNKNOWN               }, // RGBA16F
  {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,      DXGI_FORMAT_R32G32B32A32_FLOAT,       DXGI_FORMAT_UNKNOWN               }, // RGBA32F
  {DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_R10G10B10A2_UNORM,       DXGI_FORMAT_R10G10B10A2_UNORM,        DXGI_FORMAT_UNKNOWN               }, // RGB10A2
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
