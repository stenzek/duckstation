#include "win32_host_interface.h"
#include "common/log.h"
#include "common/string_util.h"
#include "resource.h"
#include <tchar.h>
Log_SetChannel(Win32HostInterface);

static constexpr LPCWSTR WINDOW_CLASS_NAME = L"DuckStationNoGUI";

Win32HostInterface::Win32HostInterface() = default;

Win32HostInterface::~Win32HostInterface() = default;

std::unique_ptr<NoGUIHostInterface> Win32HostInterface::Create()
{
  return std::make_unique<Win32HostInterface>();
}

bool Win32HostInterface::Initialize()
{
  if (!RegisterWindowClass())
    return false;

  if (!NoGUIHostInterface::Initialize())
    return false;

  return true;
}

void Win32HostInterface::Shutdown()
{
  NoGUIHostInterface::Shutdown();
}

bool Win32HostInterface::RegisterWindowClass()
{
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIconA(NULL, (LPCSTR)IDI_ICON1);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszMenuName = NULL;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIconA(NULL, (LPCSTR)IDI_ICON1);

  if (!RegisterClassExW(&wc))
  {
    MessageBoxA(nullptr, "Window registration failed.", "Error", MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

void Win32HostInterface::SetMouseMode(bool relative, bool hide_cursor) {}

bool Win32HostInterface::CreatePlatformWindow()
{
  m_hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, L"DuckStation", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT, nullptr, nullptr,
                           GetModuleHandleA(nullptr), this);
  if (!m_hwnd)
  {
    MessageBoxA(nullptr, "CreateWindowEx failed.", "Error", MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  ProcessWin32Events();

  return true;
}

void Win32HostInterface::DestroyPlatformWindow()
{
  if (m_hwnd)
  {
    DestroyWindow(m_hwnd);
    m_hwnd = {};
  }
}

std::optional<WindowInfo> Win32HostInterface::GetPlatformWindowInfo()
{
  RECT rc = {};
  GetClientRect(m_hwnd, &rc);

  WindowInfo wi;
  wi.window_handle = static_cast<void*>(m_hwnd);
  wi.type = WindowInfo::Type::Win32;
  wi.surface_width = static_cast<u32>(rc.right - rc.left);
  wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
  // wi.surface_format = WindowInfo::SurfaceFormat::Auto;
  return wi;
}

void Win32HostInterface::PollAndUpdate()
{
  ProcessWin32Events();

  NoGUIHostInterface::PollAndUpdate();
}

void Win32HostInterface::ProcessWin32Events()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

LRESULT Win32HostInterface::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  Win32HostInterface* hi = static_cast<Win32HostInterface*>(g_host_interface);
  switch (msg)
  {
    case WM_SIZE:
    {
      const u32 width = LOWORD(lParam);
      const u32 height = HIWORD(lParam);
      if (hi->m_display)
        hi->m_display->ResizeRenderWindow(static_cast<s32>(width), static_cast<s32>(height));
    }
    break;

    case WM_CLOSE:
      hi->m_quit_request = true;
      break;

    default:
      return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}

std::optional<Win32HostInterface::HostKeyCode> Win32HostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  std::optional<int> kc; // = EvDevKeyNames::GetKeyCodeForName(key_code);
  if (!kc.has_value())
    return std::nullopt;

  return static_cast<HostKeyCode>(kc.value());
}
