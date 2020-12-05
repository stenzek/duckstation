#pragma once
#include "context.h"
#include "glad_egl.h"

namespace GL {

class ContextEGL : public Context
{
public:
  ContextEGL(const WindowInfo& wi);
  ~ContextEGL() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  void* GetProcAddress(const char* name) override;
  virtual bool ChangeSurface(const WindowInfo& new_wi) override;
  virtual void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
  bool SwapBuffers() override;
  bool MakeCurrent() override;
  bool DoneCurrent() override;
  bool SetSwapInterval(s32 interval) override;
  virtual std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;

protected:
  virtual EGLNativeWindowType GetNativeWindow(EGLConfig config);

  bool Initialize(const Version* versions_to_try, size_t num_versions_to_try);
  bool CreateDisplay();
  bool CreateContext(const Version& version, EGLContext share_context);
  bool CreateContextAndSurface(const Version& version, EGLContext share_context, bool make_current);
  bool CreateSurface();
  bool CreatePBufferSurface();

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLSurface m_surface = EGL_NO_SURFACE;
  EGLContext m_context = EGL_NO_CONTEXT;

  EGLConfig m_config = {};

  bool m_supports_surfaceless = false;
};

} // namespace GL
