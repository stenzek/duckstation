// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "context_egl_x11.h"

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

std::unique_ptr<Context> ContextEGLX11::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGLX11> context = std::make_unique<ContextEGLX11>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

void ContextEGLX11::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
  ContextEGL::ResizeSurface(new_surface_width, new_surface_height);
}

EGLNativeWindowType ContextEGLX11::GetNativeWindow(EGLConfig config)
{
  return (EGLNativeWindowType)m_wi.window_handle;
}
} // namespace GL
