// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "platform_misc.h"
#include "window_info.h"
#include "metal_layer.h"

#include "common/log.h"
#include "common/small_string.h"

#include <IOKit/pwr_mgt/IOPMLib.h>
#include <Cocoa/Cocoa.h>
#include <QuartzCore/QuartzCore.h>
#include <cinttypes>
#include <vector>

Log_SetChannel(PlatformMisc);

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

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

void PlatformMisc::SuspendScreensaver()
{
  if (s_screensaver_suspended)

  if (!SetScreensaverInhibitMacOS(true))
  {
    Log_ErrorPrintf("Failed to suspend screensaver.");
    return;
  }

  s_screensaver_suspended = true;
}

void PlatformMisc::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitMacOS(false))
    Log_ErrorPrint("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

bool PlatformMisc::PlaySoundAsync(const char* path)
{
  NSString* nspath = [[NSString alloc] initWithUTF8String:path];
  NSSound* sound = [[NSSound alloc] initWithContentsOfFile:nspath byReference:YES];
  const bool result = [sound play];
  [sound release];
  [nspath release];
  return result;
}

bool CocoaTools::CreateMetalLayer(WindowInfo *wi)
{
  // Punt off to main thread if we're not calling from it already.
  if (![NSThread isMainThread])
  {
    bool ret;
    dispatch_sync(dispatch_get_main_queue(), [&ret, wi]() {
      ret = CreateMetalLayer(wi);
    });
    return ret;
  }
  
  CAMetalLayer* layer = [CAMetalLayer layer];
  if (layer == nil)
  {
    Log_ErrorPrint("Failed to create CAMetalLayer");
    return false;
  }
  
  NSView* view = (__bridge NSView*)wi->window_handle;
  [view setWantsLayer:TRUE];
  [view setLayer:layer];
  [layer setContentsScale:[[[view window] screen] backingScaleFactor]];
  
  wi->surface_handle = (__bridge void*)layer;
  return true;
}

void CocoaTools::DestroyMetalLayer(WindowInfo *wi)
{
  if (!wi->surface_handle)
    return;
  
  // Punt off to main thread if we're not calling from it already.
  if (![NSThread isMainThread])
  {
    dispatch_sync(dispatch_get_main_queue(), [wi]() { DestroyMetalLayer(wi); });
    return;
  }
  
  NSView* view = (__bridge NSView*)wi->window_handle;
  CAMetalLayer* layer = (__bridge CAMetalLayer*)wi->surface_handle;
  [view setLayer:nil];
  [view setWantsLayer:NO];
  [layer release];
}
