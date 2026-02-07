// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "video_thread.h"
#include "core.h"
#include "fullscreenui.h"
#include "gpu_backend.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_types.h"
#include "host.h"
#include "imgui_overlays.h"
#include "performance_counters.h"
#include "settings.h"
#include "shader_cache_version.h"
#include "system.h"
#include "system_private.h"
#include "video_presenter.h"
#include "video_thread_commands.h"

#include "util/gpu_device.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"
#include "util/state_wrapper.h"
#include "util/translation.h"

#include "common/align.h"
#include "common/error.h"
#include "common/log.h"
#include "common/threading.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome.h"
#include "fmt/format.h"
#include "imgui.h"

#include <optional>

LOG_CHANNEL(VideoThread);

namespace VideoThread {
enum : u32
{
  COMMAND_QUEUE_SIZE = 16 * 1024 * 1024,
  THRESHOLD_TO_WAKE_GPU = 65536,
};

static constexpr s32 THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING = 0x40000000; // CPU thread needs waking
static constexpr s32 THREAD_WAKE_COUNT_SLEEPING = -1;

// Use a slightly longer spin time on ARM64 due to power management.
#ifndef _M_ARM64
static constexpr u32 THREAD_SPIN_TIME_US = 50;
#else
static constexpr u32 THREAD_SPIN_TIME_US = 200;
#endif

static bool Reconfigure(std::optional<GPURenderer> renderer, bool upload_vram, std::optional<bool> fullscreen,
                        std::optional<bool> start_fullscreen_ui, bool recreate_device, Error* error);

// NOTE: Use with care! The handler needs to manually run the destructor.
template<class T, typename... Args>
T* AllocateCommand(u32 size, VideoThreadCommandType type, Args... args);
template<class T, typename... Args>
T* AllocateCommand(VideoThreadCommandType type, Args... args);

static u32 GetPendingCommandSize();
static void ResetCommandFIFO();
static bool IsCommandFIFOEmpty();
static void WakeThread();
static void WakeThreadIfSleeping();
static bool SleepThread(bool allow_sleep);

static bool CreateDeviceOnThread(RenderAPI api, bool fullscreen, bool start_fullscreen_ui,
                                 bool preserve_imgui_on_failure, Error* error);
static void DestroyDeviceOnThread(bool preserve_imgui_state);
static void ResizeRenderWindowOnThread(u32 width, u32 height, float scale, float refresh_rate);
static void RecreateRenderWindowOnThread(bool fullscreen, bool allow_exclusive_fullscreen);
static void RenderWindowResizedOnThread();
static bool CheckExclusiveFullscreenOnThread();

static void ReconfigureOnThread(VideoThreadReconfigureCommand* cmd);
static bool CreateGPUBackendOnThread(bool hardware_renderer, bool upload_vram, const GPUSettings* old_settings,
                                     GPURenderer* out_renderer, Error* error);
static void DestroyGPUBackendOnThread();
static void DestroyGPUPresenterOnThread();

static void SetThreadEnabled(bool enabled);
static void UpdateSettingsOnThread(GPUSettings&& new_settings);
static void UpdateGameInfoOnThread(VideoThreadUpdateGameInfoCommand* cmd);
static void GameInfoChanged(bool serial_changed);
static void ClearGameInfoOnThread();

static void UpdateRunIdle();

namespace {

struct ALIGN_TO_CACHE_LINE State
{
  // Owned by CPU thread.
  Timer::Value thread_spin_time = 0;
  Threading::ThreadHandle thread_handle;
  Common::unique_aligned_ptr<u8[]> command_fifo_data;
  WindowInfo render_window_info;
  std::optional<GPURenderer> requested_renderer;
  bool requested_fullscreen_ui = false;
  bool use_thread = false;
  bool fullscreen_state = false;

  // Hot variables between both threads.
  ALIGN_TO_CACHE_LINE std::atomic<u32> command_fifo_write_ptr{0};
  std::atomic<s32> thread_wake_count{0}; // <0 = sleeping, >= 0 = has work
  Threading::KernelSemaphore thread_wake_semaphore;
  Threading::KernelSemaphore thread_is_done_semaphore;

  // Owned by GPU thread.
  ALIGN_TO_CACHE_LINE Common::unique_aligned_ptr<GPUBackend> gpu_backend;
  std::atomic<u32> command_fifo_read_ptr{0};
  u8 run_idle_reasons = 0;
  bool run_idle_flag = false;
  GPUVSyncMode requested_vsync = GPUVSyncMode::Disabled;
  std::string game_title;
  std::string game_serial;
  std::string game_path;
  GameHash game_hash = 0;
};

} // namespace

static State s_state;

} // namespace VideoThread

const Threading::ThreadHandle& VideoThread::Internal::GetThreadHandle()
{
  return s_state.thread_handle;
}

void VideoThread::ResetCommandFIFO()
{
  Assert(!s_state.run_idle_flag && s_state.command_fifo_read_ptr.load(std::memory_order_acquire) ==
                                     s_state.command_fifo_write_ptr.load(std::memory_order_relaxed));
  s_state.command_fifo_write_ptr.store(0, std::memory_order_release);
  s_state.command_fifo_read_ptr.store(0, std::memory_order_release);
}

void VideoThread::Internal::ProcessStartup()
{
  s_state.thread_spin_time = Timer::ConvertNanosecondsToValue(THREAD_SPIN_TIME_US * 1000.0);
  s_state.command_fifo_data = Common::make_unique_aligned_for_overwrite<u8[]>(HOST_CACHE_LINE_SIZE, COMMAND_QUEUE_SIZE);
  s_state.use_thread = g_settings.gpu_use_thread;
  s_state.run_idle_reasons = static_cast<u8>(RunIdleReason::NoGPUBackend);
}

void VideoThread::Internal::RequestShutdown()
{
  INFO_LOG("Shutting down video thread...");
  SyncThread(false);

  // Thread must be enabled to shut it down.
  SetThreadEnabled(true);
  PushCommandAndWakeThread(AllocateCommand(VideoThreadCommandType::Shutdown, sizeof(VideoThreadCommand)));
}

VideoThreadCommand* VideoThread::AllocateCommand(VideoThreadCommandType command, u32 size)
{
  size = VideoThreadCommand::AlignCommandSize(size);

  for (;;)
  {
    u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
    u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed);
    if (read_ptr > write_ptr) [[unlikely]]
    {
      u32 available_size = read_ptr - write_ptr;
      while (available_size < size)
      {
        WakeThreadIfSleeping();
        read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
        available_size = (read_ptr > write_ptr) ? (read_ptr - write_ptr) : (COMMAND_QUEUE_SIZE - write_ptr);
      }
    }
    else
    {
      const u32 available_size = COMMAND_QUEUE_SIZE - write_ptr;
      if (size > available_size) [[unlikely]]
      {
        // Can't wrap around until the video thread has at least started processing commands...
        if (read_ptr == 0) [[unlikely]]
        {
          DEV_LOG("Buffer full and unprocessed, spinning");
          do
          {
            WakeThreadIfSleeping();
            read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
          } while (read_ptr == 0);
        }

        // allocate a dummy command to wrap the buffer around
        VideoThreadCommand* dummy_cmd = reinterpret_cast<VideoThreadCommand*>(&s_state.command_fifo_data[write_ptr]);
        dummy_cmd->type = VideoThreadCommandType::Wraparound;
        dummy_cmd->size = available_size;
        s_state.command_fifo_write_ptr.store(0, std::memory_order_release);
        continue;
      }
    }

    VideoThreadCommand* cmd = reinterpret_cast<VideoThreadCommand*>(&s_state.command_fifo_data[write_ptr]);
    cmd->type = command;
    cmd->size = size;
    return cmd;
  }
}

