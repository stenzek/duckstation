// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "opengl_context_egl.h"

#include <wayland-egl.h>

class OpenGLContextEGLWayland final : public OpenGLContextEGL
{
public:
  OpenGLContextEGLWayland(const WindowInfo& wi);
  ~OpenGLContextEGLWayland() override;

  static std::unique_ptr<OpenGLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try,
                                               Error* error);

  std::unique_ptr<OpenGLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;

protected:
  EGLDisplay GetPlatformDisplay(Error* error) override;
  EGLSurface CreatePlatformSurface(EGLConfig config, void* win, Error* error) override;

private:
  bool LoadModule(Error* error);

  wl_egl_window* m_wl_window = nullptr;

  void* m_wl_module = nullptr;
  wl_egl_window* (*m_wl_egl_window_create)(struct wl_surface* surface, int width, int height);
  void (*m_wl_egl_window_destroy)(struct wl_egl_window* egl_window);
  void (*m_wl_egl_window_resize)(struct wl_egl_window* egl_window, int width, int height, int dx, int dy);
};
