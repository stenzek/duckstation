#include "cpu_recompiler_code_generator.h"
#include "YBaseLib/Log.h"
#include "cpu_disasm.h"
Log_SetChannel(CPU::Recompiler);

// TODO: Turn load+sext/zext into a single signed/unsigned load
// TODO: mulx/shlx/etc
// TODO: when writing to the same register, don't allocate a temporary and copy it (mainly for shifts)

namespace CPU::Recompiler {

u32 CodeGenerator::CalculateRegisterOffset(Reg reg)
{
  return uint32(offsetof(Core, m_regs.r[0]) + (static_cast<u32>(reg) * sizeof(u32)));
}

bool CodeGenerator::CompileBlock(const CodeBlock* block, CodeBlock::HostCodePointer* out_host_code,
                                 u32* out_host_code_size)
{
  // TODO: Align code buffer.

  m_block = block;
  m_block_start = block->instructions.data();
  m_block_end = block->instructions.data() + block->instructions.size();

  EmitBeginBlock();
  BlockPrologue();

  const CodeBlockInstruction* cbi = m_block_start;
  while (cbi != m_block_end)
  {
#ifndef Y_BUILD_CONFIG_RELEASE
    SmallString disasm;
    DisassembleInstruction(&disasm, cbi->pc, cbi->instruction.bits, nullptr);
    Log_DebugPrintf("Compiling instruction '%s'", disasm.GetCharArray());
#endif

    if (!CompileInstruction(*cbi))
    {
      m_block_end = nullptr;
      m_block_start = nullptr;
      m_block = nullptr;
      return false;
    }

    cbi++;
  }

  BlockEpilogue();
  EmitEndBlock();

  FinalizeBlock(out_host_code, out_host_code_size);
  Log_ProfilePrintf("JIT block 0x%08X: %zu instructions (%u bytes), %u host bytes", block->GetPC(),
                    block->instructions.size(), block->GetSizeInBytes(), *out_host_code_size);

  DebugAssert(m_register_cache.GetUsedHostRegisters() == 0);

  m_block_end = nullptr;
  m_block_start = nullptr;
  m_block = nullptr;
  return true;
}

bool CodeGenerator::CompileInstruction(const CodeBlockInstruction& cbi)
{
  bool result;
  switch (cbi.instruction.op)
  {
#if 1
    case InstructionOp::ori:
    case InstructionOp::andi:
    case InstructionOp::xori:
      result = Compile_Bitwise(cbi);
      break;

    case InstructionOp::lb:
    case InstructionOp::lbu:
    case InstructionOp::lh:
    case InstructionOp::lhu:
    case InstructionOp::lw:
      result = Compile_Load(cbi);
      break;

    case InstructionOp::sb:
    case InstructionOp::sh:
    case InstructionOp::sw:
      result = Compile_Store(cbi);
      break;

    case InstructionOp::j:
    case InstructionOp::jal:
    case InstructionOp::b:
    case InstructionOp::beq:
    case InstructionOp::bne:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
      result = Compile_Branch(cbi);
      break;

    case InstructionOp::addi:
    case InstructionOp::addiu:
      result = Compile_Add(cbi);
      break;

    case InstructionOp::slti:
    case InstructionOp::sltiu:
      result = Compile_SetLess(cbi);
      break;

    case InstructionOp::lui:
      result = Compile_lui(cbi);
      break;

    case InstructionOp::cop0:
      result = Compile_cop0(cbi);
      break;

    case InstructionOp::cop2:
    case InstructionOp::lwc2:
    case InstructionOp::swc2:
      result = Compile_cop2(cbi);
      break;

    case InstructionOp::funct:
    {
      switch (cbi.instruction.r.funct)
      {
        case InstructionFunct::and_:
        case InstructionFunct::or_:
        case InstructionFunct::xor_:
        case InstructionFunct::nor:
          result = Compile_Bitwise(cbi);
          break;

        case InstructionFunct::sll:
        case InstructionFunct::srl:
        case InstructionFunct::sra:
        case InstructionFunct::sllv:
        case InstructionFunct::srlv:
        case InstructionFunct::srav:
          result = Compile_Shift(cbi);
          break;

        case InstructionFunct::mfhi:
        case InstructionFunct::mflo:
        case InstructionFunct::mthi:
        case InstructionFunct::mtlo:
          result = Compile_MoveHiLo(cbi);
          break;

        case InstructionFunct::add:
        case InstructionFunct::addu:
          result = Compile_Add(cbi);
          break;

        case InstructionFunct::sub:
        case InstructionFunct::subu:
          result = Compile_Subtract(cbi);
          break;

        case InstructionFunct::mult:
        case InstructionFunct::multu:
          result = Compile_Multiply(cbi);
          break;

        case InstructionFunct::slt:
        case InstructionFunct::sltu:
          result = Compile_SetLess(cbi);
          break;

        case InstructionFunct::jr:
        case InstructionFunct::jalr:
        case InstructionFunct::syscall:
        case InstructionFunct::break_:
          result = Compile_Branch(cbi);
          break;

        default:
          result = Compile_Fallback(cbi);
          break;
      }
    }
    break;
#endif

    default:
      result = Compile_Fallback(cbi);
      break;
  }

  return result;
}

Value CodeGenerator::ConvertValueSize(const Value& value, RegSize size, bool sign_extend)
{
  DebugAssert(value.size != size);

  if (value.IsConstant())
  {
    // compile-time conversion, woo!
    switch (size)
    {
      case RegSize_8:
        return Value::FromConstantU8(value.constant_value & 0xFF);

      case RegSize_16:
      {
        switch (value.size)
        {
          case RegSize_8:
            return Value::FromConstantU16(sign_extend ? SignExtend16(Truncate8(value.constant_value)) :
                                                        ZeroExtend16(Truncate8(value.constant_value)));

          default:
            return Value::FromConstantU16(value.constant_value & 0xFFFF);
        }
      }
      break;

      case RegSize_32:
      {
        switch (value.size)
        {
          case RegSize_8:
            return Value::FromConstantU32(sign_extend ? SignExtend32(Truncate8(value.constant_value)) :
                                                        ZeroExtend32(Truncate8(value.constant_value)));
          case RegSize_16:
            return Value::FromConstantU32(sign_extend ? SignExtend32(Truncate16(value.constant_value)) :
                                                        ZeroExtend32(Truncate16(value.constant_value)));

          case RegSize_32:
            return value;

          default:
            break;
        }
      }
      break;

      default:
        break;
    }

    UnreachableCode();
    return Value{};
  }

  Value new_value = m_register_cache.AllocateScratch(size);
  if (size < value.size)
  {
    EmitCopyValue(new_value.host_reg, value);
  }
  else
  {
    if (sign_extend)
      EmitSignExtend(new_value.host_reg, size, value.host_reg, value.size);
    else
      EmitZeroExtend(new_value.host_reg, size, value.host_reg, value.size);
  }

  return new_value;
}

void CodeGenerator::ConvertValueSizeInPlace(Value* value, RegSize size, bool sign_extend)
{
  DebugAssert(value->size != size);

  // We don't want to mess up the register cache value, so generate a new value if it's not scratch.
  if (value->IsConstant() || !value->IsScratch())
  {
    *value = ConvertValueSize(*value, size, sign_extend);
    return;
  }

  DebugAssert(value->IsInHostRegister() && value->IsScratch());

  // If the size is smaller and the value is in a register, we can just "view" the lower part.
  if (size < value->size)
  {
    value->size = size;
  }
  else
  {
    if (sign_extend)
      EmitSignExtend(value->host_reg, size, value->host_reg, value->size);
    else
      EmitZeroExtend(value->host_reg, size, value->host_reg, value->size);
  }

  value->size = size;
}

void* CodeGenerator::GetCurrentCodePointer() const
{
  if (m_emit == &m_near_emitter)
    return GetCurrentNearCodePointer();
  else if (m_emit == &m_far_emitter)
    return GetCurrentFarCodePointer();

  Panic("unknown emitter");
  return nullptr;
}

Value CodeGenerator::AddValues(const Value& lhs, const Value& rhs, bool set_flags)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant() && !set_flags)
  {
    // compile-time
    u64 new_cv = lhs.constant_value + rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  if (lhs.HasConstantValue(0) && !set_flags)
  {
    EmitCopyValue(res.host_reg, rhs);
    return res;
  }
  else if (rhs.HasConstantValue(0) && !set_flags)
  {
    EmitCopyValue(res.host_reg, lhs);
    return res;
  }
  else
  {
    if (lhs.IsInHostRegister())
    {
      EmitAdd(res.host_reg, lhs.host_reg, rhs, set_flags);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitAdd(res.host_reg, res.host_reg, rhs, set_flags);
    }
    return res;
  }
}

