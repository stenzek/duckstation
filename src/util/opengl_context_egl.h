// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "opengl_context.h"

#include "glad/egl.h"

class OpenGLContextEGL : public OpenGLContext
{
public:
  OpenGLContextEGL();
  ~OpenGLContextEGL() override;

  static std::unique_ptr<OpenGLContext> Create(WindowInfo& wi, SurfaceHandle* surface,
                                               std::span<const Version> versions_to_try, Error* error);

  void* GetProcAddress(const char* name) override;
  SurfaceHandle CreateSurface(WindowInfo& wi, Error* error = nullptr) override;
  void DestroySurface(SurfaceHandle handle) override;
  void ResizeSurface(WindowInfo& wi, SurfaceHandle handle) override;
  bool SwapBuffers() override;
  bool IsCurrent() const override;
  bool MakeCurrent(SurfaceHandle surface, Error* error = nullptr) override;
  bool DoneCurrent() override;
  bool SupportsNegativeSwapInterval() const override;
  bool SetSwapInterval(s32 interval, Error* error = nullptr) override;
  std::unique_ptr<OpenGLContext> CreateSharedContext(WindowInfo& wi, SurfaceHandle* surface, Error* error) override;

protected:
  virtual EGLDisplay GetPlatformDisplay(const WindowInfo& wi, Error* error);
  virtual EGLSurface CreatePlatformSurface(EGLConfig config, const WindowInfo& wi, Error* error);
  virtual void DestroyPlatformSurface(EGLSurface surface);

  bool SupportsSurfaceless() const;

  EGLDisplay TryGetPlatformDisplay(void* display, EGLenum platform, const char* platform_ext);
  EGLSurface TryCreatePlatformSurface(EGLConfig config, void* window, Error* error);
  EGLDisplay GetFallbackDisplay(void* display, Error* error);
  EGLSurface CreateFallbackSurface(EGLConfig config, void* window, Error* error);

  bool Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try, Error* error);
  bool CreateContext(bool surfaceless, GPUTextureFormat surface_format, const Version& version,
                     EGLContext share_context, Error* error);
  bool CreateContextAndSurface(WindowInfo& wi, SurfaceHandle* surface, const Version& version, EGLContext share_context,
                               bool make_current, Error* error);
  EGLSurface GetPBufferSurface(Error* error);
  EGLSurface GetSurfacelessSurface();
  bool CheckConfigSurfaceFormat(EGLConfig config, GPUTextureFormat format);
  void UpdateWindowInfoSize(WindowInfo& wi, EGLSurface surface) const;

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLContext m_context = EGL_NO_CONTEXT;
  EGLSurface m_current_surface = EGL_NO_SURFACE;

  EGLConfig m_config = {};

  EGLSurface m_pbuffer_surface = EGL_NO_SURFACE;

  bool m_use_ext_platform_base = false;
  bool m_supports_negative_swap_interval = false;
};
