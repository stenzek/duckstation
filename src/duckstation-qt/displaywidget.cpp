// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "displaywidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/fullscreen_ui.h"

#include "util/imgui_manager.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"

#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/QWindowStateChangeEvent>
#include <cmath>

#include "moc_displaywidget.cpp"

#if !defined(_WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif

#ifdef _WIN32
#include "common/windows_headers.h"
#endif

LOG_CHANNEL(Host);

DisplayWidget::DisplayWidget(QWidget* parent) : QWidget(parent)
{
  // We want a native window for both D3D and OpenGL.
  setAutoFillBackground(false);
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_DontCreateNativeAncestors, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_PaintOnScreen, true);
  setAttribute(Qt::WA_KeyCompression, false);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}

DisplayWidget::~DisplayWidget() = default;

int DisplayWidget::scaledWindowWidth() const
{
  return std::max(
    static_cast<int>(std::ceil(static_cast<qreal>(width()) * QtUtils::GetDevicePixelRatioForWidget(this))), 1);
}

int DisplayWidget::scaledWindowHeight() const
{
  return std::max(
    static_cast<int>(std::ceil(static_cast<qreal>(height()) * QtUtils::GetDevicePixelRatioForWidget(this))), 1);
}

std::optional<WindowInfo> DisplayWidget::getWindowInfo(RenderAPI render_api, Error* error)
{
  std::optional<WindowInfo> ret = QtUtils::GetWindowInfoForWidget(this, render_api, error);
  if (ret.has_value())
  {
    m_last_window_width = ret->surface_width;
    m_last_window_height = ret->surface_height;
    m_last_window_scale = ret->surface_scale;
  }
  return ret;
}

void DisplayWidget::updateRelativeMode(bool enabled)
{
#ifdef _WIN32
  // prefer ClipCursor() over warping movement when we're using raw input
  bool clip_cursor = enabled && InputManager::IsUsingRawInput();
  if (m_relative_mouse_enabled == enabled && m_clip_mouse_enabled == clip_cursor)
    return;

  INFO_LOG("updateRelativeMode(): relative={}, clip={}", enabled ? "yes" : "no", clip_cursor ? "yes" : "no");

  if (!clip_cursor && m_clip_mouse_enabled)
  {
    m_clip_mouse_enabled = false;
    ClipCursor(nullptr);
  }
#else
  if (m_relative_mouse_enabled == enabled)
    return;

  INFO_LOG("updateRelativeMode(): relative={}", enabled ? "yes" : "no");
#endif

  if (enabled)
  {
    m_relative_mouse_enabled = true;
#ifdef _WIN32
    m_clip_mouse_enabled = clip_cursor;
#endif
    m_relative_mouse_start_pos = QCursor::pos();
    updateCenterPos();
    grabMouse();
  }
  else if (m_relative_mouse_enabled)
  {
    m_relative_mouse_enabled = false;
    QCursor::setPos(m_relative_mouse_start_pos);
    releaseMouse();
  }
}

void DisplayWidget::updateCursor(bool hidden)
{
  if (m_cursor_hidden == hidden)
    return;

  m_cursor_hidden = hidden;
  if (hidden)
  {
    DEV_LOG("updateCursor(): Cursor is now hidden");
    setCursor(Qt::BlankCursor);
  }
  else
  {
    DEV_LOG("updateCursor(): Cursor is now shown");
    unsetCursor();
  }
}

void DisplayWidget::handleCloseEvent(QCloseEvent* event)
{
  event->ignore();

  // Closing the separate widget will either cancel the close, or trigger shutdown.
  // In the latter case, it's going to destroy us, so don't let Qt do it first.
  // Treat a close event while fullscreen as an exit, that way ALT+F4 closes DuckStation,
  // rather than just the game.
  if (QtHost::IsSystemValidOrStarting() && !isActuallyFullscreen())
  {
    QMetaObject::invokeMethod(g_main_window, "requestShutdown", Qt::QueuedConnection, Q_ARG(bool, true),
                              Q_ARG(bool, true), Q_ARG(bool, false), Q_ARG(bool, true), Q_ARG(bool, true),
                              Q_ARG(bool, false));
  }
  else
  {
    QMetaObject::invokeMethod(g_main_window, "requestExit", Qt::QueuedConnection);
  }
}

