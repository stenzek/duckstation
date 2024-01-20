// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cpu_core.h"
#include "bus.h"
#include "common/align.h"
#include "common/fastjmp.h"
#include "common/file_system.h"
#include "common/log.h"
#include "cpu_code_cache_private.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "cpu_pgxp.h"
#include "cpu_recompiler_thunks.h"
#include "gte.h"
#include "host.h"
#include "pcdrv.h"
#include "settings.h"
#include "system.h"
#include "timing_event.h"
#include "util/state_wrapper.h"
#include <cstdio>

Log_SetChannel(CPU::Core);

namespace CPU {
static void SetPC(u32 new_pc);
static void UpdateLoadDelay();
static void Branch(u32 target);
static void FlushLoadDelay();
static void FlushPipeline();

static u32 GetExceptionVector(bool debug_exception = false);
static void RaiseException(u32 CAUSE_bits, u32 EPC, u32 vector);

static u32 ReadReg(Reg rs);
static void WriteReg(Reg rd, u32 value);
static void WriteRegDelayed(Reg rd, u32 value);

static u32 ReadCop0Reg(Cop0Reg reg);
static void WriteCop0Reg(Cop0Reg reg, u32 value);

static void DispatchCop0Breakpoint();
static bool IsCop0ExecutionBreakpointUnmasked();
static void Cop0ExecutionBreakpointCheck();
template<MemoryAccessType type>
static void Cop0DataBreakpointCheck(VirtualMemoryAddress address);
static bool BreakpointCheck();

#ifdef _DEBUG
static void TracePrintInstruction();
#endif

static void DisassembleAndPrint(u32 addr, bool regs, const char* prefix);
static void PrintInstruction(u32 bits, u32 pc, bool regs, const char* prefix);
static void LogInstruction(u32 bits, u32 pc, bool regs);

static void HandleWriteSyscall();
static void HandlePutcSyscall();
static void HandlePutsSyscall();
static void ExecuteDebug();

template<PGXPMode pgxp_mode, bool debug>
static void ExecuteInstruction();

template<PGXPMode pgxp_mode, bool debug>
[[noreturn]] static void ExecuteImpl();

static bool FetchInstruction();
static bool FetchInstructionForInterpreterFallback();
template<bool add_ticks, bool icache_read = false, u32 word_count = 1, bool raise_exceptions>
static bool DoInstructionRead(PhysicalMemoryAddress address, void* data);
template<MemoryAccessType type, MemoryAccessSize size>
static bool DoSafeMemoryAccess(VirtualMemoryAddress address, u32& value);
template<MemoryAccessType type, MemoryAccessSize size>
static bool DoAlignmentCheck(VirtualMemoryAddress address);
static bool ReadMemoryByte(VirtualMemoryAddress addr, u8* value);
static bool ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value);
static bool ReadMemoryWord(VirtualMemoryAddress addr, u32* value);
static bool WriteMemoryByte(VirtualMemoryAddress addr, u32 value);
static bool WriteMemoryHalfWord(VirtualMemoryAddress addr, u32 value);
static bool WriteMemoryWord(VirtualMemoryAddress addr, u32 value);

State g_state;
bool TRACE_EXECUTION = false;

static fastjmp_buf s_jmp_buf;

static std::FILE* s_log_file = nullptr;
static bool s_log_file_opened = false;
static bool s_trace_to_log = false;

static constexpr u32 INVALID_BREAKPOINT_PC = UINT32_C(0xFFFFFFFF);
static std::vector<Breakpoint> s_breakpoints;
static u32 s_breakpoint_counter = 1;
static u32 s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;
static bool s_single_step = false;
static bool s_single_step_done = false;
} // namespace CPU

bool CPU::IsTraceEnabled()
{
  return s_trace_to_log;
}

void CPU::StartTrace()
{
  if (s_trace_to_log)
    return;

  s_trace_to_log = true;
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();
}

void CPU::StopTrace()
{
  if (!s_trace_to_log)
    return;

  if (s_log_file)
    std::fclose(s_log_file);

  s_log_file_opened = false;
  s_trace_to_log = false;
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();
}

void CPU::WriteToExecutionLog(const char* format, ...)
{
  if (!s_log_file_opened)
  {
    s_log_file = FileSystem::OpenCFile("cpu_log.txt", "wb");
    s_log_file_opened = true;
  }

  if (s_log_file)
  {
    std::va_list ap;
    va_start(ap, format);
    std::vfprintf(s_log_file, format, ap);
    va_end(ap);

#ifdef _DEBUG
    std::fflush(s_log_file);
#endif
  }
}

void CPU::Initialize()
{
  // From nocash spec.
  g_state.cop0_regs.PRID = UINT32_C(0x00000002);

  g_state.use_debug_dispatcher = false;
  s_breakpoints.clear();
  s_breakpoint_counter = 1;
  s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;
  s_single_step = false;

  UpdateMemoryPointers();

  GTE::Initialize();
}

void CPU::Shutdown()
{
  ClearBreakpoints();
  StopTrace();
}

void CPU::Reset()
{
  g_state.pending_ticks = 0;
  g_state.downcount = 0;
  g_state.exception_raised = false;
  g_state.bus_error = false;

  g_state.regs = {};

  g_state.cop0_regs.BPC = 0;
  g_state.cop0_regs.BDA = 0;
  g_state.cop0_regs.TAR = 0;
  g_state.cop0_regs.BadVaddr = 0;
  g_state.cop0_regs.BDAM = 0;
  g_state.cop0_regs.BPCM = 0;
  g_state.cop0_regs.EPC = 0;
  g_state.cop0_regs.sr.bits = 0;
  g_state.cop0_regs.cause.bits = 0;

  ClearICache();
  UpdateMemoryPointers();

  GTE::Reset();

  if (g_settings.gpu_pgxp_enable)
    PGXP::Reset();

  // TODO: This consumes cycles...
  SetPC(RESET_VECTOR);
}

bool CPU::DoState(StateWrapper& sw)
{
  sw.Do(&g_state.pending_ticks);
  sw.Do(&g_state.downcount);
  sw.DoArray(g_state.regs.r, static_cast<u32>(Reg::count));
  sw.Do(&g_state.pc);
  sw.Do(&g_state.npc);
  sw.Do(&g_state.cop0_regs.BPC);
  sw.Do(&g_state.cop0_regs.BDA);
  sw.Do(&g_state.cop0_regs.TAR);
  sw.Do(&g_state.cop0_regs.BadVaddr);
  sw.Do(&g_state.cop0_regs.BDAM);
  sw.Do(&g_state.cop0_regs.BPCM);
  sw.Do(&g_state.cop0_regs.EPC);
  sw.Do(&g_state.cop0_regs.PRID);
  sw.Do(&g_state.cop0_regs.sr.bits);
  sw.Do(&g_state.cop0_regs.cause.bits);
  sw.Do(&g_state.cop0_regs.dcic.bits);
  sw.Do(&g_state.next_instruction.bits);
  sw.Do(&g_state.current_instruction.bits);
  sw.Do(&g_state.current_instruction_pc);
  sw.Do(&g_state.current_instruction_in_branch_delay_slot);
  sw.Do(&g_state.current_instruction_was_branch_taken);
  sw.Do(&g_state.next_instruction_is_branch_delay_slot);
  sw.Do(&g_state.branch_was_taken);
  sw.Do(&g_state.exception_raised);
  sw.DoEx(&g_state.bus_error, 61, false);
  if (sw.GetVersion() < 59)
  {
    bool interrupt_delay;
    sw.Do(&interrupt_delay);
  }
  sw.Do(&g_state.load_delay_reg);
  sw.Do(&g_state.load_delay_value);
  sw.Do(&g_state.next_load_delay_reg);
  sw.Do(&g_state.next_load_delay_value);

  // Compatibility with old states.
  if (sw.GetVersion() < 59)
  {
    g_state.load_delay_reg =
      static_cast<Reg>(std::min(static_cast<u8>(g_state.load_delay_reg), static_cast<u8>(Reg::count)));
    g_state.next_load_delay_reg =
      static_cast<Reg>(std::min(static_cast<u8>(g_state.load_delay_reg), static_cast<u8>(Reg::count)));
  }

  sw.Do(&g_state.cache_control.bits);
  sw.DoBytes(g_state.scratchpad.data(), g_state.scratchpad.size());

  if (!GTE::DoState(sw))
    return false;

  if (sw.GetVersion() < 48)
  {
    DebugAssert(sw.IsReading());
    ClearICache();
  }
  else
  {
    sw.Do(&g_state.icache_tags);
    sw.Do(&g_state.icache_data);
  }

  if (sw.IsReading())
  {
    UpdateMemoryPointers();
    g_state.gte_completion_tick = 0;
  }

  return !sw.HasError();
}

ALWAYS_INLINE_RELEASE void CPU::SetPC(u32 new_pc)
{
  DebugAssert(Common::IsAlignedPow2(new_pc, 4));
  g_state.npc = new_pc;
  FlushPipeline();
}

ALWAYS_INLINE_RELEASE void CPU::Branch(u32 target)
{
  if (!Common::IsAlignedPow2(target, 4))
  {
    // The BadVaddr and EPC must be set to the fetching address, not the instruction about to execute.
    g_state.cop0_regs.BadVaddr = target;
    RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::AdEL, false, false, 0), target);
    return;
  }

  g_state.npc = target;
  g_state.branch_was_taken = true;
}

ALWAYS_INLINE_RELEASE u32 CPU::GetExceptionVector(bool debug_exception /* = false*/)
{
  const u32 base = g_state.cop0_regs.sr.BEV ? UINT32_C(0xbfc00100) : UINT32_C(0x80000000);
  return base | (debug_exception ? UINT32_C(0x00000040) : UINT32_C(0x00000080));
}

ALWAYS_INLINE_RELEASE void CPU::RaiseException(u32 CAUSE_bits, u32 EPC, u32 vector)
{
  g_state.cop0_regs.EPC = EPC;
  g_state.cop0_regs.cause.bits = (g_state.cop0_regs.cause.bits & ~Cop0Registers::CAUSE::EXCEPTION_WRITE_MASK) |
                                 (CAUSE_bits & Cop0Registers::CAUSE::EXCEPTION_WRITE_MASK);

#ifdef _DEBUG
  if (g_state.cop0_regs.cause.Excode != Exception::INT && g_state.cop0_regs.cause.Excode != Exception::Syscall &&
      g_state.cop0_regs.cause.Excode != Exception::BP)
  {
    Log_DevPrintf("Exception %u at 0x%08X (epc=0x%08X, BD=%s, CE=%u)",
                  static_cast<u8>(g_state.cop0_regs.cause.Excode.GetValue()), g_state.current_instruction_pc,
                  g_state.cop0_regs.EPC, g_state.cop0_regs.cause.BD ? "true" : "false",
                  g_state.cop0_regs.cause.CE.GetValue());
    DisassembleAndPrint(g_state.current_instruction_pc, 4u, 0u);
    if (s_trace_to_log)
    {
      CPU::WriteToExecutionLog("Exception %u at 0x%08X (epc=0x%08X, BD=%s, CE=%u)\n",
                               static_cast<u8>(g_state.cop0_regs.cause.Excode.GetValue()),
                               g_state.current_instruction_pc, g_state.cop0_regs.EPC,
                               g_state.cop0_regs.cause.BD ? "true" : "false", g_state.cop0_regs.cause.CE.GetValue());
    }
  }
#endif

  if (g_state.cop0_regs.cause.BD)
  {
    // TAR is set to the address which was being fetched in this instruction, or the next instruction to execute if the
    // exception hadn't occurred in the delay slot.
    g_state.cop0_regs.EPC -= UINT32_C(4);
    g_state.cop0_regs.TAR = g_state.pc;
  }

  // current -> previous, switch to kernel mode and disable interrupts
  g_state.cop0_regs.sr.mode_bits <<= 2;

  // flush the pipeline - we don't want to execute the previously fetched instruction
  g_state.npc = vector;
  g_state.exception_raised = true;
  FlushPipeline();
}

