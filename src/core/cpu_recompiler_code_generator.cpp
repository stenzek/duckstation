#include "cpu_recompiler_code_generator.h"
#include "common/log.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "gte.h"
#include "pgxp.h"
#include "settings.h"
Log_SetChannel(CPU::Recompiler);

// TODO: Turn load+sext/zext into a single signed/unsigned load
// TODO: mulx/shlx/etc
// TODO: when writing to the same register, don't allocate a temporary and copy it (mainly for shifts)

namespace CPU::Recompiler {

bool CodeGenerator::CompileBlock(CodeBlock* block, CodeBlock::HostCodePointer* out_host_code, u32* out_host_code_size)
{
  // TODO: Align code buffer.

  m_block = block;
  m_block_start = block->instructions.data();
  m_block_end = block->instructions.data() + block->instructions.size();

  m_pc = block->GetPC();
  m_pc_valid = true;

  m_fastmem_load_base_in_register = false;
  m_fastmem_store_base_in_register = false;

  EmitBeginBlock(true);
  BlockPrologue();

  m_current_instruction = m_block_start;
  while (m_current_instruction != m_block_end)
  {
    if (!CompileInstruction(*m_current_instruction))
    {
      m_current_instruction = nullptr;
      m_block_end = nullptr;
      m_block_start = nullptr;
      m_block = nullptr;
      return false;
    }

    m_current_instruction++;
  }

  if (!m_block_linked)
  {
    BlockEpilogue();
    EmitEndBlock(true, true);
  }

  FinalizeBlock(out_host_code, out_host_code_size);
  Log_ProfilePrintf("JIT block 0x%08X: %zu instructions (%u bytes), %u host bytes", block->GetPC(),
                    block->instructions.size(), block->GetSizeInBytes(), *out_host_code_size);

  DebugAssert(m_register_cache.GetUsedHostRegisters() == 0);

  m_current_instruction = nullptr;
  m_block_end = nullptr;
  m_block_start = nullptr;
  m_block = nullptr;
  return true;
}

bool CodeGenerator::CompileInstruction(const CodeBlockInstruction& cbi)
{
  if (IsNopInstruction(cbi.instruction))
  {
    InstructionPrologue(cbi, 1);
    InstructionEpilogue(cbi);
    return true;
  }

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

    case InstructionOp::lwl:
    case InstructionOp::lwr:
      result = Compile_LoadLeftRight(cbi);
      break;

    case InstructionOp::swl:
    case InstructionOp::swr:
      result = Compile_StoreLeftRight(cbi);
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

        case InstructionFunct::div:
          result = Compile_SignedDivide(cbi);
          break;

        case InstructionFunct::divu:
          result = Compile_Divide(cbi);
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

Value CodeGenerator::ShlValues(const Value& lhs, const Value& rhs, bool assume_amount_masked /* = true */)
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
      EmitShl(res.host_reg, lhs.host_reg, res.size, rhs, assume_amount_masked);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitShl(res.host_reg, res.host_reg, res.size, rhs, assume_amount_masked);
    }
  }
  return res;
}

Value CodeGenerator::ShrValues(const Value& lhs, const Value& rhs, bool assume_amount_masked /* = true */)
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
      EmitShr(res.host_reg, lhs.host_reg, res.size, rhs, assume_amount_masked);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitShr(res.host_reg, res.host_reg, res.size, rhs, assume_amount_masked);
    }
  }
  return res;
}

Value CodeGenerator::SarValues(const Value& lhs, const Value& rhs, bool assume_amount_masked /* = true */)
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
      EmitSar(res.host_reg, lhs.host_reg, res.size, rhs, assume_amount_masked);
    }
    else
    {
      EmitCopyValue(res.host_reg, lhs);
      EmitSar(res.host_reg, res.host_reg, res.size, rhs, assume_amount_masked);
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

void CodeGenerator::OrValueInPlace(Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value | rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        lhs = Value::FromConstantU8(Truncate8(new_cv));
        break;

      case RegSize_16:
        lhs = Value::FromConstantU16(Truncate16(new_cv));
        break;

      case RegSize_32:
        lhs = Value::FromConstantU32(Truncate32(new_cv));
        break;

      case RegSize_64:
        lhs = Value::FromConstantU64(new_cv);
        break;

      default:
        lhs = Value();
        break;
    }
  }

  // unlikely
  if (rhs.HasConstantValue(0))
    return;

  if (lhs.IsInHostRegister())
  {
    EmitOr(lhs.host_reg, lhs.host_reg, rhs);
  }
  else
  {
    Value new_lhs = m_register_cache.AllocateScratch(lhs.size);
    EmitCopyValue(new_lhs.host_reg, lhs);
    EmitOr(new_lhs.host_reg, new_lhs.host_reg, rhs);
    lhs = std::move(new_lhs);
  }
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

