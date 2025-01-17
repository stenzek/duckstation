// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <functional>
#include <optional>
#include <utility>

class Error;
struct WindowInfo;

namespace Threading {
class ThreadHandle;
}

enum class RenderAPI : u8;
enum class GPUVSyncMode : u8;

enum class GPURenderer : u8;
enum class GPUBackendCommandType : u8;

class GPUBackend;
struct GPUThreadCommand;
struct GPUBackendUpdateDisplayCommand;

namespace GPUThread {
using AsyncCallType = std::function<void()>;
using AsyncBackendCallType = std::function<void(GPUBackend*)>;
using AsyncBufferCallType = void(*)(void*);

enum class RunIdleReason : u8
{
  NoGPUBackend = (1 << 0),
  SystemPaused = (1 << 1),
  FullscreenUIActive = (1 << 2),
  LoadingScreenActive = (1 << 3),
};

/// Starts Big Picture UI.
bool StartFullscreenUI(bool fullscreen, Error* error);
bool IsFullscreenUIRequested();
void StopFullscreenUI();

/// Backend control.
std::optional<GPURenderer> GetRequestedRenderer();
bool CreateGPUBackend(std::string serial, GPURenderer renderer, bool upload_vram, bool fullscreen,
                      bool force_recreate_device, Error* error);
void DestroyGPUBackend();
bool HasGPUBackend();
bool IsGPUBackendRequested();
void SetGameSerial(std::string serial);

/// Re-presents the current frame. Call when things like window resizes happen to re-display
/// the current frame with the correct proportions. Should only be called from the CPU thread.
void PresentCurrentFrame();

/// Handles fullscreen transitions and such.
void UpdateDisplayWindow(bool fullscreen);

/// Called when the window is resized.
void ResizeDisplayWindow(s32 width, s32 height, float scale);

/// Access to main window size from CPU thread.
const WindowInfo& GetRenderWindowInfo();

void UpdateSettings(bool gpu_settings_changed, bool device_settings_changed);

bool IsOnThread();
bool IsUsingThread();
void RunOnThread(AsyncCallType func);
void RunOnBackend(AsyncBackendCallType func, bool sync, bool spin_or_wake);
std::pair<GPUThreadCommand*, void*> BeginASyncBufferCall(AsyncBufferCallType func, u32 buffer_size);
void EndASyncBufferCall(GPUThreadCommand* cmd);
void SetVSync(GPUVSyncMode mode, bool allow_present_throttle);

// Should only be called on the GPU thread.
bool GetRunIdleReason(RunIdleReason reason);
void SetRunIdleReason(RunIdleReason reason, bool enabled);
bool IsRunningIdle();
bool IsSystemPaused();
const std::string& GetGameSerial();

GPUThreadCommand* AllocateCommand(GPUBackendCommandType command, u32 size);
void PushCommand(GPUThreadCommand* cmd);
void PushCommandAndWakeThread(GPUThreadCommand* cmd);
void PushCommandAndSync(GPUThreadCommand* cmd, bool spin);
void SyncGPUThread(bool spin);

namespace Internal {
const Threading::ThreadHandle& GetThreadHandle();
void ProcessStartup();
void SetThreadEnabled(bool enabled);
void DoRunIdle();
void RequestShutdown();
void GPUThreadEntryPoint();
} // namespace Internal
} // namespace GPUThread

namespace Host {

/// Called when the pause state changes, or fullscreen UI opens.
void OnGPUThreadRunIdleChanged(bool is_active);

} // namespace Host
