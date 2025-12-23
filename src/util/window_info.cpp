// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "window_info.h"
#include "gpu_texture.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/scoped_guard.h"

#include <numbers>
#include <utility>

LOG_CHANNEL(WindowInfo);

WindowInfo::WindowInfo()
  : type(WindowInfoType::Surfaceless), surface_format(GPUTextureFormat::Unknown),
    surface_prerotation(PreRotation::Identity), surface_width(0), surface_height(0), surface_refresh_rate(0.0f),
    surface_scale(1.0f), display_connection(nullptr), window_handle(nullptr)
{
}

void WindowInfo::SetPreRotated(PreRotation prerotation)
{
  if (ShouldSwapDimensionsForPreRotation(prerotation) != ShouldSwapDimensionsForPreRotation(surface_prerotation))
    std::swap(surface_width, surface_height);

  surface_prerotation = prerotation;
}

float WindowInfo::GetZRotationForPreRotation(PreRotation prerotation)
{
  static constexpr const std::array<float, 4> rotation_radians = {{
    0.0f,                                        // Identity
    static_cast<float>(std::numbers::pi * 1.5f), // Rotate90Clockwise
    static_cast<float>(std::numbers::pi),        // Rotate180Clockwise
    static_cast<float>(std::numbers::pi / 2.0),  // Rotate270Clockwise
  }};

  return rotation_radians[static_cast<size_t>(prerotation)];
}

#if defined(_WIN32)

#include "common/windows_headers.h"
#include <dwmapi.h>

static std::optional<float> GetRefreshRateFromDisplayConfig(HWND hwnd, Error* error)
{
  // Partially based on Chromium ui/display/win/display_config_helper.cc.
  const HMONITOR monitor = MonitorFromWindow(hwnd, 0);
  if (!monitor) [[unlikely]]
  {
    Error::SetWin32(error, "MonitorFromWindow() failed: ", GetLastError());
    return std::nullopt;
  }

  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi))
  {
    Error::SetWin32(error, "GetMonitorInfoW() failed: ", GetLastError());
    return std::nullopt;
  }

  DynamicHeapArray<DISPLAYCONFIG_PATH_INFO> path_info;
  DynamicHeapArray<DISPLAYCONFIG_MODE_INFO> mode_info;

  // I guess this could fail if it changes inbetween two calls... unlikely.
  for (;;)
  {
    UINT32 path_size = 0, mode_size = 0;
    LONG res = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_size, &mode_size);
    if (res != ERROR_SUCCESS)
    {
      Error::SetWin32(error, "GetDisplayConfigBufferSizes() failed: ", res);
      return std::nullopt;
    }

    path_info.resize(path_size);
    mode_info.resize(mode_size);
    res =
      QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_size, path_info.data(), &mode_size, mode_info.data(), nullptr);
    if (res == ERROR_SUCCESS)
      break;
    if (res != ERROR_INSUFFICIENT_BUFFER)
    {
      Error::SetWin32(error, "QueryDisplayConfig() failed: ", res);
      return std::nullopt;
    }
  }

  for (const DISPLAYCONFIG_PATH_INFO& pi : path_info)
  {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sdn = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                       .size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME),
                                                       .adapterId = pi.sourceInfo.adapterId,
                                                       .id = pi.sourceInfo.id},
                                            .viewGdiDeviceName = {}};
    LONG res = DisplayConfigGetDeviceInfo(&sdn.header);
    if (res != ERROR_SUCCESS)
    {
      Error::SetWin32(error, "DisplayConfigGetDeviceInfo() failed: ", res);
      continue;
    }

    if (std::wcscmp(sdn.viewGdiDeviceName, mi.szDevice) == 0)
    {
      // Found the monitor!
      return static_cast<float>(static_cast<double>(pi.targetInfo.refreshRate.Numerator) /
                                static_cast<double>(pi.targetInfo.refreshRate.Denominator));
    }
  }

  return std::nullopt;
}

static std::optional<float> GetRefreshRateFromDWM(HWND hwnd, Error* error)
{
  BOOL composition_enabled;
  HRESULT hr = DwmIsCompositionEnabled(&composition_enabled);
  if (FAILED(hr))
  {
    Error::SetHResult(error, "DwmIsCompositionEnabled() failed: ", hr);
    return std::nullopt;
  }

  DWM_TIMING_INFO ti = {};
  ti.cbSize = sizeof(ti);
  hr = DwmGetCompositionTimingInfo(nullptr, &ti);
  if (SUCCEEDED(hr))
  {
    if (ti.rateRefresh.uiNumerator == 0 || ti.rateRefresh.uiDenominator == 0)
      return std::nullopt;

    return static_cast<float>(ti.rateRefresh.uiNumerator) / static_cast<float>(ti.rateRefresh.uiDenominator);
  }
  else
  {
    Error::SetHResult(error, "DwmGetCompositionTimingInfo() failed: ", hr);
    return std::nullopt;
  }
}

static std::optional<float> GetRefreshRateFromMonitor(HWND hwnd, Error* error)
{
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!mon)
  {
    Error::SetWin32(error, "MonitorFromWindow() failed: ", GetLastError());
    return std::nullopt;
  }

  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi))
  {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);

    // 0/1 are reserved for "defaults".
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    {
      return static_cast<float>(dm.dmDisplayFrequency);
    }
    else
    {
      Error::SetWin32(error, "EnumDisplaySettingsW() failed: ", GetLastError());
      return std::nullopt;
    }
  }
  else
  {
    Error::SetWin32(error, "GetMonitorInfoW() failed: ", GetLastError());
    return std::nullopt;
  }
}

std::optional<float> WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi, Error* error)
{
  std::optional<float> ret;
  if (wi.type != WindowInfoType::Win32 || !wi.window_handle)
  {
    Error::SetStringView(error, "Invalid window type.");
    return ret;
  }

  // Try DWM first, then fall back to integer values.
  const HWND hwnd = static_cast<HWND>(wi.window_handle);
  Error local_error;
  ret = GetRefreshRateFromDisplayConfig(hwnd, &local_error);
  if (!ret.has_value())
  {
    WARNING_LOG("GetRefreshRateFromDisplayConfig() failed: {}", local_error.GetDescription());

    ret = GetRefreshRateFromDWM(hwnd, &local_error);
    if (!ret.has_value())
    {
      WARNING_LOG("GetRefreshRateFromDWM() failed: {}", local_error.GetDescription());

      ret = GetRefreshRateFromMonitor(hwnd, error);
    }
  }

  return ret;
}

#elif defined(__APPLE__)

#include "util/platform_misc.h"

std::optional<float> WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi, Error* error)
{
  if (wi.type == WindowInfoType::MacOS)
    return CocoaTools::GetViewRefreshRate(wi, error);

  Error::SetStringView(error, "Invalid window type.");
  return std::nullopt;
}

#else

#ifdef ENABLE_X11
#include "x11_tools.h"
#endif

std::optional<float> WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi, Error* error)
{
#if defined(ENABLE_X11)
  if (wi.type == WindowInfoType::Xlib || wi.type == WindowInfoType::XCB)
    return GetRefreshRateFromXRandR(wi, error);
#endif

  Error::SetStringView(error, "Invalid window type.");
  return std::nullopt;
}

#endif