ALWAYS_INLINE_RELEASE void CPU::DispatchCop0Breakpoint()
{
  // When a breakpoint address match occurs the PSX jumps to 80000040h (ie. unlike normal exceptions, not to 80000080h).
  // The Excode value in the CAUSE register is set to 09h (same as BREAK opcode), and EPC contains the return address,
  // as usually. One of the first things to be done in the exception handler is to disable breakpoints (eg. if the
  // any-jump break is enabled, then it must be disabled BEFORE jumping from 80000040h to the actual exception handler).
  RaiseException(Cop0Registers::CAUSE::MakeValueForException(
                   Exception::BP, g_state.current_instruction_in_branch_delay_slot,
                   g_state.current_instruction_was_branch_taken, g_state.current_instruction.cop.cop_n),
                 g_state.current_instruction_pc, GetExceptionVector(true));
}

void CPU::RaiseException(u32 CAUSE_bits, u32 EPC)
{
  RaiseException(CAUSE_bits, EPC, GetExceptionVector());
}

void CPU::RaiseException(Exception excode)
{
  RaiseException(Cop0Registers::CAUSE::MakeValueForException(excode, g_state.current_instruction_in_branch_delay_slot,
                                                             g_state.current_instruction_was_branch_taken,
                                                             g_state.current_instruction.cop.cop_n),
                 g_state.current_instruction_pc, GetExceptionVector());
}

void CPU::RaiseBreakException(u32 CAUSE_bits, u32 EPC, u32 instruction_bits)
{
  if (g_settings.pcdrv_enable)
  {
    // Load delays need to be flushed, because the break HLE might read a register which
    // is currently being loaded, and on real hardware there isn't a hazard here.
    FlushLoadDelay();

    if (PCDrv::HandleSyscall(instruction_bits, g_state.regs))
    {
      // immediately return
      g_state.npc = EPC + 4;
      FlushPipeline();
      return;
    }
  }

  // normal exception
  RaiseException(CAUSE_bits, EPC, GetExceptionVector());
}

void CPU::SetExternalInterrupt(u8 bit)
{
  g_state.cop0_regs.cause.Ip |= static_cast<u8>(1u << bit);
  CheckForPendingInterrupt();
}

void CPU::ClearExternalInterrupt(u8 bit)
{
  g_state.cop0_regs.cause.Ip &= static_cast<u8>(~(1u << bit));
}

ALWAYS_INLINE_RELEASE void CPU::UpdateLoadDelay()
{
  // the old value is needed in case the delay slot instruction overwrites the same register
  g_state.regs.r[static_cast<u8>(g_state.load_delay_reg)] = g_state.load_delay_value;
  g_state.load_delay_reg = g_state.next_load_delay_reg;
  g_state.load_delay_value = g_state.next_load_delay_value;
  g_state.next_load_delay_reg = Reg::count;
}

ALWAYS_INLINE_RELEASE void CPU::FlushLoadDelay()
{
  g_state.next_load_delay_reg = Reg::count;
  g_state.regs.r[static_cast<u8>(g_state.load_delay_reg)] = g_state.load_delay_value;
  g_state.load_delay_reg = Reg::count;
}

ALWAYS_INLINE_RELEASE void CPU::FlushPipeline()
{
  // loads are flushed
  FlushLoadDelay();

  // not in a branch delay slot
  g_state.branch_was_taken = false;
  g_state.next_instruction_is_branch_delay_slot = false;
  g_state.current_instruction_pc = g_state.pc;

  // prefetch the next instruction
  FetchInstruction();

  // and set it as the next one to execute
  g_state.current_instruction.bits = g_state.next_instruction.bits;
  g_state.current_instruction_in_branch_delay_slot = false;
  g_state.current_instruction_was_branch_taken = false;
}

ALWAYS_INLINE u32 CPU::ReadReg(Reg rs)
{
  return g_state.regs.r[static_cast<u8>(rs)];
}

ALWAYS_INLINE void CPU::WriteReg(Reg rd, u32 value)
{
  g_state.regs.r[static_cast<u8>(rd)] = value;
  g_state.load_delay_reg = (rd == g_state.load_delay_reg) ? Reg::count : g_state.load_delay_reg;

  // prevent writes to $zero from going through - better than branching/cmov
  g_state.regs.zero = 0;
}

ALWAYS_INLINE_RELEASE void CPU::WriteRegDelayed(Reg rd, u32 value)
{
  DebugAssert(g_state.next_load_delay_reg == Reg::count);
  if (rd == Reg::zero)
    return;

  // double load delays ignore the first value
  if (g_state.load_delay_reg == rd)
    g_state.load_delay_reg = Reg::count;

  // save the old value, if something else overwrites this reg we want to preserve it
  g_state.next_load_delay_reg = rd;
  g_state.next_load_delay_value = value;
}

ALWAYS_INLINE_RELEASE u32 CPU::ReadCop0Reg(Cop0Reg reg)
{
  switch (reg)
  {
    case Cop0Reg::BPC:
      return g_state.cop0_regs.BPC;

    case Cop0Reg::BPCM:
      return g_state.cop0_regs.BPCM;

    case Cop0Reg::BDA:
      return g_state.cop0_regs.BDA;

    case Cop0Reg::BDAM:
      return g_state.cop0_regs.BDAM;

    case Cop0Reg::DCIC:
      return g_state.cop0_regs.dcic.bits;

    case Cop0Reg::JUMPDEST:
      return g_state.cop0_regs.TAR;

    case Cop0Reg::BadVaddr:
      return g_state.cop0_regs.BadVaddr;

    case Cop0Reg::SR:
      return g_state.cop0_regs.sr.bits;

    case Cop0Reg::CAUSE:
      return g_state.cop0_regs.cause.bits;

    case Cop0Reg::EPC:
      return g_state.cop0_regs.EPC;

    case Cop0Reg::PRID:
      return g_state.cop0_regs.PRID;

    default:
      return 0;
  }
}

ALWAYS_INLINE_RELEASE void CPU::WriteCop0Reg(Cop0Reg reg, u32 value)
{
  switch (reg)
  {
    case Cop0Reg::BPC:
    {
      g_state.cop0_regs.BPC = value;
      Log_DevPrintf("COP0 BPC <- %08X", value);
    }
    break;

    case Cop0Reg::BPCM:
    {
      g_state.cop0_regs.BPCM = value;
      Log_DevPrintf("COP0 BPCM <- %08X", value);
      if (UpdateDebugDispatcherFlag())
        ExitExecution();
    }
    break;

    case Cop0Reg::BDA:
    {
      g_state.cop0_regs.BDA = value;
      Log_DevPrintf("COP0 BDA <- %08X", value);
    }
    break;

    case Cop0Reg::BDAM:
    {
      g_state.cop0_regs.BDAM = value;
      Log_DevPrintf("COP0 BDAM <- %08X", value);
    }
    break;

    case Cop0Reg::JUMPDEST:
    {
      Log_WarningPrintf("Ignoring write to Cop0 JUMPDEST");
    }
    break;

    case Cop0Reg::DCIC:
    {
      g_state.cop0_regs.dcic.bits =
        (g_state.cop0_regs.dcic.bits & ~Cop0Registers::DCIC::WRITE_MASK) | (value & Cop0Registers::DCIC::WRITE_MASK);
      Log_DevPrintf("COP0 DCIC <- %08X (now %08X)", value, g_state.cop0_regs.dcic.bits);
      if (UpdateDebugDispatcherFlag())
        ExitExecution();
    }
    break;

    case Cop0Reg::SR:
    {
      g_state.cop0_regs.sr.bits =
        (g_state.cop0_regs.sr.bits & ~Cop0Registers::SR::WRITE_MASK) | (value & Cop0Registers::SR::WRITE_MASK);
      Log_DebugPrintf("COP0 SR <- %08X (now %08X)", value, g_state.cop0_regs.sr.bits);
      UpdateMemoryPointers();
      CheckForPendingInterrupt();
    }
    break;

    case Cop0Reg::CAUSE:
    {
      g_state.cop0_regs.cause.bits =
        (g_state.cop0_regs.cause.bits & ~Cop0Registers::CAUSE::WRITE_MASK) | (value & Cop0Registers::CAUSE::WRITE_MASK);
      Log_DebugPrintf("COP0 CAUSE <- %08X (now %08X)", value, g_state.cop0_regs.cause.bits);
      CheckForPendingInterrupt();
    }
    break;

    default:
      Log_DevPrintf("Unknown COP0 reg write %u (%08X)", ZeroExtend32(static_cast<u8>(reg)), value);
      break;
  }
}

ALWAYS_INLINE_RELEASE bool CPU::IsCop0ExecutionBreakpointUnmasked()
{
  static constexpr const u32 code_address_ranges[][2] = {
    // KUSEG
    {Bus::RAM_BASE, Bus::RAM_BASE | Bus::RAM_8MB_MASK},
    {Bus::BIOS_BASE, Bus::BIOS_BASE | Bus::BIOS_MASK},

    // KSEG0
    {0x80000000u | Bus::RAM_BASE, 0x80000000u | Bus::RAM_BASE | Bus::RAM_8MB_MASK},
    {0x80000000u | Bus::BIOS_BASE, 0x80000000u | Bus::BIOS_BASE | Bus::BIOS_MASK},

    // KSEG1
    {0xA0000000u | Bus::RAM_BASE, 0xA0000000u | Bus::RAM_BASE | Bus::RAM_8MB_MASK},
    {0xA0000000u | Bus::BIOS_BASE, 0xA0000000u | Bus::BIOS_BASE | Bus::BIOS_MASK},
  };

  const u32 bpc = g_state.cop0_regs.BPC;
  const u32 bpcm = g_state.cop0_regs.BPCM;
  const u32 masked_bpc = bpc & bpcm;
  for (const auto [range_start, range_end] : code_address_ranges)
  {
    if (masked_bpc >= (range_start & bpcm) && masked_bpc <= (range_end & bpcm))
      return true;
  }

  return false;
}

ALWAYS_INLINE_RELEASE void CPU::Cop0ExecutionBreakpointCheck()
{
  if (!g_state.cop0_regs.dcic.ExecutionBreakpointsEnabled())
    return;

  const u32 pc = g_state.current_instruction_pc;
  const u32 bpc = g_state.cop0_regs.BPC;
  const u32 bpcm = g_state.cop0_regs.BPCM;

  // Break condition is "((PC XOR BPC) AND BPCM)=0".
  if (bpcm == 0 || ((pc ^ bpc) & bpcm) != 0u)
    return;

  Log_DevPrintf("Cop0 execution breakpoint at %08X", pc);
  g_state.cop0_regs.dcic.status_any_break = true;
  g_state.cop0_regs.dcic.status_bpc_code_break = true;
  DispatchCop0Breakpoint();
}

template<MemoryAccessType type>
ALWAYS_INLINE_RELEASE void CPU::Cop0DataBreakpointCheck(VirtualMemoryAddress address)
{
  if constexpr (type == MemoryAccessType::Read)
  {
    if (!g_state.cop0_regs.dcic.DataReadBreakpointsEnabled())
      return;
  }
  else
  {
    if (!g_state.cop0_regs.dcic.DataWriteBreakpointsEnabled())
      return;
  }

  // Break condition is "((addr XOR BDA) AND BDAM)=0".
  const u32 bda = g_state.cop0_regs.BDA;
  const u32 bdam = g_state.cop0_regs.BDAM;
  if (bdam == 0 || ((address ^ bda) & bdam) != 0u)
    return;

  Log_DevPrintf("Cop0 data breakpoint for %08X at %08X", address, g_state.current_instruction_pc);

  g_state.cop0_regs.dcic.status_any_break = true;
  g_state.cop0_regs.dcic.status_bda_data_break = true;
  if constexpr (type == MemoryAccessType::Read)
    g_state.cop0_regs.dcic.status_bda_data_read_break = true;
  else
    g_state.cop0_regs.dcic.status_bda_data_write_break = true;

  DispatchCop0Breakpoint();
}

#ifdef _DEBUG

