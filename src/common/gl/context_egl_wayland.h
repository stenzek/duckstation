#pragma once
#include "context_egl.h"
#include <wayland-egl.h>

namespace GL {

class ContextEGLWayland final : public ContextEGL
{
public:
  ContextEGLWayland(const WindowInfo& wi);
  ~ContextEGLWayland() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;

protected:
  EGLNativeWindowType GetNativeWindow(EGLConfig config) override;

private:
  bool LoadModule();

  wl_egl_window* m_wl_window = nullptr;

  void* m_wl_module = nullptr;
  wl_egl_window* (*m_wl_egl_window_create)(struct wl_surface* surface, int width, int height);
  void (*m_wl_egl_window_destroy)(struct wl_egl_window* egl_window);
  void (*m_wl_egl_window_resize)(struct wl_egl_window* egl_window, int width, int height, int dx, int dy);
};

} // namespace GL
