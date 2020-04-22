#include "qthostdisplay.h"
#include "common/assert.h"
#include "frontend-common/imgui_styles.h"
#include "imgui.h"
#include "qtdisplaywidget.h"
#include "qthostinterface.h"
#include <cmath>

QtHostDisplay::QtHostDisplay(QtHostInterface* host_interface) : m_host_interface(host_interface) {}

QtHostDisplay::~QtHostDisplay() = default;

QtDisplayWidget* QtHostDisplay::createWidget(QWidget* parent)
{
  Assert(!m_widget);
  m_widget = new QtDisplayWidget(parent);

  // We want a native window for both D3D and OpenGL.
  m_widget->setAutoFillBackground(false);
  m_widget->setAttribute(Qt::WA_NativeWindow, true);
  m_widget->setAttribute(Qt::WA_NoSystemBackground, true);
  m_widget->setAttribute(Qt::WA_PaintOnScreen, true);

  return m_widget;
}

void QtHostDisplay::destroyWidget()
{
  Assert(m_widget);

  delete m_widget;
  m_widget = nullptr;
}

bool QtHostDisplay::hasDeviceContext() const
{
  return false;
}

bool QtHostDisplay::createDeviceContext(bool debug_device)
{
  return false;
}

bool QtHostDisplay::initializeDeviceContext(bool debug_device)
{
  if (!createImGuiContext() || !createDeviceResources())
    return false;

  return true;
}

bool QtHostDisplay::makeDeviceContextCurrent()
{
  return true;
}

void QtHostDisplay::moveContextToThread(QThread* new_thread) {}

void QtHostDisplay::destroyDeviceContext()
{
  destroyImGuiContext();
  destroyDeviceResources();
}

bool QtHostDisplay::createSurface()
{
  return false;
}

void QtHostDisplay::destroySurface() {}

bool QtHostDisplay::createImGuiContext()
{
  ImGui::CreateContext();

  auto& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.DisplaySize.x = static_cast<float>(m_window_width);
  io.DisplaySize.y = static_cast<float>(m_window_height);

  const float framebuffer_scale = static_cast<float>(m_widget->devicePixelRatioFromScreen());
  io.DisplayFramebufferScale.x = framebuffer_scale;
  io.DisplayFramebufferScale.y = framebuffer_scale;
  ImGui::GetStyle().ScaleAllSizes(framebuffer_scale);

  ImGui::StyleColorsDarker();
  ImGui::AddRobotoRegularFont(15.0f * framebuffer_scale);

  return true;
}

void QtHostDisplay::destroyImGuiContext()
{
  ImGui::DestroyContext();
}

bool QtHostDisplay::createDeviceResources()
{
  return true;
}

void QtHostDisplay::destroyDeviceResources() {}

void QtHostDisplay::WindowResized(s32 new_window_width, s32 new_window_height)
{
  HostDisplay::WindowResized(new_window_width, new_window_height);
  updateImGuiDisplaySize();
}

void QtHostDisplay::updateImGuiDisplaySize()
{
  // imgui may not have been initialized yet
  if (!ImGui::GetCurrentContext())
    return;

  auto& io = ImGui::GetIO();
  io.DisplaySize.x = static_cast<float>(m_window_width);
  io.DisplaySize.y = static_cast<float>(m_window_height);
}
