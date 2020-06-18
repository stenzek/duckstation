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

    default:
      Log_ErrorPrintf("Unhandled syswm subsystem %u", static_cast<u32>(syswm.subsystem));
      return std::nullopt;
  }

  return wi;
}
} // namespace SDLUtil