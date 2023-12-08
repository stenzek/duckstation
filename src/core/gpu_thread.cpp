// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gpu_thread.h"
#include "fullscreen_ui.h"
#include "gpu_backend.h"
#include "gpu_types.h"
#include "host.h"
#include "imgui_overlays.h"
#include "performance_counters.h"
#include "settings.h"
#include "shader_cache_version.h"
#include "system.h"
#include "system_private.h"

#include "util/gpu_device.h"
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

// TODO: SW renderer for readback flag in class.
// TODO: Smaller settings struct.
// TODO: Remove g_gpu pointer.
// TODO: Auto size video capture.
// TODO: Smooth loady bar for achievements.
// TODO: Tidy up gpu_backend headers.
// TODO: Disable thread when debug windows are enabled.
// TODO: Fullscreen UI without thread active, locks up.

namespace GPUThread {
enum : u32
{
  COMMAND_QUEUE_SIZE = 16 * 1024 * 1024,
  THRESHOLD_TO_WAKE_GPU = 65536,
  MAX_SKIPPED_PRESENT_COUNT = 50
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

static u32 GetPendingCommandSize();
static void ResetCommandFIFO();
static bool IsCommandFIFOEmpty();
static void WakeGPUThread();
static bool SleepGPUThread(bool allow_sleep);

static bool CreateDeviceOnThread(RenderAPI api, bool fullscreen, Error* error);
static void DestroyDeviceOnThread();
static void ResizeDisplayWindowOnThread(u32 width, u32 height, float scale);
static void UpdateDisplayWindowOnThread(bool fullscreen);
static void DisplayWindowResizedOnThread();
static void HandleGPUDeviceLost();
static void HandleExclusiveFullscreenLost();

static void ReconfigureOnThread(GPUThreadReconfigureCommand* cmd);
static bool CreateGPUBackendOnThread(GPURenderer renderer, bool upload_vram, Error* error);
static void DestroyGPUBackendOnThread();

static void UpdateSettingsOnThread(const Settings& old_settings);

static void UpdateRunIdle();

static void SleepUntilPresentTime(Timer::Value present_time);

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
  std::atomic<u32> command_fifo_read_ptr{0};
  u32 skipped_present_count = 0;
  u8 run_idle_reasons = 0;
  bool run_idle_flag = false;
  GPUVSyncMode requested_vsync = GPUVSyncMode::Disabled;
  bool requested_allow_present_throttle = false;
  bool requested_fullscreen_ui = false;
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

void GPUThread::Internal::SetThreadEnabled(bool enabled)
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
  Reconfigure(std::nullopt, false, std::nullopt, std::nullopt, false, nullptr);

  // Thread should be idle at this point. Reset the FIFO.
  ResetCommandFIFO();

  // Update state and reconfigure again.
  s_state.use_gpu_thread = enabled;

  Error error;
  if (!Reconfigure(requested_renderer, requested_renderer.has_value(), fullscreen, requested_fullscreen_ui, true,
                   &error))
  {
    ERROR_LOG("Reconfigure failed: {}", error.GetDescription());
    Panic("Failed to reconfigure when changing thread state.");
  }
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

bool GPUThread::Reconfigure(std::optional<GPURenderer> renderer, bool upload_vram, std::optional<bool> fullscreen,
                            std::optional<bool> start_fullscreen_ui, bool recreate_device, Error* error)
{
  INFO_LOG("Reconfiguring GPU thread.");

  GPUThreadReconfigureCommand* cmd = static_cast<GPUThreadReconfigureCommand*>(
    AllocateCommand(GPUBackendCommandType::Reconfigure, sizeof(GPUThreadReconfigureCommand)));
  cmd->renderer = renderer;
  cmd->fullscreen = fullscreen;
  cmd->start_fullscreen_ui = start_fullscreen_ui;
  cmd->vsync_mode = System::GetEffectiveVSyncMode();
  cmd->allow_present_throttle = System::ShouldAllowPresentThrottle();
  cmd->force_recreate_device = recreate_device;
  cmd->upload_vram = upload_vram;
  cmd->error_ptr = error;

  if (!s_state.use_gpu_thread) [[unlikely]]
    ReconfigureOnThread(cmd);
  else
    PushCommandAndSync(cmd, false);

  return cmd->result;
}

bool GPUThread::StartFullscreenUI(bool fullscreen, Error* error)
{
  // Don't need to reconfigure if we already have a system.
  if (System::IsValid())
  {
    RunOnThread([]() { s_state.requested_fullscreen_ui = true; });
    return true;
  }

  return Reconfigure(std::nullopt, false, fullscreen, true, false, error);
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
    RunOnThread([]() { s_state.requested_fullscreen_ui = true; });
    return;
  }