void DisplayWidget::destroy()
{
  m_destroying = true;

#ifdef _WIN32
  if (m_clip_mouse_enabled)
    ClipCursor(nullptr);
#endif

#ifdef __APPLE__
  // See Qt documentation, entire application is in full screen state, and the main
  // window will get reopened fullscreen instead of windowed if we don't close the
  // fullscreen window first.
  if (isActuallyFullscreen())
    close();
#endif
  deleteLater();
}

bool DisplayWidget::isActuallyFullscreen() const
{
  // I hate you QtWayland... have to check the parent, not ourselves.
  QWidget* container = qobject_cast<QWidget*>(parent());
  return container ? container->isFullScreen() : isFullScreen();
}

void DisplayWidget::updateCenterPos()
{
#ifdef _WIN32
  if (m_clip_mouse_enabled)
  {
    RECT rc;
    if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
      ClipCursor(&rc);
  }
  else if (m_relative_mouse_enabled)
  {
    RECT rc;
    if (GetWindowRect(reinterpret_cast<HWND>(winId()), &rc))
    {
      m_relative_mouse_center_pos.setX(((rc.right - rc.left) / 2) + rc.left);
      m_relative_mouse_center_pos.setY(((rc.bottom - rc.top) / 2) + rc.top);
      SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
    }
  }
#else
  if (m_relative_mouse_enabled)
  {
    // we do a round trip here because these coordinates are dpi-unscaled
    m_relative_mouse_center_pos = mapToGlobal(QPoint((width() + 1) / 2, (height() + 1) / 2));
    QCursor::setPos(m_relative_mouse_center_pos);
    m_relative_mouse_center_pos = QCursor::pos();
  }
#endif
}

QPaintEngine* DisplayWidget::paintEngine() const
{
  return nullptr;
}