void CodeGenerator::AndValueInPlace(Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
  {
    // compile-time
    u64 new_cv = lhs.constant_value & rhs.constant_value;
    switch (lhs.size)
    {
      case RegSize_8:
        lhs = Value::FromConstantU8(Truncate8(new_cv));
        break;

      case RegSize_16:
        lhs = Value::FromConstantU16(Truncate16(new_cv));
        break;

      case RegSize_32:
        lhs = Value::FromConstantU32(Truncate32(new_cv));
        break;

      case RegSize_64:
        lhs = Value::FromConstantU64(new_cv);
        break;

      default:
        lhs = Value();
        break;
    }
  }

  // TODO: and with -1 -> noop
  if (lhs.HasConstantValue(0) || rhs.HasConstantValue(0))
  {
    EmitXor(lhs.host_reg, lhs.host_reg, lhs);
    return;
  }

  if (lhs.IsInHostRegister())
  {
    EmitAnd(lhs.host_reg, lhs.host_reg, rhs);
  }
  else
  {
    Value new_lhs = m_register_cache.AllocateScratch(lhs.size);
    EmitCopyValue(new_lhs.host_reg, lhs);
    EmitAnd(new_lhs.host_reg, new_lhs.host_reg, rhs);
    lhs = std::move(new_lhs);
  }
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

void CodeGenerator::GenerateExceptionExit(const CodeBlockInstruction& cbi, Exception excode,
                                          Condition condition /* = Condition::Always */)
{
  const Value CAUSE_bits = Value::FromConstantU32(
    Cop0Registers::CAUSE::MakeValueForException(excode, cbi.is_branch_delay_slot, false, cbi.instruction.cop.cop_n));

  if (condition == Condition::Always)
  {
    // no need to use far code if we're always raising the exception
    m_register_cache.FlushAllGuestRegisters(true, true);
    m_register_cache.FlushLoadDelay(true);

    EmitFunctionCall(nullptr, static_cast<void (*)(u32, u32)>(&CPU::RaiseException), CAUSE_bits,
                     GetCurrentInstructionPC());
    return;
  }

  LabelType skip_exception;
  EmitConditionalBranch(condition, true, &skip_exception);

  m_register_cache.PushState();

  EmitBranch(GetCurrentFarCodePointer());

  SwitchToFarCode();
  EmitFunctionCall(nullptr, static_cast<void (*)(u32, u32)>(&CPU::RaiseException), CAUSE_bits,
                   GetCurrentInstructionPC());
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();

  EmitBindLabel(&skip_exception);
}

void CodeGenerator::BlockPrologue()
{
  InitSpeculativeRegs();

  EmitStoreCPUStructField(offsetof(State, exception_raised), Value::FromConstantU8(0));

#if 0
  EmitFunctionCall(nullptr, &Thunks::LogPC, Value::FromConstantU32(m_pc));
#endif

  if (m_block->uncached_fetch_ticks > 0 || m_block->icache_line_count > 0)
    EmitICacheCheckAndUpdate();

  // we don't know the state of the last block, so assume load delays might be in progress
  // TODO: Pull load delay into register cache
  m_current_instruction_in_branch_delay_slot_dirty = g_settings.cpu_recompiler_memory_exceptions;
  m_branch_was_taken_dirty = g_settings.cpu_recompiler_memory_exceptions;
  m_current_instruction_was_branch_taken_dirty = false;
  m_load_delay_dirty = true;
  m_gte_busy_cycles_dirty = true;
}

void CodeGenerator::BlockEpilogue()
{
#if defined(_DEBUG) && defined(CPU_X64)
  m_emit->nop();
#endif

  m_register_cache.FlushAllGuestRegisters(true, true);
  if (m_register_cache.HasLoadDelay())
    m_register_cache.WriteLoadDelayToCPU(true);

  AddPendingCycles(true);
}

void CodeGenerator::InstructionPrologue(const CodeBlockInstruction& cbi, TickCount cycles,
                                        bool force_sync /* = false */)
{
#if defined(_DEBUG) && defined(CPU_X64)
  m_emit->nop();
#endif

  // move instruction offsets forward
  if (m_pc_valid)
    m_pc += 4;

  // reset dirty flags
  if (m_branch_was_taken_dirty)
  {
    Value temp = m_register_cache.AllocateScratch(RegSize_8);
    EmitLoadCPUStructField(temp.host_reg, RegSize_8, offsetof(State, branch_was_taken));
    EmitStoreCPUStructField(offsetof(State, current_instruction_was_branch_taken), temp);
    EmitStoreCPUStructField(offsetof(State, branch_was_taken), Value::FromConstantU8(0));
    m_current_instruction_was_branch_taken_dirty = true;
    m_branch_was_taken_dirty = false;
  }
  else if (m_current_instruction_was_branch_taken_dirty)
  {
    EmitStoreCPUStructField(offsetof(State, current_instruction_was_branch_taken), Value::FromConstantU8(0));
    m_current_instruction_was_branch_taken_dirty = false;
  }

  if (m_current_instruction_in_branch_delay_slot_dirty && !cbi.is_branch_delay_slot)
  {
    EmitStoreCPUStructField(offsetof(State, current_instruction_in_branch_delay_slot), Value::FromConstantU8(0));
    m_current_instruction_in_branch_delay_slot_dirty = false;
  }

  if (!force_sync)
  {
    // Defer updates for non-faulting instructions.
    m_delayed_cycles_add += cycles;
    return;
  }

  if (cbi.is_branch_delay_slot && g_settings.cpu_recompiler_memory_exceptions)
  {
    // m_current_instruction_in_branch_delay_slot = true
    EmitStoreCPUStructField(offsetof(State, current_instruction_in_branch_delay_slot), Value::FromConstantU8(1));
    m_current_instruction_in_branch_delay_slot_dirty = true;
  }

  m_delayed_cycles_add += cycles;
  AddPendingCycles(true);
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

void CodeGenerator::TruncateBlockAtCurrentInstruction()
{
  Log_DevPrintf("Truncating block %08X at %08X", m_block->GetPC(), m_current_instruction->pc);
  m_block_end = m_current_instruction + 1;
  WriteNewPC(CalculatePC(), true);
}

void CodeGenerator::AddPendingCycles(bool commit)
{
  if (m_delayed_cycles_add == 0 && m_gte_done_cycle <= m_delayed_cycles_add)
    return;

  if (m_gte_done_cycle > m_delayed_cycles_add)
  {
    Value temp = m_register_cache.AllocateScratch(RegSize_32);
    EmitLoadCPUStructField(temp.GetHostRegister(), RegSize_32, offsetof(State, pending_ticks));
    if (m_delayed_cycles_add > 0)
    {
      EmitAdd(temp.GetHostRegister(), temp.GetHostRegister(), Value::FromConstantU32(m_delayed_cycles_add), false);
      EmitStoreCPUStructField(offsetof(State, pending_ticks), temp);
      EmitAdd(temp.GetHostRegister(), temp.GetHostRegister(),
              Value::FromConstantU32(m_gte_done_cycle - m_delayed_cycles_add), false);
      EmitStoreCPUStructField(offsetof(State, gte_completion_tick), temp);
    }
    else
    {
      EmitAdd(temp.GetHostRegister(), temp.GetHostRegister(), Value::FromConstantU32(m_gte_done_cycle), false);
      EmitStoreCPUStructField(offsetof(State, gte_completion_tick), temp);
    }
  }
  else
  {
    EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(m_delayed_cycles_add));
  }

  if (commit)
  {
    m_gte_done_cycle = std::max<TickCount>(m_gte_done_cycle - m_delayed_cycles_add, 0);
    m_delayed_cycles_add = 0;
  }
}

void CodeGenerator::AddGTETicks(TickCount ticks)
{
  m_gte_done_cycle = m_delayed_cycles_add + ticks;
  Log_DebugPrintf("Adding %d GTE ticks", ticks);
}

void CodeGenerator::StallUntilGTEComplete()
{
  if (!m_gte_busy_cycles_dirty)
  {
    // simple case - in block scheduling
    if (m_gte_done_cycle > m_delayed_cycles_add)
    {
      Log_DebugPrintf("Stalling for %d ticks from GTE", m_gte_done_cycle - m_delayed_cycles_add);
      m_delayed_cycles_add += (m_gte_done_cycle - m_delayed_cycles_add);
    }

    return;
  }

  // switch to in block scheduling
  EmitStallUntilGTEComplete();
  m_gte_done_cycle = 0;
  m_gte_busy_cycles_dirty = false;
}

Value CodeGenerator::CalculatePC(u32 offset /* = 0 */)
{
  if (!m_pc_valid)
    Panic("Attempt to get an indeterminate PC");

  return Value::FromConstantU32(m_pc + offset);
}

Value CodeGenerator::GetCurrentInstructionPC(u32 offset /* = 0 */)
{
  return Value::FromConstantU32(m_current_instruction->pc);
}

void CodeGenerator::WriteNewPC(const Value& value, bool commit)
{
  // TODO: This _could_ be moved into the register cache, but would it gain anything?
  EmitStoreGuestRegister(Reg::pc, value);
  if (commit)
  {
    m_pc_valid = value.IsConstant();
    if (m_pc_valid)
      m_pc = static_cast<u32>(value.constant_value);
  }
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

  EmitStoreCPUStructField(offsetof(State, current_instruction_pc), Value::FromConstantU32(cbi.pc));
  EmitStoreCPUStructField(offsetof(State, current_instruction.bits), Value::FromConstantU32(cbi.instruction.bits));

  // emit the function call
  if (CanInstructionTrap(cbi.instruction, m_block->key.user_mode))
  {
    // TODO: Use carry flag or something here too
    Value return_value = m_register_cache.AllocateScratch(RegSize_8);
    EmitFunctionCall(&return_value,
                     g_settings.gpu_pgxp_enable ? &Thunks::InterpretInstructionPGXP : &Thunks::InterpretInstruction);
    EmitExceptionExitOnBool(return_value);
  }
  else
  {
    EmitFunctionCall(nullptr,
                     g_settings.gpu_pgxp_enable ? &Thunks::InterpretInstructionPGXP : &Thunks::InterpretInstruction);
  }

  m_current_instruction_in_branch_delay_slot_dirty = cbi.is_branch_instruction;
  m_branch_was_taken_dirty = cbi.is_branch_instruction;
  m_next_load_delay_dirty = cbi.has_load_delay;
  InvalidateSpeculativeValues();
  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Bitwise(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const InstructionOp op = cbi.instruction.op;
  Value lhs;
  Value rhs;
  Reg dest;

  SpeculativeValue spec_lhs, spec_rhs;
  SpeculativeValue spec_value;

  if (op != InstructionOp::funct)
  {
    // rt <- rs op zext(imm)
    lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
    rhs = Value::FromConstantU32(cbi.instruction.i.imm_zext32());
    dest = cbi.instruction.i.rt;

    spec_lhs = SpeculativeReadReg(cbi.instruction.i.rs);
    spec_rhs = cbi.instruction.i.imm_zext32();
  }
  else
  {
    lhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
    rhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
    dest = cbi.instruction.r.rd;

    spec_lhs = SpeculativeReadReg(cbi.instruction.r.rs);
    spec_rhs = SpeculativeReadReg(cbi.instruction.r.rt);
  }

  Value result;
  switch (cbi.instruction.op)
  {
    case InstructionOp::ori:
    {
      if (g_settings.UsingPGXPCPUMode())
        EmitFunctionCall(nullptr, &PGXP::CPU_ORI, Value::FromConstantU32(cbi.instruction.bits), lhs);

      result = OrValues(lhs, rhs);
      if (spec_lhs && spec_rhs)
        spec_value = *spec_lhs | *spec_rhs;
    }
    break;

    case InstructionOp::andi:
    {
      if (g_settings.UsingPGXPCPUMode())
        EmitFunctionCall(nullptr, &PGXP::CPU_ANDI, Value::FromConstantU32(cbi.instruction.bits), lhs);

      result = AndValues(lhs, rhs);
      if (spec_lhs && spec_rhs)
        spec_value = *spec_lhs & *spec_rhs;
    }
    break;

    case InstructionOp::xori:
    {
      if (g_settings.UsingPGXPCPUMode())
        EmitFunctionCall(nullptr, &PGXP::CPU_XORI, Value::FromConstantU32(cbi.instruction.bits), lhs);

      result = XorValues(lhs, rhs);
      if (spec_lhs && spec_rhs)
        spec_value = *spec_lhs ^ *spec_rhs;
    }
    break;

    case InstructionOp::funct:
    {
      switch (cbi.instruction.r.funct)
      {
        case InstructionFunct::or_:
        {
          if (g_settings.UsingPGXPCPUMode())
            EmitFunctionCall(nullptr, &PGXP::CPU_OR_, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);

          result = OrValues(lhs, rhs);
          if (spec_lhs && spec_rhs)
            spec_value = *spec_lhs | *spec_rhs;
        }
        break;

        case InstructionFunct::and_:
        {
          if (g_settings.UsingPGXPCPUMode())
            EmitFunctionCall(nullptr, &PGXP::CPU_AND_, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);

          result = AndValues(lhs, rhs);
          if (spec_lhs && spec_rhs)
            spec_value = *spec_lhs & *spec_rhs;
        }
        break;

        case InstructionFunct::xor_:
        {
          if (g_settings.UsingPGXPCPUMode())
            EmitFunctionCall(nullptr, &PGXP::CPU_XOR_, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);

          result = XorValues(lhs, rhs);
          if (spec_lhs && spec_rhs)
            spec_value = *spec_lhs ^ *spec_rhs;
        }
        break;

        case InstructionFunct::nor:
        {
          if (g_settings.UsingPGXPCPUMode())
            EmitFunctionCall(nullptr, &PGXP::CPU_NOR, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);

          result = NotValue(OrValues(lhs, rhs));
          if (spec_lhs && spec_rhs)
            spec_value = ~(*spec_lhs | *spec_rhs);
        }
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
  SpeculativeWriteReg(dest, spec_value);

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Shift(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const InstructionFunct funct = cbi.instruction.r.funct;
  Value rt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
  SpeculativeValue rt_spec = SpeculativeReadReg(cbi.instruction.r.rt);
  Value shamt;
  SpeculativeValue shamt_spec;
  if (funct == InstructionFunct::sll || funct == InstructionFunct::srl || funct == InstructionFunct::sra)
  {
    // rd <- rt op shamt
    shamt = Value::FromConstantU32(cbi.instruction.r.shamt);
    shamt_spec = cbi.instruction.r.shamt;
  }
  else
  {
    // rd <- rt op (rs & 0x1F)
    shamt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
    shamt_spec = SpeculativeReadReg(cbi.instruction.r.rs);
  }

  Value result;
  SpeculativeValue result_spec;
  switch (cbi.instruction.r.funct)
  {
    case InstructionFunct::sll:
    case InstructionFunct::sllv:
    {
      if (g_settings.UsingPGXPCPUMode())
      {
        if (cbi.instruction.r.funct == InstructionFunct::sll)
          EmitFunctionCall(nullptr, &PGXP::CPU_SLL, Value::FromConstantU32(cbi.instruction.bits), rt);
        else // if (cbi.instruction.r.funct == InstructionFunct::sllv)
          EmitFunctionCall(nullptr, &PGXP::CPU_SLLV, Value::FromConstantU32(cbi.instruction.bits), rt, shamt);
      }

      result = ShlValues(rt, shamt, false);
      if (rt_spec && shamt_spec)
        result_spec = *rt_spec << *shamt_spec;
    }
    break;

    case InstructionFunct::srl:
    case InstructionFunct::srlv:
    {
      if (g_settings.UsingPGXPCPUMode())
      {
        if (cbi.instruction.r.funct == InstructionFunct::srl)
          EmitFunctionCall(nullptr, &PGXP::CPU_SRL, Value::FromConstantU32(cbi.instruction.bits), rt);
        else // if (cbi.instruction.r.funct == InstructionFunct::srlv)
          EmitFunctionCall(nullptr, &PGXP::CPU_SRLV, Value::FromConstantU32(cbi.instruction.bits), rt, shamt);
      }

      result = ShrValues(rt, shamt, false);
      if (rt_spec && shamt_spec)
        result_spec = *rt_spec >> *shamt_spec;
    }
    break;

    case InstructionFunct::sra:
    case InstructionFunct::srav:
    {
      if (g_settings.UsingPGXPCPUMode())
      {
        if (cbi.instruction.r.funct == InstructionFunct::sra)
          EmitFunctionCall(nullptr, &PGXP::CPU_SRA, Value::FromConstantU32(cbi.instruction.bits), rt);
        else // if (cbi.instruction.r.funct == InstructionFunct::srav)
          EmitFunctionCall(nullptr, &PGXP::CPU_SRAV, Value::FromConstantU32(cbi.instruction.bits), rt, shamt);
      }

      result = SarValues(rt, shamt, false);
      if (rt_spec && shamt_spec)
        result_spec = static_cast<u32>(static_cast<s32>(*rt_spec) << *shamt_spec);
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, std::move(result));
  SpeculativeWriteReg(cbi.instruction.r.rd, result_spec);

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

  SpeculativeValue address_spec = SpeculativeReadReg(cbi.instruction.i.rs);
  SpeculativeValue value_spec;
  if (address_spec)
    address_spec = *address_spec + cbi.instruction.i.imm_sext32();

  Value result;
  switch (cbi.instruction.op)
  {
    case InstructionOp::lb:
    case InstructionOp::lbu:
    {
      result = EmitLoadGuestMemory(cbi, address, address_spec, RegSize_8);
      ConvertValueSizeInPlace(&result, RegSize_32, (cbi.instruction.op == InstructionOp::lb));
      if (g_settings.gpu_pgxp_enable)
        EmitFunctionCall(nullptr, PGXP::CPU_LBx, Value::FromConstantU32(cbi.instruction.bits), result, address);

      if (address_spec)
      {
        value_spec = SpeculativeReadMemory(*address_spec & ~3u);
        if (value_spec)
          value_spec = (*value_spec >> ((*address_spec & 3u) * 8u)) & 0xFFu;
      }
    }
    break;

    case InstructionOp::lh:
    case InstructionOp::lhu:
    {
      result = EmitLoadGuestMemory(cbi, address, address_spec, RegSize_16);
      ConvertValueSizeInPlace(&result, RegSize_32, (cbi.instruction.op == InstructionOp::lh));

      if (g_settings.gpu_pgxp_enable)
        EmitFunctionCall(nullptr, PGXP::CPU_LHx, Value::FromConstantU32(cbi.instruction.bits), result, address);

      if (address_spec)
      {
        value_spec = SpeculativeReadMemory(*address_spec & ~3u);
        if (value_spec)
          value_spec = (*value_spec >> ((*address_spec & 3u) * 8u)) & 0xFFFFu;
      }
    }
    break;

    case InstructionOp::lw:
    {
      result = EmitLoadGuestMemory(cbi, address, address_spec, RegSize_32);
      if (g_settings.gpu_pgxp_enable)
        EmitFunctionCall(nullptr, PGXP::CPU_LW, Value::FromConstantU32(cbi.instruction.bits), result, address);

      if (address_spec)
        value_spec = SpeculativeReadMemory(*address_spec);
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  m_register_cache.WriteGuestRegisterDelayed(cbi.instruction.i.rt, std::move(result));
  SpeculativeWriteReg(cbi.instruction.i.rt, value_spec);

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

  SpeculativeValue address_spec = SpeculativeReadReg(cbi.instruction.i.rs);
  SpeculativeValue value_spec = SpeculativeReadReg(cbi.instruction.i.rt);
  if (address_spec)
    address_spec = *address_spec + cbi.instruction.i.imm_sext32();

  switch (cbi.instruction.op)
  {
    case InstructionOp::sb:
    {
      if (g_settings.gpu_pgxp_enable)
      {
        EmitFunctionCall(nullptr, PGXP::CPU_SB, Value::FromConstantU32(cbi.instruction.bits),
                         value.ViewAsSize(RegSize_8), address);
      }

      EmitStoreGuestMemory(cbi, address, address_spec, RegSize_8, value);

      if (address_spec)
      {
        const VirtualMemoryAddress aligned_addr = (*address_spec & ~3u);
        const SpeculativeValue aligned_existing_value = SpeculativeReadMemory(aligned_addr);
        if (aligned_existing_value)
        {
          if (value_spec)
          {
            const u32 shift = (aligned_addr & 3u) * 8u;
            SpeculativeWriteMemory(aligned_addr,
                                   (*aligned_existing_value & ~(0xFFu << shift)) | ((*value_spec & 0xFFu) << shift));
          }
          else
          {
            SpeculativeWriteMemory(aligned_addr, std::nullopt);
          }
        }
      }
    }
    break;

    case InstructionOp::sh:
    {
      if (g_settings.gpu_pgxp_enable)
      {
        EmitFunctionCall(nullptr, PGXP::CPU_SH, Value::FromConstantU32(cbi.instruction.bits),
                         value.ViewAsSize(RegSize_16), address);
      }

      EmitStoreGuestMemory(cbi, address, address_spec, RegSize_16, value);

      if (address_spec)
      {
        const VirtualMemoryAddress aligned_addr = (*address_spec & ~3u);
        const SpeculativeValue aligned_existing_value = SpeculativeReadMemory(aligned_addr);
        if (aligned_existing_value)
        {
          if (value_spec)
          {
            const u32 shift = (aligned_addr & 1u) * 16u;
            SpeculativeWriteMemory(aligned_addr, (*aligned_existing_value & ~(0xFFFFu << shift)) |
                                                   ((*value_spec & 0xFFFFu) << shift));
          }
          else
          {
            SpeculativeWriteMemory(aligned_addr, std::nullopt);
          }
        }
      }
    }
    break;

    case InstructionOp::sw:
    {
      if (g_settings.gpu_pgxp_enable)
        EmitFunctionCall(nullptr, PGXP::CPU_SW, Value::FromConstantU32(cbi.instruction.bits), value, address);

      EmitStoreGuestMemory(cbi, address, address_spec, RegSize_32, value);

      if (address_spec)
        SpeculativeWriteMemory(*address_spec, value_spec);
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  InstructionEpilogue(cbi);

  if (address_spec)
  {
    const CPU::Segment seg = GetSegmentForAddress(*address_spec);
    if (seg == Segment::KUSEG || seg == Segment::KSEG0 || seg == Segment::KSEG1)
    {
      const PhysicalMemoryAddress phys_addr = VirtualAddressToPhysical(*address_spec);
      const PhysicalMemoryAddress block_start = VirtualAddressToPhysical(m_block->GetPC());
      const PhysicalMemoryAddress block_end = VirtualAddressToPhysical(
        m_block->GetPC() + static_cast<u32>(m_block->instructions.size()) * sizeof(Instruction));
      if (phys_addr >= block_start && phys_addr < block_end)
      {
        Log_WarningPrintf("Instruction %08X speculatively writes to %08X inside block %08X-%08X. Truncating block.",
                          cbi.pc, phys_addr, block_start, block_end);
        TruncateBlockAtCurrentInstruction();
      }
    }
  }

  return true;
}

bool CodeGenerator::Compile_LoadLeftRight(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  Value base = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
  Value offset = Value::FromConstantU32(cbi.instruction.i.imm_sext32());
  Value address = AddValues(base, offset, false);
  base.ReleaseAndClear();

  SpeculativeValue address_spec = SpeculativeReadReg(cbi.instruction.i.rs);
  if (address_spec)
    address_spec = *address_spec + cbi.instruction.i.imm_sext32();

  Value shift = ShlValues(AndValues(address, Value::FromConstantU32(3)), Value::FromConstantU32(3)); // * 8
  address = AndValues(address, Value::FromConstantU32(~u32(3)));

  // hack to bypass load delays
  Value value;
  if (cbi.instruction.i.rt == m_register_cache.GetLoadDelayRegister())
  {
    const Value& ld_value = m_register_cache.GetLoadDelayValue();
    if (ld_value.IsInHostRegister())
      value.SetHostReg(&m_register_cache, ld_value.GetHostRegister(), ld_value.size);
    else
      value = ld_value;
  }
  else
  {
    // if this is the first instruction in the block, we need to stall until the load finishes
    // we don't actually care if it's our target reg or not, if it's not, it won't affect anything
    if (m_load_delay_dirty)
    {
      Log_DevPrintf("Flushing interpreter load delay for lwl/lwr instruction at 0x%08X", cbi.pc);
      EmitFlushInterpreterLoadDelay();
      m_register_cache.InvalidateGuestRegister(cbi.instruction.r.rt);
      m_load_delay_dirty = false;
    }

    value = m_register_cache.ReadGuestRegister(cbi.instruction.i.rt, true, true);
  }

  Value mem;
  if (cbi.instruction.op == InstructionOp::lwl)
  {
    Value lhs = ShrValues(Value::FromConstantU32(0x00FFFFFF), shift);
    AndValueInPlace(lhs, value);
    shift = SubValues(Value::FromConstantU32(24), shift, false);
    value.ReleaseAndClear();

    mem = EmitLoadGuestMemory(cbi, address, address_spec, RegSize_32);
    EmitShl(mem.GetHostRegister(), mem.GetHostRegister(), RegSize_32, shift);
    EmitOr(mem.GetHostRegister(), mem.GetHostRegister(), lhs);
  }
  else
  {
    Value lhs = ShlValues(Value::FromConstantU32(0xFFFFFF00), SubValues(Value::FromConstantU32(24), shift, false));
    AndValueInPlace(lhs, value);
    value.ReleaseAndClear();

    mem = EmitLoadGuestMemory(cbi, address, address_spec, RegSize_32);
    EmitShr(mem.GetHostRegister(), mem.GetHostRegister(), RegSize_32, shift);
    EmitOr(mem.GetHostRegister(), mem.GetHostRegister(), lhs);
  }

  shift.ReleaseAndClear();

  if (g_settings.gpu_pgxp_enable)
    EmitFunctionCall(nullptr, PGXP::CPU_LW, Value::FromConstantU32(cbi.instruction.bits), mem, address);

  m_register_cache.WriteGuestRegisterDelayed(cbi.instruction.i.rt, std::move(mem));

  // TODO: Speculative values
  SpeculativeWriteReg(cbi.instruction.r.rt, std::nullopt);

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_StoreLeftRight(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  Value base = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
  Value offset = Value::FromConstantU32(cbi.instruction.i.imm_sext32());
  Value address = AddValues(base, offset, false);
  base.ReleaseAndClear();

  // TODO: Speculative values
  SpeculativeValue address_spec = SpeculativeReadReg(cbi.instruction.i.rs);
  if (address_spec)
  {
    address_spec = *address_spec + cbi.instruction.i.imm_sext32();
    SpeculativeWriteMemory(*address_spec & ~3u, std::nullopt);
  }

  Value shift = ShlValues(AndValues(address, Value::FromConstantU32(3)), Value::FromConstantU32(3)); // * 8
  address = AndValues(address, Value::FromConstantU32(~u32(3)));

  Value mem;
  if (cbi.instruction.op == InstructionOp::swl)
  {
    Value mask = ShlValues(Value::FromConstantU32(0xFFFFFF00), shift);
    mem = EmitLoadGuestMemory(cbi, address, address_spec, RegSize_32);
    EmitAnd(mem.GetHostRegister(), mem.GetHostRegister(), mask);
    mask.ReleaseAndClear();

    Value reg = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
    Value lhs = ShrValues(reg, SubValues(Value::FromConstantU32(24), shift, false));
    reg.ReleaseAndClear();

    EmitOr(mem.GetHostRegister(), mem.GetHostRegister(), lhs);
  }
  else
  {
    Value mask = ShrValues(Value::FromConstantU32(0x00FFFFFF), SubValues(Value::FromConstantU32(24), shift, false));
    mem = EmitLoadGuestMemory(cbi, address, address_spec, RegSize_32);
    AndValueInPlace(mem, mask);
    mask.ReleaseAndClear();

    Value reg = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
    Value lhs = ShlValues(reg, shift);
    reg.ReleaseAndClear();

    EmitOr(mem.GetHostRegister(), mem.GetHostRegister(), lhs);
  }

  shift.ReleaseAndClear();

  EmitStoreGuestMemory(cbi, address, address_spec, RegSize_32, mem);
  if (g_settings.gpu_pgxp_enable)
    EmitFunctionCall(nullptr, PGXP::CPU_SW, Value::FromConstantU32(cbi.instruction.bits), mem, address);

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_MoveHiLo(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  switch (cbi.instruction.r.funct)
  {
    case InstructionFunct::mfhi:
    {
      Value hi = m_register_cache.ReadGuestRegister(Reg::hi);
      if (g_settings.UsingPGXPCPUMode())
        EmitFunctionCall(nullptr, &PGXP::CPU_MFHI, Value::FromConstantU32(cbi.instruction.bits), hi);

      m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, std::move(hi));
      SpeculativeWriteReg(cbi.instruction.r.rd, std::nullopt);
    }
    break;

    case InstructionFunct::mthi:
    {
      Value rs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
      if (g_settings.UsingPGXPCPUMode())
        EmitFunctionCall(nullptr, &PGXP::CPU_MTHI, Value::FromConstantU32(cbi.instruction.bits), rs);

      m_register_cache.WriteGuestRegister(Reg::hi, std::move(rs));
    }
    break;

    case InstructionFunct::mflo:
    {
      Value lo = m_register_cache.ReadGuestRegister(Reg::lo);
      if (g_settings.UsingPGXPCPUMode())
        EmitFunctionCall(nullptr, &PGXP::CPU_MFLO, Value::FromConstantU32(cbi.instruction.bits), lo);

      m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, std::move(lo));
      SpeculativeWriteReg(cbi.instruction.r.rd, std::nullopt);
    }
    break;

    case InstructionFunct::mtlo:
    {
      Value rs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
      if (g_settings.UsingPGXPCPUMode())
        EmitFunctionCall(nullptr, &PGXP::CPU_MTLO, Value::FromConstantU32(cbi.instruction.bits), rs);

      m_register_cache.WriteGuestRegister(Reg::lo, std::move(rs));
    }
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
  Reg lhs_src;
  SpeculativeValue lhs_spec, rhs_spec;
  Reg dest;

  switch (cbi.instruction.op)
  {
    case InstructionOp::addi:
    case InstructionOp::addiu:
    {
      // rt <- rs + sext(imm)
      dest = cbi.instruction.i.rt;
      lhs_src = cbi.instruction.i.rs;
      lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
      rhs = Value::FromConstantU32(cbi.instruction.i.imm_sext32());

      lhs_spec = SpeculativeReadReg(cbi.instruction.i.rs);
      rhs_spec = cbi.instruction.i.imm_sext32();
    }
    break;

    case InstructionOp::funct:
    {
      Assert(cbi.instruction.r.funct == InstructionFunct::add || cbi.instruction.r.funct == InstructionFunct::addu);
      dest = cbi.instruction.r.rd;
      lhs_src = cbi.instruction.r.rs;
      lhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
      rhs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
      lhs_spec = SpeculativeReadReg(cbi.instruction.r.rs);
      rhs_spec = SpeculativeReadReg(cbi.instruction.r.rt);
    }
    break;

    default:
      UnreachableCode();
      return false;
  }

  // detect register moves and handle them for pgxp
  if (g_settings.gpu_pgxp_enable && rhs.HasConstantValue(0))
  {
    EmitFunctionCall(nullptr, &PGXP::CPU_MOVE,
                     Value::FromConstantU32((static_cast<u32>(dest) << 8) | (static_cast<u32>(lhs_src))), lhs);
  }
  else if (g_settings.UsingPGXPCPUMode())
  {
    if (cbi.instruction.op != InstructionOp::funct)
      EmitFunctionCall(nullptr, &PGXP::CPU_ADDI, Value::FromConstantU32(cbi.instruction.bits), lhs);
    else
      EmitFunctionCall(nullptr, &PGXP::CPU_ADD, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);
  }

  Value result = AddValues(lhs, rhs, check_overflow);
  if (check_overflow)
    GenerateExceptionExit(cbi, Exception::Ov, Condition::Overflow);

  m_register_cache.WriteGuestRegister(dest, std::move(result));

  SpeculativeValue value_spec;
  if (lhs_spec && rhs_spec)
    value_spec = *lhs_spec + *rhs_spec;
  SpeculativeWriteReg(dest, value_spec);

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

  SpeculativeValue lhs_spec = SpeculativeReadReg(cbi.instruction.r.rs);
  SpeculativeValue rhs_spec = SpeculativeReadReg(cbi.instruction.r.rt);

  if (g_settings.UsingPGXPCPUMode())
    EmitFunctionCall(nullptr, &PGXP::CPU_SUB, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);

  Value result = SubValues(lhs, rhs, check_overflow);
  if (check_overflow)
    GenerateExceptionExit(cbi, Exception::Ov, Condition::Overflow);

  m_register_cache.WriteGuestRegister(cbi.instruction.r.rd, std::move(result));

  SpeculativeValue value_spec;
  if (lhs_spec && rhs_spec)
    value_spec = *lhs_spec - *rhs_spec;
  SpeculativeWriteReg(cbi.instruction.r.rd, value_spec);

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Multiply(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  const bool signed_multiply = (cbi.instruction.r.funct == InstructionFunct::mult);
  Value rs = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
  Value rt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
  if (g_settings.UsingPGXPCPUMode())
  {
    EmitFunctionCall(nullptr, signed_multiply ? &PGXP::CPU_MULT : &PGXP::CPU_MULTU,
                     Value::FromConstantU32(cbi.instruction.bits), rs, rt);
  }

  std::pair<Value, Value> result = MulValues(rs, rt, signed_multiply);
  rs.ReleaseAndClear();
  rt.ReleaseAndClear();
  m_register_cache.WriteGuestRegister(Reg::hi, std::move(result.first));
  m_register_cache.WriteGuestRegister(Reg::lo, std::move(result.second));

  InstructionEpilogue(cbi);
  return true;
}

static std::tuple<u32, u32> MIPSDivide(u32 num, u32 denom)
{
  u32 lo, hi;

  if (denom == 0)
  {
    // divide by zero
    lo = UINT32_C(0xFFFFFFFF);
    hi = static_cast<u32>(num);
  }
  else
  {
    lo = num / denom;
    hi = num % denom;
  }

  return std::tie(lo, hi);
}

static std::tuple<s32, s32> MIPSDivide(s32 num, s32 denom)
{
  s32 lo, hi;
  if (denom == 0)
  {
    // divide by zero
    lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
    hi = static_cast<u32>(num);
  }
  else if (static_cast<u32>(num) == UINT32_C(0x80000000) && denom == -1)
  {
    // unrepresentable
    lo = UINT32_C(0x80000000);
    hi = 0;
  }
  else
  {
    lo = num / denom;
    hi = num % denom;
  }

  return std::tie(lo, hi);
}

bool CodeGenerator::Compile_Divide(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  Value num = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
  Value denom = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);

  if (g_settings.UsingPGXPCPUMode())
    EmitFunctionCall(nullptr, &PGXP::CPU_DIV, Value::FromConstantU32(cbi.instruction.bits), num, denom);

  if (num.IsConstant() && denom.IsConstant())
  {
    const auto [lo, hi] = MIPSDivide(static_cast<u32>(num.constant_value), static_cast<u32>(denom.constant_value));
    m_register_cache.WriteGuestRegister(Reg::lo, Value::FromConstantU32(lo));
    m_register_cache.WriteGuestRegister(Reg::hi, Value::FromConstantU32(hi));
  }
  else
  {
    Value num_reg = GetValueInHostRegister(num, false);
    Value denom_reg = GetValueInHostRegister(denom, false);

    m_register_cache.InvalidateGuestRegister(Reg::lo);
    m_register_cache.InvalidateGuestRegister(Reg::hi);

    Value lo = m_register_cache.AllocateScratch(RegSize_32);
    Value hi = m_register_cache.AllocateScratch(RegSize_32);
    m_register_cache.InhibitAllocation();

    LabelType do_divide, done;

    if (!denom.IsConstant() || denom.HasConstantValue(0))
    {
      // if (denom == 0)
      EmitConditionalBranch(Condition::NotEqual, false, denom_reg.GetHostRegister(), Value::FromConstantU32(0),
                            &do_divide);
      {
        // unrepresentable
        EmitCopyValue(lo.GetHostRegister(), Value::FromConstantU32(0xFFFFFFFF));
        EmitCopyValue(hi.GetHostRegister(), num_reg);
        EmitBranch(&done);
      }
    }

    // else
    {
      EmitBindLabel(&do_divide);
      EmitDiv(lo.GetHostRegister(), hi.GetHostRegister(), num_reg.GetHostRegister(), denom_reg.GetHostRegister(),
              RegSize_32, false);
    }

    EmitBindLabel(&done);

    m_register_cache.UninhibitAllocation();
    m_register_cache.WriteGuestRegister(Reg::lo, std::move(lo));
    m_register_cache.WriteGuestRegister(Reg::hi, std::move(hi));
  }

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_SignedDivide(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  Value num = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
  Value denom = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);

  if (g_settings.UsingPGXPCPUMode())
    EmitFunctionCall(nullptr, &PGXP::CPU_DIV, Value::FromConstantU32(cbi.instruction.bits), num, denom);

  if (num.IsConstant() && denom.IsConstant())
  {
    const auto [lo, hi] = MIPSDivide(num.GetS32ConstantValue(), denom.GetS32ConstantValue());
    m_register_cache.WriteGuestRegister(Reg::lo, Value::FromConstantU32(static_cast<u32>(lo)));
    m_register_cache.WriteGuestRegister(Reg::hi, Value::FromConstantU32(static_cast<u32>(hi)));
  }
  else
  {
    Value num_reg = GetValueInHostRegister(num, false);
    Value denom_reg = GetValueInHostRegister(denom, false);

    m_register_cache.InvalidateGuestRegister(Reg::lo);
    m_register_cache.InvalidateGuestRegister(Reg::hi);

    Value lo = m_register_cache.AllocateScratch(RegSize_32);
    Value hi = m_register_cache.AllocateScratch(RegSize_32);
    m_register_cache.InhibitAllocation();

    // we need this in a register on ARM because it won't fit in an immediate
    EmitCopyValue(lo.GetHostRegister(), Value::FromConstantU32(0x80000000u));

    LabelType do_divide, done;

    LabelType not_zero;
    if (!denom.IsConstant() || denom.HasConstantValue(0))
    {
      // if (denom == 0)
      EmitConditionalBranch(Condition::NotEqual, false, denom_reg.GetHostRegister(), Value::FromConstantU32(0),
                            &not_zero);
      {
        // hi = static_cast<u32>(num);
        EmitCopyValue(hi.GetHostRegister(), num_reg);

        // lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
        LabelType greater_equal_zero;
        EmitConditionalBranch(Condition::GreaterEqual, false, num_reg.GetHostRegister(), Value::FromConstantU32(0),
                              &greater_equal_zero);
        EmitCopyValue(lo.GetHostRegister(), Value::FromConstantU32(1));
        EmitBranch(&done);
        EmitBindLabel(&greater_equal_zero);
        EmitCopyValue(lo.GetHostRegister(), Value::FromConstantU32(0xFFFFFFFFu));
        EmitBranch(&done);
      }
    }

    // else if (static_cast<u32>(num) == UINT32_C(0x80000000) && denom == -1)
    {
      EmitBindLabel(&not_zero);
      EmitConditionalBranch(Condition::NotEqual, false, denom_reg.GetHostRegister(), Value::FromConstantS32(-1),
                            &do_divide);
      EmitConditionalBranch(Condition::NotEqual, false, num_reg.GetHostRegister(), lo, &do_divide);

      // unrepresentable
      // EmitCopyValue(lo.GetHostRegister(), Value::FromConstantU32(0x80000000u)); // done above
      EmitCopyValue(hi.GetHostRegister(), Value::FromConstantU32(0));
      EmitBranch(&done);
    }

    // else
    {
      EmitBindLabel(&do_divide);
      EmitDiv(lo.GetHostRegister(), hi.GetHostRegister(), num_reg.GetHostRegister(), denom_reg.GetHostRegister(),
              RegSize_32, true);
    }

    EmitBindLabel(&done);

    m_register_cache.UninhibitAllocation();
    m_register_cache.WriteGuestRegister(Reg::lo, std::move(lo));
    m_register_cache.WriteGuestRegister(Reg::hi, std::move(hi));
  }

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
  SpeculativeValue lhs_spec, rhs_spec;
  if (cbi.instruction.op == InstructionOp::slti || cbi.instruction.op == InstructionOp::sltiu)
  {
    // rt <- rs < {z,s}ext(imm)
    dest = cbi.instruction.i.rt;
    lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);
    rhs = Value::FromConstantU32(cbi.instruction.i.imm_sext32());
    lhs_spec = SpeculativeReadReg(cbi.instruction.i.rs);
    rhs_spec = cbi.instruction.i.imm_sext32();

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
    lhs_spec = SpeculativeReadReg(cbi.instruction.r.rs);
    rhs_spec = SpeculativeReadReg(cbi.instruction.r.rt);

    // flush the old value which might free up a register
    if (dest != cbi.instruction.i.rs && dest != cbi.instruction.r.rt)
      m_register_cache.InvalidateGuestRegister(dest);
  }

  if (g_settings.UsingPGXPCPUMode())
  {
    if (cbi.instruction.op == InstructionOp::slti)
      EmitFunctionCall(nullptr, &PGXP::CPU_SLTI, Value::FromConstantU32(cbi.instruction.bits), lhs);
    else if (cbi.instruction.op == InstructionOp::sltiu)
      EmitFunctionCall(nullptr, &PGXP::CPU_SLTIU, Value::FromConstantU32(cbi.instruction.bits), lhs);
    else if (cbi.instruction.r.funct == InstructionFunct::slt)
      EmitFunctionCall(nullptr, &PGXP::CPU_SLT, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);
    else // if (cbi.instruction.r.funct == InstructionFunct::sltu)
      EmitFunctionCall(nullptr, &PGXP::CPU_SLTU, Value::FromConstantU32(cbi.instruction.bits), lhs, rhs);
  }

  Value result = m_register_cache.AllocateScratch(RegSize_32);
  EmitCmp(lhs.host_reg, rhs);
  EmitSetConditionResult(result.host_reg, result.size, signed_comparison ? Condition::Less : Condition::Below);

  m_register_cache.WriteGuestRegister(dest, std::move(result));

  SpeculativeValue value_spec;
  if (lhs_spec && rhs_spec)
  {
    value_spec = BoolToUInt32(signed_comparison ? (static_cast<s32>(*lhs_spec) < static_cast<s32>(*rhs_spec)) :
                                                  (*lhs_spec < *rhs_spec));
  }
  SpeculativeWriteReg(cbi.instruction.r.rd, value_spec);

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_Branch(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  auto DoBranch = [this, &cbi](Condition condition, const Value& lhs, const Value& rhs, Reg lr_reg,
                               Value&& branch_target) {
    const bool can_link_block = cbi.is_direct_branch_instruction && g_settings.cpu_recompiler_block_linking;

    // ensure the lr register is flushed, since we want it's correct value after the branch
    // we don't want to invalidate it yet because of "jalr r0, r0", branch_target could be the lr_reg.
    if (lr_reg != Reg::count && lr_reg != Reg::zero)
      m_register_cache.FlushGuestRegister(lr_reg, false, true);

    // compute return address, which is also set as the new pc when the branch isn't taken
    Value next_pc = CalculatePC(4);
    DebugAssert(next_pc.IsConstant());
    if (condition != Condition::Always)
    {
      next_pc = m_register_cache.AllocateScratch(RegSize_32);
      EmitCopyValue(next_pc.GetHostRegister(), CalculatePC(4));
    }

    Value take_branch;
    LabelType branch_taken, branch_not_taken;
    if (condition != Condition::Always)
    {
      if (!can_link_block)
      {
        // condition is inverted because we want the case for skipping it
        if (lhs.IsValid() && rhs.IsValid())
          EmitConditionalBranch(condition, true, lhs.host_reg, rhs, &branch_not_taken);
        else if (lhs.IsValid())
          EmitConditionalBranch(condition, true, lhs.host_reg, lhs.size, &branch_not_taken);
        else
          EmitConditionalBranch(condition, true, &branch_not_taken);
      }
      else
      {
        take_branch = m_register_cache.AllocateScratch(RegSize_32);
        switch (condition)
        {
          case Condition::NotEqual:
          case Condition::Equal:
          case Condition::Overflow:
          case Condition::Greater:
          case Condition::GreaterEqual:
          case Condition::LessEqual:
          case Condition::Less:
          case Condition::Above:
          case Condition::AboveEqual:
          case Condition::Below:
          case Condition::BelowEqual:
          {
            EmitCmp(lhs.GetHostRegister(), rhs);
            EmitSetConditionResult(take_branch.GetHostRegister(), take_branch.size, condition);
          }
          break;

          case Condition::Negative:
          case Condition::PositiveOrZero:
          case Condition::NotZero:
          case Condition::Zero:
          {
            Assert(!rhs.IsValid() || (rhs.IsConstant() && rhs.GetS64ConstantValue() == 0));
            EmitTest(lhs.GetHostRegister(), lhs);
            EmitSetConditionResult(take_branch.GetHostRegister(), take_branch.size, condition);
          }
          break;

          default:
            UnreachableCode();
            break;
        }
      }
    }

    // save the old PC if we want to
    if (lr_reg != Reg::count && lr_reg != Reg::zero)
    {
      // Can't cache because we have two branches. Load delay cancel is due to the immediate flush afterwards,
      // if we don't cancel it, at the end of the instruction the value we write can be overridden.
      EmitCancelInterpreterLoadDelayForReg(lr_reg);
      EmitStoreGuestRegister(lr_reg, next_pc);

      // now invalidate lr because it was possibly written in the branch
      m_register_cache.InvalidateGuestRegister(lr_reg);
    }

    // we don't need to test the address of constant branches unless they're definitely misaligned, which would be
    // strange.
    if (g_settings.cpu_recompiler_memory_exceptions &&
        (!branch_target.IsConstant() || (branch_target.constant_value & 0x3) != 0))
    {
      LabelType branch_okay;

      if (branch_target.IsConstant())
      {
        Log_WarningPrintf("Misaligned constant target branch 0x%08X, this is strange",
                          Truncate32(branch_target.constant_value));
      }
      else
      {
        // check the alignment of the target
        EmitTest(branch_target.host_reg, Value::FromConstantU32(0x3));
        EmitConditionalBranch(Condition::Zero, false, &branch_okay);
      }

      // exception exit for misaligned target
      m_register_cache.PushState();
      EmitBranch(GetCurrentFarCodePointer());
      EmitBindLabel(&branch_okay);

      SwitchToFarCode();
      EmitStoreCPUStructField(offsetof(State, cop0_regs.BadVaddr), branch_target);
      EmitFunctionCall(
        nullptr, static_cast<void (*)(u32, u32)>(&CPU::RaiseException),
        Value::FromConstantU32(Cop0Registers::CAUSE::MakeValueForException(Exception::AdEL, false, false, 0)),
        branch_target);
      EmitExceptionExit();
      SwitchToNearCode();

      m_register_cache.PopState();
    }

    if (can_link_block)
    {
      // if it's an in-block branch, compile the delay slot now
      // TODO: Make this more optimal by moving the condition down if it's a nop
      Assert((m_current_instruction + 1) != m_block_end);
      InstructionEpilogue(cbi);
      m_current_instruction++;
      if (!CompileInstruction(*m_current_instruction))
        return false;

      // flush all regs since we're at the end of the block now
      BlockEpilogue();
      m_block_linked = true;

      // check downcount
      Value pending_ticks = m_register_cache.AllocateScratch(RegSize_32);
      Value downcount = m_register_cache.AllocateScratch(RegSize_32);
      EmitLoadCPUStructField(pending_ticks.GetHostRegister(), RegSize_32, offsetof(State, pending_ticks));
      EmitLoadCPUStructField(downcount.GetHostRegister(), RegSize_32, offsetof(State, downcount));

      // pending < downcount
      LabelType return_to_dispatcher;

      if (condition != Condition::Always)
      {
        EmitBranchIfBitClear(take_branch.GetHostRegister(), take_branch.size, 0, &branch_not_taken);
        m_register_cache.PushState();
        {
          WriteNewPC(branch_target, false);
          EmitConditionalBranch(Condition::GreaterEqual, false, pending_ticks.GetHostRegister(), downcount,
                                &return_to_dispatcher);

          // we're committed at this point :D
          EmitEndBlock(true, false);

          const void* jump_pointer = GetCurrentCodePointer();
          const void* resolve_pointer = GetCurrentFarCodePointer();
          EmitBranch(resolve_pointer);
          const u32 jump_size = static_cast<u32>(static_cast<const char*>(GetCurrentCodePointer()) -
                                                 static_cast<const char*>(jump_pointer));
          SwitchToFarCode();

          EmitBeginBlock(true);
          EmitFunctionCall(nullptr, &CPU::Recompiler::Thunks::ResolveBranch, Value::FromConstantPtr(m_block),
                           Value::FromConstantPtr(jump_pointer), Value::FromConstantPtr(resolve_pointer),
                           Value::FromConstantU32(jump_size));
          EmitEndBlock(true, true);
        }
        m_register_cache.PopState();

        SwitchToNearCode();
        EmitBindLabel(&branch_not_taken);
      }

      m_register_cache.PushState();

      if (condition != Condition::Always)
      {
        WriteNewPC(next_pc, true);
      }
      else
      {
        WriteNewPC(branch_target, true);
      }

      EmitConditionalBranch(Condition::GreaterEqual, false, pending_ticks.GetHostRegister(), downcount,
                            &return_to_dispatcher);

      EmitEndBlock(true, false);

      const void* jump_pointer = GetCurrentCodePointer();
      const void* resolve_pointer = GetCurrentFarCodePointer();
      EmitBranch(GetCurrentFarCodePointer());
      const u32 jump_size =
        static_cast<u32>(static_cast<const char*>(GetCurrentCodePointer()) - static_cast<const char*>(jump_pointer));
      SwitchToFarCode();

      EmitBeginBlock(true);
      EmitFunctionCall(nullptr, &CPU::Recompiler::Thunks::ResolveBranch, Value::FromConstantPtr(m_block),
                       Value::FromConstantPtr(jump_pointer), Value::FromConstantPtr(resolve_pointer),
                       Value::FromConstantU32(jump_size));
      EmitEndBlock(true, true);

      m_register_cache.PopState();

      SwitchToNearCode();
      EmitBindLabel(&return_to_dispatcher);
      EmitEndBlock(true, true);
    }
    else
    {
      if (condition != Condition::Always)
      {
        // branch taken path - modify the next pc
        EmitBindLabel(&branch_taken);
        EmitCopyValue(next_pc.GetHostRegister(), branch_target);

        // converge point
        EmitBindLabel(&branch_not_taken);
        WriteNewPC(next_pc, true);
      }
      else
      {
        // next_pc is not used for unconditional branches
        WriteNewPC(branch_target, true);
      }

      InstructionEpilogue(cbi);
    }

    return true;
  };

  // Compute the branch target.
  // This depends on the form of the instruction.
  switch (cbi.instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
    {
      // npc = (pc & 0xF0000000) | (target << 2)
      Value branch_target = OrValues(AndValues(CalculatePC(), Value::FromConstantU32(0xF0000000)),
                                     Value::FromConstantU32(cbi.instruction.j.target << 2));

      return DoBranch(Condition::Always, Value(), Value(),
                      (cbi.instruction.op == InstructionOp::jal) ? Reg::ra : Reg::count, std::move(branch_target));
    }

    case InstructionOp::funct:
    {
      if (cbi.instruction.r.funct == InstructionFunct::jr || cbi.instruction.r.funct == InstructionFunct::jalr)
      {
        // npc = rs, link to rt
        Value branch_target = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
        return DoBranch(Condition::Always, Value(), Value(),
                        (cbi.instruction.r.funct == InstructionFunct::jalr) ? cbi.instruction.r.rd : Reg::count,
                        std::move(branch_target));
      }
      else if (cbi.instruction.r.funct == InstructionFunct::syscall ||
               cbi.instruction.r.funct == InstructionFunct::break_)
      {
        const Exception excode =
          (cbi.instruction.r.funct == InstructionFunct::syscall) ? Exception::Syscall : Exception::BP;
        GenerateExceptionExit(cbi, excode);
        InstructionEpilogue(cbi);
        return true;
      }
      else
      {
        UnreachableCode();
        return false;
      }
    }

    case InstructionOp::beq:
    case InstructionOp::bne:
    {
      // npc = pc + (sext(imm) << 2)
      Value branch_target = CalculatePC(cbi.instruction.i.imm_sext32() << 2);

      // beq zero, zero, addr -> unconditional branch
      if (cbi.instruction.op == InstructionOp::beq && cbi.instruction.i.rs == Reg::zero &&
          cbi.instruction.i.rt == Reg::zero)
      {
        return DoBranch(Condition::Always, Value(), Value(), Reg::count, std::move(branch_target));
      }
      else
      {
        // branch <- rs op rt
        Value lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);
        Value rhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rt);
        const Condition condition = (cbi.instruction.op == InstructionOp::beq) ? Condition::Equal : Condition::NotEqual;
        return DoBranch(condition, lhs, rhs, Reg::count, std::move(branch_target));
      }
    }

    case InstructionOp::bgtz:
    case InstructionOp::blez:
    {
      // npc = pc + (sext(imm) << 2)
      Value branch_target = CalculatePC(cbi.instruction.i.imm_sext32() << 2);

      // branch <- rs op 0
      Value lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);

      const Condition condition =
        (cbi.instruction.op == InstructionOp::bgtz) ? Condition::Greater : Condition::LessEqual;
      return DoBranch(condition, lhs, Value::FromConstantU32(0), Reg::count, std::move(branch_target));
    }

    case InstructionOp::b:
    {
      // npc = pc + (sext(imm) << 2)
      Value branch_target = CalculatePC(cbi.instruction.i.imm_sext32() << 2);

      const u8 rt = static_cast<u8>(cbi.instruction.i.rt.GetValue());
      const bool bgez = ConvertToBoolUnchecked(rt & u8(1));
      const Condition condition = (bgez && cbi.instruction.r.rs == Reg::zero) ?
                                    Condition::Always :
                                    (bgez ? Condition::PositiveOrZero : Condition::Negative);
      const bool link = (rt & u8(0x1E)) == u8(0x10);

      // Read has to happen before the link as the compare can use ra.
      Value lhs;
      if (condition != Condition::Always)
        lhs = m_register_cache.ReadGuestRegisterToScratch(cbi.instruction.i.rs);

      // The return address is always written if link is set, regardless of whether the branch is taken.
      if (link)
      {
        EmitCancelInterpreterLoadDelayForReg(Reg::ra);
        m_register_cache.WriteGuestRegister(Reg::ra, CalculatePC(4));
      }

      return DoBranch(condition, lhs, Value(), Reg::count, std::move(branch_target));
    }

    default:
      UnreachableCode();
      return false;
  }
}

bool CodeGenerator::Compile_lui(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  if (g_settings.UsingPGXPCPUMode())
    EmitFunctionCall(nullptr, &PGXP::CPU_LUI, Value::FromConstantU32(cbi.instruction.bits));

  // rt <- (imm << 16)
  const u32 value = cbi.instruction.i.imm_zext32() << 16;
  m_register_cache.WriteGuestRegister(cbi.instruction.i.rt, Value::FromConstantU32(value));
  SpeculativeWriteReg(cbi.instruction.i.rt, value);

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

        const Cop0Reg reg = static_cast<Cop0Reg>(cbi.instruction.r.rd.GetValue());
        switch (reg)
        {
          case Cop0Reg::BPC:
            offset = offsetof(State, cop0_regs.BPC);
            break;

          case Cop0Reg::BPCM:
            offset = offsetof(State, cop0_regs.BPCM);
            break;

          case Cop0Reg::BDA:
            offset = offsetof(State, cop0_regs.BDA);
            break;

          case Cop0Reg::BDAM:
            offset = offsetof(State, cop0_regs.BDAM);
            break;

          case Cop0Reg::DCIC:
            offset = offsetof(State, cop0_regs.dcic.bits);
            write_mask = Cop0Registers::DCIC::WRITE_MASK;
            break;

          case Cop0Reg::JUMPDEST:
            offset = offsetof(State, cop0_regs.TAR);
            write_mask = 0;
            break;

          case Cop0Reg::BadVaddr:
            offset = offsetof(State, cop0_regs.BadVaddr);
            write_mask = 0;
            break;

          case Cop0Reg::SR:
            offset = offsetof(State, cop0_regs.sr.bits);
            write_mask = Cop0Registers::SR::WRITE_MASK;
            break;

          case Cop0Reg::CAUSE:
            offset = offsetof(State, cop0_regs.cause.bits);
            write_mask = Cop0Registers::CAUSE::WRITE_MASK;
            break;

          case Cop0Reg::EPC:
            offset = offsetof(State, cop0_regs.EPC);
            write_mask = 0;
            break;

          case Cop0Reg::PRID:
            offset = offsetof(State, cop0_regs.PRID);
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

          if (g_settings.UsingPGXPCPUMode())
            EmitFunctionCall(nullptr, &PGXP::CPU_MFC0, Value::FromConstantU32(cbi.instruction.bits), value);

          m_register_cache.WriteGuestRegisterDelayed(cbi.instruction.r.rt, std::move(value));

          if (reg == Cop0Reg::SR)
            SpeculativeWriteReg(cbi.instruction.r.rt, m_speculative_constants.cop0_sr);
          else
            SpeculativeWriteReg(cbi.instruction.r.rt, std::nullopt);
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
              Value masked_value = AndValues(value, Value::FromConstantU32(write_mask));
              {
                Value old_value = m_register_cache.AllocateScratch(RegSize_32);
                EmitLoadCPUStructField(old_value.GetHostRegister(), RegSize_32, offset);
                EmitAnd(old_value.GetHostRegister(), old_value.GetHostRegister(), Value::FromConstantU32(~write_mask));
                OrValueInPlace(masked_value, old_value);
              }

              if (g_settings.UsingPGXPCPUMode())
              {
                EmitFunctionCall(nullptr, &PGXP::CPU_MTC0, Value::FromConstantU32(cbi.instruction.bits), masked_value,
                                 value);
              }
              value = std::move(masked_value);
            }
            else
            {
              if (g_settings.UsingPGXPCPUMode())
                EmitFunctionCall(nullptr, &PGXP::CPU_MTC0, Value::FromConstantU32(cbi.instruction.bits), value, value);
            }

            if (reg == Cop0Reg::SR)
              m_speculative_constants.cop0_sr = SpeculativeReadReg(cbi.instruction.r.rt);

            // changing SR[Isc] needs to update fastmem views
            if (reg == Cop0Reg::SR && g_settings.IsUsingFastmem())
            {
              LabelType skip_fastmem_update;
              Value old_value = m_register_cache.AllocateScratch(RegSize_32);
              EmitLoadCPUStructField(old_value.host_reg, RegSize_32, offset);
              EmitStoreCPUStructField(offset, value);
              EmitXor(old_value.host_reg, old_value.host_reg, value);
              EmitBranchIfBitClear(old_value.host_reg, RegSize_32, 16, &skip_fastmem_update);
              m_register_cache.InhibitAllocation();
              EmitFunctionCall(nullptr, &UpdateFastmemBase, m_register_cache.GetCPUPtr());
              EmitUpdateFastmemBase();
              EmitBindLabel(&skip_fastmem_update);
              m_register_cache.UninhibitAllocation();
            }
            else
            {
              EmitStoreCPUStructField(offset, value);
            }
          }
        }

        if (cbi.instruction.cop.CommonOp() == CopCommonInstruction::mtcn)
        {
          if (reg == Cop0Reg::CAUSE || reg == Cop0Reg::SR)
          {
            // Emit an interrupt check on load of CAUSE/SR.
            Value sr_value = m_register_cache.AllocateScratch(RegSize_32);
            Value cause_value = m_register_cache.AllocateScratch(RegSize_32);

            // m_cop0_regs.sr.IEc && ((m_cop0_regs.cause.Ip & m_cop0_regs.sr.Im) != 0)
            LabelType no_interrupt;
            EmitLoadCPUStructField(sr_value.host_reg, sr_value.size, offsetof(State, cop0_regs.sr.bits));
            EmitLoadCPUStructField(cause_value.host_reg, cause_value.size, offsetof(State, cop0_regs.cause.bits));
            EmitBranchIfBitClear(sr_value.host_reg, sr_value.size, 0, &no_interrupt);
            m_register_cache.InhibitAllocation();
            EmitAnd(sr_value.host_reg, sr_value.host_reg, cause_value);
            EmitTest(sr_value.host_reg, Value::FromConstantU32(0xFF00));
            EmitConditionalBranch(Condition::Zero, false, &no_interrupt);
            EmitStoreCPUStructField(offsetof(State, downcount), Value::FromConstantU32(0));
            EmitBindLabel(&no_interrupt);
            m_register_cache.UninhibitAllocation();
          }
          else if (reg == Cop0Reg::DCIC && g_settings.cpu_recompiler_memory_exceptions)
          {
            Value dcic_value = m_register_cache.AllocateScratch(RegSize_32);
            m_register_cache.InhibitAllocation();

            // if ((dcic & master_enable_bits) != master_enable_bits) goto not_enabled;
            LabelType not_enabled;
            EmitLoadCPUStructField(dcic_value.GetHostRegister(), dcic_value.size, offsetof(State, cop0_regs.dcic.bits));
            EmitAnd(dcic_value.GetHostRegister(), dcic_value.GetHostRegister(),
                    Value::FromConstantU32(Cop0Registers::DCIC::MASTER_ENABLE_BITS));
            EmitConditionalBranch(Condition::NotEqual, false, dcic_value.host_reg,
                                  Value::FromConstantU32(Cop0Registers::DCIC::MASTER_ENABLE_BITS), &not_enabled);

            // if ((dcic & breakpoint_bits) == 0) goto not_enabled;
            EmitLoadCPUStructField(dcic_value.GetHostRegister(), dcic_value.size, offsetof(State, cop0_regs.dcic.bits));
            EmitTest(dcic_value.GetHostRegister(),
                     Value::FromConstantU32(Cop0Registers::DCIC::ANY_BREAKPOINTS_ENABLED_BITS));
            EmitConditionalBranch(Condition::Zero, false, &not_enabled);

            // update dispatcher flag, if enabled, exit block
            EmitFunctionCall(nullptr, &UpdateDebugDispatcherFlag);
            EmitLoadCPUStructField(dcic_value.GetHostRegister(), RegSize_8, offsetof(State, use_debug_dispatcher));
            EmitBranchIfBitClear(dcic_value.GetHostRegister(), RegSize_8, 0, &not_enabled);

            m_register_cache.UninhibitAllocation();

            // exit block early if enabled
            EmitBranch(GetCurrentFarCodePointer());
            SwitchToFarCode();
            WriteNewPC(CalculatePC(), false);
            EmitExceptionExit();
            SwitchToNearCode();

            EmitBindLabel(&not_enabled);
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
        EmitLoadCPUStructField(sr.host_reg, RegSize_32, offsetof(State, cop0_regs.sr.bits));
        {
          Value new_mode_bits = m_register_cache.AllocateScratch(RegSize_32);
          EmitShr(new_mode_bits.host_reg, sr.host_reg, new_mode_bits.size, Value::FromConstantU32(2));
          EmitAnd(new_mode_bits.host_reg, new_mode_bits.host_reg, Value::FromConstantU32(mode_bits_mask));
          EmitAnd(sr.host_reg, sr.host_reg, Value::FromConstantU32(~mode_bits_mask));
          EmitOr(sr.host_reg, sr.host_reg, new_mode_bits);
        }

        EmitStoreCPUStructField(offsetof(State, cop0_regs.sr.bits), sr);

        Value cause_value = m_register_cache.AllocateScratch(RegSize_32);
        EmitLoadCPUStructField(cause_value.host_reg, cause_value.size, offsetof(State, cop0_regs.cause.bits));

        LabelType no_interrupt;
        EmitAnd(sr.host_reg, sr.host_reg, cause_value);
        EmitTest(sr.host_reg, Value::FromConstantU32(0xFF00));
        EmitConditionalBranch(Condition::Zero, false, &no_interrupt);
        m_register_cache.InhibitAllocation();
        EmitStoreCPUStructField(offsetof(State, downcount), Value::FromConstantU32(0));
        EmitBindLabel(&no_interrupt);
        m_register_cache.UninhibitAllocation();

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
      EmitFunctionCall(&value, &GTE::ReadRegister, Value::FromConstantU32(index));
    }
    break;

    default:
    {
      EmitLoadCPUStructField(value.host_reg, RegSize_32, State::GTERegisterOffset(index));
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
      EmitStoreCPUStructField(State::GTERegisterOffset(index), temp);
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
      EmitStoreCPUStructField(State::GTERegisterOffset(index), temp);
      return;
    }
    break;

    case 15: // SXY3
    {
      // writing to SXYP pushes to the FIFO
      Value temp = m_register_cache.AllocateScratch(RegSize_32);

      // SXY0 <- SXY1
      EmitLoadCPUStructField(temp.host_reg, RegSize_32, State::GTERegisterOffset(13));
      EmitStoreCPUStructField(State::GTERegisterOffset(12), temp);

      // SXY1 <- SXY2
      EmitLoadCPUStructField(temp.host_reg, RegSize_32, State::GTERegisterOffset(14));
      EmitStoreCPUStructField(State::GTERegisterOffset(13), temp);

      // SXY2 <- SXYP
      EmitStoreCPUStructField(State::GTERegisterOffset(14), value);
      return;
    }
    break;

    case 28: // IRGB
    case 30: // LZCS
    case 63: // FLAG
    {
      EmitFunctionCall(nullptr, &GTE::WriteRegister, Value::FromConstantU32(index), value);
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
      EmitStoreCPUStructField(State::GTERegisterOffset(index), value);
      return;
    }
  }
}

bool CodeGenerator::Compile_cop2(const CodeBlockInstruction& cbi)
{
  if (cbi.instruction.op == InstructionOp::lwc2 || cbi.instruction.op == InstructionOp::swc2)
  {
    StallUntilGTEComplete();
    InstructionPrologue(cbi, 1);

    const u32 reg = static_cast<u32>(cbi.instruction.i.rt.GetValue());
    Value address = AddValues(m_register_cache.ReadGuestRegister(cbi.instruction.i.rs),
                              Value::FromConstantU32(cbi.instruction.i.imm_sext32()), false);
    SpeculativeValue spec_address = SpeculativeReadReg(cbi.instruction.i.rs);
    if (spec_address)
      spec_address = *spec_address + cbi.instruction.i.imm_sext32();

    if (cbi.instruction.op == InstructionOp::lwc2)
    {
      Value value = EmitLoadGuestMemory(cbi, address, spec_address, RegSize_32);
      DoGTERegisterWrite(reg, value);

      if (g_settings.gpu_pgxp_enable)
        EmitFunctionCall(nullptr, PGXP::CPU_LWC2, Value::FromConstantU32(cbi.instruction.bits), value, address);
    }
    else
    {
      Value value = DoGTERegisterRead(reg);
      EmitStoreGuestMemory(cbi, address, spec_address, RegSize_32, value);

      if (g_settings.gpu_pgxp_enable)
        EmitFunctionCall(nullptr, PGXP::CPU_SWC2, Value::FromConstantU32(cbi.instruction.bits), value, address);

      SpeculativeValue spec_base = SpeculativeReadReg(cbi.instruction.i.rs);
      if (spec_base)
        SpeculativeWriteMemory(*spec_address, std::nullopt);
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

        StallUntilGTEComplete();
        InstructionPrologue(cbi, 1);

        Value value = DoGTERegisterRead(reg);

        // PGXP done first here before ownership is transferred.
        if (g_settings.gpu_pgxp_enable)
        {
          EmitFunctionCall(
            nullptr, (cbi.instruction.cop.CommonOp() == CopCommonInstruction::cfcn) ? PGXP::CPU_CFC2 : PGXP::CPU_MFC2,
            Value::FromConstantU32(cbi.instruction.bits), value, value);
        }

        m_register_cache.WriteGuestRegisterDelayed(cbi.instruction.r.rt, std::move(value));
        SpeculativeWriteReg(cbi.instruction.r.rt, std::nullopt);

        InstructionEpilogue(cbi);
        return true;
      }

      case CopCommonInstruction::mtcn:
      case CopCommonInstruction::ctcn:
      {
        const u32 reg = static_cast<u32>(cbi.instruction.r.rd.GetValue()) +
                        ((cbi.instruction.cop.CommonOp() == CopCommonInstruction::ctcn) ? 32 : 0);

        StallUntilGTEComplete();
        InstructionPrologue(cbi, 1);

        Value value = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
        DoGTERegisterWrite(reg, value);

        if (g_settings.gpu_pgxp_enable)
        {
          EmitFunctionCall(
            nullptr, (cbi.instruction.cop.CommonOp() == CopCommonInstruction::ctcn) ? PGXP::CPU_CTC2 : PGXP::CPU_MTC2,
            Value::FromConstantU32(cbi.instruction.bits), value, value);
        }

        InstructionEpilogue(cbi);
        return true;
      }

      default:
        return Compile_Fallback(cbi);
    }
  }
  else
  {
    TickCount func_ticks;
    GTE::InstructionImpl func = GTE::GetInstructionImpl(cbi.instruction.bits, &func_ticks);

    // forward everything to the GTE.
    StallUntilGTEComplete();
    InstructionPrologue(cbi, 1);

    Value instruction_bits = Value::FromConstantU32(cbi.instruction.bits & GTE::Instruction::REQUIRED_BITS_MASK);
    EmitFunctionCall(nullptr, func, instruction_bits);
    AddGTETicks(func_ticks);

    InstructionEpilogue(cbi);
    return true;
  }
}

void CodeGenerator::InitSpeculativeRegs()
{
  for (u8 i = 0; i < static_cast<u8>(Reg::count); i++)
    m_speculative_constants.regs[i] = g_state.regs.r[i];

  m_speculative_constants.cop0_sr = g_state.cop0_regs.sr.bits;
}

void CodeGenerator::InvalidateSpeculativeValues()
{
  m_speculative_constants.regs.fill(std::nullopt);
  m_speculative_constants.memory.clear();
  m_speculative_constants.cop0_sr.reset();
}

CodeGenerator::SpeculativeValue CodeGenerator::SpeculativeReadReg(Reg reg)
{
  return m_speculative_constants.regs[static_cast<u8>(reg)];
}

void CodeGenerator::SpeculativeWriteReg(Reg reg, SpeculativeValue value)
{
  m_speculative_constants.regs[static_cast<u8>(reg)] = value;
}

CodeGenerator::SpeculativeValue CodeGenerator::SpeculativeReadMemory(VirtualMemoryAddress address)
{
  PhysicalMemoryAddress phys_addr = address & PHYSICAL_MEMORY_ADDRESS_MASK;

  auto it = m_speculative_constants.memory.find(address);
  if (it != m_speculative_constants.memory.end())
    return it->second;

  u32 value;
  if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION)
  {
    u32 scratchpad_offset = phys_addr & DCACHE_OFFSET_MASK;
    std::memcpy(&value, &CPU::g_state.dcache[scratchpad_offset], sizeof(value));
    return value;
  }

  if (Bus::IsRAMAddress(phys_addr))
  {
    u32 ram_offset = phys_addr & Bus::g_ram_mask;
    std::memcpy(&value, &Bus::g_ram[ram_offset], sizeof(value));
    return value;
  }

  return std::nullopt;
}

void CodeGenerator::SpeculativeWriteMemory(u32 address, SpeculativeValue value)
{
  PhysicalMemoryAddress phys_addr = address & PHYSICAL_MEMORY_ADDRESS_MASK;

  auto it = m_speculative_constants.memory.find(address);
  if (it != m_speculative_constants.memory.end())
  {
    it->second = value;
    return;
  }

  if ((phys_addr & DCACHE_LOCATION_MASK) == DCACHE_LOCATION || Bus::IsRAMAddress(phys_addr))
    m_speculative_constants.memory.emplace(address, value);
}

bool CodeGenerator::SpeculativeIsCacheIsolated()
{
  if (!m_speculative_constants.cop0_sr.has_value())
    return false;

  const Cop0Registers::SR sr{m_speculative_constants.cop0_sr.value()};
  return sr.Isc;
}

} // namespace CPU::Recompiler