  Reconfigure(std::nullopt, false, std::nullopt, false, false, nullptr);
}

std::optional<GPURenderer> GPUThread::GetRequestedRenderer()
{
  return s_state.requested_renderer;
}

bool GPUThread::CreateGPUBackend(GPURenderer renderer, bool upload_vram, bool fullscreen, bool force_recreate_device,
                                 Error* error)
{
  s_state.requested_renderer = renderer;
  return Reconfigure(renderer, upload_vram, fullscreen ? std::optional<bool>(true) : std::nullopt, std::nullopt,
                     force_recreate_device, error);
}

void GPUThread::DestroyGPUBackend()
{
  Reconfigure(std::nullopt, false, std::nullopt, std::nullopt, false, nullptr);
  s_state.requested_renderer.reset();
}

bool GPUThread::HasGPUBackend()
{
  DebugAssert(IsOnThread());
  return (s_state.gpu_backend != nullptr);
}

GPUThreadCommand* GPUThread::AllocateCommand(GPUBackendCommandType command, u32 size)
{
  // Ensure size is a multiple of 8 (minimum data size) so we don't end up with an unaligned command.
  // NOTE: If we ever end up putting vectors in the command packets, this should be raised.
  size = Common::AlignUpPow2(size, 8);

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
      if ((size + sizeof(GPUBackendCommand)) > available_size)
      {
        // allocate a dummy command to wrap the buffer around
        GPUBackendCommand* dummy_cmd = reinterpret_cast<GPUBackendCommand*>(&s_state.command_fifo_data[write_ptr]);
        dummy_cmd->type = GPUBackendCommandType::Wraparound;
        dummy_cmd->size = available_size;
        dummy_cmd->params.bits = 0;
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
        Internal::PresentFrame(false, 0);
        if (!g_gpu_device->GetMainSwapChain()->IsVSyncModeBlocking())
          g_gpu_device->GetMainSwapChain()->ThrottlePresentation();

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

        case GPUBackendCommandType::Reconfigure:
        {
          ReconfigureOnThread(static_cast<GPUThreadReconfigureCommand*>(cmd));
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
}

bool GPUThread::CreateDeviceOnThread(RenderAPI api, bool fullscreen, Error* error)
{
  DebugAssert(!g_gpu_device);

  INFO_LOG("Trying to create a {} GPU device...", GPUDevice::RenderAPIToString(api));
  g_gpu_device = GPUDevice::CreateDeviceForAPI(api);

  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (fullscreen && g_gpu_device && g_gpu_device->SupportsExclusiveFullscreen())
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
        g_gpu_settings.gpu_adapter, static_cast<GPUDevice::FeatureMask>(disabled_features), shader_dump_directory,
        g_gpu_settings.gpu_disable_shader_cache ? std::string_view() : std::string_view(EmuFolders::Cache),
        SHADER_CACHE_VERSION, g_gpu_settings.gpu_use_debug_device, wi.value(), s_state.requested_vsync,
        s_state.requested_allow_present_throttle, fullscreen_mode.has_value() ? &fullscreen_mode.value() : nullptr,
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

  if (!ImGuiManager::Initialize(g_gpu_settings.display_osd_scale / 100.0f, g_gpu_settings.display_osd_margin,
                                &create_error) ||
      (s_state.requested_fullscreen_ui && !FullscreenUI::Initialize()))
  {
    ERROR_LOG("Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    Error::SetStringFmt(error, "Failed to initialize ImGuiManager: {}", create_error.GetDescription());
    FullscreenUI::Shutdown();
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

  return true;
}

void GPUThread::DestroyDeviceOnThread()
{
  if (!g_gpu_device)
    return;

  const bool has_window = g_gpu_device->HasMainSwapChain();

  ImGuiManager::DestroyOverlayTextures();
  FullscreenUI::Shutdown();
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

void GPUThread::HandleGPUDeviceLost()
{
  static Timer::Value s_last_gpu_reset_time = 0;
  static constexpr float MIN_TIME_BETWEEN_RESETS = 15.0f;

  // If we're constantly crashing on something in particular, we don't want to end up in an
  // endless reset loop.. that'd probably end up leaking memory and/or crashing us for other
  // reasons. So just abort in such case.
  const Timer::Value current_time = Timer::GetCurrentValue();
  if (s_last_gpu_reset_time != 0 &&
      Timer::ConvertValueToSeconds(current_time - s_last_gpu_reset_time) < MIN_TIME_BETWEEN_RESETS)
  {
    Panic("Host GPU lost too many times, device is probably completely wedged.");
  }
  s_last_gpu_reset_time = current_time;

  const bool is_fullscreen = Host::IsFullscreen();

  // Device lost, something went really bad.
  // Let's just toss out everything, and try to hobble on.
  DestroyGPUBackendOnThread();
  DestroyDeviceOnThread();

  Error error;
  if (!CreateDeviceOnThread(
        Settings::GetRenderAPIForRenderer(s_state.requested_renderer.value_or(g_gpu_settings.gpu_renderer)),
        is_fullscreen, &error) ||
      (s_state.requested_renderer.has_value() &&
       !CreateGPUBackendOnThread(s_state.requested_renderer.value(), true, &error)))
  {
    ERROR_LOG("Failed to recreate GPU device after loss: {}", error.GetDescription());
    Panic("Failed to recreate GPU device after loss.");
    return;
  }

  // First frame after reopening is definitely going to be trash, so skip it.
  Host::AddIconOSDWarning(
    "HostGPUDeviceLost", ICON_EMOJI_WARNING,
    TRANSLATE_STR("System", "Host GPU device encountered an error and has recovered. This may cause broken rendering."),
    Host::OSD_CRITICAL_ERROR_DURATION);
}

void GPUThread::HandleExclusiveFullscreenLost()
{
  WARNING_LOG("Lost exclusive fullscreen.");
  Host::SetFullscreen(false);
}

bool GPUThread::CreateGPUBackendOnThread(GPURenderer renderer, bool upload_vram, Error* error)
{
  const bool is_hardware = (renderer != GPURenderer::Software);

  if (is_hardware)
    s_state.gpu_backend = GPUBackend::CreateHardwareBackend();
  else
    s_state.gpu_backend = GPUBackend::CreateSoftwareBackend();

  Error local_error;
  bool okay = s_state.gpu_backend->Initialize(upload_vram, &local_error);
  if (!okay)
  {
    ERROR_LOG("Failed to create {} renderer: {}", Settings::GetRendererName(renderer), local_error.GetDescription());

    if (is_hardware)
    {
      Host::AddIconOSDMessage(
        "GPUBackendCreationFailed", ICON_FA_PAINT_ROLLER,
        fmt::format(TRANSLATE_FS("OSDMessage", "Failed to initialize {} renderer, falling back to software renderer."),
                    Settings::GetRendererName(s_state.requested_renderer.value())),
        Host::OSD_CRITICAL_ERROR_DURATION);

      s_state.requested_renderer = GPURenderer::Software;
      s_state.gpu_backend = GPUBackend::CreateSoftwareBackend();
      okay = s_state.gpu_backend->Initialize(upload_vram, &local_error);
    }

    if (!okay)
    {
      if (error)
        *error = local_error;
      return false;
    }
  }

  g_gpu_device->SetGPUTimingEnabled(g_gpu_settings.display_show_gpu_usage);
  PostProcessing::Initialize();
  ImGuiManager::UpdateDebugWindowConfig();
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
    DestroyDeviceOnThread();
    return;
  }

  // TODO: Make this suck less.
  g_gpu_settings = g_settings;

  // Readback old VRAM for hardware renderers.
  if (s_state.gpu_backend && cmd->renderer.has_value() && cmd->upload_vram)
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
    DestroyDeviceOnThread();

    Error local_error;
    if (!CreateDeviceOnThread(expected_api, fullscreen, &local_error))
    {
      Host::AddIconOSDMessage(
        "DeviceSwitchFailed", ICON_FA_PAINT_ROLLER,
        fmt::format(TRANSLATE_FS("OSDMessage", "Failed to create {} GPU device, reverting to {}.\n{}"),
                    GPUDevice::RenderAPIToString(expected_api), GPUDevice::RenderAPIToString(current_api),
                    local_error.GetDescription()),
        Host::OSD_CRITICAL_ERROR_DURATION);

      Host::ReleaseRenderWindow();
      if (current_api == RenderAPI::None || !CreateDeviceOnThread(current_api, fullscreen, &local_error))
      {
        if (cmd->error_ptr)
          *cmd->error_ptr = local_error;

        cmd->result = false;
        return;
      }
    }
  }

  if (cmd->renderer.has_value())
  {
    // Do we want a renderer?
    cmd->result = CreateGPUBackendOnThread(cmd->renderer.value(), cmd->upload_vram, cmd->error_ptr);
  }
  else if (s_state.requested_fullscreen_ui)
  {
    if (!g_gpu_device && !CreateDeviceOnThread(expected_api, cmd->fullscreen.value_or(false), cmd->error_ptr))
    {
      cmd->result = false;
      return;
    }

    // Don't need timing to run FSUI.
    g_gpu_device->SetGPUTimingEnabled(false);

    cmd->result = FullscreenUI::IsInitialized() || FullscreenUI::Initialize();
    if (!cmd->result)
      Error::SetStringView(cmd->error_ptr, "Failed to initialize FullscreenUI.");
  }
  else
  {
    // Device is no longer needed.
    DestroyDeviceOnThread();
  }
}

void GPUThread::DestroyGPUBackendOnThread()
{
  if (!s_state.gpu_backend)
    return;

  VERBOSE_LOG("Shutting down GPU backend...");

  SetRunIdleReason(RunIdleReason::NoGPUBackend, true);

  ImGuiManager::DestroyAllDebugWindows();
  PostProcessing::Shutdown();
  s_state.gpu_backend.reset();
}

void GPUThread::UpdateSettingsOnThread(const Settings& old_settings)
{
  DebugAssert(s_state.gpu_backend);
  if (g_gpu_settings.display_show_gpu_usage != old_settings.display_show_gpu_usage)
    g_gpu_device->SetGPUTimingEnabled(g_gpu_settings.display_show_gpu_usage);

  if (g_gpu_settings.display_osd_scale != old_settings.display_osd_scale)
    ImGuiManager::SetGlobalScale(g_settings.display_osd_scale / 100.0f);
  if (g_gpu_settings.display_osd_margin != old_settings.display_osd_margin)
    ImGuiManager::SetScreenMargin(g_settings.display_osd_margin);

  FullscreenUI::CheckForConfigChanges(old_settings);

  PostProcessing::UpdateSettings();

  s_state.gpu_backend->UpdateSettings(old_settings);
  if (ImGuiManager::UpdateDebugWindowConfig() || (PostProcessing::DisplayChain.IsActive() && !IsSystemPaused()))
    Internal::PresentFrame(false, 0);

  s_state.gpu_backend->RestoreDeviceContext();
}

void GPUThread::RunOnThread(AsyncCallType func)
{
  if (!s_state.use_gpu_thread) [[unlikely]]
  {
    func();
    return;
  }

  GPUThreadAsyncCallCommand* cmd = static_cast<GPUThreadAsyncCallCommand*>(
    AllocateCommand(GPUBackendCommandType::AsyncCall, sizeof(GPUThreadAsyncCallCommand)));
  new (cmd) GPUThreadAsyncCallCommand;
  cmd->func = std::move(func);
  PushCommandAndWakeThread(cmd);
}

void GPUThread::UpdateSettings(bool gpu_settings_changed)
{
  if (gpu_settings_changed)
  {
    RunOnThread([settings = g_settings]() {
      VERBOSE_LOG("Updating GPU settings on thread...");

      Settings old_settings = std::move(g_gpu_settings);
      g_gpu_settings = std::move(settings);

      if (s_state.gpu_backend)
        UpdateSettingsOnThread(old_settings);
    });
  }
  else
  {
    RunOnThread([]() {
      if (s_state.gpu_backend)
      {
        PostProcessing::UpdateSettings();
        if (ImGuiManager::UpdateDebugWindowConfig() || (PostProcessing::DisplayChain.IsActive() && !IsSystemPaused()))
          Internal::PresentFrame(false, 0);
      }
    });
  }
}

bool GPUThread::IsOnThread()
{
  return (!s_state.use_gpu_thread || s_state.gpu_thread.IsCallingThread());
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
    UpdateDisplayWindowOnThread(Host::IsFullscreen());
    return;
  }

  DisplayWindowResizedOnThread();
}

void GPUThread::UpdateDisplayWindow(bool fullscreen)
{
  RunOnThread([fullscreen]() { UpdateDisplayWindowOnThread(fullscreen); });
}

void GPUThread::UpdateDisplayWindowOnThread(bool fullscreen)
{
  // In case we get the event late.
  if (!g_gpu_device)
    return;

  std::optional<GPUDevice::ExclusiveFullscreenMode> fullscreen_mode;
  if (fullscreen && g_gpu_device->SupportsExclusiveFullscreen())
  {
    fullscreen_mode =
      GPUDevice::ExclusiveFullscreenMode::Parse(Host::GetTinyStringSettingValue("GPU", "FullscreenMode", ""));
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
    Host::AcquireRenderWindow(g_gpu_device->GetRenderAPI(), fullscreen, fullscreen_mode.has_value(), &error);
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

  DisplayWindowResizedOnThread();
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

  const float f_width = static_cast<float>(swap_chain->GetWidth());
  const float f_height = static_cast<float>(swap_chain->GetHeight());
  ImGuiManager::WindowResized(f_width, f_height);
  InputManager::SetDisplayWindowSize(f_width, f_height);

  if (s_state.gpu_backend)
  {
    Host::RunOnCPUThread(&System::DisplayWindowResized);

    // If we're paused, re-present the current frame at the new window size.
    if (IsSystemPaused())
    {
      // Hackity hack, on some systems, presenting a single frame isn't enough to actually get it
      // displayed. Two seems to be good enough. Maybe something to do with direct scanout.
      Internal::PresentFrame(false, 0);
      Internal::PresentFrame(false, 0);
    }

    if (g_gpu_settings.gpu_resolution_scale == 0)
      s_state.gpu_backend->UpdateResolutionScale();
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
}

void GPUThread::PresentCurrentFrame()
{
  RunOnThread([]() {
    if (s_state.run_idle_flag)
    {
      // If we're running idle, we're going to re-present anyway.
      return;
    }

    Internal::PresentFrame(false, 0);
  });
}

void GPUThread::SleepUntilPresentTime(Timer::Value present_time)
{
  // Use a spinwait if we undersleep for all platforms except android.. don't want to burn battery.
  // Linux also seems to do a much better job of waking up at the requested time.

#if !defined(__linux__) && !defined(__ANDROID__)
  Timer::SleepUntil(present_time, true);
#else
  Timer::SleepUntil(present_time, false);
#endif
}

void GPUThread::Internal::PresentFrame(bool allow_skip_present, u64 present_time)
{
  const bool skip_present = (!g_gpu_device->HasMainSwapChain() ||
                             (allow_skip_present && g_gpu_device->GetMainSwapChain()->ShouldSkipPresentingFrame() &&
                              s_state.skipped_present_count < MAX_SKIPPED_PRESENT_COUNT));

  if (!skip_present)
  {
    // acquire for IO.MousePos and system state.
    std::atomic_thread_fence(std::memory_order_acquire);

    FullscreenUI::Render();

    if (s_state.gpu_backend && System::IsValid())
      ImGuiManager::RenderTextOverlays(s_state.gpu_backend.get());

    ImGuiManager::RenderOSDMessages();

    if (s_state.gpu_backend && System::GetState() == System::State::Running)
      ImGuiManager::RenderSoftwareCursors();

    ImGuiManager::RenderOverlayWindows();
    ImGuiManager::RenderDebugWindows();
  }

  const GPUDevice::PresentResult pres =
    skip_present ? GPUDevice::PresentResult::SkipPresent :
                   (s_state.gpu_backend ? s_state.gpu_backend->PresentDisplay() :
                                          g_gpu_device->BeginPresent(g_gpu_device->GetMainSwapChain()));
  if (pres == GPUDevice::PresentResult::OK)
  {
    s_state.skipped_present_count = 0;

    g_gpu_device->RenderImGui(g_gpu_device->GetMainSwapChain());

    const GPUDevice::Features features = g_gpu_device->GetFeatures();
    const bool scheduled_present = (present_time != 0);
    const bool explicit_present = (scheduled_present && (features.explicit_present && !features.timed_present));
    const bool timed_present = (scheduled_present && features.timed_present);

    if (scheduled_present && !explicit_present)
    {
      // No explicit present support, simulate it with Flush.
      g_gpu_device->FlushCommands();
      SleepUntilPresentTime(present_time);
    }

    g_gpu_device->EndPresent(g_gpu_device->GetMainSwapChain(), explicit_present, timed_present ? present_time : 0);

    if (g_gpu_device->IsGPUTimingEnabled())
      PerformanceCounters::AccumulateGPUTime();

    if (explicit_present)
    {
      SleepUntilPresentTime(present_time);
      g_gpu_device->SubmitPresent(g_gpu_device->GetMainSwapChain());
    }
  }
  else
  {
    s_state.skipped_present_count++;

    if (pres == GPUDevice::PresentResult::DeviceLost) [[unlikely]]
      HandleGPUDeviceLost();
    else if (pres == GPUDevice::PresentResult::ExclusiveFullscreenLost)
      HandleExclusiveFullscreenLost();
    else if (!skip_present)
      g_gpu_device->FlushCommands();

    // Still need to kick ImGui or it gets cranky.
    ImGui::EndFrame();
  }

  ImGuiManager::NewFrame();

  RestoreContextAfterPresent();
}

void GPUThread::Internal::RestoreContextAfterPresent()
{
  if (s_state.gpu_backend)
    s_state.gpu_backend->RestoreDeviceContext();
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