void CPU::TracePrintInstruction()
{
  const u32 pc = g_state.current_instruction_pc;
  const u32 bits = g_state.current_instruction.bits;

  TinyString instr;
  TinyString comment;
  DisassembleInstruction(&instr, pc, bits);
  DisassembleInstructionComment(&comment, pc, bits);
  if (!comment.empty())
  {
    for (u32 i = instr.length(); i < 30; i++)
      instr.append(' ');
    instr.append("; ");
    instr.append(comment);
  }

  std::printf("%08x: %08x %s\n", pc, bits, instr.c_str());
}

#endif

void CPU::PrintInstruction(u32 bits, u32 pc, bool regs, const char* prefix)
{
  TinyString instr;
  DisassembleInstruction(&instr, pc, bits);
  if (regs)
  {
    TinyString comment;
    DisassembleInstructionComment(&comment, pc, bits);
    if (!comment.empty())
    {
      for (u32 i = instr.length(); i < 30; i++)
        instr.append(' ');
      instr.append("; ");
      instr.append(comment);
    }
  }

  Log_DevPrintf("%s%08x: %08x %s", prefix, pc, bits, instr.c_str());
}

void CPU::LogInstruction(u32 bits, u32 pc, bool regs)
{
  TinyString instr;
  DisassembleInstruction(&instr, pc, bits);
  if (regs)
  {
    TinyString comment;
    DisassembleInstructionComment(&comment, pc, bits);
    if (!comment.empty())
    {
      for (u32 i = instr.length(); i < 30; i++)
        instr.append(' ');
      instr.append("; ");
      instr.append(comment);
    }
  }

  WriteToExecutionLog("%08x: %08x %s\n", pc, bits, instr.c_str());
}

void CPU::HandleWriteSyscall()
{
  const auto& regs = g_state.regs;
  if (regs.a0 != 1) // stdout
    return;

  u32 addr = regs.a1;
  const u32 count = regs.a2;
  for (u32 i = 0; i < count; i++)
  {
    u8 value;
    if (!SafeReadMemoryByte(addr++, &value) || value == 0)
      break;

    Bus::AddTTYCharacter(static_cast<char>(value));
  }
}

void CPU::HandlePutcSyscall()
{
  const auto& regs = g_state.regs;
  if (regs.a0 != 0)
    Bus::AddTTYCharacter(static_cast<char>(regs.a0));
}

void CPU::HandlePutsSyscall()
{
  const auto& regs = g_state.regs;

  u32 addr = regs.a1;
  for (u32 i = 0; i < 1024; i++)
  {
    u8 value;
    if (!SafeReadMemoryByte(addr++, &value) || value == 0)
      break;

    Bus::AddTTYCharacter(static_cast<char>(value));
  }
}

void CPU::HandleA0Syscall()
{
  const auto& regs = g_state.regs;
  const u32 call = regs.t1;
  if (call == 0x03)
    HandleWriteSyscall();
  else if (call == 0x09 || call == 0x3c)
    HandlePutcSyscall();
  else if (call == 0x3e)
    HandlePutsSyscall();
}

void CPU::HandleB0Syscall()
{
  const auto& regs = g_state.regs;
  const u32 call = regs.t1;
  if (call == 0x35)
    HandleWriteSyscall();
  else if (call == 0x3b || call == 0x3d)
    HandlePutcSyscall();
  else if (call == 0x3f)
    HandlePutsSyscall();
}

const std::array<CPU::DebuggerRegisterListEntry, CPU::NUM_DEBUGGER_REGISTER_LIST_ENTRIES>
  CPU::g_debugger_register_list = {{{"zero", &CPU::g_state.regs.zero},
                                    {"at", &CPU::g_state.regs.at},
                                    {"v0", &CPU::g_state.regs.v0},
                                    {"v1", &CPU::g_state.regs.v1},
                                    {"a0", &CPU::g_state.regs.a0},
                                    {"a1", &CPU::g_state.regs.a1},
                                    {"a2", &CPU::g_state.regs.a2},
                                    {"a3", &CPU::g_state.regs.a3},
                                    {"t0", &CPU::g_state.regs.t0},
                                    {"t1", &CPU::g_state.regs.t1},
                                    {"t2", &CPU::g_state.regs.t2},
                                    {"t3", &CPU::g_state.regs.t3},
                                    {"t4", &CPU::g_state.regs.t4},
                                    {"t5", &CPU::g_state.regs.t5},
                                    {"t6", &CPU::g_state.regs.t6},
                                    {"t7", &CPU::g_state.regs.t7},
                                    {"s0", &CPU::g_state.regs.s0},
                                    {"s1", &CPU::g_state.regs.s1},
                                    {"s2", &CPU::g_state.regs.s2},
                                    {"s3", &CPU::g_state.regs.s3},
                                    {"s4", &CPU::g_state.regs.s4},
                                    {"s5", &CPU::g_state.regs.s5},
                                    {"s6", &CPU::g_state.regs.s6},
                                    {"s7", &CPU::g_state.regs.s7},
                                    {"t8", &CPU::g_state.regs.t8},
                                    {"t9", &CPU::g_state.regs.t9},
                                    {"k0", &CPU::g_state.regs.k0},
                                    {"k1", &CPU::g_state.regs.k1},
                                    {"gp", &CPU::g_state.regs.gp},
                                    {"sp", &CPU::g_state.regs.sp},
                                    {"fp", &CPU::g_state.regs.fp},
                                    {"ra", &CPU::g_state.regs.ra},
                                    {"hi", &CPU::g_state.regs.hi},
                                    {"lo", &CPU::g_state.regs.lo},
                                    {"pc", &CPU::g_state.pc},
                                    {"npc", &CPU::g_state.npc},

                                    {"COP0_SR", &CPU::g_state.cop0_regs.sr.bits},
                                    {"COP0_CAUSE", &CPU::g_state.cop0_regs.cause.bits},
                                    {"COP0_EPC", &CPU::g_state.cop0_regs.EPC},
                                    {"COP0_BadVAddr", &CPU::g_state.cop0_regs.BadVaddr},

                                    {"V0_XY", &CPU::g_state.gte_regs.r32[0]},
                                    {"V0_Z", &CPU::g_state.gte_regs.r32[1]},
                                    {"V1_XY", &CPU::g_state.gte_regs.r32[2]},
                                    {"V1_Z", &CPU::g_state.gte_regs.r32[3]},
                                    {"V2_XY", &CPU::g_state.gte_regs.r32[4]},
                                    {"V2_Z", &CPU::g_state.gte_regs.r32[5]},
                                    {"RGBC", &CPU::g_state.gte_regs.r32[6]},
                                    {"OTZ", &CPU::g_state.gte_regs.r32[7]},
                                    {"IR0", &CPU::g_state.gte_regs.r32[8]},
                                    {"IR1", &CPU::g_state.gte_regs.r32[9]},
                                    {"IR2", &CPU::g_state.gte_regs.r32[10]},
                                    {"IR3", &CPU::g_state.gte_regs.r32[11]},
                                    {"SXY0", &CPU::g_state.gte_regs.r32[12]},
                                    {"SXY1", &CPU::g_state.gte_regs.r32[13]},
                                    {"SXY2", &CPU::g_state.gte_regs.r32[14]},
                                    {"SXYP", &CPU::g_state.gte_regs.r32[15]},
                                    {"SZ0", &CPU::g_state.gte_regs.r32[16]},
                                    {"SZ1", &CPU::g_state.gte_regs.r32[17]},
                                    {"SZ2", &CPU::g_state.gte_regs.r32[18]},
                                    {"SZ3", &CPU::g_state.gte_regs.r32[19]},
                                    {"RGB0", &CPU::g_state.gte_regs.r32[20]},
                                    {"RGB1", &CPU::g_state.gte_regs.r32[21]},
                                    {"RGB2", &CPU::g_state.gte_regs.r32[22]},
                                    {"RES1", &CPU::g_state.gte_regs.r32[23]},
                                    {"MAC0", &CPU::g_state.gte_regs.r32[24]},
                                    {"MAC1", &CPU::g_state.gte_regs.r32[25]},
                                    {"MAC2", &CPU::g_state.gte_regs.r32[26]},
                                    {"MAC3", &CPU::g_state.gte_regs.r32[27]},
                                    {"IRGB", &CPU::g_state.gte_regs.r32[28]},
                                    {"ORGB", &CPU::g_state.gte_regs.r32[29]},
                                    {"LZCS", &CPU::g_state.gte_regs.r32[30]},
                                    {"LZCR", &CPU::g_state.gte_regs.r32[31]},
                                    {"RT_0", &CPU::g_state.gte_regs.r32[32]},
                                    {"RT_1", &CPU::g_state.gte_regs.r32[33]},
                                    {"RT_2", &CPU::g_state.gte_regs.r32[34]},
                                    {"RT_3", &CPU::g_state.gte_regs.r32[35]},
                                    {"RT_4", &CPU::g_state.gte_regs.r32[36]},
                                    {"TRX", &CPU::g_state.gte_regs.r32[37]},
                                    {"TRY", &CPU::g_state.gte_regs.r32[38]},
                                    {"TRZ", &CPU::g_state.gte_regs.r32[39]},
                                    {"LLM_0", &CPU::g_state.gte_regs.r32[40]},
                                    {"LLM_1", &CPU::g_state.gte_regs.r32[41]},
                                    {"LLM_2", &CPU::g_state.gte_regs.r32[42]},
                                    {"LLM_3", &CPU::g_state.gte_regs.r32[43]},
                                    {"LLM_4", &CPU::g_state.gte_regs.r32[44]},
                                    {"RBK", &CPU::g_state.gte_regs.r32[45]},
                                    {"GBK", &CPU::g_state.gte_regs.r32[46]},
                                    {"BBK", &CPU::g_state.gte_regs.r32[47]},
                                    {"LCM_0", &CPU::g_state.gte_regs.r32[48]},
                                    {"LCM_1", &CPU::g_state.gte_regs.r32[49]},
                                    {"LCM_2", &CPU::g_state.gte_regs.r32[50]},
                                    {"LCM_3", &CPU::g_state.gte_regs.r32[51]},
                                    {"LCM_4", &CPU::g_state.gte_regs.r32[52]},
                                    {"RFC", &CPU::g_state.gte_regs.r32[53]},
                                    {"GFC", &CPU::g_state.gte_regs.r32[54]},
                                    {"BFC", &CPU::g_state.gte_regs.r32[55]},
                                    {"OFX", &CPU::g_state.gte_regs.r32[56]},
                                    {"OFY", &CPU::g_state.gte_regs.r32[57]},
                                    {"H", &CPU::g_state.gte_regs.r32[58]},
                                    {"DQA", &CPU::g_state.gte_regs.r32[59]},
                                    {"DQB", &CPU::g_state.gte_regs.r32[60]},
                                    {"ZSF3", &CPU::g_state.gte_regs.r32[61]},
                                    {"ZSF4", &CPU::g_state.gte_regs.r32[62]},
                                    {"FLAG", &CPU::g_state.gte_regs.r32[63]}}};

ALWAYS_INLINE static constexpr bool AddOverflow(u32 old_value, u32 add_value, u32 new_value)
{
  return (((new_value ^ old_value) & (new_value ^ add_value)) & UINT32_C(0x80000000)) != 0;
}

ALWAYS_INLINE static constexpr bool SubOverflow(u32 old_value, u32 sub_value, u32 new_value)
{
  return (((new_value ^ old_value) & (old_value ^ sub_value)) & UINT32_C(0x80000000)) != 0;
}

void CPU::DisassembleAndPrint(u32 addr, bool regs, const char* prefix)
{
  u32 bits = 0;
  SafeReadMemoryWord(addr, &bits);
  PrintInstruction(bits, addr, regs, prefix);
}

void CPU::DisassembleAndPrint(u32 addr, u32 instructions_before /* = 0 */, u32 instructions_after /* = 0 */)
{
  u32 disasm_addr = addr - (instructions_before * sizeof(u32));
  for (u32 i = 0; i < instructions_before; i++)
  {
    DisassembleAndPrint(disasm_addr, false, "");
    disasm_addr += sizeof(u32);
  }

  // <= to include the instruction itself
  for (u32 i = 0; i <= instructions_after; i++)
  {
    DisassembleAndPrint(disasm_addr, (i == 0), (i == 0) ? "---->" : "");
    disasm_addr += sizeof(u32);
  }
}

