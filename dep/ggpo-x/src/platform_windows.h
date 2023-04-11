/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _GGPO_WINDOWS_H_
#define _GGPO_WINDOWS_H_

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include "types.h"

class Platform {
public:  // types
   typedef DWORD ProcessID;

public:  // functions
   static ProcessID GetProcessID() { return GetCurrentProcessId(); }
   static void AssertFailed(char *msg) { MessageBoxA(NULL, msg, "GGPO Assertion Failed", MB_OK | MB_ICONEXCLAMATION); }
   static uint32 GetCurrentTimeMS() { return timeGetTime(); }
   static int GetConfigInt(const char* name);
   static bool GetConfigBool(const char* name);
};

#endif
