#include "crash_handler.h"
#include "file_system.h"
#include "string_util.h"
#include <cinttypes>
#include <cstdio>

#if defined(_WIN32)
#include "windows_headers.h"

#include "thirdparty/StackWalker.h"
#include <DbgHelp.h>

namespace CrashHandler {

class CrashHandlerStackWalker : public StackWalker
{
public:
  CrashHandlerStackWalker(HANDLE out_file);
  ~CrashHandlerStackWalker();

protected:
  void OnOutput(LPCSTR szText) override;

private:
  HANDLE m_out_file;
};

CrashHandlerStackWalker::CrashHandlerStackWalker(HANDLE out_file)
  : StackWalker(RetrieveVerbose, nullptr, GetCurrentProcessId(), GetCurrentProcess()), m_out_file(out_file)
{
}

CrashHandlerStackWalker::~CrashHandlerStackWalker()
{
  if (m_out_file)
    CloseHandle(m_out_file);
}

void CrashHandlerStackWalker::OnOutput(LPCSTR szText)
{
  if (m_out_file)
  {
    DWORD written;
    WriteFile(m_out_file, szText, static_cast<DWORD>(std::strlen(szText)), &written, nullptr);
  }

  OutputDebugStringA(szText);
}

static bool WriteMinidump(HMODULE hDbgHelp, HANDLE hFile, HANDLE hProcess, DWORD process_id, DWORD thread_id,
                          PEXCEPTION_POINTERS exception, MINIDUMP_TYPE type)
{
  using PFNMINIDUMPWRITEDUMP =
    BOOL(WINAPI*)(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                  PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                  PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

  PFNMINIDUMPWRITEDUMP minidump_write_dump =
    reinterpret_cast<PFNMINIDUMPWRITEDUMP>(GetProcAddress(hDbgHelp, "MiniDumpWriteDump"));
  if (!minidump_write_dump)
    return false;

  MINIDUMP_EXCEPTION_INFORMATION mei;
  PMINIDUMP_EXCEPTION_INFORMATION mei_ptr = nullptr;
  if (exception)
  {
    mei.ThreadId = thread_id;
    mei.ExceptionPointers = exception;
    mei.ClientPointers = FALSE;
    mei_ptr = &mei;
  }

  return minidump_write_dump(hProcess, process_id, hFile, type, mei_ptr, nullptr, nullptr);
}

static std::wstring s_write_directory;
static PVOID s_veh_handle = nullptr;
static bool s_in_crash_handler = false;

static LONG NTAPI ExceptionHandler(PEXCEPTION_POINTERS exi)
{
  if (s_in_crash_handler)
    return EXCEPTION_CONTINUE_SEARCH;

  switch (exi->ExceptionRecord->ExceptionCode)
  {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_GUARD_PAGE:
      break;

    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }

  // if the debugger is attached, let it take care of it.
  if (IsDebuggerPresent())
    return EXCEPTION_CONTINUE_SEARCH;

  s_in_crash_handler = true;

  // we definitely need dbg helper - maintain an extra reference here
  HMODULE hDbgHelp = StackWalker::LoadDbgHelpLibrary();

  wchar_t filename[1024] = {};
  if (!s_write_directory.empty())
  {
    wcsncpy_s(filename, countof(filename), s_write_directory.c_str(), _TRUNCATE);
    wcsncat_s(filename, countof(filename), L"\\crash.txt", _TRUNCATE);
  }
  else
  {
    wcsncat_s(filename, countof(filename), L"crash.txt", _TRUNCATE);
  }

  // might fail
  HANDLE hFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (hFile)
  {
    char line[1024];
    DWORD written;
    std::snprintf(line, countof(line), "Exception 0x%08X at 0x%p\n", exi->ExceptionRecord->ExceptionCode,
                  exi->ExceptionRecord->ExceptionAddress);
    WriteFile(hFile, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
  }

  if (!s_write_directory.empty())
  {
    wcsncpy_s(filename, countof(filename), s_write_directory.c_str(), _TRUNCATE);
    wcsncat_s(filename, countof(filename), L"\\crash.dmp", _TRUNCATE);
  }
  else
  {
    wcsncat_s(filename, countof(filename), L"crash.dmp", _TRUNCATE);
  }

  const MINIDUMP_TYPE minidump_type =
    static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithHandleData | MiniDumpWithProcessThreadData |
                               MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
  HANDLE hMinidumpFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (!hMinidumpFile || !WriteMinidump(hDbgHelp, hMinidumpFile, GetCurrentProcess(), GetCurrentProcessId(),
                                       GetCurrentThreadId(), exi, minidump_type))
  {
    static const char error_message[] = "Failed to write minidump file.\n";
    if (hFile)
    {
      DWORD written;
      WriteFile(hFile, error_message, sizeof(error_message) - 1, &written, nullptr);
    }
  }
  if (hMinidumpFile)
    CloseHandle(hMinidumpFile);

  CrashHandlerStackWalker sw(hFile);
  sw.ShowCallstack(GetCurrentThread(), exi->ContextRecord);

  if (hFile)
    CloseHandle(hFile);

  if (hDbgHelp)
    FreeLibrary(hDbgHelp);

  return EXCEPTION_CONTINUE_SEARCH;
}

bool Install()
{
  s_veh_handle = AddVectoredExceptionHandler(0, ExceptionHandler);
  return (s_veh_handle != nullptr);
}

void SetWriteDirectory(const std::string_view& dump_directory)
{
  if (!s_veh_handle)
    return;

  s_write_directory = StringUtil::UTF8StringToWideString(dump_directory);
}

void Uninstall()
{
  if (s_veh_handle)
  {
    RemoveVectoredExceptionHandler(s_veh_handle);
    s_veh_handle = nullptr;
  }
}

} // namespace CrashHandler

#else

namespace CrashHandler {

bool Install()
{
  return false;
}

void SetWriteDirectory(const std::string_view& dump_directory) {}

void Uninstall() {}

} // namespace CrashHandler

#endif