// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "installer_ui.h"
#include "installer.h"
#include "resource.h"

#include "updater/win32_progress_callback.h"
#include "updater/win32_window_util.h"

#include "common/error.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"

#include <CommCtrl.h>
#include <ShlObj.h>
#include <combaseapi.h>
#include <shellscalingapi.h>

static constexpr const wchar_t* MSGBOX_TITLE = L"DuckStation Installer";

InstallerUI::InstallerUI() = default;

InstallerUI::~InstallerUI()
{
  Destroy();
}

std::wstring InstallerUI::GetDefaultInstallDirectory() const
{
  std::wstring result;

  PWSTR local_app_data_path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data_path)))
  {
    result = local_app_data_path;
    result += L"\\Programs\\DuckStation";
    CoTaskMemFree(local_app_data_path);
  }
  else
  {
    result = L"C:\\Program Files\\DuckStation";
  }

  return result;
}

bool InstallerUI::Execute()
{
  if (!Create())
    return false;

  m_running = true;

  MSG msg;
  while (m_running && GetMessageW(&msg, nullptr, 0, 0))
  {
    if (!IsDialogMessageW(m_window_hwnd, &msg))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  return true;
}

bool InstallerUI::Create()
{
  static constexpr LPCWSTR CLASS_NAME = L"DSInstallerUIWindow";
  static bool class_registered = false;

  if (!class_registered)
  {
    InitCommonControls();

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hIconSm = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassEx(&wc))
    {
      MessageBoxW(nullptr, L"Failed to register window class", MSGBOX_TITLE, MB_ICONERROR | MB_OK);
      return false;
    }

    class_registered = true;
  }

  m_dpi = GetDpiForSystem();

  const DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT adjusted_rect = {0, 0, Scale(WINDOW_WIDTH), Scale(WINDOW_HEIGHT)};
  AdjustWindowRectExForDpi(&adjusted_rect, window_style, FALSE, 0, m_dpi);

  const int window_width = adjusted_rect.right - adjusted_rect.left;
  const int window_height = adjusted_rect.bottom - adjusted_rect.top;

  m_window_hwnd = CreateWindowExW(0, CLASS_NAME, L"DuckStation Installer", window_style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  window_width, window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_window_hwnd)
  {
    MessageBoxW(nullptr, L"Failed to create window", MSGBOX_TITLE, MB_ICONERROR | MB_OK);
    return false;
  }

  SetWindowLongPtr(m_window_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  Win32WindowUtil::CenterWindowOnMonitorAtCursorPosition(m_window_hwnd);
  ShowWindow(m_window_hwnd, SW_SHOW);
  UpdateWindow(m_window_hwnd);
  return true;
}

void InstallerUI::Destroy()
{
  if (m_heading_font)
  {
    DeleteObject(m_heading_font);
    m_heading_font = nullptr;
  }

  if (m_font)
  {
    DeleteObject(m_font);
    m_font = nullptr;
  }

  if (m_logo_icon)
  {
    DestroyIcon(m_logo_icon);
    m_logo_icon = nullptr;
  }

  if (m_background_brush)
  {
    DeleteObject(m_background_brush);
    m_background_brush = nullptr;
  }

  if (m_window_hwnd)
  {
    DestroyWindow(m_window_hwnd);
    m_window_hwnd = nullptr;
  }
}

void InstallerUI::OnBrowseClicked()
{
  BROWSEINFOW bi = {};
  bi.hwndOwner = m_window_hwnd;
  bi.lpszTitle = L"Select Installation Directory";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;

  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (pidl)
  {
    WCHAR path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path))
    {
      std::wstring destination_directory = path;
      destination_directory += L"\\DuckStation";
      SetWindowTextW(m_destination_edit_hwnd, destination_directory.c_str());
    }
    CoTaskMemFree(pidl);
  }
}

