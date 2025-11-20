// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "win32_progress_callback.h"
#include "resource.h"

#include "common/log.h"
#include "common/string_util.h"

#include <CommCtrl.h>
#include <shellscalingapi.h>

LOG_CHANNEL(Host);

Win32ProgressCallback::Win32ProgressCallback() : ProgressCallback()
{
  Create();
}

Win32ProgressCallback::~Win32ProgressCallback()
{
  Destroy();
}

void Win32ProgressCallback::PushState()
{
  ProgressCallback::PushState();
}

void Win32ProgressCallback::PopState()
{
  ProgressCallback::PopState();
  Redraw(true);
}

void Win32ProgressCallback::SetCancellable(bool cancellable)
{
  ProgressCallback::SetCancellable(cancellable);
  Redraw(true);
}

void Win32ProgressCallback::SetTitle(const std::string_view title)
{
  SetWindowText(m_window_hwnd, StringUtil::UTF8StringToWideString(title).c_str());
}

void Win32ProgressCallback::SetStatusText(const std::string_view text)
{
  ProgressCallback::SetStatusText(text);
  Redraw(true);
}

void Win32ProgressCallback::SetProgressRange(u32 range)
{
  ProgressCallback::SetProgressRange(range);
  Redraw(false);
}

void Win32ProgressCallback::SetProgressValue(u32 value)
{
  ProgressCallback::SetProgressValue(value);
  Redraw(false);
}

bool Win32ProgressCallback::Create()
{
  static constexpr LPCWSTR CLASS_NAME = L"DSWin32ProgressCallbackWindow";
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
    wc.hCursor = LoadCursor(NULL, IDC_WAIT);
    wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassEx(&wc))
    {
      ERROR_LOG("Failed to register window class");
      return false;
    }

    class_registered = true;
  }

  m_dpi = GetDpiForSystem();

  RECT adjusted_rect = {0, 0, Scale(WINDOW_WIDTH), Scale(WINDOW_HEIGHT)};
  AdjustWindowRectExForDpi(&adjusted_rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CLIENTEDGE, m_dpi);

  m_window_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, CLASS_NAME, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 adjusted_rect.right - adjusted_rect.left, adjusted_rect.bottom - adjusted_rect.top,
                                 nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_window_hwnd)
  {
    ERROR_LOG("Failed to create window");
    return false;
  }

  SetWindowLongPtr(m_window_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ShowWindow(m_window_hwnd, SW_SHOW);
  PumpMessages();
  return true;
}

void Win32ProgressCallback::Destroy()
{
  if (!m_window_hwnd)
    return;

  if (m_font)
  {
    DeleteObject(m_font);
    m_font = nullptr;
  }

  DestroyWindow(m_window_hwnd);
  m_window_hwnd = {};
  m_text_hwnd = {};
  m_progress_hwnd = {};
}

