// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "sdl_nogui_platform.h"
#include "nogui_host.h"
#include "sdl_key_names.h"

#include "core/host.h"

#include "util/imgui_manager.h"
#include "util/sdl_input_source.h"

#include "common/log.h"
#include "common/scoped_guard.h"
#include "common/string_util.h"
#include "common/threading.h"

#include <SDL.h>
#include <SDL_syswm.h>

Log_SetChannel(SDLNoGUIPlatform);

static constexpr float DEFAULT_WINDOW_DPI = 96.0f;

SDLNoGUIPlatform::SDLNoGUIPlatform()
{
  m_message_loop_running.store(true, std::memory_order_release);
}

SDLNoGUIPlatform::~SDLNoGUIPlatform()
{
  SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

bool SDLNoGUIPlatform::Initialize()
{
  if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
  {
    Log_ErrorFmt("SDL_InitSubSystem() failed: {}", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error",
                             TinyString::from_format("SDL_InitSubSystem() failed: {}", SDL_GetError()), nullptr);
    return false;
  }

  m_func_event_id = SDL_RegisterEvents(1);
  m_wakeup_event_id = SDL_RegisterEvents(1);
  if (m_func_event_id == static_cast<u32>(-1) || m_wakeup_event_id == static_cast<u32>(-1))
  {
    Log_ErrorFmt("SDL_RegisterEvents() failed: {}", SDL_GetError());
    return false;
  }

  // prevent input source polling on main thread...
  SDLInputSource::ALLOW_EVENT_POLLING = false;

  return true;
}

void SDLNoGUIPlatform::ReportError(const std::string_view& title, const std::string_view& message)
{
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, SmallString(title).c_str(), SmallString(message).c_str(), m_window);
}

bool SDLNoGUIPlatform::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
  const SmallString title_copy(title);
  const SmallString message_copy(message);

  static constexpr SDL_MessageBoxButtonData bd[2] = {
    {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
    {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "No"},
  };
  const SDL_MessageBoxData md = {SDL_MESSAGEBOX_INFORMATION,
                                 m_window,
                                 title_copy.c_str(),
                                 message_copy.c_str(),
                                 static_cast<int>(std::size(bd)),
                                 bd,
                                 nullptr};

  int buttonid = -1;
  SDL_ShowMessageBox(&md, &buttonid);
  return (buttonid == 1);
}

void SDLNoGUIPlatform::SetDefaultConfig(SettingsInterface& si)
{
  // noop
}

bool SDLNoGUIPlatform::CreatePlatformWindow(std::string title)
{
  s32 window_x, window_y, window_width, window_height;
  if (!NoGUIHost::GetSavedPlatformWindowGeometry(&window_x, &window_y, &window_width, &window_height))
  {
    window_x = SDL_WINDOWPOS_UNDEFINED;
    window_y = SDL_WINDOWPOS_UNDEFINED;
    window_width = DEFAULT_WINDOW_WIDTH;
    window_height = DEFAULT_WINDOW_HEIGHT;
  }

  m_window = SDL_CreateWindow(title.c_str(), window_x, window_y, window_width, window_height,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS |
                                SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!m_window)
  {
    Log_ErrorFmt("SDL_CreateWindow() failed: {}", SDL_GetError());
    ReportError("Error", TinyString::from_format("SDL_CreateWindow() failed: {}", SDL_GetError()));
    return false;
  }

  if (m_fullscreen.load(std::memory_order_acquire))
    SetFullscreen(true);

  return true;
}

bool SDLNoGUIPlatform::HasPlatformWindow() const
{
  return (m_window != nullptr);
}

void SDLNoGUIPlatform::DestroyPlatformWindow()
{
  if (!m_window)
    return;

  if (!m_fullscreen.load(std::memory_order_acquire))
  {
    int window_x = SDL_WINDOWPOS_UNDEFINED, window_y = SDL_WINDOWPOS_UNDEFINED;
    int window_width = DEFAULT_WINDOW_WIDTH, window_height = DEFAULT_WINDOW_HEIGHT;
    SDL_GetWindowPosition(m_window, &window_x, &window_y);
    SDL_GetWindowSize(m_window, &window_width, &window_height);
    NoGUIHost::SavePlatformWindowGeometry(window_x, window_y, window_width, window_height);
  }

  SDL_DestroyWindow(m_window);
  m_window = nullptr;
}

