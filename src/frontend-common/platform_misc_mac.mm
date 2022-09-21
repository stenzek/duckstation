#include "platform_misc.h"
#include "common/log.h"
#include "common/string.h"
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <cinttypes>
Log_SetChannel(FrontendCommon);

#import <AppKit/AppKit.h>

static IOPMAssertionID s_prevent_idle_assertion = kIOPMNullAssertionID;

static bool SetScreensaverInhibitMacOS(bool inhibit)
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

static bool s_screensaver_suspended;
static WindowInfo s_screensaver_suspender;

void FrontendCommon::SuspendScreensaver(const WindowInfo& wi)
{
  if (s_screensaver_suspended &&
      (s_screensaver_suspender.type != wi.type || s_screensaver_suspender.window_handle != wi.window_handle))
    ResumeScreensaver();

  if (!SetScreensaverInhibitMacOS(true))
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

  if (!SetScreensaverInhibitMacOS(false))
    Log_ErrorPrint("Failed to resume screensaver.");
  else
    Log_InfoPrint("Screensaver resumed.");

  s_screensaver_suspended = false;
  s_screensaver_suspender = {};
}

bool FrontendCommon::PlaySoundAsync(const char* path)
{
  NSString* nspath = [[NSString alloc] initWithUTF8String:path];
  NSSound* sound = [[NSSound alloc] initWithContentsOfFile:nspath byReference:YES];
  const bool result = [sound play];
  [sound release];
  [nspath release];
  return result;
}
