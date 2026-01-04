// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qtwindowinfo.h"
#include "qtutils.h"

#include "core/core.h"

#include "util/gpu_device.h"

#include "common/error.h"
#include "common/log.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QWidget>

#if defined(_WIN32)
#include "common/windows_headers.h"
#elif defined(__APPLE__)
#include "common/cocoa_tools.h"
#else
#include <qpa/qplatformnativeinterface.h>
#endif

LOG_CHANNEL(Host);

WindowInfoType QtUtils::GetWindowInfoType()
{
  // Windows and Apple are easy here since there's no display connection.
#if defined(_WIN32)
  return WindowInfoType::Win32;
#elif defined(__APPLE__)
  return WindowInfoType::MacOS;
#else
  const QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("xcb"))
  {
    // This is only used for determining the automatic Vulkan renderer, therefore XCB/XLib doesn't matter here.
    // See the comment below for information about this bullshit.
    return WindowInfoType::XCB;
  }
  else if (platform_name == QStringLiteral("wayland"))
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

  UpdateSurfaceSize(widget, &wi);

  // Query refresh rate, we need it for sync.
  Error refresh_rate_error;
  std::optional<float> surface_refresh_rate = WindowInfo::QueryRefreshRateForWindow(wi, &refresh_rate_error);
  if (!surface_refresh_rate.has_value())
  {
    WARNING_LOG("Failed to get refresh rate for window, falling back to Qt: {}", refresh_rate_error.GetDescription());

    // Fallback to using the screen, getting the rate for Wayland is an utter mess otherwise.
    const QScreen* widget_screen = widget->screen();
    if (!widget_screen)
      widget_screen = QGuiApplication::primaryScreen();
    surface_refresh_rate = widget_screen ? static_cast<float>(widget_screen->refreshRate()) : 0.0f;
  }

  wi.surface_refresh_rate = surface_refresh_rate.value();

  INFO_LOG("Window size: {}x{} (Qt {}x{}), scale: {}, refresh rate {} hz", wi.surface_width, wi.surface_height,
           widget->width(), widget->height(), wi.surface_scale, wi.surface_refresh_rate);

  return wi;
}

void QtUtils::UpdateSurfaceSize(QWidget* widget, WindowInfo* wi)
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
    qreal device_pixel_ratio = widget->devicePixelRatio();
    if (Core::GetBaseBoolSettingValue("Main", "UseFractionalWindowScale", true))
    {
      if (const std::optional<double> real_device_pixel_ratio = CocoaTools::GetViewRealScalingFactor(wi->window_handle))
        device_pixel_ratio = static_cast<qreal>(real_device_pixel_ratio.value());
    }

    wi->surface_width = static_cast<u16>(size->first);
    wi->surface_height = static_cast<u16>(size->second);
    wi->surface_scale = static_cast<float>(device_pixel_ratio);
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