template<class T, typename... Args>
T* VideoThread::AllocateCommand(u32 size, VideoThreadCommandType type, Args... args)
{
  VideoThreadCommand* cmd = AllocateCommand(type, size);
  const u32 alloc_size = cmd->size;

  new (cmd) T(std::forward<Args>(args)...);

  // constructor may overwrite the fields, need to reset them
  cmd->type = type;
  cmd->size = alloc_size;

  return static_cast<T*>(cmd);
}

template<class T, typename... Args>
T* VideoThread::AllocateCommand(VideoThreadCommandType type, Args... args)
{
  return AllocateCommand<T>(sizeof(T), type, std::forward<Args>(args)...);
}

u32 VideoThread::GetPendingCommandSize()
{
  const u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
  const u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed);
  return (write_ptr >= read_ptr) ? (write_ptr - read_ptr) : (COMMAND_QUEUE_SIZE - read_ptr + write_ptr);
}

bool VideoThread::IsCommandFIFOEmpty()
{
  const u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
  const u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed);
  return (read_ptr == write_ptr);
}

void VideoThread::PushCommand(VideoThreadCommand* cmd)
{
  if (!s_state.use_thread) [[unlikely]]
  {
    DebugAssert(s_state.gpu_backend);
    s_state.gpu_backend->HandleCommand(cmd);
    return;
  }

  const u32 new_write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed) + cmd->size;
  DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
  s_state.command_fifo_write_ptr.store(new_write_ptr % COMMAND_QUEUE_SIZE, std::memory_order_release);
  if (GetPendingCommandSize() >= THRESHOLD_TO_WAKE_GPU) // TODO:FIXME: maybe purge this?
    WakeThread();
}

void VideoThread::PushCommandAndWakeThread(VideoThreadCommand* cmd)
{
  if (!s_state.use_thread) [[unlikely]]
  {
    DebugAssert(s_state.gpu_backend);
    s_state.gpu_backend->HandleCommand(cmd);
    return;
  }

  const u32 new_write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed) + cmd->size;
  DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
  s_state.command_fifo_write_ptr.store(new_write_ptr % COMMAND_QUEUE_SIZE, std::memory_order_release);
  WakeThread();
}

void VideoThread::PushCommandAndSync(VideoThreadCommand* cmd, bool spin)
{
  if (!s_state.use_thread) [[unlikely]]
  {
    DebugAssert(s_state.gpu_backend);
    s_state.gpu_backend->HandleCommand(cmd);
    return;
  }

  const u32 new_write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed) + cmd->size;
  DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
  s_state.command_fifo_write_ptr.store(new_write_ptr % COMMAND_QUEUE_SIZE, std::memory_order_release);
  WakeThread();
  SyncThread(spin);
}

ALWAYS_INLINE s32 GetThreadWakeCount(s32 state)
{
  return (state & ~VideoThread::THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING);
}

void VideoThread::WakeThread()
{
  // If sleeping, state will be <0, otherwise this will increment the pending work count.
  // We add 2 so that there's a positive work count if we were sleeping, otherwise the thread would go to sleep.
  if (s_state.thread_wake_count.fetch_add(2, std::memory_order_release) < 0)
    s_state.thread_wake_semaphore.Post();
}

ALWAYS_INLINE_RELEASE void VideoThread::WakeThreadIfSleeping()
{
  if (GetThreadWakeCount(s_state.thread_wake_count.load(std::memory_order_acquire)) < 0)
  {
    if (IsCommandFIFOEmpty())
      return;

    WakeThread();
  }
}

void VideoThread::SyncThread(bool spin)
{
  if (!s_state.use_thread)
    return;

  if (spin)
  {
    // Check if the video thread is done/sleeping.
    if (GetThreadWakeCount(s_state.thread_wake_count.load(std::memory_order_acquire)) < 0)
    {
      if (IsCommandFIFOEmpty())
        return;

      WakeThread();
    }

    const Timer::Value start_time = Timer::GetCurrentValue();
    Timer::Value current_time = start_time;
    do
    {
      // Check if the video thread is done/sleeping.
      if (GetThreadWakeCount(s_state.thread_wake_count.load(std::memory_order_acquire)) < 0)
      {
        if (IsCommandFIFOEmpty())
          return;

        WakeThread();
        continue;
      }

      // Hopefully ought to be enough.
      MultiPause();

      current_time = Timer::GetCurrentValue();
    } while ((current_time - start_time) < s_state.thread_spin_time);
  }

  // s_thread_wake_count |= THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING if not zero
  s32 value;
  do
  {
    // Check if the video thread is done/sleeping.
    value = s_state.thread_wake_count.load(std::memory_order_acquire);
    if (GetThreadWakeCount(value) < 0)
    {
      if (IsCommandFIFOEmpty())
        return;

      WakeThread();
      continue;
    }
  } while (!s_state.thread_wake_count.compare_exchange_weak(value, value | THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING,
                                                            std::memory_order_acq_rel, std::memory_order_relaxed));
  s_state.thread_is_done_semaphore.Wait();
}

bool VideoThread::SleepThread(bool allow_sleep)
{
  DebugAssert(!allow_sleep || s_state.thread_wake_count.load(std::memory_order_relaxed) >= 0);
  for (;;)
  {
    // Acknowledge any work that has been queued, but preserve the waiting flag if there is any, since we're not done
    // yet.
    s32 old_state, new_state;
    do
    {
      old_state = s_state.thread_wake_count.load(std::memory_order_relaxed);
      new_state = (GetThreadWakeCount(old_state) > 0) ? (old_state & THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING) :
                                                        (allow_sleep ? THREAD_WAKE_COUNT_SLEEPING : 0);
    } while (!s_state.thread_wake_count.compare_exchange_weak(old_state, new_state, std::memory_order_acq_rel,
                                                              std::memory_order_relaxed));

    // Are we not done yet?
    if (GetThreadWakeCount(old_state) > 0)
      return true;

    // We're done, so wake the CPU thread if it's waiting.
    if (old_state & THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING)
      s_state.thread_is_done_semaphore.Post();

    // Sleep until more work is queued.
    if (allow_sleep)
      s_state.thread_wake_semaphore.Wait();
    else
      return false;
  }
}