template<PGXPMode pgxp_mode, bool debug>
ALWAYS_INLINE_RELEASE void CPU::ExecuteInstruction()
{
restart_instruction:
  const Instruction inst = g_state.current_instruction;

#if 0
  if (g_state.current_instruction_pc == 0x80030000)
  {
    TRACE_EXECUTION = true;
    __debugbreak();
  }
#endif

#ifdef _DEBUG
  if (TRACE_EXECUTION)
    TracePrintInstruction();
#endif

  // Skip nops. Makes PGXP-CPU quicker, but also the regular interpreter.
  if (inst.bits == 0)
    return;

  switch (inst.op)
  {
    case InstructionOp::funct:
    {
      switch (inst.r.funct)
      {
        case InstructionFunct::sll:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rtVal << inst.r.shamt;
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLL(inst.bits, rtVal);
        }
        break;

        case InstructionFunct::srl:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rtVal >> inst.r.shamt;
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRL(inst.bits, rtVal);
        }
        break;

        case InstructionFunct::sra:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> inst.r.shamt);
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRA(inst.bits, rtVal);
        }
        break;

        case InstructionFunct::sllv:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 shamt = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 rdVal = rtVal << shamt;
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLLV(inst.bits, rtVal, shamt);

          WriteReg(inst.r.rd, rdVal);
        }
        break;

        case InstructionFunct::srlv:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 shamt = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 rdVal = rtVal >> shamt;
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRLV(inst.bits, rtVal, shamt);
        }
        break;

        case InstructionFunct::srav:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 shamt = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> shamt);
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRAV(inst.bits, rtVal, shamt);
        }
        break;

        case InstructionFunct::and_:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 new_value = rsVal & rtVal;
          WriteReg(inst.r.rd, new_value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_AND_(inst.bits, rsVal, rtVal);
        }
        break;

        case InstructionFunct::or_:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 new_value = rsVal | rtVal;
          WriteReg(inst.r.rd, new_value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_OR_(inst.bits, rsVal, rtVal);
          else if constexpr (pgxp_mode >= PGXPMode::Memory)
            PGXP::TryMove(inst.r.rd, inst.r.rs, inst.r.rt);
        }
        break;

        case InstructionFunct::xor_:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 new_value = rsVal ^ rtVal;
          WriteReg(inst.r.rd, new_value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_XOR_(inst.bits, rsVal, rtVal);
          else if constexpr (pgxp_mode >= PGXPMode::Memory)
            PGXP::TryMove(inst.r.rd, inst.r.rs, inst.r.rt);
        }
        break;

        case InstructionFunct::nor:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 new_value = ~(rsVal | rtVal);
          WriteReg(inst.r.rd, new_value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_NOR(inst.bits, rsVal, rtVal);
        }
        break;

        case InstructionFunct::add:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rsVal + rtVal;
          if (AddOverflow(rsVal, rtVal, rdVal))
          {
            RaiseException(Exception::Ov);
            return;
          }

          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode == PGXPMode::CPU)
            PGXP::CPU_ADD(inst.bits, rsVal, rtVal);
          else if constexpr (pgxp_mode >= PGXPMode::Memory)
            PGXP::TryMove(inst.r.rd, inst.r.rs, inst.r.rt);
        }
        break;

        case InstructionFunct::addu:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rsVal + rtVal;
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_ADD(inst.bits, rsVal, rtVal);
          else if constexpr (pgxp_mode >= PGXPMode::Memory)
            PGXP::TryMove(inst.r.rd, inst.r.rs, inst.r.rt);
        }
        break;

        case InstructionFunct::sub:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rsVal - rtVal;
          if (SubOverflow(rsVal, rtVal, rdVal))
          {
            RaiseException(Exception::Ov);
            return;
          }

          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SUB(inst.bits, rsVal, rtVal);
        }
        break;

        case InstructionFunct::subu:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rsVal - rtVal;
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SUB(inst.bits, rsVal, rtVal);
        }
        break;

        case InstructionFunct::slt:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 result = BoolToUInt32(static_cast<s32>(rsVal) < static_cast<s32>(rtVal));
          WriteReg(inst.r.rd, result);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLT(inst.bits, rsVal, rtVal);
        }
        break;

        case InstructionFunct::sltu:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 result = BoolToUInt32(rsVal < rtVal);
          WriteReg(inst.r.rd, result);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLTU(inst.bits, rsVal, rtVal);
        }
        break;

        case InstructionFunct::mfhi:
        {
          const u32 value = g_state.regs.hi;
          WriteReg(inst.r.rd, value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MOVE(static_cast<u32>(inst.r.rd.GetValue()), static_cast<u32>(Reg::hi), value);
        }
        break;

        case InstructionFunct::mthi:
        {
          const u32 value = ReadReg(inst.r.rs);
          g_state.regs.hi = value;

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MOVE(static_cast<u32>(Reg::hi), static_cast<u32>(inst.r.rs.GetValue()), value);
        }
        break;

        case InstructionFunct::mflo:
        {
          const u32 value = g_state.regs.lo;
          WriteReg(inst.r.rd, value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MOVE(static_cast<u32>(inst.r.rd.GetValue()), static_cast<u32>(Reg::lo), value);
        }
        break;

        case InstructionFunct::mtlo:
        {
          const u32 value = ReadReg(inst.r.rs);
          g_state.regs.lo = value;

          if constexpr (pgxp_mode == PGXPMode::CPU)
            PGXP::CPU_MOVE(static_cast<u32>(Reg::lo), static_cast<u32>(inst.r.rs.GetValue()), value);
        }
        break;

        case InstructionFunct::mult:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result =
            static_cast<u64>(static_cast<s64>(SignExtend64(lhs)) * static_cast<s64>(SignExtend64(rhs)));

          g_state.regs.hi = Truncate32(result >> 32);
          g_state.regs.lo = Truncate32(result);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MULT(inst.bits, lhs, rhs);
        }
        break;

        case InstructionFunct::multu:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result = ZeroExtend64(lhs) * ZeroExtend64(rhs);

          g_state.regs.hi = Truncate32(result >> 32);
          g_state.regs.lo = Truncate32(result);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MULTU(inst.bits, lhs, rhs);
        }
        break;

        case InstructionFunct::div:
        {
          const s32 num = static_cast<s32>(ReadReg(inst.r.rs));
          const s32 denom = static_cast<s32>(ReadReg(inst.r.rt));

          if (denom == 0)
          {
            // divide by zero
            g_state.regs.lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
            g_state.regs.hi = static_cast<u32>(num);
          }
          else if (static_cast<u32>(num) == UINT32_C(0x80000000) && denom == -1)
          {
            // unrepresentable
            g_state.regs.lo = UINT32_C(0x80000000);
            g_state.regs.hi = 0;
          }
          else
          {
            g_state.regs.lo = static_cast<u32>(num / denom);
            g_state.regs.hi = static_cast<u32>(num % denom);
          }

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_DIV(inst.bits, num, denom);
        }
        break;

        case InstructionFunct::divu:
        {
          const u32 num = ReadReg(inst.r.rs);
          const u32 denom = ReadReg(inst.r.rt);

          if (denom == 0)
          {
            // divide by zero
            g_state.regs.lo = UINT32_C(0xFFFFFFFF);
            g_state.regs.hi = static_cast<u32>(num);
          }
          else
          {
            g_state.regs.lo = num / denom;
            g_state.regs.hi = num % denom;
          }

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_DIVU(inst.bits, num, denom);
        }
        break;

        case InstructionFunct::jr:
        {
          g_state.next_instruction_is_branch_delay_slot = true;
          const u32 target = ReadReg(inst.r.rs);
          Branch(target);
        }
        break;

        case InstructionFunct::jalr:
        {
          g_state.next_instruction_is_branch_delay_slot = true;
          const u32 target = ReadReg(inst.r.rs);
          WriteReg(inst.r.rd, g_state.npc);
          Branch(target);
        }
        break;

        case InstructionFunct::syscall:
        {
          RaiseException(Exception::Syscall);
        }
        break;

        case InstructionFunct::break_:
        {
          RaiseBreakException(Cop0Registers::CAUSE::MakeValueForException(
                                Exception::BP, g_state.current_instruction_in_branch_delay_slot,
                                g_state.current_instruction_was_branch_taken, g_state.current_instruction.cop.cop_n),
                              g_state.current_instruction_pc, g_state.current_instruction.bits);
        }
        break;

        default:
        {
          RaiseException(Exception::RI);
          break;
        }
      }
    }
    break;

    case InstructionOp::lui:
    {
      const u32 value = inst.i.imm_zext32() << 16;
      WriteReg(inst.i.rt, value);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_LUI(inst.bits);
    }
    break;

    case InstructionOp::andi:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 new_value = rsVal & inst.i.imm_zext32();
      WriteReg(inst.i.rt, new_value);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ANDI(inst.bits, rsVal);
    }
    break;

    case InstructionOp::ori:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 imm = inst.i.imm_zext32();
      const u32 rtVal = rsVal | imm;
      WriteReg(inst.i.rt, rtVal);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ORI(inst.bits, rsVal);
      else if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::TryMoveImm(inst.r.rd, inst.r.rs, imm);
    }
    break;

    case InstructionOp::xori:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 imm = inst.i.imm_zext32();
      const u32 new_value = ReadReg(inst.i.rs) ^ imm;
      WriteReg(inst.i.rt, new_value);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_XORI(inst.bits, rsVal);
      else if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::TryMoveImm(inst.r.rd, inst.r.rs, imm);
    }
    break;

    case InstructionOp::addi:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 imm = inst.i.imm_sext32();
      const u32 rtVal = rsVal + imm;
      if (AddOverflow(rsVal, imm, rtVal))
      {
        RaiseException(Exception::Ov);
        return;
      }

      WriteReg(inst.i.rt, rtVal);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ADDI(inst.bits, rsVal);
      else if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::TryMoveImm(inst.r.rd, inst.r.rs, imm);
    }
    break;

    case InstructionOp::addiu:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 imm = inst.i.imm_sext32();
      const u32 rtVal = rsVal + imm;
      WriteReg(inst.i.rt, rtVal);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ADDI(inst.bits, rsVal);
      else if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::TryMoveImm(inst.r.rd, inst.r.rs, imm);
    }
    break;

    case InstructionOp::slti:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 result = BoolToUInt32(static_cast<s32>(rsVal) < static_cast<s32>(inst.i.imm_sext32()));
      WriteReg(inst.i.rt, result);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_SLTI(inst.bits, rsVal);
    }
    break;

    case InstructionOp::sltiu:
    {
      const u32 result = BoolToUInt32(ReadReg(inst.i.rs) < inst.i.imm_sext32());
      WriteReg(inst.i.rt, result);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_SLTIU(inst.bits, ReadReg(inst.i.rs));
    }
    break;

    case InstructionOp::lb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);

      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      const u32 sxvalue = SignExtend32(value);

      WriteRegDelayed(inst.i.rt, sxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LBx(inst.bits, addr, sxvalue);
    }
    break;

    case InstructionOp::lh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);

      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      const u32 sxvalue = SignExtend32(value);
      WriteRegDelayed(inst.i.rt, sxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LH(inst.bits, addr, sxvalue);
    }
    break;

    case InstructionOp::lw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);

      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LW(inst.bits, addr, value);
    }
    break;

    case InstructionOp::lbu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);

      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      const u32 zxvalue = ZeroExtend32(value);
      WriteRegDelayed(inst.i.rt, zxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LBx(inst.bits, addr, zxvalue);
    }
    break;

    case InstructionOp::lhu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);

      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      const u32 zxvalue = ZeroExtend32(value);
      WriteRegDelayed(inst.i.rt, zxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LHU(inst.bits, addr, zxvalue);
    }
    break;

    case InstructionOp::lwl:
    case InstructionOp::lwr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);

      u32 aligned_value;
      if (!ReadMemoryWord(aligned_addr, &aligned_value))
        return;

      // Bypasses load delay. No need to check the old value since this is the delay slot or it's not relevant.
      const u32 existing_value = (inst.i.rt == g_state.load_delay_reg) ? g_state.load_delay_value : ReadReg(inst.i.rt);
      const u8 shift = (Truncate8(addr) & u8(3)) * u8(8);
      u32 new_value;
      if (inst.op == InstructionOp::lwl)
      {
        const u32 mask = UINT32_C(0x00FFFFFF) >> shift;
        new_value = (existing_value & mask) | (aligned_value << (24 - shift));
      }
      else
      {
        const u32 mask = UINT32_C(0xFFFFFF00) << (24 - shift);
        new_value = (existing_value & mask) | (aligned_value >> shift);
      }

      WriteRegDelayed(inst.i.rt, new_value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LW(inst.bits, addr, new_value);
    }
    break;

    case InstructionOp::sb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(addr);

      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryByte(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SB(inst.bits, addr, value);
    }
    break;

    case InstructionOp::sh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(addr);

      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryHalfWord(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SH(inst.bits, addr, value);
    }
    break;

    case InstructionOp::sw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(addr);

      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryWord(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SW(inst.bits, addr, value);
    }
    break;

    case InstructionOp::swl:
    case InstructionOp::swr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
      if constexpr (debug)
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(aligned_addr);

      const u32 reg_value = ReadReg(inst.i.rt);
      const u8 shift = (Truncate8(addr) & u8(3)) * u8(8);
      u32 mem_value;
      if (!ReadMemoryWord(aligned_addr, &mem_value))
        return;

      u32 new_value;
      if (inst.op == InstructionOp::swl)
      {
        const u32 mem_mask = UINT32_C(0xFFFFFF00) << shift;
        new_value = (mem_value & mem_mask) | (reg_value >> (24 - shift));
      }
      else
      {
        const u32 mem_mask = UINT32_C(0x00FFFFFF) >> (24 - shift);
        new_value = (mem_value & mem_mask) | (reg_value << shift);
      }

      WriteMemoryWord(aligned_addr, new_value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SW(inst.bits, aligned_addr, new_value);
    }
    break;

    case InstructionOp::j:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      Branch((g_state.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::jal:
    {
      WriteReg(Reg::ra, g_state.npc);
      g_state.next_instruction_is_branch_delay_slot = true;
      Branch((g_state.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::beq:
    {
      // We're still flagged as a branch delay slot even if the branch isn't taken.
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (ReadReg(inst.i.rs) == ReadReg(inst.i.rt));
      if (branch)
        Branch(g_state.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bne:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (ReadReg(inst.i.rs) != ReadReg(inst.i.rt));
      if (branch)
        Branch(g_state.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bgtz:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) > 0);
      if (branch)
        Branch(g_state.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::blez:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) <= 0);
      if (branch)
        Branch(g_state.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::b:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      const u8 rt = static_cast<u8>(inst.i.rt.GetValue());

      // bgez is the inverse of bltz, so simply do ltz and xor the result
      const bool bgez = ConvertToBoolUnchecked(rt & u8(1));
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) < 0) ^ bgez;

      // register is still linked even if the branch isn't taken
      const bool link = (rt & u8(0x1E)) == u8(0x10);
      if (link)
        WriteReg(Reg::ra, g_state.npc);

      if (branch)
        Branch(g_state.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::cop0:
    {
      if (InUserMode() && !g_state.cop0_regs.sr.CU0)
      {
        Log_WarningPrintf("Coprocessor 0 not present in user mode");
        RaiseException(Exception::CpU);
        return;
      }

      if (inst.cop.IsCommonInstruction())
      {
        switch (inst.cop.CommonOp())
        {
          case CopCommonInstruction::mfcn:
          {
            const u32 value = ReadCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue()));
            WriteRegDelayed(inst.r.rt, value);

            if constexpr (pgxp_mode == PGXPMode::CPU)
              PGXP::CPU_MFC0(inst.bits, value);
          }
          break;

          case CopCommonInstruction::mtcn:
          {
            const u32 rtVal = ReadReg(inst.r.rt);
            WriteCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue()), rtVal);

            if constexpr (pgxp_mode == PGXPMode::CPU)
              PGXP::CPU_MTC0(inst.bits, ReadCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue())), rtVal);
          }
          break;

          default:
            Log_ErrorPrintf("Unhandled instruction at %08X: %08X", g_state.current_instruction_pc, inst.bits);
            break;
        }
      }
      else
      {
        switch (inst.cop.Cop0Op())
        {
          case Cop0Instruction::rfe:
          {
            // restore mode
            g_state.cop0_regs.sr.mode_bits =
              (g_state.cop0_regs.sr.mode_bits & UINT32_C(0b110000)) | (g_state.cop0_regs.sr.mode_bits >> 2);
            CheckForPendingInterrupt();
          }
          break;

          case Cop0Instruction::tlbr:
          case Cop0Instruction::tlbwi:
          case Cop0Instruction::tlbwr:
          case Cop0Instruction::tlbp:
            RaiseException(Exception::RI);
            break;

          default:
            Log_ErrorPrintf("Unhandled instruction at %08X: %08X", g_state.current_instruction_pc, inst.bits);
            break;
        }
      }
    }
    break;

    case InstructionOp::cop2:
    {
      if (!g_state.cop0_regs.sr.CE2)
      {
        Log_WarningPrintf("Coprocessor 2 not enabled");
        RaiseException(Exception::CpU);
        return;
      }

      StallUntilGTEComplete();

      if (inst.cop.IsCommonInstruction())
      {
        // TODO: Combine with cop0.
        switch (inst.cop.CommonOp())
        {
          case CopCommonInstruction::cfcn:
          {
            const u32 value = GTE::ReadRegister(static_cast<u32>(inst.r.rd.GetValue()) + 32);
            WriteRegDelayed(inst.r.rt, value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MFC2(inst.bits, value);
          }
          break;

          case CopCommonInstruction::ctcn:
          {
            const u32 value = ReadReg(inst.r.rt);
            GTE::WriteRegister(static_cast<u32>(inst.r.rd.GetValue()) + 32, value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MTC2(inst.bits, value);
          }
          break;

          case CopCommonInstruction::mfcn:
          {
            const u32 value = GTE::ReadRegister(static_cast<u32>(inst.r.rd.GetValue()));
            WriteRegDelayed(inst.r.rt, value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MFC2(inst.bits, value);
          }
          break;

          case CopCommonInstruction::mtcn:
          {
            const u32 value = ReadReg(inst.r.rt);
            GTE::WriteRegister(static_cast<u32>(inst.r.rd.GetValue()), value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MTC2(inst.bits, value);
          }
          break;

          default:
            Log_ErrorPrintf("Unhandled instruction at %08X: %08X", g_state.current_instruction_pc, inst.bits);
            break;
        }
      }
      else
      {
        GTE::ExecuteInstruction(inst.bits);
      }
    }
    break;

    case InstructionOp::lwc2:
    {
      if (!g_state.cop0_regs.sr.CE2)
      {
        Log_WarningPrintf("Coprocessor 2 not enabled");
        RaiseException(Exception::CpU);
        return;
      }

      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      StallUntilGTEComplete();
      GTE::WriteRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())), value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LWC2(inst.bits, addr, value);
    }
    break;

    case InstructionOp::swc2:
    {
      if (!g_state.cop0_regs.sr.CE2)
      {
        Log_WarningPrintf("Coprocessor 2 not enabled");
        RaiseException(Exception::CpU);
        return;
      }

      StallUntilGTEComplete();

      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = GTE::ReadRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())));
      WriteMemoryWord(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SWC2(inst.bits, addr, value);
    }
    break;

      // swc0/lwc0/cop1/cop3 are essentially no-ops
    case InstructionOp::cop1:
    case InstructionOp::cop3:
    case InstructionOp::lwc0:
    case InstructionOp::lwc1:
    case InstructionOp::lwc3:
    case InstructionOp::swc0:
    case InstructionOp::swc1:
    case InstructionOp::swc3:
    {
    }
    break;

      // everything else is reserved/invalid
    default:
    {
      u32 ram_value;
      if (SafeReadInstruction(g_state.current_instruction_pc, &ram_value) &&
          ram_value != g_state.current_instruction.bits)
      {
        Log_ErrorPrintf("Stale icache at 0x%08X - ICache: %08X RAM: %08X", g_state.current_instruction_pc,
                        g_state.current_instruction.bits, ram_value);
        g_state.current_instruction.bits = ram_value;
        goto restart_instruction;
      }

      RaiseException(Exception::RI);
    }
    break;
  }
}

