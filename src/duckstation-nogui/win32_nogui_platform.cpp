#include "win32_nogui_platform.h"
#include "common/log.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "frontend-common/imgui_manager.h"
#include "nogui_host.h"
#include "resource.h"
#include "win32_key_names.h"
#include <shellapi.h>
#include <tchar.h>
Log_SetChannel(Win32HostInterface);

static constexpr LPCWSTR WINDOW_CLASS_NAME = L"DuckStationNoGUI";
static constexpr DWORD WINDOWED_STYLE = WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU | WS_SIZEBOX;
static constexpr DWORD WINDOWED_EXSTYLE = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
static constexpr DWORD FULLSCREEN_STYLE = WS_POPUP | WS_MINIMIZEBOX;

static float GetWindowScale(HWND hwnd)
{
  static UINT(WINAPI * get_dpi_for_window)(HWND hwnd);
  if (!get_dpi_for_window)
  {
    HMODULE mod = GetModuleHandleW(L"user32.dll");
    if (mod)
      get_dpi_for_window = reinterpret_cast<decltype(get_dpi_for_window)>(GetProcAddress(mod, "GetDpiForWindow"));
  }
  if (!get_dpi_for_window)
    return 1.0f;

  // less than 100% scaling seems unlikely.
  const UINT dpi = hwnd ? get_dpi_for_window(hwnd) : 96;
  return (dpi > 0) ? std::max(1.0f, static_cast<float>(dpi) / 96.0f) : 1.0f;
}

Win32NoGUIPlatform::Win32NoGUIPlatform()
{
  m_message_loop_running.store(true, std::memory_order_release);
}

Win32NoGUIPlatform::~Win32NoGUIPlatform()
{
  UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(nullptr));
}

bool Win32NoGUIPlatform::Initialize()
{
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIconA(wc.hInstance, (LPCSTR)IDI_ICON1);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszMenuName = NULL;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIconA(wc.hInstance, (LPCSTR)IDI_ICON1);

  if (!RegisterClassExW(&wc))
  {
    MessageBoxW(nullptr, L"Window registration failed.", L"Error", MB_ICONERROR | MB_OK);
    return false;
  }

  m_window_thread_id = GetCurrentThreadId();
  return true;
}

void Win32NoGUIPlatform::ReportError(const std::string_view& title, const std::string_view& message)
{
  const std::wstring title_copy(StringUtil::UTF8StringToWideString(title));
  const std::wstring message_copy(StringUtil::UTF8StringToWideString(message));

  MessageBoxW(m_hwnd, message_copy.c_str(), title_copy.c_str(), MB_ICONERROR | MB_OK);
}

bool Win32NoGUIPlatform::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  const std::wstring title_copy(StringUtil::UTF8StringToWideString(title));
  const std::wstring message_copy(StringUtil::UTF8StringToWideString(message));

  return (MessageBoxW(m_hwnd, message_copy.c_str(), title_copy.c_str(), MB_ICONQUESTION | MB_YESNO) == IDYES);
}

void Win32NoGUIPlatform::SetDefaultConfig(SettingsInterface& si)
{
  // noop
}

bool Win32NoGUIPlatform::CreatePlatformWindow(std::string title)
{
  s32 window_x, window_y, window_width, window_height;
  if (!NoGUIHost::GetSavedPlatformWindowGeometry(&window_x, &window_y, &window_width, &window_height))
  {
    window_x = CW_USEDEFAULT;
    window_y = CW_USEDEFAULT;
    window_width = DEFAULT_WINDOW_WIDTH;
    window_height = DEFAULT_WINDOW_HEIGHT;
  }

  HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, StringUtil::UTF8StringToWideString(title).c_str(),
                              WINDOWED_STYLE, window_x, window_y, window_width, window_height, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
  if (!hwnd)
  {
    MessageBoxW(nullptr, L"CreateWindowEx failed.", L"Error", MB_ICONERROR | MB_OK);
    return false;
  }

  // deliberately not stored to m_hwnd yet, because otherwise the msg handlers will run
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  m_hwnd = hwnd;
  m_window_scale = GetWindowScale(m_hwnd);
  m_last_mouse_buttons = 0;

  if (m_fullscreen.load(std::memory_order_acquire))
    SetFullscreen(true);

  return true;
}

