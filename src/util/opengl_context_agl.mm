// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "opengl_context_agl.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"

#include "util/gpu_types.h"

#include <dlfcn.h>

#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

LOG_CHANNEL(GPUDevice);

OpenGLContextAGL::OpenGLContextAGL() : OpenGLContext()
{
  m_opengl_module_handle = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/Current/OpenGL", RTLD_NOW);
  if (!m_opengl_module_handle)
    ERROR_LOG("Could not open OpenGL.framework, function lookups will probably fail");
}

OpenGLContextAGL::~OpenGLContextAGL()
{
  if ([NSOpenGLContext currentContext] == m_context)
    [NSOpenGLContext clearCurrentContext];

  if (m_context)
    [m_context release];

  if (m_pixel_format)
    [m_pixel_format release];

  if (m_opengl_module_handle)
    dlclose(m_opengl_module_handle);
}

std::unique_ptr<OpenGLContext> OpenGLContextAGL::Create(WindowInfo& wi, SurfaceHandle* surface,
                                                        std::span<const Version> versions_to_try, Error* error)
{
  std::unique_ptr<OpenGLContextAGL> context = std::make_unique<OpenGLContextAGL>();
  if (!context->Initialize(wi, surface, versions_to_try, error))
    return nullptr;

  return context;
}

bool OpenGLContextAGL::Initialize(WindowInfo& wi, SurfaceHandle* surface, std::span<const Version> versions_to_try,
                                  Error* error)
{
  for (const Version& cv : versions_to_try)
  {
    if (cv.profile == Profile::NoProfile && CreateContext(nullptr, NSOpenGLProfileVersionLegacy, error))
    {
      // we already have the dummy context, so just use that
      BindContextToView(wi, m_context);
      *surface = wi.window_handle;
      m_version = cv;
      return MakeCurrent(*surface, error);
    }
    else if (cv.profile == Profile::Core)
    {
      if (cv.major_version > 4 || cv.minor_version > 1)
        continue;

      const NSOpenGLPixelFormatAttribute profile =
        (cv.major_version > 3 || cv.minor_version > 2) ? NSOpenGLProfileVersion4_1Core : NSOpenGLProfileVersion3_2Core;
      if (CreateContext(nullptr, static_cast<int>(profile), error))
      {
        BindContextToView(wi, m_context);
        *surface = wi.window_handle;
        m_version = cv;
        return MakeCurrent(*surface, error);
      }
    }
  }

  Error::SetStringView(error, "Failed to create any context versions.");
  return false;
}

void* OpenGLContextAGL::GetProcAddress(const char* name)
{
  void* addr = m_opengl_module_handle ? dlsym(m_opengl_module_handle, name) : nullptr;
  if (addr)
    return addr;

  return dlsym(RTLD_NEXT, name);
}

OpenGLContext::SurfaceHandle OpenGLContextAGL::CreateSurface(WindowInfo& wi, Error* error)
{
  if (m_context.view != nil)
  {
    Error::SetStringView(error, "Multiple windows are not supported on this backend.");
    return nullptr;
  }

  BindContextToView(wi, m_context);
  return wi.window_handle;
}

void OpenGLContextAGL::DestroySurface(SurfaceHandle handle)
{
  if (!handle)
    return;

  DebugAssert(m_context.view == handle);
  [m_context setView:nil];
}

void OpenGLContextAGL::ResizeSurface(WindowInfo& wi, SurfaceHandle handle)
{
  DebugAssert(m_context.view == handle);
  UpdateSurfaceSize(wi, m_context);
}

bool OpenGLContextAGL::SwapBuffers()
{
  [m_context flushBuffer];
  return true;
}

bool OpenGLContextAGL::IsCurrent() const
{
  return (m_context != nil && [NSOpenGLContext currentContext] == m_context);
}

bool OpenGLContextAGL::MakeCurrent(SurfaceHandle surface, Error* error)
{
  DebugAssert(surface == m_context.view);
  [m_context makeCurrentContext];
  return true;
}

