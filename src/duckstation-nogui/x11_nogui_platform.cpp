// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "x11_nogui_platform.h"

Log_SetChannel(X11NoGUIPlatform);

X11NoGUIPlatform::X11NoGUIPlatform()
{
  m_message_loop_running.store(true, std::memory_order_release);
}

X11NoGUIPlatform::~X11NoGUIPlatform()
{
  if (m_display)
  {
    // Segfaults somewhere in an unloaded module on Ubuntu 22.04 :S
    // I really don't care enough about X to figure out why. The application is shutting down
    // anyway, so a leak here isn't a big deal.
    // XCloseDisplay(m_display);
  }
}

bool X11NoGUIPlatform::Initialize()
{
  const int res = XInitThreads();
  if (res == 0)
    Log_WarningPrintf("XInitThreads() returned %d, things might not be stable.", res);

  m_display = XOpenDisplay(nullptr);
  if (!m_display)
  {
    Log_ErrorPrint("Failed to connect to X11 display.");
    return false;
  }

  InitializeKeyMap();
  return true;
}

void X11NoGUIPlatform::ReportError(const std::string_view& title, const std::string_view& message)
{
  // not implemented
}

bool X11NoGUIPlatform::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  // not implemented
  return true;
}

void X11NoGUIPlatform::SetDefaultConfig(SettingsInterface& si) {}

bool X11NoGUIPlatform::CreatePlatformWindow(std::string title)
{
  s32 window_x, window_y, window_width, window_height;
  bool has_window_pos = NoGUIHost::GetSavedPlatformWindowGeometry(&window_x, &window_y, &window_width, &window_height);
  if (!has_window_pos)
  {
    window_x = 0;
    window_y = 0;
    window_width = DEFAULT_WINDOW_WIDTH;
    window_height = DEFAULT_WINDOW_HEIGHT;
  }

  XDisplayLocker locker(m_display);
  {
    m_window = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), window_x, window_y, window_width,
                                   window_height, 0, 0, BlackPixel(m_display, 0));
    if (!m_window)
    {
      Log_ErrorPrint("Failed to create X window");
      return false;
    }

    XSelectInput(m_display, m_window,
                 StructureNotifyMask | KeyPressMask | KeyReleaseMask | FocusChangeMask | PointerMotionMask |
                   ButtonPressMask | ButtonReleaseMask);
    XStoreName(m_display, m_window, title.c_str());

    // Enable close notifications.
    Atom wmProtocols[1];
    wmProtocols[0] = XInternAtom(m_display, "WM_DELETE_WINDOW", True);
    XSetWMProtocols(m_display, m_window, wmProtocols, 1);

    m_window_info.surface_width = static_cast<u32>(window_width);
    m_window_info.surface_height = static_cast<u32>(window_height);
    m_window_info.surface_scale = 1.0f;
    m_window_info.type = WindowInfo::Type::X11;
    m_window_info.window_handle = reinterpret_cast<void*>(m_window);
    m_window_info.display_connection = m_display;

    XMapRaised(m_display, m_window);
    XFlush(m_display);
    XSync(m_display, True);
  }

  ProcessXEvents();
  return true;
}

bool X11NoGUIPlatform::HasPlatformWindow() const
{
  return m_window != 0;
}

void X11NoGUIPlatform::DestroyPlatformWindow()
{
  m_window_info = {};

  if (m_window)
  {
    XDisplayLocker locker(m_display);
    SaveWindowGeometry();
    XUnmapWindow(m_display, m_window);
    XDestroyWindow(m_display, m_window);
    m_window = {};
  }
}

std::optional<WindowInfo> X11NoGUIPlatform::GetPlatformWindowInfo()
{
  if (m_window_info.type == WindowInfo::Type::X11)
    return m_window_info;
  else
    return std::nullopt;
}

void X11NoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
  ExecuteInMessageLoop([this, title = std::move(title)]() {
    if (m_window)
    {
      XDisplayLocker locker(m_display);
      XStoreName(m_display, m_window, title.c_str());
    }
  });
}

void X11NoGUIPlatform::InitializeKeyMap()
{
  int min_keycode = 0, max_keycode = -1;
  XDisplayKeycodes(m_display, &min_keycode, &max_keycode);
  for (int keycode = 0; keycode <= max_keycode; keycode++)
  {
    KeySym keysym = NoSymbol;
    for (int i = 0; i < 8 && keysym == NoSymbol; i++)
      keysym = XKeycodeToKeysym(m_display, static_cast<KeyCode>(keycode), i);
    if (keysym == NoSymbol)
      continue;

    KeySym upper_keysym;
    XConvertCase(keysym, &keysym, &upper_keysym);

    // Would this fail?
    const char* keyname = XKeysymToString(upper_keysym);
    if (!keyname)
      continue;

    m_key_map.emplace(static_cast<s32>(upper_keysym), keyname);
  }
}

std::optional<u32> X11NoGUIPlatform::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
  for (const auto& it : m_key_map)
  {
    if (str == it.second)
      return it.first;
  }

  return std::nullopt;
}

std::optional<std::string> X11NoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
  const auto it = m_key_map.find(static_cast<s32>(code));
  return (it != m_key_map.end()) ? std::optional<std::string>(it->second) : std::nullopt;
}