Value CodeGenerator::SubValues(const Value& lhs, const Value& rhs, bool set_flags)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant() && !set_flags)
  {
    // compile-time
    u64 new_cv = lhs.constant_value - rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  if (rhs.HasConstantValue(0) && !set_flags)
  {
    EmitCopyValue(res.host_reg, lhs);
    return res;
  }
  else
  {
    if (lhs.IsInHostRegister())
    {
      EmitSub(res.host_reg, lhs.host_reg, rhs, set_flags);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitSub(res.host_reg, res.host_reg, rhs, set_flags);
    }

    return res;
  }
}

std::pair<Value, Value> CodeGenerator::MulValues(const Value& lhs, const Value& rhs, bool signed_multiply)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    switch (lhs.size)
    {
      case RegSize_8:
      {
        u16 res;
        if (signed_multiply)
          res = u16(s16(s8(lhs.constant_value)) * s16(s8(rhs.constant_value)));
        else
          res = u16(u8(lhs.constant_value)) * u16(u8(rhs.constant_value));

        return std::make_pair(Value::FromConstantU8(Truncate8(res >> 8)), Value::FromConstantU8(Truncate8(res)));
      }

      case RegSize_16:
      {
        u32 res;
        if (signed_multiply)
          res = u32(s32(s16(lhs.constant_value)) * s32(s16(rhs.constant_value)));
        else
          res = u32(u16(lhs.constant_value)) * u32(u16(rhs.constant_value));

        return std::make_pair(Value::FromConstantU16(Truncate16(res >> 16)), Value::FromConstantU16(Truncate16(res)));
      }

      case RegSize_32:
      {
        u64 res;
        if (signed_multiply)
          res = u64(s64(s32(lhs.constant_value)) * s64(s32(rhs.constant_value)));
        else
          res = u64(u32(lhs.constant_value)) * u64(u32(rhs.constant_value));

        return std::make_pair(Value::FromConstantU32(Truncate32(res >> 32)), Value::FromConstantU32(Truncate32(res)));
      }
      break;

      case RegSize_64:
      {
        u64 res;
        if (signed_multiply)
          res = u64(s64(lhs.constant_value) * s64(rhs.constant_value));
        else
          res = lhs.constant_value * rhs.constant_value;

        // TODO: 128-bit multiply...
        Panic("128-bit multiply");
        return std::make_pair(Value::FromConstantU64(0), Value::FromConstantU64(res));
      }

      default:
        return std::make_pair(Value::FromConstantU64(0), Value::FromConstantU64(0));
    }
  }

  // We need two registers for both components.
  Value hi = m_register_cache.AllocateScratch(lhs.size);
  Value lo = m_register_cache.AllocateScratch(lhs.size);
  EmitMul(hi.host_reg, lo.host_reg, lhs, rhs, signed_multiply);
  return std::make_pair(std::move(hi), std::move(lo));
}