void VideoThread::Internal::VideoThreadEntryPoint()
{
  s_state.thread_handle = Threading::ThreadHandle::GetForCallingThread();

  // Take a local copy of the FIFO, that way it's not ping-ponging between the threads.
  u8* const command_fifo_data = s_state.command_fifo_data.get();

  for (;;)
  {
    u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_acquire);
    u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_relaxed);
    if (read_ptr == write_ptr)
    {
      if (SleepThread(!s_state.run_idle_flag))
      {
        // sleep => wake, need to reload pointers
        continue;
      }
      else
      {
        DoRunIdle();
        continue;
      }
    }

    write_ptr = (write_ptr < read_ptr) ? COMMAND_QUEUE_SIZE : write_ptr;
    while (read_ptr < write_ptr)
    {
      VideoThreadCommand* cmd = reinterpret_cast<VideoThreadCommand*>(&command_fifo_data[read_ptr]);
      DebugAssert((read_ptr + cmd->size) <= COMMAND_QUEUE_SIZE);
      read_ptr += cmd->size;

      if (cmd->type > VideoThreadCommandType::Shutdown) [[likely]]
      {
        DebugAssert(s_state.gpu_backend);
        s_state.gpu_backend->HandleCommand(cmd);
        continue;
      }

      switch (cmd->type)
      {
        case VideoThreadCommandType::Wraparound:
        {
          DebugAssert(read_ptr == COMMAND_QUEUE_SIZE);
          write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_acquire);
          read_ptr = 0;

          // let the CPU thread know as early as possible that we're here
          s_state.command_fifo_read_ptr.store(read_ptr, std::memory_order_release);
        }
        break;

        case VideoThreadCommandType::AsyncCall:
        {
          VideoThreadAsyncCallCommand* acmd = static_cast<VideoThreadAsyncCallCommand*>(cmd);
          acmd->func();
          acmd->~VideoThreadAsyncCallCommand();
        }
        break;

        case VideoThreadCommandType::AsyncBackendCall:
        {
          VideoThreadAsyncBackendCallCommand* acmd = static_cast<VideoThreadAsyncBackendCallCommand*>(cmd);
          acmd->func(s_state.gpu_backend.get());
          acmd->~VideoThreadAsyncBackendCallCommand();
        }
        break;

        case VideoThreadCommandType::Reconfigure:
        {
          VideoThreadReconfigureCommand* ccmd = static_cast<VideoThreadReconfigureCommand*>(cmd);
          ReconfigureOnThread(ccmd);
          ccmd->~VideoThreadReconfigureCommand();
        }
        break;

        case VideoThreadCommandType::UpdateSettings:
        {
          VideoThreadUpdateSettingsCommand* ccmd = static_cast<VideoThreadUpdateSettingsCommand*>(cmd);
          UpdateSettingsOnThread(std::move(ccmd->settings));
          ccmd->~VideoThreadUpdateSettingsCommand();
        }
        break;

        case VideoThreadCommandType::UpdateGameInfo:
        {
          VideoThreadUpdateGameInfoCommand* ccmd = static_cast<VideoThreadUpdateGameInfoCommand*>(cmd);
          UpdateGameInfoOnThread(ccmd);
          ccmd->~VideoThreadUpdateGameInfoCommand();
        }
        break;

        case VideoThreadCommandType::Shutdown:
        {
          // Should have consumed everything, and be shutdown.
          DebugAssert(read_ptr == write_ptr);
          s_state.command_fifo_read_ptr.store(read_ptr, std::memory_order_release);
          return;
        }
        break;

          DefaultCaseIsUnreachable();
      }
    }

    // if the command was aligned to the end of the buffer, read_ptr will be COMMAND_QUEUE_SIZE.
    DebugAssert(read_ptr <= COMMAND_QUEUE_SIZE);
    s_state.command_fifo_read_ptr.store(read_ptr % COMMAND_QUEUE_SIZE, std::memory_order_release);
  }

  s_state.thread_handle = {};
}

void VideoThread::Internal::DoRunIdle()
{
  if (!g_gpu_device->HasMainSwapChain()) [[unlikely]]
  {
    // only happens during language switch
    Timer::NanoSleep(16 * 1000 * 1000);
    return;
  }

  if (!PresentFrameAndRestoreContext())
    return;

  if (g_gpu_device->HasMainSwapChain() && !g_gpu_device->GetMainSwapChain()->IsVSyncModeBlocking())
    VideoPresenter::ThrottlePresentation();
}

bool VideoThread::Reconfigure(std::optional<GPURenderer> renderer, bool upload_vram, std::optional<bool> fullscreen,
                              std::optional<bool> start_fullscreen_ui, bool recreate_device, Error* error)
{
  INFO_LOG("Reconfiguring video thread.");

  const bool new_requested_fullscreen_ui = start_fullscreen_ui.value_or(s_state.requested_fullscreen_ui);
  const bool new_fullscreen_state =
    ((renderer.has_value() || new_requested_fullscreen_ui) && fullscreen.value_or(s_state.fullscreen_state));

  GPURenderer created_renderer = GPURenderer::Count;
  VideoThreadReconfigureCommand::Result result = VideoThreadReconfigureCommand::Result::Failed;

  VideoThreadReconfigureCommand* cmd =
    AllocateCommand<VideoThreadReconfigureCommand>(VideoThreadCommandType::Reconfigure);
  cmd->renderer = renderer;
  cmd->fullscreen = new_fullscreen_state;
  cmd->start_fullscreen_ui = new_requested_fullscreen_ui;
  cmd->vsync_mode = System::GetEffectiveVSyncMode();
  cmd->present_skip_mode = System::GetEffectivePresentSkipMode();
  cmd->force_recreate_device = recreate_device;
  cmd->upload_vram = upload_vram;
  cmd->error_ptr = error;
  cmd->out_result = &result;
  cmd->out_created_renderer = &created_renderer;
  cmd->settings = g_settings;

  if (!s_state.use_thread) [[unlikely]]
    ReconfigureOnThread(cmd);
  else
    PushCommandAndSync(cmd, false);

  // Update CPU thread state.
  if (result == VideoThreadReconfigureCommand::Result::FailedWithDeviceLoss)
  {
    s_state.requested_renderer.reset();
    s_state.requested_fullscreen_ui = false;
    s_state.fullscreen_state = false;
    return false;
  }

  // But the renderer may not have been successfully switched. Keep our CPU thread state in sync.
  if (created_renderer == GPURenderer::Count)
    s_state.requested_renderer.reset();
  else
    s_state.requested_renderer = renderer;
  s_state.requested_fullscreen_ui = new_requested_fullscreen_ui;
  s_state.fullscreen_state = new_fullscreen_state;
  return (result == VideoThreadReconfigureCommand::Result::Success);
}

bool VideoThread::StartFullscreenUI(bool fullscreen, Error* error)
{
  // Don't need to reconfigure if we already have a system.
  if (System::IsValid())
  {
    s_state.requested_fullscreen_ui = true;
    return true;
  }

  return Reconfigure(std::nullopt, false, fullscreen, true, false, error);
}

bool VideoThread::IsFullscreenUIRequested()
{
  return s_state.requested_fullscreen_ui;
}

void VideoThread::StopFullscreenUI()
{
  // shouldn't be changing this while we have a system
  if (System::IsValid())
  {
    s_state.requested_fullscreen_ui = false;
    return;
  }

  Reconfigure(std::nullopt, false, std::nullopt, false, false, nullptr);
}

std::optional<GPURenderer> VideoThread::GetRequestedRenderer()
{
  return s_state.requested_renderer;
}

bool VideoThread::CreateGPUBackend(GPURenderer renderer, bool upload_vram, std::optional<bool> fullscreen, Error* error)
{
  return Reconfigure(renderer, upload_vram, fullscreen, std::nullopt, false, error);
}

void VideoThread::DestroyGPUBackend()
{
  Reconfigure(std::nullopt, false, std::nullopt, std::nullopt, false, nullptr);
}

bool VideoThread::HasGPUBackend()
{
  DebugAssert(IsOnThread());
  return (s_state.gpu_backend != nullptr);
}

bool VideoThread::IsGPUBackendRequested()
{
  return s_state.requested_renderer.has_value();
}

bool VideoThread::IsFullscreen()
{
  return s_state.fullscreen_state;
}

