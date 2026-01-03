// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Normally, system includes come last. But apparently some of our macro names are redefined...
#include <Cocoa/Cocoa.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <QuartzCore/QuartzCore.h>
#include <cinttypes>
#include <optional>
#include <vector>

#include "metal_layer.h"
#include "platform_misc.h"
#include "window_info.h"

#include "common/error.h"
#include "common/log.h"
#include "common/small_string.h"

LOG_CHANNEL(PlatformMisc);

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
    const CFStringRef reason = CFSTR("DuckStation System Running");
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

static bool s_screensaver_suspended = false;

void PlatformMisc::SuspendScreensaver()
{
  if (s_screensaver_suspended)
    return;

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

bool PlatformMisc::SetWindowRoundedCornerState(void* window_handle, bool enabled, Error* error /* = nullptr */)
{
  Error::SetStringView(error, "Unsupported on this platform.");
  return false;
}

void* CocoaTools::CreateMetalLayer(const WindowInfo& wi, Error* error)
{
  // Punt off to main thread if we're not calling from it already.
  if (![NSThread isMainThread])
  {
    void* ret;
    dispatch_sync(dispatch_get_main_queue(), [&ret, &wi, error]() { ret = CreateMetalLayer(wi, error); });
    return ret;
  }

  CAMetalLayer* layer = [[CAMetalLayer layer] retain];
  if (layer == nil)
  {
    Error::SetStringView(error, "Failed to create CAMetalLayer");
    return nullptr;
  }

  NSView* view = (NSView*)wi.window_handle;
  [view setWantsLayer:TRUE];
  [view setLayer:layer];
  [layer setContentsScale:[[[view window] screen] backingScaleFactor]];

  return layer;
}

void CocoaTools::DestroyMetalLayer(const WindowInfo& wi, void* layer)
{
  // Punt off to main thread if we're not calling from it already.
  if (![NSThread isMainThread])
  {
    dispatch_sync(dispatch_get_main_queue(), [&wi, layer]() { DestroyMetalLayer(wi, layer); });
    return;
  }

  NSView* view = (NSView*)wi.window_handle;
  CAMetalLayer* clayer = (CAMetalLayer*)layer;
  [view setLayer:nil];
  [view setWantsLayer:NO];
  [clayer release];
}

std::optional<float> CocoaTools::GetViewRefreshRate(const WindowInfo& wi, Error* error)
{
  if (![NSThread isMainThread])
  {
    std::optional<float> ret;
    dispatch_sync(dispatch_get_main_queue(), [&ret, wi, error] { ret = GetViewRefreshRate(wi, error); });
    return ret;
  }

  std::optional<float> ret;
  NSView* const view = (NSView*)wi.window_handle;
  const u32 did = [[[[[view window] screen] deviceDescription] valueForKey:@"NSScreenNumber"] unsignedIntValue];
  if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did))
  {
    ret = CGDisplayModeGetRefreshRate(mode);
    CGDisplayModeRelease(mode);
  }
  else
  {
    Error::SetStringView(error, "CGDisplayCopyDisplayMode() failed");
  }

  return ret;
}