void InstallerUI::OnInstallClicked()
{
  std::wstring destination_directory;

  // Get the current text from the edit control
  const int text_len = GetWindowTextLengthW(m_destination_edit_hwnd);
  if (text_len > 0)
  {
    destination_directory.resize(static_cast<size_t>(text_len) + 1);
    GetWindowTextW(m_destination_edit_hwnd, destination_directory.data(), text_len + 1);
    destination_directory.resize(static_cast<size_t>(text_len));
  }

  if (destination_directory.empty())
    return;

  // Check if the directory is not empty and warn the user
  const std::string destination_directory_utf8 = StringUtil::WideStringToUTF8String(destination_directory);
  if (!Installer::CheckForEmptyDirectory(destination_directory_utf8))
  {
    if (MessageBoxW(m_window_hwnd,
                    L"The selected directory is not empty. Files may be overwritten.\n\nDo you want to continue?",
                    MSGBOX_TITLE, MB_ICONWARNING | MB_YESNO) == IDNO)
    {
      return;
    }
  }

  if (!DoInstall(destination_directory_utf8))
  {
    // allow user to try again
    return;
  }

  // Ask user if they want to launch the application
  if (MessageBoxW(m_window_hwnd, L"Do you want to launch DuckStation now?", MSGBOX_TITLE, MB_ICONQUESTION | MB_YESNO) ==
      IDYES)
  {
    Error error;
    if (!Installer::LaunchApplication(destination_directory_utf8, &error))
    {
      MessageBoxW(m_window_hwnd, StringUtil::UTF8StringToWideString(error.GetDescription()).c_str(), MSGBOX_TITLE,
                  MB_ICONERROR | MB_OK);
    }
  }

  m_running = false;
}

