#pragma once
#include "context.h"
#include "glad_glx.h"
#include "x11_window.h"

namespace GL {

class ContextGLX final : public Context
{
public:
  ContextGLX(const WindowInfo& wi);
  ~ContextGLX() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  void* GetProcAddress(const char* name) override;
  bool ChangeSurface(const WindowInfo& new_wi) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
  bool SwapBuffers() override;
  bool MakeCurrent() override;
  bool DoneCurrent() override;
  bool SetSwapInterval(s32 interval) override;
  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;

private:
  ALWAYS_INLINE Display* GetDisplay() const { return static_cast<Display*>(m_wi.display_connection); }
  ALWAYS_INLINE GLXDrawable GetDrawable() const { return static_cast<GLXDrawable>(m_window.GetWindow()); }

  bool Initialize(const Version* versions_to_try, size_t num_versions_to_try);
  bool CreateWindow(int screen);
  bool CreateAnyContext(GLXContext share_context, bool make_current);
  bool CreateVersionContext(const Version& version, GLXContext share_context, bool make_current);

  GLXContext m_context = nullptr;
  GLXFBConfig m_fb_config = {};
  XVisualInfo* m_vi = nullptr;
  X11Window m_window;

  // GLAD releases its reference to libGL.so, so we need to maintain our own.
  void* m_libGL_handle = nullptr;
};

} // namespace GL
