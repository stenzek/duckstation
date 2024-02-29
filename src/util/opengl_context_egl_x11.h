// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "opengl_context_egl.h"

class OpenGLContextEGLX11 final : public OpenGLContextEGL
{
public:
  OpenGLContextEGLX11(const WindowInfo& wi);
  ~OpenGLContextEGLX11() override;

  static std::unique_ptr<OpenGLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try,
                                               Error* error);

  std::unique_ptr<OpenGLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

protected:
  EGLDisplay GetPlatformDisplay(Error* error) override;
  EGLSurface CreatePlatformSurface(EGLConfig config, void* win, Error* error) override;
};
