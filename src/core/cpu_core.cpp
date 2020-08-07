#include "cpu_core.h"
#include "common/align.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "cpu_disasm.h"
#include "cpu_recompiler_thunks.h"
#include "gte.h"
#include "pgxp.h"
#include "settings.h"
#include "timing_event.h"
#include <cstdio>
Log_SetChannel(CPU::Core);

namespace CPU {

/// Sets the PC and flushes the pipeline.
static void SetPC(u32 new_pc);

// Updates load delays - call after each instruction
static void UpdateLoadDelay();

// Fetches the instruction at m_regs.npc
static void ExecuteInstruction();
static void ExecuteCop0Instruction();
static void ExecuteCop2Instruction();
static void Branch(u32 target);

// exceptions
void RaiseException(Exception excode);
void RaiseException(Exception excode, u32 EPC, bool BD, bool BT, u8 CE);

// clears pipeline of load/branch delays
static void FlushPipeline();

// defined in cpu_memory.cpp - memory access functions which return false if an exception was thrown.
bool FetchInstruction();
bool ReadMemoryByte(VirtualMemoryAddress addr, u8* value);
bool ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value);
bool ReadMemoryWord(VirtualMemoryAddress addr, u32* value);
bool WriteMemoryByte(VirtualMemoryAddress addr, u8 value);
bool WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value);
bool WriteMemoryWord(VirtualMemoryAddress addr, u32 value);

State g_state;
bool TRACE_EXECUTION = false;
bool LOG_EXECUTION = false;

void WriteToExecutionLog(const char* format, ...)
{
  static std::FILE* log_file = nullptr;
  static bool log_file_opened = false;

  std::va_list ap;
  va_start(ap, format);

  if (!log_file_opened)
  {
    log_file = FileSystem::OpenCFile("cpu_log.txt", "wb");
    log_file_opened = true;
  }

  if (log_file)
  {
    std::vfprintf(log_file, format, ap);
    std::fflush(log_file);
  }

  va_end(ap);
}

void Initialize()
{
  // From nocash spec.
  g_state.cop0_regs.PRID = UINT32_C(0x00000002);

  GTE::Initialize();

  if (g_settings.gpu_pgxp_enable)
    PGXP::Initialize();
}

void Shutdown()
{
  // GTE::Shutdown();
}

void Reset()
{
  g_state.pending_ticks = 0;
  g_state.downcount = MAX_SLICE_SIZE;

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

  GTE::Reset();

  SetPC(RESET_VECTOR);

  if (g_settings.gpu_pgxp_enable)
    PGXP::Initialize();
}

bool DoState(StateWrapper& sw)
{
  sw.Do(&g_state.pending_ticks);
  sw.Do(&g_state.downcount);
  sw.DoArray(g_state.regs.r, countof(g_state.regs.r));
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
  sw.Do(&g_state.interrupt_delay);
  sw.Do(&g_state.load_delay_reg);
  sw.Do(&g_state.load_delay_value);
  sw.Do(&g_state.next_load_delay_reg);
  sw.Do(&g_state.next_load_delay_value);
  sw.Do(&g_state.cache_control);
  sw.DoBytes(g_state.dcache.data(), g_state.dcache.size());

  if (!GTE::DoState(sw))
    return false;

  if (sw.IsReading())
    PGXP::Initialize();

  return !sw.HasError();
}

void SetPC(u32 new_pc)
{
  DebugAssert(Common::IsAlignedPow2(new_pc, 4));
  g_state.regs.npc = new_pc;
  FlushPipeline();
}

void Branch(u32 target)
{
  if (!Common::IsAlignedPow2(target, 4))
  {
    // The BadVaddr and EPC must be set to the fetching address, not the instruction about to execute.
    g_state.cop0_regs.BadVaddr = target;
    RaiseException(Exception::AdEL, target, false, false, 0);
    return;
  }

  g_state.regs.npc = target;
  g_state.branch_was_taken = true;
}