Value CodeGenerator::ShlValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value << (rhs.constant_value & 0x1F);
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  if (rhs.HasConstantValue(0))
  {
    EmitCopyValue(res.host_reg, lhs);
  }
  else
  {
    if (lhs.IsInHostRegister())
    {
      EmitShl(res.host_reg, lhs.host_reg, res.size, rhs);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitShl(res.host_reg, res.host_reg, res.size, rhs);
    }
  }
  return res;
}

Value CodeGenerator::ShrValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value >> (rhs.constant_value & 0x1F);
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  if (rhs.HasConstantValue(0))
  {
    EmitCopyValue(res.host_reg, lhs);
  }
  else
  {
    if (lhs.IsInHostRegister())
    {
      EmitShr(res.host_reg, lhs.host_reg, res.size, rhs);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitShr(res.host_reg, res.host_reg, res.size, rhs);
    }
  }
  return res;
}

Value CodeGenerator::SarValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(
          static_cast<u8>(static_cast<s8>(Truncate8(lhs.constant_value)) >> (rhs.constant_value & 0x1F)));

      case RegSize_16:
        return Value::FromConstantU16(
          static_cast<u16>(static_cast<s16>(Truncate16(lhs.constant_value)) >> (rhs.constant_value & 0x1F)));

      case RegSize_32:
        return Value::FromConstantU32(
          static_cast<u32>(static_cast<s32>(Truncate32(lhs.constant_value)) >> (rhs.constant_value & 0x1F)));

      case RegSize_64:
        return Value::FromConstantU64(
          static_cast<u64>(static_cast<s64>(lhs.constant_value) >> (rhs.constant_value & 0x3F)));

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  if (rhs.HasConstantValue(0))
  {
    EmitCopyValue(res.host_reg, lhs);
  }
  else
  {
    if (lhs.IsInHostRegister())
    {
      EmitSar(res.host_reg, lhs.host_reg, res.size, rhs);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitSar(res.host_reg, res.host_reg, res.size, rhs);
    }
  }
  return res;
}

Value CodeGenerator::OrValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value | rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  if (lhs.HasConstantValue(0))
  {
    EmitCopyValue(res.host_reg, rhs);
    return res;
  }
  else if (rhs.HasConstantValue(0))
  {
    EmitCopyValue(res.host_reg, lhs);
    return res;
  }

  if (lhs.IsInHostRegister())
  {
    EmitOr(res.host_reg, lhs.host_reg, rhs);
  }
  else
  {
    EmitCopyValue(res.host_reg, lhs);
    EmitOr(res.host_reg, res.host_reg, rhs);
  }
  return res;
}

Value CodeGenerator::AndValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value & rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  // TODO: and with -1 -> noop
  Value res = m_register_cache.AllocateScratch(lhs.size);
  if (lhs.HasConstantValue(0) || rhs.HasConstantValue(0))
  {
    EmitXor(res.host_reg, res.host_reg, res);
    return res;
  }

  if (lhs.IsInHostRegister())
  {
    EmitAnd(res.host_reg, lhs.host_reg, rhs);
  }
  else
  {
    EmitCopyValue(res.host_reg, lhs);
    EmitAnd(res.host_reg, res.host_reg, rhs);
  }
  return res;
}

Value CodeGenerator::XorValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value ^ rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  Value res = m_register_cache.AllocateScratch(lhs.size);
  EmitCopyValue(res.host_reg, lhs);
  if (lhs.HasConstantValue(0))
  {
    EmitCopyValue(res.host_reg, rhs);
    return res;
  }
  else if (rhs.HasConstantValue(0))
  {
    EmitCopyValue(res.host_reg, lhs);
    return res;
  }

  if (lhs.IsInHostRegister())
  {
    EmitXor(res.host_reg, lhs.host_reg, rhs);

  }
  else
  {
    EmitCopyValue(res.host_reg, lhs);
    EmitXor(res.host_reg, res.host_reg, rhs);
  }

  return res;
}

Value CodeGenerator::NotValue(const Value& val)
{
  if (val.IsConstant())
  {
    u64 new_cv = ~val.constant_value;
    switch (val.size)
    {
      case RegSize_8:
        return Value::FromConstantU8(Truncate8(new_cv));

      case RegSize_16:
        return Value::FromConstantU16(Truncate16(new_cv));

      case RegSize_32:
        return Value::FromConstantU32(Truncate32(new_cv));

      case RegSize_64:
        return Value::FromConstantU64(new_cv);

      default:
        return Value();
    }
  }

  // TODO: Don't allocate scratch if the lhs is a scratch?
  Value res = m_register_cache.AllocateScratch(RegSize_32);
  EmitCopyValue(res.host_reg, val);
  EmitNot(res.host_reg, val.size);
  return res;
}

void CodeGenerator::BlockPrologue()
{
  EmitStoreCPUStructField(offsetof(Core, m_exception_raised), Value::FromConstantU8(0));

  // we don't know the state of the last block, so assume load delays might be in progress
  // TODO: Pull load delay into register cache
  m_current_instruction_in_branch_delay_slot_dirty = true;
  m_branch_was_taken_dirty = true;
  m_current_instruction_was_branch_taken_dirty = false;
  m_load_delay_dirty = true;
}

