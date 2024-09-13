// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "win32_progress_callback.h"

#include "common/log.h"
#include "common/string_util.h"

#include <CommCtrl.h>

Log_SetChannel(Win32ProgressCallback);

Win32ProgressCallback::Win32ProgressCallback() : ProgressCallback()
{
  Create();
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
  SetWindowTextW(m_window_hwnd, StringUtil::UTF8StringToWideString(title).c_str());
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
  static const char* CLASS_NAME = "DSWin32ProgressCallbackWindow";
  static bool class_registered = false;

  if (!class_registered)
  {
    InitCommonControls();

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = GetModuleHandle(nullptr);
    // wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON));
    // wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON));
    wc.hCursor = LoadCursor(NULL, IDC_WAIT);
    wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassExA(&wc))
    {
      ERROR_LOG("Failed to register window class");
      return false;
    }

    class_registered = true;
  }

  m_window_hwnd =
    CreateWindowExA(WS_EX_CLIENTEDGE, CLASS_NAME, "Win32ProgressCallback", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                    CW_USEDEFAULT, WINDOW_WIDTH, WINDOW_HEIGHT, nullptr, nullptr, GetModuleHandle(nullptr), this);
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

  DestroyWindow(m_window_hwnd);
  m_window_hwnd = {};
  m_text_hwnd = {};
  m_progress_hwnd = {};
}

void Win32ProgressCallback::PumpMessages()
{
  MSG msg;
  while (PeekMessageA(&msg, m_window_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
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

  SendMessageA(m_progress_hwnd, PBM_SETRANGE, 0, MAKELPARAM(0, m_progress_range));
  SendMessageA(m_progress_hwnd, PBM_SETPOS, static_cast<WPARAM>(m_progress_value), 0);
  SetWindowTextA(m_text_hwnd, m_status_text.c_str());
  RedrawWindow(m_text_hwnd, nullptr, nullptr, RDW_INVALIDATE);
  PumpMessages();
}

LRESULT CALLBACK Win32ProgressCallback::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  Win32ProgressCallback* cb;
  if (msg == WM_CREATE)
  {
    const CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lparam);
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
      const CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lparam);
      HFONT default_font = reinterpret_cast<HFONT>(GetStockObject(ANSI_VAR_FONT));
      SendMessageA(hwnd, WM_SETFONT, WPARAM(default_font), TRUE);

      int y = WINDOW_MARGIN;

      m_text_hwnd = CreateWindowExA(0, "Static", nullptr, WS_VISIBLE | WS_CHILD, WINDOW_MARGIN, y, SUBWINDOW_WIDTH, 16,
                                    hwnd, nullptr, cs->hInstance, nullptr);
      SendMessageA(m_text_hwnd, WM_SETFONT, WPARAM(default_font), TRUE);
      y += 16 + WINDOW_MARGIN;

      m_progress_hwnd = CreateWindowExA(0, PROGRESS_CLASSA, nullptr, WS_VISIBLE | WS_CHILD, WINDOW_MARGIN, y,
                                        SUBWINDOW_WIDTH, 32, hwnd, nullptr, cs->hInstance, nullptr);
      y += 32 + WINDOW_MARGIN;

      m_list_box_hwnd =
        CreateWindowExA(0, "LISTBOX", nullptr, WS_VISIBLE | WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_BORDER | LBS_NOSEL,
                        WINDOW_MARGIN, y, SUBWINDOW_WIDTH, 170, hwnd, nullptr, cs->hInstance, nullptr);
      SendMessageA(m_list_box_hwnd, WM_SETFONT, WPARAM(default_font), TRUE);
      y += 170;
    }
    break;

    default:
      return DefWindowProcA(hwnd, msg, wparam, lparam);
  }

  return 0;
}

void Win32ProgressCallback::DisplayError(const std::string_view message)
{
  ERROR_LOG(message);
  SendMessageW(m_list_box_hwnd, LB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(StringUtil::UTF8StringToWideString(message).c_str()));
  SendMessageW(m_list_box_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
  PumpMessages();
}

void Win32ProgressCallback::DisplayWarning(const std::string_view message)
{
  WARNING_LOG(message);
  SendMessageW(m_list_box_hwnd, LB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(StringUtil::UTF8StringToWideString(message).c_str()));
  SendMessageW(m_list_box_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
  PumpMessages();
}

void Win32ProgressCallback::DisplayInformation(const std::string_view message)
{
  INFO_LOG(message);
  SendMessageW(m_list_box_hwnd, LB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(StringUtil::UTF8StringToWideString(message).c_str()));
  SendMessageW(m_list_box_hwnd, WM_VSCROLL, SB_BOTTOM, 0);
  PumpMessages();
}

void Win32ProgressCallback::DisplayDebugMessage(const std::string_view message)
{
  DEV_LOG(message);
}

void Win32ProgressCallback::ModalError(const std::string_view message)
{
  PumpMessages();
  MessageBoxW(m_window_hwnd, StringUtil::UTF8StringToWideString(message).c_str(), L"Error", MB_ICONERROR | MB_OK);
  PumpMessages();
}

bool Win32ProgressCallback::ModalConfirmation(const std::string_view message)
{
  PumpMessages();
  bool result = MessageBoxW(m_window_hwnd, StringUtil::UTF8StringToWideString(message).c_str(), L"Confirmation",
                            MB_ICONQUESTION | MB_YESNO) == IDYES;
  PumpMessages();
  return result;
}

void Win32ProgressCallback::ModalInformation(const std::string_view message)
{
  MessageBoxW(m_window_hwnd, StringUtil::UTF8StringToWideString(message).c_str(), L"Information",
              MB_ICONINFORMATION | MB_OK);
}
