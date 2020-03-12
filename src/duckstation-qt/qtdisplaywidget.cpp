#include "qtdisplaywidget.h"
#include "imgui.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <cmath>

QtDisplayWidget::QtDisplayWidget(QtHostInterface* host_interface, QWidget* parent)
  : QWidget(parent), m_host_interface(host_interface)
{
  // We want a native window for both D3D and OpenGL.
  setAttribute(Qt::WA_NativeWindow, true);
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

qreal QtDisplayWidget::getDevicePixelRatioFromScreen() const
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

int QtDisplayWidget::getScaledWindowWidth() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(width()) * getDevicePixelRatioFromScreen()));
}

int QtDisplayWidget::getScaledWindowHeight() const
{
  return static_cast<int>(std::ceil(static_cast<qreal>(height()) * getDevicePixelRatioFromScreen()));
}

bool QtDisplayWidget::createImGuiContext()
{
  ImGui::CreateContext();

  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize.x = static_cast<float>(getScaledWindowWidth());
  io.DisplaySize.y = static_cast<float>(getScaledWindowHeight());

  const float framebuffer_scale = static_cast<float>(getDevicePixelRatioFromScreen());
  io.DisplayFramebufferScale.x = framebuffer_scale;
  io.DisplayFramebufferScale.y = framebuffer_scale;
  io.FontGlobalScale = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

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

      emit windowResizedEvent(getScaledWindowWidth(), getScaledWindowHeight());
      return true;
    }

    default:
      return QWidget::event(event);
  }
}
