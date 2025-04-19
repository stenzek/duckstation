// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_thread.h"
#include "fullscreen_ui.h"
#include "gpu_backend.h"
#include "gpu_hw_texture_cache.h"
#include "gpu_presenter.h"
#include "gpu_thread_commands.h"
#include "gpu_types.h"
#include "host.h"
#include "imgui_overlays.h"
#include "performance_counters.h"
#include "settings.h"
#include "shader_cache_version.h"
#include "system.h"
#include "system_private.h"

#include "util/gpu_device.h"
#include "util/imgui_fullscreen.h"
#include "util/imgui_manager.h"
#include "util/input_manager.h"
#include "util/postprocessing.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/error.h"
#include "common/log.h"
#include "common/threading.h"
#include "common/timer.h"

#include "IconsEmoji.h"
#include "IconsFontAwesome5.h"
#include "fmt/format.h"
#include "imgui.h"

#include <optional>

LOG_CHANNEL(GPUThread);

namespace GPUThread {
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

static bool Reconfigure(std::string serial, std::optional<GPURenderer> renderer, bool upload_vram,
                        std::optional<bool> fullscreen, std::optional<bool> start_fullscreen_ui, bool recreate_device,
                        Error* error);

// NOTE: Use with care! The handler needs to manually run the destructor.
template<class T, typename... Args>
T* AllocateCommand(u32 size, GPUBackendCommandType type, Args... args);
template<class T, typename... Args>
T* AllocateCommand(GPUBackendCommandType type, Args... args);

static u32 GetPendingCommandSize();
static void ResetCommandFIFO();
static bool IsCommandFIFOEmpty();
static void WakeGPUThread();
static bool SleepGPUThread(bool allow_sleep);

static bool CreateDeviceOnThread(RenderAPI api, bool fullscreen, bool clear_fsui_state_on_failure, Error* error);
static void DestroyDeviceOnThread(bool clear_fsui_state);
static void ResizeDisplayWindowOnThread(u32 width, u32 height, float scale);
static void UpdateDisplayWindowOnThread(bool fullscreen, bool allow_exclusive_fullscreen);
static void DisplayWindowResizedOnThread();
static bool CheckExclusiveFullscreenOnThread();

static void ReconfigureOnThread(GPUThreadReconfigureCommand* cmd);
static bool CreateGPUBackendOnThread(GPURenderer renderer, bool upload_vram, Error* error);
static void DestroyGPUBackendOnThread();
static void DestroyGPUPresenterOnThread();

static void SetThreadEnabled(bool enabled);
static void UpdateSettingsOnThread(GPUSettings&& new_settings);

static void UpdateRunIdle();

namespace {

struct ALIGN_TO_CACHE_LINE State
{
  // Owned by CPU thread.
  ALIGN_TO_CACHE_LINE Timer::Value thread_spin_time = 0;
  Threading::ThreadHandle gpu_thread;
  Common::unique_aligned_ptr<u8[]> command_fifo_data;
  WindowInfo render_window_info;
  std::optional<GPURenderer> requested_renderer; // TODO: Non thread safe accessof this
  bool use_gpu_thread = false;

  // Hot variables between both threads.
  ALIGN_TO_CACHE_LINE std::atomic<u32> command_fifo_write_ptr{0};
  std::atomic<s32> thread_wake_count{0}; // <0 = sleeping, >= 0 = has work
  Threading::KernelSemaphore thread_wake_semaphore;
  Threading::KernelSemaphore thread_is_done_semaphore;

  // Owned by GPU thread.
  ALIGN_TO_CACHE_LINE std::unique_ptr<GPUBackend> gpu_backend;
  ALIGN_TO_CACHE_LINE std::unique_ptr<GPUPresenter> gpu_presenter;
  std::atomic<u32> command_fifo_read_ptr{0};
  u8 run_idle_reasons = 0;
  bool run_idle_flag = false;
  GPUVSyncMode requested_vsync = GPUVSyncMode::Disabled;
  bool requested_allow_present_throttle = false;
  bool requested_fullscreen_ui = false;
  std::string game_serial;
};

} // namespace

static State s_state;

} // namespace GPUThread

const Threading::ThreadHandle& GPUThread::Internal::GetThreadHandle()
{
  return s_state.gpu_thread;
}

void GPUThread::ResetCommandFIFO()
{
  Assert(!s_state.run_idle_flag && s_state.command_fifo_read_ptr.load(std::memory_order_acquire) ==
                                     s_state.command_fifo_write_ptr.load(std::memory_order_relaxed));
  s_state.command_fifo_write_ptr.store(0, std::memory_order_release);
  s_state.command_fifo_read_ptr.store(0, std::memory_order_release);
}

void GPUThread::Internal::ProcessStartup()
{
  s_state.thread_spin_time = Timer::ConvertNanosecondsToValue(THREAD_SPIN_TIME_US * 1000.0);
  s_state.command_fifo_data = Common::make_unique_aligned_for_overwrite<u8[]>(HOST_CACHE_LINE_SIZE, COMMAND_QUEUE_SIZE);
  s_state.use_gpu_thread = g_settings.gpu_use_thread;
  s_state.run_idle_reasons = static_cast<u8>(RunIdleReason::NoGPUBackend);
}

void GPUThread::Internal::RequestShutdown()
{
  INFO_LOG("Shutting down GPU thread...");
  SyncGPUThread(false);

  // Thread must be enabled to shut it down.
  SetThreadEnabled(true);
  PushCommandAndWakeThread(AllocateCommand(GPUBackendCommandType::Shutdown, sizeof(GPUThreadCommand)));
}

