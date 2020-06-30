#pragma once
#include "common/types.h"
#include "common/window_info.h"
#include <optional>

struct SDL_Window;

namespace SDLUtil {
std::optional<WindowInfo> GetWindowInfoForSDLWindow(SDL_Window* window);
float GetDPIScaleFactor(SDL_Window* window);
}