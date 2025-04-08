// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_core.h"
#include "bus.h"
#include "cpu_code_cache_private.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "cpu_pgxp.h"
#include "gte.h"
#include "host.h"
#include "pcdrv.h"
#include "pio.h"
#include "settings.h"
#include "system.h"
#include "timing_event.h"

#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/fastjmp.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include "fmt/format.h"

#include <cstdio>

LOG_CHANNEL(CPU);

namespace CPU {
enum class ExecutionBreakType
{
  None,
  ExecuteOneInstruction,
  SingleStep,
  Breakpoint,
};

static void UpdateLoadDelay();
static void Branch(u32 target);
static void FlushLoadDelay();
static void FlushPipeline();

static u32 GetExceptionVector(bool debug_exception = false);
static void RaiseException(u32 CAUSE_bits, u32 EPC, u32 vector);

static u32 ReadReg(Reg rs);
static void WriteReg(Reg rd, u32 value);
static void WriteRegDelayed(Reg rd, u32 value);

static void DispatchCop0Breakpoint();
static bool IsCop0ExecutionBreakpointUnmasked();
static void Cop0ExecutionBreakpointCheck();
template<MemoryAccessType type>
static void Cop0DataBreakpointCheck(VirtualMemoryAddress address);

static BreakpointList& GetBreakpointList(BreakpointType type);
static bool CheckBreakpointList(BreakpointType type, VirtualMemoryAddress address);
static void ExecutionBreakpointCheck();
template<MemoryAccessType type>
static void MemoryBreakpointCheck(VirtualMemoryAddress address);

#ifdef _DEBUG
static void TracePrintInstruction();
#endif

static void DisassembleAndPrint(u32 addr, bool regs, const char* prefix);
static void PrintInstruction(u32 bits, u32 pc, bool regs, const char* prefix);
static void LogInstruction(u32 bits, u32 pc, bool regs);

static void HandleWriteSyscall();
static void HandlePutcSyscall();
static void HandlePutsSyscall();

static void CheckForExecutionModeChange();
[[noreturn]] static void ExecuteInterpreter();

template<PGXPMode pgxp_mode, bool debug>
static void ExecuteInstruction();

template<PGXPMode pgxp_mode, bool debug>
[[noreturn]] static void ExecuteImpl();

static bool FetchInstruction();
static bool FetchInstructionForInterpreterFallback();
template<bool add_ticks, bool icache_read = false, u32 word_count = 1, bool raise_exceptions>
static bool DoInstructionRead(PhysicalMemoryAddress address, u32* data);
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

constinit State g_state;
bool TRACE_EXECUTION = false;

static fastjmp_buf s_jmp_buf;

static std::FILE* s_log_file = nullptr;
static bool s_log_file_opened = false;
static bool s_trace_to_log = false;

static constexpr u32 INVALID_BREAKPOINT_PC = UINT32_C(0xFFFFFFFF);
static std::array<std::vector<Breakpoint>, static_cast<u32>(BreakpointType::Count)> s_breakpoints;
static u32 s_breakpoint_counter = 1;
static u32 s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;
static CPUExecutionMode s_current_execution_mode = CPUExecutionMode::Interpreter;
static ExecutionBreakType s_break_type = ExecutionBreakType::None;
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
  if (!s_log_file_opened) [[unlikely]]
  {
    s_log_file = FileSystem::OpenCFile(Path::Combine(EmuFolders::DataRoot, "cpu_log.txt").c_str(), "wb");
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

  s_current_execution_mode = g_settings.cpu_execution_mode;
  g_state.using_debug_dispatcher = false;
  g_state.using_interpreter = (s_current_execution_mode == CPUExecutionMode::Interpreter);
  for (BreakpointList& bps : s_breakpoints)
    bps.clear();
  s_breakpoint_counter = 1;
  s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;
  s_break_type = ExecutionBreakType::None;

  UpdateMemoryPointers();
  UpdateDebugDispatcherFlag();

  GTE::Initialize();
}

void CPU::Shutdown()
{
  ClearBreakpoints();
  StopTrace();
}

void CPU::Reset()
{
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
  g_state.cop0_regs.dcic.bits = 0;
  g_state.cop0_regs.sr.bits = 0;
  g_state.cop0_regs.cause.bits = 0;

  ClearICache();
  UpdateMemoryPointers();
  UpdateDebugDispatcherFlag();

  GTE::Reset();

  if (g_settings.gpu_pgxp_enable)
    PGXP::Reset();

  // This consumes cycles, so do it first.
  SetPC(RESET_VECTOR);

  g_state.downcount = 0;
  g_state.pending_ticks = 0;
  g_state.gte_completion_tick = 0;
  g_state.muldiv_completion_tick = 0;
}

bool CPU::DoState(StateWrapper& sw)
{
  sw.Do(&g_state.pending_ticks);
  sw.Do(&g_state.downcount);
  sw.DoEx(&g_state.gte_completion_tick, 78, static_cast<u32>(0));
  sw.DoEx(&g_state.muldiv_completion_tick, 80, static_cast<u32>(0));
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
  if (sw.GetVersion() < 59) [[unlikely]]
  {
    bool interrupt_delay;
    sw.Do(&interrupt_delay);
  }
  sw.Do(&g_state.load_delay_reg);
  sw.Do(&g_state.load_delay_value);
  sw.Do(&g_state.next_load_delay_reg);
  sw.Do(&g_state.next_load_delay_value);

  // Compatibility with old states.
  if (sw.GetVersion() < 59) [[unlikely]]
  {
    g_state.load_delay_reg =
      static_cast<Reg>(std::min(static_cast<u8>(g_state.load_delay_reg), static_cast<u8>(Reg::count)));
    g_state.next_load_delay_reg =
      static_cast<Reg>(std::min(static_cast<u8>(g_state.load_delay_reg), static_cast<u8>(Reg::count)));
  }

  sw.Do(&g_state.cache_control.bits);
  sw.DoBytes(g_state.scratchpad.data(), g_state.scratchpad.size());

  if (!GTE::DoState(sw)) [[unlikely]]
    return false;

  if (sw.GetVersion() < 48) [[unlikely]]
  {
    DebugAssert(sw.IsReading());
    ClearICache();
  }
  else
  {
    sw.Do(&g_state.icache_tags);
    sw.Do(&g_state.icache_data);
  }

  sw.DoEx(&g_state.using_interpreter, 67, g_state.using_interpreter);

  if (sw.IsReading())
  {
    // Trigger an execution mode change if the state was/wasn't using the interpreter.
    s_current_execution_mode =
      g_state.using_interpreter ?
        CPUExecutionMode::Interpreter :
        ((g_settings.cpu_execution_mode == CPUExecutionMode::Interpreter) ? CPUExecutionMode::CachedInterpreter :
                                                                            g_settings.cpu_execution_mode);
    g_state.gte_completion_tick = 0;
    g_state.muldiv_completion_tick = 0;
    UpdateMemoryPointers();
    UpdateDebugDispatcherFlag();
  }

  return !sw.HasError();
}

void CPU::SetPC(u32 new_pc)
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

#if defined(_DEBUG) || defined(_DEVEL)
  if (g_state.cop0_regs.cause.Excode != Exception::INT && g_state.cop0_regs.cause.Excode != Exception::Syscall &&
      g_state.cop0_regs.cause.Excode != Exception::BP)
  {
    DEV_LOG("Exception {} at 0x{:08X} (epc=0x{:08X}, BD={}, CE={})",
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

void CPU::SetIRQRequest(bool state)
{
  // Only uses bit 10.
  constexpr u32 bit = (1u << 10);
  const u32 old_cause = g_state.cop0_regs.cause.bits;
  g_state.cop0_regs.cause.bits = (g_state.cop0_regs.cause.bits & ~bit) | (state ? bit : 0u);
  if (old_cause ^ g_state.cop0_regs.cause.bits && state)
    CheckForPendingInterrupt();
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

  DEV_LOG("Cop0 execution breakpoint at {:08X}", pc);
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

  DEV_LOG("Cop0 data breakpoint for {:08X} at {:08X}", address, g_state.current_instruction_pc);

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

  DEV_LOG("{}{:08x}: {:08x} {}", prefix, pc, bits, instr);
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

  u32 addr = regs.a0;
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

ALWAYS_INLINE static constexpr bool AddOverflow(u32 old_value, u32 add_value, u32* new_value)
{
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_add_overflow(static_cast<s32>(old_value), static_cast<s32>(add_value),
                                reinterpret_cast<s32*>(new_value));
#else
  *new_value = old_value + add_value;
  return (((*new_value ^ old_value) & (*new_value ^ add_value)) & UINT32_C(0x80000000)) != 0;
#endif
}

ALWAYS_INLINE static constexpr bool SubOverflow(u32 old_value, u32 sub_value, u32* new_value)
{
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_sub_overflow(static_cast<s32>(old_value), static_cast<s32>(sub_value),
                                reinterpret_cast<s32*>(new_value));
#else
  *new_value = old_value - sub_value;
  return (((*new_value ^ old_value) & (old_value ^ sub_value)) & UINT32_C(0x80000000)) != 0;
#endif
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
            PGXP::CPU_SLL(inst, rtVal);
        }
        break;

        case InstructionFunct::srl:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rtVal >> inst.r.shamt;
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRL(inst, rtVal);
        }
        break;

        case InstructionFunct::sra:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> inst.r.shamt);
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRA(inst, rtVal);
        }
        break;

