#include "common/log.h"
#include "common/string.h"
#include "platform_misc.h"
#include <cinttypes>
Log_SetChannel(FrontendCommon);

#ifdef USE_X11
#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

static bool SetScreensaverInhibitX11(bool inhibit, const WindowInfo& wi)
{
  TinyString command;
  command.AppendString("xdg-screensaver");

  TinyString operation;
  operation.AppendString(inhibit ? "suspend" : "resume");

  TinyString id;
  id.Format("0x%" PRIx64, static_cast<u64>(reinterpret_cast<uintptr_t>(wi.window_handle)));

  char* argv[4] = {command.GetWriteableCharArray(), operation.GetWriteableCharArray(), id.GetWriteableCharArray(),
                   nullptr};
  pid_t pid;
  int res = posix_spawnp(&pid, "xdg-screensaver", nullptr, nullptr, argv, environ);
  if (res != 0)
  {
    Log_ErrorPrintf("posix_spawnp() failed: %d", res);
    return false;
  }

  return true;
}

#endif // USE_X11

static bool SetScreensaverInhibit(bool inhibit, const WindowInfo& wi)
{
  switch (wi.type)
  {
#ifdef USE_X11
    case WindowInfo::Type::X11:
      return SetScreensaverInhibitX11(inhibit, wi);
#endif

    default:
      Log_ErrorPrintf("Unknown type: %u", static_cast<unsigned>(wi.type));
      return false;
  }
}

static bool s_screensaver_suspended;
static WindowInfo s_screensaver_suspender;

void FrontendCommon::SuspendScreensaver(const WindowInfo& wi)
{
  if (s_screensaver_suspended &&
      (s_screensaver_suspender.type != wi.type || s_screensaver_suspender.window_handle != wi.window_handle))
    ResumeScreensaver();

  if (!SetScreensaverInhibit(true, wi))
  {
    Log_ErrorPrintf("Failed to suspend screensaver.");
    return;
  }

  Log_InfoPrintf("Screensaver suspended by 0x%" PRIx64 ".",
                 static_cast<u64>(reinterpret_cast<uintptr_t>(wi.window_handle)));
  s_screensaver_suspended = true;
  s_screensaver_suspender = wi;
}

void FrontendCommon::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibit(false, s_screensaver_suspender))
    Log_ErrorPrint("Failed to resume screensaver.");
  else
    Log_InfoPrint("Screensaver resumed.");

  s_screensaver_suspended = false;
  s_screensaver_suspender = {};
}

bool FrontendCommon::PlaySoundAsync(const char* path)
{
#ifdef __linux__
  // This is... pretty awful. But I can't think of a better way without linking to e.g. gstreamer.
  const char* cmdname = "aplay";
  const char* argv[] = {cmdname, path, nullptr};
  pid_t pid;

  // Since we set SA_NOCLDWAIT in Qt, we don't need to wait here.
  int res = posix_spawnp(&pid, cmdname, nullptr, nullptr, const_cast<char**>(argv), environ);
  return (res == 0);
#else
  return false;
#endif
}