bool OpenGLContextAGL::DoneCurrent()
{
  [NSOpenGLContext clearCurrentContext];
  return true;
}

bool OpenGLContextAGL::SupportsNegativeSwapInterval() const
{
  return false;
}

bool OpenGLContextAGL::SetSwapInterval(s32 interval, Error* error)
{
  GLint gl_interval = static_cast<GLint>(interval);
  [m_context setValues:&gl_interval forParameter:NSOpenGLCPSwapInterval];
  return true;
}

std::unique_ptr<OpenGLContext> OpenGLContextAGL::CreateSharedContext(WindowInfo& wi, SurfaceHandle* handle,
                                                                     Error* error)
{
  Error::SetStringView(error, "Not supported on this backend.");
  return {};
}

bool OpenGLContextAGL::CreateContext(NSOpenGLContext* share_context, int profile, Error* error)
{
  if (m_context)
  {
    [m_context release];
    m_context = nullptr;
  }

  if (m_pixel_format)
    [m_pixel_format release];

  const std::array<NSOpenGLPixelFormatAttribute, 5> attribs = {{NSOpenGLPFADoubleBuffer, NSOpenGLPFAOpenGLProfile,
                                                                static_cast<NSOpenGLPixelFormatAttribute>(profile),
                                                                NSOpenGLPFAAccelerated, 0}};
  m_pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs.data()];
  if (m_pixel_format == nil)
  {
    Error::SetStringView(error, "Failed to initialize pixel format");
    return false;
  }

  m_context = [[NSOpenGLContext alloc] initWithFormat:m_pixel_format shareContext:nil];
  if (m_context == nil)
  {
    Error::SetStringView(error, "NSOpenGLContext initWithFormat failed");
    return false;
  }

  return true;
}

void OpenGLContextAGL::BindContextToView(WindowInfo& wi, NSOpenGLContext* context)
{
  NSView* const view = static_cast<NSView*>((__bridge NSView*)wi.window_handle);
  NSWindow* const window = [view window];
  [view setWantsBestResolutionOpenGLSurface:YES];

  dispatch_block_t block = ^{
    [window makeFirstResponder:view];
    [context setView:view];
    [window makeKeyAndOrderFront:nil];
  };

  if ([NSThread isMainThread])
    block();
  else
    dispatch_sync(dispatch_get_main_queue(), block);

  const NSSize window_size = [view frame].size;
  const CGFloat window_scale = [[view window] backingScaleFactor];
  wi.surface_width = static_cast<u32>(static_cast<CGFloat>(window_size.width) * window_scale);
  wi.surface_height = static_cast<u32>(static_cast<CGFloat>(window_size.height) * window_scale);
  wi.surface_scale = window_scale;
  wi.surface_format = GPUTextureFormat::RGBA8;
}

void OpenGLContextAGL::UpdateSurfaceSize(WindowInfo& wi, NSOpenGLContext* context)
{
  NSView* const view = static_cast<NSView*>((__bridge NSView*)wi.window_handle);
  const NSSize window_size = [view frame].size;
  const CGFloat window_scale = [[view window] backingScaleFactor];
  const u32 new_width = static_cast<u32>(static_cast<CGFloat>(window_size.width) * window_scale);
  const u32 new_height = static_cast<u32>(static_cast<CGFloat>(window_size.height) * window_scale);

  if (wi.surface_width == new_width && wi.surface_height == new_height && wi.surface_scale == window_scale)
    return;

  wi.surface_width = static_cast<u16>(new_width);
  wi.surface_height = static_cast<u16>(new_height);
  wi.surface_scale = static_cast<float>(window_scale);

  dispatch_block_t block = ^{
    [context update];
  };

  if ([NSThread isMainThread])
    block();
  else
    dispatch_sync(dispatch_get_main_queue(), block);
}
