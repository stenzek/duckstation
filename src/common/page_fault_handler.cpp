#include "page_fault_handler.h"
#include "log.h"
#include "platform.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
Log_SetChannel(Common::PageFaultHandler);

#if defined(_WIN32)
#include "windows_headers.h"
#elif defined(__linux__) || defined(__ANDROID__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#define USE_SIGSEGV 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <signal.h>
#include <unistd.h>
#define USE_SIGSEGV 1
#endif

namespace Common::PageFaultHandler {

struct RegisteredHandler
{
  Callback callback;
  const void* owner;
  void* start_pc;
  u32 code_size;
};
static std::vector<RegisteredHandler> m_handlers;
static std::mutex m_handler_lock;
static thread_local bool s_in_handler;

#if defined(CPU_AARCH32)
static bool IsStoreInstruction(const void* ptr)
{
  u32 bits;
  std::memcpy(&bits, ptr, sizeof(bits));

  // TODO
  return false;
}

#elif defined(CPU_AARCH64)
static bool IsStoreInstruction(const void* ptr)
{
  u32 bits;
  std::memcpy(&bits, ptr, sizeof(bits));

  // Based on vixl's disassembler Instruction::IsStore().
  // if (Mask(LoadStoreAnyFMask) != LoadStoreAnyFixed)
  if ((bits & 0x0a000000) != 0x08000000)
    return false;

  // if (Mask(LoadStorePairAnyFMask) == LoadStorePairAnyFixed)
  if ((bits & 0x3a000000) == 0x28000000)
  {
    // return Mask(LoadStorePairLBit) == 0
    return (bits & (1 << 22)) == 0;
  }

  switch (bits & 0xC4C00000)
  {
    case 0x00000000: // STRB_w
    case 0x40000000: // STRH_w
    case 0x80000000: // STR_w
    case 0xC0000000: // STR_x
    case 0x04000000: // STR_b
    case 0x44000000: // STR_h
    case 0x84000000: // STR_s
    case 0xC4000000: // STR_d
    case 0x04800000: // STR_q
      return true;

    default:
      return false;
  }
}
#endif

#if defined(_WIN32) && !defined(_UWP) && (defined(CPU_X64) || defined(CPU_AARCH64))
static PVOID s_veh_handle;

static LONG ExceptionHandler(PEXCEPTION_POINTERS exi)
{
  if (exi->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || s_in_handler)
    return EXCEPTION_CONTINUE_SEARCH;

  s_in_handler = true;

#if defined(_M_AMD64)
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Rip);
#elif defined(_M_ARM64)
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Pc);
#else
  void* const exception_pc = nullptr;
#endif

  void* const exception_address = reinterpret_cast<void*>(exi->ExceptionRecord->ExceptionInformation[1]);
  bool const is_write = exi->ExceptionRecord->ExceptionInformation[0] == 1;

  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (rh.callback(exception_pc, exception_address, is_write) == HandlerResult::ContinueExecution)
    {
      s_in_handler = false;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }

  s_in_handler = false;

  return EXCEPTION_CONTINUE_SEARCH;
}

u32 GetHandlerCodeSize()
{
  return 0;
}

#elif defined(_UWP)

// https://docs.microsoft.com/en-us/cpp/build/exception-handling-x64?view=msvc-160
struct UNWIND_INFO
{
  BYTE version : 3;
  BYTE flags : 5;
  BYTE size_of_prologue;
  BYTE count_of_unwind_codes;
  BYTE frame_register : 4;
  BYTE frame_offset_scaled : 4;
  ULONG exception_handler_address;
};

struct UnwindHandler
{
  RUNTIME_FUNCTION runtime_function;
  UNWIND_INFO unwind_info;
  uint8_t exception_handler_code[32];
};

static constexpr size_t UNWIND_HANDLER_ALLOC_SIZE = 4096;
static_assert(sizeof(UnwindHandler) <= UNWIND_HANDLER_ALLOC_SIZE);

