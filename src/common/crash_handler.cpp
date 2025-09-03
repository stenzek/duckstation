// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "crash_handler.h"
#include "dynamic_library.h"
#include "file_system.h"
#include "string_util.h"
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <ctime>

#if defined(_WIN32)
#include "windows_headers.h"

#include "thirdparty/StackWalker.h"
#include <DbgHelp.h>

namespace {
class CrashHandlerStackWalker : public StackWalker
{
public:
  explicit CrashHandlerStackWalker(HANDLE out_file);
  ~CrashHandlerStackWalker();

protected:
  void OnOutput(LPCSTR szText) override;

private:
  HANDLE m_out_file;
};
} // namespace

CrashHandlerStackWalker::CrashHandlerStackWalker(HANDLE out_file)
  : StackWalker(RetrieveVerbose, nullptr, GetCurrentProcessId(), GetCurrentProcess()), m_out_file(out_file)
{
}

CrashHandlerStackWalker::~CrashHandlerStackWalker() = default;

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
    hDbgHelp ?
      reinterpret_cast<PFNMINIDUMPWRITEDUMP>(reinterpret_cast<void*>(GetProcAddress(hDbgHelp, "MiniDumpWriteDump"))) :
      nullptr;
  if (!minidump_write_dump)
    return false;

  MINIDUMP_EXCEPTION_INFORMATION mei = {};
  if (exception)
  {
    mei.ThreadId = thread_id;
    mei.ExceptionPointers = exception;
    mei.ClientPointers = FALSE;
    return minidump_write_dump(hProcess, process_id, hFile, type, &mei, nullptr, nullptr);
  }

  __try
  {
    RaiseException(EXCEPTION_INVALID_HANDLE, 0, 0, nullptr);
  }
  __except (WriteMinidump(hDbgHelp, hFile, GetCurrentProcess(), GetCurrentProcessId(), GetCurrentThreadId(),
                          GetExceptionInformation(), type),
            EXCEPTION_EXECUTE_HANDLER)
  {
  }

  return true;
}

static std::wstring s_write_directory;
static DynamicLibrary s_dbghelp_module;
static CrashHandler::CleanupHandler s_cleanup_handler;
static bool s_in_crash_handler = false;

static void GenerateCrashFilename(wchar_t* buf, size_t len, const wchar_t* prefix, const wchar_t* extension)
{
  SYSTEMTIME st = {};
  GetLocalTime(&st);

  _snwprintf_s(buf, len, _TRUNCATE, L"%s%scrash-%04u-%02u-%02u-%02u-%02u-%02u-%03u.%s", prefix ? prefix : L"",
               prefix ? L"\\" : L"", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               extension);
}

static void WriteMinidumpAndCallstack(PEXCEPTION_POINTERS exi, const std::string_view message)
{
  wchar_t filename[1024] = {};
  GenerateCrashFilename(filename, std::size(filename), s_write_directory.empty() ? nullptr : s_write_directory.c_str(),
                        L"txt");

  // might fail
  HANDLE hFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  DWORD written;

  if (!message.empty() && hFile != INVALID_HANDLE_VALUE)
  {
    const char newline = '\n';
    WriteFile(hFile, message.data(), static_cast<DWORD>(message.length()), &written, nullptr);
    WriteFile(hFile, &newline, sizeof(newline), &written, nullptr);
  }

  GenerateCrashFilename(filename, std::size(filename), s_write_directory.empty() ? nullptr : s_write_directory.c_str(),
                        L"dmp");

  const MINIDUMP_TYPE minidump_type =
    static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithHandleData | MiniDumpWithProcessThreadData |
                               MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
  const HANDLE hMinidumpFile = CreateFileW(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (hMinidumpFile == INVALID_HANDLE_VALUE ||
      !WriteMinidump(static_cast<HMODULE>(s_dbghelp_module.GetHandle()), hMinidumpFile, GetCurrentProcess(),
                     GetCurrentProcessId(), GetCurrentThreadId(), exi, minidump_type))
  {
    static const char error_message[] = "Failed to write minidump file.\n";
    if (hFile != INVALID_HANDLE_VALUE)
      WriteFile(hFile, error_message, sizeof(error_message) - 1, &written, nullptr);
  }
  if (hMinidumpFile != INVALID_HANDLE_VALUE)
    CloseHandle(hMinidumpFile);

  CrashHandlerStackWalker sw(hFile);
  sw.ShowCallstack(GetCurrentThread(), exi ? exi->ContextRecord : nullptr);

  if (hFile != INVALID_HANDLE_VALUE)
    CloseHandle(hFile);
}

static LONG NTAPI ExceptionHandler(PEXCEPTION_POINTERS exi)
{
  // if the debugger is attached, or we're recursively crashing, let it take care of it.
  if (!s_in_crash_handler)
  {
    s_in_crash_handler = true;
    if (s_cleanup_handler)
      s_cleanup_handler();

    char message[128];
    std::snprintf(message, std::size(message), "Exception 0x%08X at 0x%p",
                  static_cast<unsigned>(exi->ExceptionRecord->ExceptionCode), exi->ExceptionRecord->ExceptionAddress);

    WriteMinidumpAndCallstack(exi, message);
  }

  // returning EXCEPTION_CONTINUE_SEARCH makes sense, except for the fact that it seems to leave zombie processes
  // around. instead, force ourselves to terminate.
  TerminateProcess(GetCurrentProcess(), 0xFEFEFEFEu);
  return EXCEPTION_CONTINUE_SEARCH;
}

static void InvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                                    unsigned int line, uintptr_t pReserved)
{
  // if the debugger is attached, or we're recursively crashing, let it take care of it.
  if (!s_in_crash_handler && !IsDebuggerPresent())
  {
    s_in_crash_handler = true;
    if (s_cleanup_handler)
      s_cleanup_handler();

    WriteMinidumpAndCallstack(nullptr, "Invalid parameter handler invoked");
  }

  __fastfail(FAST_FAIL_INVALID_ARG);
}