bool VideoThread::CreateDeviceOnThread(RenderAPI api, bool fullscreen, bool start_fullscreen_ui,
                                       bool preserve_imgui_on_failure, Error* error)
{
  DebugAssert(!g_gpu_device);

  INFO_LOG("Trying to create a {} GPU device...", GPUDevice::RenderAPIToString(api));
  g_gpu_device = GPUDevice::CreateDeviceForAPI(api);

  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (fullscreen && g_gpu_device && g_gpu_device->GetFeatures().exclusive_fullscreen)
  {
    fullscreen_mode =
      GPUDevice::ExclusiveFullscreenMode::Parse(Core::GetTinyStringSettingValue("GPU", "FullscreenMode", ""));
  }
  std::optional<bool> exclusive_fullscreen_control;
  if (g_gpu_settings.display_exclusive_fullscreen_control != DisplayExclusiveFullscreenControl::Automatic)
  {
    exclusive_fullscreen_control =
      (g_gpu_settings.display_exclusive_fullscreen_control == DisplayExclusiveFullscreenControl::Allowed);
  }

  GPUDevice::CreateFlags create_flags = GPUDevice::CreateFlags::None;
  if (g_gpu_settings.gpu_prefer_gles_context)
    create_flags |= GPUDevice::CreateFlags::PreferGLESContext;
  if (g_gpu_settings.gpu_use_debug_device)
    create_flags |= GPUDevice::CreateFlags::EnableDebugDevice;
  if (g_gpu_settings.gpu_use_debug_device && g_gpu_settings.gpu_use_debug_device_gpu_validation)
    create_flags |= GPUDevice::CreateFlags::EnableGPUValidation;
  if (g_gpu_settings.gpu_disable_shader_cache)
    create_flags |= GPUDevice::CreateFlags::DisableShaderCache;
  if (g_gpu_settings.gpu_disable_dual_source_blend)
    create_flags |= GPUDevice::CreateFlags::DisableDualSourceBlend;
  if (g_gpu_settings.gpu_disable_framebuffer_fetch)
    create_flags |= GPUDevice::CreateFlags::DisableFramebufferFetch;
  if (g_gpu_settings.gpu_disable_texture_buffers)
    create_flags |= GPUDevice::CreateFlags::DisableTextureBuffers;
  if (g_gpu_settings.gpu_disable_texture_copy_to_self)
    create_flags |= GPUDevice::CreateFlags::DisableTextureCopyToSelf;
  if (g_gpu_settings.gpu_disable_memory_import)
    create_flags |= GPUDevice::CreateFlags::DisableMemoryImport;
  if (g_gpu_settings.gpu_disable_raster_order_views)
    create_flags |= GPUDevice::CreateFlags::DisableRasterOrderViews;
  if (g_gpu_settings.gpu_disable_compute_shaders)
    create_flags |= GPUDevice::CreateFlags::DisableComputeShaders;
  if (g_gpu_settings.gpu_disable_compressed_textures)
    create_flags |= GPUDevice::CreateFlags::DisableCompressedTextures;

  // Only dump shaders on debug builds for Android, users will complain about storage...
#if !defined(__ANDROID__) || defined(_DEBUG)
  const std::string_view shader_dump_directory(EmuFolders::DataRoot);
#else
  const std::string_view shader_dump_directory;
#endif

  Error create_error;
  std::optional<WindowInfo> wi;
  if (!g_gpu_device ||
      !(wi = Host::AcquireRenderWindow(api, fullscreen, fullscreen_mode.has_value(), &create_error)).has_value() ||
      !g_gpu_device->Create(Core::GetStringSettingValue("GPU", "Adapter"), create_flags, shader_dump_directory,
                            EmuFolders::Cache, SHADER_CACHE_VERSION, wi.value(), s_state.requested_vsync,
                            fullscreen_mode.has_value() ? &fullscreen_mode.value() : nullptr,
                            exclusive_fullscreen_control, &create_error))
  {
    ERROR_LOG("Failed to create GPU device: {}", create_error.GetDescription());
    if (g_gpu_device)
      g_gpu_device->Destroy();
    g_gpu_device.reset();
    if (wi.has_value())
      Host::ReleaseRenderWindow();

    Error::SetStringFmt(
      error,
      TRANSLATE_FS("System", "Failed to create render device:\n\n{0}\n\nThis may be due to your GPU not supporting the "
                             "chosen renderer ({1}), or because your graphics drivers need to be updated."),
      create_error.GetDescription(), GPUDevice::RenderAPIToString(api));

    return false;
  }

  // might be initialized already if we're recreating the device
  const bool imgui_was_initialized = ImGuiManager::IsInitialized();
  if (!(imgui_was_initialized ? ImGuiManager::CreateGPUResources(&create_error) :
                                ImGuiManager::Initialize(&create_error)))
  {
    ERROR_LOG("Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    Error::SetStringFmt(error, "Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    if (preserve_imgui_on_failure)
      ImGuiManager::DestroyGPUResources();
    else
      ImGuiManager::Shutdown();

    g_gpu_device->Destroy();
    g_gpu_device.reset();
    if (wi.has_value())
      Host::ReleaseRenderWindow();
    return false;
  }

  // still need to update the window size
  if (imgui_was_initialized && g_gpu_device->HasMainSwapChain())
  {
    const GPUSwapChain* sc = g_gpu_device->GetMainSwapChain();
    ImGuiManager::WindowResized(sc->GetWindowInfo().surface_format, static_cast<float>(sc->GetWidth()),
                                static_cast<float>(sc->GetHeight()));
  }

  if (start_fullscreen_ui)
    FullscreenUI::Initialize();

  if (const GPUSwapChain* swap_chain = g_gpu_device->GetMainSwapChain())
    s_state.render_window_info = swap_chain->GetWindowInfo();
  else
    s_state.render_window_info = WindowInfo();
  s_state.render_window_info.surface_width = std::max<u16>(s_state.render_window_info.surface_width, 1);
  s_state.render_window_info.surface_height = std::max<u16>(s_state.render_window_info.surface_height, 1);

  UpdateRunIdle();

  // Switch to borderless if exclusive failed.
  if (fullscreen_mode.has_value() && !CheckExclusiveFullscreenOnThread())
  {
    WARNING_LOG("Failed to get exclusive fullscreen, requesting borderless fullscreen instead.");
    RecreateRenderWindowOnThread(true, false);
  }

  return true;
}

void VideoThread::DestroyDeviceOnThread(bool preserve_imgui_state)
{
  if (!g_gpu_device)
    return;

  // Presenter should be gone by this point
  Assert(!VideoPresenter::HasDisplayTexture());

  if (preserve_imgui_state)
  {
    DEV_LOG("Preserving ImGui state on destroy");
    ImGuiManager::DestroyGPUResources();
  }
  else
  {
    ImGuiManager::Shutdown();
  }

  INFO_LOG("Destroying {} GPU device...", GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()));
  g_gpu_device->Destroy();
  g_gpu_device.reset();
  Host::ReleaseRenderWindow();

  UpdateRunIdle();
  s_state.render_window_info = WindowInfo();
}

