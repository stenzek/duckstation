#include "sdl_util.h"
#include "common/log.h"
#include <SDL_syswm.h>
Log_SetChannel(SDLUtil);

#ifdef __APPLE__
#include <objc/message.h>
struct NSView;

static NSView* GetContentViewFromWindow(NSWindow* window)
{
  // window.contentView
  return reinterpret_cast<NSView* (*)(id, SEL)>(objc_msgSend)(reinterpret_cast<id>(window), sel_getUid("contentView"));
}
#endif

namespace SDLUtil {

std::optional<WindowInfo> GetWindowInfoForSDLWindow(SDL_Window* window)
{
  SDL_SysWMinfo syswm = {};
  SDL_VERSION(&syswm.version);
  if (!SDL_GetWindowWMInfo(window, &syswm))
  {
    Log_ErrorPrintf("SDL_GetWindowWMInfo failed");
    return std::nullopt;
  }

  int window_width, window_height;
  SDL_GetWindowSize(window, &window_width, &window_height);

  WindowInfo wi;
  wi.surface_width = static_cast<u32>(window_width);
  wi.surface_height = static_cast<u32>(window_height);
  wi.surface_scale = GetDPIScaleFactor(window);
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

float GetDPIScaleFactor(SDL_Window* window)
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
} // namespace SDLUtil