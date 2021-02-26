#pragma once
#include "context_egl.h"

namespace GL {

class ContextEGLFBDev final : public ContextEGL
{
public:
  ContextEGLFBDev(const WindowInfo& wi);
  ~ContextEGLFBDev() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;

protected:
  EGLNativeWindowType GetNativeWindow(EGLConfig config) override;
};

} // namespace GL
