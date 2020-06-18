#include "qthostdisplay.h"
#include "common/assert.h"
#include "frontend-common/imgui_styles.h"
#include "imgui.h"
#include "qtdisplaywidget.h"
#include "qthostinterface.h"
#include <QtGui/QGuiApplication>
#include <QtCore/QDebug>
#include <cmath>
#if !defined(WIN32) && !defined(APPLE)
#include <qpa/qplatformnativeinterface.h>
#endif

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

bool QtHostDisplay::initializeDeviceContext(std::string_view shader_cache_directory, bool debug_device)
{
  if (!createImGuiContext() || !createDeviceResources())
    return false;

  return true;
}

bool QtHostDisplay::activateDeviceContext()
{
  return true;
}

void QtHostDisplay::deactivateDeviceContext() {}

void QtHostDisplay::destroyDeviceContext()
{
  destroyImGuiContext();
  destroyDeviceResources();
}

bool QtHostDisplay::recreateSurface()
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

std::optional<WindowInfo> QtHostDisplay::getWindowInfo() const
{
  WindowInfo wi;

  // Windows and Apple are easy here since there's no display connection.
#if defined(WIN32)
  wi.type = WindowInfo::Type::Win32;
  wi.window_handle = reinterpret_cast<void*>(m_widget->winId());
#elif defined(__APPLE__)
  wi.type = WindowInfo::Type::MacOS;
  wi.window_handle = reinterpret_cast<void*>(m_widget->winId());
#else
  QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
  const QString platform_name = QGuiApplication::platformName();
  if (platform_name == QStringLiteral("xcb"))
  {
    wi.type = WindowInfo::Type::X11;
    wi.display_connection = pni->nativeResourceForWindow("display", m_widget->windowHandle());
    wi.window_handle = reinterpret_cast<void*>(m_widget->winId());
  }
  else if (platform_name == QStringLiteral("wayland"))
  {
    wi.type = WindowInfo::Type::Wayland;
    wi.display_connection = pni->nativeResourceForWindow("display", m_widget->windowHandle());
    wi.window_handle = pni->nativeResourceForWindow("surface", m_widget->windowHandle());
  }
  else
  {
    qCritical() << "Unknown PNI platform " << platform_name;
    return std::nullopt;
  }
#endif

  wi.surface_width = m_widget->width();
  wi.surface_height = m_widget->height();
  wi.surface_format = WindowInfo::SurfaceFormat::RGB8;

  return wi;
}
