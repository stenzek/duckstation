// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "opengl_context_egl.h"
#include "x11_tools.h"

#include <unordered_map>

class OpenGLContextEGLXCB final : public OpenGLContextEGL
{
public:
  OpenGLContextEGLXCB();
  ~OpenGLContextEGLXCB() override;

  static std::unique_ptr<OpenGLContext> Create(WindowInfo& wi, SurfaceHandle* surface,
                                               std::span<const Version> versions_to_try, Error* error);

  std::unique_ptr<OpenGLContext> CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface, Error* error) override;

  void ResizeSurface(WindowInfo& wi, SurfaceHandle handle) override;

protected:
  EGLDisplay GetPlatformDisplay(const WindowInfo& wi, Error* error) override;
  EGLSurface CreatePlatformSurface(EGLConfig config, const WindowInfo& wi, Error* error) override;
  void DestroyPlatformSurface(EGLSurface surface) override;

private:
  using X11WindowMap = std::unordered_map<EGLSurface, X11Window>;

  X11WindowMap m_x11_windows;
  bool m_using_platform_display = false;
};
