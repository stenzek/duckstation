// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/window_info.h"

#include "common/types.h"

enum class RenderAPI : u8;

class QWidget;

namespace QtUtils {

/// Returns the window type for the current Qt platform.
WindowInfoType GetWindowInfoType();

/// Returns the common window info structure for a Qt widget.
std::optional<WindowInfo> GetWindowInfoForWidget(QWidget* widget, RenderAPI render_api, Error* error = nullptr);

/// Calculates the pixel size (real geometry) for a widget.
/// Also sets the "real" DPR scale for the widget, ignoring any operating-system level downsampling.
void UpdateSurfaceSize(QWidget* widget, WindowInfo* wi);

/// Changes the screensaver inhibit state.
bool SetScreensaverInhibit(bool inhibit, Error* error);

#ifdef _WIN32

/// Sets the rounded corner state for a window.
/// Currently only supported on Windows.
bool SetWindowRoundedCornerState(QWidget* widget, bool enabled);

#endif

} // namespace QtUtils