void Win32NoGUIPlatform::DestroyPlatformWindow()
{
  if (!m_hwnd)
    return;

  RECT rc;
  if (!m_fullscreen.load(std::memory_order_acquire) && GetWindowRect(m_hwnd, &rc))
  {
    NoGUIHost::SavePlatformWindowGeometry(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
  }

  DestroyWindow(m_hwnd);
  m_hwnd = {};
}

std::optional<WindowInfo> Win32NoGUIPlatform::GetPlatformWindowInfo()
{
  if (!m_hwnd)
    return std::nullopt;

  RECT rc = {};
  GetWindowRect(m_hwnd, &rc);

  WindowInfo wi;
  wi.surface_width = static_cast<u32>(rc.right - rc.left);
  wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
  wi.surface_scale = m_window_scale;
  wi.type = WindowInfo::Type::Win32;
  wi.window_handle = m_hwnd;
  return wi;
}

void Win32NoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
  if (!m_hwnd)
    return;

  SetWindowTextW(m_hwnd, StringUtil::UTF8StringToWideString(title).c_str());
}

void* Win32NoGUIPlatform::GetPlatformWindowHandle()
{
  return m_hwnd;
}

std::optional<u32> Win32NoGUIPlatform::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
  std::optional<DWORD> converted(Win32KeyNames::GetKeyCodeForName(str));
  return converted.has_value() ? std::optional<u32>(static_cast<u32>(converted.value())) : std::nullopt;
}

std::optional<std::string> Win32NoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
  const char* converted = Win32KeyNames::GetKeyName(code);
  return converted ? std::optional<std::string>(converted) : std::nullopt;
}

void Win32NoGUIPlatform::RunMessageLoop()
{
  while (m_message_loop_running.load(std::memory_order_acquire))
  {
    MSG msg;
    if (GetMessageW(&msg, NULL, 0, 0))
    {
      // handle self messages (when we don't have a window yet)
      if (msg.hwnd == NULL && msg.message >= WM_FIRST && msg.message <= WM_LAST)
      {
        WndProc(NULL, msg.message, msg.wParam, msg.lParam);
      }
      else
      {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }
}

void Win32NoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
  std::function<void()>* pfunc = new std::function<void()>(std::move(func));
  if (m_hwnd)
    PostMessageW(m_hwnd, WM_FUNC, 0, reinterpret_cast<LPARAM>(pfunc));
  else
    PostThreadMessageW(m_window_thread_id, WM_FUNC, 0, reinterpret_cast<LPARAM>(pfunc));
}

void Win32NoGUIPlatform::QuitMessageLoop()
{
  m_message_loop_running.store(false, std::memory_order_release);
  PostThreadMessageW(m_window_thread_id, WM_WAKEUP, 0, 0);
}

void Win32NoGUIPlatform::SetFullscreen(bool enabled)
{
  if (!m_hwnd || m_fullscreen.load(std::memory_order_acquire) == enabled)
    return;

  LONG style = GetWindowLong(m_hwnd, GWL_STYLE);
  LONG exstyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
  RECT rc;

  if (enabled)
  {
    HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
      return;

    MONITORINFO mi = {sizeof(MONITORINFO)};
    if (!GetMonitorInfo(monitor, &mi) || !GetWindowRect(m_hwnd, &m_windowed_rect))
      return;

    style = (style & ~WINDOWED_STYLE) | FULLSCREEN_STYLE;
    exstyle = (style & ~WINDOWED_EXSTYLE);
    rc = mi.rcMonitor;
  }
  else
  {
    style = (style & ~FULLSCREEN_STYLE) | WINDOWED_STYLE;
    exstyle = exstyle | WINDOWED_EXSTYLE;
    rc = m_windowed_rect;
  }

  SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
  SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exstyle);
  SetWindowPos(m_hwnd, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW);

  m_fullscreen.store(enabled, std::memory_order_release);
}

bool Win32NoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  RECT rc;
  if (!m_hwnd || m_fullscreen.load(std::memory_order_acquire) || !GetWindowRect(m_hwnd, &rc))
  {
    return false;
  }

  return SetWindowPos(m_hwnd, NULL, rc.left, rc.top, new_window_width, new_window_height, SWP_SHOWWINDOW);
}

bool Win32NoGUIPlatform::OpenURL(const std::string_view& url)
{
  return (ShellExecuteW(nullptr, L"open", StringUtil::UTF8StringToWideString(url).c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL) != NULL);
}