void CodeGenerator::BlockEpilogue()
{
#if defined(_DEBUG) && defined(Y_CPU_X64)
  m_emit->nop();
#endif

  m_register_cache.FlushAllGuestRegisters(true, true);
  if (m_register_cache.HasLoadDelay())
    m_register_cache.WriteLoadDelayToCPU(true);

  AddPendingCycles();
}

void CodeGenerator::InstructionPrologue(const CodeBlockInstruction& cbi, TickCount cycles,
                                        bool force_sync /* = false */)
{
#if defined(_DEBUG) && defined(Y_CPU_X64)
  m_emit->nop();
#endif

  // reset dirty flags
  if (m_branch_was_taken_dirty)
  {
    Value temp = m_register_cache.AllocateScratch(RegSize_8);
    EmitLoadCPUStructField(temp.host_reg, RegSize_8, offsetof(Core, m_branch_was_taken));
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_was_branch_taken), temp);
    EmitStoreCPUStructField(offsetof(Core, m_branch_was_taken), Value::FromConstantU8(0));
    m_current_instruction_was_branch_taken_dirty = true;
    m_branch_was_taken_dirty = false;
  }
  else if (m_current_instruction_was_branch_taken_dirty)
  {
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_was_branch_taken), Value::FromConstantU8(0));
    m_current_instruction_was_branch_taken_dirty = false;
  }

  if (m_current_instruction_in_branch_delay_slot_dirty && !cbi.is_branch_delay_slot)
  {
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_in_branch_delay_slot), Value::FromConstantU8(0));
    m_current_instruction_in_branch_delay_slot_dirty = false;
  }

  // increment PC, except if we're in the branch delay slot where it was just changed
  if (!cbi.is_branch_delay_slot)
  {
    Assert(!m_register_cache.IsGuestRegisterInHostRegister(Reg::pc));
    m_register_cache.WriteGuestRegister(Reg::pc, Value::FromConstantU32(cbi.pc + 4));
  }

  if (!CanInstructionTrap(cbi.instruction, m_block->key.user_mode) && !force_sync)
  {
    // Defer updates for non-faulting instructions.
    m_delayed_cycles_add += cycles;
    return;
  }

  if (cbi.is_branch_delay_slot)
  {
    // m_current_instruction_in_branch_delay_slot = true
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_in_branch_delay_slot), Value::FromConstantU8(1));
    m_current_instruction_in_branch_delay_slot_dirty = true;
  }

  // Sync current instruction PC
  EmitStoreCPUStructField(offsetof(Core, m_current_instruction_pc), Value::FromConstantU32(cbi.pc));

  m_delayed_cycles_add += cycles;
  AddPendingCycles();
}

void CodeGenerator::InstructionEpilogue(const CodeBlockInstruction& cbi)
{
  m_register_cache.UpdateLoadDelay();

  if (m_load_delay_dirty)
  {
    // we have to invalidate the register cache, since the load delayed register might've been cached
    Log_DebugPrint("Emitting delay slot flush");
    EmitFlushInterpreterLoadDelay();
    m_register_cache.InvalidateAllNonDirtyGuestRegisters();
    m_load_delay_dirty = false;
  }

  // copy if the previous instruction was a load, reset the current value on the next instruction
  if (m_next_load_delay_dirty)
  {
    Log_DebugPrint("Emitting delay slot flush (with move next)");
    EmitMoveNextInterpreterLoadDelay();
    m_next_load_delay_dirty = false;
    m_load_delay_dirty = true;
  }
}

void CodeGenerator::AddPendingCycles()
{
  if (m_delayed_cycles_add == 0)
    return;

  EmitAddCPUStructField(offsetof(Core, m_pending_ticks), Value::FromConstantU32(m_delayed_cycles_add));
  m_delayed_cycles_add = 0;
}

