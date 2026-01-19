// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team, 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cocoa_tools.h"
#include "assert.h"
#include "error.h"
#include "log.h"
#include "small_string.h"

#include "fmt/format.h"

#include <AppKit/AppKit.h>
#include <Cocoa/Cocoa.h>
#include <QuartzCore/QuartzCore.h>
#include <cinttypes>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <vector>

#if __has_feature(objc_arc)
#error ARC should not be enabled.
#endif

NSString* CocoaTools::StringViewToNSString(std::string_view str)
{
  if (str.empty())
    return nil;

  return [[[NSString alloc] initWithBytes:str.data()
                                   length:static_cast<NSUInteger>(str.length())
                                 encoding:NSUTF8StringEncoding] autorelease];
}

std::string CocoaTools::NSErrorToString(NSError* error)
{
  return fmt::format("{}: {}", static_cast<u32>(error.code), [error.description UTF8String]);
}

void CocoaTools::NSErrorToErrorObject(Error* errptr, std::string_view message, NSError* error)
{
  Error::SetStringFmt(errptr, "{}NSError Code {}: {}", message, static_cast<u32>(error.code),
                      [error.description UTF8String]);
}

bool CocoaTools::MoveFile(const char* source, const char* destination, Error* error)
{
  @autoreleasepool
  {
    NSError* nserror;
    const BOOL result = [[NSFileManager defaultManager] moveItemAtPath:[NSString stringWithUTF8String:source]
                                                                toPath:[NSString stringWithUTF8String:destination]
                                                                 error:&nserror];
    if (!result)
    {
      Error::SetString(error, NSErrorToString(nserror));
      return false;
    }

    return true;
  }
}

// Used for present timing.
static const struct mach_timebase_info s_timebase_info = []() {
  struct mach_timebase_info val;
  const kern_return_t res = mach_timebase_info(&val);
  Assert(res == KERN_SUCCESS);
  return val;
}();

u64 CocoaTools::ConvertMachTimeBaseToNanoseconds(u64 time)
{
  return ((time * s_timebase_info.numer) / s_timebase_info.denom);
}

u64 CocoaTools::ConvertNanosecondsToMachTimeBase(u64 time)
{
  return ((time * s_timebase_info.denom) / s_timebase_info.numer);
}

std::optional<std::string> CocoaTools::GetBundlePath()
{
  std::optional<std::string> ret;
  @autoreleasepool
  {
    NSURL* url = [NSURL fileURLWithPath:[[NSBundle mainBundle] bundlePath]];
    if (url)
      ret = std::string([url fileSystemRepresentation]);
  }
  return ret;
}

std::optional<std::string> CocoaTools::GetNonTranslocatedBundlePath()
{
  // See https://objective-see.com/blog/blog_0x15.html
  std::optional<std::string> ret;
  @autoreleasepool
  {
    NSURL* url = [NSURL fileURLWithPath:[[NSBundle mainBundle] bundlePath]];
    if (!url)
      return ret;

    if (void* handle = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY))
    {
      auto IsTranslocatedURL =
        reinterpret_cast<Boolean (*)(CFURLRef path, bool* isTranslocated, CFErrorRef* __nullable error)>(
          dlsym(handle, "SecTranslocateIsTranslocatedURL"));
      auto CreateOriginalPathForURL =
        reinterpret_cast<CFURLRef __nullable (*)(CFURLRef translocatedPath, CFErrorRef* __nullable error)>(
          dlsym(handle, "SecTranslocateCreateOriginalPathForURL"));
      bool is_translocated = false;
      if (IsTranslocatedURL)
        IsTranslocatedURL((__bridge CFURLRef)url, &is_translocated, nullptr);
      if (is_translocated)
      {
        if (CFURLRef actual = CreateOriginalPathForURL((__bridge CFURLRef)url, nullptr))
          url = (__bridge NSURL*)actual;
      }
      dlclose(handle);
    }

    ret = std::string([url fileSystemRepresentation]);
  }

  return ret;
}

