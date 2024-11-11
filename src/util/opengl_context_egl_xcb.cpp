// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_context_egl_xcb.h"

#include "common/error.h"
#include "common/log.h"

LOG_CHANNEL(GPUDevice);

OpenGLContextEGLXCB::OpenGLContextEGLXCB() = default;

OpenGLContextEGLXCB::~OpenGLContextEGLXCB() = default;

std::unique_ptr<OpenGLContext> OpenGLContextEGLXCB::Create(WindowInfo& wi, SurfaceHandle* surface,
                                                           std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextEGLXCB> context = std::make_unique<OpenGLContextEGLXCB>();
  if (!context->Initialize(wi, surface, versions_to_try, error))
    return nullptr;

  return context;
}

std::unique_ptr<OpenGLContext> OpenGLContextEGLXCB::CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface,
                                                                        Error* error)
{
  std::unique_ptr<OpenGLContextEGLXCB> context = std::make_unique<OpenGLContextEGLXCB>();
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(wi, surface, m_version, m_context, false, error))
    return nullptr;

  return context;
}

EGLDisplay OpenGLContextEGLXCB::GetPlatformDisplay(const WindowInfo& wi, Error* error)
{
  EGLDisplay dpy = TryGetPlatformDisplay(wi.display_connection, EGL_PLATFORM_XCB_EXT, "EGL_EXT_platform_xcb");
  m_using_platform_display = (dpy != EGL_NO_DISPLAY);
  if (!m_using_platform_display)
    dpy = GetFallbackDisplay(wi.display_connection, error);

  return dpy;
}

EGLSurface OpenGLContextEGLXCB::CreatePlatformSurface(EGLConfig config, const WindowInfo& wi, Error* error)
{
  // Try YOLO'ing it, if the depth/visual is compatible we don't need to create a subwindow.
  // Seems to be the case with Mesa, but not on NVIDIA.
  xcb_window_t xcb_window = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(wi.window_handle));
  EGLSurface surface = m_using_platform_display ? TryCreatePlatformSurface(config, &xcb_window, error) :
                                                  CreateFallbackSurface(config, wi.window_handle, error);
  if (surface != EGL_NO_SURFACE)
  {
    // Yay, no subwindow.
    return surface;
  }

  // Why do we need this shit? XWayland on NVIDIA....
  EGLint native_visual_id = 0;
  if (!eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &native_visual_id))
  {
    Error::SetStringView(error, "Failed to get XCB visual ID");
    return EGL_NO_SURFACE;
  }

  X11Window subwindow;
  if (!subwindow.Create(static_cast<xcb_connection_t*>(wi.display_connection), xcb_window,
                        static_cast<xcb_visualid_t>(native_visual_id), error))
  {
    Error::AddPrefix(error, "Failed to create subwindow");
    return EGL_NO_SURFACE;
  }

  // This is hideous.. the EXT version requires a pointer to the window, whereas the base
  // version requires the window itself, casted to void*...
  surface = TryCreatePlatformSurface(config, subwindow.GetWindowPtr(), error);
  if (surface == EGL_NO_SURFACE)
    surface = CreateFallbackSurface(config, wi.window_handle, error);

  if (surface != EGL_NO_SURFACE)
  {
    DEV_LOG("Created {}x{} subwindow with visual ID {}", subwindow.GetWidth(), subwindow.GetHeight(), native_visual_id);
    m_x11_windows.emplace(surface, std::move(subwindow));
  }

  return surface;
}

void OpenGLContextEGLXCB::DestroyPlatformSurface(EGLSurface surface)
{
  OpenGLContextEGL::DestroyPlatformSurface(surface);

  auto it = m_x11_windows.find((EGLSurface)surface);
  if (it != m_x11_windows.end())
    m_x11_windows.erase(it);
}

void OpenGLContextEGLXCB::ResizeSurface(WindowInfo& wi, SurfaceHandle handle)
{
  const auto it = m_x11_windows.find((EGLSurface)handle);
  if (it != m_x11_windows.end())
    it->second.Resize(wi.surface_width, wi.surface_height);
}