bool CodeGenerator::Compile_Fallback(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1, true);

  // flush and invalidate all guest registers, since the fallback could change any of them
  m_register_cache.FlushAllGuestRegisters(true, true);
  if (m_register_cache.HasLoadDelay())
  {
    m_load_delay_dirty = true;
    m_register_cache.WriteLoadDelayToCPU(true);
  }

  EmitStoreCPUStructField(offsetof(Core, m_current_instruction.bits), Value::FromConstantU32(cbi.instruction.bits));

  // emit the function call
  if (CanInstructionTrap(cbi.instruction, m_block->key.user_mode))
  {
    // TODO: Use carry flag or something here too
    Value return_value = m_register_cache.AllocateScratch(RegSize_8);
    EmitFunctionCall(&return_value, &Thunks::InterpretInstruction, m_register_cache.GetCPUPtr());
    EmitExceptionExitOnBool(return_value);
  }
  else
  {
    EmitFunctionCall(nullptr, &Thunks::InterpretInstruction, m_register_cache.GetCPUPtr());
  }

  m_current_instruction_in_branch_delay_slot_dirty = cbi.is_branch_instruction;
  m_branch_was_taken_dirty = cbi.is_branch_instruction;
  m_next_load_delay_dirty = cbi.has_load_delay;
  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Bitwise(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const InstructionOp op = cbi.instruction.op;
  const InstructionFunct funct = cbi.instruction.r.funct;
  Value lhs;
  Value rhs;
  Reg dest;
  if (op != InstructionOp::funct)
  {
    // rt <- rs op zext(imm)
    lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
    rhs = Value::FromConstantU32(cbi.instruction.i.imm_zext32());
    dest = cbi.instruction.i.rt;
  }
  else
  {
    lhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
    rhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
    dest = cbi.instruction.r.rd;
  }

  Value result;
  switch (cbi.instruction.op)
  {
    case InstructionOp::ori:
      result = OrValues(lhs, rhs);
      break;

    case InstructionOp::andi:
      result = AndValues(lhs, rhs);
      break;

    case InstructionOp::xori:
      result = XorValues(lhs, rhs);
      break;

    case InstructionOp::funct:
    {
      switch (cbi.instruction.r.funct)
      {
        case InstructionFunct::or_:
          result = OrValues(lhs, rhs);
          break;

        case InstructionFunct::and_:
          result = AndValues(lhs, rhs);
          break;

        case InstructionFunct::xor_:
          result = XorValues(lhs, rhs);
          break;

        case InstructionFunct::nor:
          result = NotValue(OrValues(lhs, rhs));
          break;

        default:
          UnreachableCode();
          break;
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  m_register_cache.WriteGuestRegister(dest, std::move(result));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Shift(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const InstructionFunct funct = cbi.instruction.r.funct;
  Value rt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
  Value shamt;
  if (funct == InstructionFunct::sll || funct == InstructionFunct::srl || funct == InstructionFunct::sra)
  {
    // rd <- rt op shamt
    shamt = Value::FromConstantU32(cbi.instruction.r.shamt);
  }
  else
  {
    // rd <- rt op (rs & 0x1F)
    shamt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
    if constexpr (!SHIFTS_ARE_IMPLICITLY_MASKED)
      EmitAnd(shamt.host_reg, shamt.host_reg, Value::FromConstantU32(0x1F));
  }

  Value result;
  switch (cbi.instruction.r.funct)
  {
    case InstructionFunct::sll:
    case InstructionFunct::sllv:
      result = ShlValues(rt, shamt);
      break;

    case InstructionFunct::srl:
    case InstructionFunct::srlv:
      result = ShrValues(rt, shamt);
      break;

    case InstructionFunct::sra:
    case InstructionFunct::srav:
      result = SarValues(rt, shamt);
      break;

    default:
      UnreachableCode();
      break;
  }

  m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, std::move(result));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Load(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // rt <- mem[rs + sext(imm)]
  Value base = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
  Value offset = Value::FromConstantU32(cbi.instruction.i.imm_sext32());
  Value address = AddValues(base, offset, false);

  Value result;
  switch (cbi.instruction.op)
  {
    case InstructionOp::lb:
    case InstructionOp::lbu:
      result = EmitLoadGuestMemory(address, RegSize_8);
      ConvertValueSizeInPlace(&result, RegSize_32, (cbi.instruction.op == InstructionOp::lb));
      break;

    case InstructionOp::lh:
    case InstructionOp::lhu:
      result = EmitLoadGuestMemory(address, RegSize_16);
      ConvertValueSizeInPlace(&result, RegSize_32, (cbi.instruction.op == InstructionOp::lh));
      break;

    case InstructionOp::lw:
      result = EmitLoadGuestMemory(address, RegSize_32);
      break;

    default:
      UnreachableCode();
      break;
  }

  m_register_cache.WriteGuestRegisterDelayed(cbi.instruction.i.rt, std::move(result));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Store(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // mem[rs + sext(imm)] <- rt
  Value base = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
  Value offset = Value::FromConstantU32(cbi.instruction.i.imm_sext32());
  Value address = AddValues(base, offset, false);
  Value value = m_register_cache.ReadGuestRegister(cbi.instruction.i.rt);

  switch (cbi.instruction.op)
  {
    case InstructionOp::sb:
      EmitStoreGuestMemory(address, value.ViewAsSize(RegSize_8));
      break;

    case InstructionOp::sh:
      EmitStoreGuestMemory(address, value.ViewAsSize(RegSize_16));
      break;

    case InstructionOp::sw:
      EmitStoreGuestMemory(address, value);
      break;

    default:
      UnreachableCode();
      break;
  }

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_MoveHiLo(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  switch (cbi.instruction.r.funct)
  {
    case InstructionFunct::mfhi:
      m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, m_register_cache.ReadGuestRegister(Reg::hi));
      break;

    case InstructionFunct::mthi:
      m_register_cache.WriteGuestRegister(Reg::hi, m_register_cache.ReadGuestRegister(cbi.instruction.r.rs));
      break;

    case InstructionFunct::mflo:
      m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, m_register_cache.ReadGuestRegister(Reg::lo));
      break;

    case InstructionFunct::mtlo:
      m_register_cache.WriteGuestRegister(Reg::lo, m_register_cache.ReadGuestRegister(cbi.instruction.r.rs));
      break;

    default:
      UnreachableCode();
      break;
  }

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Add(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const bool check_overflow =
    (cbi.instruction.op == InstructionOp::addi ||
     (cbi.instruction.op == InstructionOp::funct && cbi.instruction.r.funct == InstructionFunct::add));

  Value lhs, rhs;
  Reg dest;
  switch (cbi.instruction.op)
  {
    case InstructionOp::addi:
    case InstructionOp::addiu:
    {
      // rt <- rs + sext(imm)
      dest = cbi.instruction.i.rt;
      lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
      rhs = Value::FromConstantU32(cbi.instruction.i.imm_sext32());
    }
    break;

    case InstructionOp::funct:
    {
      Assert(cbi.instruction.r.funct == InstructionFunct::add || cbi.instruction.r.funct == InstructionFunct::addu);
      dest = cbi.instruction.r.rd;
      lhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
      rhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
    }
    break;

    default:
      UnreachableCode();
      return false;
  }

  Value result = AddValues(lhs, rhs, check_overflow);
  if (check_overflow)
    EmitRaiseException(Exception::Ov, Condition::Overflow);

  m_register_cache.WriteGuestRegister(dest, std::move(result));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Subtract(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  Assert(cbi.instruction.op == InstructionOp::funct);
  const bool check_overflow = (cbi.instruction.r.funct == InstructionFunct::sub);

  Value lhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
  Value rhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);

  Value result = SubValues(lhs, rhs, check_overflow);
  if (check_overflow)
    EmitRaiseException(Exception::Ov, Condition::Overflow);

  m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, std::move(result));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Multiply(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const bool signed_multiply = (cbi.instruction.r.funct == InstructionFunct::mult);
  std::pair<Value, Value> result = MulValues(m_register_cache.ReadGuestRegister(cbi.instruction.r.rs),
                                             m_register_cache.ReadGuestRegister(cbi.instruction.r.rt), signed_multiply);
  m_register_cache.WriteGuestRegister(Reg::hi, std::move(result.first));
  m_register_cache.WriteGuestRegister(Reg::lo, std::move(result.second));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_SetLess(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const bool signed_comparison =
    (cbi.instruction.op == InstructionOp::slti ||
     (cbi.instruction.op == InstructionOp::funct && cbi.instruction.r.funct == InstructionFunct::slt));

  Reg dest;
  Value lhs, rhs;
  if (cbi.instruction.op == InstructionOp::slti || cbi.instruction.op == InstructionOp::sltiu)
  {
    // rt <- rs < {z,s}ext(imm)
    dest = cbi.instruction.i.rt;
    lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);
    rhs = Value::FromConstantU32(cbi.instruction.i.imm_sext32());

    // flush the old value which might free up a register
    if (dest != cbi.instruction.r.rs)
      m_register_cache.InvalidateGuestRegister(dest);
  }
  else
  {
    // rd <- rs < rt
    dest = cbi.instruction.r.rd;
    lhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs, true, true);
    rhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);

    // flush the old value which might free up a register
    if (dest != cbi.instruction.i.rs && dest != cbi.instruction.r.rt)
      m_register_cache.InvalidateGuestRegister(dest);
  }

  Value result = m_register_cache.AllocateScratch(RegSize_32);
  EmitCmp(lhs.host_reg, rhs);
  EmitSetConditionResult(result.host_reg, result.size, signed_comparison ? Condition::Less : Condition::Below);
  m_register_cache.WriteGuestRegister(dest, std::move(result));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Branch(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // Compute the branch target.
  // This depends on the form of the instruction.
  switch (cbi.instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
    {
      // npc = (pc & 0xF0000000) | (target << 2)
      Value branch_target =
        OrValues(AndValues(m_register_cache.ReadGuestRegister(Reg::pc), Value::FromConstantU32(0xF0000000)),
                 Value::FromConstantU32(cbi.instruction.j.target << 2));

      EmitBranch(Condition::Always, (cbi.instruction.op == InstructionOp::jal) ? Reg::ra : Reg::count,
                 std::move(branch_target));
    }
    break;

    case InstructionOp::funct:
    {
      if (cbi.instruction.r.funct == InstructionFunct::jr || cbi.instruction.r.funct == InstructionFunct::jalr)
      {
        // npc = rs, link to rt
        Value branch_target = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
        EmitBranch(Condition::Always,
                   (cbi.instruction.r.funct == InstructionFunct::jalr) ? cbi.instruction.r.rd : Reg::count,
                   std::move(branch_target));
      }
      else if (cbi.instruction.r.funct == InstructionFunct::syscall ||
               cbi.instruction.r.funct == InstructionFunct::break_)
      {
        const Exception excode =
          (cbi.instruction.r.funct == InstructionFunct::syscall) ? Exception::Syscall : Exception::BP;
        EmitRaiseException(excode);
      }
      else
      {
        UnreachableCode();
      }
    }
    break;

    case InstructionOp::beq:
    case InstructionOp::bne:
    {
      // npc = pc + (sext(imm) << 2)
      Value branch_target = AddValues(m_register_cache.ReadGuestRegister(Reg::pc),
                                      Value::FromConstantU32(cbi.instruction.i.imm_sext32() << 2), false);

      // branch <- rs op rt
      Value lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);
      Value rhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rt);
      EmitCmp(lhs.host_reg, rhs);

      const Condition condition = (cbi.instruction.op == InstructionOp::beq) ? Condition::Equal : Condition::NotEqual;
      EmitBranch(condition, Reg::count, std::move(branch_target));
    }
    break;

    case InstructionOp::bgtz:
    case InstructionOp::blez:
    {
      // npc = pc + (sext(imm) << 2)
      Value branch_target = AddValues(m_register_cache.ReadGuestRegister(Reg::pc),
                                      Value::FromConstantU32(cbi.instruction.i.imm_sext32() << 2), false);

      // branch <- rs op 0
      Value lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);
      EmitCmp(lhs.host_reg, Value::FromConstantU32(0));

      const Condition condition =
        (cbi.instruction.op == InstructionOp::bgtz) ? Condition::Greater : Condition::LessEqual;
      EmitBranch(condition, Reg::count, std::move(branch_target));
    }
    break;

    case InstructionOp::b:
    {
      // npc = pc + (sext(imm) << 2)
      Value branch_target = AddValues(m_register_cache.ReadGuestRegister(Reg::pc),
                                      Value::FromConstantU32(cbi.instruction.i.imm_sext32() << 2), false);

      const u8 rt = static_cast<u8>(cbi.instruction.i.rt.GetValue());
      const bool bgez = ConvertToBoolUnchecked(rt & u8(1));
      const Condition condition = bgez ? Condition::PositiveOrZero : Condition::Negative;
      const bool link = (rt & u8(0x1E)) == u8(0x10);

      // Read has to happen before the link as the compare can use ra.
      // This is a little dangerous since lhs can get freed, but there aren't any allocations inbetween here and the
      // test so it shouldn't be an issue.
      Value lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);

      // The return address is always written if link is set, regardless of whether the branch is taken.
      if (link)
      {
        EmitCancelInterpreterLoadDelayForReg(Reg::ra);
        m_register_cache.WriteGuestRegister(
          Reg::ra, AddValues(m_register_cache.ReadGuestRegister(Reg::pc), Value::FromConstantU32(4), false));
      }

      EmitTest(lhs.host_reg, lhs);
      EmitBranch(condition, Reg::count, std::move(branch_target));
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_lui(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // rt <- (imm << 16)
  m_register_cache.WriteGuestRegister(cbi.instruction.i.rt,
                                      Value::FromConstantU32(cbi.instruction.i.imm_zext32() << 16));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_cop0(const CodeBlockInstruction& cbi)
{
  if (cbi.instruction.cop.IsCommonInstruction())
  {
    switch (cbi.instruction.cop.CommonOp())
    {
      case CopCommonInstruction::mfcn:
      case CopCommonInstruction::mtcn:
      {
        u32 offset;
        u32 write_mask = UINT32_C(0xFFFFFFFF);
        switch (static_cast<Cop0Reg>(cbi.instruction.r.rd.GetValue()))
        {
          case Cop0Reg::BPC:
            offset = offsetof(Core, m_cop0_regs.BPC);
            break;

          case Cop0Reg::BPCM:
            offset = offsetof(Core, m_cop0_regs.BPCM);
            break;

          case Cop0Reg::BDA:
            offset = offsetof(Core, m_cop0_regs.BDA);
            break;

          case Cop0Reg::BDAM:
            offset = offsetof(Core, m_cop0_regs.BDAM);
            break;

          case Cop0Reg::DCIC:
            offset = offsetof(Core, m_cop0_regs.dcic.bits);
            write_mask = Cop0Registers::DCIC::WRITE_MASK;
            break;

          case Cop0Reg::JUMPDEST:
            offset = offsetof(Core, m_cop0_regs.TAR);
            write_mask = 0;
            break;

          case Cop0Reg::BadVaddr:
            offset = offsetof(Core, m_cop0_regs.BadVaddr);
            write_mask = 0;
            break;

          case Cop0Reg::SR:
            offset = offsetof(Core, m_cop0_regs.sr.bits);
            write_mask = Cop0Registers::SR::WRITE_MASK;
            break;

          case Cop0Reg::CAUSE:
            offset = offsetof(Core, m_cop0_regs.cause.bits);
            write_mask = Cop0Registers::CAUSE::WRITE_MASK;
            break;

          case Cop0Reg::EPC:
            offset = offsetof(Core, m_cop0_regs.EPC);
            write_mask = 0;
            break;

          case Cop0Reg::PRID:
            offset = offsetof(Core, m_cop0_regs.PRID);
            write_mask = 0;
            break;

          default:
            return Compile_Fallback(cbi);
        }

        InstructionPrologue(cbi, 1);

        if (cbi.instruction.cop.CommonOp() == CopCommonInstruction::mfcn)
        {
          // coprocessor loads are load-delayed
          Value value = m_register_cache.AllocateScratch(RegSize_32);
          EmitLoadCPUStructField(value.host_reg, value.size, offset);
          m_register_cache.WriteGuestRegisterDelayed(cbi.instruction.r.rt, std::move(value));
        }
        else
        {
          // some registers are not writable, so ignore those
          if (write_mask != 0)
          {
            Value value = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
            if (write_mask != UINT32_C(0xFFFFFFFF))
            {
              // need to adjust the mask
              value = AndValues(value, Value::FromConstantU32(write_mask));
            }

            EmitStoreCPUStructField(offset, value);
          }
        }

        InstructionEpilogue(cbi);
        return true;
      }

      // only mfc/mtc for cop0
      default:
        return Compile_Fallback(cbi);
    }
  }
  else
  {
    switch (cbi.instruction.cop.Cop0Op())
    {
      case Cop0Instruction::rfe:
      {
        InstructionPrologue(cbi, 1);

        // shift mode bits right two, preserving upper bits
        static constexpr u32 mode_bits_mask = UINT32_C(0b1111);
        Value sr = m_register_cache.AllocateScratch(RegSize_32);
        EmitLoadCPUStructField(sr.host_reg, RegSize_32, offsetof(Core, m_cop0_regs.sr.bits));
        {
          Value new_mode_bits = m_register_cache.AllocateScratch(RegSize_32);
          EmitShr(new_mode_bits.host_reg, sr.host_reg, new_mode_bits.size, Value::FromConstantU32(2));
          EmitAnd(new_mode_bits.host_reg, new_mode_bits.host_reg, Value::FromConstantU32(mode_bits_mask));
          EmitAnd(sr.host_reg, sr.host_reg, Value::FromConstantU32(~mode_bits_mask));
          EmitOr(sr.host_reg, sr.host_reg, new_mode_bits);
        }

        EmitStoreCPUStructField(offsetof(Core, m_cop0_regs.sr.bits), sr);

        InstructionEpilogue(cbi);
        return true;
      }

      default:
        return Compile_Fallback(cbi);
    }
  }
}

Value CodeGenerator::DoGTERegisterRead(u32 index)
{
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  // Most GTE registers can be read directly. Handle the special cases here.
  if (index == 15) // SXY3
  {
    // mirror of SXY2
    index = 14;
  }

  switch (index)
  {
    case 28: // IRGB
    case 29: // ORGB
    {
      EmitFunctionCall(&value, &Thunks::ReadGTERegister, m_register_cache.GetCPUPtr(), Value::FromConstantU32(index));
    }
    break;

    default:
    {
      EmitLoadCPUStructField(value.host_reg, RegSize_32, offsetof(Core, m_cop2.m_regs.r32[index]));
    }
    break;
  }

  return value;
}

void CodeGenerator::DoGTERegisterWrite(u32 index, const Value& value)
{
  switch (index)
  {
    case 1:  // V0[z]
    case 3:  // V1[z]
    case 5:  // V2[z]
    case 8:  // IR0
    case 9:  // IR1
    case 10: // IR2
    case 11: // IR3
    case 36: // RT33
    case 44: // L33
    case 52: // LR33
    case 58: // H       - sign-extended on read but zext on use
    case 59: // DQA
    case 61: // ZSF3
    case 62: // ZSF4
    {
      // sign-extend z component of vector registers
      Value temp = ConvertValueSize(value.ViewAsSize(RegSize_16), RegSize_32, true);
      EmitStoreCPUStructField(offsetof(Core, m_cop2.m_regs.r32[index]), temp);
      return;
    }
    break;

    case 7:  // OTZ
    case 16: // SZ0
    case 17: // SZ1
    case 18: // SZ2
    case 19: // SZ3
    {
      // zero-extend unsigned values
      Value temp = ConvertValueSize(value.ViewAsSize(RegSize_16), RegSize_32, false);
      EmitStoreCPUStructField(offsetof(Core, m_cop2.m_regs.r32[index]), temp);
      return;
    }
    break;

    case 15: // SXY3
    {
      // writing to SXYP pushes to the FIFO
      Value temp = m_register_cache.AllocateScratch(RegSize_32);

      // SXY0 <- SXY1
      EmitLoadCPUStructField(temp.host_reg, RegSize_32, offsetof(Core, m_cop2.m_regs.r32[13]));
      EmitStoreCPUStructField(offsetof(Core, m_cop2.m_regs.r32[12]), temp);

      // SXY1 <- SXY2
      EmitLoadCPUStructField(temp.host_reg, RegSize_32, offsetof(Core, m_cop2.m_regs.r32[14]));
      EmitStoreCPUStructField(offsetof(Core, m_cop2.m_regs.r32[13]), temp);

      // SXY2 <- SXYP
      EmitStoreCPUStructField(offsetof(Core, m_cop2.m_regs.r32[14]), value);
      return;
    }
    break;

    case 28: // IRGB
    case 30: // LZCS
    case 63: // FLAG
    {
      EmitFunctionCall(nullptr, &Thunks::WriteGTERegister, m_register_cache.GetCPUPtr(), Value::FromConstantU32(index),
                       value);
      return;
    }

    case 29: // ORGB
    case 31: // LZCR
    {
      // read-only registers
      return;
    }

    default:
    {
      // written as-is, 2x16 or 1x32 bits
      EmitStoreCPUStructField(offsetof(Core, m_cop2.m_regs.r32[index]), value);
      return;
    }
  }
}

bool CodeGenerator::Compile_cop2(const CodeBlockInstruction& cbi)
{
  if (cbi.instruction.op == InstructionOp::lwc2 || cbi.instruction.op == InstructionOp::swc2)
  {
    InstructionPrologue(cbi, 1);

    const u32 reg = static_cast<u32>(cbi.instruction.i.rt.GetValue());
    Value address = AddValues(m_register_cache.ReadGuestRegister(cbi.instruction.i.rs),
                              Value::FromConstantU32(cbi.instruction.i.imm_sext32()), false);
    if (cbi.instruction.op == InstructionOp::lwc2)
    {
      Value value = EmitLoadGuestMemory(address, RegSize_32);
      DoGTERegisterWrite(reg, value);
    }
    else
    {
      Value value = DoGTERegisterRead(reg);
      EmitStoreGuestMemory(address, value);
    }

    InstructionEpilogue(cbi);
    return true;
  }

  Assert(cbi.instruction.op == InstructionOp::cop2);

  if (cbi.instruction.cop.IsCommonInstruction())
  {
    switch (cbi.instruction.cop.CommonOp())
    {
      case CopCommonInstruction::mfcn:
      case CopCommonInstruction::cfcn:
      {
        const u32 reg = static_cast<u32>(cbi.instruction.r.rd.GetValue()) +
                        ((cbi.instruction.cop.CommonOp() == CopCommonInstruction::cfcn) ? 32 : 0);

        InstructionPrologue(cbi, 1);
        m_register_cache.WriteGuestRegisterDelayed(cbi.instruction.r.rt, DoGTERegisterRead(reg));
        InstructionEpilogue(cbi);
        return true;
      }

      case CopCommonInstruction::mtcn:
      case CopCommonInstruction::ctcn:
      {
        const u32 reg = static_cast<u32>(cbi.instruction.r.rd.GetValue()) +
                        ((cbi.instruction.cop.CommonOp() == CopCommonInstruction::ctcn) ? 32 : 0);

        InstructionPrologue(cbi, 1);
        DoGTERegisterWrite(reg, m_register_cache.ReadGuestRegister(cbi.instruction.r.rt));
        InstructionEpilogue(cbi);
        return true;
      }

      default:
        return Compile_Fallback(cbi);
    }
  }
  else
  {
    // forward everything to the GTE.
    InstructionPrologue(cbi, 1);

    Value instruction_bits = Value::FromConstantU32(cbi.instruction.bits & GTE::Instruction::REQUIRED_BITS_MASK);
    EmitFunctionCall(nullptr, &Thunks::ExecuteGTEInstruction, m_register_cache.GetCPUPtr(), instruction_bits);

    InstructionEpilogue(cbi);
    return true;
  }
}
} // namespace CPU::Recompiler
