#include "common/assert.h"
#include "common/log.h"
#include "core/system.h"
#include "frontend-common/sdl_initializer.h"
#include "sdl_host_interface.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>

#undef main
int main(int argc, char* argv[])
{
  // set log flags
#ifndef _DEBUG
  const LOGLEVEL level = LOGLEVEL_INFO;
  // const LOGLEVEL level = LOGLEVEL_DEV;
  // const LOGLEVEL level = LOGLEVEL_PROFILE;
  Log::SetConsoleOutputParams(true, nullptr, level);
  Log::SetFilterLevel(level);
#else
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_DEBUG);
  Log::SetConsoleOutputParams(true, "Pad DigitalController MemoryCard SPU", LOGLEVEL_DEBUG);
  // Log::SetConsoleOutputParams(true, "GPU GPU_HW_OpenGL SPU Pad DigitalController", LOGLEVEL_DEBUG);
  // Log::SetConsoleOutputParams(true, "GPU GPU_HW_OpenGL Pad DigitalController MemoryCard InterruptController SPU
  // MDEC", LOGLEVEL_DEBUG); g_pLog->SetFilterLevel(LOGLEVEL_TRACE);
  Log::SetFilterLevel(LOGLEVEL_DEBUG);
  // Log::SetFilterLevel(LOGLEVEL_DEV);
#endif

  FrontendCommon::EnsureSDLInitialized();

  std::unique_ptr<SDLHostInterface> host_interface = SDLHostInterface::Create();
  std::unique_ptr<SystemBootParameters> boot_params;
  if (!host_interface->ParseCommandLineParameters(argc, argv, &boot_params))
  {
    SDL_Quit();
    return EXIT_FAILURE;
  }

  if (!host_interface->Initialize())
  {
    host_interface->Shutdown();
    SDL_Quit();
    return EXIT_FAILURE;
  }

  if (boot_params)
  {
    if (!host_interface->BootSystem(*boot_params) && host_interface->InBatchMode())
    {
      host_interface->Shutdown();
      host_interface.reset();
      SDL_Quit();
      return EXIT_FAILURE;
    }

    boot_params.reset();
  }

  host_interface->Run();
  host_interface->Shutdown();
  host_interface.reset();

  SDL_Quit();
  return EXIT_SUCCESS;
}
