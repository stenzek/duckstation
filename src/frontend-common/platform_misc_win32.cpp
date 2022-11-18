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

void FrontendCommon::SuspendScreensaver()
{
  if (s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitWin32(true))
  {
    Log_ErrorPrintf("Failed to suspend screensaver.");
    return;
  }

  s_screensaver_suspended = true;
}

void FrontendCommon::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitWin32(false))
    Log_ErrorPrint("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

bool FrontendCommon::PlaySoundAsync(const char* path)
{
  const std::wstring wpath(StringUtil::UTF8StringToWideString(path));
  return PlaySoundW(wpath.c_str(), NULL, SND_ASYNC | SND_NODEFAULT);
}