std::optional<WindowInfo> SDLNoGUIPlatform::GetPlatformWindowInfo()
{
  if (!m_window)
    return std::nullopt;

  SDL_SysWMinfo swi = {};
  SDL_VERSION(&swi.version);

  if (!SDL_GetWindowWMInfo(m_window, &swi))
  {
    Log_ErrorFmt("SDL_GetWindowWMInfo() failed: {}", SDL_GetError());
    return std::nullopt;
  }

  int window_width = 1, window_height = 1;
  int window_px_width = 1, window_px_height = 1;
  SDL_GetWindowSize(m_window, &window_width, &window_height);
  SDL_GetWindowSizeInPixels(m_window, &window_px_width, &window_px_height);
  m_window_scale = static_cast<float>(std::max(window_px_width, 1)) / static_cast<float>(std::max(window_width, 1));

  if (const int display_index = SDL_GetWindowDisplayIndex(m_window); display_index >= 0)
  {
    float ddpi, hdpi, vdpi;
    if (SDL_GetDisplayDPI(display_index, &ddpi, &hdpi, &vdpi) == 0)
      m_window_scale = std::max(ddpi / DEFAULT_WINDOW_DPI, 0.5f);
  }

  WindowInfo wi;
  wi.surface_width = static_cast<u32>(window_px_width);
  wi.surface_height = static_cast<u32>(window_px_height);
  wi.surface_scale = m_window_scale;

  switch (swi.subsystem)
  {
#ifdef SDL_VIDEO_DRIVER_WINDOWS
    case SDL_SYSWM_WINDOWS:
      wi.type = WindowInfo::Type::Win32;
      wi.window_handle = swi.info.win.window;
      break;
#endif

#ifdef SDL_VIDEO_DRIVER_X11
    case SDL_SYSWM_X11:
      wi.type = WindowInfo::Type::X11;
      wi.display_connection = swi.info.x11.display;
      wi.window_handle = swi.info.x11.window;
      break;
#endif

#ifdef SDL_VIDEO_DRIVER_WAYLAND
    case SDL_SYSWM_WAYLAND:
      wi.type = WindowInfo::Type::Wayland;
      wi.display_connection = swi.info.wl.display;
      wi.window_handle = swi.info.wl.surface;
      break;
#endif

#ifdef SDL_VIDEO_DRIVER_COCOA
    case SDL_SYSWM_COCOA:
      wi.type = WindowInfo::Type::MacOS;
      wi.window_handle = swi.info.cocoa.window;
      break;
#endif

    default:
      Log_ErrorFmt("Unhandled WM subsystem {}", static_cast<int>(swi.subsystem));
      return std::nullopt;
  }

  return wi;
}

void SDLNoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
  if (!m_window)
    return;

  SDL_SetWindowTitle(m_window, title.c_str());
}

std::optional<u32> SDLNoGUIPlatform::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
  std::optional<DWORD> converted(SDLKeyNames::GetKeyCodeForName(str));
  return converted.has_value() ? std::optional<u32>(static_cast<u32>(converted.value())) : std::nullopt;
  return std::nullopt;
}

std::optional<std::string> SDLNoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
  const char* converted = SDLKeyNames::GetKeyName(code);
  return converted ? std::optional<std::string>(converted) : std::nullopt;
}

void SDLNoGUIPlatform::RunMessageLoop()
{
  while (m_message_loop_running.load(std::memory_order_acquire))
  {
    SDL_Event ev;
    if (!SDL_WaitEvent(&ev))
      continue;

    ProcessEvent(&ev);
  }
}

void SDLNoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
  std::function<void()>* pfunc = new std::function<void()>(std::move(func));

  SDL_Event ev;
  ev.user = {};
  ev.type = m_func_event_id;
  ev.user.data1 = pfunc;
  SDL_PushEvent(&ev);
}

void SDLNoGUIPlatform::QuitMessageLoop()
{
  m_message_loop_running.store(false, std::memory_order_release);

  SDL_Event ev;
  ev.user = {};
  ev.type = m_wakeup_event_id;
  SDL_PushEvent(&ev);
}

