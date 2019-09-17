#include "cpu_core.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "cpu_disasm.h"
#include <cstdio>
Log_SetChannel(CPU::Core);

namespace CPU {
bool TRACE_EXECUTION = false;

Core::Core() = default;

Core::~Core() = default;

bool Core::Initialize(Bus* bus)
{
  m_bus = bus;

  // From nocash spec.
  m_cop0_regs.PRID = UINT32_C(0x00000002);

  return true;
}

void Core::Reset()
{
  m_slice_ticks = std::numeric_limits<decltype(m_slice_ticks)>::max();

  m_regs = {};

  m_cop0_regs.BPC = 0;
  m_cop0_regs.BDA = 0;
  m_cop0_regs.JUMPDEST = 0;
  m_cop0_regs.BadVaddr = 0;
  m_cop0_regs.BDAM = 0;
  m_cop0_regs.BPCM = 0;
  m_cop0_regs.EPC = 0;
  m_cop0_regs.sr.bits = 0;
  m_cop0_regs.cause.bits = 0;

  SetPC(RESET_VECTOR);
}

bool Core::DoState(StateWrapper& sw)
{
  sw.Do(&m_slice_ticks);
  sw.DoArray(m_regs.r, countof(m_regs.r));
  sw.Do(&m_regs.pc);
  sw.Do(&m_regs.hi);
  sw.Do(&m_regs.lo);
  sw.Do(&m_regs.npc);
  sw.Do(&m_cop0_regs.BPC);
  sw.Do(&m_cop0_regs.BDA);
  sw.Do(&m_cop0_regs.JUMPDEST);
  sw.Do(&m_cop0_regs.BadVaddr);
  sw.Do(&m_cop0_regs.BDAM);
  sw.Do(&m_cop0_regs.BPCM);
  sw.Do(&m_cop0_regs.EPC);
  sw.Do(&m_cop0_regs.PRID);
  sw.Do(&m_cop0_regs.sr.bits);
  sw.Do(&m_cop0_regs.cause.bits);
  sw.Do(&m_cop0_regs.dcic.bits);
  sw.Do(&m_next_instruction.bits);
  sw.Do(&m_current_instruction_pc);
  sw.Do(&m_load_delay_reg);
  sw.Do(&m_load_delay_old_value);
  sw.Do(&m_next_load_delay_reg);
  sw.Do(&m_next_load_delay_old_value);
  sw.Do(&m_in_branch_delay_slot);
  sw.Do(&m_branched);
  sw.Do(&m_cache_control);
  sw.DoBytes(m_dcache.data(), m_dcache.size());
  return !sw.HasError();
}

void Core::SetPC(u32 new_pc)
{
  m_regs.npc = new_pc;
  FlushPipeline();
}

bool Core::ReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte, false, true>(addr, temp);
  *value = Truncate8(temp);
  return result;
}

bool Core::ReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::HalfWord>(addr))
    return false;

  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord, false, true>(addr, temp);
  *value = Truncate16(temp);
  return result;
}

bool Core::ReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(addr))
    return false;

  return DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word, false, true>(addr, *value);
}

bool Core::WriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte, false, true>(addr, temp);
}

bool Core::WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::HalfWord>(addr))
    return false;

  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord, false, true>(addr, temp);
}

bool Core::WriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  if (!DoAlignmentCheck<MemoryAccessType::Write, MemoryAccessSize::Word>(addr))
    return false;

  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word, false, true>(addr, value);
}

bool Core::SafeReadMemoryByte(VirtualMemoryAddress addr, u8* value)
{
  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte, false, false>(addr, temp);
  *value = Truncate8(temp);
  return result;
}

bool Core::SafeReadMemoryHalfWord(VirtualMemoryAddress addr, u16* value)
{
  u32 temp = 0;
  const bool result = DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord, false, false>(addr, temp);
  *value = Truncate16(temp);
  return result;
}

bool Core::SafeReadMemoryWord(VirtualMemoryAddress addr, u32* value)
{
  return DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word, false, false>(addr, *value);
}

