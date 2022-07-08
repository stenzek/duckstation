#pragma once
#include "context.h"
#include "loader.h"

#if defined(__APPLE__) && defined(__OBJC__)
#import <AppKit/AppKit.h>
#else
struct NSOpenGLContext;
struct NSOpenGLPixelFormat;
struct NSView;
#define __bridge
#endif

namespace GL {

class ContextAGL final : public Context
{
public:
  ContextAGL(const WindowInfo& wi);
  ~ContextAGL() override;

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
  ALWAYS_INLINE NSView* GetView() const { return static_cast<NSView*>((__bridge NSView*)m_wi.window_handle); }

  bool Initialize(const Version* versions_to_try, size_t num_versions_to_try);
  bool CreateContext(NSOpenGLContext* share_context, int profile, bool make_current);
  void BindContextToView();

  // returns true if dimensions have changed
  bool UpdateDimensions();

  NSOpenGLContext* m_context = nullptr;
  NSOpenGLPixelFormat* m_pixel_format = nullptr;
  void* m_opengl_module_handle = nullptr;
};

} // namespace GL