ALWAYS_INLINE static u32 GetExceptionVector(Exception excode)
{
  const u32 base = g_state.cop0_regs.sr.BEV ? UINT32_C(0xbfc00100) : UINT32_C(0x80000000);

#if 0
  // apparently this isn't correct...
  switch (excode)
  {
    case Exception::BP:
      return base | UINT32_C(0x00000040);

    default:
      return base | UINT32_C(0x00000080);
  }
#else
  return base | UINT32_C(0x00000080);
#endif
}

void RaiseException(Exception excode)
{
  RaiseException(excode, g_state.current_instruction_pc, g_state.current_instruction_in_branch_delay_slot,
                 g_state.current_instruction_was_branch_taken, g_state.current_instruction.cop.cop_n);
}

void RaiseException(Exception excode, u32 EPC, bool BD, bool BT, u8 CE)
{
#ifdef _DEBUG
  if (excode != Exception::INT && excode != Exception::Syscall && excode != Exception::BP)
  {
    Log_DebugPrintf("Exception %u at 0x%08X (epc=0x%08X, BD=%s, CE=%u)", static_cast<u32>(excode),
                    g_state.current_instruction_pc, EPC, BD ? "true" : "false", ZeroExtend32(CE));
    DisassembleAndPrint(g_state.current_instruction_pc, 4, 0);
    if (LOG_EXECUTION)
    {
      CPU::WriteToExecutionLog("Exception %u at 0x%08X (epc=0x%08X, BD=%s, CE=%u)\n", static_cast<u32>(excode),
                               g_state.current_instruction_pc, EPC, BD ? "true" : "false", ZeroExtend32(CE));
    }
  }
#endif

  g_state.cop0_regs.EPC = EPC;
  g_state.cop0_regs.cause.Excode = excode;
  g_state.cop0_regs.cause.BD = BD;
  g_state.cop0_regs.cause.BT = BT;
  g_state.cop0_regs.cause.CE = CE;

  if (BD)
  {
    // TAR is set to the address which was being fetched in this instruction, or the next instruction to execute if the
    // exception hadn't occurred in the delay slot.
    g_state.cop0_regs.EPC -= UINT32_C(4);
    g_state.cop0_regs.TAR = g_state.regs.pc;
  }

  // current -> previous, switch to kernel mode and disable interrupts
  g_state.cop0_regs.sr.mode_bits <<= 2;

  // flush the pipeline - we don't want to execute the previously fetched instruction
  g_state.regs.npc = GetExceptionVector(excode);
  g_state.exception_raised = true;
  FlushPipeline();
}

void SetExternalInterrupt(u8 bit)
{
  g_state.cop0_regs.cause.Ip |= static_cast<u8>(1u << bit);
  g_state.interrupt_delay = 1;
}

void ClearExternalInterrupt(u8 bit)
{
  g_state.cop0_regs.cause.Ip &= static_cast<u8>(~(1u << bit));
}

bool HasPendingInterrupt()
{
  // const bool do_interrupt = g_state.m_cop0_regs.sr.IEc && ((g_state.m_cop0_regs.cause.Ip & g_state.m_cop0_regs.sr.Im)
  // != 0);
  const bool do_interrupt = g_state.cop0_regs.sr.IEc &&
                            (((g_state.cop0_regs.cause.bits & g_state.cop0_regs.sr.bits) & (UINT32_C(0xFF) << 8)) != 0);

  const bool interrupt_delay = g_state.interrupt_delay;
  g_state.interrupt_delay = false;

  return do_interrupt && !interrupt_delay;
}

void DispatchInterrupt()
{
  // If the instruction we're about to execute is a GTE instruction, delay dispatching the interrupt until the next
  // instruction. For some reason, if we don't do this, we end up with incorrectly sorted polygons and flickering..
  if (g_state.next_instruction.IsCop2Instruction())
    return;

  // Interrupt raising occurs before the start of the instruction.
  RaiseException(Exception::INT, g_state.regs.pc, g_state.next_instruction_is_branch_delay_slot,
                 g_state.branch_was_taken, g_state.next_instruction.cop.cop_n);
}

void UpdateLoadDelay()
{
  // the old value is needed in case the delay slot instruction overwrites the same register
  if (g_state.load_delay_reg != Reg::count)
    g_state.regs.r[static_cast<u8>(g_state.load_delay_reg)] = g_state.load_delay_value;

  g_state.load_delay_reg = g_state.next_load_delay_reg;
  g_state.load_delay_value = g_state.next_load_delay_value;
  g_state.next_load_delay_reg = Reg::count;
}