GPUThreadCommand* GPUThread::AllocateCommand(GPUBackendCommandType command, u32 size)
{
  size = GPUThreadCommand::AlignCommandSize(size);

  for (;;)
  {
    u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
    u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed);
    if (read_ptr > write_ptr)
    {
      u32 available_size = read_ptr - write_ptr;
      while (available_size < (size + sizeof(GPUBackendCommandType)))
      {
        WakeGPUThread();
        read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
        available_size = (read_ptr > write_ptr) ? (read_ptr - write_ptr) : (COMMAND_QUEUE_SIZE - write_ptr);
      }
    }
    else
    {
      const u32 available_size = COMMAND_QUEUE_SIZE - write_ptr;
      if ((size + sizeof(GPUThreadCommand)) > available_size)
      {
        // allocate a dummy command to wrap the buffer around
        GPUThreadCommand* dummy_cmd = reinterpret_cast<GPUThreadCommand*>(&s_state.command_fifo_data[write_ptr]);
        dummy_cmd->type = GPUBackendCommandType::Wraparound;
        dummy_cmd->size = available_size;
        s_state.command_fifo_write_ptr.store(0, std::memory_order_release);
        continue;
      }
    }

    GPUThreadCommand* cmd = reinterpret_cast<GPUThreadCommand*>(&s_state.command_fifo_data[write_ptr]);
    cmd->type = command;
    cmd->size = size;
    return cmd;
  }
}

template<class T, typename... Args>
T* GPUThread::AllocateCommand(u32 size, GPUBackendCommandType type, Args... args)
{
  const u32 alloc_size = GPUThreadCommand::AlignCommandSize(size);
  GPUThreadCommand* cmd = AllocateCommand(type, alloc_size);
  DebugAssert(cmd->size == alloc_size);

  new (cmd) T(std::forward<Args>(args)...);

  // constructor may overwrite the fields, need to reset them
  cmd->type = type;
  cmd->size = alloc_size;

  return static_cast<T*>(cmd);
}

template<class T, typename... Args>
T* GPUThread::AllocateCommand(GPUBackendCommandType type, Args... args)
{
  return AllocateCommand<T>(sizeof(T), type, std::forward<Args>(args)...);
}

u32 GPUThread::GetPendingCommandSize()
{
  const u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
  const u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed);
  return (write_ptr >= read_ptr) ? (write_ptr - read_ptr) : (COMMAND_QUEUE_SIZE - read_ptr + write_ptr);
}

bool GPUThread::IsCommandFIFOEmpty()
{
  const u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_acquire);
  const u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_relaxed);
  return (read_ptr == write_ptr);
}

void GPUThread::PushCommand(GPUThreadCommand* cmd)
{
  if (!s_state.use_gpu_thread) [[unlikely]]
  {
    DebugAssert(s_state.gpu_backend);
    s_state.gpu_backend->HandleCommand(cmd);
    return;
  }

  const u32 new_write_ptr = s_state.command_fifo_write_ptr.fetch_add(cmd->size, std::memory_order_release) + cmd->size;
  DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
  UNREFERENCED_VARIABLE(new_write_ptr);
  if (GetPendingCommandSize() >= THRESHOLD_TO_WAKE_GPU) // TODO:FIXME: maybe purge this?
    WakeGPUThread();
}

void GPUThread::PushCommandAndWakeThread(GPUThreadCommand* cmd)
{
  if (!s_state.use_gpu_thread) [[unlikely]]
  {
    DebugAssert(s_state.gpu_backend);
    s_state.gpu_backend->HandleCommand(cmd);
    return;
  }

  const u32 new_write_ptr = s_state.command_fifo_write_ptr.fetch_add(cmd->size, std::memory_order_release) + cmd->size;
  DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
  UNREFERENCED_VARIABLE(new_write_ptr);
  WakeGPUThread();
}

void GPUThread::PushCommandAndSync(GPUThreadCommand* cmd, bool spin)
{
  if (!s_state.use_gpu_thread) [[unlikely]]
  {
    DebugAssert(s_state.gpu_backend);
    s_state.gpu_backend->HandleCommand(cmd);
    return;
  }

  const u32 new_write_ptr = s_state.command_fifo_write_ptr.fetch_add(cmd->size, std::memory_order_release) + cmd->size;
  DebugAssert(new_write_ptr <= COMMAND_QUEUE_SIZE);
  UNREFERENCED_VARIABLE(new_write_ptr);
  WakeGPUThread();
  SyncGPUThread(spin);
}

ALWAYS_INLINE s32 GetThreadWakeCount(s32 state)
{
  return (state & ~GPUThread::THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING);
}

void GPUThread::WakeGPUThread()
{
  // If sleeping, state will be <0, otherwise this will increment the pending work count.
  // We add 2 so that there's a positive work count if we were sleeping, otherwise the thread would go to sleep.
  if (s_state.thread_wake_count.fetch_add(2, std::memory_order_release) < 0)
    s_state.thread_wake_semaphore.Post();
}

