// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cpu_core.h"
#include "bus.h"
#include "common/align.h"
#include "common/fastjmp.h"
#include "common/file_system.h"
#include "common/log.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "cpu_recompiler_thunks.h"
#include "gte.h"
#include "host.h"
#include "pcdrv.h"
#include "pgxp.h"
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
static void FlushPipeline();

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

bool IsTraceEnabled()
{
  return s_trace_to_log;
}

void StartTrace()
{
  if (s_trace_to_log)
    return;

  s_trace_to_log = true;
  UpdateDebugDispatcherFlag();
}

void StopTrace()
{
  if (!s_trace_to_log)
    return;

  if (s_log_file)
    std::fclose(s_log_file);

  s_log_file_opened = false;
  s_trace_to_log = false;
  UpdateDebugDispatcherFlag();
}

void WriteToExecutionLog(const char* format, ...)
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

void Initialize()
{
  // From nocash spec.
  g_state.cop0_regs.PRID = UINT32_C(0x00000002);

  g_state.use_debug_dispatcher = false;
  s_breakpoints.clear();
  s_breakpoint_counter = 1;
  s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;
  s_single_step = false;

  UpdateFastmemBase();

  GTE::Initialize();
}

void Shutdown()
{
  ClearBreakpoints();
  StopTrace();
}

void Reset()
{
  g_state.pending_ticks = 0;
  g_state.downcount = 0;

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
  UpdateFastmemBase();

  GTE::Reset();

  // TODO: This consumes cycles...
  SetPC(RESET_VECTOR);
}

bool DoState(StateWrapper& sw)
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
    g_state.load_delay_reg = static_cast<Reg>(std::min(static_cast<u8>(g_state.load_delay_reg), static_cast<u8>(Reg::count)));
    g_state.next_load_delay_reg = static_cast<Reg>(std::min(static_cast<u8>(g_state.load_delay_reg), static_cast<u8>(Reg::count)));
  }

  sw.Do(&g_state.cache_control.bits);
  sw.DoBytes(g_state.dcache.data(), g_state.dcache.size());

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
    UpdateFastmemBase();
    g_state.gte_completion_tick = 0;
  }

  return !sw.HasError();
}

void UpdateFastmemBase()
{
  if (g_state.cop0_regs.sr.Isc)
    g_state.fastmem_base = nullptr;
  else
    g_state.fastmem_base = Bus::GetFastmemBase();
}

ALWAYS_INLINE_RELEASE void SetPC(u32 new_pc)
{
  DebugAssert(Common::IsAlignedPow2(new_pc, 4));
  g_state.npc = new_pc;
  FlushPipeline();
}

ALWAYS_INLINE_RELEASE void Branch(u32 target)
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

ALWAYS_INLINE static u32 GetExceptionVector(bool debug_exception = false)
{
  const u32 base = g_state.cop0_regs.sr.BEV ? UINT32_C(0xbfc00100) : UINT32_C(0x80000000);
  return base | (debug_exception ? UINT32_C(0x00000040) : UINT32_C(0x00000080));
}

ALWAYS_INLINE_RELEASE static void RaiseException(u32 CAUSE_bits, u32 EPC, u32 vector)
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
    DisassembleAndPrint(g_state.current_instruction_pc, 4, 0);
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

ALWAYS_INLINE_RELEASE static void DispatchCop0Breakpoint()
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

void RaiseException(u32 CAUSE_bits, u32 EPC)
{
  RaiseException(CAUSE_bits, EPC, GetExceptionVector());
}

void RaiseException(Exception excode)
{
  RaiseException(Cop0Registers::CAUSE::MakeValueForException(excode, g_state.current_instruction_in_branch_delay_slot,
                                                             g_state.current_instruction_was_branch_taken,
                                                             g_state.current_instruction.cop.cop_n),
                 g_state.current_instruction_pc, GetExceptionVector());
}

void RaiseBreakException(u32 CAUSE_bits, u32 EPC, u32 instruction_bits)
{
  if (PCDrv::HandleSyscall(instruction_bits, g_state.regs))
  {
    // immediately return
    g_state.npc = EPC + 4;
    FlushPipeline();
    return;
  }

  // normal exception
  RaiseException(CAUSE_bits, EPC, GetExceptionVector());
}

