// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "window_info.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/scoped_guard.h"

Log_SetChannel(WindowInfo);

void WindowInfo::SetSurfaceless()
{
  type = Type::Surfaceless;
  window_handle = nullptr;
  surface_width = 0;
  surface_height = 0;
  surface_refresh_rate = 0.0f;
  surface_scale = 1.0f;
  surface_format = GPUTexture::Format::Unknown;

#ifdef __APPLE__
  surface_handle = nullptr;
#endif
}

#if defined(_WIN32)

#include "common/windows_headers.h"
#include <dwmapi.h>

static std::optional<float> GetRefreshRateFromDisplayConfig(HWND hwnd)
{
  // Partially based on Chromium ui/display/win/display_config_helper.cc.
  const HMONITOR monitor = MonitorFromWindow(hwnd, 0);
  if (!monitor) [[unlikely]]
  {
    ERROR_LOG("{}() failed: {}", "MonitorFromWindow", Error::CreateWin32(GetLastError()).GetDescription());
    return std::nullopt;
  }

  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(monitor, &mi))
  {
    ERROR_LOG("{}() failed: {}", "GetMonitorInfoW", Error::CreateWin32(GetLastError()).GetDescription());
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
      ERROR_LOG("{}() failed: {}", "GetDisplayConfigBufferSizes", Error::CreateWin32(res).GetDescription());
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
      ERROR_LOG("{}() failed: {}", "QueryDisplayConfig", Error::CreateWin32(res).GetDescription());
      return std::nullopt;
    }
  }

  for (const DISPLAYCONFIG_PATH_INFO& pi : path_info)
  {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sdn = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                       .size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME),
                                                       .adapterId = pi.sourceInfo.adapterId,
                                                       .id = pi.sourceInfo.id}};
    LONG res = DisplayConfigGetDeviceInfo(&sdn.header);
    if (res != ERROR_SUCCESS)
    {
      ERROR_LOG("{}() failed: {}", "DisplayConfigGetDeviceInfo", Error::CreateWin32(res).GetDescription());
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

static std::optional<float> GetRefreshRateFromDWM(HWND hwnd)
{
  BOOL composition_enabled;
  if (FAILED(DwmIsCompositionEnabled(&composition_enabled)))
    return std::nullopt;

  DWM_TIMING_INFO ti = {};
  ti.cbSize = sizeof(ti);
  HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &ti);
  if (SUCCEEDED(hr))
  {
    if (ti.rateRefresh.uiNumerator == 0 || ti.rateRefresh.uiDenominator == 0)
      return std::nullopt;

    return static_cast<float>(ti.rateRefresh.uiNumerator) / static_cast<float>(ti.rateRefresh.uiDenominator);
  }

  return std::nullopt;
}

static std::optional<float> GetRefreshRateFromMonitor(HWND hwnd)
{
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!mon)
    return std::nullopt;

  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi))
  {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);

    // 0/1 are reserved for "defaults".
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
      return static_cast<float>(dm.dmDisplayFrequency);
  }

  return std::nullopt;
}

std::optional<float> WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi)
{
  std::optional<float> ret;
  if (wi.type != Type::Win32 || !wi.window_handle)
    return ret;

  // Try DWM first, then fall back to integer values.
  const HWND hwnd = static_cast<HWND>(wi.window_handle);
  ret = GetRefreshRateFromDisplayConfig(hwnd);
  if (!ret.has_value())
  {
    ret = GetRefreshRateFromDWM(hwnd);
    if (!ret.has_value())
      ret = GetRefreshRateFromMonitor(hwnd);
  }

  return ret;
}

#elif defined(__APPLE__)

#include "util/platform_misc.h"

std::optional<float> WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi)
{
  if (wi.type == WindowInfo::Type::MacOS)
    return CocoaTools::GetViewRefreshRate(wi);

  return std::nullopt;
}

#else

#ifdef ENABLE_X11

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

// Helper class for managing X errors
namespace {
class X11InhibitErrors;

static X11InhibitErrors* s_current_error_inhibiter;

class X11InhibitErrors
{
public:
  X11InhibitErrors()
  {
    Assert(!s_current_error_inhibiter);
    m_old_handler = XSetErrorHandler(ErrorHandler);
    s_current_error_inhibiter = this;
  }

