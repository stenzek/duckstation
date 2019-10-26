#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/StringConverter.h"
#include "core/system.h"
#include "sdl_interface.h"
#include <SDL.h>
#include <cstdio>

#if 0
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
#endif

static int Run(int argc, char* argv[])
{
  // init sdl
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
  {
    Panic("SDL initialization failed");
    return -1;
  }

  // parameters
  const char* filename = nullptr;
  const char* exp1_filename = nullptr;
  TinyString state_filename;
  for (int i = 1; i < argc; i++)
  {
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

    if (CHECK_ARG_PARAM("-state"))
      state_filename = SDLInterface::GetSaveStateFilename(std::strtoul(argv[++i], nullptr, 10));
    else if (CHECK_ARG_PARAM("-exp1"))
      exp1_filename = argv[++i];
    else
      filename = argv[i];

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
  }

  // create display and host interface
  std::unique_ptr<SDLInterface> host_interface =
    SDLInterface::Create(filename, exp1_filename, state_filename.IsEmpty() ? nullptr : state_filename.GetCharArray());
  if (!host_interface)
  {
    Panic("Failed to create host interface");
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
#ifdef Y_BUILD_CONFIG_RELEASE
  const LOGLEVEL level = LOGLEVEL_INFO;
  // const LOGLEVEL level = LOGLEVEL_DEV;
  // const LOGLEVEL level = LOGLEVEL_PROFILE;
  g_pLog->SetConsoleOutputParams(true, nullptr, level);
  g_pLog->SetFilterLevel(level);
#else
  g_pLog->SetConsoleOutputParams(true, nullptr, LOGLEVEL_DEBUG);
  // g_pLog->SetConsoleOutputParams(true, "GPU GPU_HW_OpenGL SPU Pad DigitalController", LOGLEVEL_DEBUG);
  g_pLog->SetConsoleOutputParams(true, "GPU GPU_HW_OpenGL Pad DigitalController InterruptController SPU MDEC", LOGLEVEL_DEBUG);
  // g_pLog->SetFilterLevel(LOGLEVEL_TRACE);
  g_pLog->SetFilterLevel(LOGLEVEL_DEBUG);
  // g_pLog->SetFilterLevel(LOGLEVEL_DEV);
#endif

  // return NoGUITest();
  return Run(argc, argv);
}