bool Core::SafeWriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte, false, false>(addr, temp);
}

bool Core::SafeWriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  u32 temp = ZeroExtend32(value);
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord, false, false>(addr, temp);
}

bool Core::SafeWriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  return DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word, false, false>(addr, value);
}

void Core::Branch(u32 target)
{
  m_regs.npc = target;
  m_branched = true;
}

u32 Core::GetExceptionVector(Exception excode) const
{
  const u32 base = m_cop0_regs.sr.BEV ? UINT32_C(0xbfc00100) : UINT32_C(0x80000000);
  switch (excode)
  {
    case Exception::BP:
      return base | UINT32_C(0x00000040);

    default:
      return base | UINT32_C(0x00000080);
  }
}

void Core::RaiseException(Exception excode, u8 coprocessor /* = 0 */)
{
  m_cop0_regs.EPC = m_in_branch_delay_slot ? (m_current_instruction_pc - UINT32_C(4)) : m_current_instruction_pc;
  m_cop0_regs.cause.Excode = excode;
  m_cop0_regs.cause.BD = m_in_branch_delay_slot;
  m_cop0_regs.cause.CE = coprocessor;

  // current -> previous, switch to kernel mode and disable interrupts
  m_cop0_regs.sr.mode_bits <<= 2;

  // flush the pipeline - we don't want to execute the previously fetched instruction
  m_regs.npc = GetExceptionVector(excode);
  FlushPipeline();
}

void Core::SetExternalInterrupt(u8 bit)
{
  m_cop0_regs.cause.Ip |= static_cast<u8>(1u << bit);
}

void Core::ClearExternalInterrupt(u8 bit)
{
  m_cop0_regs.cause.Ip &= static_cast<u8>(~(1u << bit));
}

bool Core::DispatchInterrupts()
{
  // const bool do_interrupt = m_cop0_regs.sr.IEc && ((m_cop0_regs.cause.Ip & m_cop0_regs.sr.Im) != 0);
  const bool do_interrupt =
    m_cop0_regs.sr.IEc && (((m_cop0_regs.cause.bits & m_cop0_regs.sr.bits) & (UINT32_C(0xFF) << 8)) != 0);
  if (!do_interrupt)
    return false;

  RaiseException(Exception::INT);
  return true;
}

void Core::FlushLoadDelay()
{
  m_load_delay_reg = Reg::count;
  m_load_delay_old_value = 0;
  m_next_load_delay_reg = Reg::count;
  m_next_load_delay_old_value = 0;
}

void Core::FlushPipeline()
{
  // loads are flushed
  FlushLoadDelay();

  // not in a branch delay slot
  m_branched = false;
  m_in_branch_delay_slot = false;

  // prefetch the next instruction
  FetchInstruction();
}

u32 Core::ReadReg(Reg rs)
{
  return rs == m_load_delay_reg ? m_load_delay_old_value : m_regs.r[static_cast<u8>(rs)];
}

void Core::WriteReg(Reg rd, u32 value)
{
  if (rd != Reg::zero)
    m_regs.r[static_cast<u8>(rd)] = value;
}

void Core::WriteRegDelayed(Reg rd, u32 value)
{
  Assert(m_next_load_delay_reg == Reg::count);
  if (rd == Reg::zero)
    return;

  // save the old value, this will be returned if the register is read in the next instruction
  m_next_load_delay_reg = rd;
  m_next_load_delay_old_value = ReadReg(rd);
  m_regs.r[static_cast<u8>(rd)] = value;
}

void Core::WriteCacheControl(u32 value)
{
  Log_WarningPrintf("Cache control <- 0x%08X", value);
  m_cache_control = value;
}

static void PrintInstruction(u32 bits, u32 pc)
{
  TinyString instr;
  DisassembleInstruction(&instr, pc, bits);

  std::printf("%08x: %08x %s\n", pc, bits, instr.GetCharArray());
}

static constexpr bool AddOverflow(u32 old_value, u32 add_value, u32 new_value)
{
  return (((new_value ^ old_value) & (new_value ^ add_value)) & UINT32_C(0x80000000)) != 0;
}

