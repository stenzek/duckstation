#include "context_egl_x11.h"
#include "../log.h"
Log_SetChannel(GL::ContextEGLX11);

namespace GL {
ContextEGLX11::ContextEGLX11(const WindowInfo& wi) : ContextEGL(wi) {}
ContextEGLX11::~ContextEGLX11() = default;

std::unique_ptr<Context> ContextEGLX11::Create(const WindowInfo& wi, const Version* versions_to_try,
                                               size_t num_versions_to_try)
{
  std::unique_ptr<ContextEGLX11> context = std::make_unique<ContextEGLX11>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

std::unique_ptr<Context> ContextEGLX11::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGLX11> context = std::make_unique<ContextEGLX11>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

void ContextEGLX11::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
  m_window.Resize();
  ContextEGL::ResizeSurface(new_surface_width, new_surface_height);
}

EGLNativeWindowType ContextEGLX11::GetNativeWindow(EGLConfig config)
{
  X11InhibitErrors ei;

  EGLint native_visual_id = 0;
  if (!eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &native_visual_id))
  {
    Log_ErrorPrintf("Failed to get X11 visual ID");
    return false;
  }

  XVisualInfo vi_query = {};
  vi_query.visualid = native_visual_id;

  int num_vis;
  XVisualInfo* vi = XGetVisualInfo(static_cast<Display*>(m_wi.display_connection), VisualIDMask, &vi_query, &num_vis);
  if (num_vis <= 0 || !vi)
  {
    Log_ErrorPrintf("Failed to query visual from X11");
    return false;
  }

  m_window.Destroy();
  if (!m_window.Create(GetDisplay(), static_cast<Window>(reinterpret_cast<uintptr_t>(m_wi.window_handle)), vi))
  {
    Log_ErrorPrintf("Faild to create X11 child window");
    XFree(vi);
    return false;
  }

  XFree(vi);
  return static_cast<EGLNativeWindowType>(m_window.GetWindow());
}
} // namespace GL
