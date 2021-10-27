#include "sdl_host_interface.h"
#include "core/system.h"
#include "frontend-common/controller_interface.h"
#include "frontend-common/fullscreen_ui.h"
#include "frontend-common/icon.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/sdl_controller_interface.h"
#include "frontend-common/sdl_initializer.h"
#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "scmversion/scmversion.h"
#include "sdl_key_names.h"
#include <SDL_syswm.h>
#include <cinttypes>
#include <cmath>
Log_SetChannel(SDLHostInterface);

#ifdef __APPLE__
#include <objc/message.h>
struct NSView;

static NSView* GetContentViewFromWindow(NSWindow* window)
{
  // window.contentView
  return reinterpret_cast<NSView* (*)(id, SEL)>(objc_msgSend)(reinterpret_cast<id>(window), sel_getUid("contentView"));
}
#endif

static float GetDPIScaleFactor(SDL_Window* window)
{
#ifdef __APPLE__
  static constexpr float DEFAULT_DPI = 72.0f;
#else
  static constexpr float DEFAULT_DPI = 96.0f;
#endif

  if (!window)
  {
    SDL_Window* dummy_window = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1, 1,
                                                SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (!dummy_window)
      return 1.0f;

    const float scale = GetDPIScaleFactor(dummy_window);

    SDL_DestroyWindow(dummy_window);

    return scale;
  }

  int display_index = SDL_GetWindowDisplayIndex(window);
  float display_dpi = DEFAULT_DPI;
  if (SDL_GetDisplayDPI(display_index, &display_dpi, nullptr, nullptr) != 0)
    return 1.0f;

  return display_dpi / DEFAULT_DPI;
}

SDLHostInterface::SDLHostInterface() = default;

SDLHostInterface::~SDLHostInterface() = default;

const char* SDLHostInterface::GetFrontendName() const
{
  return "DuckStation NoGUI Frontend";
}

std::unique_ptr<SDLHostInterface> SDLHostInterface::Create()
{
  return std::make_unique<SDLHostInterface>();
}

bool SDLHostInterface::Initialize()
{
  FrontendCommon::EnsureSDLInitialized();

  if (!NoGUIHostInterface::Initialize())
    return false;

  return true;
}

void SDLHostInterface::Shutdown()
{
  NoGUIHostInterface::Shutdown();
}

bool SDLHostInterface::IsFullscreen() const
{
  return m_fullscreen;
}

bool SDLHostInterface::SetFullscreen(bool enabled)
{
  if (m_fullscreen == enabled)
    return true;

  const std::string fullscreen_mode(GetStringSettingValue("GPU", "FullscreenMode", ""));
  const bool is_exclusive_fullscreen = (enabled && !fullscreen_mode.empty() && m_display->SupportsFullscreen());
  const bool was_exclusive_fullscreen = m_display->IsFullscreen();

  if (was_exclusive_fullscreen)
    m_display->SetFullscreen(false, 0, 0, 0.0f);

  SDL_SetWindowFullscreen(m_window, (enabled && !is_exclusive_fullscreen) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);

  if (is_exclusive_fullscreen)
  {
    u32 width, height;
    float refresh_rate;
    bool result = false;

    if (ParseFullscreenMode(fullscreen_mode, &width, &height, &refresh_rate))
    {
      result = m_display->SetFullscreen(true, width, height, refresh_rate);
      if (result)
      {
        AddOSDMessage(TranslateStdString("OSDMessage", "Acquired exclusive fullscreen."), 10.0f);
      }
      else
      {
        AddOSDMessage(TranslateStdString("OSDMessage", "Failed to acquire exclusive fullscreen."), 10.0f);
        enabled = false;
      }
    }
  }

  m_fullscreen = enabled;

  const bool hide_cursor = (enabled && GetBoolSettingValue("Main", "HideCursorInFullscreen", true));
  SDL_ShowCursor(hide_cursor ? SDL_DISABLE : SDL_ENABLE);
  return true;
}

bool SDLHostInterface::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  if (new_window_width <= 0 || new_window_height <= 0 || m_fullscreen)
    return false;

  // use imgui scale as the dpr
  const float dpi_scale = ImGui::GetIO().DisplayFramebufferScale.x;
  const s32 scaled_width =
    std::max<s32>(static_cast<s32>(std::ceil(static_cast<float>(new_window_width) * dpi_scale)), 1);
  const s32 scaled_height = std::max<s32>(
    static_cast<s32>(std::ceil(static_cast<float>(new_window_height) * dpi_scale)) + m_display->GetDisplayTopMargin(),
    1);

  SDL_SetWindowSize(m_window, scaled_width, scaled_height);
  return true;
}