void SDLNoGUIPlatform::SetFullscreen(bool enabled)
{
  if (!m_window || m_fullscreen.load(std::memory_order_acquire) == enabled)
    return;

  if (SDL_SetWindowFullscreen(m_window, enabled ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
  {
    Log_ErrorFmt("SDL_SetWindowFullscreen() failed: {}", SDL_GetError());
    return;
  }

  m_fullscreen.store(enabled, std::memory_order_release);
}

bool SDLNoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  if (!m_window || m_fullscreen.load(std::memory_order_acquire))
    return false;

  SDL_SetWindowSize(m_window, new_window_width, new_window_height);
  return true;
}

bool SDLNoGUIPlatform::OpenURL(const std::string_view& url)
{
  if (SDL_OpenURL(SmallString(url).c_str()) != 0)
  {
    Log_ErrorFmt("SDL_OpenURL() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

bool SDLNoGUIPlatform::CopyTextToClipboard(const std::string_view& text)
{
  if (SDL_SetClipboardText(SmallString(text).c_str()) != 0)
  {
    Log_ErrorFmt("SDL_SetClipboardText() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLNoGUIPlatform::ProcessEvent(const SDL_Event* ev)
{
  switch (ev->type)
  {
    case SDL_WINDOWEVENT:
    {
      switch (ev->window.event)
      {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        {
          int window_width = ev->window.data1, window_height = ev->window.data2;
          SDL_GetWindowSizeInPixels(m_window, &window_width, &window_height);
          NoGUIHost::ProcessPlatformWindowResize(window_width, window_height, m_window_scale);
        }
        break;

        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        {
          const int new_display = ev->window.data1;
          float ddpi, hdpi, vdpi;
          if (SDL_GetDisplayDPI(new_display, &ddpi, &hdpi, &vdpi) == 0)
          {
            if (const float new_scale = std::max(ddpi / DEFAULT_WINDOW_DPI, 0.5f); new_scale != m_window_scale)
            {
              m_window_scale = new_scale;

              int window_width = 1, window_height = 1;
              SDL_GetWindowSizeInPixels(m_window, &window_width, &window_height);
              NoGUIHost::ProcessPlatformWindowResize(window_width, window_height, m_window_scale);
            }
          }
        }
        break;

        case SDL_WINDOWEVENT_CLOSE:
        {
          Host::RunOnCPUThread([]() { Host::RequestExit(false); });
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_GAINED:
        {
          NoGUIHost::PlatformWindowFocusGained();
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_LOST:
        {
          NoGUIHost::PlatformWindowFocusLost();
        }
        break;

        default:
          break;
      }
    }
    break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
      const bool pressed = (ev->type == SDL_KEYDOWN);
      NoGUIHost::ProcessPlatformKeyEvent(static_cast<s32>(ev->key.keysym.sym), pressed);
    }
    break;

    case SDL_TEXTINPUT:
    {
      if (ImGuiManager::WantsTextInput())
        NoGUIHost::ProcessPlatformTextEvent(ev->text.text);
    }
    break;

    case SDL_MOUSEMOTION:
    {
      const float x = static_cast<float>(ev->motion.x);
      const float y = static_cast<float>(ev->motion.y);
      NoGUIHost::ProcessPlatformMouseMoveEvent(x, y);
    }
    break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
      const bool pressed = (ev->type == SDL_MOUSEBUTTONDOWN);
      if (ev->button.button > 0)
        NoGUIHost::ProcessPlatformMouseButtonEvent(ev->button.button - 1, pressed);
    }
    break;

    case SDL_MOUSEWHEEL:
    {
      NoGUIHost::ProcessPlatformMouseWheelEvent(ev->wheel.preciseX, ev->wheel.preciseY);
    }
    break;

    case SDL_QUIT:
    {
      Host::RunOnCPUThread([]() { Host::RequestExit(false); });
    }
    break;

    default:
    {
      if (ev->type == m_func_event_id)
      {
        std::function<void()>* pfunc = reinterpret_cast<std::function<void()>*>(ev->user.data1);
        if (pfunc)
        {
          (*pfunc)();
          delete pfunc;
        }
      }
      else if (ev->type == m_wakeup_event_id)
      {
      }
      else if (SDLInputSource::IsHandledInputEvent(ev) && InputManager::GetInputSourceInterface(InputSourceType::SDL))
      {
        Host::RunOnCPUThread([event_copy = *ev]() {
          SDLInputSource* is =
            static_cast<SDLInputSource*>(InputManager::GetInputSourceInterface(InputSourceType::SDL));
          if (is) [[likely]]
            is->ProcessSDLEvent(&event_copy);
        });
      }
    }
    break;
  }
}

std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateSDLPlatform()
{
  std::unique_ptr<SDLNoGUIPlatform> ret(new SDLNoGUIPlatform());
  if (!ret->Initialize())
    return {};

  return ret;
}