bool VideoThread::CreateGPUBackendOnThread(bool hardware_renderer, bool upload_vram, const GPUSettings* old_settings,
                                           GPURenderer* out_renderer, Error* error)
{
  Error local_error;

  // Create presenter if we don't already have one.
  if (!VideoPresenter::IsInitialized())
  {
    if (!VideoPresenter::Initialize(&local_error))
    {
      ERROR_LOG("Failed to create presenter: {}", local_error.GetDescription());
      Error::SetStringFmt(error, "Failed to create presenter: {}", local_error.GetDescription());
      return false;
    }
  }
  else if (old_settings)
  {
    // Settings may have still changed.
    if (!VideoPresenter::UpdateSettings(*old_settings, &local_error))
    {
      ERROR_LOG("Failed to update presenter settings: {}", local_error.GetDescription());
      Error::SetStringFmt(error, "Failed to update presenter settings: {}", local_error.GetDescription());
      VideoPresenter::Shutdown();
      return false;
    }
  }

#ifndef __ANDROID__
  ImGuiManager::UpdateDebugWindowConfig();
#endif

  if (hardware_renderer)
    s_state.gpu_backend = GPUBackend::CreateHardwareBackend();
  else
    s_state.gpu_backend = GPUBackend::CreateSoftwareBackend();

  bool okay = s_state.gpu_backend->Initialize(upload_vram, &local_error);
  if (!okay)
  {
    ERROR_LOG("Failed to create {} renderer: {}", hardware_renderer ? "hardware" : "software",
              local_error.GetDescription());

    if (hardware_renderer && !System::IsStartupCancelled())
    {
      Host::AddIconOSDMessage(
        OSDMessageType::Error, "GPUBackendCreationFailed", ICON_FA_PAINT_ROLLER,
        fmt::format(
          "{}\n{}",
          TRANSLATE_SV("OSDMessage", "Failed to initialize hardware renderer, falling back to software renderer."),
          local_error.GetDescription()));

      hardware_renderer = false;
      s_state.gpu_backend = GPUBackend::CreateSoftwareBackend();
      if (!s_state.gpu_backend->Initialize(upload_vram, &local_error))
        Panic("Failed to initialize fallback software renderer");
    }

    if (!okay)
    {
      if (error)
        *error = local_error;
      return false;
    }
  }

  *out_renderer =
    hardware_renderer ? Settings::GetRendererForRenderAPI(g_gpu_device->GetRenderAPI()) : GPURenderer::Software;

  g_gpu_device->SetGPUTimingEnabled(g_gpu_settings.display_show_gpu_usage);
  s_state.gpu_backend->RestoreDeviceContext();
  SetRunIdleReason(RunIdleReason::NoGPUBackend, false);
  return true;
}

void VideoThread::ReconfigureOnThread(VideoThreadReconfigureCommand* cmd)
{
  // Store state.
  s_state.requested_vsync = cmd->vsync_mode;
  VideoPresenter::SetPresentSkipMode(cmd->present_skip_mode);

  // Are we shutting down everything?
  if (!cmd->renderer.has_value() && !cmd->start_fullscreen_ui)
  {
    // Serial clear must be after backend destroy, otherwise textures won't dump.
    DestroyGPUBackendOnThread();
    DestroyGPUPresenterOnThread();
    DestroyDeviceOnThread(false);
    ClearGameInfoOnThread();
    *cmd->out_result = VideoThreadReconfigureCommand::Result::Success;
    *cmd->out_created_renderer = GPURenderer::Count;
    return;
  }

  const GPUSettings old_settings = std::move(g_gpu_settings);
  g_gpu_settings = std::move(cmd->settings);

  // Readback old VRAM for hardware renderers.
  const bool had_renderer = static_cast<bool>(s_state.gpu_backend);
  if (had_renderer && cmd->renderer.has_value() && cmd->upload_vram)
  {
    GPUBackendReadVRAMCommand read_cmd;
    read_cmd.type = VideoThreadCommandType::ReadVRAM;
    read_cmd.size = sizeof(cmd);
    read_cmd.x = 0;
    read_cmd.y = 0;
    read_cmd.width = VRAM_WIDTH;
    read_cmd.height = VRAM_HEIGHT;
    s_state.gpu_backend->HandleCommand(&read_cmd);
  }

  if (s_state.gpu_backend)
    DestroyGPUBackendOnThread();

  // Device recreation?
  const RenderAPI current_api = g_gpu_device ? g_gpu_device->GetRenderAPI() : RenderAPI::None;
  const RenderAPI expected_api =
    (!cmd->force_recreate_device && current_api != RenderAPI::None &&
     (!cmd->renderer.has_value() || cmd->renderer.value() == GPURenderer::Software)) ?
      current_api :
      Settings::GetRenderAPIForRenderer(cmd->renderer.value_or(g_gpu_settings.gpu_renderer));
  if (cmd->force_recreate_device || !GPUDevice::IsSameRenderAPI(current_api, expected_api))
  {
    Timer timer;
    DestroyGPUPresenterOnThread();
    DestroyDeviceOnThread(true);

    Error local_error;
    if (!CreateDeviceOnThread(expected_api, cmd->fullscreen, cmd->start_fullscreen_ui, current_api != RenderAPI::None,
                              &local_error))
    {
      Host::AddIconOSDMessage(
        OSDMessageType::Error, "DeviceSwitchFailed", ICON_FA_PAINT_ROLLER,
        fmt::format(TRANSLATE_FS("OSDMessage", "Failed to create {} GPU device, reverting to {}.\n{}"),
                    GPUDevice::RenderAPIToString(expected_api), GPUDevice::RenderAPIToString(current_api),
                    local_error.GetDescription()));

      if (current_api == RenderAPI::None ||
          !CreateDeviceOnThread(current_api, cmd->fullscreen, cmd->start_fullscreen_ui, false, &local_error))
      {
        if (cmd->error_ptr)
          *cmd->error_ptr = local_error;

        *cmd->out_result = VideoThreadReconfigureCommand::Result::FailedWithDeviceLoss;
        *cmd->out_created_renderer = GPURenderer::Count;
        return;
      }
    }

    INFO_LOG("GPU device created in {:.2f}ms", timer.GetTimeMilliseconds());
  }

  if (cmd->renderer.has_value())
  {
    Timer timer;

    // Do we want a renderer?
    if (CreateGPUBackendOnThread(cmd->renderer.value() != GPURenderer::Software, cmd->upload_vram, &old_settings,
                                 cmd->out_created_renderer, cmd->error_ptr))
    {
      *cmd->out_result = VideoThreadReconfigureCommand::Result::Success;
      INFO_LOG("GPU backend created in {:.2f}ms", timer.GetTimeMilliseconds());
    }
    else
    {
      // No renderer created.
      *cmd->out_result = VideoThreadReconfigureCommand::Result::Failed;
      *cmd->out_created_renderer = GPURenderer::Count;

      // If we had a renderer, it means it was a switch, and we need to bail out the thread.
      if (had_renderer)
      {
        VideoThread::ReportFatalErrorAndShutdown("Failed to switch GPU backend.");
      }
      else
      {
        // No point keeping the presenter around.
        DestroyGPUPresenterOnThread();
        ClearGameInfoOnThread();

        // Drop device if we're not running FSUI.
        if (!cmd->start_fullscreen_ui)
          DestroyDeviceOnThread(false);
      }
    }
  }
  else
  {
    // Full shutdown case handled above. This is just for running FSUI.
    DebugAssert(cmd->start_fullscreen_ui);
    DebugAssert(g_gpu_device);

    // Don't need to present game frames anymore.
    DestroyGPUPresenterOnThread();
    ClearGameInfoOnThread();

    *cmd->out_result = VideoThreadReconfigureCommand::Result::Success;
  }
}

void VideoThread::DestroyGPUBackendOnThread()
{
  if (!s_state.gpu_backend)
    return;

  VERBOSE_LOG("Shutting down GPU backend...");

  SetRunIdleReason(RunIdleReason::NoGPUBackend, true);

  s_state.gpu_backend.reset();
}

void VideoThread::DestroyGPUPresenterOnThread()
{
  if (!VideoPresenter::IsInitialized())
    return;

  VERBOSE_LOG("Shutting down GPU presenter...");

  ImGuiManager::DestroyAllDebugWindows();
  ImGuiManager::DestroyOverlayTextures();

  // Should have no queued frames by this point. Backend can get replaced with null.
  Assert(!s_state.gpu_backend);
  Assert(GPUBackend::GetQueuedFrameCount() == 0);

  // Don't need timing anymore.
  g_gpu_device->SetGPUTimingEnabled(false);

  VideoPresenter::Shutdown();
}

