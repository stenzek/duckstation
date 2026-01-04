// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "displaywidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "qtwindowinfo.h"

#include "core/core.h"
#include "core/fullscreenui.h"

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

const std::optional<WindowInfo>& DisplayWidget::getWindowInfo(RenderAPI render_api, Error* error)
{
  if (!m_window_info.has_value())
    m_window_info = QtUtils::GetWindowInfoForWidget(this, render_api, error);

  return m_window_info;
}

void DisplayWidget::clearWindowInfo()
{
  m_window_info.reset();
}

void DisplayWidget::checkForSizeChange()
{
  if (!m_window_info.has_value())
    return;

  // avoid spamming resize events for paint events (sent on move on windows)
  const u16 prev_width = m_window_info->surface_width;
  const u16 prev_height = m_window_info->surface_height;
  const float prev_scale = m_window_info->surface_scale;
  QtUtils::UpdateSurfaceSize(this, &m_window_info.value());
  if (prev_width != m_window_info->surface_width || prev_height != m_window_info->surface_height ||
      prev_scale != m_window_info->surface_scale)
  {
    DEV_LOG("Display widget resized to {}x{} (Qt {}x{}) DPR={}", m_window_info->surface_width,
            m_window_info->surface_height, width(), height(), m_window_info->surface_scale);
    emit windowResizedEvent(m_window_info->surface_width, m_window_info->surface_height, m_window_info->surface_scale);
  }

  updateCenterPos();
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
  if ((QtHost::IsSystemValidOrStarting() || QtHost::IsFullscreenUIStarted()) && !isActuallyFullscreen())
  {
    QMetaObject::invokeMethod(g_main_window, &MainWindow::requestShutdown, Qt::QueuedConnection, true, true, false,
                              true, true, true, false);
  }
  else
  {
    QMetaObject::invokeMethod(g_main_window, &MainWindow::requestExit, Qt::QueuedConnection, true);
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

      const Qt::KeyboardModifiers modifiers = key_event->modifiers();
      const bool pressed = (key_event->type() == QEvent::KeyPress);
      const auto it =
        std::find(m_keys_pressed_with_modifiers.begin(), m_keys_pressed_with_modifiers.end(), key_event->key());
      if (it != m_keys_pressed_with_modifiers.end())
      {
        if (pressed)
          return true;
        else
          m_keys_pressed_with_modifiers.erase(it);
      }
      else if (modifiers != Qt::NoModifier && modifiers != Qt::KeypadModifier && pressed)
      {
        m_keys_pressed_with_modifiers.push_back(key_event->key());
      }

      if (const std::optional<u32> key = QtUtils::KeyEventToCode(key_event))
        emit windowKeyEvent(key.value(), pressed);

      return true;
    }

    case QEvent::MouseMove:
    {
      if (!m_relative_mouse_enabled)
      {
        const float surface_scale =
          m_window_info.has_value() ? m_window_info->surface_scale : static_cast<float>(devicePixelRatio());
        const QPoint mouse_pos = static_cast<QMouseEvent*>(event)->pos();
        const float scaled_x = static_cast<float>(mouse_pos.x()) * surface_scale;
        const float scaled_y = static_cast<float>(mouse_pos.y()) * surface_scale;
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
      // we don't get press events for double-click, the dblclick event is substituted instead
      if (!m_relative_mouse_enabled || !InputManager::IsUsingRawInput())
      {
        const u32 button_index = CountTrailingZeros(static_cast<u32>(static_cast<const QMouseEvent*>(event)->button()));
        emit windowMouseButtonEvent(static_cast<int>(button_index), true);
      }

      // don't toggle fullscreen when we're bound.. that wouldn't end well.
      if (static_cast<const QMouseEvent*>(event)->button() == Qt::LeftButton && QtHost::IsSystemValid() &&
          ((!QtHost::IsSystemPaused() && !m_relative_mouse_enabled &&
            !InputManager::HasAnyBindingsForKey(InputManager::MakePointerButtonKey(0, 0))) ||
           (QtHost::IsSystemPaused() && !ImGuiManager::WantsMouseInput())) &&
          Core::GetBoolSettingValue("Main", "DoubleClickTogglesFullscreen", true))
      {
        g_core_thread->toggleFullscreen();

        // when swapping fullscreen, the window is going to get recreated, and we won't get the release event.
        // therefore we need to trigger it here instead, otherwise it gets lost and imgui is confused.
        // skip this if we're not running on wankland or using render-to-main, since the window is preserved.
        if (QtHost::IsDisplayWidgetContainerNeeded() || g_main_window->canRenderToMainWindow())
          Host::RunOnCoreThread(&ImGuiManager::ClearMouseButtonState);
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

    case QEvent::Show:
    case QEvent::Resize:
    case QEvent::DevicePixelRatioChange:
    {
      QWidget::event(event);

      checkForSizeChange();
      return true;
    }

    case QEvent::Move:
    {
      QWidget::event(event);

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

    case QEvent::Paint:
      return true;

    case QEvent::WindowStateChange:
    {
      QWidget::event(event);

      const QWindowStateChangeEvent* ws_event = static_cast<const QWindowStateChangeEvent*>(event);
      if (ws_event->oldState() & Qt::WindowMinimized)
        emit windowRestoredEvent();

#ifdef __APPLE__
      // On MacOS, the user can "cancel" fullscreen by unmaximizing the window.
      if (ws_event->oldState() & Qt::WindowFullScreen && !(windowState() & Qt::WindowFullScreen))
        g_core_thread->setFullscreen(false);
#endif

      return true;
    }

    case QEvent::WinIdChange:
    {
      QWidget::event(event);

      if (m_window_info.has_value())
      {
        ERROR_LOG("Window ID changed while we had a valid WindowInfo. This is NOT expected, please report.");
        clearWindowInfo();
      }

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

const std::optional<WindowInfo>& AuxiliaryDisplayWidget::getWindowInfo(RenderAPI render_api, Error* error)
{
  if (!m_window_info.has_value())
    m_window_info = QtUtils::GetWindowInfoForWidget(this, render_api, error);

  return m_window_info;
}

void AuxiliaryDisplayWidget::checkForSizeChange()
{
  if (!m_window_info.has_value())
    return;

  // avoid spamming resize events for paint events (sent on move on windows)
  const u16 prev_width = m_window_info->surface_width;
  const u16 prev_height = m_window_info->surface_height;
  const float prev_scale = m_window_info->surface_scale;
  QtUtils::UpdateSurfaceSize(this, &m_window_info.value());
  if (prev_width != m_window_info->surface_width || prev_height != m_window_info->surface_height ||
      prev_scale != m_window_info->surface_scale)
  {
    DEV_LOG("Auxiliary display widget resized to {}x{} (Qt {}x{}) DPR={}", m_window_info->surface_width,
            m_window_info->surface_height, width(), height(), m_window_info->surface_scale);

    g_core_thread->queueAuxiliaryRenderWindowInputEvent(
      m_userdata, Host::AuxiliaryRenderWindowEvent::Resized,
      Host::AuxiliaryRenderWindowEventParam{.uint_param = static_cast<u32>(m_window_info->surface_width)},
      Host::AuxiliaryRenderWindowEventParam{.uint_param = static_cast<u32>(m_window_info->surface_height)},
      Host::AuxiliaryRenderWindowEventParam{.float_param = static_cast<float>(m_window_info->surface_scale)});
  }
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

          g_core_thread->queueAuxiliaryRenderWindowInputEvent(
            m_userdata, Host::AuxiliaryRenderWindowEvent::TextEntered,
            Host::AuxiliaryRenderWindowEventParam{.uint_param = static_cast<u32>(ch.unicode())});
        }
      }

      if (key_event->isAutoRepeat())
        return true;

      g_core_thread->queueAuxiliaryRenderWindowInputEvent(
        m_userdata,
        (type == QEvent::KeyPress) ? Host::AuxiliaryRenderWindowEvent::KeyPressed :
                                     Host::AuxiliaryRenderWindowEvent::KeyReleased,
        Host::AuxiliaryRenderWindowEventParam{.uint_param = static_cast<u32>(key_event->key())});

      return true;
    }

    case QEvent::MouseMove:
    {
      const QPoint mouse_pos = static_cast<QMouseEvent*>(event)->pos();
      const float surface_scale =
        m_window_info.has_value() ? m_window_info->surface_scale : static_cast<float>(devicePixelRatio());
      const float scaled_x = static_cast<float>(mouse_pos.x()) * surface_scale;
      const float scaled_y = static_cast<float>(mouse_pos.y()) * surface_scale;

      g_core_thread->queueAuxiliaryRenderWindowInputEvent(
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
      g_core_thread->queueAuxiliaryRenderWindowInputEvent(
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
        g_core_thread->queueAuxiliaryRenderWindowInputEvent(
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

      g_core_thread->queueAuxiliaryRenderWindowInputEvent(m_userdata, Host::AuxiliaryRenderWindowEvent::CloseRequest);
      event->ignore();
      return true;
    }

    case QEvent::Show:
    case QEvent::Resize:
    case QEvent::DevicePixelRatioChange:
    {
      QWidget::event(event);

      checkForSizeChange();
      return true;
    }

    case QEvent::WinIdChange:
    {
      QWidget::event(event);

      if (m_window_info.has_value())
      {
        ERROR_LOG("Auxiliary display widget window ID changed while we had a valid WindowInfo. This is NOT expected, "
                  "please report.");
        m_window_info.reset();
      }

      return true;
    }

    case QEvent::Paint:
      return true;

    default:
      return QWidget::event(event);
  }
}

AuxiliaryDisplayWidget* AuxiliaryDisplayWidget::create(s32 pos_x, s32 pos_y, u32 width, u32 height,
                                                       const QString& title, const QString& icon_name, void* userdata)
{
  QStackedWidget* parent = nullptr;
  if (QtHost::IsDisplayWidgetContainerNeeded())
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
  m_window_info.reset();

  QWidget* container = static_cast<QWidget*>(parent());
  if (!container)
    container = this;
  container->close();
  container->deleteLater();
}
