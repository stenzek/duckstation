// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "opengl_context_egl_x11.h"

#include "common/error.h"

OpenGLContextEGLX11::OpenGLContextEGLX11(const WindowInfo& wi) : OpenGLContextEGL(wi)
{
}

OpenGLContextEGLX11::~OpenGLContextEGLX11() = default;

std::unique_ptr<OpenGLContext> OpenGLContextEGLX11::Create(const WindowInfo& wi,
                                                           std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextEGLX11> context = std::make_unique<OpenGLContextEGLX11>(wi);
  if (!context->Initialize(versions_to_try, error))
    return nullptr;

  return context;
}

std::unique_ptr<OpenGLContext> OpenGLContextEGLX11::CreateSharedContext(const WindowInfo& wi, Error* error)
{
  std::unique_ptr<OpenGLContextEGLX11> context = std::make_unique<OpenGLContextEGLX11>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

EGLDisplay OpenGLContextEGLX11::GetPlatformDisplay(const EGLAttrib* attribs, Error* error)
{
  EGLDisplay dpy = TryGetPlatformDisplay(EGL_PLATFORM_X11_KHR, attribs);
  if (dpy == EGL_NO_DISPLAY)
    dpy = GetFallbackDisplay(error);

  return dpy;
}
