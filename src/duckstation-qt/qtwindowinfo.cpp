// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qtwindowinfo.h"
#include "qtutils.h"

#include "core/core.h"
#include "core/video_thread.h"

#include "util/gpu_device.h"

#include "common/error.h"
#include "common/log.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QWidget>

#if defined(_WIN32)

#include "common/windows_headers.h"
#include <dwmapi.h>

#elif defined(__APPLE__)

#include "common/cocoa_tools.h"
#include <CoreFoundation/CFString.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#elif defined(__linux__)

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#include <qpa/qplatformnativeinterface.h>

#else

#include <qpa/qplatformnativeinterface.h>

#endif

LOG_CHANNEL(Host);

using namespace Qt::StringLiterals;

namespace {

struct WindowInfoLocals
{
  bool screensaver_inhibited;

#if defined(__APPLE__)
  IOPMAssertionID screensaver_inhibit_assertion;
#elif defined(__linux__)
  u32 screensaver_inhibit_cookie;
  std::optional<QDBusInterface> screensaver_inhibit_interface;
#endif
};

} // namespace

static WindowInfoLocals s_window_info_locals;

WindowInfoType QtUtils::GetWindowInfoType()
{
  // Windows and Apple are easy here since there's no display connection.
#if defined(_WIN32)
  return WindowInfoType::Win32;
#elif defined(__APPLE__)
  return WindowInfoType::MacOS;
#else
  const QString platform_name = QGuiApplication::platformName();
  if (platform_name == "xcb"_L1)
  {
    // This is only used for determining the automatic Vulkan renderer, therefore XCB/XLib doesn't matter here.
    // See the comment below for information about this bullshit.
    return WindowInfoType::XCB;
  }
  else if (platform_name == "wayland"_L1)
  {
    return WindowInfoType::Wayland;
  }
  else
  {
    return WindowInfoType::Surfaceless;
  }
#endif
}

std::optional<WindowInfo> QtUtils::GetWindowInfoForWidget(QWidget* widget, RenderAPI render_api, Error* error)
{
  WindowInfo wi = {};

  // Windows and Apple are easy here since there's no display connection.
#if defined(_WIN32)
  wi.type = WindowInfoType::Win32;
  wi.window_handle = reinterpret_cast<void*>(widget->winId());
#elif defined(__APPLE__)
  wi.type = WindowInfoType::MacOS;
  wi.window_handle = reinterpret_cast<void*>(widget->winId());
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  const QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("xcb"))
  {
    // This is fucking ridiculous. NVIDIA+XWayland doesn't support Xlib, and NVIDIA+Xorg doesn't support XCB.
    // Use Xlib if we're not running under Wayland, or we're not requesting OpenGL. Vulkan+XCB seems fine.
    const char* xdg_session_type = std::getenv("XDG_SESSION_TYPE");
    const bool is_running_on_xwayland = (xdg_session_type && std::strstr(xdg_session_type, "wayland"));
    if (is_running_on_xwayland || render_api == RenderAPI::Vulkan)
    {
      wi.type = WindowInfoType::XCB;
      wi.display_connection = pni->nativeResourceForWindow("connection", widget->windowHandle());
    }
    else
    {
      wi.type = WindowInfoType::Xlib;
      wi.display_connection = pni->nativeResourceForWindow("display", widget->windowHandle());
    }
    wi.window_handle = reinterpret_cast<void*>(widget->winId());
  }
  else if (platform_name == QStringLiteral("wayland"))
  {
    wi.type = WindowInfoType::Wayland;
    wi.display_connection = pni->nativeResourceForWindow("display", widget->windowHandle());
    wi.window_handle = pni->nativeResourceForWindow("surface", widget->windowHandle());
  }
  else
  {
    Error::SetStringFmt(error, "Unknown PNI platform {}", platform_name.toStdString());
    return std::nullopt;
  }
