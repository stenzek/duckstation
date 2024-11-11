// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_context_egl_xlib.h"

#include "common/error.h"

OpenGLContextEGLXlib::OpenGLContextEGLXlib() = default;

OpenGLContextEGLXlib::~OpenGLContextEGLXlib() = default;

std::unique_ptr<OpenGLContext> OpenGLContextEGLXlib::Create(WindowInfo& wi, SurfaceHandle* surface,
                                                            std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextEGLXlib> context = std::make_unique<OpenGLContextEGLXlib>();
  if (!context->Initialize(wi, surface, versions_to_try, error))
    return nullptr;

  return context;
}

std::unique_ptr<OpenGLContext> OpenGLContextEGLXlib::CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface,
                                                                         Error* error)
{
  std::unique_ptr<OpenGLContextEGLXlib> context = std::make_unique<OpenGLContextEGLXlib>();
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(wi, surface, m_version, m_context, false, error))
    return nullptr;

  return context;
}

EGLDisplay OpenGLContextEGLXlib::GetPlatformDisplay(const WindowInfo& wi, Error* error)
{
  EGLDisplay dpy = TryGetPlatformDisplay(wi.display_connection, EGL_PLATFORM_X11_KHR, "EGL_EXT_platform_x11");
  if (dpy == EGL_NO_DISPLAY)
    dpy = GetFallbackDisplay(wi.display_connection, error);

  return dpy;
}

EGLSurface OpenGLContextEGLXlib::CreatePlatformSurface(EGLConfig config, const WindowInfo& wi, Error* error)
{
  // This is hideous.. the EXT version requires a pointer to the window, whereas the base
  // version requires the window itself, casted to void*...
  void* win = wi.window_handle;
  EGLSurface surface = TryCreatePlatformSurface(config, &win, error);
  if (surface == EGL_NO_SURFACE)
    surface = CreateFallbackSurface(config, wi.window_handle, error);

  return surface;
}
