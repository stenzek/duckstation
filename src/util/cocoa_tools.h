// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include <string_view>

struct WindowInfo;

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#import <Cocoa/Cocoa.h>

namespace CocoaTools {
  NSString* StringViewToNSString(const std::string_view& str);
}

#endif

namespace CocoaTools {
  /// Add a handler to be run when macOS changes between dark and light themes
  void AddThemeChangeHandler(void* ctx, void(handler)(void* ctx));

  /// Remove a handler previously added using AddThemeChangeHandler with the given context
  void RemoveThemeChangeHandler(void* ctx);
}

