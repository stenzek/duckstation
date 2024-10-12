// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_context_egl_wayland.h"

#include "common/assert.h"
#include "common/error.h"

#include <dlfcn.h>

static const char* WAYLAND_EGL_MODNAME = "libwayland-egl.so.1";

OpenGLContextEGLWayland::OpenGLContextEGLWayland() = default;

OpenGLContextEGLWayland::~OpenGLContextEGLWayland()
{
  AssertMsg(m_wl_window_map.empty(), "WL window map should be empty on destructor.");
  if (m_wl_module)
    dlclose(m_wl_module);
}

bool OpenGLContextEGLWayland::LoadModule(Error* error)
{
  m_wl_module = dlopen(WAYLAND_EGL_MODNAME, RTLD_NOW | RTLD_GLOBAL);
  if (!m_wl_module)
  {
    const char* err = dlerror();
    Error::SetStringFmt(error, "Loading {} failed: {}", WAYLAND_EGL_MODNAME, err ? err : "<UNKNOWN>");
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
    Error::SetStringFmt(error, "Failed to load one or more functions from {}.", WAYLAND_EGL_MODNAME);
    return false;
  }

  return true;
}

EGLDisplay OpenGLContextEGLWayland::GetPlatformDisplay(const WindowInfo& wi, Error* error)
{
  EGLDisplay dpy = TryGetPlatformDisplay(wi.display_connection, EGL_PLATFORM_WAYLAND_KHR, "EGL_EXT_platform_wayland");
  if (dpy == EGL_NO_DISPLAY)
    dpy = GetFallbackDisplay(wi.display_connection, error);

  return dpy;
}

EGLSurface OpenGLContextEGLWayland::CreatePlatformSurface(EGLConfig config, const WindowInfo& wi, Error* error)
{
  struct wl_egl_window* wl_window =
    m_wl_egl_window_create(static_cast<wl_surface*>(wi.window_handle), wi.surface_width, wi.surface_height);
  if (!wl_window)
  {
    Error::SetStringView(error, "wl_egl_window_create() failed");
    return EGL_NO_SURFACE;
  }

  EGLSurface surface = TryCreatePlatformSurface(config, wl_window, error);
  if (surface == EGL_NO_SURFACE)
  {
    surface = CreateFallbackSurface(config, wl_window, error);
    if (surface == EGL_NO_SURFACE)
    {
      m_wl_egl_window_destroy(wl_window);
      return nullptr;
    }
  }

  m_wl_window_map.emplace(surface, wl_window);
  return surface;
}

void OpenGLContextEGLWayland::ResizeSurface(WindowInfo& wi, SurfaceHandle handle)
{
  const auto it = m_wl_window_map.find((EGLSurface)handle);
  AssertMsg(it != m_wl_window_map.end(), "Missing WL window");
  m_wl_egl_window_resize(it->second, wi.surface_width, wi.surface_height, 0, 0);

  OpenGLContextEGL::ResizeSurface(wi, handle);
}

void OpenGLContextEGLWayland::DestroyPlatformSurface(EGLSurface surface)
{
  const auto it = m_wl_window_map.find((EGLSurface)surface);
  AssertMsg(it != m_wl_window_map.end(), "Missing WL window");
  m_wl_egl_window_destroy(it->second);
  m_wl_window_map.erase(it);

  OpenGLContextEGL::DestroyPlatformSurface(surface);
}

std::unique_ptr<OpenGLContext> OpenGLContextEGLWayland::Create(WindowInfo& wi, SurfaceHandle* surface,
                                                               std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextEGLWayland> context = std::make_unique<OpenGLContextEGLWayland>();
  if (!context->LoadModule(error) || !context->Initialize(wi, surface, versions_to_try, error))
    return nullptr;

  return context;
}

std::unique_ptr<OpenGLContext> OpenGLContextEGLWayland::CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface,
                                                                            Error* error)
{
  std::unique_ptr<OpenGLContextEGLWayland> context = std::make_unique<OpenGLContextEGLWayland>();
  context->m_display = m_display;

  if (!context->LoadModule(error) || !context->CreateContextAndSurface(wi, surface, m_version, m_context, false, error))
    return nullptr;

  return context;
}