ALWAYS_INLINE static TinyString GetWindowTitle()
{
  return TinyString::FromFormat("DuckStation %s (%s)", g_scm_tag_str, g_scm_branch_str);
}

bool SDLHostInterface::CreatePlatformWindow()
{
  // Create window.
  const u32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

  int window_x, window_y, window_width, window_height;
  GetSavedWindowGeometry(&window_x, &window_y, &window_width, &window_height);
  m_window = SDL_CreateWindow(GetWindowTitle(), window_x, window_y, window_width, window_height, window_flags);
  if (!m_window)
    return false;

  // Set window icon.
  SDL_Surface* icon_surface =
    SDL_CreateRGBSurfaceFrom(const_cast<unsigned int*>(WINDOW_ICON_DATA), WINDOW_ICON_WIDTH, WINDOW_ICON_HEIGHT, 32,
                             WINDOW_ICON_WIDTH * sizeof(u32), UINT32_C(0x000000FF), UINT32_C(0x0000FF00),
                             UINT32_C(0x00FF0000), UINT32_C(0xFF000000));
  if (icon_surface)
  {
    SDL_SetWindowIcon(m_window, icon_surface);
    SDL_FreeSurface(icon_surface);
  }

  ImGui_ImplSDL2_Init(m_window);

  // Process events so that we have everything sorted out before creating a child window for the GL context (X11).
  SDL_PumpEvents();
  return true;
}

void SDLHostInterface::DestroyPlatformWindow()
{
  SaveWindowGeometry();
  ImGui_ImplSDL2_Shutdown();
  SDL_DestroyWindow(m_window);
  m_window = nullptr;
  m_fullscreen = false;
}

std::optional<WindowInfo> SDLHostInterface::GetPlatformWindowInfo()
{
  SDL_SysWMinfo syswm = {};
  SDL_VERSION(&syswm.version);
  if (!SDL_GetWindowWMInfo(m_window, &syswm))
  {
    Log_ErrorPrintf("SDL_GetWindowWMInfo failed");
    return std::nullopt;
  }

  int window_width, window_height;
  SDL_GetWindowSize(m_window, &window_width, &window_height);

  WindowInfo wi;
  wi.surface_width = static_cast<u32>(window_width);
  wi.surface_height = static_cast<u32>(window_height);
  wi.surface_scale = GetDPIScaleFactor(m_window);
  wi.surface_format = WindowInfo::SurfaceFormat::RGB8;

  switch (syswm.subsystem)
  {
#ifdef SDL_VIDEO_DRIVER_WINDOWS
    case SDL_SYSWM_WINDOWS:
      wi.type = WindowInfo::Type::Win32;
      wi.window_handle = syswm.info.win.window;
      break;
#endif

#ifdef SDL_VIDEO_DRIVER_COCOA
    case SDL_SYSWM_COCOA:
      wi.type = WindowInfo::Type::MacOS;
      wi.window_handle = GetContentViewFromWindow(syswm.info.cocoa.window);
      break;
#endif

#ifdef SDL_VIDEO_DRIVER_X11
    case SDL_SYSWM_X11:
      wi.type = WindowInfo::Type::X11;
      wi.window_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(syswm.info.x11.window));
      wi.display_connection = syswm.info.x11.display;
      break;
#endif

#ifdef SDL_VIDEO_DRIVER_WAYLAND
    case SDL_SYSWM_WAYLAND:
      wi.type = WindowInfo::Type::Wayland;
      wi.window_handle = syswm.info.wl.surface;
      wi.display_connection = syswm.info.wl.display;
      break;
#endif

    default:
      Log_ErrorPrintf("Unhandled syswm subsystem %u", static_cast<u32>(syswm.subsystem));
      return std::nullopt;
  }

  return wi;
}

std::optional<CommonHostInterface::HostKeyCode> SDLHostInterface::GetHostKeyCode(const std::string_view key_code) const
{
  const std::optional<u32> code = SDLKeyNames::ParseKeyString(key_code);
  if (!code)
    return std::nullopt;

  return static_cast<HostKeyCode>(*code);
}

void SDLHostInterface::SetMouseMode(bool relative, bool hide_cursor) {}

void SDLHostInterface::PollAndUpdate()
{
  // Process SDL events before the controller interface can steal them.
  const bool is_sdl_controller_interface =
    (m_controller_interface && m_controller_interface->GetBackend() == ControllerInterface::Backend::SDL);

  for (;;)
  {
    SDL_Event ev;
    if (!SDL_PollEvent(&ev))
      break;

    if (is_sdl_controller_interface &&
        static_cast<SDLControllerInterface*>(m_controller_interface.get())->ProcessSDLEvent(&ev))
    {
      continue;
    }

    HandleSDLEvent(&ev);
  }

  ImGui_ImplSDL2_NewFrame();
  NoGUIHostInterface::PollAndUpdate();
}

