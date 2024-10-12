// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "opengl_context_egl.h"

#include <wayland-egl.h>

class OpenGLContextEGLWayland final : public OpenGLContextEGL
{
public:
  OpenGLContextEGLWayland();
  ~OpenGLContextEGLWayland() override;

  static std::unique_ptr<OpenGLContext> Create(WindowInfo& wi, SurfaceHandle* surface,
                                               std::span<const Version> versions_to_try, Error* error);

  std::unique_ptr<OpenGLContext> CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface, Error* error) override;

  void ResizeSurface(WindowInfo& wi, SurfaceHandle handle) override;

protected:
  EGLDisplay GetPlatformDisplay(const WindowInfo& wi, Error* error) override;
  EGLSurface CreatePlatformSurface(EGLConfig config, const WindowInfo& wi, Error* error) override;
  void DestroyPlatformSurface(EGLSurface surface) override;

private:
  // Truely awful, I hate this so much, and all this work for a bloody windowing system where
  // we can't even position windows to begin with, which makes multi-window kinda pointless...
  using WLWindowMap = std::unordered_map<EGLSurface, struct wl_egl_window*>;

  bool LoadModule(Error* error);

  void* m_wl_module = nullptr;
  wl_egl_window* (*m_wl_egl_window_create)(struct wl_surface* surface, int width, int height);
  void (*m_wl_egl_window_destroy)(struct wl_egl_window* egl_window);
  void (*m_wl_egl_window_resize)(struct wl_egl_window* egl_window, int width, int height, int dx, int dy);

  WLWindowMap m_wl_window_map;
};