static constexpr bool SubOverflow(u32 old_value, u32 sub_value, u32 new_value)
{
  return (((new_value ^ old_value) & (old_value ^ sub_value)) & UINT32_C(0x80000000)) != 0;
}

void Core::DisassembleAndPrint(u32 addr)
{
  u32 bits;
  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word, true, false>(addr, bits);
  PrintInstruction(bits, addr);
}

TickCount Core::Execute()
{
  TickCount executed_ticks = 0;
  while (executed_ticks < m_slice_ticks)
  {
    executed_ticks++;

    // now executing the instruction we previously fetched
    const Instruction inst = m_next_instruction;
    m_current_instruction_pc = m_regs.pc;

    // fetch the next instruction
    if (DispatchInterrupts() || !FetchInstruction())
      continue;

    // handle branch delays - we are now in a delay slot if we just branched
    m_in_branch_delay_slot = m_branched;
    m_branched = false;

    // execute the instruction we previously fetched
    ExecuteInstruction(inst);

    // next load delay
    m_load_delay_reg = m_next_load_delay_reg;
    m_next_load_delay_reg = Reg::count;
    m_load_delay_old_value = m_next_load_delay_old_value;
    m_next_load_delay_old_value = 0;
  }

  // reset slice ticks, it'll be updated when the components execute
  m_slice_ticks = MAX_CPU_SLICE_SIZE;
  return executed_ticks;
}

bool Core::FetchInstruction()
{
  m_regs.pc = m_regs.npc;

  if (!DoAlignmentCheck<MemoryAccessType::Read, MemoryAccessSize::Word>(static_cast<VirtualMemoryAddress>(m_regs.npc)))
  {
    // this will call FetchInstruction() again when the pipeline is flushed.
    return false;
  }

  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word, true, true>(
    static_cast<VirtualMemoryAddress>(m_regs.npc), m_next_instruction.bits);
  m_regs.npc += sizeof(m_next_instruction.bits);
  return true;
}

