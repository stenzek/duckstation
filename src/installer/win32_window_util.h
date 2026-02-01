// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/windows_headers.h"

namespace Win32WindowUtil {

/// Centers a window on the monitor where the mouse cursor is currently located.
/// Should be called before ShowWindow().
inline void CenterWindowOnMonitorAtCursorPosition(HWND hwnd)
{
  // Get the current cursor position
  POINT cursor_pos = {};
  GetCursorPos(&cursor_pos);

  // Find the monitor containing the cursor
  HMONITOR monitor = MonitorFromPoint(cursor_pos, MONITOR_DEFAULTTONEAREST);

  // Get monitor info (work area excludes taskbar)
  MONITORINFO mi = {};
  mi.cbSize = sizeof(MONITORINFO);
  if (!GetMonitorInfoW(monitor, &mi))
    return;

  // Get the window dimensions
  RECT window_rect = {};
  GetWindowRect(hwnd, &window_rect);
  const int window_width = window_rect.right - window_rect.left;
  const int window_height = window_rect.bottom - window_rect.top;

  // Calculate centered position within the monitor's work area
  const RECT& work_area = mi.rcWork;
  const int window_x = work_area.left + (work_area.right - work_area.left - window_width) / 2;
  const int window_y = work_area.top + (work_area.bottom - work_area.top - window_height) / 2;

  // Move the window
  SetWindowPos(hwnd, nullptr, window_x, window_y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace Win32WindowUtil
