// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "context.h"

#include "glad_egl.h"

namespace GL {

class ContextEGL : public Context
{
public:
  ContextEGL(const WindowInfo& wi);
  ~ContextEGL() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, std::span<const Version> versions_to_try, Error* error);

  void* GetProcAddress(const char* name) override;
  virtual bool ChangeSurface(const WindowInfo& new_wi) override;
  virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
  bool SwapBuffers() override;
  bool IsCurrent() override;
  bool MakeCurrent() override;
  bool DoneCurrent() override;
  bool SetSwapInterval(s32 interval) override;
  virtual std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;

protected:
  virtual bool SetDisplay();
  virtual EGLNativeWindowType GetNativeWindow(EGLConfig config);

  bool Initialize(std::span<const Version> versions_to_try, Error* error);
  bool CreateDisplay();
  bool CreateContext(const Version& version, EGLContext share_context);
  bool CreateContextAndSurface(const Version& version, EGLContext share_context, bool make_current);
  bool CreateSurface();
  bool CreatePBufferSurface();
  bool CheckConfigSurfaceFormat(EGLConfig config, GPUTexture::Format format);
  GPUTexture::Format GetSurfaceTextureFormat() const;
  void DestroyContext();
  void DestroySurface();

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLSurface m_surface = EGL_NO_SURFACE;
  EGLContext m_context = EGL_NO_CONTEXT;

  EGLConfig m_config = {};

  bool m_supports_surfaceless = false;
};

} // namespace GL