void Core::ExecuteInstruction(Instruction inst)
{
#if 0
  if (inst_pc == 0xBFC06FF0)
  {
    TRACE_EXECUTION = true;
    __debugbreak();
  }
#endif

  if (TRACE_EXECUTION)
    PrintInstruction(inst.bits, m_current_instruction_pc);

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
          WriteReg(inst.r.rd, m_regs.hi);
        }
        break;

        case InstructionFunct::mthi:
        {
          const u32 value = ReadReg(inst.r.rs);
          m_regs.hi = value;
        }
        break;

        case InstructionFunct::mflo:
        {
          WriteReg(inst.r.rd, m_regs.lo);
        }
        break;

        case InstructionFunct::mtlo:
        {
          const u32 value = ReadReg(inst.r.rs);
          m_regs.lo = value;
        }
        break;

        case InstructionFunct::mult:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result =
            static_cast<u64>(static_cast<s64>(SignExtend64(lhs)) * static_cast<s64>(SignExtend64(rhs)));
          m_regs.hi = Truncate32(result >> 32);
          m_regs.lo = Truncate32(result);
        }
        break;

        case InstructionFunct::multu:
        {
          const u32 lhs = ReadReg(inst.r.rs);
          const u32 rhs = ReadReg(inst.r.rt);
          const u64 result = ZeroExtend64(lhs) * ZeroExtend64(rhs);
          m_regs.hi = Truncate32(result >> 32);
          m_regs.lo = Truncate32(result);
        }
        break;

        case InstructionFunct::div:
        {
          const s32 num = static_cast<s32>(ReadReg(inst.r.rs));
          const s32 denom = static_cast<s32>(ReadReg(inst.r.rt));

          if (denom == 0)
          {
            // divide by zero
            m_regs.lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
            m_regs.hi = static_cast<u32>(num);
          }
          else if (static_cast<u32>(num) == UINT32_C(0x80000000) && denom == -1)
          {
            // unrepresentable
            m_regs.lo = UINT32_C(0x80000000);
            m_regs.hi = 0;
          }
          else
          {
            m_regs.lo = static_cast<u32>(num / denom);
            m_regs.hi = static_cast<u32>(num % denom);
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
            m_regs.lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
            m_regs.hi = static_cast<u32>(num);
          }
          else
          {
            m_regs.lo = num / denom;
            m_regs.hi = num % denom;
          }
        }
        break;

        case InstructionFunct::jr:
        {
          const u32 target = ReadReg(inst.r.rs);
          Branch(target);
        }
        break;

        case InstructionFunct::jalr:
        {
          const u32 target = ReadReg(inst.r.rs);
          WriteReg(inst.r.rd, m_regs.npc);
          Branch(target);
        }
        break;

        case InstructionFunct::syscall:
        {
          Log_DebugPrintf("Syscall 0x%X(0x%X)", m_regs.s0, m_regs.a0);
          RaiseException(Exception::Syscall);
        }
        break;

        case InstructionFunct::break_:
        {
          RaiseException(Exception::BP);
        }
        break;

        default:
          UnreachableCode();
          break;
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

      WriteRegDelayed(inst.i.rt, SignExtend32(value));
    }
    break;

    case InstructionOp::lh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, SignExtend32(value));
    }
    break;

    case InstructionOp::lw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u32 value;
      if (!ReadMemoryWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, value);
    }
    break;

    case InstructionOp::lbu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u8 value;
      if (!ReadMemoryByte(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, ZeroExtend32(value));
    }
    break;

    case InstructionOp::lhu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      u16 value;
      if (!ReadMemoryHalfWord(addr, &value))
        return;

      WriteRegDelayed(inst.i.rt, ZeroExtend32(value));
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

      // note: bypasses load delay on the read
      const u32 existing_value = m_regs.r[static_cast<u8>(inst.i.rt.GetValue())];
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
    }
    break;

    case InstructionOp::sb:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u8 value = Truncate8(ReadReg(inst.i.rt));
      WriteMemoryByte(addr, value);
    }
    break;

    case InstructionOp::sh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u16 value = Truncate16(ReadReg(inst.i.rt));
      WriteMemoryHalfWord(addr, value);
    }
    break;

    case InstructionOp::sw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = ReadReg(inst.i.rt);
      WriteMemoryWord(addr, value);
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
    }
    break;

    case InstructionOp::j:
    {
      Branch((m_regs.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::jal:
    {
      m_regs.ra = m_regs.npc;
      Branch((m_regs.pc & UINT32_C(0xF0000000)) | (inst.j.target << 2));
    }
    break;

    case InstructionOp::beq:
    {
      const bool branch = (ReadReg(inst.i.rs) == ReadReg(inst.i.rt));
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bne:
    {
      const bool branch = (ReadReg(inst.i.rs) != ReadReg(inst.i.rt));
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::bgtz:
    {
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) > 0);
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::blez:
    {
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) <= 0);
      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::b:
    {
      const u8 rt = static_cast<u8>(inst.i.rt.GetValue());

      // bgez is the inverse of bltz, so simply do ltz and xor the result
      const bool bgez = ConvertToBoolUnchecked(rt & u8(1));
      const bool branch = (static_cast<s32>(ReadReg(inst.i.rs)) < 0) ^ bgez;

      // register is still linked even if the branch isn't taken
      const bool link = (rt & u8(0x1E)) == u8(0x10);
      if (link)
        m_regs.ra = m_regs.npc;

      if (branch)
        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
    }
    break;

    case InstructionOp::cop0:
    {
      if (!m_cop0_regs.sr.CU0 && InUserMode())
      {
        Log_WarningPrintf("Coprocessor 0 not present in user mode");
        RaiseException(Exception::CpU, 0);
        return;
      }

      ExecuteCop0Instruction(inst);
    }
    break;

    // COP1/COP3 are not present
    case InstructionOp::cop1:
    case InstructionOp::cop2:
    {
      RaiseException(Exception::CpU, inst.cop.cop_n);
    }
    break;

    case InstructionOp::cop3:
    {
      Panic("GTE not implemented");
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void Core::ExecuteCop0Instruction(Instruction inst)
{
  switch (inst.cop.cop0_op())
  {
    case Cop0Instruction::mtc0:
    {
      const u32 value = ReadReg(inst.r.rt);
      switch (static_cast<Cop0Reg>(inst.r.rd.GetValue()))
      {
        case Cop0Reg::BPC:
        {
          m_cop0_regs.BPC = value;
          Log_WarningPrintf("COP0 BPC <- %08X", value);
        }
        break;

        case Cop0Reg::BPCM:
        {
          m_cop0_regs.BPCM = value;
          Log_WarningPrintf("COP0 BPCM <- %08X", value);
        }
        break;

        case Cop0Reg::BDA:
        {
          m_cop0_regs.BDA = value;
          Log_WarningPrintf("COP0 BDA <- %08X", value);
        }
        break;

        case Cop0Reg::BDAM:
        {
          m_cop0_regs.BDAM = value;
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
          m_cop0_regs.dcic.bits =
            (m_cop0_regs.dcic.bits & ~Cop0Registers::DCIC::WRITE_MASK) | (value & Cop0Registers::DCIC::WRITE_MASK);
          Log_WarningPrintf("COP0 DCIC <- %08X (now %08X)", value, m_cop0_regs.dcic.bits);
        }
        break;

        case Cop0Reg::SR:
        {
          m_cop0_regs.sr.bits =
            (m_cop0_regs.sr.bits & ~Cop0Registers::SR::WRITE_MASK) | (value & Cop0Registers::SR::WRITE_MASK);
          Log_WarningPrintf("COP0 SR <- %08X (now %08X)", value, m_cop0_regs.sr.bits);
        }
        break;

        case Cop0Reg::CAUSE:
        {
          m_cop0_regs.cause.bits =
            (m_cop0_regs.cause.bits & ~Cop0Registers::CAUSE::WRITE_MASK) | (value & Cop0Registers::CAUSE::WRITE_MASK);
          Log_WarningPrintf("COP0 CAUSE <- %08X (now %08X)", value, m_cop0_regs.cause.bits);
        }
        break;

        default:
          Panic("Unknown COP0 reg");
          break;
      }
    }
    break;

    case Cop0Instruction::mfc0:
    {
      u32 value;
      switch (static_cast<Cop0Reg>(inst.r.rd.GetValue()))
      {
        case Cop0Reg::BPC:
          value = m_cop0_regs.BPC;
          break;

        case Cop0Reg::BPCM:
          value = m_cop0_regs.BPCM;
          break;

        case Cop0Reg::BDA:
          value = m_cop0_regs.BDA;
          break;

        case Cop0Reg::BDAM:
          value = m_cop0_regs.BDAM;
          break;

        case Cop0Reg::DCIC:
          value = m_cop0_regs.dcic.bits;
          break;

        case Cop0Reg::JUMPDEST:
          value = m_cop0_regs.JUMPDEST;
          break;

        case Cop0Reg::BadVaddr:
          value = m_cop0_regs.BadVaddr;
          break;

        case Cop0Reg::SR:
          value = m_cop0_regs.sr.bits;
          break;

        case Cop0Reg::CAUSE:
          value = m_cop0_regs.cause.bits;
          break;

        case Cop0Reg::EPC:
          value = m_cop0_regs.EPC;
          break;

        case Cop0Reg::PRID:
          value = m_cop0_regs.PRID;
          break;

        default:
          Panic("Unknown COP0 reg");
          value = 0;
          break;
      }

      WriteRegDelayed(inst.r.rt, value);
    }
    break;

    case Cop0Instruction::rfe:
    {
      // restore mode
      m_cop0_regs.sr.mode_bits = (m_cop0_regs.sr.mode_bits & UINT32_C(0b110000)) | (m_cop0_regs.sr.mode_bits >> 2);
    }
    break;

    default:
      Panic("Unhandled instruction");
      break;
  }
}

} // namespace CPU