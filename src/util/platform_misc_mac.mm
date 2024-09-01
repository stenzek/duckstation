// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Normally, system includes come last. But apparently some of our macro names are redefined...
#include <Cocoa/Cocoa.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <QuartzCore/QuartzCore.h>
#include <cinttypes>
#include <optional>
#include <sys/sysctl.h>
#include <vector>

#include "metal_layer.h"
#include "platform_misc.h"
#include "window_info.h"

#include "common/log.h"
#include "common/small_string.h"

Log_SetChannel(PlatformMisc);

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

static IOPMAssertionID s_prevent_idle_assertion = kIOPMNullAssertionID;

bool PlatformMisc::InitializeSocketSupport(Error* error)
{
  return true;
}

static bool SetScreensaverInhibitMacOS(bool inhibit)
{
  if (inhibit)
  {
    const CFStringRef reason = CFSTR("System Running");
    if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, reason,
                                    &s_prevent_idle_assertion) != kIOReturnSuccess)
    {
      ERROR_LOG("IOPMAssertionCreateWithName() failed");
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
      ERROR_LOG("Failed to suspend screensaver.");
      return;
    }

  s_screensaver_suspended = true;
}

void PlatformMisc::ResumeScreensaver()
{
  if (!s_screensaver_suspended)
    return;

  if (!SetScreensaverInhibitMacOS(false))
    ERROR_LOG("Failed to resume screensaver.");

  s_screensaver_suspended = false;
}

template<typename T>
static std::optional<T> sysctlbyname(const char* name)
{
  T output = 0;
  size_t output_size = sizeof(output);
  if (sysctlbyname(name, &output, &output_size, nullptr, 0) != 0)
    return std::nullopt;

  return output;
}

size_t PlatformMisc::GetRuntimePageSize()
{
  return sysctlbyname<u32>("hw.pagesize").value_or(0);
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

bool CocoaTools::CreateMetalLayer(WindowInfo* wi)
{
  // Punt off to main thread if we're not calling from it already.
  if (![NSThread isMainThread])
  {
    bool ret;
    dispatch_sync(dispatch_get_main_queue(), [&ret, wi]() { ret = CreateMetalLayer(wi); });
    return ret;
  }

  CAMetalLayer* layer = [CAMetalLayer layer];
  if (layer == nil)
  {
    ERROR_LOG("Failed to create CAMetalLayer");
    return false;
  }

  NSView* view = (__bridge NSView*)wi->window_handle;
  [view setWantsLayer:TRUE];
  [view setLayer:layer];
  [layer setContentsScale:[[[view window] screen] backingScaleFactor]];

  wi->surface_handle = (__bridge void*)layer;
  return true;
}

void CocoaTools::DestroyMetalLayer(WindowInfo* wi)
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

std::optional<float> CocoaTools::GetViewRefreshRate(const WindowInfo& wi)
{
  if (![NSThread isMainThread])
  {
    std::optional<float> ret;
    dispatch_sync(dispatch_get_main_queue(), [&ret, wi]{ ret = GetViewRefreshRate(wi); });
    return ret;
  }

  std::optional<float> ret;
  NSView* const view = (__bridge NSView*)wi.window_handle;
  const u32 did = [[[[[view window] screen] deviceDescription] valueForKey:@"NSScreenNumber"] unsignedIntValue];
  if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did))
  {
    ret = CGDisplayModeGetRefreshRate(mode);
    CGDisplayModeRelease(mode);
  }

  return ret;
}