        case InstructionFunct::sllv:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 shamt = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 rdVal = rtVal << shamt;
          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLLV(inst, rtVal, shamt);

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
            PGXP::CPU_SRLV(inst, rtVal, shamt);
        }
        break;

        case InstructionFunct::srav:
        {
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 shamt = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> shamt);
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SRAV(inst, rtVal, shamt);
        }
        break;

        case InstructionFunct::and_:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 new_value = rsVal & rtVal;
          WriteReg(inst.r.rd, new_value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_AND_(inst, rsVal, rtVal);
        }
        break;

        case InstructionFunct::or_:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 new_value = rsVal | rtVal;
          WriteReg(inst.r.rd, new_value);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_OR_(inst, rsVal, rtVal);
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
            PGXP::CPU_XOR_(inst, rsVal, rtVal);
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
            PGXP::CPU_NOR(inst, rsVal, rtVal);
        }
        break;

        case InstructionFunct::add:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          u32 rdVal;
          if (AddOverflow(rsVal, rtVal, &rdVal)) [[unlikely]]
          {
            RaiseException(Exception::Ov);
            return;
          }

          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode == PGXPMode::CPU)
            PGXP::CPU_ADD(inst, rsVal, rtVal);
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
            PGXP::CPU_ADD(inst, rsVal, rtVal);
          else if constexpr (pgxp_mode >= PGXPMode::Memory)
            PGXP::TryMove(inst.r.rd, inst.r.rs, inst.r.rt);
        }
        break;

        case InstructionFunct::sub:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          u32 rdVal;
          if (SubOverflow(rsVal, rtVal, &rdVal)) [[unlikely]]
          {
            RaiseException(Exception::Ov);
            return;
          }

          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SUB(inst, rsVal, rtVal);
        }
        break;

        case InstructionFunct::subu:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 rdVal = rsVal - rtVal;
          WriteReg(inst.r.rd, rdVal);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SUB(inst, rsVal, rtVal);
        }
        break;

        case InstructionFunct::slt:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 result = BoolToUInt32(static_cast<s32>(rsVal) < static_cast<s32>(rtVal));
          WriteReg(inst.r.rd, result);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLT(inst, rsVal, rtVal);
        }
        break;

        case InstructionFunct::sltu:
        {
          const u32 rsVal = ReadReg(inst.r.rs);
          const u32 rtVal = ReadReg(inst.r.rt);
          const u32 result = BoolToUInt32(rsVal < rtVal);
          WriteReg(inst.r.rd, result);

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_SLTU(inst, rsVal, rtVal);
        }
        break;

        case InstructionFunct::mfhi:
        {
          const u32 value = g_state.regs.hi;
          WriteReg(inst.r.rd, value);

          StallUntilMulDivComplete();

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MOVE(static_cast<u32>(inst.r.rd.GetValue()), static_cast<u32>(Reg::hi), value);
        }
        break;

        case InstructionFunct::mthi:
        {
          const u32 value = ReadReg(inst.r.rs);
          g_state.regs.hi = value;

          StallUntilMulDivComplete();

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MOVE(static_cast<u32>(Reg::hi), static_cast<u32>(inst.r.rs.GetValue()), value);
        }
        break;

        case InstructionFunct::mflo:
        {
          const u32 value = g_state.regs.lo;
          WriteReg(inst.r.rd, value);

          StallUntilMulDivComplete();

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MOVE(static_cast<u32>(inst.r.rd.GetValue()), static_cast<u32>(Reg::lo), value);
        }
        break;

        case InstructionFunct::mtlo:
        {
          const u32 value = ReadReg(inst.r.rs);
          g_state.regs.lo = value;

          StallUntilMulDivComplete();

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

          StallUntilMulDivComplete();
          AddMulDivTicks(GetMultTicks(static_cast<s32>(lhs)));

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MULT(inst, lhs, rhs);
        }
        break;

        case InstructionFunct::multu:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result = ZeroExtend64(lhs) * ZeroExtend64(rhs);

          g_state.regs.hi = Truncate32(result >> 32);
          g_state.regs.lo = Truncate32(result);

          StallUntilMulDivComplete();
          AddMulDivTicks(GetMultTicks(lhs));

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_MULTU(inst, lhs, rhs);
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

          StallUntilMulDivComplete();
          AddMulDivTicks(GetDivTicks());

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_DIV(inst, num, denom);
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

          StallUntilMulDivComplete();
          AddMulDivTicks(GetDivTicks());

          if constexpr (pgxp_mode >= PGXPMode::CPU)
            PGXP::CPU_DIVU(inst, num, denom);
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
        PGXP::CPU_LUI(inst);
    }
    break;

    case InstructionOp::andi:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 new_value = rsVal & inst.i.imm_zext32();
      WriteReg(inst.i.rt, new_value);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ANDI(inst, rsVal);
    }
    break;

    case InstructionOp::ori:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 imm = inst.i.imm_zext32();
      const u32 rtVal = rsVal | imm;
      WriteReg(inst.i.rt, rtVal);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ORI(inst, rsVal);
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
        PGXP::CPU_XORI(inst, rsVal);
      else if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::TryMoveImm(inst.r.rd, inst.r.rs, imm);
    }
    break;

    case InstructionOp::addi:
    {
      const u32 rsVal = ReadReg(inst.i.rs);
      const u32 imm = inst.i.imm_sext32();
      u32 rtVal;
      if (AddOverflow(rsVal, imm, &rtVal)) [[unlikely]]
      {
        RaiseException(Exception::Ov);
        return;
      }

      WriteReg(inst.i.rt, rtVal);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_ADDI(inst, rsVal);
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
        PGXP::CPU_ADDI(inst, rsVal);
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
        PGXP::CPU_SLTI(inst, rsVal);
    }
    break;

    case InstructionOp::sltiu:
    {
      const u32 result = BoolToUInt32(ReadReg(inst.i.rs) < inst.i.imm_sext32());
      WriteReg(inst.i.rt, result);

      if constexpr (pgxp_mode >= PGXPMode::CPU)
        PGXP::CPU_SLTIU(inst, ReadReg(inst.i.rs));
    }
    break;

    case InstructionOp::lb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Read>(addr);
      }

      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      const u32 sxvalue = SignExtend32(value);

      WriteRegDelayed(inst.i.rt, sxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LBx(inst, addr, sxvalue);
    }
    break;

    case InstructionOp::lh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Read>(addr);
      }

      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      const u32 sxvalue = SignExtend32(value);
      WriteRegDelayed(inst.i.rt, sxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LH(inst, addr, sxvalue);
    }
    break;

    case InstructionOp::lw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Read>(addr);
      }

      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LW(inst, addr, value);
    }
    break;

    case InstructionOp::lbu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Read>(addr);
      }

      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      const u32 zxvalue = ZeroExtend32(value);
      WriteRegDelayed(inst.i.rt, zxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LBx(inst, addr, zxvalue);
    }
    break;

    case InstructionOp::lhu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Read>(addr);
      }

      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      const u32 zxvalue = ZeroExtend32(value);
      WriteRegDelayed(inst.i.rt, zxvalue);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LHU(inst, addr, zxvalue);
    }
    break;

    case InstructionOp::lwl:
    case InstructionOp::lwr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Read>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Read>(addr);
      }

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
        PGXP::CPU_LW(inst, addr, new_value);
    }
    break;

    case InstructionOp::sb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Write>(addr);
      }

      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryByte(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SB(inst, addr, value);
    }
    break;

    case InstructionOp::sh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Write>(addr);
      }

      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryHalfWord(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SH(inst, addr, value);
    }
    break;

    case InstructionOp::sw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(addr);
        MemoryBreakpointCheck<MemoryAccessType::Write>(addr);
      }

      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryWord(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SW(inst, addr, value);
    }
    break;

    case InstructionOp::swl:
    case InstructionOp::swr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
      if constexpr (debug)
      {
        Cop0DataBreakpointCheck<MemoryAccessType::Write>(aligned_addr);
        MemoryBreakpointCheck<MemoryAccessType::Write>(aligned_addr);
      }

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
        PGXP::CPU_SW(inst, aligned_addr, new_value);
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
        WARNING_LOG("Coprocessor 0 not present in user mode");
        RaiseException(Exception::CpU);
        return;
      }

      if (inst.cop.IsCommonInstruction())
      {
        switch (inst.cop.CommonOp())
        {
          case CopCommonInstruction::mfcn:
          {
            u32 value;

            switch (static_cast<Cop0Reg>(inst.r.rd.GetValue()))
            {
              case Cop0Reg::BPC:
                value = g_state.cop0_regs.BPC;
                break;

              case Cop0Reg::BPCM:
                value = g_state.cop0_regs.BPCM;
                break;

              case Cop0Reg::BDA:
                value = g_state.cop0_regs.BDA;
                break;

              case Cop0Reg::BDAM:
                value = g_state.cop0_regs.BDAM;
                break;

              case Cop0Reg::DCIC:
                value = g_state.cop0_regs.dcic.bits;
                break;

              case Cop0Reg::JUMPDEST:
                value = g_state.cop0_regs.TAR;
                break;

              case Cop0Reg::BadVaddr:
                value = g_state.cop0_regs.BadVaddr;
                break;

              case Cop0Reg::SR:
                value = g_state.cop0_regs.sr.bits;
                break;

              case Cop0Reg::CAUSE:
                value = g_state.cop0_regs.cause.bits;
                break;

              case Cop0Reg::EPC:
                value = g_state.cop0_regs.EPC;
                break;

              case Cop0Reg::PRID:
                value = g_state.cop0_regs.PRID;
                break;

              default:
                RaiseException(Exception::RI);
                return;
            }

            WriteRegDelayed(inst.r.rt, value);

            if constexpr (pgxp_mode == PGXPMode::CPU)
              PGXP::CPU_MFC0(inst, value);
          }
          break;

          case CopCommonInstruction::mtcn:
          {
            u32 value = ReadReg(inst.r.rt);
            [[maybe_unused]] const u32 orig_value = value;

            switch (static_cast<Cop0Reg>(inst.r.rd.GetValue()))
            {
              case Cop0Reg::BPC:
              {
                g_state.cop0_regs.BPC = value;
                DEV_LOG("COP0 BPC <- {:08X}", value);
              }
              break;

              case Cop0Reg::BPCM:
              {
                g_state.cop0_regs.BPCM = value;
                DEV_LOG("COP0 BPCM <- {:08X}", value);
                if (UpdateDebugDispatcherFlag())
                  ExitExecution();
              }
              break;

              case Cop0Reg::BDA:
              {
                g_state.cop0_regs.BDA = value;
                DEV_LOG("COP0 BDA <- {:08X}", value);
              }
              break;

              case Cop0Reg::BDAM:
              {
                g_state.cop0_regs.BDAM = value;
                DEV_LOG("COP0 BDAM <- {:08X}", value);
              }
              break;

              case Cop0Reg::JUMPDEST:
              {
                WARNING_LOG("Ignoring write to Cop0 JUMPDEST");
              }
              break;

              case Cop0Reg::DCIC:
              {
                g_state.cop0_regs.dcic.bits = (g_state.cop0_regs.dcic.bits & ~Cop0Registers::DCIC::WRITE_MASK) |
                                              (value & Cop0Registers::DCIC::WRITE_MASK);
                DEV_LOG("COP0 DCIC <- {:08X} (now {:08X})", value, g_state.cop0_regs.dcic.bits);
                value = g_state.cop0_regs.dcic.bits;
                if (UpdateDebugDispatcherFlag())
                  ExitExecution();
              }
              break;

              case Cop0Reg::SR:
              {
                g_state.cop0_regs.sr.bits = (g_state.cop0_regs.sr.bits & ~Cop0Registers::SR::WRITE_MASK) |
                                            (value & Cop0Registers::SR::WRITE_MASK);
                DEBUG_LOG("COP0 SR <- {:08X} (now {:08X})", value, g_state.cop0_regs.sr.bits);
                value = g_state.cop0_regs.sr.bits;
                UpdateMemoryPointers();
                CheckForPendingInterrupt();
              }
              break;

              case Cop0Reg::CAUSE:
              {
                g_state.cop0_regs.cause.bits = (g_state.cop0_regs.cause.bits & ~Cop0Registers::CAUSE::WRITE_MASK) |
                                               (value & Cop0Registers::CAUSE::WRITE_MASK);
                DEBUG_LOG("COP0 CAUSE <- {:08X} (now {:08X})", value, g_state.cop0_regs.cause.bits);
                value = g_state.cop0_regs.cause.bits;
                CheckForPendingInterrupt();
              }
              break;

              [[unlikely]] default:
                RaiseException(Exception::RI);
                return;
            }

            if constexpr (pgxp_mode == PGXPMode::CPU)
              PGXP::CPU_MTC0(inst, value, orig_value);
          }
          break;

          default:
            [[unlikely]] ERROR_LOG("Unhandled instruction at {:08X}: {:08X}", g_state.current_instruction_pc,
                                   inst.bits);
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
            return;

          default:
            [[unlikely]] ERROR_LOG("Unhandled instruction at {:08X}: {:08X}", g_state.current_instruction_pc,
                                   inst.bits);
            break;
        }
      }
    }
    break;

    case InstructionOp::cop2:
    {
      if (!g_state.cop0_regs.sr.CE2) [[unlikely]]
      {
        WARNING_LOG("Coprocessor 2 not enabled");
        RaiseException(Exception::CpU);
        return;
      }

      if (inst.cop.IsCommonInstruction())
      {
        // TODO: Combine with cop0.
        switch (inst.cop.CommonOp())
        {
          case CopCommonInstruction::cfcn:
          {
            StallUntilGTEComplete();

            const u32 value = GTE::ReadRegister(static_cast<u32>(inst.r.rd.GetValue()) + 32);
            WriteRegDelayed(inst.r.rt, value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MFC2(inst, value);
          }
          break;

          case CopCommonInstruction::ctcn:
          {
            const u32 value = ReadReg(inst.r.rt);
            GTE::WriteRegister(static_cast<u32>(inst.r.rd.GetValue()) + 32, value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MTC2(inst, value);
          }
          break;

          case CopCommonInstruction::mfcn:
          {
            StallUntilGTEComplete();

            const u32 value = GTE::ReadRegister(static_cast<u32>(inst.r.rd.GetValue()));
            WriteRegDelayed(inst.r.rt, value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MFC2(inst, value);
          }
          break;

          case CopCommonInstruction::mtcn:
          {
            const u32 value = ReadReg(inst.r.rt);
            GTE::WriteRegister(static_cast<u32>(inst.r.rd.GetValue()), value);

            if constexpr (pgxp_mode >= PGXPMode::Memory)
              PGXP::CPU_MTC2(inst, value);
          }
          break;

          default:
            [[unlikely]] ERROR_LOG("Unhandled instruction at {:08X}: {:08X}", g_state.current_instruction_pc,
                                   inst.bits);
            break;
        }
      }
      else
      {
        StallUntilGTEComplete();
        GTE::ExecuteInstruction(inst.bits);
      }
    }
    break;

    case InstructionOp::lwc2:
    {
      if (!g_state.cop0_regs.sr.CE2) [[unlikely]]
      {
        WARNING_LOG("Coprocessor 2 not enabled");
        RaiseException(Exception::CpU);
        return;
      }

      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      GTE::WriteRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())), value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_LWC2(inst, addr, value);
    }
    break;

    case InstructionOp::swc2:
    {
      if (!g_state.cop0_regs.sr.CE2) [[unlikely]]
      {
        WARNING_LOG("Coprocessor 2 not enabled");
        RaiseException(Exception::CpU);
        return;
      }

      StallUntilGTEComplete();

      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = GTE::ReadRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())));
      WriteMemoryWord(addr, value);

      if constexpr (pgxp_mode >= PGXPMode::Memory)
        PGXP::CPU_SWC2(inst, addr, value);
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
    [[unlikely]]
    default:
    {
      u32 ram_value;
      if (SafeReadInstruction(g_state.current_instruction_pc, &ram_value) &&
          ram_value != g_state.current_instruction.bits) [[unlikely]]
      {
        ERROR_LOG("Stale icache at 0x{:08X} - ICache: {:08X} RAM: {:08X}", g_state.current_instruction_pc,
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
  // The GTE is a co-processor, therefore it executes the instruction even if we're servicing an exception.
  // The exception handlers should recognize this and increment the PC if the EPC was a cop2 instruction.
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

CPUExecutionMode CPU::GetCurrentExecutionMode()
{
  return s_current_execution_mode;
}

bool CPU::UpdateDebugDispatcherFlag()
{
  const bool has_any_breakpoints = (HasAnyBreakpoints() || s_break_type == ExecutionBreakType::SingleStep);

  const auto& dcic = g_state.cop0_regs.dcic;
  const bool has_cop0_breakpoints = dcic.super_master_enable_1 && dcic.super_master_enable_2 &&
                                    dcic.execution_breakpoint_enable && IsCop0ExecutionBreakpointUnmasked();

  const bool use_debug_dispatcher =
    has_any_breakpoints || has_cop0_breakpoints || s_trace_to_log ||
    (g_settings.cpu_execution_mode == CPUExecutionMode::Interpreter && g_settings.bios_tty_logging);
  if (use_debug_dispatcher == g_state.using_debug_dispatcher)
    return false;

  DEV_LOG("{} debug dispatcher", use_debug_dispatcher ? "Now using" : "No longer using");
  g_state.using_debug_dispatcher = use_debug_dispatcher;
  return true;
}

void CPU::CheckForExecutionModeChange()
{
  // Currently, any breakpoints require the interpreter.
  const CPUExecutionMode new_execution_mode =
    (g_state.using_debug_dispatcher ? CPUExecutionMode::Interpreter : g_settings.cpu_execution_mode);
  if (s_current_execution_mode == new_execution_mode) [[likely]]
  {
    DebugAssert(g_state.using_interpreter == (s_current_execution_mode == CPUExecutionMode::Interpreter));
    return;
  }

  WARNING_LOG("Execution mode changed from {} to {}", Settings::GetCPUExecutionModeName(s_current_execution_mode),
              Settings::GetCPUExecutionModeName(new_execution_mode));

  const bool new_interpreter = (new_execution_mode == CPUExecutionMode::Interpreter);
  if (g_state.using_interpreter != new_interpreter)
  {
    // Have to clear out the icache too, only the tags are valid in the recs.
    ClearICache();
    g_state.bus_error = false;

    if (new_interpreter)
    {
      // Switching to interpreter. Set up the pipeline.
      // We'll also need to fetch the next instruction to execute.
      if (!SafeReadInstruction(g_state.pc, &g_state.next_instruction.bits)) [[unlikely]]
      {
        g_state.next_instruction.bits = 0;
        ERROR_LOG("Failed to read current instruction from 0x{:08X}", g_state.pc);
      }

      g_state.npc = g_state.pc + sizeof(Instruction);
    }
    else
    {
      // Switching to recompiler. We can't start a rec block in a branch delay slot, so we need to execute the
      // instruction if we're currently in one.
      if (g_state.next_instruction_is_branch_delay_slot) [[unlikely]]
      {
        while (g_state.next_instruction_is_branch_delay_slot)
        {
          WARNING_LOG("EXECMODE: Executing instruction at 0x{:08X} because it is in a branch delay slot.", g_state.pc);
          if (fastjmp_set(&s_jmp_buf) == 0)
          {
            s_break_type = ExecutionBreakType::ExecuteOneInstruction;
            g_state.using_debug_dispatcher = true;
            ExecuteInterpreter();
          }
        }

        // Need to restart the whole process again, because the branch slot could change the debug flag.
        UpdateDebugDispatcherFlag();
        CheckForExecutionModeChange();
        return;
      }
    }
  }

  s_current_execution_mode = new_execution_mode;
  g_state.using_interpreter = new_interpreter;

  // Wipe out code cache when switching modes.
  if (!new_interpreter)
    CPU::CodeCache::Reset();
}

[[noreturn]] void CPU::ExitExecution()
{
  // can't exit while running events without messing things up
  DebugAssert(!TimingEvents::IsRunningEvents());
  fastjmp_jmp(&s_jmp_buf, 1);
}

bool CPU::HasAnyBreakpoints()
{
  return (GetBreakpointList(BreakpointType::Execute).size() + GetBreakpointList(BreakpointType::Read).size() +
          GetBreakpointList(BreakpointType::Write).size()) > 0;
}

ALWAYS_INLINE CPU::BreakpointList& CPU::GetBreakpointList(BreakpointType type)
{
  return s_breakpoints[static_cast<size_t>(type)];
}

const char* CPU::GetBreakpointTypeName(BreakpointType type)
{
  static constexpr std::array<const char*, static_cast<u32>(BreakpointType::Count)> names = {{
    "Execute",
    "Read",
    "Write",
  }};
  return names[static_cast<size_t>(type)];
}

bool CPU::HasBreakpointAtAddress(BreakpointType type, VirtualMemoryAddress address)
{
  for (Breakpoint& bp : GetBreakpointList(type))
  {
    if (bp.enabled && (bp.address & 0x0FFFFFFFu) == (address & 0x0FFFFFFFu))
    {
      bp.hit_count++;
      return true;
    }
  }

  return false;
}

CPU::BreakpointList CPU::CopyBreakpointList(bool include_auto_clear, bool include_callbacks)
{
  BreakpointList bps;

  size_t total = 0;
  for (const BreakpointList& bplist : s_breakpoints)
    total += bplist.size();

  bps.reserve(total);

  for (const BreakpointList& bplist : s_breakpoints)
  {
    for (const Breakpoint& bp : bplist)
    {
      if (bp.callback && !include_callbacks)
        continue;
      if (bp.auto_clear && !include_auto_clear)
        continue;

      bps.push_back(bp);
    }
  }

  return bps;
}

bool CPU::AddBreakpoint(BreakpointType type, VirtualMemoryAddress address, bool auto_clear, bool enabled)
{
  if (HasBreakpointAtAddress(type, address))
    return false;

  INFO_LOG("Adding {} breakpoint at {:08X}, auto clear = {}", GetBreakpointTypeName(type), address,
           static_cast<unsigned>(auto_clear));

  Breakpoint bp{address, nullptr, auto_clear ? 0 : s_breakpoint_counter++, 0, type, auto_clear, enabled};
  GetBreakpointList(type).push_back(std::move(bp));
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();

  if (!auto_clear)
    Host::ReportDebuggerMessage(fmt::format("Added breakpoint at 0x{:08X}.", address));

  return true;
}

bool CPU::AddBreakpointWithCallback(BreakpointType type, VirtualMemoryAddress address, BreakpointCallback callback)
{
  if (HasBreakpointAtAddress(type, address))
    return false;

  INFO_LOG("Adding {} breakpoint with callback at {:08X}", GetBreakpointTypeName(type), address);

  Breakpoint bp{address, callback, 0, 0, type, false, true};
  GetBreakpointList(type).push_back(std::move(bp));
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();
  return true;
}

bool CPU::SetBreakpointEnabled(BreakpointType type, VirtualMemoryAddress address, bool enabled)
{
  BreakpointList& bplist = GetBreakpointList(type);
  auto it =
    std::find_if(bplist.begin(), bplist.end(), [address](const Breakpoint& bp) { return bp.address == address; });
  if (it == bplist.end())
    return false;

  Host::ReportDebuggerMessage(fmt::format("{} {} breakpoint at 0x{:08X}.", enabled ? "Enabled" : "Disabled",
                                          GetBreakpointTypeName(type), address));
  it->enabled = enabled;

  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();

  if (address == s_last_breakpoint_check_pc && !enabled)
    s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;

  return true;
}

bool CPU::RemoveBreakpoint(BreakpointType type, VirtualMemoryAddress address)
{
  BreakpointList& bplist = GetBreakpointList(type);
  auto it =
    std::find_if(bplist.begin(), bplist.end(), [address](const Breakpoint& bp) { return bp.address == address; });
  if (it == bplist.end())
    return false;

  Host::ReportDebuggerMessage(fmt::format("Removed {} breakpoint at 0x{:08X}.", GetBreakpointTypeName(type), address));

  bplist.erase(it);
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();

  if (address == s_last_breakpoint_check_pc)
    s_last_breakpoint_check_pc = INVALID_BREAKPOINT_PC;

  return true;
}

void CPU::ClearBreakpoints()
{
  for (BreakpointList& bplist : s_breakpoints)
    bplist.clear();
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
    Host::ReportDebuggerMessage(fmt::format("0x{:08X} is not a call instruction.", g_state.pc));
    return false;
  }

  if (!SafeReadInstruction(bp_pc, &inst.bits))
    return false;

  if (IsBranchInstruction(inst))
  {
    Host::ReportDebuggerMessage(fmt::format("Can't step over double branch at 0x{:08X}", g_state.pc));
    return false;
  }

  // skip the delay slot
  bp_pc += sizeof(Instruction);

  Host::ReportDebuggerMessage(fmt::format("Stepping over to 0x{:08X}.", bp_pc));

  return AddBreakpoint(BreakpointType::Execute, bp_pc, true);
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
      Host::ReportDebuggerMessage(
        fmt::format("Instruction read failed at {:08X} while searching for function end.", ret_pc));
      return false;
    }

    if (IsReturnInstruction(inst))
    {
      Host::ReportDebuggerMessage(fmt::format("Stepping out to 0x{:08X}.", ret_pc));
      return AddBreakpoint(BreakpointType::Execute, ret_pc, true);
    }
  }

  Host::ReportDebuggerMessage(fmt::format("No return instruction found after {} instructions for step-out at {:08X}.",
                                          max_instructions_to_search, g_state.pc));

  return false;
}

