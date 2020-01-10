#include "assert.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>

#ifdef Y_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN 1
#include <intrin.h>
#include <tlhelp32.h>
#include <windows.h>
#endif

static std::mutex s_AssertFailedMutex;

static inline void FreezeThreads(void** ppHandle)
{
#ifdef Y_PLATFORM_WINDOWS
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
#ifdef Y_PLATFORM_WINDOWS
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
  std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)", szMessage, szFunction, szFile, uLine);

#ifdef Y_PLATFORM_WINDOWS
  SetConsoleTextAttribute(GetStdHandle(STD_ERROR_HANDLE), FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
  WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), szMsg, Y_strlen(szMsg), NULL, NULL);
  OutputDebugStringA(szMsg);

  Y_strncat(szMsg, sizeof(szMsg),
            "\n\nPress Abort to exit, Retry to break to debugger, or Ignore to attempt to continue.");
  int result = MessageBoxA(NULL, szMsg, NULL, MB_ABORTRETRYIGNORE | MB_ICONERROR);
  if (result == IDRETRY)
    __debugbreak();
  else if (result != IDIGNORE)
    TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
#else
  fputs(szMsg, stderr);
  fputs("\nAborting application.\n", stderr);
  fflush(stderr);
  abort();
#endif

  ResumeThreads(pHandle);
}

void Y_OnPanicReached(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
  std::lock_guard<std::mutex> guard(s_AssertFailedMutex);

  void* pHandle;
  FreezeThreads(&pHandle);

  char szMsg[512];
  std::snprintf(szMsg, sizeof(szMsg), "%s in function %s (%s:%u)", szMessage, szFunction, szFile, uLine);

#ifdef Y_PLATFORM_WINDOWS
  SetConsoleTextAttribute(GetStdHandle(STD_ERROR_HANDLE), FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
  WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), szMsg, Y_strlen(szMsg), NULL, NULL);
  OutputDebugStringA(szMsg);

  Y_strncat(szMsg, sizeof(szMsg),
            "\n\nDo you want to attempt to break into a debugger? Pressing Cancel will abort the application.");
  int result = MessageBoxA(NULL, szMsg, NULL, MB_OKCANCEL | MB_ICONERROR);
  if (result == IDOK)
    __debugbreak();

  TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
#else
  fputs(szMsg, stderr);
  fputs("\nAborting application.\n", stderr);
  fflush(stderr);
  abort();
#endif

  ResumeThreads(pHandle);
}
