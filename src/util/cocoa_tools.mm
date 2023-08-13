// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cocoa_tools.h"

NSString* CocoaTools::StringViewToNSString(const std::string_view& str)
{
  if (str.empty())
    return nil;

  return [[[NSString alloc] initWithBytes:str.data()
                                                length:static_cast<NSUInteger>(str.length())
                                              encoding:NSUTF8StringEncoding] autorelease];
}