static EXCEPTION_DISPOSITION UnwindExceptionHandler(PEXCEPTION_RECORD ExceptionRecord, ULONG64 EstablisherFrame,
                                                    PCONTEXT ContextRecord, PDISPATCHER_CONTEXT DispatcherContext)
{
  if (s_in_handler)
    return ExceptionContinueSearch;

  s_in_handler = true;

  void* const exception_pc = reinterpret_cast<void*>(DispatcherContext->ControlPc);
  void* const exception_address = reinterpret_cast<void*>(ExceptionRecord->ExceptionInformation[1]);
  bool const is_write = ExceptionRecord->ExceptionInformation[0] == 1;

  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (static_cast<const u8*>(exception_pc) >= static_cast<const u8*>(rh.start_pc) &&
        static_cast<const u8*>(exception_pc) <= (static_cast<const u8*>(rh.start_pc) + rh.code_size))
    {
      if (rh.callback(exception_pc, exception_address, is_write) == HandlerResult::ContinueExecution)
      {
        s_in_handler = false;
        return ExceptionContinueExecution;
      }
    }
  }

  s_in_handler = false;
  return ExceptionContinueSearch;
}

static PRUNTIME_FUNCTION GetRuntimeFunctionCallback(DWORD64 ControlPc, PVOID Context)
{
  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (ControlPc >= reinterpret_cast<DWORD64>(rh.start_pc) &&
        ControlPc <= (reinterpret_cast<DWORD64>(rh.start_pc) + rh.code_size))
    {
      return reinterpret_cast<PRUNTIME_FUNCTION>(rh.start_pc);
    }
  }

  return nullptr;
}

static bool InstallFunctionTableCallback(const void* owner, void* start_pc, u32 code_size)
{
  if (code_size < UNWIND_HANDLER_ALLOC_SIZE)
  {
    Log_ErrorPrintf("Invalid code size: %u @ %p", code_size, UNWIND_HANDLER_ALLOC_SIZE);
    return false;
  }

  if (!RtlInstallFunctionTableCallback(reinterpret_cast<DWORD64>(owner) | 0x3, reinterpret_cast<DWORD64>(start_pc),
                                       static_cast<DWORD>(code_size), &GetRuntimeFunctionCallback, nullptr, nullptr))
  {
    Log_ErrorPrintf("RtlInstallFunctionTableCallback() failed: %08X", GetLastError());
    return false;
  }

  // This is only valid on x86 for now.
#ifndef CPU_X64
  Log_ErrorPrint("Exception unwind codegen not implemented");
  return false;
#else
  UnwindHandler* uh = static_cast<UnwindHandler*>(start_pc);
  ULONG old_protection;
  if (!VirtualProtectFromApp(uh, UNWIND_HANDLER_ALLOC_SIZE, PAGE_READWRITE, &old_protection))
  {
    Log_ErrorPrintf("VirtualProtectFromApp(RW) for exception handler failed: %08X", GetLastError());
    return false;
  }

  uh->runtime_function.BeginAddress = UNWIND_HANDLER_ALLOC_SIZE;
  uh->runtime_function.EndAddress = code_size;
  uh->runtime_function.UnwindInfoAddress = offsetof(UnwindHandler, unwind_info);

  uh->unwind_info.version = 1;
  uh->unwind_info.flags = UNW_FLAG_EHANDLER;
  uh->unwind_info.size_of_prologue = 0;
  uh->unwind_info.count_of_unwind_codes = 0;
  uh->unwind_info.frame_register = 0;
  uh->unwind_info.frame_offset_scaled = 0;
  uh->unwind_info.exception_handler_address = offsetof(UnwindHandler, exception_handler_code);

  // mov rax, handler
  const void* handler = UnwindExceptionHandler;
  uh->exception_handler_code[0] = 0x48;
  uh->exception_handler_code[1] = 0xb8;
  std::memcpy(&uh->exception_handler_code[2], &handler, sizeof(handler));

  // jmp rax
  uh->exception_handler_code[10] = 0xff;
  uh->exception_handler_code[11] = 0xe0;

  if (!VirtualProtectFromApp(uh, UNWIND_HANDLER_ALLOC_SIZE, PAGE_EXECUTE_READ, &old_protection))
  {
    Log_ErrorPrintf("VirtualProtectFromApp(RX) for exception handler failed: %08X", GetLastError());
    return false;
  }

  return true;
#endif
}

u32 GetHandlerCodeSize()
{
  return UNWIND_HANDLER_ALLOC_SIZE;
}

#elif defined(USE_SIGSEGV)

static struct sigaction s_old_sigsegv_action;
#if defined(__APPLE__) || defined(__aarch64__)
static struct sigaction s_old_sigbus_action;
#endif

static void SIGSEGVHandler(int sig, siginfo_t* info, void* ctx)
{
  if ((info->si_code != SEGV_MAPERR && info->si_code != SEGV_ACCERR) || s_in_handler)
    return;

#if defined(__linux__) || defined(__ANDROID__)
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);

