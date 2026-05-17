// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "video_thread.h"

namespace VideoThread {

void ProcessStartup();
void ProcessShutdown();
void DoRunIdle();

} // namespace VideoThread

namespace Host {

/// Called when the core is creating a render device.
/// This could also be fullscreen transition.
std::optional<WindowInfo> AcquireRenderWindow(RenderAPI render_api, bool fullscreen, bool exclusive_fullscreen,
                                              Error* error);

/// Called when the core is finished with a render window.
void ReleaseRenderWindow();

/// Called before a fullscreen transition occurs.
bool CanChangeFullscreenMode(bool new_fullscreen_state);

/// Called when the pause state changes, or fullscreen UI opens.
void OnVideoThreadRunIdleChanged(bool is_active);

} // namespace Host