void CPU::DispatchInterrupt()
{
  // If the instruction we're about to execute is a GTE instruction, delay dispatching the interrupt until the next
  // instruction. For some reason, if we don't do this, we end up with incorrectly sorted polygons and flickering..
  SafeReadInstruction(g_state.pc, &g_state.next_instruction.bits);
  if (g_state.next_instruction.op == InstructionOp::cop2 && !g_state.next_instruction.cop.IsCommonInstruction())
  {
    StallUntilGTEComplete();
    GTE::ExecuteInstruction(g_state.next_instruction.bits);
  }

  // Interrupt raising occurs before the start of the instruction.
  RaiseException(
    Cop0Registers::CAUSE::MakeValueForException(Exception::INT, g_state.next_instruction_is_branch_delay_slot,
                                                g_state.branch_was_taken, g_state.next_instruction.cop.cop_n),
    g_state.pc);

  // Fix up downcount, the pending IRQ set it to zero.
  TimingEvents::UpdateCPUDowncount();
}

bool CPU::UpdateDebugDispatcherFlag()
{
  const bool has_any_breakpoints = !s_breakpoints.empty();

  // TODO: cop0 breakpoints
  const auto& dcic = g_state.cop0_regs.dcic;
  const bool has_cop0_breakpoints = dcic.super_master_enable_1 && dcic.super_master_enable_2 &&
                                    dcic.execution_breakpoint_enable && IsCop0ExecutionBreakpointUnmasked();

  const bool use_debug_dispatcher =
    has_any_breakpoints || has_cop0_breakpoints || s_trace_to_log ||
    (g_settings.cpu_execution_mode == CPUExecutionMode::Interpreter && g_settings.bios_tty_logging);
  if (use_debug_dispatcher == g_state.use_debug_dispatcher)
    return false;

  Log_DevPrintf("%s debug dispatcher", use_debug_dispatcher ? "Now using" : "No longer using");
  g_state.use_debug_dispatcher = use_debug_dispatcher;
  return true;
}

void CPU::ExitExecution()
{
  // can't exit while running events without messing things up
  if (TimingEvents::IsRunningEvents())
    TimingEvents::SetFrameDone();
  else
    fastjmp_jmp(&s_jmp_buf, 1);
}

bool CPU::HasAnyBreakpoints()
{
  return !s_breakpoints.empty();
}

bool CPU::HasBreakpointAtAddress(VirtualMemoryAddress address)
{
  for (const Breakpoint& bp : s_breakpoints)
  {
    if (bp.address == address)
      return true;
  }

  return false;
}

CPU::BreakpointList CPU::GetBreakpointList(bool include_auto_clear, bool include_callbacks)
{
  BreakpointList bps;
  bps.reserve(s_breakpoints.size());

  for (const Breakpoint& bp : s_breakpoints)
  {
    if (bp.callback && !include_callbacks)
      continue;
    if (bp.auto_clear && !include_auto_clear)
      continue;

    bps.push_back(bp);
  }

  return bps;
}

bool CPU::AddBreakpoint(VirtualMemoryAddress address, bool auto_clear, bool enabled)
{
  if (HasBreakpointAtAddress(address))
    return false;

  Log_InfoPrintf("Adding breakpoint at %08X, auto clear = %u", address, static_cast<unsigned>(auto_clear));

  Breakpoint bp{address, nullptr, auto_clear ? 0 : s_breakpoint_counter++, 0, auto_clear, enabled};
  s_breakpoints.push_back(std::move(bp));
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();

  if (!auto_clear)
  {
    Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Added breakpoint at 0x%08X."), address);
  }

  return true;
}

