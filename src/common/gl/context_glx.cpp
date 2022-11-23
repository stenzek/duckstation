#include "context_glx.h"
#include "../assert.h"
#include "../log.h"
#include <dlfcn.h>
Log_SetChannel(GL::ContextGLX);

namespace GL {
ContextGLX::ContextGLX(const WindowInfo& wi) : Context(wi) {}

ContextGLX::~ContextGLX()
{
  if (glXGetCurrentContext() == m_context)
    glXMakeContextCurrent(GetDisplay(), None, None, None);

  if (m_context)
    glXDestroyContext(GetDisplay(), m_context);

  if (m_vi)
    XFree(m_vi);

  if (m_libGL_handle)
    dlclose(m_libGL_handle);
}

std::unique_ptr<Context> ContextGLX::Create(const WindowInfo& wi, const Version* versions_to_try,
                                            size_t num_versions_to_try)
{
  std::unique_ptr<ContextGLX> context = std::make_unique<ContextGLX>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

bool ContextGLX::Initialize(const Version* versions_to_try, size_t num_versions_to_try)
{
  // We need libGL loaded, because GLAD loads its own, then releases it.
  m_libGL_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!m_libGL_handle)
  {
    m_libGL_handle = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!m_libGL_handle)
    {
      Log_ErrorPrintf("Failed to load libGL.so: %s", dlerror());
      return false;
    }
  }

  const int screen = DefaultScreen(GetDisplay());
  if (!gladLoadGLX(GetDisplay(), screen))
  {
    Log_ErrorPrintf("Loading GLAD GLX functions failed");
    return false;
  }

  if (m_wi.type == WindowInfo::Type::X11)
  {
    if (!CreateWindow(screen))
      return false;
  }

  for (size_t i = 0; i < num_versions_to_try; i++)
  {
    const Version& cv = versions_to_try[i];
    if (cv.profile == Profile::NoProfile && CreateAnyContext(nullptr, true))
    {
      m_version = cv;
      return true;
    }
    else if (cv.profile != Profile::NoProfile && CreateVersionContext(cv, nullptr, true))
    {
      m_version = cv;
      return true;
    }
  }

  return false;
}

void* ContextGLX::GetProcAddress(const char* name)
{
  return reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
}

bool ContextGLX::ChangeSurface(const WindowInfo& new_wi)
{
  const bool was_current = (glXGetCurrentContext() == m_context);
  if (was_current)
    glXMakeContextCurrent(GetDisplay(), None, None, None);

  m_window.Destroy();
  m_wi = new_wi;

  if (new_wi.type == WindowInfo::Type::X11)
  {
    const int screen = DefaultScreen(GetDisplay());
    if (!CreateWindow(screen))
      return false;
  }

  if (was_current && !glXMakeContextCurrent(GetDisplay(), GetDrawable(), GetDrawable(), m_context))
  {
    Log_ErrorPrintf("Failed to make context current again after surface change");
    return false;
  }

  return true;
}

void ContextGLX::ResizeSurface(u32 new_surface_width /*= 0*/, u32 new_surface_height /*= 0*/)
{
  m_window.Resize(new_surface_width, new_surface_height);
  m_wi.surface_width = m_window.GetWidth();
  m_wi.surface_height = m_window.GetHeight();
}

bool ContextGLX::SwapBuffers()
{
  glXSwapBuffers(GetDisplay(), GetDrawable());
  return true;
}

bool ContextGLX::MakeCurrent()
{
  return (glXMakeContextCurrent(GetDisplay(), GetDrawable(), GetDrawable(), m_context) == True);
}

bool ContextGLX::DoneCurrent()
{
  return (glXMakeContextCurrent(GetDisplay(), None, None, None) == True);
}

bool ContextGLX::SetSwapInterval(s32 interval)
{
  if (GLAD_GLX_EXT_swap_control)
  {
    glXSwapIntervalEXT(GetDisplay(), GetDrawable(), interval);
    return true;
  }
  else if (GLAD_GLX_MESA_swap_control)
  {
    return (glXSwapIntervalMESA(static_cast<u32>(std::max(interval, 0))) != 0);
  }
  else if (GLAD_GLX_SGI_swap_control)
  {
    return (glXSwapIntervalSGI(interval) != 0);
  }
  else
  {
    return false;
  }
}

std::unique_ptr<Context> ContextGLX::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextGLX> context = std::make_unique<ContextGLX>(wi);
  if (wi.type == WindowInfo::Type::X11)
  {
    const int screen = DefaultScreen(context->GetDisplay());
    if (!context->CreateWindow(screen))
      return nullptr;
  }
  else
  {
    Panic("Create pbuffer");
  }

  if (m_version.profile == Profile::NoProfile)
  {
    if (!context->CreateAnyContext(m_context, false))
      return nullptr;
  }
  else
  {
    if (!context->CreateVersionContext(m_version, m_context, false))
      return nullptr;
  }

  context->m_version = m_version;
  return context;
}

