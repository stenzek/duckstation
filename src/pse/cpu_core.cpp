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
  return true;
}

void Core::Reset()
{
  m_regs = {};
  m_regs.npc = RESET_VECTOR;
  FetchInstruction();
}

bool Core::DoState(StateWrapper& sw)
{
  return false;
}

u8 Core::ReadMemoryByte(VirtualMemoryAddress addr)
{
  u32 value;
  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Byte, false>(addr, value);
  return Truncate8(value);
}

u16 Core::ReadMemoryHalfWord(VirtualMemoryAddress addr)
{
  Assert(Common::IsAlignedPow2(addr, 2));
  u32 value;
  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::HalfWord, false>(addr, value);
  return Truncate16(value);
}

u32 Core::ReadMemoryWord(VirtualMemoryAddress addr)
{
  Assert(Common::IsAlignedPow2(addr, 4));
  u32 value;
  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word, false>(addr, value);
  return value;
}

void Core::WriteMemoryByte(VirtualMemoryAddress addr, u8 value)
{
  u32 value32 = ZeroExtend32(value);
  DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Byte, false>(addr, value32);
}

void Core::WriteMemoryHalfWord(VirtualMemoryAddress addr, u16 value)
{
  Assert(Common::IsAlignedPow2(addr, 2));
  u32 value32 = ZeroExtend32(value);
  DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::HalfWord, false>(addr, value32);
}

void Core::WriteMemoryWord(VirtualMemoryAddress addr, u32 value)
{
  Assert(Common::IsAlignedPow2(addr, 4));
  DoMemoryAccess<MemoryAccessType::Write, MemoryAccessSize::Word, false>(addr, value);
}

void Core::Branch(u32 target)
{
  m_regs.npc = target;
  m_branched = true;
}

void Core::RaiseException(u32 inst_pc, Exception excode)
{
  m_cop0_regs.EPC = m_in_branch_delay_slot ? (inst_pc - UINT32_C(4)) : inst_pc;
  m_cop0_regs.cause.Excode = excode;
  m_cop0_regs.cause.BD = m_in_branch_delay_slot;

  // current -> previous
  m_cop0_regs.sr.mode_bits <<= 2;

  // flush the pipeline - we don't want to execute the previously fetched instruction
  m_regs.npc = m_cop0_regs.sr.BEV ? UINT32_C(0xbfc00180) : UINT32_C(0x80000080);
  FlushPipeline();
}

void Core::FlushPipeline()
{
  // loads are flushed
  m_load_delay_reg = Reg::count;
  m_load_delay_old_value = 0;
  m_next_load_delay_reg = Reg::count;
  m_next_load_delay_old_value = 0;

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
  m_next_load_delay_old_value = m_regs.r[static_cast<u8>(rd)];
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

void Core::DisassembleAndPrint(u32 addr)
{
  u32 bits;
  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word, true>(addr, bits);
  PrintInstruction(bits, addr);
}

void Core::Execute()
{
  // now executing the instruction we previously fetched
  const Instruction inst = m_next_instruction;
  const u32 inst_pc = m_regs.pc;

  // fetch the next instruction
  FetchInstruction();

  // handle branch delays - we are now in a delay slot if we just branched
  m_in_branch_delay_slot = m_branched;
  m_branched = false;

  // execute the instruction we previously fetched
  ExecuteInstruction(inst, inst_pc);

  // next load delay
  m_load_delay_reg = m_next_load_delay_reg;
  m_next_load_delay_reg = Reg::count;
  m_load_delay_old_value = m_next_load_delay_old_value;
  m_next_load_delay_old_value = 0;
}

void Core::FetchInstruction()
{
  DoMemoryAccess<MemoryAccessType::Read, MemoryAccessSize::Word, true>(static_cast<VirtualMemoryAddress>(m_regs.npc),
                                                                       m_next_instruction.bits);
  m_regs.pc = m_regs.npc;
  m_regs.npc += sizeof(m_next_instruction.bits);
}

void Core::ExecuteInstruction(Instruction inst, u32 inst_pc)
{
  if (TRACE_EXECUTION)
    PrintInstruction(inst.bits, inst_pc);

#if 0
  if (inst_pc == 0x8005ab80)
    __debugbreak();
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
            RaiseException(inst_pc, Exception::Ov);

          WriteReg(inst.r.rd, new_value);
        }
        break;

        case InstructionFunct::addu:
        {
          const u32 new_value = ReadReg(inst.r.rs) + ReadReg(inst.r.rt);
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
          RaiseException(inst_pc, Exception::Syscall);
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

    case InstructionOp::addi:
    {
      const u32 old_value = ReadReg(inst.i.rs);
      const u32 add_value = inst.i.imm_sext32();
      const u32 new_value = old_value + add_value;
      if (AddOverflow(old_value, add_value, new_value))
        RaiseException(inst_pc, Exception::Ov);

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
      const u8 value = ReadMemoryByte(addr);
      WriteRegDelayed(inst.i.rt, SignExtend32(value));
    }
    break;

    case InstructionOp::lh:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u16 value = ReadMemoryHalfWord(addr);
      WriteRegDelayed(inst.i.rt, SignExtend32(value));
    }
    break;

    case InstructionOp::lw:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u32 value = ReadMemoryWord(addr);
      WriteRegDelayed(inst.i.rt, value);
    }
    break;

    case InstructionOp::lbu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u8 value = ReadMemoryByte(addr);
      WriteRegDelayed(inst.i.rt, ZeroExtend32(value));
    }
    break;

    case InstructionOp::lhu:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const u16 value = ReadMemoryHalfWord(addr);
      WriteRegDelayed(inst.i.rt, ZeroExtend32(value));
    }
    break;

    case InstructionOp::lwl:
    case InstructionOp::lwr:
    {
      const VirtualMemoryAddress addr = ReadReg(inst.i.rs) + inst.i.imm_sext32();
      const VirtualMemoryAddress aligned_addr = addr & ~UINT32_C(3);
      const u32 aligned_value = ReadMemoryWord(aligned_addr);

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
      const u32 mem_value = ReadMemoryWord(aligned_addr);
      const u32 reg_value = ReadReg(inst.i.rt);
      const u8 shift = (Truncate8(addr) & u8(3)) * u8(8);

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
      if (branch)
      {
        const bool link = ConvertToBoolUnchecked((rt >> 4) & u8(1));
        if (link)
          m_regs.ra = m_regs.npc;

        Branch(m_regs.pc + (inst.i.imm_sext32() << 2));
      }
    }
    break;

    case InstructionOp::cop0:
    {
      if (!m_cop0_regs.sr.CU0 && InUserMode())
      {
        Log_WarningPrintf("Coprocessor 0 not present in user mode");
        RaiseException(inst_pc, Exception::CpU);
        return;
      }

      ExecuteCop0Instruction(inst, inst_pc);
    }
    break;

    // COP1/COP3 are not present
    case InstructionOp::cop1:
    case InstructionOp::cop2:
    {
      RaiseException(inst_pc, Exception::CpU);
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

void Core::ExecuteCop0Instruction(Instruction inst, u32 inst_pc)
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

        case Cop0Reg::SR:
          value = m_cop0_regs.sr.bits;
          break;

        case Cop0Reg::CAUSE:
          value = m_cop0_regs.cause.bits;
          break;

        case Cop0Reg::EPC:
          value = m_cop0_regs.EPC;
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