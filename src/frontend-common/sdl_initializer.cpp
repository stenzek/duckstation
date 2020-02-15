#include "sdl_initializer.h"
#include "common/assert.h"
#include <SDL.h>

namespace FrontendCommon {
static bool s_sdl_initialized = false;

void EnsureSDLInitialized()
{
  if (s_sdl_initialized)
    return;

  if (SDL_Init(0) < 0)
  {
    Panic("SDL_Init(0) failed");
    return;
  }

  s_sdl_initialized = true;
}
} // namespace FrontendCommon
