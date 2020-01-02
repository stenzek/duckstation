#include "qtdisplaywindow.h"
#include "imgui.h"
#include "qthostinterface.h"
#include <QtGui/QKeyEvent>

QtDisplayWindow::QtDisplayWindow(QtHostInterface* host_interface, QWindow* parent)
  : QWindow(parent), m_host_interface(host_interface)
{
}

QtDisplayWindow::~QtDisplayWindow() = default;

HostDisplay* QtDisplayWindow::getHostDisplayInterface()
{
  return nullptr;
}

bool QtDisplayWindow::createDeviceContext(QThread* worker_thread)
{
  return true;
}

bool QtDisplayWindow::initializeDeviceContext()
{
  if (!createImGuiContext() || !createDeviceResources())
    return false;

  return true;
}

void QtDisplayWindow::destroyDeviceContext()
{
  destroyImGuiContext();
  destroyDeviceResources();
}

bool QtDisplayWindow::createImGuiContext()
{
  ImGui::CreateContext();

  auto& io = ImGui::GetIO();
  io.DisplaySize.x = static_cast<float>(width());
  io.DisplaySize.y = static_cast<float>(height());

  return true;
}

void QtDisplayWindow::destroyImGuiContext()
{
  ImGui::DestroyContext();
}

bool QtDisplayWindow::createDeviceResources()
{
  return true;
}

void QtDisplayWindow::destroyDeviceResources() {}

void QtDisplayWindow::Render() {}

void QtDisplayWindow::onWindowResized(int width, int height)
{
  // imgui may not have been initialized yet
  if (!ImGui::GetCurrentContext())
    return;

  auto& io = ImGui::GetIO();
  io.DisplaySize.x = static_cast<float>(width);
  io.DisplaySize.y = static_cast<float>(height);
}

void QtDisplayWindow::keyPressEvent(QKeyEvent* event)
{
  m_host_interface->handleKeyEvent(event->key(), true);
}

void QtDisplayWindow::keyReleaseEvent(QKeyEvent* event)
{
  m_host_interface->handleKeyEvent(event->key(), false);
}

void QtDisplayWindow::resizeEvent(QResizeEvent* event)
{
  QWindow::resizeEvent(event);
  emit windowResizedEvent(event->size().width(), event->size().height());
}