bool DisplayWidget::event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      const QKeyEvent* key_event = static_cast<QKeyEvent*>(event);

      if (ImGuiManager::WantsTextInput() && key_event->type() == QEvent::KeyPress)
      {
        // Don't forward backspace characters. We send the backspace as a normal key event,
        // so if we send the character too, it double-deletes.
        QString text(key_event->text());
        text.remove(QChar('\b'));
        if (!text.isEmpty())
          emit windowTextEntered(text);
      }

      if (key_event->isAutoRepeat())
        return true;

      // For some reason, Windows sends "fake" key events.
      // Scenario: Press shift, press F1, release shift, release F1.
      // Events: Shift=Pressed, F1=Pressed, Shift=Released, **F1=Pressed**, F1=Released.
      // To work around this, we keep track of keys pressed with modifiers in a list, and
      // discard the press event when it's been previously activated. It's pretty gross,
      // but I can't think of a better way of handling it, and there doesn't appear to be
      // any window flag which changes this behavior that I can see.

      const u32 key = QtUtils::KeyEventToCode(key_event);
      const Qt::KeyboardModifiers modifiers = key_event->modifiers();
      const bool pressed = (key_event->type() == QEvent::KeyPress);
      const auto it = std::find(m_keys_pressed_with_modifiers.begin(), m_keys_pressed_with_modifiers.end(), key);
      if (it != m_keys_pressed_with_modifiers.end())
      {
        if (pressed)
          return true;
        else
          m_keys_pressed_with_modifiers.erase(it);
      }
      else if (modifiers != Qt::NoModifier && modifiers != Qt::KeypadModifier && pressed)
      {
        m_keys_pressed_with_modifiers.push_back(key);
      }

      emit windowKeyEvent(key, pressed);
      return true;
    }

    case QEvent::MouseMove:
    {
      if (!m_relative_mouse_enabled)
      {
        const qreal dpr = QtUtils::GetDevicePixelRatioForWidget(this);
        const QPoint mouse_pos = static_cast<QMouseEvent*>(event)->pos();

        const float scaled_x = static_cast<float>(static_cast<qreal>(mouse_pos.x()) * dpr);
        const float scaled_y = static_cast<float>(static_cast<qreal>(mouse_pos.y()) * dpr);
        InputManager::UpdatePointerAbsolutePosition(0, scaled_x, scaled_y);
      }
      else
      {
        // On windows, we use winapi here. The reason being that the coordinates in QCursor
        // are un-dpi-scaled, so we lose precision at higher desktop scalings.
        float dx = 0.0f, dy = 0.0f;

#ifndef _WIN32
        const QPoint mouse_pos = QCursor::pos();
        if (mouse_pos != m_relative_mouse_center_pos)
        {
          dx = static_cast<float>(mouse_pos.x() - m_relative_mouse_center_pos.x());
          dy = static_cast<float>(mouse_pos.y() - m_relative_mouse_center_pos.y());
          QCursor::setPos(m_relative_mouse_center_pos);
        }
#else
        POINT mouse_pos;
        if (GetCursorPos(&mouse_pos))
        {
          dx = static_cast<float>(mouse_pos.x - m_relative_mouse_center_pos.x());
          dy = static_cast<float>(mouse_pos.y - m_relative_mouse_center_pos.y());
          SetCursorPos(m_relative_mouse_center_pos.x(), m_relative_mouse_center_pos.y());
        }
#endif

        if (!InputManager::IsUsingRawInput())
        {
          if (dx != 0.0f)
            InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::X, dx);
          if (dy != 0.0f)
            InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::Y, dy);
        }
      }

      return true;
    }

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    {
      if (!m_relative_mouse_enabled || !InputManager::IsUsingRawInput())
      {
        const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
        emit windowMouseButtonEvent(static_cast<int>(button_index), event->type() != QEvent::MouseButtonRelease);
      }

      return true;
    }

    case QEvent::MouseButtonDblClick:
    {
      // since we don't get press and release events for double-click, we need to send both the down and up
      // otherwise the second click in a double click won't be registered by the input system
      if (!m_relative_mouse_enabled || !InputManager::IsUsingRawInput())
      {
        const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
        emit windowMouseButtonEvent(static_cast<int>(button_index), true);
        emit windowMouseButtonEvent(static_cast<int>(button_index), false);
      }

      // don't toggle fullscreen when we're bound.. that wouldn't end well.
      if (static_cast<const QMouseEvent*>(event)->button() == Qt::LeftButton && QtHost::IsSystemValid() &&
          ((!QtHost::IsSystemPaused() && !m_relative_mouse_enabled &&
            !InputManager::HasAnyBindingsForKey(InputManager::MakePointerButtonKey(0, 0))) ||
           (QtHost::IsSystemPaused() && !ImGuiManager::WantsMouseInput())) &&
          Host::GetBoolSettingValue("Main", "DoubleClickTogglesFullscreen", true))
      {
        g_emu_thread->toggleFullscreen();
      }

      return true;
    }

    case QEvent::Wheel:
    {
      const QWheelEvent* wheel_event = static_cast<QWheelEvent*>(event);
      const QPoint angle_delta = wheel_event->angleDelta();
      const float dx = static_cast<float>(angle_delta.x()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep);
      const float dy = (static_cast<float>(angle_delta.y()) / static_cast<float>(QWheelEvent::DefaultDeltasPerStep)) *
                       static_cast<float>(QApplication::wheelScrollLines());
      emit windowMouseWheelEvent(dx, dy);
      return true;
    }

      // According to https://bugreports.qt.io/browse/QTBUG-95925 the recommended practice for handling DPI change is
      // responding to paint events
    case QEvent::Paint:
    case QEvent::Resize:
    {
      QWidget::event(event);

      const float dpr = QtUtils::GetDevicePixelRatioForWidget(this);
      const u32 scaled_width =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(width()) * dpr)), 1));
      const u32 scaled_height =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(height()) * dpr)), 1));

      // avoid spamming resize events for paint events (sent on move on windows)
      if (m_last_window_width != scaled_width || m_last_window_height != scaled_height || m_last_window_scale != dpr)
      {
        m_last_window_width = scaled_width;
        m_last_window_height = scaled_height;
        m_last_window_scale = dpr;
        emit windowResizedEvent(scaled_width, scaled_height, dpr);
      }

      updateCenterPos();
      return true;
    }

    case QEvent::Move:
    {
      updateCenterPos();
      return true;
    }

    case QEvent::Close:
    {
      if (m_destroying)
        return QWidget::event(event);

      handleCloseEvent(static_cast<QCloseEvent*>(event));
      return true;
    }

    case QEvent::WindowStateChange:
    {
      QWidget::event(event);

      const QWindowStateChangeEvent* ws_event = static_cast<const QWindowStateChangeEvent*>(event);
      if (ws_event->oldState() & Qt::WindowMinimized)
        emit windowRestoredEvent();

#ifdef __APPLE__
      // On MacOS, the user can "cancel" fullscreen by unmaximizing the window.
      if (ws_event->oldState() & Qt::WindowFullScreen && !(windowState() & Qt::WindowFullScreen))
        g_emu_thread->setFullscreen(false);
#endif

      return true;
    }

    default:
      return QWidget::event(event);
  }
}

