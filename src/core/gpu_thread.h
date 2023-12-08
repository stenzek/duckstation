// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <functional>
#include <optional>

class Error;
struct WindowInfo;

namespace Threading {
class ThreadHandle;
}

enum class RenderAPI : u8;
enum class GPUVSyncMode : u8;

enum class GPURenderer : u8;
enum class GPUBackendCommandType : u8;

struct GPUThreadCommand;
struct GPUBackendUpdateDisplayCommand;

namespace GPUThread {
using AsyncCallType = std::function<void()>;

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
bool CreateGPUBackend(GPURenderer renderer, bool upload_vram, bool fullscreen, bool force_recreate_device,
                      Error* error);
void DestroyGPUBackend();
bool HasGPUBackend();

/// Re-presents the current frame. Call when things like window resizes happen to re-display
/// the current frame with the correct proportions. Should only be called from the CPU thread.
void PresentCurrentFrame();

/// Handles fullscreen transitions and such.
void UpdateDisplayWindow(bool fullscreen);

/// Called when the window is resized.
void ResizeDisplayWindow(s32 width, s32 height, float scale);

/// Access to main window size from CPU thread.
const WindowInfo& GetRenderWindowInfo();

void UpdateSettings(bool gpu_settings_changed);

bool IsOnThread();
void RunOnThread(AsyncCallType func);
void SetVSync(GPUVSyncMode mode, bool allow_present_throttle);

bool GetRunIdleReason(RunIdleReason reason);
void SetRunIdleReason(RunIdleReason reason, bool enabled);

// Should only be called on the GPU thread.
bool IsSystemPaused();

GPUThreadCommand* AllocateCommand(GPUBackendCommandType command, u32 size);
void PushCommand(GPUThreadCommand* cmd);
void PushCommandAndWakeThread(GPUThreadCommand* cmd);
void PushCommandAndSync(GPUThreadCommand* cmd, bool spin);
void SyncGPUThread(bool spin);

// NOTE: Only called by GPUBackend
namespace Internal {
const Threading::ThreadHandle& GetThreadHandle();
void ProcessStartup();
void SetThreadEnabled(bool enabled);
void RequestShutdown();
void GPUThreadEntryPoint();
void PresentFrame(bool allow_skip_present, u64 present_time);
void RestoreContextAfterPresent();
} // namespace Internal
} // namespace GPUThread

namespace Host {

/// Called when the pause state changes, or fullscreen UI opens.
void OnGPUThreadRunIdleChanged(bool is_active);

}
