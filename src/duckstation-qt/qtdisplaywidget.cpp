#include "qtdisplaywidget.h"
#include "frontend-common/imgui_styles.h"
#include "imgui.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtGui/QWindowStateChangeEvent>
#include <cmath>

QtDisplayWidget::QtDisplayWidget(QtHostInterface* host_interface, QWidget* parent)
  : QWidget(parent), m_host_interface(host_interface)
{
  // We want a native window for both D3D and OpenGL.
  setAutoFillBackground(false);
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_PaintOnScreen, true);
}

QtDisplayWidget::~QtDisplayWidget() = default;

HostDisplay* QtDisplayWidget::getHostDisplayInterface()
{
  return nullptr;
}

bool QtDisplayWidget::hasDeviceContext() const
{
  return true;
}

bool QtDisplayWidget::createDeviceContext(QThread* worker_thread, bool debug_device)
{
  return true;
}

bool QtDisplayWidget::initializeDeviceContext(bool debug_device)
{
  if (!createImGuiContext() || !createDeviceResources())
    return false;

  return true;
}

void QtDisplayWidget::destroyDeviceContext()
{
  destroyImGuiContext();
  destroyDeviceResources();
}

qreal QtDisplayWidget::devicePixelRatioFromScreen() const
{
  QScreen* screen_for_ratio;
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
  screen_for_ratio = windowHandle()->screen();
#else
  screen_for_ratio = screen();
#endif
  if (!screen_for_ratio)
    screen_for_ratio = QGuiApplication::primaryScreen();

  return screen_for_ratio ? screen_for_ratio->devicePixelRatio() : static_cast<qreal>(1);
}

int QtDisplayWidget::scaledWindowWidth() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(width()) * devicePixelRatioFromScreen()));
}

int QtDisplayWidget::scaledWindowHeight() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(height()) * devicePixelRatioFromScreen()));
}

bool QtDisplayWidget::createImGuiContext()
{
  ImGui::CreateContext();

  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize.x = static_cast<float>(scaledWindowWidth());
  io.DisplaySize.y = static_cast<float>(scaledWindowHeight());

  const float framebuffer_scale = static_cast<float>(devicePixelRatioFromScreen());
  io.DisplayFramebufferScale.x = framebuffer_scale;
  io.DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f * framebuffer_scale);

  return true;
}

void QtDisplayWidget::destroyImGuiContext()
{
  ImGui::DestroyContext();
}

bool QtDisplayWidget::createDeviceResources()
{
  return true;
}

void QtDisplayWidget::destroyDeviceResources() {}

void QtDisplayWidget::windowResized(s32 new_window_width, s32 new_window_height)
{
  // imgui may not have been initialized yet
  if (!ImGui::GetCurrentContext())
    return;

  auto& io = ImGui::GetIO();
  io.DisplaySize.x = static_cast<float>(new_window_width);
  io.DisplaySize.y = static_cast<float>(new_window_height);
}

QPaintEngine* QtDisplayWidget::paintEngine() const
{
  return nullptr;
}

bool QtDisplayWidget::event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    {
      QKeyEvent* key_event = static_cast<QKeyEvent*>(event);
      if (!key_event->isAutoRepeat())
        m_host_interface->handleKeyEvent(QtUtils::KeyEventToInt(key_event), event->type() == QEvent::KeyPress);

      return true;
    }

    case QEvent::Resize:
    {
      QWidget::event(event);

      emit windowResizedEvent(scaledWindowWidth(), scaledWindowHeight());
      return true;
    }

    case QEvent::Close:
    {
      m_host_interface->synchronousPowerOffSystem();
      QWidget::event(event);
      return true;
    }

    case QEvent::WindowStateChange:
    {
      QWidget::event(event);

      if (static_cast<QWindowStateChangeEvent*>(event)->oldState() & Qt::WindowMinimized)
        emit windowRestoredEvent();

      return true;
    }

    default:
      return QWidget::event(event);
  }
}