void FlushPipeline()
{
  // loads are flushed
  g_state.next_load_delay_reg = Reg::count;
  if (g_state.load_delay_reg != Reg::count)
  {
    g_state.regs.r[static_cast<u8>(g_state.load_delay_reg)] = g_state.load_delay_value;
    g_state.load_delay_reg = Reg::count;
  }

  // not in a branch delay slot
  g_state.branch_was_taken = false;
  g_state.next_instruction_is_branch_delay_slot = false;
  g_state.current_instruction_pc = g_state.regs.pc;

  // prefetch the next instruction
  FetchInstruction();

  // and set it as the next one to execute
  g_state.current_instruction.bits = g_state.next_instruction.bits;
  g_state.current_instruction_in_branch_delay_slot = false;
  g_state.current_instruction_was_branch_taken = false;
}

ALWAYS_INLINE u32 ReadReg(Reg rs)
{
  return g_state.regs.r[static_cast<u8>(rs)];
}

ALWAYS_INLINE void WriteReg(Reg rd, u32 value)
{
  g_state.regs.r[static_cast<u8>(rd)] = value;
  g_state.load_delay_reg = (rd == g_state.load_delay_reg) ? Reg::count : g_state.load_delay_reg;

  // prevent writes to $zero from going through - better than branching/cmov
  g_state.regs.zero = 0;
}

static void WriteRegDelayed(Reg rd, u32 value)
{
  Assert(g_state.next_load_delay_reg == Reg::count);
  if (rd == Reg::zero)
    return;

  // double load delays ignore the first value
  if (g_state.load_delay_reg == rd)
    g_state.load_delay_reg = Reg::count;

  // save the old value, if something else overwrites this reg we want to preserve it
  g_state.next_load_delay_reg = rd;
  g_state.next_load_delay_value = value;
}

static std::optional<u32> ReadCop0Reg(Cop0Reg reg)
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
      Log_DevPrintf("Unknown COP0 reg %u", ZeroExtend32(static_cast<u8>(reg)));
      return std::nullopt;
  }
}

static void WriteCop0Reg(Cop0Reg reg, u32 value)
{
  switch (reg)
  {
    case Cop0Reg::BPC:
    {
      g_state.cop0_regs.BPC = value;
      Log_WarningPrintf("COP0 BPC <- %08X", value);
    }
    break;

    case Cop0Reg::BPCM:
    {
      g_state.cop0_regs.BPCM = value;
      Log_WarningPrintf("COP0 BPCM <- %08X", value);
    }
    break;

    case Cop0Reg::BDA:
    {
      g_state.cop0_regs.BDA = value;
      Log_WarningPrintf("COP0 BDA <- %08X", value);
    }
    break;

    case Cop0Reg::BDAM:
    {
      g_state.cop0_regs.BDAM = value;
      Log_WarningPrintf("COP0 BDAM <- %08X", value);
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
      Log_WarningPrintf("COP0 DCIC <- %08X (now %08X)", value, g_state.cop0_regs.dcic.bits);
    }
    break;

    case Cop0Reg::SR:
    {
      g_state.cop0_regs.sr.bits =
        (g_state.cop0_regs.sr.bits & ~Cop0Registers::SR::WRITE_MASK) | (value & Cop0Registers::SR::WRITE_MASK);
      Log_DebugPrintf("COP0 SR <- %08X (now %08X)", value, g_state.cop0_regs.sr.bits);
    }
    break;

    case Cop0Reg::CAUSE:
    {
      g_state.cop0_regs.cause.bits =
        (g_state.cop0_regs.cause.bits & ~Cop0Registers::CAUSE::WRITE_MASK) | (value & Cop0Registers::CAUSE::WRITE_MASK);
      Log_DebugPrintf("COP0 CAUSE <- %08X (now %08X)", value, g_state.cop0_regs.cause.bits);
    }
    break;

    default:
      Log_DevPrintf("Unknown COP0 reg %u", ZeroExtend32(static_cast<u8>(reg)));
      break;
  }
}