void SetExternalInterrupt(u8 bit)
{
  g_state.cop0_regs.cause.Ip |= static_cast<u8>(1u << bit);
  CheckForPendingInterrupt();
}

void ClearExternalInterrupt(u8 bit)
{
  g_state.cop0_regs.cause.Ip &= static_cast<u8>(~(1u << bit));
}

ALWAYS_INLINE_RELEASE static void UpdateLoadDelay()
{
  // the old value is needed in case the delay slot instruction overwrites the same register
  g_state.regs.r[static_cast<u8>(g_state.load_delay_reg)] = g_state.load_delay_value;
  g_state.load_delay_reg = g_state.next_load_delay_reg;
  g_state.load_delay_value = g_state.next_load_delay_value;
  g_state.next_load_delay_reg = Reg::count;
}

ALWAYS_INLINE_RELEASE static void FlushPipeline()
{
  // loads are flushed
  g_state.next_load_delay_reg = Reg::count;
  g_state.regs.r[static_cast<u8>(g_state.load_delay_reg)] = g_state.load_delay_value;
  g_state.load_delay_reg = Reg::count;

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

ALWAYS_INLINE static u32 ReadReg(Reg rs)
{
  return g_state.regs.r[static_cast<u8>(rs)];
}

ALWAYS_INLINE static void WriteReg(Reg rd, u32 value)
{
  g_state.regs.r[static_cast<u8>(rd)] = value;
  g_state.load_delay_reg = (rd == g_state.load_delay_reg) ? Reg::count : g_state.load_delay_reg;

  // prevent writes to $zero from going through - better than branching/cmov
  g_state.regs.zero = 0;
}

ALWAYS_INLINE_RELEASE static void WriteRegDelayed(Reg rd, u32 value)
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

ALWAYS_INLINE_RELEASE static u32 ReadCop0Reg(Cop0Reg reg)
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

ALWAYS_INLINE_RELEASE static void WriteCop0Reg(Cop0Reg reg, u32 value)
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
      UpdateDebugDispatcherFlag();
    }
    break;

    case Cop0Reg::SR:
    {
      g_state.cop0_regs.sr.bits =
        (g_state.cop0_regs.sr.bits & ~Cop0Registers::SR::WRITE_MASK) | (value & Cop0Registers::SR::WRITE_MASK);
      Log_DebugPrintf("COP0 SR <- %08X (now %08X)", value, g_state.cop0_regs.sr.bits);
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

ALWAYS_INLINE_RELEASE void Cop0ExecutionBreakpointCheck()
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
ALWAYS_INLINE_RELEASE void Cop0DataBreakpointCheck(VirtualMemoryAddress address)
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

static void TracePrintInstruction()
{
  const u32 pc = g_state.current_instruction_pc;
  const u32 bits = g_state.current_instruction.bits;

  TinyString instr;
  TinyString comment;
  DisassembleInstruction(&instr, pc, bits);
  DisassembleInstructionComment(&comment, pc, bits, &g_state.regs);
  if (!comment.IsEmpty())
  {
    for (u32 i = instr.GetLength(); i < 30; i++)
      instr.AppendCharacter(' ');
    instr.AppendString("; ");
    instr.AppendString(comment);
  }

  std::printf("%08x: %08x %s\n", pc, bits, instr.GetCharArray());
}

#endif

static void PrintInstruction(u32 bits, u32 pc, Registers* regs, const char* prefix)
{
  TinyString instr;
  TinyString comment;
  DisassembleInstruction(&instr, pc, bits);
  DisassembleInstructionComment(&comment, pc, bits, regs);
  if (!comment.IsEmpty())
  {
    for (u32 i = instr.GetLength(); i < 30; i++)
      instr.AppendCharacter(' ');
    instr.AppendString("; ");
    instr.AppendString(comment);
  }

  Log_DevPrintf("%s%08x: %08x %s", prefix, pc, bits, instr.GetCharArray());
}

static void LogInstruction(u32 bits, u32 pc, Registers* regs)
{
  TinyString instr;
  TinyString comment;
  DisassembleInstruction(&instr, pc, bits);
  DisassembleInstructionComment(&comment, pc, bits, regs);
  if (!comment.IsEmpty())
  {
    for (u32 i = instr.GetLength(); i < 30; i++)
      instr.AppendCharacter(' ');
    instr.AppendString("; ");
    instr.AppendString(comment);
  }

  WriteToExecutionLog("%08x: %08x %s\n", pc, bits, instr.GetCharArray());
}

const std::array<DebuggerRegisterListEntry, NUM_DEBUGGER_REGISTER_LIST_ENTRIES> g_debugger_register_list = {
  {{"zero", &CPU::g_state.regs.zero},
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

void DisassembleAndPrint(u32 addr, const char* prefix)
{
  u32 bits = 0;
  SafeReadMemoryWord(addr, &bits);
  PrintInstruction(bits, addr, &g_state.regs, prefix);
}

void DisassembleAndPrint(u32 addr, u32 instructions_before /* = 0 */, u32 instructions_after /* = 0 */)
{
  u32 disasm_addr = addr - (instructions_before * sizeof(u32));
  for (u32 i = 0; i < instructions_before; i++)
  {
    DisassembleAndPrint(disasm_addr, "");
    disasm_addr += sizeof(u32);
  }

  // <= to include the instruction itself
  for (u32 i = 0; i <= instructions_after; i++)
  {
    DisassembleAndPrint(disasm_addr, (i == 0) ? "---->" : "");
    disasm_addr += sizeof(u32);
  }
}

template<PGXPMode pgxp_mode, bool debug>
ALWAYS_INLINE_RELEASE static void ExecuteInstruction()
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
          const u32 new_value = ReadReg(inst.r.rt) << inst.r.shamt;
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLL(inst.bits, ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srl:
        {
          const u32 new_value = ReadReg(inst.r.rt) >> inst.r.shamt;
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRL(inst.bits, ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sra:
        {
          const u32 new_value = static_cast<u32>(static_cast<s32>(ReadReg(inst.r.rt)) >> inst.r.shamt);
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRA(inst.bits, ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sllv:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = ReadReg(inst.r.rt) << shift_amount;
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLLV(inst.bits, ReadReg(inst.r.rt), shift_amount);

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srlv:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = ReadReg(inst.r.rt) >> shift_amount;
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRLV(inst.bits, ReadReg(inst.r.rt), shift_amount);

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srav:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = static_cast<u32>(static_cast<s32>(ReadReg(inst.r.rt)) >> shift_amount);
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRAV(inst.bits, ReadReg(inst.r.rt), shift_amount);

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::and_:
        {
          const u32 new_value = ReadReg(inst.r.rs) & ReadReg(inst.r.rt);
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_AND_(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::or_:
        {
          const u32 new_value = ReadReg(inst.r.rs) | ReadReg(inst.r.rt);
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_OR_(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::xor_:
        {
          const u32 new_value = ReadReg(inst.r.rs) ^ ReadReg(inst.r.rt);
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_XOR_(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::nor:
        {
          const u32 new_value = ~(ReadReg(inst.r.rs) | ReadReg(inst.r.rt));
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_NOR(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::add:
        {
          const u32 old_value = ReadReg(inst.r.rs);
          const u32 add_value = ReadReg(inst.r.rt);
          const u32 new_value = old_value + add_value;
          if (AddOverflow(old_value, add_value, new_value))
          {
            RaiseException(Exception::Ov);
            return;
          }

          if constexpr (pgxp_mode == PGXPMode::CPU)
            PGXP::CPU_ADD(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));
          else if constexpr (pgxp_mode >= PGXPMode::Memory)
          {
            if (add_value == 0)
            {
              PGXP::CPU_MOVE((static_cast<u32>(inst.r.rd.GetValue()) << 8) | static_cast<u32>(inst.r.rs.GetValue()),
                             old_value);
            }
          }

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::addu:
        {
          const u32 old_value = ReadReg(inst.r.rs);
          const u32 add_value = ReadReg(inst.r.rt);
          const u32 new_value = old_value + add_value;
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_ADD(inst.bits, old_value, add_value);
          else if constexpr (pgxp_mode >= PGXPMode::Memory)
          {
            if (add_value == 0)
            {
              PGXP::CPU_MOVE((static_cast<u32>(inst.r.rd.GetValue()) << 8) | static_cast<u32>(inst.r.rs.GetValue()),
                             old_value);
            }
          }

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sub:
        {
          const u32 old_value = ReadReg(inst.r.rs);
          const u32 sub_value = ReadReg(inst.r.rt);
          const u32 new_value = old_value - sub_value;
          if (SubOverflow(old_value, sub_value, new_value))
          {
            RaiseException(Exception::Ov);
            return;
          }

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SUB(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::subu:
        {
          const u32 new_value = ReadReg(inst.r.rs) - ReadReg(inst.r.rt);
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SUB(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::slt:
        {
          const u32 result = BoolToUInt32(static_cast<s32>(ReadReg(inst.r.rs)) < static_cast<s32>(ReadReg(inst.r.rt)));
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLT(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, result);
        }
        break;

        case InstructionFunct::sltu:
        {
          const u32 result = BoolToUInt32(ReadReg(inst.r.rs) < ReadReg(inst.r.rt));
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLTU(inst.bits, ReadReg(inst.r.rs), ReadReg(inst.r.rt));

          WriteReg(inst.r.rd, result);
        }
        break;

        case InstructionFunct::mfhi:
        {
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MFHI(inst.bits, g_state.regs.hi);

          WriteReg(inst.r.rd, g_state.regs.hi);
        }
        break;

        case InstructionFunct::mthi:
        {
          const u32 value = ReadReg(inst.r.rs);
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MTHI(inst.bits, value);

          g_state.regs.hi = value;
        }
        break;

        case InstructionFunct::mflo:
        {
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MFLO(inst.bits, g_state.regs.lo);

          WriteReg(inst.r.rd, g_state.regs.lo);
        }
        break;

        case InstructionFunct::mtlo:
        {
          const u32 value = ReadReg(inst.r.rs);
          if constexpr (pgxp_mode == PGXPMode::CPU)
            PGXP::CPU_MTLO(inst.bits, value);

          g_state.regs.lo = value;
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

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MULTU(inst.bits, lhs, rhs);

          g_state.regs.hi = Truncate32(result >> 32);
          g_state.regs.lo = Truncate32(result);
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
      const u32 new_value = ReadReg(inst.i.rs) & inst.i.imm_zext32();

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ANDI(inst.bits, ReadReg(inst.i.rs));

      WriteReg(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::ori:
    {
      const u32 new_value = ReadReg(inst.i.rs) | inst.i.imm_zext32();

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ORI(inst.bits, ReadReg(inst.i.rs));

      WriteReg(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::xori:
    {
      const u32 new_value = ReadReg(inst.i.rs) ^ inst.i.imm_zext32();

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_XORI(inst.bits, ReadReg(inst.i.rs));

      WriteReg(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::addi:
    {
      const u32 old_value = ReadReg(inst.i.rs);
      const u32 add_value = inst.i.imm_sext32();
      const u32 new_value = old_value + add_value;
      if (AddOverflow(old_value, add_value, new_value))
      {
        RaiseException(Exception::Ov);
        return;
      }

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ADDI(inst.bits, ReadReg(inst.i.rs));
      else if constexpr (pgxp_mode >= PGXPMode::Memory)
      {
        if (add_value == 0)
        {
          PGXP::CPU_MOVE((static_cast<u32>(inst.i.rt.GetValue()) << 8) | static_cast<u32>(inst.i.rs.GetValue()),
                         old_value);
        }
      }

      WriteReg(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::addiu:
    {
      const u32 old_value = ReadReg(inst.i.rs);
      const u32 add_value = inst.i.imm_sext32();
      const u32 new_value = old_value + add_value;

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ADDI(inst.bits, ReadReg(inst.i.rs));
      else if constexpr (pgxp_mode >= PGXPMode::Memory)
      {
        if (add_value == 0)
        {
          PGXP::CPU_MOVE((static_cast<u32>(inst.i.rt.GetValue()) << 8) | static_cast<u32>(inst.i.rs.GetValue()),
                         old_value);
        }
      }

      WriteReg(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::slti:
    {
      const u32 result = BoolToUInt32(static_cast<s32>(ReadReg(inst.i.rs)) < static_cast<s32>(inst.i.imm_sext32()));

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_SLTI(inst.bits, ReadReg(inst.i.rs));

      WriteReg(inst.i.rt, result);
    }
    break;

    case InstructionOp::sltiu:
    {
      const u32 result = BoolToUInt32(ReadReg(inst.i.rs) < inst.i.imm_sext32());

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_SLTIU(inst.bits, ReadReg(inst.i.rs));

      WriteReg(inst.i.rt, result);
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
        PGXP::CPU_LHx(inst.bits, addr, sxvalue);
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
        PGXP::CPU_LHx(inst.bits, addr, zxvalue);
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

            if constexpr (pgxp_mode == PGXPMode::CPU)
              PGXP::CPU_MFC0(inst.bits, value);

            WriteRegDelayed(inst.r.rt, value);
          }
          break;

          case CopCommonInstruction::mtcn:
          {
            WriteCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue()), ReadReg(inst.r.rt));

            if constexpr (pgxp_mode == PGXPMode::CPU)
              PGXP::CPU_MTC0(inst.bits, ReadCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue())), ReadReg(inst.i.rt));
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

void DispatchInterrupt()
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

void UpdateDebugDispatcherFlag()
{
  const bool has_any_breakpoints = !s_breakpoints.empty();

  // TODO: cop0 breakpoints
  const auto& dcic = g_state.cop0_regs.dcic;
  const bool has_cop0_breakpoints =
    dcic.super_master_enable_1 && dcic.super_master_enable_2 && dcic.execution_breakpoint_enable;

  const bool use_debug_dispatcher = has_any_breakpoints || has_cop0_breakpoints || s_trace_to_log;
  if (use_debug_dispatcher == g_state.use_debug_dispatcher)
    return;

  Log_DevPrintf("%s debug dispatcher", use_debug_dispatcher ? "Now using" : "No longer using");
  g_state.use_debug_dispatcher = use_debug_dispatcher;
  ExitExecution();
}

void ExitExecution()
{
  // can't exit while running events without messing things up
  if (TimingEvents::IsRunningEvents())
    TimingEvents::SetFrameDone();
  else
    fastjmp_jmp(&s_jmp_buf, 1);
}

bool HasAnyBreakpoints()
{
  return !s_breakpoints.empty();
}

bool HasBreakpointAtAddress(VirtualMemoryAddress address)
{
  for (const Breakpoint& bp : s_breakpoints)
  {
    if (bp.address == address)
      return true;
  }

  return false;
}

BreakpointList GetBreakpointList(bool include_auto_clear, bool include_callbacks)
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

bool AddBreakpoint(VirtualMemoryAddress address, bool auto_clear, bool enabled)
{
  if (HasBreakpointAtAddress(address))
    return false;

  Log_InfoPrintf("Adding breakpoint at %08X, auto clear = %u", address, static_cast<unsigned>(auto_clear));

  Breakpoint bp{address, nullptr, auto_clear ? 0 : s_breakpoint_counter++, 0, auto_clear, enabled};
  s_breakpoints.push_back(std::move(bp));
  UpdateDebugDispatcherFlag();

  if (!auto_clear)
  {
    Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Added breakpoint at 0x%08X."),
                                         address);
  }

  return true;
}

bool AddBreakpointWithCallback(VirtualMemoryAddress address, BreakpointCallback callback)
{
  if (HasBreakpointAtAddress(address))
    return false;

  Log_InfoPrintf("Adding breakpoint with callback at %08X", address);

  Breakpoint bp{address, callback, 0, 0, false, true};
  s_breakpoints.push_back(std::move(bp));
  UpdateDebugDispatcherFlag();
  return true;
}

bool RemoveBreakpoint(VirtualMemoryAddress address)
{
  auto it = std::find_if(s_breakpoints.begin(), s_breakpoints.end(),
                         [address](const Breakpoint& bp) { return bp.address == address; });
  if (it == s_breakpoints.end())
    return false;

  Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Removed breakpoint at 0x%08X."),
                                       address);

  s_breakpoints.erase(it);
  UpdateDebugDispatcherFlag();

  if (address == s_last_breakpoint_check_pc)
    s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;

  return true;
}

void ClearBreakpoints()
{
  s_breakpoints.clear();
  s_breakpoint_counter = 0;
  s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;
  UpdateDebugDispatcherFlag();
}

bool AddStepOverBreakpoint()
{
  u32 bp_pc = g_state.pc;

  Instruction inst;
  if (!SafeReadInstruction(bp_pc, &inst.bits))
    return false;

  bp_pc += sizeof(Instruction);

  if (!IsCallInstruction(inst))
  {
    Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "0x%08X is not a call instruction."),
                                         g_state.pc);
    return false;
  }

  if (!SafeReadInstruction(bp_pc, &inst.bits))
    return false;

  if (IsBranchInstruction(inst))
  {
    Host::ReportFormattedDebuggerMessage(
      TRANSLATE("DebuggerMessage", "Can't step over double branch at 0x%08X"), g_state.pc);
    return false;
  }

  // skip the delay slot
  bp_pc += sizeof(Instruction);

  Host::ReportFormattedDebuggerMessage(TRANSLATE("DebuggerMessage", "Stepping over to 0x%08X."), bp_pc);

  return AddBreakpoint(bp_pc, true);
}

bool AddStepOutBreakpoint(u32 max_instructions_to_search)
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
        TRANSLATE("DebuggerMessage", "Instruction read failed at %08X while searching for function end."),
        ret_pc);
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

ALWAYS_INLINE_RELEASE static bool BreakpointCheck()
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
    }
  }

  s_last_breakpoint_check_pc = pc;
  return System::IsPaused();
}

template<PGXPMode pgxp_mode, bool debug>
[[noreturn]] static void ExecuteImpl()
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
          LogInstruction(g_state.current_instruction.bits, g_state.current_instruction_pc, &g_state.regs);
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

static void ExecuteDebug()
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

void Execute()
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

void SingleStep()
{
  if (fastjmp_set(&s_jmp_buf) == 0)
    ExecuteDebug();
  Host::ReportFormattedDebuggerMessage("Stepped to 0x%08X.", g_state.pc);
}

namespace CodeCache {

template<PGXPMode pgxp_mode>
void InterpretCachedBlock(const CodeBlock& block)
{
  // set up the state so we've already fetched the instruction
  DebugAssert(g_state.pc == block.GetPC());
  g_state.npc = block.GetPC() + 4;

  for (const CodeBlockInstruction& cbi : block.instructions)
  {
    g_state.pending_ticks++;

    // now executing the instruction we previously fetched
    g_state.current_instruction.bits = cbi.instruction.bits;
    g_state.current_instruction_pc = cbi.pc;
    g_state.current_instruction_in_branch_delay_slot = cbi.is_branch_delay_slot;
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
  }

  // cleanup so the interpreter can kick in if needed
  g_state.next_instruction_is_branch_delay_slot = false;
}

template void InterpretCachedBlock<PGXPMode::Disabled>(const CodeBlock& block);
template void InterpretCachedBlock<PGXPMode::Memory>(const CodeBlock& block);
template void InterpretCachedBlock<PGXPMode::CPU>(const CodeBlock& block);

template<PGXPMode pgxp_mode>
void InterpretUncachedBlock()
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

    in_branch_delay_slot = branch;
  }
}

template void InterpretUncachedBlock<PGXPMode::Disabled>();
template void InterpretUncachedBlock<PGXPMode::Memory>();
template void InterpretUncachedBlock<PGXPMode::CPU>();

} // namespace CodeCache

namespace Recompiler::Thunks {

bool InterpretInstruction()
{
  ExecuteInstruction<PGXPMode::Disabled, false>();
  return g_state.exception_raised;
}

bool InterpretInstructionPGXP()
{
  ExecuteInstruction<PGXPMode::Memory, false>();
  return g_state.exception_raised;
}

} // namespace Recompiler::Thunks

} // namespace CPU