ALWAYS_INLINE_RELEASE bool CPU::CheckBreakpointList(BreakpointType type, VirtualMemoryAddress address)
{
  BreakpointList& bplist = GetBreakpointList(type);
  size_t count = bplist.size();
  if (count == 0) [[likely]]
    return false;

  for (size_t i = 0; i < count;)
  {
    Breakpoint& bp = bplist[i];
    if (!bp.enabled || (bp.address & 0x0FFFFFFFu) != (address & 0x0FFFFFFFu))
    {
      i++;
      continue;
    }

    bp.hit_count++;

    const u32 pc = g_state.pc;

    if (bp.callback)
    {
      // if callback returns false, the bp is no longer recorded
      if (!bp.callback(BreakpointType::Execute, pc, address))
      {
        bplist.erase(bplist.begin() + i);
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

      TinyString msg;
      if (bp.auto_clear)
      {
        msg.format("Stopped execution at 0x{:08X}.", pc);
        Host::ReportDebuggerMessage(msg);
        bplist.erase(bplist.begin() + i);
        count--;
        UpdateDebugDispatcherFlag();
      }
      else
      {
        msg.format("Hit {} breakpoint {} at 0x{:08X}, Hit Count {}.", GetBreakpointTypeName(type), bp.number, address,
                   bp.hit_count);
        Host::ReportDebuggerMessage(msg);
        i++;
      }

      return true;
    }
  }

  return false;
}

ALWAYS_INLINE_RELEASE void CPU::ExecutionBreakpointCheck()
{
  if (s_breakpoints[static_cast<u32>(BreakpointType::Execute)].empty()) [[likely]]
    return;

  const u32 pc = g_state.pc;
  if (pc == s_last_breakpoint_check_pc || s_break_type == ExecutionBreakType::ExecuteOneInstruction) [[unlikely]]
  {
    // we don't want to trigger the same breakpoint which just paused us repeatedly.
    return;
  }

  s_last_breakpoint_check_pc = pc;

  if (CheckBreakpointList(BreakpointType::Execute, pc)) [[unlikely]]
  {
    s_break_type = ExecutionBreakType::None;
    ExitExecution();
  }
}

template<MemoryAccessType type>
ALWAYS_INLINE_RELEASE void CPU::MemoryBreakpointCheck(VirtualMemoryAddress address)
{
  const BreakpointType bptype = (type == MemoryAccessType::Read) ? BreakpointType::Read : BreakpointType::Write;
  if (CheckBreakpointList(bptype, address)) [[unlikely]]
    s_break_type = ExecutionBreakType::Breakpoint;
}

template<PGXPMode pgxp_mode, bool debug>
[[noreturn]] void CPU::ExecuteImpl()
{
  if (g_state.pending_ticks >= g_state.downcount)
    TimingEvents::RunEvents();

  for (;;)
  {
    do
    {
      if constexpr (debug)
      {
        Cop0ExecutionBreakpointCheck();
        ExecutionBreakpointCheck();
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

        // handle all mirrors of the syscall trampoline. will catch 200000A0 etc, but those aren't fetchable anyway
        const u32 masked_pc = (g_state.current_instruction_pc & KSEG_MASK);
        if (masked_pc == 0xA0) [[unlikely]]
          HandleA0Syscall();
        else if (masked_pc == 0xB0) [[unlikely]]
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

      if constexpr (debug)
      {
        if (s_break_type != ExecutionBreakType::None) [[unlikely]]
        {
          const ExecutionBreakType break_type = std::exchange(s_break_type, ExecutionBreakType::None);
          if (break_type >= ExecutionBreakType::SingleStep)
            System::PauseSystem(true);

          UpdateDebugDispatcherFlag();
          ExitExecution();
        }
      }
    } while (g_state.pending_ticks < g_state.downcount);

    TimingEvents::RunEvents();
  }
}

void CPU::ExecuteInterpreter()
{
  if (g_state.using_debug_dispatcher)
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
  else
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
}

fastjmp_buf* CPU::GetExecutionJmpBuf()
{
  return &s_jmp_buf;
}

void CPU::Execute()
{
  CheckForExecutionModeChange();

  if (fastjmp_set(&s_jmp_buf) != 0)
    return;

  if (g_state.using_interpreter)
    ExecuteInterpreter();
  else
    CodeCache::Execute();
}

void CPU::SetSingleStepFlag()
{
  s_break_type = ExecutionBreakType::SingleStep;
  if (UpdateDebugDispatcherFlag())
    System::InterruptExecution();
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
    g_state.current_instruction_pc = g_state.pc;
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

bool CPU::RecompilerThunks::InterpretInstruction()
{
  ExecuteInstruction<PGXPMode::Disabled, false>();
  return g_state.exception_raised;
}

bool CPU::RecompilerThunks::InterpretInstructionPGXP()
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

template<bool add_ticks, bool icache_read, u32 word_count, bool raise_exceptions>
ALWAYS_INLINE_RELEASE bool CPU::DoInstructionRead(PhysicalMemoryAddress address, u32* data)
{
  using namespace Bus;

  // We can shortcut around VirtualAddressToPhysical() here because we're never going to be
  // calling with an out-of-range address.
  DebugAssert(VirtualAddressToPhysical(address) == (address & KSEG_MASK));
  address &= KSEG_MASK;

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
  else if (address >= EXP1_BASE && address < (EXP1_BASE + EXP1_SIZE))
  {
    g_pio_device->CodeReadHandler(address & EXP1_MASK, data, word_count);
    if constexpr (add_ticks)
      g_state.pending_ticks += g_exp1_access_time[static_cast<u32>(MemoryAccessSize::Word)] * word_count;

    return true;
  }
  else [[unlikely]]
  {
    if (raise_exceptions)
    {
      g_state.cop0_regs.BadVaddr = address;
      RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::IBE, false, false, 0), address);
    }

    std::memset(data, 0, sizeof(u32) * word_count);
    return false;
  }
}

TickCount CPU::GetInstructionReadTicks(VirtualMemoryAddress address)
{
  using namespace Bus;

  DebugAssert(VirtualAddressToPhysical(address) == (address & KSEG_MASK));
  address &= KSEG_MASK;

  if (address < RAM_MIRROR_END)
  {
    return RAM_READ_TICKS;
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_MIRROR_SIZE))
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

  DebugAssert(VirtualAddressToPhysical(address) == (address & KSEG_MASK));
  address &= KSEG_MASK;

  if (address < RAM_MIRROR_END)
  {
    return 1 * ((ICACHE_LINE_SIZE - (address & (ICACHE_LINE_SIZE - 1))) / sizeof(u32));
  }
  else if (address >= BIOS_BASE && address < (BIOS_BASE + BIOS_MIRROR_SIZE))
  {
    return g_bios_access_time[static_cast<u32>(MemoryAccessSize::Word)] *
           ((ICACHE_LINE_SIZE - (address & (ICACHE_LINE_SIZE - 1))) / sizeof(u32));
  }
  else
  {
    return 0;
  }
}

