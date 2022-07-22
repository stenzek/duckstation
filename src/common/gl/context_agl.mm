#include "context_agl.h"
#include "../assert.h"
#include "../log.h"
#include "loader.h"
#include <dlfcn.h>
Log_SetChannel(GL::ContextAGL);

namespace GL {
ContextAGL::ContextAGL(const WindowInfo& wi) : Context(wi)
{
  m_opengl_module_handle = dlopen("/System/Library/Frameworks/OpenGL.framework/Versions/Current/OpenGL", RTLD_NOW);
  if (!m_opengl_module_handle)
    Log_ErrorPrint("Could not open OpenGL.framework, function lookups will probably fail");
}

ContextAGL::~ContextAGL()
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

std::unique_ptr<Context> ContextAGL::Create(const WindowInfo& wi, const Version* versions_to_try,
                                            size_t num_versions_to_try)
{
  std::unique_ptr<ContextAGL> context = std::make_unique<ContextAGL>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

bool ContextAGL::Initialize(const Version* versions_to_try, size_t num_versions_to_try)
{
  for (size_t i = 0; i < num_versions_to_try; i++)
  {
    const Version& cv = versions_to_try[i];
    if (cv.profile == Profile::NoProfile && CreateContext(nullptr, NSOpenGLProfileVersionLegacy, true))
    {
      // we already have the dummy context, so just use that
      m_version = cv;
      return true;
    }
    else if (cv.profile == Profile::Core)
    {
      if (cv.major_version > 4 || cv.minor_version > 1)
        continue;
      
      const NSOpenGLPixelFormatAttribute profile = (cv.major_version > 3 || cv.minor_version > 2) ? NSOpenGLProfileVersion4_1Core : NSOpenGLProfileVersion3_2Core;
      if (CreateContext(nullptr, static_cast<int>(profile), true))
      {        
        m_version = cv;
        return true;
      }
    }
  }

  return false;
}

void* ContextAGL::GetProcAddress(const char* name)
{
  void* addr = m_opengl_module_handle ? dlsym(m_opengl_module_handle, name) : nullptr;
  if (addr)
    return addr;

  return dlsym(RTLD_NEXT, name);
}

bool ContextAGL::ChangeSurface(const WindowInfo& new_wi)
{
  m_wi = new_wi;
  BindContextToView();
  return true;
}

void ContextAGL::ResizeSurface(u32 new_surface_width /*= 0*/, u32 new_surface_height /*= 0*/)
{
  UpdateDimensions();
}

bool ContextAGL::UpdateDimensions()
{
  const NSSize window_size = [GetView() frame].size;
  const CGFloat window_scale = [[GetView() window] backingScaleFactor];
  const u32 new_width = static_cast<u32>(static_cast<CGFloat>(window_size.width) * window_scale);
  const u32 new_height = static_cast<u32>(static_cast<CGFloat>(window_size.height) * window_scale);

  if (m_wi.surface_width == new_width && m_wi.surface_height == new_height)
    return false;

  m_wi.surface_width = new_width;
  m_wi.surface_height = new_height;

  dispatch_block_t block = ^{
    [m_context update];
  };

  if ([NSThread isMainThread])
    block();
  else
    dispatch_sync(dispatch_get_main_queue(), block);

  return true;
}

bool ContextAGL::SwapBuffers()
{
  [m_context flushBuffer];
  return true;
}

bool ContextAGL::MakeCurrent()
{
  [m_context makeCurrentContext];
  return true;
}

bool ContextAGL::DoneCurrent()
{
  [NSOpenGLContext clearCurrentContext];
  return true;
}

bool ContextAGL::SetSwapInterval(s32 interval)
{
  GLint gl_interval = static_cast<GLint>(interval);
  [m_context setValues:&gl_interval forParameter:NSOpenGLCPSwapInterval];
  return true;
}

std::unique_ptr<Context> ContextAGL::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextAGL> context = std::make_unique<ContextAGL>(wi);

  context->m_context = [[NSOpenGLContext alloc] initWithFormat:m_pixel_format shareContext:m_context];
  if (context->m_context == nil)
    return nullptr;

  context->m_version = m_version;
  context->m_pixel_format = m_pixel_format;
  [context->m_pixel_format retain];

  if (wi.type == WindowInfo::Type::MacOS)
    context->BindContextToView();
  
  return context;
}

bool ContextAGL::CreateContext(NSOpenGLContext* share_context, int profile, bool make_current)
{
  if (m_context)
  {
    [m_context release];
    m_context = nullptr;
  }

  if (m_pixel_format)
    [m_pixel_format release];

  const std::array<NSOpenGLPixelFormatAttribute, 5> attribs = {{
      NSOpenGLPFADoubleBuffer,
      NSOpenGLPFAOpenGLProfile,
      static_cast<NSOpenGLPixelFormatAttribute>(profile),
      NSOpenGLPFAAccelerated,
      0}};
  m_pixel_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs.data()];
  if (m_pixel_format == nil)
  {
    Log_ErrorPrintf("Failed to initialize pixel format");
    return false;
  }

  m_context = [[NSOpenGLContext alloc] initWithFormat:m_pixel_format shareContext:nil];
  if (m_context == nil)
    return false;

  if (m_wi.type == WindowInfo::Type::MacOS)
    BindContextToView();

  if (make_current)
    [m_context makeCurrentContext];

  return true;
}

void ContextAGL::BindContextToView()
{
  NSView* const view = GetView();
  NSWindow* const window = [view window];
  [view setWantsBestResolutionOpenGLSurface:YES];

  UpdateDimensions();

  dispatch_block_t block = ^{
    [window makeFirstResponder:view];
    [m_context setView:view];
    [window makeKeyAndOrderFront:nil];
  };

  if ([NSThread isMainThread])
    block();
  else
    dispatch_sync(dispatch_get_main_queue(), block);
}
} // namespace GL
