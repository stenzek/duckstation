// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "context_egl_x11.h"

#include "common/error.h"

namespace GL {
ContextEGLX11::ContextEGLX11(const WindowInfo& wi) : ContextEGL(wi)
{
}
ContextEGLX11::~ContextEGLX11() = default;

std::unique_ptr<Context> ContextEGLX11::Create(const WindowInfo& wi, std::span<const Version> versions_to_try,
                                               Error* error)
{
  std::unique_ptr<ContextEGLX11> context = std::make_unique<ContextEGLX11>(wi);
  if (!context->Initialize(versions_to_try, error))
    return nullptr;

  return context;
}

std::unique_ptr<Context> ContextEGLX11::CreateSharedContext(const WindowInfo& wi, Error* error)
{
  std::unique_ptr<ContextEGLX11> context = std::make_unique<ContextEGLX11>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

EGLDisplay ContextEGLX11::GetPlatformDisplay(const EGLAttrib* attribs, Error* error)
{
  EGLDisplay dpy = TryGetPlatformDisplay(EGL_PLATFORM_X11_KHR, attribs);
  if (dpy == EGL_NO_DISPLAY)
    dpy = GetFallbackDisplay(error);

  return dpy;
}

} // namespace GL