DisplayContainer::DisplayContainer() : QStackedWidget(nullptr)
{
}

DisplayContainer::~DisplayContainer() = default;

bool DisplayContainer::isNeeded(bool fullscreen, bool render_to_main)
{
#if defined(_WIN32) || defined(__APPLE__)
  return false;
#else
  if (!QtHost::IsRunningOnWayland())
    return false;

  // We only need this on Wayland because of client-side decorations...
  return (fullscreen || !render_to_main);
#endif
}

void DisplayContainer::setDisplayWidget(DisplayWidget* widget)
{
  Assert(!m_display_widget);
  m_display_widget = widget;
  addWidget(widget);
}

DisplayWidget* DisplayContainer::removeDisplayWidget()
{
  DisplayWidget* widget = m_display_widget;
  Assert(widget);
  m_display_widget = nullptr;
  removeWidget(widget);
  return widget;
}

bool DisplayContainer::event(QEvent* event)
{
  if (event->type() == QEvent::Close && m_display_widget)
  {
    m_display_widget->handleCloseEvent(static_cast<QCloseEvent*>(event));
    return true;
  }

  const bool res = QStackedWidget::event(event);
  if (!m_display_widget)
    return res;

  switch (event->type())
  {
    case QEvent::WindowStateChange:
    {
      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit m_display_widget->windowRestoredEvent();
    }
    break;

    default:
      break;
  }

  return res;
}

AuxiliaryDisplayWidget::AuxiliaryDisplayWidget(QWidget* parent, u32 width, u32 height, const QString& title,
                                               void* userdata)
  : QWidget(parent), m_userdata(userdata)
{
  // We want a native window for both D3D and OpenGL.
  setAutoFillBackground(false);
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_DontCreateNativeAncestors, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_PaintOnScreen, true);
  setAttribute(Qt::WA_KeyCompression, false);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setWindowTitle(title);
  resize(width, height);
}

AuxiliaryDisplayWidget::~AuxiliaryDisplayWidget() = default;

QPaintEngine* AuxiliaryDisplayWidget::paintEngine() const
{
  return nullptr;
}

