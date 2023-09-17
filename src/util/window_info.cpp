// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "window_info.h"

#include "common/assert.h"

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

static bool GetRefreshRateFromDWM(HWND hwnd, float* refresh_rate)
{
  BOOL composition_enabled;
  if (FAILED(DwmIsCompositionEnabled(&composition_enabled)))
    return false;

  DWM_TIMING_INFO ti = {};
  ti.cbSize = sizeof(ti);
  HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &ti);
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

#else

#ifdef ENABLE_X11

#include "common/scoped_guard.h"
#include "common/log.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

Log_SetChannel(WindowInfo);

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
    Log_WarningPrintf("X11 Error: %s (Error %u Minor %u Request %u)", error_string, ee->error_code, ee->minor_code,
                      ee->request_code);

    s_current_error_inhibiter->m_had_error = true;
    return 0;
  }

  XErrorHandler m_old_handler = {};
  bool m_had_error = false;
};
} // namespace

static bool GetRefreshRateFromXRandR(const WindowInfo& wi, float* refresh_rate)
{
  Display* display = static_cast<Display*>(wi.display_connection);
  Window window = static_cast<Window>(reinterpret_cast<uintptr_t>(wi.window_handle));
  if (!display || !window)
    return false;

  X11InhibitErrors inhibiter;

  XRRScreenResources* res = XRRGetScreenResources(display, window);
  if (!res)
  {
    Log_ErrorPrint("XRRGetScreenResources() failed");
    return false;
  }

  ScopedGuard res_guard([res]() { XRRFreeScreenResources(res); });

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

  ScopedGuard mi_guard([mi]() { XRRFreeMonitors(mi); });
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

  ScopedGuard oi_guard([oi]() { XRRFreeOutputInfo(oi); });

  XRRCrtcInfo* ci = XRRGetCrtcInfo(display, res, oi->crtc);
  if (!ci)
  {
    Log_ErrorPrint("XRRGetCrtcInfo() failed");
    return false;
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

#endif // ENABLE_X11

bool WindowInfo::QueryRefreshRateForWindow(const WindowInfo& wi, float* refresh_rate)
{
#if defined(ENABLE_X11)
  if (wi.type == WindowInfo::Type::X11)
    return GetRefreshRateFromXRandR(wi, refresh_rate);
#endif

  return false;
}

#endif