void SDLHostInterface::HandleSDLEvent(const SDL_Event* event)
{
  ImGui_ImplSDL2_ProcessEvent(event);

  switch (event->type)
  {
    case SDL_WINDOWEVENT:
    {
      switch (event->window.event)
      {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        {
          s32 window_width, window_height;
          SDL_GetWindowSize(m_window, &window_width, &window_height);
          m_display->ResizeRenderWindow(window_width, window_height);
          OnHostDisplayResized();
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_LOST:
        {
          if (g_settings.pause_on_focus_loss && System::IsRunning() && !m_was_paused_by_focus_loss)
          {
            PauseSystem(true);
            m_was_paused_by_focus_loss = true;
          }
        }
        break;

        case SDL_WINDOWEVENT_FOCUS_GAINED:
        {
          if (m_was_paused_by_focus_loss)
          {
            if (System::IsPaused())
              PauseSystem(false);
            m_was_paused_by_focus_loss = false;
          }
        }
        break;

        default:
          break;
      }
    }
    break;

    case SDL_QUIT:
      m_quit_request = true;
      break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
      const bool pressed = (event->type == SDL_KEYDOWN);

      // Binding mode
      if (m_fullscreen_ui_enabled && FullscreenUI::IsBindingInput())
      {
        if (event->key.repeat > 0)
          return;

        TinyString key_string;
        if (SDLKeyNames::KeyEventToString(event, key_string))
        {
          if (FullscreenUI::HandleKeyboardBinding(key_string, pressed))
            return;
        }
      }

      if (!ImGui::GetIO().WantCaptureKeyboard && event->key.repeat == 0)
      {
        const u32 code = SDLKeyNames::KeyEventToInt(event);
        HandleHostKeyEvent(code & SDLKeyNames::KEY_MASK, code & SDLKeyNames::MODIFIER_MASK, pressed);
      }
    }
    break;

    case SDL_MOUSEMOTION:
    {
      m_display->SetMousePosition(event->motion.x, event->motion.y);
    }
    break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
      // map left -> 0, right -> 1, middle -> 2 to match with qt
      static constexpr std::array<s32, 5> mouse_mapping = {{1, 3, 2, 4, 5}};
      if (!ImGui::GetIO().WantCaptureMouse && event->button.button > 0 && event->button.button <= mouse_mapping.size())
      {
        const s32 button = mouse_mapping[event->button.button - 1];
        const bool pressed = (event->type == SDL_MOUSEBUTTONDOWN);
        HandleHostMouseEvent(button, pressed);
      }
    }
    break;
  }
}

void SDLHostInterface::GetSavedWindowGeometry(int* x, int* y, int* width, int* height)
{
  auto lock = GetSettingsLock();
  *x = m_settings_interface->GetIntValue("SDLHostInterface", "WindowX", SDL_WINDOWPOS_UNDEFINED);
  *y = m_settings_interface->GetIntValue("SDLHostInterface", "WindowY", SDL_WINDOWPOS_UNDEFINED);

  *width = m_settings_interface->GetIntValue("SDLHostInterface", "WindowWidth", -1);
  *height = m_settings_interface->GetIntValue("SDLHostInterface", "WindowHeight", -1);

  if (*width < 0 || *height < 0)
  {
    *width = DEFAULT_WINDOW_WIDTH;
    *height = DEFAULT_WINDOW_HEIGHT;

    // macOS does DPI scaling differently..
#ifndef __APPLE__
    {
      // scale by default monitor's DPI
      float scale = GetDPIScaleFactor(nullptr);
      *width = static_cast<int>(std::round(static_cast<float>(*width) * scale));
      *height = static_cast<int>(std::round(static_cast<float>(*height) * scale));
    }
#endif
  }
}

void SDLHostInterface::SaveWindowGeometry()
{
  if (m_fullscreen)
    return;

  int x = 0;
  int y = 0;
  SDL_GetWindowPosition(m_window, &x, &y);

  int width = DEFAULT_WINDOW_WIDTH;
  int height = DEFAULT_WINDOW_HEIGHT;
  SDL_GetWindowSize(m_window, &width, &height);

  int old_x, old_y, old_width, old_height;
  GetSavedWindowGeometry(&old_x, &old_y, &old_width, &old_height);
  if (x == old_x && y == old_y && width == old_width && height == old_height)
    return;

  auto lock = GetSettingsLock();
  m_settings_interface->SetIntValue("SDLHostInterface", "WindowX", x);
  m_settings_interface->SetIntValue("SDLHostInterface", "WindowY", y);
  m_settings_interface->SetIntValue("SDLHostInterface", "WindowWidth", width);
  m_settings_interface->SetIntValue("SDLHostInterface", "WindowHeight", height);
}
