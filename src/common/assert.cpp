// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "assert.h"
#include "crash_handler.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#include "windows_headers.h"
#include <intrin.h>
#include <tlhelp32.h>
#endif

#ifdef __clang__
#pragma clang diagnostic ignored "-Winvalid-noreturn"
#endif

static std::mutex s_AssertFailedMutex;

static inline void FreezeThreads(void** ppHandle)
{
#if defined(_WIN32)
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

  *ppHandle = (void*)hSnapshot;
#else
  *ppHandle = nullptr;
#endif
}

static inline void ResumeThreads(void* pHandle)
{
#if defined(_WIN32)
  HANDLE hSnapshot = (HANDLE)pHandle;
  if (pHandle != INVALID_HANDLE_VALUE)
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
#else
#endif
}

void Y_OnAssertFailed(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
  std::lock_guard<std::mutex> guard(s_AssertFailedMutex);

  void* pHandle;
  FreezeThreads(&pHandle);

  char szMsg[512];
  std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)\n", szMessage, szFunction, szFile, uLine);

#if defined(_WIN32)
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
    CrashHandler::WriteDumpForCaller();
    TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
  }
#else
  std::fputs(szMsg, stderr);
  CrashHandler::WriteDumpForCaller();
  std::fputs("Aborting application.\n", stderr);
  std::fflush(stderr);
  std::abort();
#endif

  ResumeThreads(pHandle);
}

[[noreturn]] void Y_OnPanicReached(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
  std::lock_guard<std::mutex> guard(s_AssertFailedMutex);

  void* pHandle;
  FreezeThreads(&pHandle);

  char szMsg[512];
  std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)\n", szMessage, szFunction, szFile, uLine);

#if defined(_WIN32)
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
    CrashHandler::WriteDumpForCaller();

  TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
#else
  std::fputs(szMsg, stderr);
  CrashHandler::WriteDumpForCaller();
  std::fputs("Aborting application.\n", stderr);
  std::fflush(stderr);
  std::abort();
#endif

  ResumeThreads(pHandle);
}
