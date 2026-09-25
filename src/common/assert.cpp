// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#if !defined(__APPLE__) && !defined(__ANDROID__)

#include "assert.h"
#include "crash_handler.h"
#include "threading.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32

#include "windows_headers.h"
#include <intrin.h>
#include <tlhelp32.h>

#include <mutex>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic ignored "-Winvalid-noreturn"
#endif

static Threading::Mutex s_assertion_mutex;

[[noreturn]] static void TerminateWithoutDialog()
{
  TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
  std::_Exit(0xBA);
}

static void FreezeThreads(DWORD dialog_thread_id, std::vector<HANDLE>* suspended_threads)
{
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    TerminateWithoutDialog();

  THREADENTRY32 thread_entry{};
  thread_entry.dwSize = sizeof(thread_entry);
  if (!Thread32First(snapshot, &thread_entry))
    TerminateWithoutDialog();

  do
  {
    if (thread_entry.th32OwnerProcessID != GetCurrentProcessId() || thread_entry.th32ThreadID == GetCurrentThreadId() ||
        thread_entry.th32ThreadID == dialog_thread_id)
    {
      continue;
    }

    if (HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, thread_entry.th32ThreadID))
      suspended_threads->push_back(thread);
  } while (Thread32Next(snapshot, &thread_entry));
  CloseHandle(snapshot);

  // Collect handles before suspending anything, so allocation cannot wait on a suspended thread. Suspending arbitrary
  // threads can still deadlock if one owns a process-wide lock needed by the dialog thread, so keep the work performed
  // while they are suspended to a minimum.
  size_t suspended_count = 0;
  for (size_t i = 0; i < suspended_threads->size(); i++)
  {
    HANDLE thread = (*suspended_threads)[i];
    if (SuspendThread(thread) != static_cast<DWORD>(-1))
      (*suspended_threads)[suspended_count++] = thread;
    else
      CloseHandle(thread);
  }
  suspended_threads->resize(suspended_count);
}

static void ResumeThreads(const std::vector<HANDLE>& suspended_threads)
{
  for (HANDLE thread : suspended_threads)
  {
    ResumeThread(thread);
    CloseHandle(thread);
  }
}

namespace {
struct AssertionDialogContext
{
  HANDLE ready_event;
  HANDLE start_event;
  const char* message;
  UINT flags;
  int result = 0;
};
} // namespace

static DWORD WINAPI AssertionDialogThread(void* parameter)
{
  AssertionDialogContext* context = static_cast<AssertionDialogContext*>(parameter);
  SetEvent(context->ready_event);
  if (WaitForSingleObject(context->start_event, INFINITE) == WAIT_OBJECT_0)
    context->result = MessageBoxA(nullptr, context->message, nullptr, context->flags);
  return 0;
}

static int ShowIsolatedMessageBox(const char* message, UINT flags, std::vector<HANDLE>* suspended_threads)
{
  HANDLE ready_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE start_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (!ready_event || !start_event)
  {
    if (ready_event)
      CloseHandle(ready_event);
    if (start_event)
      CloseHandle(start_event);
    return 0;
  }

  AssertionDialogContext context{ready_event, start_event, message, flags};
  DWORD dialog_thread_id;
  HANDLE dialog_thread = CreateThread(nullptr, 0, AssertionDialogThread, &context, 0, &dialog_thread_id);
  if (!dialog_thread)
  {
    CloseHandle(ready_event);
    CloseHandle(start_event);
    return 0;
  }

  // Wait until the dialog thread has started before suspending the other threads.
  const HANDLE ready_handles[] = {ready_event, dialog_thread};
  if (WaitForMultipleObjects(2, ready_handles, FALSE, INFINITE) != WAIT_OBJECT_0)
    TerminateWithoutDialog();

  FreezeThreads(dialog_thread_id, suspended_threads);
  if (!SetEvent(start_event) || WaitForSingleObject(dialog_thread, INFINITE) != WAIT_OBJECT_0)
    TerminateWithoutDialog();

  CloseHandle(dialog_thread);
  CloseHandle(ready_event);
  CloseHandle(start_event);
  return context.result;
}

#endif // _WIN32

void Y_OnAssertFailed(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
#if defined(_WIN32)
  std::lock_guard lock(s_assertion_mutex);
#endif

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

  std::vector<HANDLE> suspended_threads;
  const int result =
    ShowIsolatedMessageBox(szMsg, MB_ABORTRETRYIGNORE | MB_ICONERROR | MB_SETFOREGROUND, &suspended_threads);
  if (result == IDRETRY)
  {
    __debugbreak();
  }
  else if (result != IDIGNORE)
  {
    CrashHandler::WriteDumpForCaller(szMsg);
    TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
    std::_Exit(0xBA);
  }
  ResumeThreads(suspended_threads);

#else
  std::fputs(szMsg, stderr);
  std::fflush(stderr);
  std::abort();
#endif
}

[[noreturn]] void Y_OnPanicReached(const char* szMessage, const char* szFunction, const char* szFile, unsigned uLine)
{
#if defined(_WIN32)
  std::lock_guard lock(s_assertion_mutex);
#endif

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

  std::vector<HANDLE> suspended_threads;
  const int result = ShowIsolatedMessageBox(szMsg, MB_OKCANCEL | MB_ICONERROR | MB_SETFOREGROUND, &suspended_threads);
  if (result == IDOK)
    __debugbreak();
  else
    CrashHandler::WriteDumpForCaller(szMsg);

  TerminateProcess(GetCurrentProcess(), 0xBAADC0DE);
  std::_Exit(0xBA);

#else
  std::fputs(szMsg, stderr);
  std::fflush(stderr);
  std::abort();
#endif
}

#endif // !defined(__APPLE__) && !defined(__ANDROID__)