bool VideoThread::Internal::PresentFrameAndRestoreContext()
{
  DebugAssert(IsOnThread());

  if (s_state.gpu_backend)
    s_state.gpu_backend->FlushRender();

  if (g_gpu_device->HasMainSwapChain())
  {
    if (!VideoPresenter::PresentFrame(s_state.gpu_backend.get(), 0))
      return false;

    if (s_state.gpu_backend)
      s_state.gpu_backend->RestoreDeviceContext();
  }

  return true;
}

void VideoThread::SetThreadEnabled(bool enabled)
{
  if (s_state.use_thread == enabled)
    return;

  if (s_state.use_thread)
    SyncThread(false);

  // Was anything active?
  if (!g_gpu_device)
  {
    // Thread should be idle. Just reset the FIFO.
    s_state.use_thread = enabled;
    ResetCommandFIFO();
    return;
  }

  const bool fullscreen_state = s_state.fullscreen_state;
  const bool requested_fullscreen_ui = s_state.requested_fullscreen_ui;
  const std::optional<GPURenderer> requested_renderer = s_state.requested_renderer;

  // Force VRAM download, we're recreating.
  if (requested_renderer.has_value())
  {
    GPUBackendReadVRAMCommand* cmd = GPUBackend::NewReadVRAMCommand();
    cmd->x = 0;
    cmd->y = 0;
    cmd->width = VRAM_WIDTH;
    cmd->height = VRAM_HEIGHT;
    PushCommand(cmd);
  }

  // Shutdown reconfigure.
  Reconfigure(std::nullopt, false, false, false, false, nullptr);

  // Thread should be idle at this point. Reset the FIFO.
  ResetCommandFIFO();

  // Update state and reconfigure again.
  s_state.use_thread = enabled;

  Error error;
  if (!Reconfigure(requested_renderer, requested_renderer.has_value(), fullscreen_state, requested_fullscreen_ui, true,
                   &error))
  {
    ERROR_LOG("Reconfigure failed: {}", error.GetDescription());
    ReportFatalErrorAndShutdown(fmt::format("Reconfigure failed: {}", error.GetDescription()));
  }
}

void VideoThread::UpdateSettingsOnThread(GPUSettings&& new_settings)
{
  VERBOSE_LOG("Updating GPU settings on thread...");

  GPUSettings old_settings = std::move(g_gpu_settings);
  g_gpu_settings = std::move(new_settings);

  if (g_gpu_device)
  {
    if (g_gpu_settings.display_osd_scale != old_settings.display_osd_scale)
      ImGuiManager::RequestScaleUpdate();

    FullscreenUI::CheckForConfigChanges(old_settings);
  }

  if (s_state.gpu_backend)
  {
    if (g_gpu_settings.display_show_gpu_usage != old_settings.display_show_gpu_usage)
      g_gpu_device->SetGPUTimingEnabled(g_gpu_settings.display_show_gpu_usage);

    Error error;
    if (!VideoPresenter::UpdateSettings(old_settings, &error) ||
        !s_state.gpu_backend->UpdateSettings(old_settings, &error)) [[unlikely]]
    {
      ReportFatalErrorAndShutdown(fmt::format("Failed to update settings: {}", error.GetDescription()));
      return;
    }

    if (ImGuiManager::UpdateDebugWindowConfig())
      Internal::PresentFrameAndRestoreContext();
    else
      s_state.gpu_backend->RestoreDeviceContext();
  }
}

void VideoThread::RunOnThread(AsyncCallType func)
{
  if (!s_state.use_thread) [[unlikely]]
  {
    func();
    return;
  }

  VideoThreadAsyncCallCommand* cmd =
    AllocateCommand<VideoThreadAsyncCallCommand>(VideoThreadCommandType::AsyncCall, std::move(func));
  PushCommandAndWakeThread(cmd);
}

void VideoThread::RunOnBackend(AsyncBackendCallType func, bool sync, bool spin_or_wake)
{
  if (!s_state.use_thread) [[unlikely]]
  {
    func(s_state.gpu_backend.get());
    return;
  }

  VideoThreadAsyncBackendCallCommand* cmd =
    AllocateCommand<VideoThreadAsyncBackendCallCommand>(VideoThreadCommandType::AsyncBackendCall, std::move(func));
  if (sync)
    PushCommandAndSync(cmd, spin_or_wake);
  else if (spin_or_wake)
    PushCommandAndWakeThread(cmd);
  else
    PushCommand(cmd);
}

std::pair<VideoThreadCommand*, void*> VideoThread::BeginASyncBufferCall(AsyncBufferCallType func, u32 buffer_size)
{
  // this is less than optimal, but it's only used for input osd updates currently, so whatever
  VideoThreadAsyncCallCommand* const cmd = AllocateCommand<VideoThreadAsyncCallCommand>(
    sizeof(VideoThreadAsyncCallCommand) + buffer_size, VideoThreadCommandType::AsyncCall);
  void* const buffer = static_cast<void*>(cmd + 1);
  cmd->func = [func, buffer]() { func(buffer); };
  return std::make_pair(static_cast<VideoThreadCommand*>(cmd), buffer);
}

void VideoThread::EndASyncBufferCall(VideoThreadCommand* cmd)
{
  if (!s_state.use_thread) [[unlikely]]
  {
    VideoThreadAsyncCallCommand* const acmd = static_cast<VideoThreadAsyncCallCommand*>(cmd);
    acmd->func();
    acmd->~VideoThreadAsyncCallCommand();
    return;
  }

  PushCommand(cmd);
}

void VideoThread::UpdateSettings(bool gpu_settings_changed, bool device_settings_changed, bool thread_changed)
{
  // thread should be a device setting
  if (thread_changed)
  {
    DebugAssert(device_settings_changed);
    SetThreadEnabled(g_settings.gpu_use_thread);
  }
  else if (device_settings_changed)
  {
    INFO_LOG("Reconfiguring after device settings changed.");

    Error error;
    if (!Reconfigure(s_state.requested_renderer, s_state.requested_renderer.has_value(), std::nullopt, std::nullopt,
                     true, &error)) [[unlikely]]
    {
      Host::ReportErrorAsync("Error", fmt::format("Failed to recreate GPU device: {}", error.GetDescription()));
    }
  }
  else if (gpu_settings_changed)
  {
    if (s_state.use_thread) [[likely]]
    {
      VideoThreadUpdateSettingsCommand* cmd =
        AllocateCommand<VideoThreadUpdateSettingsCommand>(VideoThreadCommandType::UpdateSettings, g_settings);
      PushCommandAndWakeThread(cmd);
    }
    else
    {
      UpdateSettingsOnThread(GPUSettings(g_settings));
    }
  }
  else
  {
#ifndef __ANDROID__
    // Not needed on Android, debug windows are not used.
    RunOnThread([]() {
      if (s_state.gpu_backend)
      {
        if (ImGuiManager::UpdateDebugWindowConfig())
          Internal::PresentFrameAndRestoreContext();
      }
    });
#endif
  }
}

