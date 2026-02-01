// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/windows_headers.h"

#include <string>

class InstallerUI
{
public:
  InstallerUI();
  ~InstallerUI();

  bool Execute();

private:
  enum : int
  {
    WINDOW_MARGIN = 20,
    WINDOW_WIDTH = 500,
    WINDOW_HEIGHT = 300,

    LOGO_SIZE = 32,
    HEADING_HEIGHT = 24,
    INFO_TEXT_HEIGHT = 40,
    EDIT_HEIGHT = 24,
    BUTTON_WIDTH = 80,
    BUTTON_HEIGHT = 28,
    CHECKBOX_HEIGHT = 20,
    CONTROL_SPACING = 10,
  };

  enum : int
  {
    IDC_DESTINATION_EDIT = 1001,
    IDC_BROWSE_BUTTON = 1002,
    IDC_START_MENU_CHECKBOX = 1003,
    IDC_DESKTOP_CHECKBOX = 1004,
    IDC_INSTALL_BUTTON = 1005,
    IDC_CANCEL_BUTTON = 1006,
  };

  bool Create();
  void Destroy();

  void OnBrowseClicked();
  void OnInstallClicked();
  void OnCancelClicked();
  void OnDestinationDirectoryChanged();

  bool DoInstall(const std::string& destination_directory_utf8);

  static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  int Scale(int value) const;

  std::wstring GetDefaultInstallDirectory() const;

  HWND m_window_hwnd = nullptr;
  HWND m_logo_hwnd = nullptr;
  HWND m_heading_hwnd = nullptr;
  HWND m_info_text_hwnd = nullptr;
  HWND m_destination_label_hwnd = nullptr;
  HWND m_destination_edit_hwnd = nullptr;
  HWND m_browse_button_hwnd = nullptr;
  HWND m_start_menu_checkbox_hwnd = nullptr;
  HWND m_desktop_checkbox_hwnd = nullptr;
  HWND m_install_button_hwnd = nullptr;
  HWND m_cancel_button_hwnd = nullptr;

  HFONT m_font = nullptr;
  HFONT m_heading_font = nullptr;
  HICON m_logo_icon = nullptr;
  HBRUSH m_background_brush = nullptr;
  UINT m_dpi = 96;

  bool m_running = false;
};