void CPU::CheckAndUpdateICacheTags(u32 line_count)
{
  VirtualMemoryAddress current_pc = g_state.pc & ICACHE_TAG_ADDRESS_MASK;

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

u32 CPU::FillICache(VirtualMemoryAddress address)
{
  const u32 line = GetICacheLine(address);
  const u32 line_word_offset = GetICacheLineWordOffset(address);
  u32* const line_data = g_state.icache_data.data() + (line * ICACHE_WORDS_PER_LINE);
  u32* const offset_line_data = line_data + line_word_offset;
  u32 line_tag;
  switch (line_word_offset)
  {
    case 0:
      DoInstructionRead<true, true, 4, false>(address & ~(ICACHE_LINE_SIZE - 1u), offset_line_data);
      line_tag = GetICacheTagForAddress(address);
      break;
    case 1:
      DoInstructionRead<true, true, 3, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0x4), offset_line_data);
      line_tag = GetICacheTagForAddress(address) | 0x1;
      break;
    case 2:
      DoInstructionRead<true, true, 2, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0x8), offset_line_data);
      line_tag = GetICacheTagForAddress(address) | 0x3;
      break;
    case 3:
    default:
      DoInstructionRead<true, true, 1, false>(address & (~(ICACHE_LINE_SIZE - 1u) | 0xC), offset_line_data);
      line_tag = GetICacheTagForAddress(address) | 0x7;
      break;
  }

  g_state.icache_tags[line] = line_tag;
  return offset_line_data[0];
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
  const u32 line_word_offset = GetICacheLineWordOffset(address);
  const u32* const line_data = g_state.icache_data.data() + (line * ICACHE_WORDS_PER_LINE);
  return line_data[line_word_offset];
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
      CPU::RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::IBE, false, false, 0), address);
      return false;
    }
  }

  g_state.pc = g_state.npc;
  g_state.npc += sizeof(g_state.next_instruction.bits);
  return true;
}