void X11NoGUIPlatform::ProcessXEvents()
{
  XDisplayLocker locker(m_display);

  for (int num_events = XPending(m_display); num_events > 0; num_events--)
  {
    XEvent event;
    XNextEvent(m_display, &event);
    switch (event.type)
    {
      case KeyPress:
      case KeyRelease:
      {
        KeySym sym = XLookupKeysym(&event.xkey, 0);
        if (sym != NoSymbol)
        {
          KeySym upper_sym = sym;
          XConvertCase(sym, &sym, &upper_sym);
          NoGUIHost::ProcessPlatformKeyEvent(static_cast<s32>(upper_sym), (event.type == KeyPress));
        }
      }
      break;

      case ButtonPress:
      case ButtonRelease:
      {
        if (event.xbutton.button >= Button4 && event.xbutton.button <= Button5)
        {
          // Button 4/5 are mouse wheel events on X, apparently...
          NoGUIHost::ProcessPlatformMouseWheelEvent(0.0f, (event.xbutton.button == Button4) ? 1.0f : -1.0f);
        }
        else if (event.xbutton.button >= Button1)
        {
          // Swap middle and right buttons.
          const u32 xbutton = event.xbutton.button;
          const u32 mapped_button = (xbutton == Button3) ? 1 : (xbutton == Button2 ? 2 : (xbutton - Button1));
          NoGUIHost::ProcessPlatformMouseButtonEvent(mapped_button, event.type == ButtonPress);
        }
      }
      break;

      case MotionNotify:
      {
        NoGUIHost::ProcessPlatformMouseMoveEvent(static_cast<float>(event.xmotion.x),
                                                 static_cast<float>(event.xmotion.y));
      }
      break;

      case ConfigureNotify:
      {
        const s32 width = std::max<s32>(static_cast<s32>(event.xconfigure.width), 1);
        const s32 height = std::max<s32>(static_cast<s32>(event.xconfigure.height), 1);
        NoGUIHost::ProcessPlatformWindowResize(width, height, m_window_info.surface_scale);
      }
      break;

      case FocusIn:
      {
        NoGUIHost::PlatformWindowFocusGained();
      }
      break;

      case FocusOut:
      {
        NoGUIHost::PlatformWindowFocusGained();
      }
      break;

      case ClientMessage:
      {
        if (static_cast<Atom>(event.xclient.data.l[0]) == XInternAtom(m_display, "WM_DELETE_WINDOW", False))
          Host::RequestExit(false);
      }
      break;

      default:
        break;
    }
  }
}

void X11NoGUIPlatform::RunMessageLoop()
{
  while (m_message_loop_running.load(std::memory_order_acquire))
  {
    ProcessXEvents();

    {
      std::unique_lock lock(m_callback_queue_mutex);
      while (!m_callback_queue.empty())
      {
        std::function<void()> func = std::move(m_callback_queue.front());
        m_callback_queue.pop_front();
        lock.unlock();
        func();
        lock.lock();
      }
    }

    // TODO: Make this suck less.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void X11NoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
  std::unique_lock lock(m_callback_queue_mutex);
  m_callback_queue.push_back(std::move(func));
}

void X11NoGUIPlatform::QuitMessageLoop()
{
  m_message_loop_running.store(false, std::memory_order_release);
}

void X11NoGUIPlatform::SetFullscreen(bool enabled)
{
  if (!m_window || m_fullscreen.load(std::memory_order_acquire) == enabled)
    return;

  XDisplayLocker locker(m_display);

  XEvent event;
  event.xclient.type = ClientMessage;
  event.xclient.message_type = XInternAtom(m_display, "_NET_WM_STATE", False);
  event.xclient.window = m_window;
  event.xclient.format = 32;
  event.xclient.data.l[0] = _NET_WM_STATE_TOGGLE;
  event.xclient.data.l[1] = XInternAtom(m_display, "_NET_WM_STATE_FULLSCREEN", False);
  if (!XSendEvent(m_display, DefaultRootWindow(m_display), False, SubstructureRedirectMask | SubstructureNotifyMask,
                  &event))
  {
    Log_ErrorPrintf("Failed to switch to %s", enabled ? "Fullscreen" : "windowed");
    return;
  }

  m_fullscreen.store(enabled, std::memory_order_release);
}

void X11NoGUIPlatform::SaveWindowGeometry()
{
  int x = 0, y = 0;
  unsigned int width = 0, height = 0;
  unsigned int dummy_border, dummy_depth;
  Window dummy_window;
  XGetGeometry(m_display, m_window, &dummy_window, &x, &y, &width, &height, &dummy_border, &dummy_depth);
  if (width > 0 && height > 0)
    NoGUIHost::SavePlatformWindowGeometry(x, y, width, height);
}

bool X11NoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  return false;
}

bool X11NoGUIPlatform::OpenURL(const std::string_view& url)
{
  Log_ErrorPrintf("X11NoGUIPlatform::OpenURL() not implemented: %.*s", static_cast<int>(url.size()), url.data());
  return false;
}

bool X11NoGUIPlatform::CopyTextToClipboard(const std::string_view& text)
{
  Log_ErrorPrintf("X11NoGUIPlatform::CopyTextToClipboard() not implemented: %.*s", static_cast<int>(text.size()),
                  text.data());
  return false;
}

std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateX11Platform()
{
  std::unique_ptr<X11NoGUIPlatform> ret = std::unique_ptr<X11NoGUIPlatform>(new X11NoGUIPlatform());
  if (!ret->Initialize())
    return {};

  return ret;
}
