// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#if !defined(__APPLE__) && !defined(__ANDROID__)

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
  std::fputs(szMsg, stderr);
  std::fflush(stderr);
  std::abort();
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
  std::fputs(szMsg, stderr);
  std::fflush(stderr);
  std::abort();
#endif
}

#endif // !defined(__APPLE__) && !defined(__ANDROID__)