#endif

  UpdateSurfaceSize(widget, render_api, &wi);
  UpdateSurfaceRefreshRate(widget, &wi);

  INFO_LOG("Window size: {}x{} (Qt {}x{}), scale: {}, refresh rate {} hz", wi.surface_width, wi.surface_height,
           widget->width(), widget->height(), wi.surface_scale, wi.surface_refresh_rate);

  return wi;
}

void QtUtils::UpdateSurfaceSize(QWidget* widget, RenderAPI render_api, WindowInfo* wi)
{
  // Why this nonsense? Qt's device independent sizes are integer, and fractional scaling is lossy.
  // We can't get back the "real" size of the window. So we have to platform natively query the actual client size.
#if defined(_WIN32)
  if (RECT rc; GetClientRect(static_cast<HWND>(wi->window_handle), &rc))
  {
    const qreal device_pixel_ratio = widget->devicePixelRatio();
    wi->surface_width = static_cast<u16>(rc.right - rc.left);
    wi->surface_height = static_cast<u16>(rc.bottom - rc.top);
    wi->surface_scale = static_cast<float>(device_pixel_ratio);
    return;
  }
#elif defined(__APPLE__)
  if (std::optional<std::pair<int, int>> size =
        CocoaTools::GetViewSizeInPixels(reinterpret_cast<void*>(widget->winId())))
  {
    wi->surface_width = static_cast<u16>(size->first);
    wi->surface_height = static_cast<u16>(size->second);
    wi->surface_scale = static_cast<float>(widget->devicePixelRatio());

    // Only use "real" fractional window scale for Metal renderer.
    // Vulkan returns suboptimal constantly, triggering swap chain recreations.
    if (render_api == RenderAPI::Metal && Core::GetBaseBoolSettingValue("Main", "UseFractionalWindowScale", false))
    {
      if (const std::optional<double> real_device_pixel_ratio = CocoaTools::GetViewRealScalingFactor(wi->window_handle))
      {
        const QSize scaled_size =
          ApplyDevicePixelRatioToSize(widget->size(), static_cast<qreal>(real_device_pixel_ratio.value()));
        wi->surface_width = static_cast<u16>(scaled_size.width());
        wi->surface_height = static_cast<u16>(scaled_size.height());
        wi->surface_scale = static_cast<float>(real_device_pixel_ratio.value());
      }
    }

    return;
  }
#endif

  // On Linux, fuck you, enjoy round trip to the X server, and on Wayland you can't query it in the first place...
  // I ain't dealing with this crap OS. Enjoy your mismatched sizes and shit experience.
  const qreal device_pixel_ratio = widget->devicePixelRatio();
  const QSize scaled_size = ApplyDevicePixelRatioToSize(widget->size(), device_pixel_ratio);
  wi->surface_width = static_cast<u16>(scaled_size.width());
  wi->surface_height = static_cast<u16>(scaled_size.height());
  wi->surface_scale = static_cast<float>(device_pixel_ratio);
}

void QtUtils::UpdateSurfaceRefreshRate(QWidget* widget, WindowInfo* wi)
{
  // Query refresh rate, we need it for sync.
  Error refresh_rate_error;
  std::optional<float> surface_refresh_rate = WindowInfo::QueryRefreshRateForWindow(*wi, &refresh_rate_error);
  if (surface_refresh_rate.value_or(0.0f) > 0.0f)
  {
    wi->surface_refresh_rate = surface_refresh_rate.value();
    return;
  }

  WARNING_LOG("Failed to get refresh rate for window, falling back to Qt: {}", refresh_rate_error.GetDescription());

  // Fallback to using the screen, getting the rate for Wayland is an utter mess otherwise.
  const QScreen* widget_screen = widget->screen();
  if (!widget_screen)
    widget_screen = QGuiApplication::primaryScreen();
  wi->surface_refresh_rate = widget_screen ? static_cast<float>(widget_screen->refreshRate()) : 0.0f;
}

#ifdef __linux__

static void FormatQDBusReplyError(Error* error, const char* prefix, const QDBusError& qerror)
{
  Error::SetStringFmt(error, "{}{}: {}", prefix, qerror.name().toStdString(), qerror.message().toStdString());
}

#endif