bool InstallerUI::DoInstall(const std::string& destination_directory_utf8)
{
  // Get checkbox states
  const bool create_start_menu_shortcut = (SendMessage(m_start_menu_checkbox_hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
  const bool create_desktop_shortcut = (SendMessage(m_desktop_checkbox_hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);

  Win32ProgressCallback progress(m_window_hwnd);
  Installer installer(&progress, destination_directory_utf8);
  if (!installer.Install())
    return false;

  // Create shortcuts if requested
  if (create_start_menu_shortcut)
    installer.CreateStartMenuShortcut();
  if (create_desktop_shortcut)
    installer.CreateDesktopShortcut();

  progress.ModalInformation("Installation completed successfully!");
  return true;
}

void InstallerUI::OnCancelClicked()
{
  m_running = false;
}

void InstallerUI::OnDestinationDirectoryChanged()
{
  const int text_len = GetWindowTextLengthW(m_destination_edit_hwnd);
  EnableWindow(m_install_button_hwnd, text_len > 0);
}

LRESULT CALLBACK InstallerUI::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  InstallerUI* ui;
  if (msg == WM_CREATE)
  {
    const CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
    ui = static_cast<InstallerUI*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
  }
  else
  {
    ui = reinterpret_cast<InstallerUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  if (ui)
    return ui->WndProc(hwnd, msg, wparam, lparam);
  else
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK InstallerUI::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg)
  {
    case WM_CREATE:
    {
      const CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
      m_dpi = GetDpiForWindow(hwnd);

      // Create background brush for consistent control backgrounds
      m_background_brush = CreateSolidBrush(GetSysColor(COLOR_3DFACE));

      // Create fonts
      LOGFONTW lf = {};
      SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0, m_dpi);
      m_font = CreateFontIndirectW(&lf);

      // Create a larger font for the heading
      lf.lfHeight = -MulDiv(16, m_dpi, 72);
      lf.lfWeight = FW_BOLD;
      m_heading_font = CreateFontIndirectW(&lf);

      // Load the logo icon
      m_logo_icon = reinterpret_cast<HICON>(
        LoadImageW(cs->hInstance, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, Scale(LOGO_SIZE), Scale(LOGO_SIZE), 0));

      // Create logo static control
      m_logo_hwnd = CreateWindowExW(0, L"Static", nullptr, WS_VISIBLE | WS_CHILD | SS_ICON, 0, 0, 0, 0, hwnd, nullptr,
                                    cs->hInstance, nullptr);
      SendMessageW(m_logo_hwnd, STM_SETICON, reinterpret_cast<WPARAM>(m_logo_icon), 0);

      // Create heading label
      m_heading_hwnd = CreateWindowExW(0, L"Static", L"DuckStation Installer", WS_VISIBLE | WS_CHILD | SS_LEFT, 0, 0, 0,
                                       0, hwnd, nullptr, cs->hInstance, nullptr);
      SendMessageW(m_heading_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_heading_font), TRUE);

      // Create info text
      m_info_text_hwnd =
        CreateWindowExW(0, L"Static",
                        L"This program will install DuckStation on your computer. Choose the destination folder and "
                        L"click Install to continue.",
                        WS_VISIBLE | WS_CHILD | SS_LEFT, 0, 0, 0, 0, hwnd, nullptr, cs->hInstance, nullptr);
      SendMessageW(m_info_text_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

      // Create destination label
      m_destination_label_hwnd =
        CreateWindowExW(0, L"Static", L"Installation Directory:", WS_VISIBLE | WS_CHILD | SS_LEFT, 0, 0, 0, 0, hwnd,
                        nullptr, cs->hInstance, nullptr);
      SendMessageW(m_destination_label_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

      // Create destination edit control
      m_destination_edit_hwnd =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", GetDefaultInstallDirectory().c_str(),
                        WS_VISIBLE | WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DESTINATION_EDIT)), cs->hInstance, nullptr);
      SendMessageW(m_destination_edit_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

      // Create browse button
      m_browse_button_hwnd =
        CreateWindowExW(0, L"Button", L"Browse...", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE_BUTTON)), cs->hInstance, nullptr);
      SendMessageW(m_browse_button_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

      // Create start menu checkbox
      m_start_menu_checkbox_hwnd = CreateWindowExW(
        0, L"Button", L"Create Start Menu shortcut", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_START_MENU_CHECKBOX)), cs->hInstance, nullptr);
      SendMessageW(m_start_menu_checkbox_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_start_menu_checkbox_hwnd, BM_SETCHECK, BST_CHECKED, 0);

      // Create desktop checkbox
      m_desktop_checkbox_hwnd = CreateWindowExW(
        0, L"Button", L"Create Desktop shortcut", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DESKTOP_CHECKBOX)), cs->hInstance, nullptr);
      SendMessageW(m_desktop_checkbox_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_desktop_checkbox_hwnd, BM_SETCHECK, BST_UNCHECKED, 0);

      // Create install button
      m_install_button_hwnd = CreateWindowExW(
        0, L"Button", L"Install", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INSTALL_BUTTON)), cs->hInstance, nullptr);
      SendMessageW(m_install_button_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

      // Create cancel button
      m_cancel_button_hwnd =
        CreateWindowExW(0, L"Button", L"Cancel", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CANCEL_BUTTON)), cs->hInstance, nullptr);
      SendMessageW(m_cancel_button_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
    }
      [[fallthrough]];

    case WM_SIZE:
    {
      RECT client_rect = {};
      GetClientRect(hwnd, &client_rect);

      const int client_width = client_rect.right - client_rect.left;
      const int margin = Scale(WINDOW_MARGIN);
      const int control_spacing = Scale(CONTROL_SPACING);
      const int content_width = client_width - (margin * 2);

      int y = margin;

      // Logo and heading on the same row
      const int logo_size = Scale(LOGO_SIZE);
      SetWindowPos(m_logo_hwnd, nullptr, margin, y, logo_size, logo_size, SWP_NOZORDER | SWP_NOACTIVATE);

      const int heading_x = margin + logo_size + control_spacing;
      const int heading_width = content_width - logo_size - control_spacing;
      const int heading_height = Scale(HEADING_HEIGHT);
      SetWindowPos(m_heading_hwnd, nullptr, heading_x, y + (logo_size - heading_height) / 2, heading_width,
                   heading_height, SWP_NOZORDER | SWP_NOACTIVATE);

      y += logo_size + control_spacing;

      // Info text
      const int info_height = Scale(INFO_TEXT_HEIGHT);
      SetWindowPos(m_info_text_hwnd, nullptr, margin, y, content_width, info_height, SWP_NOZORDER | SWP_NOACTIVATE);
      y += info_height + control_spacing;

      // Destination label
      const int label_height = Scale(16);
      SetWindowPos(m_destination_label_hwnd, nullptr, margin, y, content_width, label_height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      y += label_height + control_spacing / 2;

      // Destination edit and browse button
      const int edit_height = Scale(EDIT_HEIGHT);
      const int browse_width = Scale(BUTTON_WIDTH);
      const int edit_width = content_width - browse_width - control_spacing;
      SetWindowPos(m_destination_edit_hwnd, nullptr, margin, y, edit_width, edit_height, SWP_NOZORDER | SWP_NOACTIVATE);
      SetWindowPos(m_browse_button_hwnd, nullptr, margin + edit_width + control_spacing, y, browse_width, edit_height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      y += edit_height + control_spacing;

      // Checkboxes
      const int checkbox_height = Scale(CHECKBOX_HEIGHT);
      SetWindowPos(m_start_menu_checkbox_hwnd, nullptr, margin, y, content_width, checkbox_height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      y += checkbox_height + control_spacing / 2;

      SetWindowPos(m_desktop_checkbox_hwnd, nullptr, margin, y, content_width, checkbox_height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      y += checkbox_height + control_spacing;

      // Install and Cancel buttons at the bottom right
      const int button_width = Scale(BUTTON_WIDTH);
      const int button_height = Scale(BUTTON_HEIGHT);
      const int button_y = client_rect.bottom - margin - button_height;
      const int cancel_x = client_width - margin - button_width;
      const int install_x = cancel_x - control_spacing - button_width;

      SetWindowPos(m_install_button_hwnd, nullptr, install_x, button_y, button_width, button_height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      SetWindowPos(m_cancel_button_hwnd, nullptr, cancel_x, button_y, button_width, button_height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    break;

    case WM_CTLCOLORSTATIC:
    {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      SetBkColor(hdc, GetSysColor(COLOR_3DFACE));
      return reinterpret_cast<LRESULT>(m_background_brush);
    }

    case WM_COMMAND:
    {
      const int control_id = LOWORD(wparam);
      const int notification = HIWORD(wparam);

      if (control_id == IDCANCEL)
      {
        OnCancelClicked();
      }
      else if (notification == BN_CLICKED)
      {
        switch (control_id)
        {
          case IDC_BROWSE_BUTTON:
            OnBrowseClicked();
            break;
          case IDC_INSTALL_BUTTON:
            OnInstallClicked();
            break;
          case IDC_CANCEL_BUTTON:
            OnCancelClicked();
            break;
        }
      }
      else if (control_id == IDC_DESTINATION_EDIT && notification == EN_CHANGE)
      {
        OnDestinationDirectoryChanged();
      }
    }
    break;

    case DM_GETDEFID:
      return MAKELRESULT(IDC_INSTALL_BUTTON, DC_HASDEFID);

    case WM_CLOSE:
      OnCancelClicked();
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_DPICHANGED:
    {
      m_dpi = HIWORD(wparam);

      // Recreate fonts
      if (m_font)
      {
        DeleteObject(m_font);
        m_font = nullptr;
      }
      if (m_heading_font)
      {
        DeleteObject(m_heading_font);
        m_heading_font = nullptr;
      }

      LOGFONTW lf = {};
      SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0, m_dpi);
      m_font = CreateFontIndirectW(&lf);

      lf.lfHeight = -MulDiv(16, m_dpi, 72);
      lf.lfWeight = FW_BOLD;
      m_heading_font = CreateFontIndirectW(&lf);

      // Update fonts on controls
      SendMessageW(m_heading_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_heading_font), TRUE);
      SendMessageW(m_info_text_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_destination_label_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_destination_edit_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_browse_button_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_start_menu_checkbox_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_desktop_checkbox_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_install_button_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
      SendMessageW(m_cancel_button_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

      // Reload icon at new size
      if (m_logo_icon)
      {
        DestroyIcon(m_logo_icon);
        m_logo_icon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDI_ICON1),
                                                         IMAGE_ICON, Scale(LOGO_SIZE), Scale(LOGO_SIZE), 0));
        SendMessageW(m_logo_hwnd, STM_SETICON, reinterpret_cast<WPARAM>(m_logo_icon), 0);
      }

      // Resize window
      const RECT* new_rect = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(hwnd, nullptr, new_rect->left, new_rect->top, new_rect->right - new_rect->left,
                   new_rect->bottom - new_rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    break;

    default:
      return DefWindowProc(hwnd, msg, wparam, lparam);
  }

  return 0;
}

int InstallerUI::Scale(int value) const
{
  return MulDiv(value, m_dpi, 96);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
  // Shell dialogs (SHBrowseForFolder) require single-threaded apartment mode
  const bool com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  const ScopedGuard com_guard = [com_initialized]() {
    if (com_initialized)
      CoUninitialize();
  };

  InstallerUI ui;
  return ui.Execute() ? EXIT_SUCCESS : EXIT_FAILURE;
}