static void PureCallHandler()
{
  // if the debugger is attached, or we're recursively crashing, let it take care of it.
  if (!s_in_crash_handler && !IsDebuggerPresent())
  {
    s_in_crash_handler = true;
    if (s_cleanup_handler)
      s_cleanup_handler();

    WriteMinidumpAndCallstack(nullptr, "Pure call handler invoked");
  }

  __fastfail(FAST_FAIL_INVALID_ARG);
}

static void AbortSignalHandler(int signal)
{
  // if the debugger is attached, or we're recursively crashing, let it take care of it.
  if (!s_in_crash_handler && !IsDebuggerPresent())
  {
    s_in_crash_handler = true;
    if (s_cleanup_handler)
      s_cleanup_handler();

    WriteMinidumpAndCallstack(nullptr, "Pure call handler invoked");
  }

  if (IsDebuggerPresent())
    __debugbreak();

  TerminateProcess(GetCurrentProcess(), 0xFAFAFAFAu);
}

bool CrashHandler::Install(CleanupHandler cleanup_handler)
{
  // load dbghelp at install/startup, that way we're not LoadLibrary()'ing after a crash
  // .. because that probably wouldn't go down well.
  HMODULE mod = StackWalker::LoadDbgHelpLibrary();
  if (mod)
    s_dbghelp_module.Adopt(mod);

  s_cleanup_handler = cleanup_handler;

  SetUnhandledExceptionFilter(ExceptionHandler);
  _set_invalid_parameter_handler(InvalidParameterHandler);
  _set_purecall_handler(PureCallHandler);
#ifdef _DEBUG
  _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
  _set_abort_behavior(_WRITE_ABORT_MSG | _CALL_REPORTFAULT, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  signal(SIGABRT, AbortSignalHandler);
  return true;
}

void CrashHandler::SetWriteDirectory(std::string_view dump_directory)
{
  s_write_directory = StringUtil::UTF8StringToWideString(dump_directory);
}

void CrashHandler::WriteDumpForCaller(std::string_view message)
{
  WriteMinidumpAndCallstack(nullptr, message);
}

#elif !defined(__APPLE__) && !defined(__ANDROID__)

#include <backtrace.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace CrashHandler {
namespace {
struct BacktraceBuffer
{
  char* buffer;
  size_t used;
  size_t size;
};
} // namespace

static const char* GetSignalName(int signal_no);
static void AllocateBuffer(BacktraceBuffer* buf);
static void FreeBuffer(BacktraceBuffer* buf);
static void AppendToBuffer(BacktraceBuffer* buf, const char* format, ...);
static int BacktraceFullCallback(void* data, uintptr_t pc, const char* filename, int lineno, const char* function);
static void LogCallstack(int signal, const void* exception_pc);

static std::recursive_mutex s_crash_mutex;
static bool s_in_signal_handler = false;

static CleanupHandler s_cleanup_handler;
static backtrace_state* s_backtrace_state = nullptr;
} // namespace CrashHandler

const char* CrashHandler::GetSignalName(int signal_no)
{
  switch (signal_no)
  {
      // Don't need to list all of them, there's only a couple we register.
      // clang-format off
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGABRT: return "SIGABRT";
    default: return "UNKNOWN";
      // clang-format on
  }
}

