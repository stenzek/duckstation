#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "core/system.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef WITH_VTY
#include "vty_host_interface.h"
#endif

#ifdef WITH_SDL2
#include "sdl_host_interface.h"

static bool IsSDLHostInterfaceAvailable()
{
#if defined(__linux__)
  // Only available if we have a X11 or Wayland display.
  if (std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY"))
    return true;
  else
    return false;
#else
  // Always available on Windows/Apple.
  return true;
#endif
}
#endif

#ifdef _WIN32
#include "common/windows_headers.h"
#include "win32_host_interface.h"
#include <shellapi.h>
#endif

static std::unique_ptr<NoGUIHostInterface> CreateHostInterface()
{
  const char* platform = std::getenv("DUCKSTATION_NOGUI_PLATFORM");
  std::unique_ptr<NoGUIHostInterface> host_interface;

#ifdef WITH_SDL2
  if (!host_interface && (!platform || StringUtil::Strcasecmp(platform, "sdl") == 0) && IsSDLHostInterfaceAvailable())
    host_interface = SDLHostInterface::Create();
#endif

#ifdef WITH_VTY
  if (!host_interface && (!platform || StringUtil::Strcasecmp(platform, "vty") == 0))
    host_interface = VTYHostInterface::Create();
#endif

#ifdef _WIN32
  if (!host_interface && (!platform || StringUtil::Strcasecmp(platform, "win32") == 0))
    host_interface = Win32HostInterface::Create();
#endif

  return host_interface;
}

static int Run(std::unique_ptr<NoGUIHostInterface> host_interface, std::unique_ptr<SystemBootParameters> boot_params)
{
  if (!host_interface->Initialize())
  {
    host_interface->Shutdown();
    return EXIT_FAILURE;
  }

  if (boot_params)
    host_interface->BootSystem(std::move(boot_params));

  int result;
  if (System::IsValid() || !host_interface->InBatchMode())
  {
    host_interface->Run();
    result = EXIT_SUCCESS;
  }
  else
  {
    host_interface->ReportError("No file specified, and we're in batch mode. Exiting.");
    result = EXIT_FAILURE;
  }

  host_interface->Shutdown();
  return result;
}

#ifdef _WIN32

int wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
  std::unique_ptr<NoGUIHostInterface> host_interface = CreateHostInterface();
  std::unique_ptr<SystemBootParameters> boot_params;

  {
    std::vector<std::string> argc_strings;
    argc_strings.reserve(1);

    // CommandLineToArgvW() only adds the program path if the command line is empty?!
    argc_strings.push_back(FileSystem::GetProgramPath());

    if (std::wcslen(lpCmdLine) > 0)
    {
      int argc;
      LPWSTR* argv_wide = CommandLineToArgvW(lpCmdLine, &argc);
      if (argv_wide)
      {
        for (int i = 0; i < argc; i++)
          argc_strings.push_back(StringUtil::WideStringToUTF8String(argv_wide[i]));

        LocalFree(argv_wide);
      }
    }

    std::vector<char*> argc_pointers;
    argc_pointers.reserve(argc_strings.size());
    for (std::string& arg : argc_strings)
      argc_pointers.push_back(arg.data());

    if (!host_interface->ParseCommandLineParameters(static_cast<int>(argc_pointers.size()), argc_pointers.data(),
                                                    &boot_params))
    {
      return EXIT_FAILURE;
    }
  }

  return Run(std::move(host_interface), std::move(boot_params));
}

#else

int main(int argc, char* argv[])
{
  std::unique_ptr<NoGUIHostInterface> host_interface = CreateHostInterface();
  std::unique_ptr<SystemBootParameters> boot_params;
  if (!host_interface->ParseCommandLineParameters(argc, argv, &boot_params))
    return EXIT_FAILURE;

  return Run(std::move(host_interface), std::move(boot_params));
}

#endif