bool AuxiliaryDisplayWidget::event(QEvent* event)
{
  const QEvent::Type type = event->type();
  switch (type)
  {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);

      if (type == QEvent::KeyPress)
      {
        // note this won't work for emojis.. deal with that if it's ever needed
        for (const QChar& ch : key_event->text())
        {
          // Don't forward backspace characters. We send the backspace as a normal key event,
          // so if we send the character too, it double-deletes.
          if (ch == QChar('\b'))
            break;

          g_emu_thread->queueAuxiliaryRenderWindowInputEvent(
            m_userdata, Host::AuxiliaryRenderWindowEvent::TextEntered,
            Host::AuxiliaryRenderWindowEventParam{.uint_param = static_cast<u32>(ch.unicode())});
        }
      }

      if (key_event->isAutoRepeat())
        return true;

      g_emu_thread->queueAuxiliaryRenderWindowInputEvent(
        m_userdata,
        (type == QEvent::KeyPress) ? Host::AuxiliaryRenderWindowEvent::KeyPressed :
                                     Host::AuxiliaryRenderWindowEvent::KeyReleased,
        Host::AuxiliaryRenderWindowEventParam{.uint_param = static_cast<u32>(key_event->key())});

      return true;
    }

    case QEvent::MouseMove:
    {
      const qreal dpr = QtUtils::GetDevicePixelRatioForWidget(this);
      const QPoint mouse_pos = static_cast<QMouseEvent*>(event)->pos();
      const float scaled_x = static_cast<float>(static_cast<qreal>(mouse_pos.x()) * dpr);
      const float scaled_y = static_cast<float>(static_cast<qreal>(mouse_pos.y()) * dpr);

      g_emu_thread->queueAuxiliaryRenderWindowInputEvent(
        m_userdata, Host::AuxiliaryRenderWindowEvent::MouseMoved,
        Host::AuxiliaryRenderWindowEventParam{.float_param = scaled_x},
        Host::AuxiliaryRenderWindowEventParam{.float_param = scaled_y});

      return true;
    }

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    {
      const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
      g_emu_thread->queueAuxiliaryRenderWindowInputEvent(
        m_userdata,
        (type == QEvent::MouseButtonRelease) ? Host::AuxiliaryRenderWindowEvent::MouseReleased :
                                               Host::AuxiliaryRenderWindowEvent::MousePressed,
        Host::AuxiliaryRenderWindowEventParam{.uint_param = button_index},
        Host::AuxiliaryRenderWindowEventParam{.uint_param = BoolToUInt32(type == QEvent::MouseButtonRelease)});

      return true;
    }

    case QEvent::Wheel:
    {
      const QWheelEvent* wheel_event = static_cast<QWheelEvent*>(event);
      const QPoint delta = wheel_event->angleDelta();
      if (delta.x() != 0 || delta.y())
      {
        g_emu_thread->queueAuxiliaryRenderWindowInputEvent(
          m_userdata, Host::AuxiliaryRenderWindowEvent::MouseWheel,
          Host::AuxiliaryRenderWindowEventParam{.float_param = static_cast<float>(delta.x())},
          Host::AuxiliaryRenderWindowEventParam{.float_param = static_cast<float>(delta.y())});
      }

      return true;
    }

    case QEvent::Close:
    {
      if (m_destroying)
        return QWidget::event(event);

      g_emu_thread->queueAuxiliaryRenderWindowInputEvent(m_userdata, Host::AuxiliaryRenderWindowEvent::CloseRequest);
      event->ignore();
      return true;
    }

    case QEvent::Paint:
    case QEvent::Resize:
    {
      QWidget::event(event);

      const float dpr = QtUtils::GetDevicePixelRatioForWidget(this);
      const u32 scaled_width =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(width()) * dpr)), 1));
      const u32 scaled_height =
        static_cast<u32>(std::max(static_cast<int>(std::ceil(static_cast<qreal>(height()) * dpr)), 1));

      // avoid spamming resize events for paint events (sent on move on windows)
      if (m_last_window_width != scaled_width || m_last_window_height != scaled_height || m_last_window_scale != dpr)
      {
        m_last_window_width = scaled_width;
        m_last_window_height = scaled_height;
        m_last_window_scale = dpr;
        g_emu_thread->queueAuxiliaryRenderWindowInputEvent(
          m_userdata, Host::AuxiliaryRenderWindowEvent::Resized,
          Host::AuxiliaryRenderWindowEventParam{.uint_param = scaled_width},
          Host::AuxiliaryRenderWindowEventParam{.uint_param = scaled_height},
          Host::AuxiliaryRenderWindowEventParam{.float_param = dpr});
      }

      return true;
    }

    default:
      return QWidget::event(event);
  }
}

AuxiliaryDisplayWidget* AuxiliaryDisplayWidget::create(s32 pos_x, s32 pos_y, u32 width, u32 height,
                                                       const QString& title, const QString& icon_name, void* userdata)
{
  QStackedWidget* parent = nullptr;
  if (DisplayContainer::isNeeded(false, false))
  {
    parent = new QStackedWidget(nullptr);
    parent->resize(width, height);
    parent->setWindowTitle(title);
  }

  AuxiliaryDisplayWidget* widget = new AuxiliaryDisplayWidget(parent, width, height, title, userdata);
  if (parent)
    parent->addWidget(widget);

  QWidget* window = parent ? static_cast<QWidget*>(parent) : static_cast<QWidget*>(widget);
  if (!icon_name.isEmpty())
  {
    if (const QIcon icon(icon_name); !icon.isNull())
      window->setWindowIcon(icon);
    else
      window->setWindowIcon(QtHost::GetAppIcon());
  }
  else
  {
    window->setWindowIcon(QtHost::GetAppIcon());
  }

  if (pos_x != std::numeric_limits<s32>::min() && pos_y != std::numeric_limits<s32>::min())
    window->move(pos_x, pos_y);

  window->show();
  return widget;
}

void AuxiliaryDisplayWidget::destroy()
{
  m_destroying = true;

  QWidget* container = static_cast<QWidget*>(parent());
  if (!container)
    container = this;
  container->close();
  container->deleteLater();
}