void VideoThread::UpdateGameInfo(const std::string& title, const std::string& serial, const std::string& path,
                                 GameHash hash, bool wake_thread /*= true*/)
{
  if (!s_state.use_thread)
  {
    const bool serial_changed = (s_state.game_serial != serial);
    s_state.game_title = title;
    s_state.game_serial = serial;
    s_state.game_path = path;
    s_state.game_hash = hash;
    GameInfoChanged(serial_changed);
    return;
  }

  VideoThreadUpdateGameInfoCommand* cmd = AllocateCommand<VideoThreadUpdateGameInfoCommand>(
    VideoThreadCommandType::UpdateGameInfo, title, serial, path, hash);

  if (wake_thread)
    PushCommandAndWakeThread(cmd);
  else
    PushCommand(cmd);
}

void VideoThread::ClearGameInfo()
{
  if (!s_state.use_thread)
  {
    ClearGameInfoOnThread();
    return;
  }

  PushCommandAndWakeThread(AllocateCommand<VideoThreadUpdateGameInfoCommand>(VideoThreadCommandType::UpdateGameInfo));
}

void VideoThread::UpdateGameInfoOnThread(VideoThreadUpdateGameInfoCommand* cmd)
{
  DEV_LOG("Updating game info on GPU thread: {}/{}", cmd->game_serial, cmd->game_title);
  const bool serial_changed = (s_state.game_serial != cmd->game_serial);
  s_state.game_title = std::move(cmd->game_title);
  s_state.game_serial = std::move(cmd->game_serial);
  s_state.game_path = std::move(cmd->game_path);
  s_state.game_hash = cmd->game_hash;
  GameInfoChanged(serial_changed);
}

void VideoThread::GameInfoChanged(bool serial_changed)
{
  if (!serial_changed)
    return;

  if (HasGPUBackend())
    GPUTextureCache::GameSerialChanged();
  if (SaveStateSelectorUI::IsOpen())
    SaveStateSelectorUI::RefreshList();
}

void VideoThread::ClearGameInfoOnThread()
{
  DEV_LOG("Clearing game info on GPU thread.");
  s_state.game_hash = 0;
  s_state.game_path = {};
  s_state.game_serial = {};
  s_state.game_title = {};
}

void VideoThread::ReportFatalErrorAndShutdown(std::string_view reason)
{
  DebugAssert(IsOnThread());

  std::string message = fmt::format("GPU thread shut down with fatal error:\n\n{}", reason);
  Host::RunOnCoreThread([message = std::move(message)]() { System::AbnormalShutdown(message); });

  // replace the renderer with a dummy/null backend, so that all commands get dropped
  ERROR_LOG("Switching to null renderer: {}", reason);
  VideoPresenter::ClearDisplayTexture();
  s_state.gpu_backend.reset();
  s_state.gpu_backend = GPUBackend::CreateNullBackend();
  if (!s_state.gpu_backend->Initialize(false, nullptr)) [[unlikely]]
    Panic("Failed to initialize null GPU backend");
}

bool VideoThread::IsOnThread()
{
  return (!s_state.use_thread || s_state.thread_handle.IsCallingThread());
}

bool VideoThread::IsUsingThread()
{
  return s_state.use_thread;
}

void VideoThread::ResizeRenderWindow(s32 width, s32 height, float scale, float refresh_rate)
{
  const u16 clamped_width = static_cast<u16>(std::clamp<s32>(width, 1, std::numeric_limits<u16>::max()));
  const u16 clamped_height = static_cast<u16>(std::clamp<s32>(height, 1, std::numeric_limits<u16>::max()));
  const bool size_changed = (s_state.render_window_info.surface_width != clamped_width ||
                             s_state.render_window_info.surface_height != clamped_height);
  const bool refresh_rate_changed = (s_state.render_window_info.surface_refresh_rate != refresh_rate);

  s_state.render_window_info.surface_width = clamped_width;
  s_state.render_window_info.surface_height = clamped_height;
  s_state.render_window_info.surface_scale = scale;
  s_state.render_window_info.surface_refresh_rate = refresh_rate;

  RunOnThread(
    [width, height, scale, refresh_rate]() { ResizeRenderWindowOnThread(width, height, scale, refresh_rate); });

  if (System::IsValid())
  {
    if (size_changed)
      System::RenderWindowResized();
    if (refresh_rate_changed)
      System::UpdateSpeedLimiterState();
  }
}

void VideoThread::ResizeRenderWindowOnThread(u32 width, u32 height, float scale, float refresh_rate)
{
  // We should _not_ be getting this without a device, since we should have shut down.
  if (!g_gpu_device || !g_gpu_device->HasMainSwapChain())
    return;

  DEV_LOG("Render window resized to {}x{} @ {}x/{}hz", width, height, scale, refresh_rate);

  Error error;
  GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();
  if (!swap_chain->ResizeBuffers(width, height, &error))
  {
    // ick, CPU thread read, but this is unlikely to happen in the first place
    ERROR_LOG("Failed to resize main swap chain: {}", error.GetDescription());
    RecreateRenderWindowOnThread(s_state.fullscreen_state, true);
    return;
  }

  swap_chain->SetScale(scale);
  swap_chain->SetRefreshRate(refresh_rate);

  RenderWindowResizedOnThread();
}

void VideoThread::RecreateRenderWindow()
{
  RunOnThread([fullscreen = s_state.fullscreen_state]() { RecreateRenderWindowOnThread(fullscreen, true); });
}

void VideoThread::SetFullscreen(bool fullscreen)
{
  // Technically not safe to read g_gpu_device here on the CPU thread, but we do sync on create/destroy.
  if (s_state.fullscreen_state == fullscreen || !Host::CanChangeFullscreenMode(fullscreen) || !g_gpu_device)
    return;

  s_state.fullscreen_state = fullscreen;
  RunOnThread([fullscreen]() { RecreateRenderWindowOnThread(fullscreen, true); });
}

void VideoThread::SetFullscreenWithCompletionHandler(bool fullscreen, AsyncCallType completion_handler)
{
  if (s_state.fullscreen_state == fullscreen || !Host::CanChangeFullscreenMode(fullscreen) || !g_gpu_device)
  {
    if (completion_handler)
      completion_handler();

    return;
  }

  s_state.fullscreen_state = fullscreen;
  RunOnThread([fullscreen, completion_handler = std::move(completion_handler)]() {
    RecreateRenderWindowOnThread(fullscreen, true);
    if (completion_handler)
      completion_handler();
  });
}

void VideoThread::RecreateRenderWindowOnThread(bool fullscreen, bool allow_exclusive_fullscreen)
{
  // In case we get the event late.
  if (!g_gpu_device)
    return;

  bool exclusive_fullscreen_requested = false;
  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (allow_exclusive_fullscreen && fullscreen && g_gpu_device->GetFeatures().exclusive_fullscreen)
  {
    fullscreen_mode =
      GPUDevice::ExclusiveFullscreenMode::Parse(Core::GetTinyStringSettingValue("GPU", "FullscreenMode", ""));
    exclusive_fullscreen_requested = fullscreen_mode.has_value();
  }
  std::optional<bool> exclusive_fullscreen_control;
  if (g_settings.display_exclusive_fullscreen_control != DisplayExclusiveFullscreenControl::Automatic)
  {
    exclusive_fullscreen_control =
      (g_settings.display_exclusive_fullscreen_control == DisplayExclusiveFullscreenControl::Allowed);
  }

  g_gpu_device->DestroyMainSwapChain();

  Error error;
  std::optional<WindowInfo> wi =
    Host::AcquireRenderWindow(g_gpu_device->GetRenderAPI(), fullscreen, exclusive_fullscreen_requested, &error);
  if (!wi.has_value())
  {
    Host::ReportFatalError("Failed to get render window after update", error.GetDescription());
    return;
  }

  // if surfaceless, just leave it
  if (!wi->IsSurfaceless())
  {
    if (!g_gpu_device->RecreateMainSwapChain(wi.value(), s_state.requested_vsync,
                                             fullscreen_mode.has_value() ? &fullscreen_mode.value() : nullptr,
                                             exclusive_fullscreen_control, &error))
    {
      Host::ReportFatalError("Failed to change window after update", error.GetDescription());
      return;
    }
  }
  else
  {
    WARNING_LOG("Switching to surfaceless rendering");
    if (!g_gpu_device->SwitchToSurfacelessRendering(&error))
      ERROR_LOG("Failed to switch to surfaceless, rendering commands may fail: {}", error.GetDescription());
  }

  // If exclusive fullscreen failed, switch to borderless fullscreen.
  if (exclusive_fullscreen_requested && !CheckExclusiveFullscreenOnThread())
  {
    RecreateRenderWindowOnThread(true, false);
    return;
  }

  // Need to notify the core thread of the change, since it won't necessarily get a resize event.
  Host::RunOnCoreThread([wi = g_gpu_device->HasMainSwapChain() ?
                                g_gpu_device->GetMainSwapChain()->GetWindowInfo() :
                                WindowInfo()]() { s_state.render_window_info = std::move(wi); });

  RenderWindowResizedOnThread();
}