  ~X11InhibitErrors()
  {
    Assert(s_current_error_inhibiter == this);
    s_current_error_inhibiter = nullptr;
    XSetErrorHandler(m_old_handler);
  }

  ALWAYS_INLINE bool HadError() const { return m_had_error; }

private:
  static int ErrorHandler(Display* display, XErrorEvent* ee)
  {
    char error_string[256] = {};
    XGetErrorText(display, ee->error_code, error_string, sizeof(error_string));
    WARNING_LOG("X11 Error: {} (Error {} Minor {} Request {})", error_string, ee->error_code, ee->minor_code,
                ee->request_code);

    s_current_error_inhibiter->m_had_error = true;
    return 0;
  }

  XErrorHandler m_old_handler = {};
  bool m_had_error = false;
};
} // namespace

static std::optional<float> GetRefreshRateFromXRandR(const WindowInfo& wi)
{
  Display* display = static_cast<Display*>(wi.display_connection);
  Window window = static_cast<Window>(reinterpret_cast<uintptr_t>(wi.window_handle));
  if (!display || !window)
    return std::nullopt;

  X11InhibitErrors inhibiter;

  XRRScreenResources* res = XRRGetScreenResources(display, window);
  if (!res)
  {
    ERROR_LOG("XRRGetScreenResources() failed");
    return std::nullopt;
  }

  ScopedGuard res_guard([res]() { XRRFreeScreenResources(res); });

  int num_monitors;
  XRRMonitorInfo* mi = XRRGetMonitors(display, window, True, &num_monitors);
  if (num_monitors < 0)
  {
    ERROR_LOG("XRRGetMonitors() failed");
    return std::nullopt;
  }
  else if (num_monitors > 1)
  {
    WARNING_LOG("XRRGetMonitors() returned {} monitors, using first", num_monitors);
  }

  ScopedGuard mi_guard([mi]() { XRRFreeMonitors(mi); });
  if (mi->noutput <= 0)
  {
    ERROR_LOG("Monitor has no outputs");
    return std::nullopt;
  }
  else if (mi->noutput > 1)
  {
    WARNING_LOG("Monitor has {} outputs, using first", mi->noutput);
  }

  XRROutputInfo* oi = XRRGetOutputInfo(display, res, mi->outputs[0]);
  if (!oi)
  {
    ERROR_LOG("XRRGetOutputInfo() failed");
    return std::nullopt;
  }

  ScopedGuard oi_guard([oi]() { XRRFreeOutputInfo(oi); });

  XRRCrtcInfo* ci = XRRGetCrtcInfo(display, res, oi->crtc);
  if (!ci)
  {
    ERROR_LOG("XRRGetCrtcInfo() failed");
    return std::nullopt;
  }

  ScopedGuard ci_guard([ci]() { XRRFreeCrtcInfo(ci); });

  XRRModeInfo* mode = nullptr;
  for (int i = 0; i < res->nmode; i++)
  {
    if (res->modes[i].id == ci->mode)
    {
      mode = &res->modes[i];
      break;
    }
  }
  if (!mode)
  {
    ERROR_LOG("Failed to look up mode {} (of {})", static_cast<int>(ci->mode), res->nmode);
    return std::nullopt;
  }

  if (mode->dotClock == 0 || mode->hTotal == 0 || mode->vTotal == 0)
  {
    ERROR_LOG("Modeline is invalid: {}/{}/{}", mode->dotClock, mode->hTotal, mode->vTotal);
    return std::nullopt;
  }

  return static_cast<float>(static_cast<double>(mode->dotClock) /
                            (static_cast<double>(mode->hTotal) * static_cast<double>(mode->vTotal)));
}

#endif // ENABLE_X11

std::optional<float> WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi)
{
#if defined(ENABLE_X11)
  if (wi.type == WindowInfo::Type::X11)
    return GetRefreshRateFromXRandR(wi);
#endif

  return std::nullopt;
}

#endif