void GPUThread::SyncGPUThread(bool spin)
{
  if (!s_state.use_gpu_thread)
    return;

  if (spin)
  {
    // Check if the GPU thread is done/sleeping.
    if (GetThreadWakeCount(s_state.thread_wake_count.load(std::memory_order_acquire)) < 0)
    {
      if (IsCommandFIFOEmpty())
        return;

      WakeGPUThread();
    }

    const Timer::Value start_time = Timer::GetCurrentValue();
    Timer::Value current_time = start_time;
    do
    {
      // Check if the GPU thread is done/sleeping.
      if (GetThreadWakeCount(s_state.thread_wake_count.load(std::memory_order_acquire)) < 0)
      {
        if (IsCommandFIFOEmpty())
          return;

        WakeGPUThread();
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
    // Check if the GPU thread is done/sleeping.
    value = s_state.thread_wake_count.load(std::memory_order_acquire);
    if (GetThreadWakeCount(value) < 0)
    {
      if (IsCommandFIFOEmpty())
        return;

      WakeGPUThread();
      continue;
    }
  } while (!s_state.thread_wake_count.compare_exchange_weak(value, value | THREAD_WAKE_COUNT_CPU_THREAD_IS_WAITING,
                                                            std::memory_order_acq_rel, std::memory_order_relaxed));
  s_state.thread_is_done_semaphore.Wait();
}

bool GPUThread::SleepGPUThread(bool allow_sleep)
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

void GPUThread::Internal::GPUThreadEntryPoint()
{
  s_state.gpu_thread = Threading::ThreadHandle::GetForCallingThread();
  std::atomic_thread_fence(std::memory_order_release);

  // Take a local copy of the FIFO, that way it's not ping-ponging between the threads.
  u8* const command_fifo_data = s_state.command_fifo_data.get();

  for (;;)
  {
    u32 write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_acquire);
    u32 read_ptr = s_state.command_fifo_read_ptr.load(std::memory_order_relaxed);
    if (read_ptr == write_ptr)
    {
      if (SleepGPUThread(!s_state.run_idle_flag))
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
      GPUThreadCommand* cmd = reinterpret_cast<GPUThreadCommand*>(&command_fifo_data[read_ptr]);
      DebugAssert((read_ptr + cmd->size) <= COMMAND_QUEUE_SIZE);
      read_ptr += cmd->size;

      if (cmd->type > GPUBackendCommandType::Shutdown) [[likely]]
      {
        DebugAssert(s_state.gpu_backend);
        s_state.gpu_backend->HandleCommand(cmd);
        continue;
      }

      switch (cmd->type)
      {
        case GPUBackendCommandType::Wraparound:
        {
          DebugAssert(read_ptr == COMMAND_QUEUE_SIZE);
          write_ptr = s_state.command_fifo_write_ptr.load(std::memory_order_acquire);
          read_ptr = 0;

          // let the CPU thread know as early as possible that we're here
          s_state.command_fifo_read_ptr.store(read_ptr, std::memory_order_release);
        }
        break;

        case GPUBackendCommandType::AsyncCall:
        {
          GPUThreadAsyncCallCommand* acmd = static_cast<GPUThreadAsyncCallCommand*>(cmd);
          acmd->func();
          acmd->~GPUThreadAsyncCallCommand();
        }
        break;

        case GPUBackendCommandType::AsyncBackendCall:
        {
          GPUThreadAsyncBackendCallCommand* acmd = static_cast<GPUThreadAsyncBackendCallCommand*>(cmd);
          acmd->func(s_state.gpu_backend.get());
          acmd->~GPUThreadAsyncBackendCallCommand();
        }
        break;

        case GPUBackendCommandType::Reconfigure:
        {
          GPUThreadReconfigureCommand* ccmd = static_cast<GPUThreadReconfigureCommand*>(cmd);
          ReconfigureOnThread(ccmd);
          ccmd->~GPUThreadReconfigureCommand();
        }
        break;

        case GPUBackendCommandType::UpdateSettings:
        {
          GPUThreadUpdateSettingsCommand* ccmd = static_cast<GPUThreadUpdateSettingsCommand*>(cmd);
          UpdateSettingsOnThread(std::move(ccmd->settings));
          ccmd->~GPUThreadUpdateSettingsCommand();
        }
        break;

        case GPUBackendCommandType::Shutdown:
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

    s_state.command_fifo_read_ptr.store(read_ptr, std::memory_order_release);
  }

  s_state.gpu_thread = {};
}

void GPUThread::Internal::DoRunIdle()
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
    g_gpu_device->GetMainSwapChain()->ThrottlePresentation();
}

bool GPUThread::Reconfigure(std::string serial, std::optional<GPURenderer> renderer, bool upload_vram,
                            std::optional<bool> fullscreen, std::optional<bool> start_fullscreen_ui,
                            bool recreate_device, Error* error)
{
  INFO_LOG("Reconfiguring GPU thread.");

  bool result = false;
  GPUThreadReconfigureCommand* cmd = AllocateCommand<GPUThreadReconfigureCommand>(GPUBackendCommandType::Reconfigure);
  cmd->game_serial = std::move(serial);
  cmd->renderer = renderer;
  cmd->fullscreen = fullscreen;
  cmd->start_fullscreen_ui = start_fullscreen_ui;
  cmd->vsync_mode = System::GetEffectiveVSyncMode();
  cmd->allow_present_throttle = System::ShouldAllowPresentThrottle();
  cmd->force_recreate_device = recreate_device;
  cmd->upload_vram = upload_vram;
  cmd->error_ptr = error;
  cmd->out_result = &result;
  cmd->settings = g_settings;

  if (!s_state.use_gpu_thread) [[unlikely]]
    ReconfigureOnThread(cmd);
  else
    PushCommandAndSync(cmd, false);

  return result;
}

bool GPUThread::StartFullscreenUI(bool fullscreen, Error* error)
{
  // Don't need to reconfigure if we already have a system.
  if (System::IsValid())
  {
    RunOnThread([]() { s_state.requested_fullscreen_ui = true; });
    return true;
  }

  return Reconfigure(std::string(), std::nullopt, false, fullscreen, true, false, error);
}

bool GPUThread::IsFullscreenUIRequested()
{
  return s_state.requested_fullscreen_ui;
}

void GPUThread::StopFullscreenUI()
{
  // Don't need to reconfigure if we already have a system.
  if (System::IsValid())
  {
    RunOnThread([]() { s_state.requested_fullscreen_ui = false; });
    return;
  }

  Reconfigure(std::string(), std::nullopt, false, std::nullopt, false, false, nullptr);
}

std::optional<GPURenderer> GPUThread::GetRequestedRenderer()
{
  return s_state.requested_renderer;
}

bool GPUThread::CreateGPUBackend(std::string serial, GPURenderer renderer, bool upload_vram, bool fullscreen,
                                 bool force_recreate_device, Error* error)
{
  s_state.requested_renderer = renderer;
  return Reconfigure(std::move(serial), renderer, upload_vram, fullscreen ? std::optional<bool>(true) : std::nullopt,
                     std::nullopt, force_recreate_device, error);
}

void GPUThread::DestroyGPUBackend()
{
  Reconfigure(std::string(), std::nullopt, false, std::nullopt, std::nullopt, false, nullptr);
  s_state.requested_renderer.reset();
}

bool GPUThread::HasGPUBackend()
{
  DebugAssert(IsOnThread());
  return (s_state.gpu_backend != nullptr);
}

bool GPUThread::IsGPUBackendRequested()
{
  return s_state.requested_renderer.has_value();
}

bool GPUThread::CreateDeviceOnThread(RenderAPI api, bool fullscreen, bool clear_fsui_state_on_failure, Error* error)
{
  DebugAssert(!g_gpu_device);

  INFO_LOG("Trying to create a {} GPU device...", GPUDevice::RenderAPIToString(api));
  g_gpu_device = GPUDevice::CreateDeviceForAPI(api);

  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (fullscreen && g_gpu_device && g_gpu_device->GetFeatures().exclusive_fullscreen)
  {
    fullscreen_mode =
      GPUDevice::ExclusiveFullscreenMode::Parse(Host::GetTinyStringSettingValue("GPU", "FullscreenMode", ""));
  }
  std::optional<bool> exclusive_fullscreen_control;
  if (g_gpu_settings.display_exclusive_fullscreen_control != DisplayExclusiveFullscreenControl::Automatic)
  {
    exclusive_fullscreen_control =
      (g_gpu_settings.display_exclusive_fullscreen_control == DisplayExclusiveFullscreenControl::Allowed);
  }

  u32 disabled_features = 0;
  if (g_gpu_settings.gpu_disable_dual_source_blend)
    disabled_features |= GPUDevice::FEATURE_MASK_DUAL_SOURCE_BLEND;
  if (g_gpu_settings.gpu_disable_framebuffer_fetch)
    disabled_features |= GPUDevice::FEATURE_MASK_FRAMEBUFFER_FETCH;
  if (g_gpu_settings.gpu_disable_texture_buffers)
    disabled_features |= GPUDevice::FEATURE_MASK_TEXTURE_BUFFERS;
  if (g_gpu_settings.gpu_disable_memory_import)
    disabled_features |= GPUDevice::FEATURE_MASK_MEMORY_IMPORT;
  if (g_gpu_settings.gpu_disable_raster_order_views)
    disabled_features |= GPUDevice::FEATURE_MASK_RASTER_ORDER_VIEWS;
  if (g_gpu_settings.gpu_disable_compute_shaders)
    disabled_features |= GPUDevice::FEATURE_MASK_COMPUTE_SHADERS;
  if (g_gpu_settings.gpu_disable_compressed_textures)
    disabled_features |= GPUDevice::FEATURE_MASK_COMPRESSED_TEXTURES;

  // Don't dump shaders on debug builds for Android, users will complain about storage...
#if !defined(__ANDROID__) || defined(_DEBUG)
  const std::string_view shader_dump_directory(EmuFolders::DataRoot);
#else
  const std::string_view shader_dump_directory;
#endif

  Error create_error;
  std::optional<WindowInfo> wi;
  if (!g_gpu_device ||
      !(wi = Host::AcquireRenderWindow(api, fullscreen, fullscreen_mode.has_value(), &create_error)).has_value() ||
      !g_gpu_device->Create(
        Host::GetStringSettingValue("GPU", "Adapter"), static_cast<GPUDevice::FeatureMask>(disabled_features),
        shader_dump_directory,
        g_gpu_settings.gpu_disable_shader_cache ? std::string_view() : std::string_view(EmuFolders::Cache),
        SHADER_CACHE_VERSION, g_gpu_settings.gpu_use_debug_device, g_gpu_settings.gpu_use_debug_device_gpu_validation,
        wi.value(), s_state.requested_vsync, s_state.requested_allow_present_throttle,
        fullscreen_mode.has_value() ? &fullscreen_mode.value() : nullptr, exclusive_fullscreen_control, &create_error))
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

  if (!ImGuiManager::Initialize(g_gpu_settings.display_osd_scale / 100.0f, g_gpu_settings.display_osd_margin,
                                &create_error) ||
      (s_state.requested_fullscreen_ui && !FullscreenUI::Initialize()))
  {
    ERROR_LOG("Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    Error::SetStringFmt(error, "Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    FullscreenUI::Shutdown(clear_fsui_state_on_failure);
    ImGuiManager::Shutdown();
    g_gpu_device->Destroy();
    g_gpu_device.reset();
    if (wi.has_value())
      Host::ReleaseRenderWindow();
    return false;
  }

  InputManager::SetDisplayWindowSize(ImGuiManager::GetWindowWidth(), ImGuiManager::GetWindowHeight());

  if (const GPUSwapChain* swap_chain = g_gpu_device->GetMainSwapChain())
    s_state.render_window_info = swap_chain->GetWindowInfo();
  else
    s_state.render_window_info = WindowInfo();

  std::atomic_thread_fence(std::memory_order_release);
  UpdateRunIdle();

  // Switch to borderless if exclusive failed.
  if (fullscreen_mode.has_value() && !CheckExclusiveFullscreenOnThread())
    UpdateDisplayWindowOnThread(true, false);

  return true;
}

void GPUThread::DestroyDeviceOnThread(bool clear_fsui_state)
{
  if (!g_gpu_device)
    return;

  // Presenter should be gone by this point
  Assert(!s_state.gpu_presenter);

  const bool has_window = g_gpu_device->HasMainSwapChain();

  FullscreenUI::Shutdown(clear_fsui_state);
  ImGuiManager::Shutdown();

  INFO_LOG("Destroying {} GPU device...", GPUDevice::RenderAPIToString(g_gpu_device->GetRenderAPI()));
  g_gpu_device->Destroy();
  g_gpu_device.reset();
  if (has_window)
    Host::ReleaseRenderWindow();

  UpdateRunIdle();
  s_state.render_window_info = WindowInfo();
  std::atomic_thread_fence(std::memory_order_release);
}

bool GPUThread::CreateGPUBackendOnThread(GPURenderer renderer, bool upload_vram, Error* error)
{
  Error local_error;

  // Create presenter if we don't already have one.
  if (!s_state.gpu_presenter)
  {
    s_state.gpu_presenter = std::make_unique<GPUPresenter>();
    if (!s_state.gpu_presenter->Initialize(&local_error))
    {
      ERROR_LOG("Failed to create presenter: {}", local_error.GetDescription());
      Error::SetStringFmt(error, "Failed to create presenter: {}", local_error.GetDescription());
      s_state.gpu_presenter.reset();
      return false;
    }

    ImGuiManager::UpdateDebugWindowConfig();
  }

  const bool is_hardware = (renderer != GPURenderer::Software);

  if (is_hardware)
    s_state.gpu_backend = GPUBackend::CreateHardwareBackend(*s_state.gpu_presenter);
  else
    s_state.gpu_backend = GPUBackend::CreateSoftwareBackend(*s_state.gpu_presenter);

  bool okay = s_state.gpu_backend->Initialize(upload_vram, &local_error);
  if (!okay)
  {
    ERROR_LOG("Failed to create {} renderer: {}", Settings::GetRendererName(renderer), local_error.GetDescription());

    if (is_hardware && !System::IsStartupCancelled())
    {
      Host::AddIconOSDMessage(
        "GPUBackendCreationFailed", ICON_FA_PAINT_ROLLER,
        fmt::format(TRANSLATE_FS("OSDMessage", "Failed to initialize {} renderer, falling back to software renderer."),
                    Settings::GetRendererName(s_state.requested_renderer.value())),
        Host::OSD_CRITICAL_ERROR_DURATION);

      s_state.requested_renderer = GPURenderer::Software;
      s_state.gpu_backend = GPUBackend::CreateSoftwareBackend(*s_state.gpu_presenter);
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

  g_gpu_device->SetGPUTimingEnabled(g_gpu_settings.display_show_gpu_usage);
  s_state.gpu_backend->RestoreDeviceContext();
  SetRunIdleReason(RunIdleReason::NoGPUBackend, false);
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

void GPUThread::ReconfigureOnThread(GPUThreadReconfigureCommand* cmd)
{
  // Store state.
  s_state.requested_vsync = cmd->vsync_mode;
  s_state.requested_allow_present_throttle = cmd->allow_present_throttle;
  s_state.requested_fullscreen_ui = cmd->start_fullscreen_ui.value_or(s_state.requested_fullscreen_ui);

  // Are we shutting down everything?
  if (!cmd->renderer.has_value() && !s_state.requested_fullscreen_ui)
  {
    DestroyGPUBackendOnThread();
    DestroyGPUPresenterOnThread();
    DestroyDeviceOnThread(true);
    s_state.game_serial = {};
    return;
  }

  // Serial clear must be after backend destroy, otherwise textures won't dump.
  s_state.game_serial = std::move(cmd->game_serial);
  g_gpu_settings = std::move(cmd->settings);

  // Readback old VRAM for hardware renderers.
  const bool had_renderer = static_cast<bool>(s_state.gpu_backend);
  if (had_renderer && cmd->renderer.has_value() && cmd->upload_vram)
  {
    GPUBackendReadVRAMCommand read_cmd;
    read_cmd.type = GPUBackendCommandType::ReadVRAM;
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
    (cmd->renderer.has_value() && cmd->renderer.value() == GPURenderer::Software && current_api != RenderAPI::None) ?
      current_api :
      Settings::GetRenderAPIForRenderer(s_state.requested_renderer.value_or(g_gpu_settings.gpu_renderer));
  if (cmd->force_recreate_device || !GPUDevice::IsSameRenderAPI(current_api, expected_api))
  {
    const bool fullscreen = cmd->fullscreen.value_or(Host::IsFullscreen());
    DestroyGPUPresenterOnThread();
    DestroyDeviceOnThread(false);

    Error local_error;
    if (!CreateDeviceOnThread(expected_api, fullscreen, false, &local_error))
    {
      Host::AddIconOSDMessage(
        "DeviceSwitchFailed", ICON_FA_PAINT_ROLLER,
        fmt::format(TRANSLATE_FS("OSDMessage", "Failed to create {} GPU device, reverting to {}.\n{}"),
                    GPUDevice::RenderAPIToString(expected_api), GPUDevice::RenderAPIToString(current_api),
                    local_error.GetDescription()),
        Host::OSD_CRITICAL_ERROR_DURATION);

      Host::ReleaseRenderWindow();
      if (current_api == RenderAPI::None || !CreateDeviceOnThread(current_api, fullscreen, true, &local_error))
      {
        if (cmd->error_ptr)
          *cmd->error_ptr = local_error;

        *cmd->out_result = false;
        return;
      }
    }
  }

  if (cmd->renderer.has_value())
  {
    // Do we want a renderer?
    if (!(*cmd->out_result = CreateGPUBackendOnThread(cmd->renderer.value(), cmd->upload_vram, cmd->error_ptr)))
    {
      // If we had a renderer, it means it was a switch, and we need to bail out the thread.
      if (had_renderer)
      {
        GPUThread::ReportFatalErrorAndShutdown("Failed to switch GPU backend.");
        *cmd->out_result = true;
      }
      else
      {
        // No point keeping the presenter around.
        DestroyGPUBackendOnThread();
        DestroyGPUPresenterOnThread();
      }
    }
  }
  else if (s_state.requested_fullscreen_ui)
  {
    const bool had_gpu_device = static_cast<bool>(g_gpu_device);
    if (!g_gpu_device && !CreateDeviceOnThread(expected_api, cmd->fullscreen.value_or(false), true, cmd->error_ptr))
    {
      *cmd->out_result = false;
      return;
    }

    // Don't need to present game frames anymore.
    DestroyGPUPresenterOnThread();

    // Don't need timing to run FSUI.
    g_gpu_device->SetGPUTimingEnabled(false);

    if (!(*cmd->out_result = FullscreenUI::IsInitialized() || FullscreenUI::Initialize()))
    {
      Error::SetStringView(cmd->error_ptr, "Failed to initialize FullscreenUI.");
      if (!had_gpu_device)
        DestroyDeviceOnThread(true);
    }
  }
  else
  {
    // Device is no longer needed.
    DestroyGPUBackendOnThread();
    DestroyDeviceOnThread(true);
  }
}

void GPUThread::DestroyGPUBackendOnThread()
{
  if (!s_state.gpu_backend)
    return;

  VERBOSE_LOG("Shutting down GPU backend...");

  SetRunIdleReason(RunIdleReason::NoGPUBackend, true);

  s_state.gpu_backend.reset();
}

void GPUThread::DestroyGPUPresenterOnThread()
{
  if (!s_state.gpu_presenter)
    return;

  VERBOSE_LOG("Shutting down GPU presenter...");

  ImGuiManager::DestroyAllDebugWindows();
  ImGuiManager::DestroyOverlayTextures();

  // Should have no queued frames by this point. Backend can get replaced with null.
  Assert(!s_state.gpu_backend);
  Assert(GPUBackend::GetQueuedFrameCount() == 0);

  s_state.gpu_presenter.reset();
}

bool GPUThread::Internal::PresentFrameAndRestoreContext()
{
  DebugAssert(IsOnThread());

  if (s_state.gpu_backend)
    s_state.gpu_backend->FlushRender();

  if (!GPUPresenter::PresentFrame(s_state.gpu_presenter.get(), s_state.gpu_backend.get(), false, 0))
    return false;

  if (s_state.gpu_backend)
    s_state.gpu_backend->RestoreDeviceContext();

  return true;
}

void GPUThread::SetThreadEnabled(bool enabled)
{
  if (s_state.use_gpu_thread == enabled)
    return;

  if (s_state.use_gpu_thread)
  {
    SyncGPUThread(false);
    std::atomic_thread_fence(std::memory_order_acquire);
  }

  // Was anything active?
  if (!g_gpu_device)
  {
    // Thread should be idle. Just reset the FIFO.
    s_state.use_gpu_thread = enabled;
    ResetCommandFIFO();
    return;
  }

  const bool fullscreen = Host::IsFullscreen();
  const bool requested_fullscreen_ui = s_state.requested_fullscreen_ui;
  const std::optional<GPURenderer> requested_renderer = s_state.requested_renderer;
  std::string serial = s_state.game_serial;

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
  Reconfigure(std::string(), std::nullopt, false, false, false, false, nullptr);

  // Thread should be idle at this point. Reset the FIFO.
  ResetCommandFIFO();

  // Update state and reconfigure again.
  s_state.use_gpu_thread = enabled;

  Error error;
  if (!Reconfigure(std::move(serial), requested_renderer, requested_renderer.has_value(), fullscreen,
                   requested_fullscreen_ui, true, &error))
  {
    ERROR_LOG("Reconfigure failed: {}", error.GetDescription());
    ReportFatalErrorAndShutdown(fmt::format("Reconfigure failed: {}", error.GetDescription()));
  }
}

void GPUThread::UpdateSettingsOnThread(GPUSettings&& new_settings)
{
  VERBOSE_LOG("Updating GPU settings on thread...");

  GPUSettings old_settings = std::move(g_gpu_settings);
  g_gpu_settings = std::move(new_settings);

  if (g_gpu_device)
  {
    if (g_gpu_settings.display_osd_scale != old_settings.display_osd_scale)
      ImGuiManager::SetGlobalScale(g_settings.display_osd_scale / 100.0f);
    if (g_gpu_settings.display_osd_margin != old_settings.display_osd_margin)
      ImGuiManager::SetScreenMargin(g_settings.display_osd_margin);

    FullscreenUI::CheckForConfigChanges(old_settings);
  }

  if (s_state.gpu_backend)
  {
    if (g_gpu_settings.display_show_gpu_usage != old_settings.display_show_gpu_usage)
      g_gpu_device->SetGPUTimingEnabled(g_gpu_settings.display_show_gpu_usage);

    Error error;
    if (!s_state.gpu_presenter->UpdateSettings(old_settings, &error) ||
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

void GPUThread::RunOnThread(AsyncCallType func)
{
  if (!s_state.use_gpu_thread) [[unlikely]]
  {
    func();
    return;
  }

  GPUThreadAsyncCallCommand* cmd =
    AllocateCommand<GPUThreadAsyncCallCommand>(GPUBackendCommandType::AsyncCall, std::move(func));
  PushCommandAndWakeThread(cmd);
}

void GPUThread::RunOnBackend(AsyncBackendCallType func, bool sync, bool spin_or_wake)
{
  if (!s_state.use_gpu_thread) [[unlikely]]
  {
    func(s_state.gpu_backend.get());
    return;
  }

  GPUThreadAsyncBackendCallCommand* cmd =
    AllocateCommand<GPUThreadAsyncBackendCallCommand>(GPUBackendCommandType::AsyncBackendCall, std::move(func));
  if (sync)
    PushCommandAndSync(cmd, spin_or_wake);
  else if (spin_or_wake)
    PushCommandAndWakeThread(cmd);
  else
    PushCommand(cmd);
}

std::pair<GPUThreadCommand*, void*> GPUThread::BeginASyncBufferCall(AsyncBufferCallType func, u32 buffer_size)
{
  // this is less than optimal, but it's only used for input osd updates currently, so whatever
  GPUThreadAsyncCallCommand* const cmd = AllocateCommand<GPUThreadAsyncCallCommand>(
    sizeof(GPUThreadAsyncCallCommand) + buffer_size, GPUBackendCommandType::AsyncCall);
  void* const buffer = static_cast<void*>(cmd + 1);
  cmd->func = [func, buffer]() { func(buffer); };
  return std::make_pair(static_cast<GPUThreadCommand*>(cmd), buffer);
}

void GPUThread::EndASyncBufferCall(GPUThreadCommand* cmd)
{
  if (!s_state.use_gpu_thread) [[unlikely]]
  {
    GPUThreadAsyncCallCommand* const acmd = static_cast<GPUThreadAsyncCallCommand*>(cmd);
    acmd->func();
    acmd->~GPUThreadAsyncCallCommand();
    return;
  }

  PushCommand(cmd);
}

void GPUThread::UpdateSettings(bool gpu_settings_changed, bool device_settings_changed, bool thread_changed)
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
    if (!Reconfigure(System::GetGameSerial(), s_state.requested_renderer, s_state.requested_renderer.has_value(),
                     std::nullopt, std::nullopt, true, &error)) [[unlikely]]
    {
      Host::ReportErrorAsync("Error", fmt::format("Failed to recreate GPU device: {}", error.GetDescription()));
    }
  }
  else if (gpu_settings_changed)
  {
    if (s_state.use_gpu_thread) [[likely]]
    {
      GPUThreadUpdateSettingsCommand* cmd =
        AllocateCommand<GPUThreadUpdateSettingsCommand>(GPUBackendCommandType::UpdateSettings, g_settings);
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

void GPUThread::ReportFatalErrorAndShutdown(std::string_view reason)
{
  DebugAssert(IsOnThread());

  std::string message = fmt::format("GPU thread shut down with fatal error:\n\n{}", reason);
  Host::RunOnCPUThread([message = std::move(message)]() { System::AbnormalShutdown(message); });

  // replace the renderer with a dummy/null backend, so that all commands get dropped
  ERROR_LOG("Switching to null renderer: {}", reason);
  s_state.gpu_presenter->ClearDisplayTexture();
  s_state.gpu_backend.reset();
  s_state.gpu_backend = GPUBackend::CreateNullBackend(*s_state.gpu_presenter);
  if (!s_state.gpu_backend->Initialize(false, nullptr)) [[unlikely]]
    Panic("Failed to initialize null GPU backend");
}

bool GPUThread::IsOnThread()
{
  return (!s_state.use_gpu_thread || s_state.gpu_thread.IsCallingThread());
}

bool GPUThread::IsUsingThread()
{
  return s_state.use_gpu_thread;
}

void GPUThread::ResizeDisplayWindow(s32 width, s32 height, float scale)
{
  RunOnThread([width, height, scale]() { ResizeDisplayWindowOnThread(width, height, scale); });
}

void GPUThread::ResizeDisplayWindowOnThread(u32 width, u32 height, float scale)
{
  // We should _not_ be getting this without a device, since we should have shut down.
  if (!g_gpu_device || !g_gpu_device->HasMainSwapChain())
    return;

  DEV_LOG("Display window resized to {}x{}", width, height);

  Error error;
  if (!g_gpu_device->GetMainSwapChain()->ResizeBuffers(width, height, scale, &error))
  {
    ERROR_LOG("Failed to resize main swap chain: {}", error.GetDescription());
    UpdateDisplayWindowOnThread(Host::IsFullscreen(), true);
    return;
  }

  DisplayWindowResizedOnThread();
}

void GPUThread::UpdateDisplayWindow(bool fullscreen)
{
  RunOnThread([fullscreen]() { UpdateDisplayWindowOnThread(fullscreen, true); });
}

void GPUThread::UpdateDisplayWindowOnThread(bool fullscreen, bool allow_exclusive_fullscreen)
{
  // In case we get the event late.
  if (!g_gpu_device)
    return;

  bool exclusive_fullscreen_requested = false;
  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (allow_exclusive_fullscreen && fullscreen && g_gpu_device->GetFeatures().exclusive_fullscreen)
  {
    fullscreen_mode =
      GPUDevice::ExclusiveFullscreenMode::Parse(Host::GetTinyStringSettingValue("GPU", "FullscreenMode", ""));
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
    if (!g_gpu_device->RecreateMainSwapChain(
          wi.value(), s_state.requested_vsync, s_state.requested_allow_present_throttle,
          fullscreen_mode.has_value() ? &fullscreen_mode.value() : nullptr, exclusive_fullscreen_control, &error))
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
    UpdateDisplayWindowOnThread(true, false);
    return;
  }

  DisplayWindowResizedOnThread();
}

bool GPUThread::CheckExclusiveFullscreenOnThread()
{
  if (g_gpu_device->HasMainSwapChain() && g_gpu_device->GetMainSwapChain()->IsExclusiveFullscreen())
    return true;

  Host::AddIconOSDWarning(
    "ExclusiveFullscreenFailed", ICON_EMOJI_WARNING,
    TRANSLATE_STR("OSDMessage", "Failed to switch to exclusive fullscreen, using borderless instead."),
    Host::OSD_INFO_DURATION);
  return false;
}

void GPUThread::DisplayWindowResizedOnThread()
{
  const GPUSwapChain* swap_chain = g_gpu_device->GetMainSwapChain();
  if (swap_chain)
    s_state.render_window_info = swap_chain->GetWindowInfo();
  else
    s_state.render_window_info = WindowInfo();
  std::atomic_thread_fence(std::memory_order_release);

  // surfaceless is usually temporary, so just ignore it
  if (!swap_chain)
    return;

  // our imgui stuff can't cope with 0x0/hidden windows
  const float f_width = static_cast<float>(std::max(swap_chain->GetWidth(), 1u));
  const float f_height = static_cast<float>(std::max(swap_chain->GetHeight(), 1u));
  ImGuiManager::WindowResized(swap_chain->GetFormat(), f_width, f_height);
  InputManager::SetDisplayWindowSize(f_width, f_height);

  if (s_state.gpu_backend)
  {
    Host::RunOnCPUThread(&System::DisplayWindowResized);

    // If we're paused, re-present the current frame at the new window size.
    if (IsSystemPaused())
    {
      // Hackity hack, on some systems, presenting a single frame isn't enough to actually get it
      // displayed. Two seems to be good enough. Maybe something to do with direct scanout.
      Internal::PresentFrameAndRestoreContext();
      Internal::PresentFrameAndRestoreContext();
    }
  }
}

const WindowInfo& GPUThread::GetRenderWindowInfo()
{
  // This is infrequently used, so we can get away with a full barrier.
  std::atomic_thread_fence(std::memory_order_acquire);
  return s_state.render_window_info;
}

void GPUThread::SetVSync(GPUVSyncMode mode, bool allow_present_throttle)
{
  RunOnThread([mode, allow_present_throttle]() {
    if (s_state.requested_vsync == mode && s_state.requested_allow_present_throttle == allow_present_throttle)
      return;

    s_state.requested_vsync = mode;
    s_state.requested_allow_present_throttle = allow_present_throttle;

    if (!g_gpu_device->HasMainSwapChain())
      return;

    Error error;
    if (!g_gpu_device->GetMainSwapChain()->SetVSyncMode(s_state.requested_vsync,
                                                        s_state.requested_allow_present_throttle, &error))
    {
      ERROR_LOG("Failed to update vsync mode: {}", error.GetDescription());
    }
  });

  // If we're turning on vsync or turning off present throttle, we want to drain the GPU thread.
  // Otherwise if it is currently behind, it'll be permanently stuck behind.
  if (mode != GPUVSyncMode::Disabled)
    SyncGPUThread(false);
}

void GPUThread::PresentCurrentFrame()
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

bool GPUThread::GetRunIdleReason(RunIdleReason reason)
{
  return (s_state.run_idle_reasons & static_cast<u8>(reason)) != 0;
}

void GPUThread::SetRunIdleReason(RunIdleReason reason, bool enabled)
{
  const u8 bit = static_cast<u8>(reason);
  if (((s_state.run_idle_reasons & bit) != 0) == enabled)
    return;

  s_state.run_idle_reasons = enabled ? (s_state.run_idle_reasons | bit) : (s_state.run_idle_reasons & ~bit);
  UpdateRunIdle();
}

bool GPUThread::IsRunningIdle()
{
  return s_state.run_idle_flag;
}

bool GPUThread::IsSystemPaused()
{
  return ((s_state.run_idle_reasons & static_cast<u8>(RunIdleReason::SystemPaused)) != 0);
}

void GPUThread::UpdateRunIdle()
{
  DebugAssert(IsOnThread());

  // We require either invalid-system or paused for run idle.
  static constexpr u8 REQUIRE_MASK = static_cast<u8>(RunIdleReason::NoGPUBackend) |
                                     static_cast<u8>(RunIdleReason::SystemPaused) |
                                     static_cast<u8>(RunIdleReason::LoadingScreenActive);
  static constexpr u8 ACTIVATE_MASK =
    static_cast<u8>(RunIdleReason::FullscreenUIActive) | static_cast<u8>(RunIdleReason::LoadingScreenActive);

  const bool new_flag = (g_gpu_device && ((s_state.run_idle_reasons & REQUIRE_MASK) != 0) &&
                         ((s_state.run_idle_reasons & ACTIVATE_MASK) != 0));
  if (s_state.run_idle_flag == new_flag)
    return;

  s_state.run_idle_flag = new_flag;
  DEV_LOG("GPU thread now {} idle", new_flag ? "running" : "NOT running");
  Host::OnGPUThreadRunIdleChanged(new_flag);
}

const std::string& GPUThread::GetGameSerial()
{
  DebugAssert(IsOnThread());
  return s_state.game_serial;
}

void GPUThread::SetGameSerial(std::string serial)
{
  DebugAssert(!IsOnThread() || !s_state.use_gpu_thread);
  RunOnThread([serial = std::move(serial)]() mutable {
    const bool changed = (s_state.game_serial != serial);
    s_state.game_serial = std::move(serial);
    if (changed)
    {
      GPUTextureCache::GameSerialChanged();
      if (SaveStateSelectorUI::IsOpen())
        SaveStateSelectorUI::RefreshList();
    }
  });
}
