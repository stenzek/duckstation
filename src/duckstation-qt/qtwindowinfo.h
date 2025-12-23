// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/window_info.h"

#include "common/types.h"

enum class RenderAPI : u8;

class QWidget;

namespace QtUtils {

/// Returns the pixel size (real geometry) for a widget.
/// Also returns the "real" DPR scale for the widget, ignoring any operating-system level downsampling.
std::pair<QSize, qreal> GetPixelSizeForWidget(const QWidget* widget);

/// Returns the window type for the current Qt platform.
WindowInfoType GetWindowInfoType();

/// Returns the common window info structure for a Qt widget.
std::optional<WindowInfo> GetWindowInfoForWidget(QWidget* widget, RenderAPI render_api, Error* error = nullptr);

} // namespace QtUtils
