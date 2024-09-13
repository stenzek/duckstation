// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "page_fault_handler.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/log.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include "common/windows_headers.h"
#elif defined(__linux__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <signal.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/task.h>
#endif

namespace PageFaultHandler {
static std::recursive_mutex s_exception_handler_mutex;
static bool s_in_exception_handler = false;
static bool s_installed = false;
} // namespace PageFaultHandler

#if defined(CPU_ARCH_ARM64)
[[maybe_unused]] static bool IsStoreInstruction(const void* ptr)
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
#elif defined(CPU_ARCH_RISCV64)
[[maybe_unused]] static bool IsStoreInstruction(const void* ptr)
{
  u32 bits;
  std::memcpy(&bits, ptr, sizeof(bits));

  return ((bits & 0x7Fu) == 0b0100011u);
}
#endif

#if defined(_WIN32)

namespace PageFaultHandler {
static LONG ExceptionHandler(PEXCEPTION_POINTERS exi);
}

LONG PageFaultHandler::ExceptionHandler(PEXCEPTION_POINTERS exi)
{
  // Executing the handler concurrently from multiple threads wouldn't go down well.
  std::unique_lock lock(s_exception_handler_mutex);

  // Prevent recursive exception filtering.
  if (s_in_exception_handler)
    return EXCEPTION_CONTINUE_SEARCH;

  // Only interested in page faults.
  if (exi->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;

#if defined(_M_AMD64)
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Rip);
#elif defined(_M_ARM64)
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Pc);
#else
  void* const exception_pc = nullptr;
#endif

  void* const exception_address = reinterpret_cast<void*>(exi->ExceptionRecord->ExceptionInformation[1]);
  const bool is_write = exi->ExceptionRecord->ExceptionInformation[0] == 1;

  s_in_exception_handler = true;

  const HandlerResult handled = HandlePageFault(exception_pc, exception_address, is_write);

  s_in_exception_handler = false;

  return (handled == HandlerResult::ContinueExecution) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

bool PageFaultHandler::Install(Error* error)
{
  std::unique_lock lock(s_exception_handler_mutex);
  AssertMsg(!s_installed, "Page fault handler has already been installed.");

  PVOID handle = AddVectoredExceptionHandler(1, ExceptionHandler);
  if (!handle)
  {
    Error::SetWin32(error, "AddVectoredExceptionHandler() failed: ", GetLastError());
    return false;
  }

  s_installed = true;
  return true;
}

#else

namespace PageFaultHandler {
static void SignalHandler(int sig, siginfo_t* info, void* ctx);
} // namespace PageFaultHandler

void PageFaultHandler::SignalHandler(int sig, siginfo_t* info, void* ctx)
{
#if defined(__linux__)
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);

#if defined(CPU_ARCH_X64)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(CPU_ARCH_ARM32)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.arm_pc);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.error_code & (1 << 11)) != 0; // DFSR.WnR
#elif defined(CPU_ARCH_ARM64)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#elif defined(CPU_ARCH_RISCV64)
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.__gregs[REG_PC]);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#elif defined(__APPLE__)

#if defined(CPU_ARCH_X64)
  void* const exception_address =
    reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__faultvaddr);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__err & 2) != 0;
#elif defined(CPU_ARCH_ARM64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#elif defined(__FreeBSD__)

#if defined(CPU_ARCH_X64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_addr);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_err & 2) != 0;
#elif defined(CPU_ARCH_ARM64)
  void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);
  void* const exception_pc = nullptr;
  const bool is_write = false;
#endif

#endif

  // Executing the handler concurrently from multiple threads wouldn't go down well.
  s_exception_handler_mutex.lock();

  // Prevent recursive exception filtering.
  HandlerResult result = HandlerResult::ExecuteNextHandler;
  if (!s_in_exception_handler)
  {
    s_in_exception_handler = true;
    result = HandlePageFault(exception_pc, exception_address, is_write);
    s_in_exception_handler = false;
  }
  
  s_exception_handler_mutex.unlock();

  // Resumes execution right where we left off (re-executes instruction that caused the SIGSEGV).
  if (result == HandlerResult::ContinueExecution)
    return;

  // We couldn't handle it. Pass it off to the crash dumper.
  CrashHandler::CrashSignalHandler(sig, info, ctx);
}

bool PageFaultHandler::Install(Error* error)
{
  std::unique_lock lock(s_exception_handler_mutex);
  AssertMsg(!s_installed, "Page fault handler has already been installed.");

  struct sigaction sa;

  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = SignalHandler;
#ifdef __linux__
  // Don't block the signal from executing recursively, we want to fire the original handler.
  sa.sa_flags |= SA_NODEFER;
#endif
  if (sigaction(SIGSEGV, &sa, nullptr) != 0)
  {
    Error::SetErrno(error, "sigaction() for SIGSEGV failed: ", errno);
    return false;
  }
#if defined(__APPLE__) || defined(__aarch64__)
  // MacOS uses SIGBUS for memory permission violations
  if (sigaction(SIGBUS, &sa, nullptr) != 0)
  {
    Error::SetErrno(error, "sigaction() for SIGBUS failed: ", errno);
    return false;
  }
#endif
#ifdef __APPLE__
  task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif

  s_installed = true;
  return true;
}

#endif