bool VideoThread::CheckExclusiveFullscreenOnThread()
{
  if (g_gpu_device->HasMainSwapChain() && g_gpu_device->GetMainSwapChain()->IsExclusiveFullscreen())
    return true;

  Host::AddIconOSDMessage(
    OSDMessageType::Error, "ExclusiveFullscreenFailed", ICON_EMOJI_WARNING,
    TRANSLATE_STR("OSDMessage", "Failed to switch to exclusive fullscreen, using borderless instead."));
  return false;
}

void VideoThread::RenderWindowResizedOnThread()
{
  // surfaceless is usually temporary, so just ignore it
  const GPUSwapChain* const swap_chain = g_gpu_device->GetMainSwapChain();
  if (!swap_chain)
    return;

  // our imgui stuff can't cope with 0x0/hidden windows
  const float f_width = static_cast<float>(std::max(swap_chain->GetWidth(), 1u));
  const float f_height = static_cast<float>(std::max(swap_chain->GetHeight(), 1u));
  ImGuiManager::WindowResized(swap_chain->GetFormat(), f_width, f_height);

  // If we're paused, re-present the current frame at the new window size.
  if (s_state.gpu_backend && IsSystemPaused())
  {
    // Hackity hack, on some systems, presenting a single frame isn't enough to actually get it
    // displayed. Two seems to be good enough. Maybe something to do with direct scanout.
    Internal::PresentFrameAndRestoreContext();
    Internal::PresentFrameAndRestoreContext();
  }
}

const WindowInfo& VideoThread::GetRenderWindowInfo()
{
  return s_state.render_window_info;
}

void VideoThread::SetVSync(GPUVSyncMode mode, PresentSkipMode present_skip_mode)
{
  RunOnThread([mode, present_skip_mode]() {
    VideoPresenter::SetPresentSkipMode(present_skip_mode);
    if (s_state.requested_vsync == mode)
      return;

    s_state.requested_vsync = mode;

    if (!g_gpu_device || !g_gpu_device->HasMainSwapChain())
      return;

    Error error;
    if (!g_gpu_device->GetMainSwapChain()->SetVSyncMode(s_state.requested_vsync, &error))
      ERROR_LOG("Failed to update vsync mode: {}", error.GetDescription());
  });

  // If we're turning on vsync or turning off present throttle, we want to drain the GPU thread.
  // Otherwise if it is currently behind, it'll be permanently stuck behind.
  if (mode != GPUVSyncMode::Disabled)
    SyncThread(false);
}

void VideoThread::PresentCurrentFrame()
{
  RunOnThread([]() {
    if (s_state.run_idle_flag)
    {
      // If we're running idle, we're going to re-present anyway.
      return;
    }

    // But we shouldn't be not running idle without a GPU backend.
    if (s_state.gpu_backend)
      Internal::PresentFrameAndRestoreContext();
  });
}

bool VideoThread::GetRunIdleReason(RunIdleReason reason)
{
  return (s_state.run_idle_reasons & static_cast<u8>(reason)) != 0;
}

void VideoThread::SetRunIdleReason(RunIdleReason reason, bool enabled)
{
  const u8 bit = static_cast<u8>(reason);
  if (((s_state.run_idle_reasons & bit) != 0) == enabled)
    return;

  if (Log::IsLogVisible(Log::Level::Dev, Log::Channel::VideoThread))
  {
    static constexpr const std::array reason_strings = {
      "NoGPUBackend",        "SystemPaused",        "OSDMessagesActive",         "FullscreenUIActive",
      "NotificationsActive", "LoadingScreenActive", "AchievementOverlaysActive",
    };
    for (u32 i = 0; i < reason_strings.size(); ++i)
    {
      if (static_cast<RunIdleReason>(1u << i) == reason)
      {
        DEV_COLOR_LOG(StrongYellow, "Setting run idle reason '{}' to {}", reason_strings[i], enabled);
        break;
      }
    }
  }

  s_state.run_idle_reasons = enabled ? (s_state.run_idle_reasons | bit) : (s_state.run_idle_reasons & ~bit);
  UpdateRunIdle();
}

bool VideoThread::IsRunningIdle()
{
  return s_state.run_idle_flag;
}

bool VideoThread::IsSystemPaused()
{
  return ((s_state.run_idle_reasons & static_cast<u8>(RunIdleReason::SystemPaused)) != 0);
}

void VideoThread::UpdateRunIdle()
{
  DebugAssert(IsOnThread());

  // We require either invalid-system or paused for run idle.
  static constexpr u8 REQUIRE_MASK = static_cast<u8>(RunIdleReason::NoGPUBackend) |
                                     static_cast<u8>(RunIdleReason::SystemPaused) |
                                     static_cast<u8>(RunIdleReason::LoadingScreenActive);
  static constexpr u8 ACTIVATE_MASK =
    static_cast<u8>(RunIdleReason::OSDMessagesActive) | static_cast<u8>(RunIdleReason::FullscreenUIActive) |
    static_cast<u8>(RunIdleReason::NotificationsActive) | static_cast<u8>(RunIdleReason::LoadingScreenActive) |
    static_cast<u8>(RunIdleReason::AchievementOverlaysActive);

  const bool new_flag = (g_gpu_device && ((s_state.run_idle_reasons & REQUIRE_MASK) != 0) &&
                         ((s_state.run_idle_reasons & ACTIVATE_MASK) != 0));
  if (s_state.run_idle_flag == new_flag)
    return;

  s_state.run_idle_flag = new_flag;
  if (new_flag)
    DEV_COLOR_LOG(StrongYellow, "GPU thread now running idle");
  else
    DEV_COLOR_LOG(StrongOrange, "GPU thread now NOT running idle");

  Host::OnVideoThreadRunIdleChanged(new_flag);
}

const std::string& VideoThread::GetGameTitle()
{
  DebugAssert(IsOnThread());
  return s_state.game_title;
}

const std::string& VideoThread::GetGameSerial()
{
  DebugAssert(IsOnThread());
  return s_state.game_serial;
}

const std::string& VideoThread::GetGamePath()
{
  DebugAssert(IsOnThread());
  return s_state.game_path;
}

GameHash VideoThread::GetGameHash()
{
  DebugAssert(IsOnThread());
  return s_state.game_hash;
}
