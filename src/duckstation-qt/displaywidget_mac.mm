// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "displaywidget.h"

#include "common/log.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

LOG_CHANNEL(Host);

void DisplayWidget::updateRelativeMode(bool enabled)
{
  const bool request_changed = (m_relative_mouse_enabled != enabled);
  if (request_changed)
  {
    if (enabled)
    {
      // Keep watching focus while relative mode is requested so capture can be suspended for another top-level
      // window and restored when the display window becomes active again.
      connect(qApp, &QGuiApplication::focusWindowChanged, this, &DisplayWidget::onFocusWindowChanged);
      m_relative_mouse_enabled = true;
    }
    else
    {
      disconnect(qApp, &QGuiApplication::focusWindowChanged, this, &DisplayWidget::onFocusWindowChanged);
    }
  }

  const bool activate = enabled && window()->isActiveWindow();
  if (m_relative_mouse_active == activate)
  {
    m_relative_mouse_enabled = enabled;
    return;
  }

  INFO_LOG("updateRelativeMode(): enabled={}, active={}", enabled, activate);

  if (activate)
  {
    m_relative_mouse_start_pos = QCursor::pos();

    // Qt implements QCursor::setPos() by posting a synthetic event, which requires Accessibility permission. Warp the
    // cursor once through CoreGraphics instead, then disassociate it so physical movement produces NSEvent deltas
    // without moving the system cursor or requiring repeated warps.
    const QPoint center_pos = mapToGlobal(QPoint((width() + 1) / 2, (height() + 1) / 2));
    CGEventRef const location_event = CGEventCreate(nullptr);
    const CGPoint old_position =
      location_event ? CGEventGetLocation(location_event) : CGPointMake(center_pos.x(), center_pos.y());
    if (location_event)
      CFRelease(location_event);

    const CGPoint new_position = CGPointMake(center_pos.x(), center_pos.y());
    const CGError warp_result = CGWarpMouseCursorPosition(new_position);
    if (warp_result != kCGErrorSuccess)
    {
      ERROR_LOG("CGWarpMouseCursorPosition() failed: {}", static_cast<int>(warp_result));
      return;
    }

    const CGError result = CGAssociateMouseAndMouseCursorPosition(false);
    if (result != kCGErrorSuccess)
    {
      ERROR_LOG("CGAssociateMouseAndMouseCursorPosition(false) failed: {}", static_cast<int>(result));
      CGWarpMouseCursorPosition(old_position);
      return;
    }

    // macOS includes the cursor warp in the next NSEvent delta. Cancel it while retaining any physical movement that
    // was combined into the same event.
    m_mac_warp_delta_x = static_cast<float>(old_position.x - new_position.x);
    m_mac_warp_delta_y = static_cast<float>(old_position.y - new_position.y);
    m_mac_has_warp_delta = (m_mac_warp_delta_x != 0.0f || m_mac_warp_delta_y != 0.0f);
    m_relative_mouse_active = true;

    // QWidget::nativeEvent() does not receive mouse NSEvents from Qt's Cocoa platform plugin, and translated
    // QMouseEvents do not retain native movement deltas. Install the application-wide native filter only while those
    // deltas are needed for active relative capture.
    QCoreApplication::instance()->installNativeEventFilter(this);
    grabMouse();
  }
  else
  {
    // Reassociate the physical mouse before restoring the cursor to its pre-capture position.
    const CGError result = CGAssociateMouseAndMouseCursorPosition(true);
    if (result != kCGErrorSuccess)
    {
      ERROR_LOG("CGAssociateMouseAndMouseCursorPosition(true) failed: {}", static_cast<int>(result));
      if (request_changed)
        connect(qApp, &QGuiApplication::focusWindowChanged, this, &DisplayWidget::onFocusWindowChanged);
      return;
    }

    m_mac_has_warp_delta = false;
    const CGError warp_result =
      CGWarpMouseCursorPosition(CGPointMake(m_relative_mouse_start_pos.x(), m_relative_mouse_start_pos.y()));
    if (warp_result != kCGErrorSuccess)
      ERROR_LOG("CGWarpMouseCursorPosition() failed: {}", static_cast<int>(warp_result));

    m_relative_mouse_active = false;
    releaseMouse();
    QCoreApplication::instance()->removeNativeEventFilter(this);
  }

  m_relative_mouse_enabled = enabled;
}

bool DisplayWidget::nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result)
{
  Q_UNUSED(result);

  if (!m_relative_mouse_active || event_type != "mac_generic_NSEvent")
    return false;

  NSEvent* const event = static_cast<NSEvent*>(message);
  switch (event.type)
  {
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
    {
      float dx = static_cast<float>(event.deltaX);
      float dy = static_cast<float>(event.deltaY);
      if (m_mac_has_warp_delta)
      {
        dx += m_mac_warp_delta_x;
        dy += m_mac_warp_delta_y;
        m_mac_has_warp_delta = false;
      }

      if (dx != 0.0f || dy != 0.0f)
        emit windowMouseMoveRelativeEvent(dx, dy);
      break;
    }

    default:
      break;
  }

  return false;
}
