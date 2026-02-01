// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/windows_headers.h"

namespace Win32WindowUtil {

/// Centers a window on the monitor where the mouse cursor is currently located.
/// Should be called before ShowWindow().
void CenterWindowOnMonitorAtCursorPosition(HWND hwnd);

} // namespace Win32WindowUtil