bool ContextGLX::CreateWindow(int screen)
{
  int attribs[32] = {GLX_X_RENDERABLE,  True,           GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                     GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_DOUBLEBUFFER,  True};
  int nattribs = 8;

  switch (m_wi.surface_format)
  {
    case WindowInfo::SurfaceFormat::RGB8:
      attribs[nattribs++] = GLX_RED_SIZE;
      attribs[nattribs++] = 8;
      attribs[nattribs++] = GLX_GREEN_SIZE;
      attribs[nattribs++] = 8;
      attribs[nattribs++] = GLX_BLUE_SIZE;
      attribs[nattribs++] = 8;
      break;

    case WindowInfo::SurfaceFormat::RGBA8:
      attribs[nattribs++] = GLX_RED_SIZE;
      attribs[nattribs++] = 8;
      attribs[nattribs++] = GLX_GREEN_SIZE;
      attribs[nattribs++] = 8;
      attribs[nattribs++] = GLX_BLUE_SIZE;
      attribs[nattribs++] = 8;
      attribs[nattribs++] = GLX_ALPHA_SIZE;
      attribs[nattribs++] = 8;
      break;

    case WindowInfo::SurfaceFormat::RGB565:
      attribs[nattribs++] = GLX_RED_SIZE;
      attribs[nattribs++] = 5;
      attribs[nattribs++] = GLX_GREEN_SIZE;
      attribs[nattribs++] = 6;
      attribs[nattribs++] = GLX_BLUE_SIZE;
      attribs[nattribs++] = 5;
      break;

    case WindowInfo::SurfaceFormat::Auto:
      break;

    default:
      UnreachableCode();
      break;
  }

  attribs[nattribs++] = None;
  attribs[nattribs++] = 0;

  int fbcount = 0;
  GLXFBConfig* fbc = glXChooseFBConfig(GetDisplay(), screen, attribs, &fbcount);
  if (!fbc || !fbcount)
  {
    Log_ErrorPrintf("glXChooseFBConfig() failed");
    return false;
  }
  m_fb_config = *fbc;
  XFree(fbc);

  if (!GLAD_GLX_VERSION_1_3)
  {
    Log_ErrorPrintf("GLX Version 1.3 is required");
    return false;
  }

  m_vi = glXGetVisualFromFBConfig(GetDisplay(), m_fb_config);
  if (!m_vi)
  {
    Log_ErrorPrintf("glXGetVisualFromFBConfig() failed");
    return false;
  }

  return m_window.Create(GetDisplay(), static_cast<Window>(reinterpret_cast<uintptr_t>(m_wi.window_handle)), m_vi);
}

bool ContextGLX::CreateAnyContext(GLXContext share_context, bool make_current)
{
  X11InhibitErrors ie;

  m_context = glXCreateContext(GetDisplay(), m_vi, share_context, True);
  if (!m_context || ie.HadError())
  {
    Log_ErrorPrintf("glxCreateContext() failed");
    return false;
  }

  if (make_current)
  {
    if (!glXMakeCurrent(GetDisplay(), GetDrawable(), m_context))
    {
      Log_ErrorPrintf("glXMakeCurrent() failed");
      return false;
    }
  }

  return true;
}

bool ContextGLX::CreateVersionContext(const Version& version, GLXContext share_context, bool make_current)
{
  // we need create context attribs
  if (!GLAD_GLX_VERSION_1_3)
  {
    Log_ErrorPrint("Missing GLX version 1.3.");
    return false;
  }

  int attribs[32];
  int nattribs = 0;
  attribs[nattribs++] = GLX_CONTEXT_PROFILE_MASK_ARB;
  attribs[nattribs++] =
    ((version.profile == Profile::ES) ?
       ((version.major_version >= 2) ? GLX_CONTEXT_ES2_PROFILE_BIT_EXT : GLX_CONTEXT_ES_PROFILE_BIT_EXT) :
       GLX_CONTEXT_CORE_PROFILE_BIT_ARB);
  attribs[nattribs++] = GLX_CONTEXT_MAJOR_VERSION_ARB;
  attribs[nattribs++] = version.major_version;
  attribs[nattribs++] = GLX_CONTEXT_MINOR_VERSION_ARB;
  attribs[nattribs++] = version.minor_version;
  attribs[nattribs++] = None;
  attribs[nattribs++] = 0;

  X11InhibitErrors ie;
  m_context = glXCreateContextAttribsARB(GetDisplay(), m_fb_config, share_context, True, attribs);
  XSync(GetDisplay(), False);
  if (ie.HadError())
    m_context = nullptr;
  if (!m_context)
    return false;

  if (make_current)
  {
    if (!glXMakeContextCurrent(GetDisplay(), GetDrawable(), GetDrawable(), m_context))
    {
      Log_ErrorPrint("glXMakeContextCurrent() failed");
      glXDestroyContext(GetDisplay(), m_context);
      m_context = nullptr;
      return false;
    }
  }

  return true;
}
} // namespace GL
