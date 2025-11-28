// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#ifndef __APPLE__
#error This file should only be included when compiling for MacOS.
#endif

#include "types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

/// Moves a file from one location to another, using NSFileManager.
bool MoveFile(const char* source, const char* destination, Error* error);

/// Returns the bundle path.
std::optional<std::string> GetBundlePath();

/// Get the bundle path to the actual application without any translocation fun
std::optional<std::string> GetNonTranslocatedBundlePath();

/// Launch the given application once this one quits
bool DelayedLaunch(std::string_view file, std::span<const std::string_view> args = {});

/// Returns the size of a NSView in pixels.
std::optional<std::pair<int, int>> GetViewSizeInPixels(const void* view);

/// Returns the "real" scaling factor for a given view, on its current display.
std::optional<double> GetViewRealScalingFactor(const void* view);
} // namespace CocoaTools
