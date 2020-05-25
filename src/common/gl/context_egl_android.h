#pragma once
#include "context_egl.h"
#include "x11_window.h"

namespace GL {

class ContextEGLAndroid final : public ContextEGL
{
public:
  ContextEGLAndroid(const WindowInfo& wi);
  ~ContextEGLAndroid() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;

protected:
  EGLNativeWindowType GetNativeWindow(EGLConfig config) override;

private:
  ALWAYS_INLINE Display* GetDisplay() const { return static_cast<Display*>(m_wi.display_connection); }

  X11Window m_window;
};

} // namespace GL
