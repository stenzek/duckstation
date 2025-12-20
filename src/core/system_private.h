// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "system.h"

#include <functional>

class GPUBackend;
struct GPUBackendFramePresentationParameters;

namespace System {

/// Memory save states - only for internal use.
struct MemorySaveState
{
  DynamicHeapArray<u8> state_data;
  size_t state_size;

  std::unique_ptr<GPUTexture> vram_texture;
  DynamicHeapArray<u8> gpu_state_data;
  size_t gpu_state_size;
};

MemorySaveState& AllocateMemoryState();
MemorySaveState& GetFirstMemoryState();
MemorySaveState& PopMemoryState();
bool AllocateMemoryStates(size_t state_count, bool recycle_old_textures);
void FreeMemoryStateStorage(bool release_memory, bool release_textures, bool recycle_textures);
void LoadMemoryState(MemorySaveState& mss, bool update_display);
void SaveMemoryState(MemorySaveState& mss);

bool IsRunaheadActive();
void IncrementFrameNumber();
void IncrementInternalFrameNumber();
void FrameDone();

/// Returns true if vsync should be used.
GPUVSyncMode GetEffectiveVSyncMode();
bool ShouldAllowPresentThrottle();

/// Retrieves timing information for frame presentation on the GPU thread.
/// Returns false if this frame should not be presented or the command buffer flushed.
bool GetFramePresentationParameters(GPUBackendFramePresentationParameters* frame);

/// Call when host display size changes.
void DisplayWindowResized();

/// Updates the internal GTE aspect ratio. Use with "match display" aspect ratio setting.
void UpdateGTEAspectRatio();

/// Updates the resolution scale when it is set to automatic.
void UpdateAutomaticResolutionScale();

/// Called on card read/write, handles fast forwarding.
void OnMemoryCardAccessed();

/// Immediately terminates the virtual machine, no state is saved.
void AbnormalShutdown(const std::string_view reason);

/// Performs mandatory hardware checks.
bool PerformEarlyHardwareChecks(Error* error);

/// Called on process startup, as early as possible.
bool ProcessStartup(Error* error);

/// Called on process shutdown.
void ProcessShutdown();

/// Called on CPU thread initialization.
bool CoreThreadInitialize(Error* error);

/// Called on CPU thread shutdown.
void CoreThreadShutdown();

/// Returns a handle to the CPU thread.
const Threading::ThreadHandle& GetCoreThreadHandle();

/// Changes the CPU thread handle, use with care.
void SetCoreThreadHandle(Threading::ThreadHandle handle);

/// Polls input, updates subsystems which are present while paused/inactive.
void IdlePollUpdate();

} // namespace System

namespace Host {

/// Called with the settings lock held, when system settings are being loaded (should load input sources, etc).
void LoadSettings(const SettingsInterface& si, std::unique_lock<std::mutex>& lock);

/// Called after settings are updated.
void CheckForSettingsChanges(const Settings& old_settings);

/// Called when the VM is starting initialization, but has not been completed yet.
void OnSystemStarting();

/// Called when the VM is created.
void OnSystemStarted();

/// Called when the VM is shutting down.
void OnSystemStopping();

/// Called when the VM is shut down or destroyed.
void OnSystemDestroyed();

/// Called when the VM is paused.
void OnSystemPaused();

/// Called when the VM is resumed after being paused.
void OnSystemResumed();

/// Called when the VM abnormally exits because an error has occurred, and it cannot continue.
void OnSystemAbnormalShutdown(const std::string_view reason);

/// Called when performance metrics are updated, approximately once a second.
void OnPerformanceCountersUpdated(const GPUBackend* gpu_backend);

/// Provided by the host; called when the running executable changes.
void OnSystemGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name,
                         GameHash game_hash);

/// Provided by the host; called when the undo save state availability changes.
void OnSystemUndoStateAvailabilityChanged(bool available, u64 timestamp);

/// Called when media capture starts/stops.
void OnMediaCaptureStarted();
void OnMediaCaptureStopped();

/// Provided by the host; called once per frame at guest vsync.
void PumpMessagesOnCoreThread();

/// Requests a specific display window size.
void RequestResizeHostDisplay(s32 width, s32 height);

} // namespace Host
