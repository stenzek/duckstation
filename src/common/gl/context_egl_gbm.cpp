#include "context_egl_gbm.h"
#include "../assert.h"
#include "../log.h"
#include <drm.h>
#include <drm_fourcc.h>
#include <gbm.h>
Log_SetChannel(GL::ContextEGLGBM);

namespace GL {
ContextEGLGBM::ContextEGLGBM(const WindowInfo& wi) : ContextEGL(wi)
{
#ifdef CONTEXT_EGL_GBM_USE_PRESENT_THREAD
  StartPresentThread();
#endif
}

ContextEGLGBM::~ContextEGLGBM()
{
#ifdef CONTEXT_EGL_GBM_USE_PRESENT_THREAD
  StopPresentThread();
  Assert(!m_current_present_buffer);
#endif

  m_drm_display.RestoreBuffer();

  // We have to destroy the context before the surface/device.
  // Leaving it to the base class would be too late.
  DestroySurface();
  DestroyContext();

  while (m_num_buffers > 0)
  {
    Buffer& buffer = m_buffers[--m_num_buffers];
    m_drm_display.RemoveBuffer(buffer.fb_id);
  }

  if (m_fb_surface)
    gbm_surface_destroy(m_fb_surface);

  if (m_gbm_device)
    gbm_device_destroy(m_gbm_device);
}

std::unique_ptr<Context> ContextEGLGBM::Create(const WindowInfo& wi, const Version* versions_to_try,
                                               size_t num_versions_to_try)
{
  std::unique_ptr<ContextEGLGBM> context = std::make_unique<ContextEGLGBM>(wi);
  if (!context->CreateDisplay() || !context->CreateGBMDevice() ||
      !context->Initialize(versions_to_try, num_versions_to_try))
  {
    return nullptr;
  }

  return context;
}

std::unique_ptr<Context> ContextEGLGBM::CreateSharedContext(const WindowInfo& wi)
{
  std::unique_ptr<ContextEGLGBM> context = std::make_unique<ContextEGLGBM>(wi);
  context->m_display = m_display;

  if (!context->CreateContextAndSurface(m_version, m_context, false))
    return nullptr;

  return context;
}

void ContextEGLGBM::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
  ContextEGL::ResizeSurface(new_surface_width, new_surface_height);
}

bool ContextEGLGBM::CreateGBMDevice()
{
  Assert(!m_gbm_device);
  m_gbm_device = gbm_create_device(m_drm_display.GetCardFD());
  if (!m_gbm_device)
  {
    Log_ErrorPrintf("gbm_create_device() failed: %d", errno);
    return false;
  }

  return true;
}

bool ContextEGLGBM::CreateDisplay()
{
  if (!m_drm_display.Initialize(m_wi.surface_width, m_wi.surface_height, m_wi.surface_refresh_rate))
    return false;

  m_wi.surface_width = m_drm_display.GetWidth();
  m_wi.surface_height = m_drm_display.GetHeight();
  m_wi.surface_refresh_rate = m_drm_display.GetRefreshRate();
  return true;
}

bool ContextEGLGBM::SetDisplay()
{
  if (!eglGetPlatformDisplayEXT)
  {
    Log_ErrorPrintf("eglGetPlatformDisplayEXT() not loaded");
    return false;
  }

  m_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, m_gbm_device, nullptr);
  if (!m_display)
  {
    Log_ErrorPrintf("eglGetPlatformDisplayEXT() failed");
    return false;
  }

  return true;
}

