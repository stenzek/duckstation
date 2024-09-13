// SPDX-FileCopyrightText: 2002-2023 PCSX2 Dev Team, 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cocoa_tools.h"
#include "assert.h"
#include "error.h"
#include "small_string.h"

#include "fmt/format.h"

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

@interface CommonKVOHelper : NSObject
- (void)addCallback:(void*)ctx run:(void (*)(void*))callback;
- (void)removeCallback:(void*)ctx;
@end

@implementation CommonKVOHelper
{
  std::vector<std::pair<void*, void (*)(void*)>> _callbacks;
}

- (void)addCallback:(void*)ctx run:(void (*)(void*))callback
{
  _callbacks.push_back(std::make_pair(ctx, callback));
}

- (void)removeCallback:(void*)ctx
{
  auto new_end =
    std::remove_if(_callbacks.begin(), _callbacks.end(), [ctx](const auto& entry) { return ctx == entry.first; });
  _callbacks.erase(new_end, _callbacks.end());
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context
{
  for (const auto& callback : _callbacks)
    callback.second(callback.first);
}

@end

static CommonKVOHelper* s_themeChangeHandler;

void CocoaTools::AddThemeChangeHandler(void* ctx, void(handler)(void* ctx))
{
  assert([NSThread isMainThread]);
  if (!s_themeChangeHandler)
  {
    s_themeChangeHandler = [[CommonKVOHelper alloc] init];
    NSApplication* app = [NSApplication sharedApplication];
    [app addObserver:s_themeChangeHandler forKeyPath:@"effectiveAppearance" options:0 context:nil];
  }
  [s_themeChangeHandler addCallback:ctx run:handler];
}

void CocoaTools::RemoveThemeChangeHandler(void* ctx)
{
  assert([NSThread isMainThread]);
  [s_themeChangeHandler removeCallback:ctx];
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

  NSURL* url = [NSURL fileURLWithPath:[[NSBundle mainBundle] bundlePath]];
  if (!url)
    return std::nullopt;

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
        url = (__bridge_transfer NSURL*)actual;
    }
    dlclose(handle);
  }

  return std::string([url fileSystemRepresentation]);
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
