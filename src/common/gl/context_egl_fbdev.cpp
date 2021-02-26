#include "context_egl_fbdev.h"

namespace GL {
ContextEGLFBDev::ContextEGLFBDev(const WindowInfo& wi) : ContextEGL(wi) {}
ContextEGLFBDev::~ContextEGLFBDev() = default;

std::unique_ptr<Context> ContextEGLFBDev::Create(const WindowInfo& wi, const Version* versions_to_try,
                                               size_t num_versions_to_try)
{
  std::unique_ptr<ContextEGLFBDev> context = std::make_unique<ContextEGLFBDev>(wi);
  if (!context->Initialize(versions_to_try, num_versions_to_try))
    return nullptr;

  return context;
}

std::unique_ptr<Context> ContextEGLFBDev::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGLFBDev> context = std::make_unique<ContextEGLFBDev>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

EGLNativeWindowType ContextEGLFBDev::GetNativeWindow(EGLConfig config)
{
  return static_cast<EGLNativeWindowType>(0);
}
} // namespace GL
