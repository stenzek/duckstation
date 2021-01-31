#include "crash_handler.h"
#include "file_system.h"
#include "string_util.h"
#include <cinttypes>
#include <cstdio>

#ifdef _WIN32
#include "thirdparty/StackWalker.h"
#include "windows_headers.h"

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

static std::wstring s_write_directory;
static PVOID s_veh_handle;

static LONG ExceptionHandler(PEXCEPTION_POINTERS exi)
{
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

  CrashHandlerStackWalker sw(hFile);
  sw.ShowCallstack(GetCurrentThread(), exi->ContextRecord);
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