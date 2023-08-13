// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "platform_misc.h"
#include "common/log.h"
#include "common/string.h"
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <Cocoa/Cocoa.h>
#include <QuartzCore/QuartzCore.h>
#include <cinttypes>
#include <vector>
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

void FrontendCommon::SuspendScreensaver()
{
  if (s_screensaver_suspended)

  if (!SetScreensaverInhibitMacOS(true))
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

  if (!SetScreensaverInhibitMacOS(false))
    Log_ErrorPrint("Failed to resume screensaver.");

  s_screensaver_suspended = false;
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

// From https://github.com/PCSX2/pcsx2/blob/1b673d9dd0829a48f5f0b6604c1de2108e981399/common/CocoaTools.mm

@interface PCSX2KVOHelper : NSObject

- (void)addCallback:(void*)ctx run:(void(*)(void*))callback;
- (void)removeCallback:(void*)ctx;

@end

@implementation PCSX2KVOHelper
{
	std::vector<std::pair<void*, void(*)(void*)>> _callbacks;
}

- (void)addCallback:(void*)ctx run:(void(*)(void*))callback
{
	_callbacks.push_back(std::make_pair(ctx, callback));
}

- (void)removeCallback:(void*)ctx
{
	auto new_end = std::remove_if(_callbacks.begin(), _callbacks.end(), [ctx](const auto& entry){
		return ctx == entry.first;
	});
	_callbacks.erase(new_end, _callbacks.end());
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary<NSKeyValueChangeKey,id> *)change context:(void *)context
{
	for (const auto& callback : _callbacks)
		callback.second(callback.first);
}

@end

static PCSX2KVOHelper* s_themeChangeHandler;

void FrontendCommon::AddThemeChangeHandler(void* ctx, void(handler)(void* ctx))
{
	assert([NSThread isMainThread]);
	if (!s_themeChangeHandler)
	{
		s_themeChangeHandler = [[PCSX2KVOHelper alloc] init];
		NSApplication* app = [NSApplication sharedApplication];
		[app addObserver:s_themeChangeHandler
		      forKeyPath:@"effectiveAppearance"
		         options:0
		         context:nil];
	}
	[s_themeChangeHandler addCallback:ctx run:handler];
}

void FrontendCommon::RemoveThemeChangeHandler(void* ctx)
{
	assert([NSThread isMainThread]);
	[s_themeChangeHandler removeCallback:ctx];
}
