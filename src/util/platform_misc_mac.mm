// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Normally, system includes come last. But apparently some of our macro names are redefined...
#include <Cocoa/Cocoa.h>
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

bool PlatformMisc::InitializeSocketSupport(Error* error)
{
  return true;
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
