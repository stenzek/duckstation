#include "inhibit_screensaver.h"
#include "common/log.h"
#include "common/string.h"
#include <cinttypes>
Log_SetChannel(FrontendCommon);

#if defined(_WIN32) && !defined(_UWP)
#include "common/windows_headers.h"

static bool SetScreensaverInhibitWin32(bool inhibit, const WindowInfo& wi)
{
  if (SetThreadExecutionState(ES_CONTINUOUS | (inhibit ? (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : 0)) == NULL)
  {
    Log_ErrorPrintf("SetThreadExecutionState() failed: %d", GetLastError());
    return false;
  }

  return true;
}

#endif // _WIN32

#ifdef __APPLE__
#include <IOKit/pwr_mgt/IOPMLib.h>

static IOPMAssertionID s_prevent_idle_assertion = kIOPMNullAssertionID;

static bool SetScreensaverInhibitMacOS(bool inhibit, const WindowInfo& wi)
{
  if (inhibit)
  {
    const CFStringRef reason = CFSTR("System Running");
    if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, reason,
                                    &s_prevent_idle_assertion) != kIOReturnSuccess)
    {
      Log_ErrorPrintf("IOPMAssertionCreateWithName() failed");
      return false;
    }

    return true;
  }
  else
  {
    IOPMAssertionRelease(s_prevent_idle_assertion);
    s_prevent_idle_assertion = kIOPMNullAssertionID;
    return true;
  }
}

#endif // __APPLE__

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

  int status = 0;
  while (waitpid(pid, &status, 0) == -1)
    ;

  if (WEXITSTATUS(status) == 0)
    return true;

  Log_ErrorPrintf("xdg-screensaver returned error %d", WEXITSTATUS(status));
  return false;
}

#endif // USE_X11

static bool SetScreensaverInhibit(bool inhibit, const WindowInfo& wi)
{
  switch (wi.type)
  {
#if defined(_WIN32) && !defined(_UWP)
    case WindowInfo::Type::Win32:
      return SetScreensaverInhibitWin32(inhibit, wi);
#endif

#ifdef __APPLE__
    case WindowInfo::Type::MacOS:
      return SetScreensaverInhibitMacOS(inhibit, wi);
#endif

#ifdef USE_X11
    case WindowInfo::Type::X11:
      return SetScreensaverInhibitX11(inhibit, wi);
#endif

    default:
      Log_ErrorPrintf("Unknown type: %u", static_cast<unsigned>(wi.type));
      return false;
  }
}

namespace FrontendCommon {

static bool s_screensaver_suspended;
static WindowInfo s_screensaver_suspender;

void SuspendScreensaver(const WindowInfo& wi)
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

void ResumeScreensaver()
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

} // namespace FrontendCommon