static void PrintInstruction(u32 bits, u32 pc, Registers* regs)
{
  TinyString instr;
  DisassembleInstruction(&instr, pc, bits, regs);

  std::printf("%08x: %08x %s\n", pc, bits, instr.GetCharArray());
}

static void LogInstruction(u32 bits, u32 pc, Registers* regs)
{
  TinyString instr;
  DisassembleInstruction(&instr, pc, bits, regs);

  WriteToExecutionLog("%08x: %08x %s\n", pc, bits, instr.GetCharArray());
}

static constexpr bool AddOverflow(u32 old_value, u32 add_value, u32 new_value)
{
  return (((new_value ^ old_value) & (new_value ^ add_value)) & UINT32_C(0x80000000)) != 0;
}

static constexpr bool SubOverflow(u32 old_value, u32 sub_value, u32 new_value)
{
  return (((new_value ^ old_value) & (old_value ^ sub_value)) & UINT32_C(0x80000000)) != 0;
}

void DisassembleAndPrint(u32 addr)
{
  u32 bits = 0;
  SafeReadMemoryWord(addr, &bits);
  PrintInstruction(bits, addr, &g_state.regs);
}

void DisassembleAndPrint(u32 addr, u32 instructions_before /* = 0 */, u32 instructions_after /* = 0 */)
{
  u32 disasm_addr = addr - (instructions_before * sizeof(u32));
  for (u32 i = 0; i < instructions_before; i++)
  {
    DisassembleAndPrint(disasm_addr);
    disasm_addr += sizeof(u32);
  }

  std::printf("----> ");

  // <= to include the instruction itself
  for (u32 i = 0; i <= instructions_after; i++)
  {
    DisassembleAndPrint(disasm_addr);
    disasm_addr += sizeof(u32);
  }
}

void Execute()
{
  g_state.frame_done = false;
  while (!g_state.frame_done)
  {
    TimingEvents::UpdateCPUDowncount();

    while (g_state.pending_ticks <= g_state.downcount)
    {
      if (HasPendingInterrupt())
        DispatchInterrupt();

      g_state.pending_ticks++;

      // now executing the instruction we previously fetched
      g_state.current_instruction.bits = g_state.next_instruction.bits;
      g_state.current_instruction_pc = g_state.regs.pc;
      g_state.current_instruction_in_branch_delay_slot = g_state.next_instruction_is_branch_delay_slot;
      g_state.current_instruction_was_branch_taken = g_state.branch_was_taken;
      g_state.next_instruction_is_branch_delay_slot = false;
      g_state.branch_was_taken = false;
      g_state.exception_raised = false;

      // fetch the next instruction
      if (!FetchInstruction())
        continue;

#if 0 // GTE flag test debugging
      if (g_state.m_current_instruction_pc == 0x8002cdf4)
      {
        if (g_state.m_regs.v1 != g_state.m_regs.v0)
          printf("Got %08X Expected? %08X\n", g_state.m_regs.v1, g_state.m_regs.v0);
      }
#endif

      // execute the instruction we previously fetched
      ExecuteInstruction();

      // next load delay
      UpdateLoadDelay();
    }

    TimingEvents::RunEvents();
  }
}