void CrashHandler::AllocateBuffer(BacktraceBuffer* buf)
{
  buf->used = 0;
  buf->size = sysconf(_SC_PAGESIZE);
  buf->buffer =
    static_cast<char*>(mmap(nullptr, buf->size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
  if (buf->buffer == static_cast<char*>(MAP_FAILED))
  {
    buf->buffer = nullptr;
    buf->size = 0;
  }
}

void CrashHandler::FreeBuffer(BacktraceBuffer* buf)
{
  if (buf->buffer)
    munmap(buf->buffer, buf->size);
}

void CrashHandler::AppendToBuffer(BacktraceBuffer* buf, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);

  // Hope this doesn't allocate memory... it *can*, but hopefully unlikely since
  // it won't be the first call, and we're providing the buffer.
  if (buf->size > 0 && buf->used < (buf->size - 1))
  {
    const int written = std::vsnprintf(buf->buffer + buf->used, buf->size - buf->used, format, ap);
    if (written > 0)
      buf->used += static_cast<size_t>(written);
  }

  va_end(ap);
}

int CrashHandler::BacktraceFullCallback(void* data, uintptr_t pc, const char* filename, int lineno,
                                        const char* function)
{
  BacktraceBuffer* buf = static_cast<BacktraceBuffer*>(data);
  AppendToBuffer(buf, "  %016p", pc);
  if (function)
    AppendToBuffer(buf, " %s", function);
  if (filename)
    AppendToBuffer(buf, " [%s:%d]", filename, lineno);

  AppendToBuffer(buf, "\n");
  return 0;
}

void CrashHandler::LogCallstack(int signal, const void* exception_pc)
{
  BacktraceBuffer buf;
  AllocateBuffer(&buf);
  if (signal != 0 || exception_pc)
    AppendToBuffer(&buf, "*************** Unhandled %s at %p ***************\n", GetSignalName(signal), exception_pc);
  else
    AppendToBuffer(&buf, "*******************************************************************\n");

  const int rc = backtrace_full(s_backtrace_state, 0, BacktraceFullCallback, nullptr, &buf);
  if (rc != 0)
    AppendToBuffer(&buf, "  backtrace_full() failed: %d\n");

  AppendToBuffer(&buf, "*******************************************************************\n");

  if (buf.used > 0)
    write(STDERR_FILENO, buf.buffer, buf.used);

  FreeBuffer(&buf);
}

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
  std::unique_lock lock(s_crash_mutex);

  // If we crash somewhere in libbacktrace, don't bother trying again.
  if (!s_in_signal_handler)
  {
    s_in_signal_handler = true;

    if (s_cleanup_handler)
      s_cleanup_handler();

#if defined(__APPLE__) && defined(__x86_64__)
    void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip);
#elif defined(__FreeBSD__) && defined(__x86_64__)
    void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip);
#elif defined(__x86_64__)
    void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
#else
    void* const exception_pc = nullptr;
#endif

    LogCallstack(signal, exception_pc);

    s_in_signal_handler = false;
  }

  lock.unlock();

  // We can't continue from here. Just bail out and dump core.
  static const char abort_message[] = "Aborting application.\n";
  write(STDERR_FILENO, abort_message, sizeof(abort_message) - 1);

  // Call default abort signal handler, regardless of whether this was SIGSEGV or SIGABRT.
  lock.lock();
  std::signal(SIGABRT, SIG_DFL);
  raise(SIGABRT);
}

bool CrashHandler::Install(CleanupHandler cleanup_handler)
{
  const std::string progpath = FileSystem::GetProgramPath();
  s_backtrace_state = backtrace_create_state(progpath.empty() ? nullptr : progpath.c_str(), 0, nullptr, nullptr);
  if (!s_backtrace_state)
    return false;

  struct sigaction sa;

  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sa.sa_sigaction = CrashSignalHandler;
  if (sigaction(SIGBUS, &sa, nullptr) != 0)
    return false;
  if (sigaction(SIGSEGV, &sa, nullptr) != 0)
    return false;
  if (sigaction(SIGABRT, &sa, nullptr) != 0)
    return false;

  s_cleanup_handler = cleanup_handler;
  return true;
}

void CrashHandler::SetWriteDirectory(std::string_view dump_directory)
{
}

void CrashHandler::WriteDumpForCaller(std::string_view message)
{
  LogCallstack(0, nullptr);
}

#elif !defined(__ANDROID__)

bool CrashHandler::Install(CleanupHandler cleanup_handler)
{
  return false;
}

void CrashHandler::SetWriteDirectory(std::string_view dump_directory)
{
}

void CrashHandler::WriteDumpForCaller(std::string_view message)
{
}

void CrashHandler::CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
  // We can't continue from here. Just bail out and dump core.
  std::fputs("Aborting application.\n", stderr);
  std::fflush(stderr);
  std::abort();
}

#endif