bool CPU::AddBreakpointWithCallback(VirtualMemoryAddress address, BreakpointCallback callback)
{
  if (HasBreakpointAtAddress(address))
    return false;

  Log_InfoPrintf("Adding breakpoint with callback at %08X", address);

  Breakpoint bp{address, callback, 0, 0, false, true};
  s_breakpoints.push_back(std::move(bp));
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();
  return true;
}

bool CPU::RemoveBreakpoint(VirtualMemoryAddress address)
{
  auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
                         [address](const Breakpoint& bp) { return bp.address == address; });
  if (it == s_breakpoints.end())
    return false;

  Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Removed breakpoint at 0x%08X."), address);

  s_breakpoints.erase(it);
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();

  if (address == s_last_breakpoint_check_pc)
    s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;

  return true;
}

void CPU::ClearBreakpoints()
{
  s_breakpoints.clear();
  s_breakpoint_counter = 0;
  s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();
}

bool CPU::AddStepOverBreakpoint()
{
  u32 bp_pc = g_state.pc;

  Instruction inst;
  if (!SafeReadInstruction(bp_pc, &inst.bits))
    return false;

  bp_pc += sizeof(Instruction);

  if (!IsCallInstruction(inst))
  {
    Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "0x%08X is not a call instruction."), g_state.pc);
    return false;
  }

  if (!SafeReadInstruction(bp_pc, &inst.bits))
    return false;

  if (IsBranchInstruction(inst))
  {
    Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Can't step over double branch at 0x%08X"),
                                         g_state.pc);
    return false;
  }

  // skip the delay slot
  bp_pc += sizeof(Instruction);

  Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Stepping over to 0x%08X."), bp_pc);

  return AddBreakpoint(bp_pc, true);
}

bool CPU::AddStepOutBreakpoint(u32 max_instructions_to_search)
{
  // find the branch-to-ra instruction.
  u32 ret_pc = g_state.pc;
  for (u32 i = 0; i < max_instructions_to_search; i++)
  {
    ret_pc += sizeof(Instruction);

    Instruction inst;
    if (!SafeReadInstruction(ret_pc, &inst.bits))
    {
      Host::ReportFormattedDebuggerMessage(
        TRANSLATE("DebuggerMessage", "Instruction read failed at %08X while searching for function end."), ret_pc);
      return false;
    }

    if (IsReturnInstruction(inst))
    {
      Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Stepping out to 0x%08X."), ret_pc);

      return AddBreakpoint(ret_pc, true);
    }
  }

  Host::ReportFormattedDebuggerMessage(
    TRANSLATE("DebuggerMessage", "No return instruction found after %u instructions for step-out at %08X."),
    max_instructions_to_search, g_state.pc);

  return false;
}

bool CPU::BreakpointCheck()
{
  const u32 pc = g_state.pc;

  // single step - we want to break out after this instruction, so set a pending exit
  // the bp check happens just before execution, so this is fine
  if (s_single_step)
  {
    if (s_single_step_done)
      ExitExecution();
    else
      s_single_step_done = true;

    s_last_breakpoint_check_pc = pc;
    return false;
  }

  if (pc == s_last_breakpoint_check_pc)
  {
    // we don't want to trigger the same breakpoint which just paused us repeatedly.
    return false;
  }

  u32 count = static_cast<u32>(s_breakpoints.size());
  for (u32 i = 0; i < count;)
  {
    Breakpoint& bp = s_breakpoints[i];
    if (!bp.enabled || bp.address != pc)
    {
      i++;
      continue;
    }

    bp.hit_count++;

    if (bp.callback)
    {
      // if callback returns false, the bp is no longer recorded
      if (!bp.callback(pc))
      {
        s_breakpoints.erase(s_breakpoints.begin() + i);
        count--;
        UpdateDebugDispatcherFlag();
      }
      else
      {
        i++;
      }
    }
    else
    {
      System::PauseSystem(true);

      if (bp.auto_clear)
      {
        Host::ReportFormattedDebuggerMessage("Stopped execution at 0x%08X.", pc);
        s_breakpoints.erase(s_breakpoints.begin() + i);
        count--;
        UpdateDebugDispatcherFlag();
      }
      else
      {
        Host::ReportFormattedDebuggerMessage("Hit breakpoint %u at 0x%08X.", bp.number, pc);
        i++;
      }

      ExitExecution();
    }
  }

  s_last_breakpoint_check_pc = pc;
  return System::IsPaused();
}

template<PGXPMode pgxp_mode, bool debug>
[[noreturn]] void CPU::ExecuteImpl()
{
  for (;;)
  {
    TimingEvents::RunEvents();

    while (g_state.pending_ticks < g_state.downcount)
    {
      if constexpr (debug)
      {
        Cop0ExecutionBreakpointCheck();

        if (BreakpointCheck())
        {
          // continue is measurably faster than break on msvc for some reason
          continue;
        }
      }

      g_state.pending_ticks++;

      // now executing the instruction we previously fetched
      g_state.current_instruction.bits = g_state.next_instruction.bits;
      g_state.current_instruction_pc = g_state.pc;
      g_state.current_instruction_in_branch_delay_slot = g_state.next_instruction_is_branch_delay_slot;
      g_state.current_instruction_was_branch_taken = g_state.branch_was_taken;
      g_state.next_instruction_is_branch_delay_slot = false;
      g_state.branch_was_taken = false;
      g_state.exception_raised = false;

      // fetch the next instruction - even if this fails, it'll still refetch on the flush so we can continue
      if (!FetchInstruction())
        continue;

      // trace functionality
      if constexpr (debug)
      {
        if (s_trace_to_log)
          LogInstruction(g_state.current_instruction.bits, g_state.current_instruction_pc, true);

        if (g_state.current_instruction_pc == 0xA0) [[unlikely]]
          HandleA0Syscall();
        else if (g_state.current_instruction_pc == 0xB0) [[unlikely]]
          HandleB0Syscall();
      }

#if 0 // GTE flag test debugging
      if (g_state.m_current_instruction_pc == 0x8002cdf4)
      {
        if (g_state.m_regs.v1 != g_state.m_regs.v0)
          printf("Got %08X Expected? %08X\n", g_state.m_regs.v1, g_state.m_regs.v0);
      }
#endif

      // execute the instruction we previously fetched
      ExecuteInstruction<pgxp_mode, debug>();

      // next load delay
      UpdateLoadDelay();
    }
  }
}

void CPU::ExecuteDebug()
{
  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_pgxp_cpu)
      ExecuteImpl<PGXPMode::CPU, true>();
    else
      ExecuteImpl<PGXPMode::Memory, true>();
  }
  else
  {
    ExecuteImpl<PGXPMode::Disabled, true>();
  }
}

void CPU::Execute()
{
  const CPUExecutionMode exec_mode = g_settings.cpu_execution_mode;
  const bool use_debug_dispatcher = g_state.use_debug_dispatcher;
  if (fastjmp_set(&s_jmp_buf) != 0)
  {
    // Before we return, set npc to pc so that we can switch from recs to int.
    if (exec_mode != CPUExecutionMode::Interpreter && !use_debug_dispatcher)
      g_state.npc = g_state.pc;

    return;
  }

  if (use_debug_dispatcher)
  {
    ExecuteDebug();
    return;
  }

  switch (exec_mode)
  {
    case CPUExecutionMode::Recompiler:
    case CPUExecutionMode::CachedInterpreter:
    case CPUExecutionMode::NewRec:
      CodeCache::Execute();
      break;

    case CPUExecutionMode::Interpreter:
    default:
    {
      if (g_settings.gpu_pgxp_enable)
      {
        if (g_settings.gpu_pgxp_cpu)
          ExecuteImpl<PGXPMode::CPU, false>();
        else
          ExecuteImpl<PGXPMode::Memory, false>();
      }
      else
      {
        ExecuteImpl<PGXPMode::Disabled, false>();
      }
    }
    break;
  }
}

void CPU::SingleStep()
{
  s_single_step = true;
  s_single_step_done = false;
  if (fastjmp_set(&s_jmp_buf) == 0)
    ExecuteDebug();
  Host::ReportFormattedDebuggerMessage("Stepped to 0x%08X.", g_state.pc);
}

template<PGXPMode pgxp_mode>
void CPU::CodeCache::InterpretCachedBlock(const Block* block)
{
  // set up the state so we've already fetched the instruction
  DebugAssert(g_state.pc == block->pc);
  g_state.npc = block->pc + 4;

  const Instruction* instruction = block->Instructions();
  const Instruction* end_instruction = instruction + block->size;
  const CodeCache::InstructionInfo* info = block->InstructionsInfo();

  do
  {
    g_state.pending_ticks++;

    // now executing the instruction we previously fetched
    g_state.current_instruction.bits = instruction->bits;
    g_state.current_instruction_pc = info->pc;
    g_state.current_instruction_in_branch_delay_slot = info->is_branch_delay_slot; // TODO: let int set it instead
    g_state.current_instruction_was_branch_taken = g_state.branch_was_taken;
    g_state.branch_was_taken = false;
    g_state.exception_raised = false;

    // update pc
    g_state.pc = g_state.npc;
    g_state.npc += 4;

    // execute the instruction we previously fetched
    ExecuteInstruction<pgxp_mode, false>();

    // next load delay
    UpdateLoadDelay();

    if (g_state.exception_raised)
      break;

    instruction++;
    info++;
  } while (instruction != end_instruction);

  // cleanup so the interpreter can kick in if needed
  g_state.next_instruction_is_branch_delay_slot = false;
}

template void CPU::CodeCache::InterpretCachedBlock<PGXPMode::Disabled>(const Block* block);
template void CPU::CodeCache::InterpretCachedBlock<PGXPMode::Memory>(const Block* block);
template void CPU::CodeCache::InterpretCachedBlock<PGXPMode::CPU>(const Block* block);

template<PGXPMode pgxp_mode>
void CPU::CodeCache::InterpretUncachedBlock()
{
  g_state.npc = g_state.pc;
  if (!FetchInstructionForInterpreterFallback())
    return;

  // At this point, pc contains the last address executed (in the previous block). The instruction has not been fetched
  // yet. pc shouldn't be updated until the fetch occurs, that way the exception occurs in the delay slot.
  bool in_branch_delay_slot = false;
  for (;;)
  {
    g_state.pending_ticks++;

    // now executing the instruction we previously fetched
    g_state.current_instruction.bits = g_state.next_instruction.bits;
    g_state.current_instruction_pc = g_state.pc;
    g_state.current_instruction_in_branch_delay_slot = g_state.next_instruction_is_branch_delay_slot;
    g_state.current_instruction_was_branch_taken = g_state.branch_was_taken;
    g_state.next_instruction_is_branch_delay_slot = false;
    g_state.branch_was_taken = false;
    g_state.exception_raised = false;

    // Fetch the next instruction, except if we're in a branch delay slot. The "fetch" is done in the next block.
    const bool branch = IsBranchInstruction(g_state.current_instruction);
    if (!g_state.current_instruction_in_branch_delay_slot || branch)
    {
      if (!FetchInstructionForInterpreterFallback())
        break;
    }
    else
    {
      g_state.pc = g_state.npc;
    }

    // execute the instruction we previously fetched
    ExecuteInstruction<pgxp_mode, false>();

    // next load delay
    UpdateLoadDelay();

    if (g_state.exception_raised || (!branch && in_branch_delay_slot) ||
        IsExitBlockInstruction(g_state.current_instruction))
    {
      break;
    }
    else if ((g_state.current_instruction.bits & 0xFFC0FFFFu) == 0x40806000u && HasPendingInterrupt())
    {
      // mtc0 rt, sr - Jackie Chan Stuntmaster, MTV Sports games.
      // Pain in the ass games trigger a software interrupt by writing to SR.Im.
      break;
    }

    in_branch_delay_slot = branch;
  }
}

