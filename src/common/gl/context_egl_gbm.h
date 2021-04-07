#pragma once
#include "../drm_display.h"
#include "context_egl.h"
#include <atomic>
#include <condition_variable>
#include <gbm.h>
#include <mutex>
#include <thread>

#define CONTEXT_EGL_GBM_USE_PRESENT_THREAD 1

namespace GL {

class ContextEGLGBM final : public ContextEGL
{
public:
  ContextEGLGBM(const WindowInfo& wi);
  ~ContextEGLGBM() override;

  static std::unique_ptr<Context> Create(const WindowInfo& wi, const Version* versions_to_try,
                                         size_t num_versions_to_try);

  std::unique_ptr<Context> CreateSharedContext(const WindowInfo& wi) override;
  void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;

  bool SwapBuffers() override;
  bool SetSwapInterval(s32 interval) override;

  std::vector<FullscreenModeInfo> EnumerateFullscreenModes() override;

protected:
  bool SetDisplay() override;
  EGLNativeWindowType GetNativeWindow(EGLConfig config) override;

private:
  enum : u32
  {
    MAX_BUFFERS = 5
  };

  struct Buffer
  {
    struct gbm_bo* bo;
    u32 fb_id;
  };

  bool CreateDisplay();

  bool CreateGBMDevice();
  Buffer* LockFrontBuffer();
  void ReleaseBuffer(Buffer* buffer);
  void PresentBuffer(Buffer* buffer, bool wait_for_vsync);

  void StartPresentThread();
  void StopPresentThread();
  void PresentThread();

  DRMDisplay m_drm_display;
  struct gbm_device* m_gbm_device = nullptr;
  struct gbm_surface* m_fb_surface = nullptr;
  bool m_vsync = true;

#ifdef CONTEXT_EGL_GBM_USE_PRESENT_THREAD
  std::thread m_present_thread;
  std::mutex m_present_mutex;
  std::condition_variable m_present_cv;
  std::atomic_bool m_present_pending{false};
  std::atomic_bool m_present_thread_shutdown{false};
  std::condition_variable m_present_done_cv;

  Buffer* m_current_present_buffer = nullptr;
#endif

  u32 m_num_buffers = 0;
  std::array<Buffer, MAX_BUFFERS> m_buffers{};
};

} // namespace GL