void Win32ProgressCallback::PumpMessages()
{
  MSG msg;
  while (PeekMessage(&msg, m_window_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void Win32ProgressCallback::Redraw(bool force)
{
  const int percent =
    static_cast<int>((static_cast<float>(m_progress_value) / static_cast<float>(m_progress_range)) * 100.0f);
  if (percent == m_last_progress_percent && !force)
  {
    PumpMessages();
    return;
  }

  m_last_progress_percent = percent;

  SendMessage(m_progress_hwnd, PBM_SETRANGE, 0, MAKELPARAM(0, m_progress_range));
  SendMessage(m_progress_hwnd, PBM_SETPOS, static_cast<WPARAM>(m_progress_value), 0);
  SetWindowText(m_text_hwnd, StringUtil::UTF8StringToWideString(m_status_text).c_str());
  RedrawWindow(m_text_hwnd, nullptr, nullptr, RDW_INVALIDATE);
  PumpMessages();
}

LRESULT CALLBACK Win32ProgressCallback::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  Win32ProgressCallback* cb;
  if (msg == WM_CREATE)
  {
    const CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
    cb = static_cast<Win32ProgressCallback*>(cs->lpCreateParams);
  }
  else
  {
    cb = reinterpret_cast<Win32ProgressCallback*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
  }

  return cb->WndProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK Win32ProgressCallback::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg)
  {
    case WM_CREATE:
    {
      const CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
      m_dpi = GetDpiForWindow(hwnd);
      EnableMenuItem(GetSystemMenu(hwnd, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);

      LOGFONT lf = {};
      SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0, m_dpi);
      m_font = CreateFontIndirect(&lf);

      SendMessage(hwnd, WM_SETFONT, WPARAM(m_font), TRUE);

      m_text_hwnd =
        CreateWindowEx(0, L"Static", nullptr, WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, nullptr, cs->hInstance, nullptr);
      SendMessage(m_text_hwnd, WM_SETFONT, WPARAM(m_font), TRUE);

      m_progress_hwnd = CreateWindowEx(0, PROGRESS_CLASSW, nullptr, WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, nullptr,
                                       cs->hInstance, nullptr);

      m_list_box_hwnd =
        CreateWindowEx(0, L"LISTBOX", nullptr, WS_VISIBLE | WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_BORDER | LBS_NOSEL,
                       0, 0, 0, 0, hwnd, nullptr, cs->hInstance, nullptr);
      SendMessage(m_list_box_hwnd, WM_SETFONT, WPARAM(m_font), TRUE);
    }
      [[fallthrough]];

    case WM_SIZE:
    {
      RECT window_rect = {};
      GetClientRect(m_window_hwnd, &window_rect);

      const int control_width = (window_rect.right - window_rect.left) - (Scale(WINDOW_MARGIN) * 2);
      int y = Scale(WINDOW_MARGIN);

      SetWindowPos(m_text_hwnd, nullptr, Scale(WINDOW_MARGIN), y, control_width, Scale(STATUS_TEXT_HEIGHT),
                   SWP_NOZORDER | SWP_NOACTIVATE);
      y += Scale(STATUS_TEXT_HEIGHT) + Scale(CONTROL_SPACING);

      SetWindowPos(m_progress_hwnd, nullptr, Scale(WINDOW_MARGIN), y, control_width, Scale(PROGRESS_BAR_HEIGHT),
                   SWP_NOZORDER | SWP_NOACTIVATE);
      y += Scale(PROGRESS_BAR_HEIGHT) + Scale(CONTROL_SPACING);

      const int listbox_height = (window_rect.bottom - window_rect.top) - y - Scale(WINDOW_MARGIN);
      SetWindowPos(m_list_box_hwnd, nullptr, Scale(WINDOW_MARGIN), y, control_width, listbox_height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    break;

    case WM_DPICHANGED:
    {
      m_dpi = HIWORD(wparam);

      // Need to update the font.
      if (m_font)
      {
        DeleteObject(m_font);
        m_font = nullptr;
      }

      LOGFONTW lf = {};
      SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0, m_dpi);
      m_font = CreateFontIndirect(&lf);
      SendMessage(m_window_hwnd, WM_SETFONT, WPARAM(m_font), TRUE);
      SendMessage(m_text_hwnd, WM_SETFONT, WPARAM(m_font), TRUE);
      SendMessage(m_progress_hwnd, WM_SETFONT, WPARAM(m_font), TRUE);
      SendMessage(m_list_box_hwnd, WM_SETFONT, WPARAM(m_font), TRUE);

      // Will trigger WM_SIZE.
      const RECT* new_rect = reinterpret_cast<RECT*>(lparam);
      SetWindowPos(m_window_hwnd, nullptr, new_rect->left, new_rect->top, new_rect->right - new_rect->left,
                   new_rect->bottom - new_rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    break;

    default:
      return DefWindowProc(hwnd, msg, wparam, lparam);
  }

  return 0;
}

int Win32ProgressCallback::Scale(int value) const
{
  return MulDiv(value, m_dpi, 96);
}

void Win32ProgressCallback::DisplayError(const std::string_view message)
{
  ERROR_LOG(message);
  SendMessage(m_list_box_hwnd, LB_ADDSTRING, 0,
              reinterpret_cast<LPARAM>(StringUtil::UTF8StringToWideString(message).c_str()));
  SendMessage(m_list_box_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
  PumpMessages();
}

void Win32ProgressCallback::DisplayWarning(const std::string_view message)
{
  WARNING_LOG(message);
  SendMessage(m_list_box_hwnd, LB_ADDSTRING, 0,
              reinterpret_cast<LPARAM>(StringUtil::UTF8StringToWideString(message).c_str()));
  SendMessage(m_list_box_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
  PumpMessages();
}

void Win32ProgressCallback::DisplayInformation(const std::string_view message)
{
  INFO_LOG(message);
  SendMessage(m_list_box_hwnd, LB_ADDSTRING, 0,
              reinterpret_cast<LPARAM>(StringUtil::UTF8StringToWideString(message).c_str()));
  SendMessage(m_list_box_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
  PumpMessages();
}

void Win32ProgressCallback::DisplayDebugMessage(const std::string_view message)
{
  DEV_LOG(message);
}

void Win32ProgressCallback::ModalError(const std::string_view message)
{
  PumpMessages();
  MessageBox(m_window_hwnd, StringUtil::UTF8StringToWideString(message).c_str(), L"Error", MB_ICONERROR | MB_OK);
  PumpMessages();
}

bool Win32ProgressCallback::ModalConfirmation(const std::string_view message)
{
  PumpMessages();
  bool result = MessageBox(m_window_hwnd, StringUtil::UTF8StringToWideString(message).c_str(), L"Confirmation",
                           MB_ICONQUESTION | MB_YESNO) == IDYES;
  PumpMessages();
  return result;
}

void Win32ProgressCallback::ModalInformation(const std::string_view message)
{
  MessageBox(m_window_hwnd, StringUtil::UTF8StringToWideString(message).c_str(), L"Information",
             MB_ICONINFORMATION | MB_OK);
}
