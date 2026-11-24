// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "page_fault_handler.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"
#include "common/log.h"

#include <cstring>

#if defined(_WIN32)
#include "common/windows_headers.h"
#elif defined(__linux__) && !defined(__ANDROID__)
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <signal.h>
#include <unistd.h>
#else
#error Unsupported platform.
#endif

#ifdef __APPLE__
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/task.h>
#endif

#ifdef CPU_ARCH_ARM64
[[maybe_unused]] static constexpr u64 ARM64_ESR_ISS_DA_WNR = (1u << 6);

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

#if defined(__linux__) && !defined(__ANDROID__)

static std::optional<u64> GetLinuxAArch64ESR(const ucontext_t* context)
{
  // Why doesn't Linux keep this in the context... ugh.
  struct LinuxAArch64ContextHeader
  {
    u32 magic;
    u32 size;
  };

  struct LinuxAArch64ESRContext
  {
    LinuxAArch64ContextHeader head;
    u64 esr;
  };

  static constexpr u32 LINUX_AARCH64_ESR_MAGIC = 0x45535201;

  const u8* ptr = context->uc_mcontext.__reserved;
  const u8* const end = ptr + sizeof(context->uc_mcontext.__reserved);

  while (ptr < end)
  {
    const std::size_t remaining = static_cast<std::size_t>(end - ptr);
    if (remaining < sizeof(LinuxAArch64ContextHeader))
      break;

    const auto* header = reinterpret_cast<const LinuxAArch64ContextHeader*>(ptr);
    if (header->magic == 0 && header->size == 0)
      break;

    if (header->size < sizeof(LinuxAArch64ContextHeader) || header->size > remaining || (header->size & 15) != 0)
      break;

    if (header->magic == LINUX_AARCH64_ESR_MAGIC && header->size >= sizeof(LinuxAArch64ESRContext))
      return reinterpret_cast<const LinuxAArch64ESRContext*>(ptr)->esr;

    ptr += header->size;
  }

  return std::nullopt;
}

#endif // __linux__

#elifdef CPU_ARCH_RISCV64
[[maybe_unused]] static bool IsStoreInstruction(const void* ptr)
{
  u32 bits;
  std::memcpy(&bits, ptr, sizeof(bits));

  return ((bits & 0x7Fu) == 0b0100011u);
}
#endif

#ifdef _WIN32

namespace PageFaultHandler {
static LONG ExceptionHandler(PEXCEPTION_POINTERS exi);
} // namespace PageFaultHandler

LONG PageFaultHandler::ExceptionHandler(PEXCEPTION_POINTERS exi)
{
  // Only interested in page faults.
  if (exi->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;

#ifdef _M_AMD64
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Rip);
#elifdef _M_ARM64
  void* const exception_pc = reinterpret_cast<void*>(exi->ContextRecord->Pc);
#else
  void* const exception_pc = nullptr;
#endif

  void* const exception_address = reinterpret_cast<void*>(exi->ExceptionRecord->ExceptionInformation[1]);
  const bool is_write = exi->ExceptionRecord->ExceptionInformation[0] == 1;

  const HandlerResult handled = HandlePageFault(exception_pc, exception_address, is_write);
  return (handled == HandlerResult::ContinueExecution) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

bool PageFaultHandler::Install(Error* error)
{
  PVOID handle = AddVectoredExceptionHandler(1, ExceptionHandler);
  if (!handle)
  {
    Error::SetWin32(error, "AddVectoredExceptionHandler() failed: ", GetLastError());
    return false;
  }

  return true;
}

#elif !defined(__ANDROID__)

namespace PageFaultHandler {
static void SignalHandler(int sig, siginfo_t* info, void* ctx);
} // namespace PageFaultHandler

void PageFaultHandler::SignalHandler(int sig, siginfo_t* info, void* ctx)
{
#ifdef __linux__
  void* const exception_address = reinterpret_cast<void*>(info->si_addr);

#ifdef CPU_ARCH_X64
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elifdef CPU_ARCH_ARM32
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.arm_pc);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.error_code & (1 << 11)) != 0; // DFSR.WnR
#elifdef CPU_ARCH_ARM64
  ucontext_t* const context = static_cast<ucontext_t*>(ctx);
  void* const exception_pc = reinterpret_cast<void*>(context->uc_mcontext.pc);
  const std::optional<u64> esr = GetLinuxAArch64ESR(context); // NOTE: Must not read instruction unless ESR check fails.
  const bool is_write =
    esr.has_value() ? ((esr.value() & ARM64_ESR_ISS_DA_WNR) != 0) : IsStoreInstruction(exception_pc);
#elifdef CPU_ARCH_RISCV64
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.__gregs[REG_PC]);
  const bool is_write = IsStoreInstruction(exception_pc);
#else
#error Unsupported architecture.
#endif

#elifdef __APPLE__

#ifdef CPU_ARCH_X64
  void* const exception_address =
    reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__faultvaddr);
  void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip);
  const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__err & 2) != 0;
#elifdef CPU_ARCH_ARM64
  ucontext_t* const context = static_cast<ucontext_t*>(ctx);
  void* const exception_address = reinterpret_cast<void*>(context->uc_mcontext->__es.__far);
  void* const exception_pc = reinterpret_cast<void*>(context->uc_mcontext->__ss.__pc);
  const bool is_write = (context->uc_mcontext->__es.__esr & ARM64_ESR_ISS_DA_WNR) != 0;
#else
#error Unsupported architecture.
#endif

#else

#error Unsupported platform.

#endif

  // Resumes execution right where we left off (re-executes instruction that caused the SIGSEGV).
  const HandlerResult result = HandlePageFault(exception_pc, exception_address, is_write);
  if (result == HandlerResult::ContinueExecution)
    return;

  // We couldn't handle it. Pass it off to the crash dumper.
  CrashHandler::CrashSignalHandler(sig, info, ctx);
}

bool PageFaultHandler::Install(Error* error)
{
  struct sigaction sa;

  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = SignalHandler;

  // Don't block the signal from executing recursively, we want to fire the original handler.
  sa.sa_flags |= SA_NODEFER;

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

  return true;
}

#endif