bool CocoaTools::DelayedLaunch(std::string_view file, std::span<const std::string_view> args)
{
  @autoreleasepool
  {
    const int pid = [[NSProcessInfo processInfo] processIdentifier];

    // Hopefully we're not too large here...
    std::string task_args =
      fmt::format("while /bin/ps -p {} > /dev/null; do /bin/sleep 0.1; done; exec /usr/bin/open \"{}\"", pid, file);
    if (!args.empty())
    {
      task_args += " --args";
      for (const std::string_view& arg : args)
      {
        task_args += " \"";
        task_args += arg;
        task_args += "\"";
      }
    }

    NSTask* task = [NSTask new];
    [task setExecutableURL:[NSURL fileURLWithPath:@"/bin/sh"]];
    [task setArguments:@[ @"-c", [NSString stringWithUTF8String:task_args.c_str()] ]];
    return [task launchAndReturnError:nil];
  }
}

std::optional<std::pair<int, int>> CocoaTools::GetViewSizeInPixels(const void* view)
{
  std::optional<std::pair<int, int>> ret;
  if (view)
  {
    NSView* nsview = (__bridge NSView*)view;
    const NSSize size = [nsview convertSizeToBacking:nsview.frame.size];
    ret = std::make_pair(static_cast<int>(size.width), static_cast<int>(size.height));
  }

  return ret;
}

std::optional<double> CocoaTools::GetViewRealScalingFactor(const void* view)
{
  if (!view)
    return std::nullopt;

  NSView* const nsview = (__bridge NSView*)view;
  NSWindow* const nswindow = nsview.window;
  if (nswindow == nil)
    return std::nullopt;

  NSScreen* const nsscreen = nswindow.screen;
  if (nsscreen == nil)
    return std::nullopt;

  const u32 did = [[nsscreen.deviceDescription valueForKey:@"NSScreenNumber"] unsignedIntValue];
  const NSArray* all_modes = (__bridge NSArray*)CGDisplayCopyAllDisplayModes(did, nil);
  if (all_modes == nil)
  {
    GENERIC_LOG(Log::Channel::WindowInfo, Log::Level::Dev, Log::Color::Default,
                "GetViewRealScalingFactor(): CGDisplayCopyAllDisplayModes() failed");
    return std::nullopt;
  }

  u32 max_width = 0;
  for (NSUInteger i = 0; i < all_modes.count; i++)
    max_width = std::max(max_width, static_cast<u32>(CGDisplayModeGetPixelWidth((CGDisplayModeRef)all_modes[i])));
  CFRelease(all_modes);
  if (max_width == 0)
  {
    GENERIC_LOG(Log::Channel::WindowInfo, Log::Level::Dev, Log::Color::Default,
                "GetViewRealScalingFactor(): Max width is zero");
    return std::nullopt;
  }

  // Sanity check: Scale should not be less than 100%, and cannot be more than 200%.
  const CGFloat frame_width = nsscreen.frame.size.width;
  const CGFloat scale = static_cast<CGFloat>(max_width) / frame_width;
  GENERIC_LOG(Log::Channel::WindowInfo, Log::Level::Dev, Log::Color::Default,
              "GetViewRealScalingFactor(): MaxWidth={}, FrameWidth={}, Scale={}", max_width, frame_width, scale);
  if (scale < 1.0f)
    return std::nullopt;

  return static_cast<double>(scale);
}

std::optional<float> CocoaTools::GetViewRefreshRate(const void* view, Error* error)
{
  if (!view)
    return std::nullopt;

  if (![NSThread isMainThread])
  {
    std::optional<float> ret;
    dispatch_sync(dispatch_get_main_queue(), [&ret, view, error] { ret = GetViewRefreshRate(view, error); });
    return ret;
  }

  std::optional<float> ret;
  NSView* const nsview = (__bridge NSView*)view;
  const u32 did = [[[[[nsview window] screen] deviceDescription] valueForKey:@"NSScreenNumber"] unsignedIntValue];
  if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did))
  {
    ret = CGDisplayModeGetRefreshRate(mode);
    if (ret.value() <= 0.0f)
    {
      ret.reset();

      // Ignore deprecration warnings here. The new APIs don't seem to have something that matches the semantics.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

      CVDisplayLinkRef link = nullptr;
      if (CVDisplayLinkCreateWithCGDisplay(did, &link) == 0)
      {
        const CVTime time = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
        if (!(time.flags & kCVTimeIsIndefinite) && time.timeValue != 0)
        {
          ret = static_cast<float>(static_cast<double>(time.timeScale) / static_cast<double>(time.timeValue));
        }
        else
        {
          Error::SetStringFmt(error, "Refresh period is invalid (flags=0x{:X}, timeValue={}, timeScale={})",
                              static_cast<u32>(time.flags), time.timeValue, time.timeScale);
        }
      }
      else
      {
        Error::SetStringView(error, "CVDisplayLinkCreateWithCGDisplay() failed");
      }
#pragma clang diagnostic pop
    }
    CGDisplayModeRelease(mode);
  }
  else
  {
    Error::SetStringView(error, "CGDisplayCopyDisplayMode() failed");
  }

  return ret;
}