bool Win32NoGUIPlatform::CopyTextToClipboard(const std::string_view& text)
{
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.length()), nullptr, 0);
  if (wlen < 0)
    return false;

  if (!OpenClipboard(m_hwnd))
    return false;

  ScopedGuard clipboard_cleanup([]() { CloseClipboard(); });
  EmptyClipboard();

  const HANDLE hText = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
  if (hText == NULL)
    return false;

  LPWSTR mem = static_cast<LPWSTR>(GlobalLock(hText));
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.length()), mem, wlen);
  mem[wlen] = 0;
  GlobalUnlock(hText);

  SetClipboardData(CF_UNICODETEXT, hText);
  return true;
}

LRESULT CALLBACK Win32NoGUIPlatform::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  Win32NoGUIPlatform* platform = static_cast<Win32NoGUIPlatform*>(g_nogui_window.get());
  if (hwnd != platform->m_hwnd && msg != WM_FUNC)
    return DefWindowProcW(hwnd, msg, wParam, lParam);

  switch (msg)
  {
    case WM_SIZE:
    {
      const u32 width = LOWORD(lParam);
      const u32 height = HIWORD(lParam);
      NoGUIHost::ProcessPlatformWindowResize(width, height, platform->m_window_scale);
    }
    break;

    case WM_KEYDOWN:
    case WM_KEYUP:
    {
      const bool pressed = (msg == WM_KEYDOWN);
      NoGUIHost::ProcessPlatformKeyEvent(static_cast<s32>(wParam), pressed);
    }
    break;

    case WM_CHAR:
    {
      if (ImGuiManager::WantsTextInput())
      {
        const WCHAR utf16[2] = {static_cast<wchar_t>(wParam), 0};
        char utf8[8] = {};
        const int utf8_len =
          WideCharToMultiByte(CP_UTF8, 0, utf16, sizeof(utf16), utf8, sizeof(utf8) - 1, nullptr, nullptr);
        if (utf8_len > 0)
        {
          utf8[utf8_len] = 0;
          NoGUIHost::ProcessPlatformTextEvent(utf8);
        }
      }
    }
    break;

    case WM_MOUSEMOVE:
    {
      const float x = static_cast<float>(static_cast<s16>(LOWORD(lParam)));
      const float y = static_cast<float>(static_cast<s16>(HIWORD(lParam)));
      NoGUIHost::ProcessPlatformMouseMoveEvent(x, y);
    }
    break;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    {
      const DWORD buttons = static_cast<DWORD>(wParam);
      const DWORD changed = platform->m_last_mouse_buttons ^ buttons;
      platform->m_last_mouse_buttons = buttons;

      static constexpr DWORD masks[] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2};
      for (u32 i = 0; i < std::size(masks); i++)
      {
        if (changed & masks[i])
          NoGUIHost::ProcessPlatformMouseButtonEvent(i, (buttons & masks[i]) != 0);
      }
    }
    break;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    {
      const float d =
        std::clamp(static_cast<float>(static_cast<s16>(HIWORD(wParam))) / static_cast<float>(WHEEL_DELTA), -1.0f, 1.0f);
      NoGUIHost::ProcessPlatformMouseWheelEvent((msg == WM_MOUSEHWHEEL) ? d : 0.0f, (msg == WM_MOUSEWHEEL) ? d : 0.0f);
    }
    break;

    case WM_ACTIVATEAPP:
    {
      if (wParam)
        NoGUIHost::PlatformWindowFocusGained();
      else
        NoGUIHost::PlatformWindowFocusLost();
    }
    break;

    case WM_CLOSE:
    case WM_QUIT:
    {
      Host::RunOnCPUThread([]() { Host::RequestExit(g_settings.save_state_on_exit); });
    }
    break;

    case WM_FUNC:
    {
      std::function<void()>* pfunc = reinterpret_cast<std::function<void()>*>(lParam);
      if (pfunc)
      {
        (*pfunc)();
        delete pfunc;
      }
    }
    break;

    case WM_WAKEUP:
      break;

    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  return 0;
}

std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateWin32Platform()
{
  std::unique_ptr<Win32NoGUIPlatform> ret(new Win32NoGUIPlatform());
  if (!ret->Initialize())
    return {};

  return ret;
}