#if defined(CPU_X64)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(CPU_AARCH32)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.arm_pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#elif defined(CPU_AARCH64)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#elif defined(__APPLE__)

#if defined(CPU_X64)
  void* const exception_address =
    reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__faultvaddr);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__err & 2) != 0;
#elif defined(CPU_AARCH64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#elif defined(__FreeBSD__)

#if defined(CPU_X64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_addr);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_err & 2) != 0;
#elif defined(CPU_AARCH64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#endif

  std::lock_guard<std::mutex> guard(m_handler_lock);
  for (const RegisteredHandler& rh : m_handlers)
  {
    if (rh.callback(exception_pc, exception_address, is_write) == HandlerResult::ContinueExecution)
    {
      s_in_handler = false;
      return;
    }
  }

  // call old signal handler
#if !defined(__APPLE__) && !defined(__aarch64__)
  const struct sigaction& sa = s_old_sigsegv_action;
#else
  const struct sigaction& sa = (sig == SIGBUS) ? s_old_sigbus_action : s_old_sigsegv_action;
#endif
  if (sa.sa_flags & SA_SIGINFO)
    sa.sa_sigaction(sig, info, ctx);
  else if (sa.sa_handler == SIG_DFL)
    signal(sig, SIG_DFL);
  else if (sa.sa_handler == SIG_IGN)
    return;
  else
    sa.sa_handler(sig);
}

u32 GetHandlerCodeSize()
{
  return 0;
}

#else

u32 GetHandlerCodeSize()
{
  return 0;
}

#endif

bool InstallHandler(const void* owner, void* start_pc, u32 code_size, Callback callback)
{
  bool was_empty;
  {
    std::lock_guard<std::mutex> guard(m_handler_lock);
    if (std::find_if(m_handlers.begin(), m_handlers.end(),
                     [owner](const RegisteredHandler& rh) { return rh.owner == owner; }) != m_handlers.end())
    {
      return false;
    }

    was_empty = m_handlers.empty();
  }

  if (was_empty)
  {
#if defined(_WIN32) && !defined(_UWP) && (defined(CPU_X64) || defined(CPU_AARCH64))
    s_veh_handle = AddVectoredExceptionHandler(1, ExceptionHandler);
    if (!s_veh_handle)
    {
      Log_ErrorPrint("Failed to add vectored exception handler");
      return false;
    }
#elif defined(_UWP)
    if (!InstallFunctionTableCallback(owner, start_pc, code_size))
    {
      Log_ErrorPrint("Failed to install function table callback");
      return false;
    }
#elif defined(USE_SIGSEGV)
    struct sigaction sa = {};
    sa.sa_sigaction = SIGSEGVHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_old_sigsegv_action) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGSEGV) failed: %d", errno);
      return false;
    }
#if defined(__APPLE__) || defined(__aarch64__)
    if (sigaction(SIGBUS, &sa, &s_old_sigbus_action) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGBUS) failed: %d", errno);
      return false;
    }
#endif

#else
    return false;
#endif
  }

  m_handlers.push_back(RegisteredHandler{callback, owner, start_pc, code_size});
  return true;
}

bool RemoveHandler(const void* owner)
{
  std::lock_guard<std::mutex> guard(m_handler_lock);
  auto it = std::find_if(m_handlers.begin(), m_handlers.end(),
                         [owner](const RegisteredHandler& rh) { return rh.owner == owner; });
  if (it == m_handlers.end())
    return false;

  m_handlers.erase(it);

  if (m_handlers.empty())
  {
#if defined(_WIN32) && !defined(_UWP) && (defined(CPU_X64) || defined(CPU_AARCH64))
    RemoveVectoredExceptionHandler(s_veh_handle);
    s_veh_handle = nullptr;
#elif defined(_UWP)
    // nothing to do here, any unregistered regions will be ignored
#elif defined(USE_SIGSEGV)
    // restore old signal handler
#if defined(__APPLE__) || defined(__aarch64__)
    if (sigaction(SIGBUS, &s_old_sigbus_action, nullptr) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGBUS) failed: %d", errno);
      return false;
    }
    s_old_sigbus_action = {};
#endif

    if (sigaction(SIGSEGV, &s_old_sigsegv_action, nullptr) < 0)
    {
      Log_ErrorPrintf("sigaction(SIGSEGV) failed: %d", errno);
      return false;
    }

    s_old_sigsegv_action = {};
#else
    return false;
#endif
  }

  return true;
}

} // namespace Common::PageFaultHandler