template void CPU::CodeCache::InterpretUncachedBlock<PGXPMode::Disabled>();
template void CPU::CodeCache::InterpretUncachedBlock<PGXPMode::Memory>();
template void CPU::CodeCache::InterpretUncachedBlock<PGXPMode::CPU>();

bool CPU::Recompiler::Thunks::InterpretInstruction()
{
  ExecuteInstruction<PGXPMode::Disabled, false>();
  return g_state.exception_raised;
}

bool CPU::Recompiler::Thunks::InterpretInstructionPGXP()
{
  ExecuteInstruction<PGXPMode::Memory, false>();
  return g_state.exception_raised;
}

ALWAYS_INLINE_RELEASE Bus::MemoryReadHandler CPU::GetMemoryReadHandler(VirtualMemoryAddress address,
                                                                       MemoryAccessSize size)
{
  Bus::MemoryReadHandler* base =
    Bus::OffsetHandlerArray<Bus::MemoryReadHandler>(g_state.memory_handlers, size, MemoryAccessType::Read);
  return base[address >> Bus::MEMORY_LUT_PAGE_SHIFT];
}

ALWAYS_INLINE_RELEASE Bus::MemoryWriteHandler CPU::GetMemoryWriteHandler(VirtualMemoryAddress address,
                                                                         MemoryAccessSize size)
{
  Bus::MemoryWriteHandler* base =
    Bus::OffsetHandlerArray<Bus::MemoryWriteHandler>(g_state.memory_handlers, size, MemoryAccessType::Write);
  return base[address >> Bus::MEMORY_LUT_PAGE_SHIFT];
}

void CPU::UpdateMemoryPointers()
{
  g_state.memory_handlers = Bus::GetMemoryHandlers(g_state.cop0_regs.sr.Isc, g_state.cop0_regs.sr.Swc);
  g_state.fastmem_base = Bus::GetFastmemBase(g_state.cop0_regs.sr.Isc);
}

void CPU::ExecutionModeChanged()
{
  g_state.bus_error = false;
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();
}

template<bool add_ticks, bool icache_read, u32 word_count, bool raise_exceptions>
ALWAYS_INLINE_RELEASE bool CPU::DoInstructionRead(PhysicalMemoryAddress address, void* data)
{
  using namespace Bus;

  address &= PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < RAM_MIRROR_END)
  {
    std::memcpy(data, &g_ram[address & g_ram_mask], sizeof(u32) * word_count);
    if constexpr (add_ticks)
      g_state.pending_ticks += (icache_read ? 1 : RAM_READ_TICKS) * word_count;

    return true;
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
  {
    std::memcpy(data, &g_bios[(address - BIOS_BASE) & BIOS_MASK], sizeof(u32) * word_count);
    if constexpr (add_ticks)
      g_state.pending_ticks += g_bios_access_time[static_cast<u32>(MemoryAccessSize::Word)] * word_count;

    return true;
  }
  else
  {
    if (raise_exceptions)
      CPU::RaiseException(address, Cop0Registers::CAUSE::MakeValueForException(Exception::IBE, false, false, 0));

    std::memset(data, 0, sizeof(u32) * word_count);
    return false;
  }
}

TickCount CPU::GetInstructionReadTicks(VirtualMemoryAddress address)
{
  using namespace Bus;

  address &= PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < RAM_MIRROR_END)
  {
    return RAM_READ_TICKS;
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
  {
    return g_bios_access_time[static_cast<u32>(MemoryAccessSize::Word)];
  }
  else
  {
    return 0;
  }
}

TickCount CPU::GetICacheFillTicks(VirtualMemoryAddress address)
{
  using namespace Bus;

  address &= PHYSICAL_MEMORY_ADDRESS_MASK;

  if (address < RAM_MIRROR_END)
  {
    return 1 * ((ICACHE_LINE_SIZE - (address & (ICACHE_LINE_SIZE - 1))) / sizeof(u32));
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
  {
    return g_bios_access_time[static_cast<u32>(MemoryAccessSize::Word)] *
           ((ICACHE_LINE_SIZE - (address & (ICACHE_LINE_SIZE - 1))) / sizeof(u32));
  }
  else
  {
    return 0;
  }
}

void CPU::CheckAndUpdateICacheTags(u32 line_count, TickCount uncached_ticks)
{
  VirtualMemoryAddress current_pc = g_state.pc & ICACHE_TAG_ADDRESS_MASK;
  if (IsCachedAddress(current_pc))
  {
    TickCount ticks = 0;
    TickCount cached_ticks_per_line = GetICacheFillTicks(current_pc);
    for (u32 i = 0; i < line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const u32 line = GetICacheLine(current_pc);
      if (g_state.icache_tags[line] != current_pc)
      {
        g_state.icache_tags[line] = current_pc;
        ticks += cached_ticks_per_line;
      }
    }

    g_state.pending_ticks += ticks;
  }
  else
  {
    g_state.pending_ticks += uncached_ticks;
  }
}

u32 CPU::FillICache(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  u8* line_data = &g_state.icache_data[line * ICACHE_LINE_SIZE];
  u32 line_tag;
  switch ((address >> 2) & 0x03u)
  {
    case 0:
      DoInstructionRead<true, true, 4, false>(address & ~(ICACHE_LINE_SIZE - 1u), line_data);
      line_tag = GetICacheTagForAddress(address);
      break;
    case 1:
      DoInstructionRead<true, true, 3, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0x4), line_data + 0x4);
      line_tag = GetICacheTagForAddress(address) | 0x1;
      break;
    case 2:
      DoInstructionRead<true, true, 2, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0x8), line_data + 0x8);
      line_tag = GetICacheTagForAddress(address) | 0x3;
      break;
    case 3:
    default:
      DoInstructionRead<true, true, 1, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0xC), line_data + 0xC);
      line_tag = GetICacheTagForAddress(address) | 0x7;
      break;
  }
  g_state.icache_tags[line] = line_tag;

  const u32 offset = GetICacheLineOffset(address);
  u32 result;
  std::memcpy(&result, &line_data[offset], sizeof(result));
  return result;
}

void CPU::ClearICache()
{
  std::memset(g_state.icache_data.data(), 0, ICACHE_SIZE);
  g_state.icache_tags.fill(ICACHE_INVALID_BITS);
}

namespace CPU {
ALWAYS_INLINE_RELEASE static u32 ReadICache(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  const u8* line_data = &g_state.icache_data[line * ICACHE_LINE_SIZE];
  const u32 offset = GetICacheLineOffset(address);
  u32 result;
  std::memcpy(&result, &line_data[offset], sizeof(result));
  return result;
}
} // namespace CPU

ALWAYS_INLINE_RELEASE bool CPU::FetchInstruction()
{
  DebugAssert(Common::IsAlignedPow2(g_state.npc, 4));

  const PhysicalMemoryAddress address = g_state.npc;
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    {
#if 0
      DoInstructionRead<true, false, 1, false>(address, &g_state.next_instruction.bits);
#else
      if (CompareICacheTag(address))
        g_state.next_instruction.bits = ReadICache(address);
      else
        g_state.next_instruction.bits = FillICache(address);
#endif
    }
    break;

    case 0x05: // KSEG1 - physical memory uncached
    {
      if (!DoInstructionRead<true, false, 1, true>(address, &g_state.next_instruction.bits))
        return false;
    }
    break;

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
    {
      CPU::RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::IBE,
                                                                      g_state.current_instruction_in_branch_delay_slot,
                                                                      g_state.current_instruction_was_branch_taken, 0),
                          address);
      return false;
    }
  }

  g_state.pc = g_state.npc;
  g_state.npc += sizeof(g_state.next_instruction.bits);
  return true;
}

bool CPU::FetchInstructionForInterpreterFallback()
{
  DebugAssert(Common::IsAlignedPow2(g_state.npc, 4));

  const PhysicalMemoryAddress address = g_state.npc;
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    case 0x05: // KSEG1 - physical memory uncached
    {
      // We don't use the icache when doing interpreter fallbacks, because it's probably stale.
      if (!DoInstructionRead<false, false, 1, true>(address, &g_state.next_instruction.bits))
        return false;
    }
    break;

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
    {
      CPU::RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::IBE,
                                                                      g_state.current_instruction_in_branch_delay_slot,
                                                                      g_state.current_instruction_was_branch_taken, 0),
                          address);
      return false;
    }
  }

  g_state.pc = g_state.npc;
  g_state.npc += sizeof(g_state.next_instruction.bits);
  return true;
}

bool CPU::SafeReadInstruction(VirtualMemoryAddress addr, u32* value)
{
  switch (addr >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    case 0x05: // KSEG1 - physical memory uncached
    {
      // TODO: Check icache.
      return DoInstructionRead<false, false, 1, false>(addr, value);
    }

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
    {
      return false;
    }
  }
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE bool CPU::DoSafeMemoryAccess(VirtualMemoryAddress address, u32& value)
{
  using namespace Bus;

  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    {
      if ((address & SCRATCHPAD_ADDR_MASK) == SCRATCHPAD_ADDR)
      {
        const u32 offset = address & SCRATCHPAD_OFFSET_MASK;

        if constexpr (type == MemoryAccessType::Read)
        {
          if constexpr (size == MemoryAccessSize::Byte)
          {
            value = CPU::g_state.scratchpad[offset];
          }
          else if constexpr (size == MemoryAccessSize::HalfWord)
          {
            u16 temp;
            std::memcpy(&temp, &CPU::g_state.scratchpad[offset], sizeof(u16));
            value = ZeroExtend32(temp);
          }
          else if constexpr (size == MemoryAccessSize::Word)
          {
            std::memcpy(&value, &CPU::g_state.scratchpad[offset], sizeof(u32));
          }
        }
        else
        {
          if constexpr (size == MemoryAccessSize::Byte)
          {
            CPU::g_state.scratchpad[offset] = Truncate8(value);
          }
          else if constexpr (size == MemoryAccessSize::HalfWord)
          {
            std::memcpy(&CPU::g_state.scratchpad[offset], &value, sizeof(u16));
          }
          else if constexpr (size == MemoryAccessSize::Word)
          {
            std::memcpy(&CPU::g_state.scratchpad[offset], &value, sizeof(u32));
          }
        }

        return true;
      }

      address &= PHYSICAL_MEMORY_ADDRESS_MASK;
    }
    break;

    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
    case 0x06: // KSEG2
    case 0x07: // KSEG2
    {
      // Above 512mb raises an exception.
      return false;
    }

    case 0x05: // KSEG1 - physical memory uncached
    {
      address &= PHYSICAL_MEMORY_ADDRESS_MASK;
    }
    break;
  }

  if (address < RAM_MIRROR_END)
  {
    const u32 offset = address & g_ram_mask;
    if constexpr (type == MemoryAccessType::Read)
    {
      if constexpr (size == MemoryAccessSize::Byte)
      {
        value = g_unprotected_ram[offset];
      }
      else if constexpr (size == MemoryAccessSize::HalfWord)
      {
        u16 temp;
        std::memcpy(&temp, &g_unprotected_ram[offset], sizeof(temp));
        value = ZeroExtend32(temp);
      }
      else if constexpr (size == MemoryAccessSize::Word)
      {
        std::memcpy(&value, &g_unprotected_ram[offset], sizeof(u32));
      }
    }
    else
    {
      const u32 page_index = offset / HOST_PAGE_SIZE;

      if constexpr (size == MemoryAccessSize::Byte)
      {
        if (g_unprotected_ram[offset] != Truncate8(value))
        {
          g_unprotected_ram[offset] = Truncate8(value);
          if (g_ram_code_bits[page_index])
            CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);
        }
      }
      else if constexpr (size == MemoryAccessSize::HalfWord)
      {
        const u16 new_value = Truncate16(value);
        u16 old_value;
        std::memcpy(&old_value, &g_unprotected_ram[offset], sizeof(old_value));
        if (old_value != new_value)
        {
          std::memcpy(&g_unprotected_ram[offset], &new_value, sizeof(u16));
          if (g_ram_code_bits[page_index])
            CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);
        }
      }
      else if constexpr (size == MemoryAccessSize::Word)
      {
        u32 old_value;
        std::memcpy(&old_value, &g_unprotected_ram[offset], sizeof(u32));
        if (old_value != value)
        {
          std::memcpy(&g_unprotected_ram[offset], &value, sizeof(u32));
          if (g_ram_code_bits[page_index])
            CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);
        }
      }
    }

    return true;
  }
  if constexpr (type == MemoryAccessType::Read)
  {
    if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_SIZE))
    {
      const u32 offset = (address & BIOS_MASK);
      if constexpr (size == MemoryAccessSize::Byte)
      {
        value = ZeroExtend32(g_bios[offset]);
      }
      else if constexpr (size == MemoryAccessSize::HalfWord)
      {
        u16 halfword;
        std::memcpy(&halfword, &g_bios[offset], sizeof(u16));
        value = ZeroExtend32(halfword);
      }
      else
      {
        std::memcpy(&value, &g_bios[offset], sizeof(u32));
      }

      return true;
    }
  }
  return false;
}

