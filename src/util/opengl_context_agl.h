// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "opengl_context.h"
#include "opengl_loader.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#if defined(__APPLE__) && defined(__OBJC__)
#import <AppKit/AppKit.h>
#else
struct NSOpenGLContext;
struct NSOpenGLPixelFormat;
struct NSView;
#define __bridge
#endif

class OpenGLContextAGL final : public OpenGLContext
{
public:
  OpenGLContextAGL();
  ~OpenGLContextAGL() override;

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

private:
  bool Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try, Error* error);
  bool CreateContext(NSOpenGLContext* share_context, int profile, Error* error);

  static void BindContextToView(WindowInfo& wi, NSOpenGLContext* context);
  static void UpdateSurfaceSize(WindowInfo& wi, NSOpenGLContext* context);

  NSOpenGLContext* m_context = nullptr;
  NSOpenGLPixelFormat* m_pixel_format = nullptr;
  void* m_opengl_module_handle = nullptr;
};

#ifdef __clang__
#pragma clang diagnostic pop
#endif
