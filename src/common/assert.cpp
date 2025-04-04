// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "assert.h"
#include "crash_handler.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32

#include "windows_headers.h"
#include <intrin.h>
#include <tlhelp32.h>

#include <mutex>

#ifdef __clang__
#pragma clang diagnostic ignored "-Winvalid-noreturn"
#endif

static std::mutex s_AssertFailedMutex;

static HANDLE FreezeThreads()
{
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (hSnapshot != INVALID_HANDLE_VALUE)
  {
    THREADENTRY32 threadEntry;
    if (Thread32First(hSnapshot, &threadEntry))
    {
      do
      {
        if (threadEntry.th32ThreadID == GetCurrentThreadId())
          continue;

        HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.th32ThreadID);
        if (hThread != NULL)
        {
          SuspendThread(hThread);
          CloseHandle(hThread);
        }
      } while (Thread32Next(hSnapshot, &threadEntry));
    }
  }

  return hSnapshot;
}

static void ResumeThreads(HANDLE hSnapshot)
{
  if (hSnapshot != INVALID_HANDLE_VALUE)
  {
    THREADENTRY32 threadEntry;
    if (Thread32First(hSnapshot, &threadEntry))
    {
      do
      {
        if (threadEntry.th32ThreadID == GetCurrentThreadId())
          continue;

        HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadEntry.th32ThreadID);
        if (hThread != NULL)
        {
          ResumeThread(hThread);
          CloseHandle(hThread);
        }
      } while (Thread32Next(hSnapshot, &threadEntry));
    }
    CloseHandle(hSnapshot);
  }
}

#else

#ifdef __ANDROID__
// Define as a weak symbol for ancient devices that don't have it.
extern "C" __attribute__((weak)) void android_set_abort_message(const char*);
#endif

[[noreturn]] ALWAYS_INLINE static void AbortWithMessage(const char* szMsg)
{
#ifndef __ANDROID__
  std::fputs(szMsg, stderr);
  CrashHandler::WriteDumpForCaller(szMsg);
  std::fputs("Aborting application.\n", stderr);
  std::fflush(stderr);
  std::abort();
#else
  if (&android_set_abort_message)
    android_set_abort_message(szMsg);

  std::abort();
#endif
}

#endif // _WIN32

void Y_OnAssertFailed(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
  char szMsg[512];
  std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)\n", szMessage, szFunction, szFile, uLine);

#if defined(_WIN32)
  std::unique_lock lock(s_AssertFailedMutex);
  HANDLE pHandle = FreezeThreads();

  SetConsoleTextAttribute(GetStdHandle(STD_ERROR_HANDLE), FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
  WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), szMsg, static_cast<DWORD>(std::strlen(szMsg)), NULL, NULL);
  OutputDebugStringA(szMsg);

  std::snprintf(
    szMsg, sizeof(szMsg),
    "%s in function %s (%s:%u)\nPress Abort to exit, Retry to break to debugger, or Ignore to attempt to continue.",
    szMessage, szFunction, szFile, uLine);

  int result = MessageBoxA(NULL, szMsg, NULL, MB_ABORTRETRYIGNORE | MB_ICONERROR);
  if (result == IDRETRY)
  {
    __debugbreak();
  }
  else if (result != IDIGNORE)
  {
    CrashHandler::WriteDumpForCaller(szMsg);
    TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
  }

  ResumeThreads(pHandle);
#else
  AbortWithMessage(szMsg);
#endif
}

[[noreturn]] void Y_OnPanicReached(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
  char szMsg[512];
  std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)\n", szMessage, szFunction, szFile, uLine);

#if defined(_WIN32)
  std::unique_lock guard(s_AssertFailedMutex);
  HANDLE pHandle = FreezeThreads();

  SetConsoleTextAttribute(GetStdHandle(STD_ERROR_HANDLE), FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
  WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), szMsg, static_cast<DWORD>(std::strlen(szMsg)), NULL, NULL);
  OutputDebugStringA(szMsg);

  std::snprintf(szMsg, sizeof(szMsg),
                "%s in function %s (%s:%u)\nDo you want to attempt to break into a debugger? Pressing Cancel will "
                "abort the application.",
                szMessage, szFunction, szFile, uLine);

  int result = MessageBoxA(NULL, szMsg, NULL, MB_OKCANCEL | MB_ICONERROR);
  if (result == IDOK)
    __debugbreak();
  else
    CrashHandler::WriteDumpForCaller(szMsg);

  TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);

  ResumeThreads(pHandle);
#else
  AbortWithMessage(szMsg);
#endif
}