void ExecuteInstruction()
{
  const Instruction inst = g_state.current_instruction;

#if 0
  if (g_state.m_current_instruction_pc == 0x80010000)
  {
    LOG_EXECUTION = true;
    __debugbreak();
  }
#endif

#if 0
  if (g_state.m_current_instruction_pc == 0x8002bf50)
  {
    TRACE_EXECUTION = true;
    __debugbreak();
  }
#endif

#ifdef _DEBUG
  if (TRACE_EXECUTION)
    PrintInstruction(inst.bits, g_state.current_instruction_pc, &g_state.regs);
  if (LOG_EXECUTION)
    LogInstruction(inst.bits, g_state.current_instruction_pc, &g_state.regs);
#endif

  switch (inst.op)
  {
    case InstructionOp::funct:
    {
      switch (inst.r.funct)
      {
        case InstructionFunct::sll:
        {
          const u32 new_value = ReadReg(inst.r.rt) << inst.r.shamt;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srl:
        {
          const u32 new_value = ReadReg(inst.r.rt) >> inst.r.shamt;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sra:
        {
          const u32 new_value = static_cast<u32>(static_cast<s32>(ReadReg(inst.r.rt)) >> inst.r.shamt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::sllv:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = ReadReg(inst.r.rt) << shift_amount;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srlv:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = ReadReg(inst.r.rt) >> shift_amount;
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::srav:
        {
          const u32 shift_amount = ReadReg(inst.r.rs) & UINT32_C(0x1F);
          const u32 new_value = static_cast<u32>(static_cast<s32>(ReadReg(inst.r.rt)) >> shift_amount);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::and_:
        {
          const u32 new_value = ReadReg(inst.r.rs) & ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::or_:
        {
          const u32 new_value = ReadReg(inst.r.rs) | ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::xor_:
        {
          const u32 new_value = ReadReg(inst.r.rs) ^ ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::nor:
        {
          const u32 new_value = ~(ReadReg(inst.r.rs) | ReadReg(inst.r.rt));
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

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::addu:
        {
          const u32 new_value = ReadReg(inst.r.rs) + ReadReg(inst.r.rt);
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

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::subu:
        {
          const u32 new_value = ReadReg(inst.r.rs) - ReadReg(inst.r.rt);
          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::slt:
        {
          const u32 result = BoolToUInt32(static_cast<s32>(ReadReg(inst.r.rs)) < static_cast<s32>(ReadReg(inst.r.rt)));
          WriteReg(inst.r.rd, result);
        }
        break;

        case InstructionFunct::sltu:
        {
          const u32 result = BoolToUInt32(ReadReg(inst.r.rs) < ReadReg(inst.r.rt));
          WriteReg(inst.r.rd, result);
        }
        break;

        case InstructionFunct::mfhi:
        {
          WriteReg(inst.r.rd, g_state.regs.hi);
        }
        break;

        case InstructionFunct::mthi:
        {
          const u32 value = ReadReg(inst.r.rs);
          g_state.regs.hi = value;
        }
        break;

        case InstructionFunct::mflo:
        {
          WriteReg(inst.r.rd, g_state.regs.lo);
        }
        break;

        case InstructionFunct::mtlo:
        {
          const u32 value = ReadReg(inst.r.rs);
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
        }
        break;

        case InstructionFunct::multu:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result = ZeroExtend64(lhs) * ZeroExtend64(rhs);
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
          WriteReg(inst.r.rd, g_state.regs.npc);
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
          RaiseException(Exception::BP);
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
      WriteReg(inst.i.rt, inst.i.imm_zext32() << 16);
    }
    break;

    case InstructionOp::andi:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) & inst.i.imm_zext32());
    }
    break;

    case InstructionOp::ori:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) | inst.i.imm_zext32());
    }
    break;

    case InstructionOp::xori:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) ^ inst.i.imm_zext32());
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

      WriteReg(inst.i.rt, new_value);
    }
    break;

    case InstructionOp::addiu:
    {
      WriteReg(inst.i.rt, ReadReg(inst.i.rs) + inst.i.imm_sext32());
    }
    break;

    case InstructionOp::slti:
    {
      const u32 result = BoolToUInt32(static_cast<s32>(ReadReg(inst.i.rs)) < static_cast<s32>(inst.i.imm_sext32()));
      WriteReg(inst.i.rt, result);
    }
    break;

    case InstructionOp::sltiu:
    {
      const u32 result = BoolToUInt32(ReadReg(inst.i.rs) < inst.i.imm_sext32());
      WriteReg(inst.i.rt, result);
    }
    break;

    case InstructionOp::lb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      const u32 sxvalue = SignExtend32(value);

      WriteRegDelayed(inst.i.rt, sxvalue);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_LBx(inst.bits, sxvalue, addr);
    }
    break;

    case InstructionOp::lh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      const u32 sxvalue = SignExtend32(value);
      WriteRegDelayed(inst.i.rt, sxvalue);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_LHx(inst.bits, sxvalue, addr);
    }
    break;

    case InstructionOp::lw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, value);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_LW(inst.bits, value, addr);
    }
    break;

    case InstructionOp::lbu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      const u32 zxvalue = ZeroExtend32(value);
      WriteRegDelayed(inst.i.rt, zxvalue);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_LBx(inst.bits, zxvalue, addr);
    }
    break;

    case InstructionOp::lhu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      const u32 zxvalue = ZeroExtend32(value);
      WriteRegDelayed(inst.i.rt, zxvalue);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_LHx(inst.bits, zxvalue, addr);
    }
    break;

    case InstructionOp::lwl:
    case InstructionOp::lwr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
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

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_LW(inst.bits, new_value, addr);
    }
    break;

    case InstructionOp::sb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u8 value = Truncate8(ReadReg(inst.i.rt));
      WriteMemoryByte(addr, value);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_SB(inst.bits, value, addr);
    }
    break;

    case InstructionOp::sh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u16 value = Truncate16(ReadReg(inst.i.rt));
      WriteMemoryHalfWord(addr, value);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_SH(inst.bits, value, addr);
    }
    break;

    case InstructionOp::sw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryWord(addr, value);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_SW(inst.bits, value, addr);
    }
    break;

    case InstructionOp::swl:
    case InstructionOp::swr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
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

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_SW(inst.bits, new_value, addr);
    }
    break;

    case InstructionOp::j:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      Branch((g_state.regs.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::jal:
    {
      WriteReg(Reg::ra, g_state.regs.npc);
      g_state.next_instruction_is_branch_delay_slot = true;
      Branch((g_state.regs.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::beq:
    {
      // We're still flagged as a branch delay slot even if the branch isn't taken.
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (ReadReg(inst.i.rs) == ReadReg(inst.i.rt));
      if (branch)
        Branch(g_state.regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bne:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (ReadReg(inst.i.rs) != ReadReg(inst.i.rt));
      if (branch)
        Branch(g_state.regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bgtz:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) > 0);
      if (branch)
        Branch(g_state.regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::blez:
    {
      g_state.next_instruction_is_branch_delay_slot = true;
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) <= 0);
      if (branch)
        Branch(g_state.regs.pc + (inst.i.imm_sext32() << 2));
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
        WriteReg(Reg::ra, g_state.regs.npc);

      if (branch)
        Branch(g_state.regs.pc + (inst.i.imm_sext32() << 2));
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

      ExecuteCop0Instruction();
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

      ExecuteCop2Instruction();
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

      GTE::WriteRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())), value);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_LWC2(inst.bits, value, addr);
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

      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = GTE::ReadRegister(ZeroExtend32(static_cast<u8>(inst.i.rt.GetValue())));
      WriteMemoryWord(addr, value);

      if (g_settings.gpu_pgxp_enable)
        PGXP::CPU_SWC2(inst.bits, value, addr);
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
      RaiseException(Exception::RI);
    }
    break;
  }
}