bool CPU::FetchInstructionForInterpreterFallback()
{
  if (!Common::IsAlignedPow2(g_state.npc, 4)) [[unlikely]]
  {
    // The BadVaddr and EPC must be set to the fetching address, not the instruction about to execute.
    g_state.cop0_regs.BadVaddr = g_state.npc;
    RaiseException(Cop0Registers::CAUSE::MakeValueForException(Exception::AdEL, false, false, 0), g_state.npc);
    return false;
  }

  const PhysicalMemoryAddress address = g_state.npc;
  switch (address >> 29)
  {
    case 0x00: // KUSEG 0M-512M
    case 0x04: // KSEG0 - physical memory cached
    case 0x05: // KSEG1 - physical memory uncached
    {
      // We don't use the icache when doing interpreter fallbacks, because it's probably stale.
      if (!DoInstructionRead<false, false, 1, true>(address, &g_state.next_instruction.bits)) [[unlikely]]
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

      address &= KSEG_MASK;
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
      address &= KSEG_MASK;
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
      const u32 page_index = offset >> HOST_PAGE_SHIFT;

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

bool CPU::SafeReadMemoryBytes(VirtualMemoryAddress addr, void* data, u32 length)
{
  using namespace Bus;

  const u32 seg = (addr >> 29);
  if ((seg != 0 && seg != 4 && seg != 5) || (((addr + length) & KSEG_MASK) >= RAM_MIRROR_END) ||
      (((addr & g_ram_mask) + length) > g_ram_size))
  {
    u8* ptr = static_cast<u8*>(data);
    u8* const ptr_end = ptr + length;
    while (ptr != ptr_end)
    {
      if (!SafeReadMemoryByte(addr++, ptr++))
        return false;
    }

    return true;
  }

  // Fast path: all in RAM, no wraparound.
  std::memcpy(data, &g_ram[addr & g_ram_mask], length);
  return true;
}

bool CPU::SafeWriteMemoryBytes(VirtualMemoryAddress addr, const void* data, u32 length)
{
  using namespace Bus;

  const u32 seg = (addr >> 29);
  if ((seg != 0 && seg != 4 && seg != 5) || (((addr + length) & KSEG_MASK) >= RAM_MIRROR_END) ||
      (((addr & g_ram_mask) + length) > g_ram_size))
  {
    const u8* ptr = static_cast<const u8*>(data);
    const u8* const ptr_end = ptr + length;
    while (ptr != ptr_end)
    {
      if (!SafeWriteMemoryByte(addr++, *(ptr++)))
        return false;
    }

    return true;
  }

  // Fast path: all in RAM, no wraparound.
  std::memcpy(&g_ram[addr & g_ram_mask], data, length);
  return true;
}

bool CPU::SafeWriteMemoryBytes(VirtualMemoryAddress addr, const std::span<const u8> data)
{
  return SafeWriteMemoryBytes(addr, data.data(), static_cast<u32>(data.size()));
}

bool CPU::SafeZeroMemoryBytes(VirtualMemoryAddress addr, u32 length)
{
  using namespace Bus;

  const u32 seg = (addr >> 29);
  if ((seg != 0 && seg != 4 && seg != 5) || (((addr + length) & KSEG_MASK) >= RAM_MIRROR_END) ||
      (((addr & g_ram_mask) + length) > g_ram_size))
  {
    while ((addr & 3u) != 0 && length > 0)
    {
      if (!CPU::SafeWriteMemoryByte(addr, 0)) [[unlikely]]
        return false;

      addr++;
      length--;
    }
    while (length >= 4)
    {
      if (!CPU::SafeWriteMemoryWord(addr, 0)) [[unlikely]]
        return false;

      addr += 4;
      length -= 4;
    }
    while (length > 0)
    {
      if (!CPU::SafeWriteMemoryByte(addr, 0)) [[unlikely]]
        return false;

      addr++;
      length--;
    }

    return true;
  }

  // Fast path: all in RAM, no wraparound.
  std::memset(&g_ram[addr & g_ram_mask], 0, length);
  return true;
}

void* CPU::GetDirectReadMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size, TickCount* read_ticks)
{
  using namespace Bus;

  const u32 seg = (address >> 29);
  if (seg != 0 && seg != 4 && seg != 5)
    return nullptr;

  const PhysicalMemoryAddress paddr = VirtualAddressToPhysical(address);
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

  const PhysicalMemoryAddress paddr = address & KSEG_MASK;

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

u64 CPU::RecompilerThunks::ReadMemoryByte(u32 address)
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

u64 CPU::RecompilerThunks::ReadMemoryHalfWord(u32 address)
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

u64 CPU::RecompilerThunks::ReadMemoryWord(u32 address)
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

u32 CPU::RecompilerThunks::WriteMemoryByte(u32 address, u32 value)
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

u32 CPU::RecompilerThunks::WriteMemoryHalfWord(u32 address, u32 value)
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

u32 CPU::RecompilerThunks::WriteMemoryWord(u32 address, u32 value)
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

u32 CPU::RecompilerThunks::UncheckedReadMemoryByte(u32 address)
{
  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::Byte)(address);
  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Byte, address, value);
  return value;
}

u32 CPU::RecompilerThunks::UncheckedReadMemoryHalfWord(u32 address)
{
  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::HalfWord)(address);
  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::HalfWord, address, value);
  return value;
}

u32 CPU::RecompilerThunks::UncheckedReadMemoryWord(u32 address)
{
  const u32 value = GetMemoryReadHandler(address, MemoryAccessSize::Word)(address);
  MEMORY_BREAKPOINT(MemoryAccessType::Read, MemoryAccessSize::Word, address, value);
  return value;
}

void CPU::RecompilerThunks::UncheckedWriteMemoryByte(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Byte, address, value);
  GetMemoryWriteHandler(address, MemoryAccessSize::Byte)(address, value);
}

void CPU::RecompilerThunks::UncheckedWriteMemoryHalfWord(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::HalfWord, address, value);
  GetMemoryWriteHandler(address, MemoryAccessSize::HalfWord)(address, value);
}

void CPU::RecompilerThunks::UncheckedWriteMemoryWord(u32 address, u32 value)
{
  MEMORY_BREAKPOINT(MemoryAccessType::Write, MemoryAccessSize::Word, address, value);
  GetMemoryWriteHandler(address, MemoryAccessSize::Word)(address, value);
}

#undef MEMORY_BREAKPOINT
