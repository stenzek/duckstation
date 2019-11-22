#include "cpu_recompiler_code_generator.h"
#include "YBaseLib/Log.h"
#include "cpu_disasm.h"
Log_SetChannel(CPU::Recompiler);

namespace CPU::Recompiler {

CodeGenerator::CodeGenerator(Core* cpu, JitCodeBuffer* code_buffer, const ASMFunctions& asm_functions)
  : m_cpu(cpu), m_code_buffer(code_buffer), m_asm_functions(asm_functions), m_register_cache(*this),
    m_near_emitter(code_buffer->GetFreeCodeSpace(), code_buffer->GetFreeCodePointer()),
    m_far_emitter(code_buffer->GetFreeFarCodeSpace(), code_buffer->GetFreeFarCodePointer()), m_emit(&m_near_emitter)
{
  InitHostRegs();
}

CodeGenerator::~CodeGenerator() = default;

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
      result = Compile_BitwiseImmediate(cbi);
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
    case InstructionOp::beq:
    case InstructionOp::bne:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
      result = Compile_Branch(cbi);
      break;

    case InstructionOp::lui:
      result = Compile_lui(cbi);
      break;

    case InstructionOp::addiu:
      result = Compile_addiu(cbi);
      break;

    case InstructionOp::funct:
    {
      switch (cbi.instruction.r.funct)
      {
        case InstructionFunct::sll:
        case InstructionFunct::srl:
        case InstructionFunct::sra:
          result = Compile_ShiftImmediate(cbi);
          break;

        case InstructionFunct::sllv:
        case InstructionFunct::srlv:
        case InstructionFunct::srav:
          result = Compile_ShiftVariable(cbi);
          break;

        case InstructionFunct::mfhi:
        case InstructionFunct::mflo:
        case InstructionFunct::mthi:
        case InstructionFunct::mtlo:
          result = Compile_MoveHiLo(cbi);
          break;

        case InstructionFunct::mult:
        case InstructionFunct::multu:
          result = Compile_Multiply(cbi);
          break;

        case InstructionFunct::jr:
        case InstructionFunct::jalr:
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

  // release temporary effective addresses
  for (Value& value : m_operand_memory_addresses)
    value.ReleaseAndClear();

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

Value CodeGenerator::AddValues(const Value& lhs, const Value& rhs)
{
  DebugAssert(lhs.size == rhs.size);
  if (lhs.IsConstant() && rhs.IsConstant())
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
  else
  {
    EmitCopyValue(res.host_reg, lhs);
    EmitAdd(res.host_reg, rhs);
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
  EmitCopyValue(res.host_reg, lhs);
  if (!rhs.HasConstantValue(0))
    EmitShl(res.host_reg, res.size, rhs);
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
  EmitCopyValue(res.host_reg, lhs);
  if (!rhs.HasConstantValue(0))
    EmitShr(res.host_reg, res.size, rhs);
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
  EmitCopyValue(res.host_reg, lhs);
  if (!rhs.HasConstantValue(0))
    EmitSar(res.host_reg, res.size, rhs);
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

  EmitCopyValue(res.host_reg, lhs);
  EmitOr(res.host_reg, rhs);
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
    EmitXor(res.host_reg, res);
    return res;
  }

  EmitCopyValue(res.host_reg, lhs);
  EmitAnd(res.host_reg, rhs);
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

  EmitCopyValue(res.host_reg, lhs);
  EmitXor(res.host_reg, rhs);
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

  // sync m_current_instruction_pc so we can simply add to it
  SyncCurrentInstructionPC();

  // and the same for m_regs.pc
  SyncPC();

  EmitAddCPUStructField(offsetof(Core, m_regs.npc), Value::FromConstantU32(4));
}

void CodeGenerator::BlockEpilogue()
{
#if defined(_DEBUG) && defined(Y_CPU_X64)
  m_emit->nop();
#endif

  m_register_cache.FlushAllGuestRegisters(true, true);
  if (m_register_cache.HasLoadDelay())
    m_register_cache.WriteLoadDelayToCPU(true);

  // if the last instruction wasn't a fallback, we need to add its fetch
  if (m_delayed_pc_add > 0)
  {
    EmitAddCPUStructField(offsetof(Core, m_regs.npc), Value::FromConstantU32(m_delayed_pc_add));
    m_delayed_pc_add = 0;
  }

  AddPendingCycles();

  // TODO: correct value for is_branch_delay_slot - branches in branch delay slot.
  EmitStoreCPUStructField(offsetof(Core, m_next_instruction_is_branch_delay_slot), Value::FromConstantU8(0));
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

  if (cbi.is_branch_delay_slot)
  {
    // m_regs.pc should be synced for the next block, as the branch wrote to npc
    SyncCurrentInstructionPC();
    SyncPC();

    // m_current_instruction_in_branch_delay_slot = true
    EmitStoreCPUStructField(offsetof(Core, m_current_instruction_in_branch_delay_slot), Value::FromConstantU8(1));
    m_current_instruction_in_branch_delay_slot_dirty = true;
  }

  if (!CanInstructionTrap(cbi.instruction, m_block->key.user_mode) && !force_sync)
  {
    // Defer updates for non-faulting instructions.
    m_delayed_pc_add += INSTRUCTION_SIZE;
    m_delayed_cycles_add += cycles;
    return;
  }

  if (m_delayed_pc_add > 0)
  {
    // m_current_instruction_pc += m_delayed_pc_add
    EmitAddCPUStructField(offsetof(Core, m_current_instruction_pc), Value::FromConstantU32(m_delayed_pc_add));

    // m_regs.pc += m_delayed_pc_add
    EmitAddCPUStructField(offsetof(Core, m_regs.pc), Value::FromConstantU32(m_delayed_pc_add));

    // m_regs.npc += m_delayed_pc_add
    // TODO: This can go once we recompile branch instructions and unconditionally set npc
    EmitAddCPUStructField(offsetof(Core, m_regs.npc), Value::FromConstantU32(m_delayed_pc_add));

    m_delayed_pc_add = 0;
  }

  if (!cbi.is_branch_instruction)
    m_delayed_pc_add = INSTRUCTION_SIZE;

  m_delayed_cycles_add += cycles;
  AddPendingCycles();
}

void CodeGenerator::InstructionEpilogue(const CodeBlockInstruction& cbi)
{
  m_register_cache.UpdateLoadDelay();

  // copy if the previous instruction was a load, reset the current value on the next instruction
  if (m_next_load_delay_dirty)
  {
    Log_DebugPrint("Emitting delay slot flush (with move next)");
    EmitDelaySlotUpdate(false, false, true);
    m_next_load_delay_dirty = false;
    m_load_delay_dirty = true;
  }
  else if (m_load_delay_dirty)
  {
    Log_DebugPrint("Emitting delay slot flush");
    EmitDelaySlotUpdate(true, false, false);
    m_load_delay_dirty = false;
  }
}

void CodeGenerator::SyncCurrentInstructionPC()
{
  // m_current_instruction_pc = m_regs.pc
  Value pc_value = m_register_cache.AllocateScratch(RegSize_32);
  EmitLoadCPUStructField(pc_value.host_reg, RegSize_32, offsetof(Core, m_regs.pc));
  EmitStoreCPUStructField(offsetof(Core, m_current_instruction_pc), pc_value);
}

void CodeGenerator::SyncPC()
{
  // m_regs.pc = m_regs.npc
  Value npc_value = m_register_cache.AllocateScratch(RegSize_32);
  EmitLoadCPUStructField(npc_value.host_reg, RegSize_32, offsetof(Core, m_regs.npc));
  EmitStoreCPUStructField(offsetof(Core, m_regs.pc), npc_value);
}

void CodeGenerator::AddPendingCycles()
{
  if (m_delayed_cycles_add == 0)
    return;

  EmitAddCPUStructField(offsetof(Core, m_pending_ticks), Value::FromConstantU32(m_delayed_cycles_add));
  EmitAddCPUStructField(offsetof(Core, m_downcount), Value::FromConstantU32(~u32(m_delayed_cycles_add - 1)));
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

bool CodeGenerator::Compile_BitwiseImmediate(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // rt <- rs op zext(imm)
  Value rs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs);
  Value imm = Value::FromConstantU32(cbi.instruction.i.imm_zext32());
  Value result;
  switch (cbi.instruction.op)
  {
    case InstructionOp::ori:
      result = OrValues(rs, imm);
      break;

    case InstructionOp::andi:
      result = AndValues(rs, imm);
      break;

    case InstructionOp::xori:
      result = XorValues(rs, imm);
      break;

    default:
      UnreachableCode();
      break;
  }

  m_register_cache.WriteGuestRegister(cbi.instruction.i.rt, std::move(result));

  InstructionEpilogue(cbi);
  return true;
}

bool CodeGenerator::Compile_ShiftImmediate(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // rd <- rt op shamt
  Value rt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
  Value shamt = Value::FromConstantU32(cbi.instruction.r.shamt);
  Value result;
  switch (cbi.instruction.r.funct)
  {
    case InstructionFunct::sll:
      result = ShlValues(rt, shamt);
      break;

    case InstructionFunct::srl:
      result = ShrValues(rt, shamt);
      break;

    case InstructionFunct::sra:
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

bool CodeGenerator::Compile_ShiftVariable(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // rd <- rt op (rs & 0x1F)
  Value rt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rt);
  Value shamt = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
  if constexpr (!SHIFTS_ARE_IMPLICITLY_MASKED)
    EmitAnd(shamt.host_reg, Value::FromConstantU32(0x1F));

  Value result;
  switch (cbi.instruction.r.funct)
  {
    case InstructionFunct::sllv:
      result = ShlValues(rt, shamt);
      break;

    case InstructionFunct::srlv:
      result = ShrValues(rt, shamt);
      break;

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
  Value address = AddValues(base, offset);

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
  Value address = AddValues(base, offset);
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

bool CodeGenerator::Compile_Branch(const CodeBlockInstruction& cbi)
{
  // Force sync since we branches are PC-relative.
  InstructionPrologue(cbi, 1, true);

  // Compute the branch target.
  // This depends on the form of the instruction.
  switch (cbi.instruction.op)
  {
    case InstructionOp::j:
    case InstructionOp::jal:
    {
      // npc = (pc & 0xF0000000) | (target << 2)
      Value branch_target =
        OrValues(AndValues(m_register_cache.ReadGuestRegister(Reg::pc, false), Value::FromConstantU32(0xF0000000)),
                 Value::FromConstantU32(cbi.instruction.j.target << 2));

      EmitBranch(Condition::Always, (cbi.instruction.op == InstructionOp::jal) ? Reg::ra : Reg::count,
                 std::move(branch_target));
    }
    break;

    case InstructionOp::funct:
    {
      Assert(cbi.instruction.r.funct == InstructionFunct::jr || cbi.instruction.r.funct == InstructionFunct::jalr);

      // npc = rs, link to rt
      Value branch_target = m_register_cache.ReadGuestRegister(cbi.instruction.r.rs);
      EmitBranch(Condition::Always,
                 (cbi.instruction.r.funct == InstructionFunct::jalr) ? cbi.instruction.r.rd : Reg::count,
                 std::move(branch_target));
    }
    break;

    case InstructionOp::beq:
    case InstructionOp::bne:
    case InstructionOp::bgtz:
    case InstructionOp::blez:
    {
      // npc = pc + (sext(imm) << 2)
      Value branch_target = AddValues(m_register_cache.ReadGuestRegister(Reg::pc, false),
                                      Value::FromConstantU32(cbi.instruction.i.imm_sext32() << 2));

      // branch <- rs op rt
      Value lhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rs, true, true);
      Value rhs = m_register_cache.ReadGuestRegister(cbi.instruction.i.rt);
      EmitCmp(lhs.host_reg, rhs);

      Condition condition;
      switch (cbi.instruction.op)
      {
        case InstructionOp::beq:
          condition = Condition::Equal;
          break;
        case InstructionOp::bne:
          condition = Condition::NotEqual;
          break;
        case InstructionOp::bgtz:
          condition = Condition::GreaterThanZero;
          break;
        case InstructionOp::blez:
          condition = Condition::LessOrEqualToZero;
          break;
        default:
          condition = Condition::Always;
          break;
      }

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

bool CodeGenerator::Compile_addiu(const CodeBlockInstruction& cbi)
{
  InstructionPrologue(cbi, 1);

  // rt <- rs + sext(imm)
  m_register_cache.WriteGuestRegister(cbi.instruction.i.rt,
                                      AddValues(m_register_cache.ReadGuestRegister(cbi.instruction.i.rs),
                                                Value::FromConstantU32(cbi.instruction.i.imm_sext32())));

  InstructionEpilogue(cbi);
  return true;
}

} // namespace CPU::Recompiler