void ExecuteCop0Instruction()
{
  const Instruction inst = g_state.current_instruction;

  if (inst.cop.IsCommonInstruction())
  {
    switch (inst.cop.CommonOp())
    {
      case CopCommonInstruction::mfcn:
      {
        const std::optional<u32> value = ReadCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue()));
        if (value)
          WriteRegDelayed(inst.r.rt, value.value());
        else
          RaiseException(Exception::RI);
      }
      break;

      case CopCommonInstruction::mtcn:
      {
        WriteCop0Reg(static_cast<Cop0Reg>(inst.r.rd.GetValue()), ReadReg(inst.r.rt));
      }
      break;

      default:
        Panic("Missing implementation");
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
      }
      break;

      default:
        Panic("Missing implementation");
        break;
    }
  }
}

void ExecuteCop2Instruction()
{
  const Instruction inst = g_state.current_instruction;

  if (inst.cop.IsCommonInstruction())
  {
    // TODO: Combine with cop0.
    switch (inst.cop.CommonOp())
    {
      case CopCommonInstruction::cfcn:
      {
        const u32 value = GTE::ReadRegister(static_cast<u32>(inst.r.rd.GetValue()) + 32);
        WriteRegDelayed(inst.r.rt, value);

        if (g_settings.gpu_pgxp_enable)
          PGXP::CPU_CFC2(inst.bits, value, value);
      }
      break;

      case CopCommonInstruction::ctcn:
      {
        const u32 value = ReadReg(inst.r.rt);
        GTE::WriteRegister(static_cast<u32>(inst.r.rd.GetValue()) + 32, value);

        if (g_settings.gpu_pgxp_enable)
          PGXP::CPU_CTC2(inst.bits, value, value);
      }
      break;

      case CopCommonInstruction::mfcn:
      {
        const u32 value = GTE::ReadRegister(static_cast<u32>(inst.r.rd.GetValue()));
        WriteRegDelayed(inst.r.rt, value);

        if (g_settings.gpu_pgxp_enable)
          PGXP::CPU_MFC2(inst.bits, value, value);
      }
      break;

      case CopCommonInstruction::mtcn:
      {
        const u32 value = ReadReg(inst.r.rt);
        GTE::WriteRegister(static_cast<u32>(inst.r.rd.GetValue()), value);

        if (g_settings.gpu_pgxp_enable)
          PGXP::CPU_MTC2(inst.bits, value, value);
      }
      break;

      case CopCommonInstruction::bcnc:
      default:
        Panic("Missing implementation");
        break;
    }
  }
  else
  {
    GTE::ExecuteInstruction(inst.bits);
  }
}

