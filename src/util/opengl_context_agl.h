// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "opengl_context.h"
#include "opengl_loader.h"

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
  OpenGLContextAGL(const WindowInfo& wi);
  ~OpenGLContextAGL() override;

  static std::unique_ptr<OpenGLContext> Create(const WindowInfo& wi, std::span<const Version> versions_to_try,
                                               Error* error);

  void* GetProcAddress(const char* name) override;
  bool ChangeSurface(const WindowInfo& new_wi) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
  bool SwapBuffers() override;
  bool IsCurrent() const override;
  bool MakeCurrent() override;
  bool DoneCurrent() override;
  bool SupportsNegativeSwapInterval() const override;
  bool SetSwapInterval(s32 interval) override;
  std::unique_ptr<OpenGLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

private:
  ALWAYS_INLINE NSView* GetView() const { return static_cast<NSView*>((__bridge NSView*)m_wi.window_handle); }

  bool Initialize(std::span<const Version> versions_to_try, Error* error);
  bool CreateContext(NSOpenGLContext* share_context, int profile, bool make_current, Error* error);
  void BindContextToView();

  // returns true if dimensions have changed
  bool UpdateDimensions();

  NSOpenGLContext* m_context = nullptr;
  NSOpenGLPixelFormat* m_pixel_format = nullptr;
  void* m_opengl_module_handle = nullptr;
};
