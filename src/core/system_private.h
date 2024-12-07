// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "system.h"

#include <functional>

namespace System {

/// Memory save states - only for internal use.
struct MemorySaveState
{
  std::unique_ptr<GPUTexture> vram_texture;
  DynamicHeapArray<u8> state_data;
  size_t state_size;
};

MemorySaveState& AllocateMemoryState();
MemorySaveState& GetFirstMemoryState();
MemorySaveState& PopMemoryState();
void FreeMemoryStateStorage();
void LoadMemoryState(MemorySaveState& mss, bool update_display);
bool SaveMemoryState(MemorySaveState& mss);

/// Returns the maximum size of a save state, considering the current configuration.
size_t GetMaxSaveStateSize();

void IncrementFrameNumber();
void IncrementInternalFrameNumber();
void FrameDone();

/// Returns true if vsync should be used.
GPUVSyncMode GetEffectiveVSyncMode();
bool ShouldAllowPresentThrottle();

/// Call when host display size changes.
void DisplayWindowResized();

/// Updates the internal GTE aspect ratio. Use with "match display" aspect ratio setting.
void UpdateGTEAspectRatio();

/// Performs mandatory hardware checks.
bool PerformEarlyHardwareChecks(Error* error);

/// Called on process startup, as early as possible.
bool ProcessStartup(Error* error);

/// Called on process shutdown.
void ProcessShutdown();

/// Called on CPU thread initialization.
bool CPUThreadInitialize(Error* error);

/// Called on CPU thread shutdown.
void CPUThreadShutdown();

/// Returns a handle to the CPU thread.
const Threading::ThreadHandle& GetCPUThreadHandle();

/// Polls input, updates subsystems which are present while paused/inactive.
void IdlePollUpdate();

/// Task threads, asynchronous work which will block system shutdown.
void QueueTaskOnThread(std::function<void()> task);
void RemoveSelfFromTaskThreads();

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

/// Called when the VM is shut down or destroyed.
void OnSystemDestroyed();

/// Called when the VM is paused.
void OnSystemPaused();

/// Called when the VM is resumed after being paused.
void OnSystemResumed();

/// Called when the pause state changes, or fullscreen UI opens.
void OnIdleStateChanged();

/// Called when performance metrics are updated, approximately once a second.
void OnPerformanceCountersUpdated();

/// Provided by the host; called when the running executable changes.
void OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name);

/// Called when media capture starts/stops.
void OnMediaCaptureStarted();
void OnMediaCaptureStopped();

/// Provided by the host; called once per frame at guest vsync.
void PumpMessagesOnCPUThread();

/// Requests a specific display window size.
void RequestResizeHostDisplay(s32 width, s32 height);

} // namespace Host