namespace CodeCache {

void InterpretCachedBlock(const CodeBlock& block)
{
  // set up the state so we've already fetched the instruction
  DebugAssert(g_state.regs.pc == block.GetPC());

  g_state.regs.npc = block.GetPC() + 4;

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
    g_state.regs.pc = g_state.regs.npc;
    g_state.regs.npc += 4;

    // execute the instruction we previously fetched
    ExecuteInstruction();

    // next load delay
    UpdateLoadDelay();

    if (g_state.exception_raised)
      break;
  }

  // cleanup so the interpreter can kick in if needed
  g_state.next_instruction_is_branch_delay_slot = false;
}

void InterpretUncachedBlock()
{
  Panic("Fixme with regards to re-fetching PC");

  // At this point, pc contains the last address executed (in the previous block). The instruction has not been fetched
  // yet. pc shouldn't be updated until the fetch occurs, that way the exception occurs in the delay slot.
  bool in_branch_delay_slot = false;
  for (;;)
  {
    g_state.pending_ticks++;

    // now executing the instruction we previously fetched
    g_state.current_instruction.bits = g_state.next_instruction.bits;
    g_state.current_instruction_pc = g_state.regs.pc;
    g_state.current_instruction_in_branch_delay_slot = g_state.next_instruction_is_branch_delay_slot;
    g_state.current_instruction_was_branch_taken = g_state.branch_was_taken;
    g_state.next_instruction_is_branch_delay_slot = false;
    g_state.branch_was_taken = false;
    g_state.exception_raised = false;

    // Fetch the next instruction, except if we're in a branch delay slot. The "fetch" is done in the next block.
    if (!FetchInstruction())
      break;

    // execute the instruction we previously fetched
    ExecuteInstruction();

    // next load delay
    UpdateLoadDelay();

    const bool branch = IsBranchInstruction(g_state.current_instruction);
    if (g_state.exception_raised || (!branch && in_branch_delay_slot) ||
        IsExitBlockInstruction(g_state.current_instruction))
    {
      break;
    }

    in_branch_delay_slot = branch;
  }
}

} // namespace CodeCache

namespace Recompiler::Thunks {

bool InterpretInstruction()
{
  ExecuteInstruction();
  return g_state.exception_raised;
}

void RaiseException(u32 epc, u32 ri_bits)
{
  const RaiseExceptionInfo ri{ri_bits};
  RaiseException(static_cast<Exception>(ri.excode), epc, ri.BD, g_state.current_instruction_was_branch_taken, ri.CE);
}

void RaiseAddressException(u32 address, bool store, bool branch)
{
  g_state.cop0_regs.BadVaddr = address;
  if (branch)
    RaiseException(Exception::AdEL, address, false, false, 0);
  else
    RaiseException(store ? Exception::AdES : Exception::AdEL);
}

} // namespace Recompiler::Thunks

} // namespace CPU