void* CocoaTools::CreateMetalLayer(void* view, Error* error)
{
  // Punt off to main thread if we're not calling from it already.
  if (![NSThread isMainThread])
  {
    void* ret;
    dispatch_sync(dispatch_get_main_queue(), [&ret, view, error]() { ret = CreateMetalLayer(view, error); });
    return ret;
  }

  CAMetalLayer* layer = [[CAMetalLayer layer] retain];
  if (layer == nil)
  {
    Error::SetStringView(error, "Failed to create CAMetalLayer");
    return nullptr;
  }

  NSView* nsview = (__bridge NSView*)view;
  [nsview setWantsLayer:TRUE];
  [nsview setLayer:layer];
  [layer setContentsScale:[[[nsview window] screen] backingScaleFactor]];

  return layer;
}

void CocoaTools::DestroyMetalLayer(void* view, void* layer)
{
  // Punt off to main thread if we're not calling from it already.
  if (![NSThread isMainThread])
  {
    dispatch_sync(dispatch_get_main_queue(), [view, layer]() { DestroyMetalLayer(view, layer); });
    return;
  }

  NSView* nsview = (__bridge NSView*)view;
  CAMetalLayer* clayer = (CAMetalLayer*)layer;
  [nsview setLayer:nil];
  [nsview setWantsLayer:NO];
  [clayer release];
}

void Y_OnAssertFailed(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
  if (![NSThread isMainThread])
  {
    dispatch_sync(dispatch_get_main_queue(),
                  [szMessage, szFunction, szFile, uLine]() { Y_OnAssertFailed(szMessage, szFunction, szFile, uLine); });
    return;
  }

  char szMsg[512];
  std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)\n", szMessage, szFunction, szFile, uLine);
  std::fputs(szMsg, stderr);
  std::fflush(stderr);

  @autoreleasepool
  {
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:@"Assertion Failed"];

    NSString* text = [NSString stringWithFormat:@"%s in function %s (%s:%u)\nPress Abort to exit, Break to break to "
                                                @"debugger, or Ignore to attempt to continue.",
                                                szMessage, szFunction, szFile, uLine];
    [alert setInformativeText:text];
    [alert setAlertStyle:NSAlertStyleCritical];
    [alert addButtonWithTitle:@"Abort"];
    [alert addButtonWithTitle:@"Break"];
    [alert addButtonWithTitle:@"Ignore"];

    const NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn)
      std::abort();
    else if (response == NSAlertSecondButtonReturn)
      __builtin_debugtrap();
  }
}

[[noreturn]] void Y_OnPanicReached(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
  if (![NSThread isMainThread])
  {
    dispatch_sync(dispatch_get_main_queue(),
                  [szMessage, szFunction, szFile, uLine]() { Y_OnAssertFailed(szMessage, szFunction, szFile, uLine); });
  }
  else
  {
    char szMsg[512];
    std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)\n", szMessage, szFunction, szFile, uLine);

    @autoreleasepool
    {
      NSAlert* alert = [[[NSAlert alloc] init] autorelease];
      [alert setMessageText:@"Critical Error"];

      NSString* text =
        [NSString stringWithFormat:@"%s in function %s (%s:%u)\nDo you want to attempt to break into a debugger?",
                                   szMessage, szFunction, szFile, uLine];
      [alert setInformativeText:text];
      [alert setAlertStyle:NSAlertStyleCritical];
      [alert addButtonWithTitle:@"Abort"];
      [alert addButtonWithTitle:@"Break"];

      const NSModalResponse response = [alert runModal];
      if (response == NSAlertSecondButtonReturn)
        __builtin_debugtrap();
    }
  }

  std::abort();
}
