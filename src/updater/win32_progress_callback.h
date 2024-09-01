// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/progress_callback.h"
#include "common/windows_headers.h"

class Win32ProgressCallback final : public ProgressCallback
{
public:
  Win32ProgressCallback();

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
    WINDOW_WIDTH = 600,
    WINDOW_HEIGHT = 300,
    WINDOW_MARGIN = 10,
    SUBWINDOW_WIDTH = WINDOW_WIDTH - 20 - WINDOW_MARGIN - WINDOW_MARGIN,
  };

  bool Create();
  void Destroy();
  void Redraw(bool force);
  void PumpMessages();

  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  HWND m_window_hwnd{};
  HWND m_text_hwnd{};
  HWND m_progress_hwnd{};
  HWND m_list_box_hwnd{};

  int m_last_progress_percent = -1;
};
