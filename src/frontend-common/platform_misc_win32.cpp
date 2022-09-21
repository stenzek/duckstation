#include "common/log.h"
#include "common/string.h"
#include "common/string_util.h"
#include "platform_misc.h"
#include <cinttypes>
Log_SetChannel(FrontendCommon);

#include "common/windows_headers.h"
#include <mmsystem.h>

static bool SetScreensaverInhibitWin32(bool inhibit)
{
  if (SetThreadExecutionState(ES_CONTINUOUS | (inhibit ? (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : 0)) == NULL)
  {
    Log_ErrorPrintf("SetThreadExecutionState() failed: %d", GetLastError());
    return false;
  }

  return true;
}

static bool s_screensaver_suspended;
static WindowInfo s_screensaver_suspender;

void FrontendCommon::SuspendScreensaver(const WindowInfo& wi)
{
  if (s_screensaver_suspended &&
      (s_screensaver_suspender.type != wi.type || s_screensaver_suspender.window_handle != wi.window_handle))
    ResumeScreensaver();

  if (!SetScreensaverInhibitWin32(true))
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

  if (!SetScreensaverInhibitWin32(false))
    Log_ErrorPrint("Failed to resume screensaver.");
  else
    Log_InfoPrint("Screensaver resumed.");

  s_screensaver_suspended = false;
  s_screensaver_suspender = {};
}

bool FrontendCommon::PlaySoundAsync(const char* path)
{
  const std::wstring wpath(StringUtil::UTF8StringToWideString(path));
  return PlaySoundW(wpath.c_str(), NULL, SND_ASYNC | SND_NODEFAULT);
}
