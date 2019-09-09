#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/StringConverter.h"
#include "sdl_interface.h"
#include "pse/types.h"
#include "pse/system.h"
#include <SDL.h>
#include <cstdio>

static int NoGUITest()
{
  std::unique_ptr<System> system = std::make_unique<System>();
  if (!system->Initialize())
    return -1;
  
  system->Reset();

  while (true)
    system->RunFrame();
  return 0;
}

static int Run(int argc, char* argv[])
{
  if (argc < 2)
  {
    std::fprintf(stderr, "Usage: %s <path to system ini> [save state index]\n", argv[0]);
    return -1;
  }

  // init sdl
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
  {
    Panic("SDL initialization failed");
    return -1;
  }

  // create display and host interface
  std::unique_ptr<SDLInterface> host_interface = SDLInterface::Create();
  if (!host_interface)
  {
    Panic("Failed to create host interface");
    SDL_Quit();
    return -1;
  }

  // create system
  s32 state_index = -1;
  if (argc > 2)
    state_index = StringConverter::StringToInt32(argv[2]);
  if (!host_interface->InitializeSystem(argv[1], state_index))
  {
    host_interface.reset();
    SDL_Quit();
    return -1;
  }

  // run
  host_interface->Run();

  // done
  host_interface.reset();
  SDL_Quit();
  return 0;
}

// SDL requires the entry point declared without c++ decoration
#undef main
int main(int argc, char* argv[])
{
  // set log flags
  g_pLog->SetConsoleOutputParams(true, nullptr, LOGLEVEL_DEBUG);

#ifdef Y_BUILD_CONFIG_RELEASE
  g_pLog->SetFilterLevel(LOGLEVEL_INFO);
  // g_pLog->SetFilterLevel(LOGLEVEL_PROFILE);
#else
  // g_pLog->SetFilterLevel(LOGLEVEL_TRACE);
  g_pLog->SetFilterLevel(LOGLEVEL_DEBUG);
#endif

  return NoGUITest();
  //return Run(argc, argv);
}
