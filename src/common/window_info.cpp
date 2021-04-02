#include "window_info.h"
#include "common/log.h"
Log_SetChannel(WindowInfo);

#if defined(_WIN32)

#include "common/windows_headers.h"
#include <dwmapi.h>

static bool GetRefreshRateFromDWM(HWND hwnd, float* refresh_rate)
{
  static HMODULE dwm_module = nullptr;
  static HRESULT(STDAPICALLTYPE * is_composition_enabled)(BOOL * pfEnabled) = nullptr;
  static HRESULT(STDAPICALLTYPE * get_timing_info)(HWND hwnd, DWM_TIMING_INFO * pTimingInfo) = nullptr;
  static bool load_tried = false;
  if (!load_tried)
  {
    load_tried = true;
    dwm_module = LoadLibrary("dwmapi.dll");
    if (dwm_module)
    {
      std::atexit([]() {
        FreeLibrary(dwm_module);
        dwm_module = nullptr;
      });
      is_composition_enabled =
        reinterpret_cast<decltype(is_composition_enabled)>(GetProcAddress(dwm_module, "DwmIsCompositionEnabled"));
      get_timing_info =
        reinterpret_cast<decltype(get_timing_info)>(GetProcAddress(dwm_module, "DwmGetCompositionTimingInfo"));
    }
  }

  BOOL composition_enabled;
  if (!is_composition_enabled || FAILED(is_composition_enabled(&composition_enabled) || !get_timing_info))
    return false;

  DWM_TIMING_INFO ti = {};
  ti.cbSize = sizeof(ti);
  HRESULT hr = get_timing_info(nullptr, &ti);
  if (SUCCEEDED(hr))
  {
    if (ti.rateRefresh.uiNumerator == 0 || ti.rateRefresh.uiDenominator == 0)
      return false;

    *refresh_rate = static_cast<float>(ti.rateRefresh.uiNumerator) / static_cast<float>(ti.rateRefresh.uiDenominator);
    return true;
  }

  return false;
}

static bool GetRefreshRateFromMonitor(HWND hwnd, float* refresh_rate)
{
  HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (!mon)
    return false;

  MONITORINFOEXW mi = {};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi))
  {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);

    // 0/1 are reserved for "defaults".
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    {
      *refresh_rate = static_cast<float>(dm.dmDisplayFrequency);
      return true;
    }
  }

  return false;
}

bool WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi, float* refresh_rate)
{
  if (wi.type != Type::Win32 || !wi.window_handle)
    return false;

  // Try DWM first, then fall back to integer values.
  const HWND hwnd = static_cast<HWND>(wi.window_handle);
  return GetRefreshRateFromDWM(hwnd, refresh_rate) || GetRefreshRateFromMonitor(hwnd, refresh_rate);
}

#elif defined(__linux__)

#ifdef USE_X11

#include "common/scope_guard.h"
#include "gl/x11_window.h"
#include <X11/extensions/Xrandr.h>

static bool GetRefreshRateFromXRandR(const WindowInfo& wi, float* refresh_rate)
{
  Display* display = static_cast<Display*>(wi.display_connection);
  Window window = static_cast<Window>(reinterpret_cast<uintptr_t>(wi.window_handle));
  if (!display || !window)
    return false;

  GL::X11InhibitErrors inhibiter;

  XRRScreenResources* res = XRRGetScreenResources(display, window);
  if (!res)
  {
    Log_ErrorPrint("XRRGetScreenResources() failed");
    return false;
  }

  Common::ScopeGuard res_guard([res]() { XRRFreeScreenResources(res); });

  int num_monitors;
  XRRMonitorInfo* mi = XRRGetMonitors(display, window, True, &num_monitors);
  if (num_monitors < 0)
  {
    Log_ErrorPrint("XRRGetMonitors() failed");
    return false;
  }
  else if (num_monitors > 1)
  {
    Log_WarningPrintf("XRRGetMonitors() returned %d monitors, using first", num_monitors);
  }

  Common::ScopeGuard mi_guard([mi]() { XRRFreeMonitors(mi); });
  if (mi->noutput <= 0)
  {
    Log_ErrorPrint("Monitor has no outputs");
    return false;
  }
  else if (mi->noutput > 1)
  {
    Log_WarningPrintf("Monitor has %d outputs, using first", mi->noutput);
  }

  XRROutputInfo* oi = XRRGetOutputInfo(display, res, mi->outputs[0]);
  if (!oi)
  {
    Log_ErrorPrint("XRRGetOutputInfo() failed");
    return false;
  }

  Common::ScopeGuard oi_guard([oi]() { XRRFreeOutputInfo(oi); });

  XRRCrtcInfo* ci = XRRGetCrtcInfo(display, res, oi->crtc);
  if (!ci)
  {
    Log_ErrorPrint("XRRGetCrtcInfo() failed");
    return false;
  }

  Common::ScopeGuard ci_guard([ci]() { XRRFreeCrtcInfo(ci); });

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
    Log_ErrorPrintf("Failed to look up mode %d (of %d)", static_cast<int>(ci->mode), res->nmode);
    return false;
  }

  if (mode->dotClock == 0 || mode->hTotal == 0 || mode->vTotal == 0)
  {
    Log_ErrorPrintf("Modeline is invalid: %ld/%d/%d", mode->dotClock, mode->hTotal, mode->vTotal);
    return false;
  }

  *refresh_rate =
    static_cast<double>(mode->dotClock) / (static_cast<double>(mode->hTotal) * static_cast<double>(mode->vTotal));
  return true;
}

#endif // USE_X11

bool WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi, float* refresh_rate)
{
#if defined(USE_X11)
  if (wi.type == WindowInfo::Type::X11)
    return GetRefreshRateFromXRandR(wi, refresh_rate);
#endif

  return false;
}

#endif