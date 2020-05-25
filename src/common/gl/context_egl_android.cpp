#include "context_egl_android.h"
#include "../log.h"
Log_SetChannel(GL::ContextEGLAndroid);

namespace GL {
ContextEGLX11::ContextEGLAndroid(const WindowInfo& wi) : ContextEGL(wi) {}
ContextEGLX11::~ContextEGLAndroid() = default;

std::unique_ptr<Context> ContextEGLAndroid::Create(const WindowInfo& wi, const Version* versions_to_try,
                                               size_t num_versions_to_try)
{
  std::unique_ptr<ContextEGLAndroid> context = std::make_unique<ContextEGLAndroid>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

std::unique_ptr<Context> ContextEGLAndroid::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGLAndroid> context = std::make_unique<ContextEGLAndroid>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

EGLNativeWindowType ContextEGLAndroid::GetNativeWindow(EGLConfig config)
{
  X11InhibitErrors ei;

  EGLint native_visual_id = 0;
  if (!eglGetConfigAttrib(m_display, m_config, EGL_NATIVE_VISUAL_ID, &native_visual_id))
  {
    Log_ErrorPrintf("Failed to get X11 visual ID");
    return false;
  }

  ANativeWindow_setBuffersGeometry(static_cast<ANativeWindow*>(m_wi.window_handle), 0, 0, static_cast<int32_t>(native_visual_id));
  return static_cast<EGLNativeWindowType>(m_wi.window_handle);
}
} // namespace GL
