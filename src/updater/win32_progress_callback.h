// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "updater_progress_callback.h"

#include "common/windows_headers.h"

class Win32ProgressCallback final : public UpdaterProgressCallback
{
public:
  Win32ProgressCallback();
  ~Win32ProgressCallback() override;

  void PushState() override;
  void PopState() override;

  void SetCancellable(bool cancellable) override;
  void SetTitle(const std::string_view title) override;
  void SetStatusText(const std::string_view text) override;
  void SetProgressRange(u32 range) override;
  void SetProgressValue(u32 value) override;

  void DisplayError(const std::string_view message) override;
  void DisplayWarning(const std::string_view message) override;
  void DisplayInformation(const std::string_view message) override;
  void DisplayDebugMessage(const std::string_view message) override;

  void ModalError(const std::string_view message) override;
  bool ModalConfirmation(const std::string_view message) override;
  void ModalInformation(const std::string_view message) override;

private:
  enum : int
  {
    WINDOW_MARGIN = 10,
    STATUS_TEXT_HEIGHT = 16,
    PROGRESS_BAR_HEIGHT = 20,
    LIST_BOX_HEIGHT = 170,
    CONTROL_SPACING = 10,
    WINDOW_WIDTH = 600,
    WINDOW_HEIGHT = WINDOW_MARGIN * 2 + STATUS_TEXT_HEIGHT + CONTROL_SPACING + PROGRESS_BAR_HEIGHT + CONTROL_SPACING +
                    LIST_BOX_HEIGHT,
  };

  bool Create();
  void Destroy();
  void Redraw(bool force);
  void PumpMessages();

  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  int Scale(int value) const;

  HWND m_window_hwnd = nullptr;
  HWND m_text_hwnd = nullptr;
  HWND m_progress_hwnd = nullptr;
  HWND m_list_box_hwnd = nullptr;

  HFONT m_font = nullptr;
  UINT m_dpi = 96;

  int m_last_progress_percent = -1;
};