EGLNativeWindowType ContextEGLGBM::GetNativeWindow(EGLConfig config)
{
  EGLint visual_id;
  eglGetConfigAttrib(m_display, config, EGL_NATIVE_VISUAL_ID, &visual_id);

  Assert(!m_fb_surface);
  m_fb_surface = gbm_surface_create(m_gbm_device, m_drm_display.GetWidth(), m_drm_display.GetHeight(),
                                    static_cast<u32>(visual_id), GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
  if (!m_fb_surface)
  {
    Log_ErrorPrintf("gbm_surface_create() failed: %d", errno);
    return {};
  }

  return (EGLNativeWindowType)((void*)m_fb_surface);
}

ContextEGLGBM::Buffer* ContextEGLGBM::LockFrontBuffer()
{
  struct gbm_bo* bo = gbm_surface_lock_front_buffer(m_fb_surface);

  Buffer* buffer = nullptr;
  for (u32 i = 0; i < m_num_buffers; i++)
  {
    if (m_buffers[i].bo == bo)
    {
      buffer = &m_buffers[i];
      break;
    }
  }

  if (!buffer)
  {
    // haven't tracked this buffer yet
    Assert(m_num_buffers < MAX_BUFFERS);

    const u32 width = gbm_bo_get_width(bo);
    const u32 height = gbm_bo_get_height(bo);
    const u32 stride = gbm_bo_get_stride(bo);
    const u32 format = gbm_bo_get_format(bo);
    const u32 handle = gbm_bo_get_handle(bo).u32;

    std::optional<u32> fb_id = m_drm_display.AddBuffer(width, height, format, handle, stride, 0);
    if (!fb_id.has_value())
      return nullptr;

    buffer = &m_buffers[m_num_buffers];
    buffer->bo = bo;
    buffer->fb_id = fb_id.value();
    m_num_buffers++;
  }

  return buffer;
}

void ContextEGLGBM::ReleaseBuffer(Buffer* buffer)
{
  gbm_surface_release_buffer(m_fb_surface, buffer->bo);
}

void ContextEGLGBM::PresentBuffer(Buffer* buffer, bool wait_for_vsync)
{
  m_drm_display.PresentBuffer(buffer->fb_id, wait_for_vsync);
}

bool ContextEGLGBM::SwapBuffers()
{
  if (!ContextEGL::SwapBuffers())
    return false;

#ifdef CONTEXT_EGL_GBM_USE_PRESENT_THREAD
  std::unique_lock lock(m_present_mutex);
  m_present_pending.store(true);
  m_present_cv.notify_one();
  if (m_vsync)
    m_present_done_cv.wait(lock, [this]() { return !m_present_pending.load(); });
#else
  Buffer* front_buffer = LockFrontBuffer();
  if (!front_buffer)
    return false;

  PresentSurface(front_buffer, m_vsync && m_last_front_buffer);

  if (m_last_front_buffer)
    ReleaseBuffer(m_last_front_buffer);

  m_last_front_buffer = front_buffer;
#endif

  return true;
}

bool ContextEGLGBM::SetSwapInterval(s32 interval)
{
  if (interval < 0 || interval > 1)
    return false;

  std::unique_lock lock(m_present_mutex);
  m_vsync = (interval > 0);
  return true;
}

std::vector<Context::FullscreenModeInfo> ContextEGLGBM::EnumerateFullscreenModes()
{
  std::vector<Context::FullscreenModeInfo> modes;
  modes.reserve(m_drm_display.GetModeCount());
  for (u32 i = 0; i < m_drm_display.GetModeCount(); i++)
  {
    modes.push_back(FullscreenModeInfo{m_drm_display.GetModeWidth(i), m_drm_display.GetModeHeight(i),
                                       m_drm_display.GetModeRefreshRate(i)});
  }
  return modes;
}

#ifdef CONTEXT_EGL_GBM_USE_PRESENT_THREAD

void ContextEGLGBM::StartPresentThread()
{
  m_present_thread_shutdown.store(false);
  m_present_thread = std::thread(&ContextEGLGBM::PresentThread, this);
}

void ContextEGLGBM::StopPresentThread()
{
  if (!m_present_thread.joinable())
    return;

  {
    std::unique_lock lock(m_present_mutex);
    m_present_thread_shutdown.store(true);
    m_present_cv.notify_one();
  }

  m_present_thread.join();
}

void ContextEGLGBM::PresentThread()
{
  std::unique_lock lock(m_present_mutex);

  while (!m_present_thread_shutdown.load())
  {
    m_present_cv.wait(lock);

    if (!m_present_pending.load())
      continue;

    Buffer* next_buffer = LockFrontBuffer();
    const bool wait_for_vsync = m_vsync && m_current_present_buffer;

    lock.unlock();
    PresentBuffer(next_buffer, wait_for_vsync);
    lock.lock();

    if (m_current_present_buffer)
      ReleaseBuffer(m_current_present_buffer);

    m_current_present_buffer = next_buffer;
    m_present_pending.store(false);
    m_present_done_cv.notify_one();
  }

  if (m_current_present_buffer)
  {
    ReleaseBuffer(m_current_present_buffer);
    m_current_present_buffer = nullptr;
  }
}

#endif

} // namespace GL