bool CPU::SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  if (!DoSafeMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte>(addr, temp))
    return false;

  *value = Truncate8(temp);
  return true;
}

bool CPU::SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  if ((addr & 1) == 0)
  {
    u32 temp = 0;
    if (!DoSafeMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr, temp))
      return false;

    *value = Truncate16(temp);
    return true;
  }

  u8 low, high;
  if (!SafeReadMemoryByte(addr, &low) || !SafeReadMemoryByte(addr + 1, &high))
    return false;

  *value = (ZeroExtend16(high) << 8) | ZeroExtend16(low);
  return true;
}

bool CPU::SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  if ((addr & 3) == 0)
    return DoSafeMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(addr, *value);

  u16 low, high;
  if (!SafeReadMemoryHalfWord(addr, &low) || !SafeReadMemoryHalfWord(addr + 2, &high))
    return false;

  *value = (ZeroExtend32(high) << 16) | ZeroExtend32(low);
  return true;
}

bool CPU::SafeReadMemoryCString(VirtualMemoryAddress addr, std::string* value, u32 max_length /*= 1024*/)
{
  value->clear();

  u8 ch;
  while (SafeReadMemoryByte(addr, &ch))
  {
    if (ch == 0)
      return true;

    value->push_back(ch);
    if (value->size() >= max_length)
      return true;

    addr++;
  }

  value->clear();
  return false;
}

bool CPU::SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DoSafeMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte>(addr, temp);
}

bool CPU::SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  if ((addr & 1) == 0)
  {
    u32 temp = ZeroExtend32(value);
    return DoSafeMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr, temp);
  }

  return SafeWriteMemoryByte(addr, Truncate8(value)) && SafeWriteMemoryByte(addr + 1, Truncate8(value >> 8));
}

bool CPU::SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  if ((addr & 3) == 0)
    return DoSafeMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word>(addr, value);

  return SafeWriteMemoryHalfWord(addr, Truncate16(value)) && SafeWriteMemoryHalfWord(addr + 2, Truncate16(value >> 16));
}

void* CPU::GetDirectReadMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size, TickCount* read_ticks)
{
  using namespace Bus;

  const u32 seg = (address >> 29);
  if (seg != 0 && seg != 4 && seg != 5)
    return nullptr;

  const PhysicalMemoryAddress paddr = address & PHYSICAL_MEMORY_ADDRESS_MASK;
  if (paddr < RAM_MIRROR_END)
  {
    if (read_ticks)
      *read_ticks = RAM_READ_TICKS;

    return &g_ram[paddr & g_ram_mask];
  }

  if ((paddr & SCRATCHPAD_ADDR_MASK) == SCRATCHPAD_ADDR)
  {
    if (read_ticks)
      *read_ticks = 0;

    return &g_state.scratchpad[paddr & SCRATCHPAD_OFFSET_MASK];
  }

  if (paddr >= BIOS_BASE && paddr < (BIOS_BASE + BIOS_SIZE))
  {
    if (read_ticks)
      *read_ticks = g_bios_access_time[static_cast<u32>(size)];

    return &g_bios[paddr & BIOS_MASK];
  }

  return nullptr;
}

void* CPU::GetDirectWriteMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size)
{
  using namespace Bus;

  const u32 seg = (address >> 29);
  if (seg != 0 && seg != 4 && seg != 5)
    return nullptr;

  const PhysicalMemoryAddress paddr = address & PHYSICAL_MEMORY_ADDRESS_MASK;

  if (paddr < RAM_MIRROR_END)
    return &g_ram[paddr & g_ram_mask];

  if ((paddr & SCRATCHPAD_ADDR_MASK) == SCRATCHPAD_ADDR)
    return &g_state.scratchpad[paddr & SCRATCHPAD_OFFSET_MASK];

  return nullptr;
}

template<MemoryAccessType type, MemoryAccessSize size>
ALWAYS_INLINE_RELEASE bool CPU::DoAlignmentCheck(VirtualMemoryAddress address)
{
  if constexpr (size == MemoryAccessSize::HalfWord)
  {
    if (Common::IsAlignedPow2(address, 2))
      return true;
  }
  else if constexpr (size == MemoryAccessSize::Word)
  {
    if (Common::IsAlignedPow2(address, 4))
      return true;
  }
  else
  {
    return true;
  }

  g_state.cop0_regs.BadVaddr = address;
  RaiseException(type == MemoryAccessType::Read ? Exception::AdEL : Exception::AdES);
  return false;
}

#if 0
static void MemoryBreakpoint(MemoryAccessType type, MemoryAccessSize size, VirtualMemoryAddress addr, u32 value)
{
  static constexpr const char* sizes[3] = { "byte", "halfword", "word" };
  static constexpr const char* types[2] = { "read", "write" };

  const u32 cycle = TimingEvents::GetGlobalTickCounter() + CPU::g_state.pending_ticks;
  if (cycle == 3301006373)
    __debugbreak();

#if 0
  static std::FILE* fp = nullptr;
  if (!fp)
    fp = std::fopen("D:\\memory.txt", "wb");
  if (fp)
  {
    std::fprintf(fp, "%u %s %s %08X %08X\n", cycle, types[static_cast<u32>(type)], sizes[static_cast<u32>(size)], addr, value);
    std::fflush(fp);
  }
#endif

#if 0
  if (type == MemoryAccessType::Read && addr == 0x1F000084)
    __debugbreak();
#endif
#if 0
  if (type == MemoryAccessType::Write && addr == 0x000000B0 /*&& value == 0x3C080000*/)
    __debugbreak();
#endif

#if 0 // TODO: MEMBP
  if (type == MemoryAccessType::Write && address == 0x80113028)
  {
    if ((TimingEvents::GetGlobalTickCounter() + CPU::g_state.pending_ticks) == 5051485)
      __debugbreak();

    Log_WarningPrintf("VAL %08X @ %u", value, (TimingEvents::GetGlobalTickCounter() + CPU::g_state.pending_ticks));
  }
#endif
}
#define MEMORY_BREAKPOINT(type, size, addr, value) MemoryBreakpoint((type), (size), (addr), (value))
#else
#define MEMORY_BREAKPOINT(type, size, addr, value)
#endif

bool CPU::ReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  *value = Truncate8(GetMemoryReadHandler(addr, MemoryAccessSize::Byte)(addr));
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    RaiseException(Exception::DBE);
    return false;
  }

  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Byte, addr, *value);
  return true;
}

bool CPU::ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr))
    return false;

  *value = Truncate16(GetMemoryReadHandler(addr, MemoryAccessSize::HalfWord)(addr));
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    RaiseException(Exception::DBE);
    return false;
  }

  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::HalfWord, addr, *value);
  return true;
}

bool CPU::ReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(addr))
    return false;

  *value = GetMemoryReadHandler(addr, MemoryAccessSize::Word)(addr);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    RaiseException(Exception::DBE);
    return false;
  }

  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Word, addr, *value);
  return true;
}

bool CPU::WriteMemoryByte(VirtualMemoryAddress addr, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Byte, addr, value);

  GetMemoryWriteHandler(addr, MemoryAccessSize::Byte)(addr, value);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    RaiseException(Exception::DBE);
    return false;
  }

  return true;
}

bool CPU::WriteMemoryHalfWord(VirtualMemoryAddress addr, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::HalfWord, addr, value);

  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr))
    return false;

  GetMemoryWriteHandler(addr, MemoryAccessSize::HalfWord)(addr, value);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    RaiseException(Exception::DBE);
    return false;
  }

  return true;
}

bool CPU::WriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Word, addr, value);

  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::Word>(addr))
    return false;

  GetMemoryWriteHandler(addr, MemoryAccessSize::Word)(addr, value);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    RaiseException(Exception::DBE);
    return false;
  }

  return true;
}

u64 CPU::Recompiler::Thunks::ReadMemoryByte(u32 address)
{
  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::Byte)(address);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    return static_cast<u64>(-static_cast<s64>(Exception::DBE));
  }

  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Byte, address, value);
  return ZeroExtend64(value);
}

u64 CPU::Recompiler::Thunks::ReadMemoryHalfWord(u32 address)
{
  if (!Common::IsAlignedPow2(address, 2)) [[unlikely]]
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u64>(-static_cast<s64>(Exception::AdEL));
  }

  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::HalfWord)(address);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    return static_cast<u64>(-static_cast<s64>(Exception::DBE));
  }

  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::HalfWord, address, value);
  return ZeroExtend64(value);
}

u64 CPU::Recompiler::Thunks::ReadMemoryWord(u32 address)
{
  if (!Common::IsAlignedPow2(address, 4)) [[unlikely]]
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u64>(-static_cast<s64>(Exception::AdEL));
  }

  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::Word)(address);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    return static_cast<u64>(-static_cast<s64>(Exception::DBE));
  }

  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Word, address, value);
  return ZeroExtend64(value);
}

u32 CPU::Recompiler::Thunks::WriteMemoryByte(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Byte, address, value);

  GetMemoryWriteHandler(address, MemoryAccessSize::Byte)(address, value);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    return static_cast<u32>(Exception::DBE);
  }

  return 0;
}

u32 CPU::Recompiler::Thunks::WriteMemoryHalfWord(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::HalfWord, address, value);

  if (!Common::IsAlignedPow2(address, 2)) [[unlikely]]
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u32>(Exception::AdES);
  }

  GetMemoryWriteHandler(address, MemoryAccessSize::HalfWord)(address, value);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    return static_cast<u32>(Exception::DBE);
  }

  return 0;
}

u32 CPU::Recompiler::Thunks::WriteMemoryWord(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Word, address, value);

  if (!Common::IsAlignedPow2(address, 4)) [[unlikely]]
  {
    g_state.cop0_regs.BadVaddr = address;
    return static_cast<u32>(Exception::AdES);
  }

  GetMemoryWriteHandler(address, MemoryAccessSize::Word)(address, value);
  if (g_state.bus_error) [[unlikely]]
  {
    g_state.bus_error = false;
    return static_cast<u32>(Exception::DBE);
  }

  return 0;
}

u32 CPU::Recompiler::Thunks::UncheckedReadMemoryByte(u32 address)
{
  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::Byte)(address);
  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Byte, address, value);
  return value;
}

u32 CPU::Recompiler::Thunks::UncheckedReadMemoryHalfWord(u32 address)
{
  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::HalfWord)(address);
  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::HalfWord, address, value);
  return value;
}

u32 CPU::Recompiler::Thunks::UncheckedReadMemoryWord(u32 address)
{
  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::Word)(address);
  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Word, address, value);
  return value;
}

void CPU::Recompiler::Thunks::UncheckedWriteMemoryByte(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Byte, address, value);
  GetMemoryWriteHandler(address, MemoryAccessSize::Byte)(address, value);
}

void CPU::Recompiler::Thunks::UncheckedWriteMemoryHalfWord(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::HalfWord, address, value);
  GetMemoryWriteHandler(address, MemoryAccessSize::HalfWord)(address, value);
}

void CPU::Recompiler::Thunks::UncheckedWriteMemoryWord(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Word, address, value);
  GetMemoryWriteHandler(address, MemoryAccessSize::Word)(address, value);
}

#undef MEMORY_BREAKPOINT