bool Host::SetScreensaverInhibit(bool inhibit, Error* error)
{
  if (s_window_info_locals.screensaver_inhibited == inhibit)
    return true;

#if defined(_WIN32)

  if (SetThreadExecutionState(ES_CONTINUOUS | (inhibit ? (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : 0)) == NULL)
  {
    Error::SetWin32(error, "SetThreadExecutionState() failed: ", GetLastError());
    return false;
  }

  s_window_info_locals.screensaver_inhibited = inhibit;
  return true;

#elif defined(__APPLE__)

  if (inhibit)
  {
    const CFStringRef reason = CFSTR("DuckStation VM is running.");
    const IOReturn ret =
      IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, reason,
                                  &s_window_info_locals.screensaver_inhibit_assertion);
    if (ret != kIOReturnSuccess)
    {
      Error::SetStringFmt(error, "IOPMAssertionCreateWithName() failed: {}", static_cast<s32>(ret));
      return false;
    }

    s_window_info_locals.screensaver_inhibited = true;
    return true;
  }
  else
  {
    const IOReturn ret = IOPMAssertionRelease(s_window_info_locals.screensaver_inhibit_assertion);
    if (ret != kIOReturnSuccess)
    {
      Error::SetStringFmt(error, "IOPMAssertionRelease() failed: {}", static_cast<s32>(ret));
      return false;
    }

    s_window_info_locals.screensaver_inhibit_assertion = kIOPMNullAssertionID;
    s_window_info_locals.screensaver_inhibited = false;
    return true;
  }

#elif defined(__linux__)

  if (!s_window_info_locals.screensaver_inhibit_interface.has_value())
  {
    const QDBusConnection connection = QDBusConnection::sessionBus();
    if (!connection.isConnected())
    {
      Error::SetStringView(error, "Failed to connect to the D-Bus session bus");
      return false;
    }

    s_window_info_locals.screensaver_inhibit_interface.emplace(
      "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", connection);
    if (!s_window_info_locals.screensaver_inhibit_interface->isValid())
    {
      s_window_info_locals.screensaver_inhibit_interface.reset();
      Error::SetStringView(error, "org.freedesktop.ScreenSaver interface is invalid");
      return false;
    }
  }

  if (inhibit)
  {
    const QDBusReply<quint32> msg = s_window_info_locals.screensaver_inhibit_interface->call(
      "Inhibit", "DuckStation"_L1, "DuckStation VM is running."_L1);
    if (!msg.isValid())
    {
      FormatQDBusReplyError(error, "Inhibit message call failed: ", msg.error());
      return false;
    }

    s_window_info_locals.screensaver_inhibit_cookie = msg.value();
    s_window_info_locals.screensaver_inhibited = true;
    return true;
  }
  else
  {
    const QDBusReply<void> msg = s_window_info_locals.screensaver_inhibit_interface->call(
      "UnInhibit", s_window_info_locals.screensaver_inhibit_cookie);
    if (!msg.isValid())
    {
      FormatQDBusReplyError(error, "UnInhibit message call failed: ", msg.error());
      return false;
    }

    s_window_info_locals.screensaver_inhibit_cookie = 0;
    s_window_info_locals.screensaver_inhibited = false;
    return true;
  }

#else

  Error::SetStringView(error, "Not implemented.");
  return false;

#endif
}

#ifdef _WIN32

bool QtUtils::SetWindowRoundedCornerState(QWidget* widget, bool enabled)
{
  const HWND window_handle = reinterpret_cast<HWND>(widget->winId());
  const DWM_WINDOW_CORNER_PREFERENCE value = enabled ? DWMWCP_DEFAULT : DWMWCP_DONOTROUND;
  const HRESULT hr = DwmSetWindowAttribute(window_handle, DWMWA_WINDOW_CORNER_PREFERENCE, &value, sizeof(value));
  if (FAILED(hr))
  {
    ERROR_LOG("DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE) failed: ",
              Error::CreateHResult(hr).GetDescription());
  }

  return SUCCEEDED(hr);
}

#endif // _WIN32
