// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

class Error;

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>

namespace CocoaTools {
NSString* StringViewToNSString(std::string_view str);
void NSErrorToErrorObject(Error* errptr, std::string_view message, NSError* error);

/// Converts NSError to a human-readable string.
std::string NSErrorToString(NSError* error);
} // namespace CocoaTools

#endif

namespace CocoaTools {
// Converts to Mach timebase.
u64 ConvertMachTimeBaseToNanoseconds(u64 ns);
u64 ConvertNanosecondsToMachTimeBase(u64 ns);

/// Add a handler to be run when macOS changes between dark and light themes
void AddThemeChangeHandler(void* ctx, void(handler)(void* ctx));

/// Remove a handler previously added using AddThemeChangeHandler with the given context
void RemoveThemeChangeHandler(void* ctx);

/// Moves a file from one location to another, using NSFileManager.
bool MoveFile(const char* source, const char* destination, Error* error);

/// Returns the bundle path.
std::optional<std::string> GetBundlePath();

/// Get the bundle path to the actual application without any translocation fun
std::optional<std::string> GetNonTranslocatedBundlePath();

/// Launch the given application once this one quits
bool DelayedLaunch(std::string_view file, std::span<const std::string_view> args = {});
} // namespace CocoaTools
