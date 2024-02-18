// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "context_egl_wayland.h"

#include "common/error.h"
#include "common/log.h"

#include <dlfcn.h>

Log_SetChannel(ContextEGL);

namespace GL {
static const char* WAYLAND_EGL_MODNAME = "libwayland-egl.so.1";

ContextEGLWayland::ContextEGLWayland(const WindowInfo& wi) : ContextEGL(wi)
{
}
ContextEGLWayland::~ContextEGLWayland()
{
  if (m_wl_window)
    m_wl_egl_window_destroy(m_wl_window);
  if (m_wl_module)
    dlclose(m_wl_module);
}

std::unique_ptr<Context> ContextEGLWayland::Create(const WindowInfo& wi, std::span<const Version> versions_to_try,
                                                   Error* error)
{
  std::unique_ptr<ContextEGLWayland> context = std::make_unique<ContextEGLWayland>(wi);
  if (!context->LoadModule() || !context->Initialize(versions_to_try, error))
    return nullptr;

  return context;
}

std::unique_ptr<Context> ContextEGLWayland::CreateSharedContext(const WindowInfo& wi, Error* error)
{
  std::unique_ptr<ContextEGLWayland> context = std::make_unique<ContextEGLWayland>(wi);
  context->m_display = m_display;

  if (!context->LoadModule() || !context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

void ContextEGLWayland::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
  if (m_wl_window)
    m_wl_egl_window_resize(m_wl_window, new_surface_width, new_surface_height, 0, 0);

  ContextEGL::ResizeSurface(new_surface_width, new_surface_height);
}

EGLDisplay ContextEGLWayland::GetPlatformDisplay(const EGLAttrib* attribs, Error* error)
{
  EGLDisplay dpy = TryGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, attribs);
  if (dpy == EGL_NO_DISPLAY)
    dpy = GetFallbackDisplay(error);

  return dpy;
}

EGLSurface ContextEGLWayland::CreatePlatformSurface(EGLConfig config, const EGLAttrib* attribs, Error* error)
{
  if (m_wl_window)
  {
    m_wl_egl_window_destroy(m_wl_window);
    m_wl_window = nullptr;
  }

  m_wl_window =
    m_wl_egl_window_create(static_cast<wl_surface*>(m_wi.window_handle), m_wi.surface_width, m_wi.surface_height);
  if (!m_wl_window)
  {
    Error::SetStringView(error, "wl_egl_window_create() failed");
    return EGL_NO_SURFACE;
  }

  EGLSurface surface = EGL_NO_SURFACE;
  if (GLAD_EGL_VERSION_1_5)
  {
    surface = eglCreatePlatformWindowSurface(m_display, config, m_wl_window, attribs);
    if (surface == EGL_NO_SURFACE)
    {
      const EGLint err = eglGetError();
      Error::SetStringFmt(error, "eglCreatePlatformWindowSurface() for Wayland failed: {} (0x{:X})", err, err);
    }
  }
  if (surface == EGL_NO_SURFACE)
    surface = CreateFallbackSurface(config, attribs, m_wl_window, error);

  if (surface == EGL_NO_SURFACE)
  {
    m_wl_egl_window_destroy(m_wl_window);
    m_wl_window = nullptr;
  }

  return surface;
}

bool ContextEGLWayland::LoadModule()
{
  m_wl_module = dlopen(WAYLAND_EGL_MODNAME, RTLD_NOW | RTLD_GLOBAL);
  if (!m_wl_module)
  {
    Log_ErrorPrintf("Failed to load %s.", WAYLAND_EGL_MODNAME);
    return false;
  }

  m_wl_egl_window_create =
    reinterpret_cast<decltype(m_wl_egl_window_create)>(dlsym(m_wl_module, "wl_egl_window_create"));
  m_wl_egl_window_destroy =
    reinterpret_cast<decltype(m_wl_egl_window_destroy)>(dlsym(m_wl_module, "wl_egl_window_destroy"));
  m_wl_egl_window_resize =
    reinterpret_cast<decltype(m_wl_egl_window_resize)>(dlsym(m_wl_module, "wl_egl_window_resize"));
  if (!m_wl_egl_window_create || !m_wl_egl_window_destroy || !m_wl_egl_window_resize)
  {
    Log_ErrorPrintf("Failed to load one or more functions from %s.", WAYLAND_EGL_MODNAME);
    return false;
  }

  return true;
}
} // namespace GL
