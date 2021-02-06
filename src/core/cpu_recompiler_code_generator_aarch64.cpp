#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#include "settings.h"
#include "timing_event.h"
Log_SetChannel(CPU::Recompiler);

namespace a64 = vixl::aarch64;

namespace CPU::Recompiler {

constexpr HostReg RCPUPTR = 19;
constexpr HostReg RMEMBASEPTR = 20;
constexpr HostReg RRETURN = 0;
constexpr HostReg RARG1 = 0;
constexpr HostReg RARG2 = 1;
constexpr HostReg RARG3 = 2;
constexpr HostReg RARG4 = 3;
constexpr HostReg RSCRATCH = 8;
constexpr u64 FUNCTION_CALL_STACK_ALIGNMENT = 16;
constexpr u64 FUNCTION_CALL_SHADOW_SPACE = 32;
constexpr u64 FUNCTION_CALLEE_SAVED_SPACE_RESERVE = 80;  // 8 registers
constexpr u64 FUNCTION_CALLER_SAVED_SPACE_RESERVE = 144; // 18 registers -> 224 bytes
constexpr u64 FUNCTION_STACK_SIZE =
  FUNCTION_CALLEE_SAVED_SPACE_RESERVE + FUNCTION_CALLER_SAVED_SPACE_RESERVE + FUNCTION_CALL_SHADOW_SPACE;

// PC we return to after the end of the block
static void* s_dispatcher_return_address;

static s64 GetPCDisplacement(const void* current, const void* target)
{
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(current), 4));
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(target), 4));
  return static_cast<s64>((reinterpret_cast<ptrdiff_t>(target) - reinterpret_cast<ptrdiff_t>(current)) >> 2);
}

static const a64::WRegister GetHostReg8(HostReg reg)
{
  return a64::WRegister(reg);
}

static const a64::WRegister GetHostReg8(const Value& value)
{
  DebugAssert(value.size == RegSize_8 && value.IsInHostRegister());
  return a64::WRegister(value.host_reg);
}

static const a64::WRegister GetHostReg16(HostReg reg)
{
  return a64::WRegister(reg);
}

static const a64::WRegister GetHostReg16(const Value& value)
{
  DebugAssert(value.size == RegSize_16 && value.IsInHostRegister());
  return a64::WRegister(value.host_reg);
}

static const a64::WRegister GetHostReg32(HostReg reg)
{
  return a64::WRegister(reg);
}

static const a64::WRegister GetHostReg32(const Value& value)
{
  DebugAssert(value.size == RegSize_32 && value.IsInHostRegister());
  return a64::WRegister(value.host_reg);
}

static const a64::XRegister GetHostReg64(HostReg reg)
{
  return a64::XRegister(reg);
}

static const a64::XRegister GetHostReg64(const Value& value)
{
  DebugAssert(value.size == RegSize_64 && value.IsInHostRegister());
  return a64::XRegister(value.host_reg);
}

static const a64::XRegister GetCPUPtrReg()
{
  return GetHostReg64(RCPUPTR);
}

static const a64::XRegister GetFastmemBasePtrReg()
{
  return GetHostReg64(RMEMBASEPTR);
}

CodeGenerator::CodeGenerator(JitCodeBuffer* code_buffer)
  : m_code_buffer(code_buffer), m_register_cache(*this),
    m_near_emitter(static_cast<vixl::byte*>(code_buffer->GetFreeCodePointer()), code_buffer->GetFreeCodeSpace(),
                   a64::PositionDependentCode),
    m_far_emitter(static_cast<vixl::byte*>(code_buffer->GetFreeFarCodePointer()), code_buffer->GetFreeFarCodeSpace(),
                  a64::PositionDependentCode),
    m_emit(&m_near_emitter)
{
  // remove the temporaries from vixl's list to prevent it from using them.
  // eventually we won't use the macro assembler and this won't be a problem...
  m_near_emitter.GetScratchRegisterList()->Remove(16);
  m_near_emitter.GetScratchRegisterList()->Remove(17);
  m_far_emitter.GetScratchRegisterList()->Remove(16);
  m_far_emitter.GetScratchRegisterList()->Remove(17);
  InitHostRegs();
}

CodeGenerator::~CodeGenerator() = default;

const char* CodeGenerator::GetHostRegName(HostReg reg, RegSize size /*= HostPointerSize*/)
{
  static constexpr std::array<const char*, HostReg_Count> reg32_names = {
    {"w0",  "w1",  "w2",  "w3",  "w4",  "w5",  "w6",  "w7",  "w8",  "w9",  "w10", "w11", "w12", "w13", "w14", "w15",
     "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23", "w24", "w25", "w26", "w27", "w28", "w29", "w30", "w31"}};
  static constexpr std::array<const char*, HostReg_Count> reg64_names = {
    {"x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
     "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30", "x31"}};
  if (reg >= static_cast<HostReg>(HostReg_Count))
    return "";

  switch (size)
  {
    case RegSize_32:
      return reg32_names[reg];
    case RegSize_64:
      return reg64_names[reg];
    default:
      return "";
  }
}

void CodeGenerator::AlignCodeBuffer(JitCodeBuffer* code_buffer)
{
  code_buffer->Align(16, 0x90);
}

void CodeGenerator::InitHostRegs()
{
  // TODO: function calls mess up the parameter registers if we use them.. fix it
  // allocate nonvolatile before volatile
  m_register_cache.SetHostRegAllocationOrder(
    {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17});
  m_register_cache.SetCallerSavedHostRegs({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17});
  m_register_cache.SetCalleeSavedHostRegs({19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30});
  m_register_cache.SetCPUPtrHostReg(RCPUPTR);
}

void CodeGenerator::SwitchToFarCode()
{
  m_emit = &m_far_emitter;
}

void CodeGenerator::SwitchToNearCode()
{
  m_emit = &m_near_emitter;
}

void* CodeGenerator::GetCurrentNearCodePointer() const
{
  return static_cast<u8*>(m_code_buffer->GetFreeCodePointer()) + m_near_emitter.GetCursorOffset();
}

void* CodeGenerator::GetCurrentFarCodePointer() const
{
  return static_cast<u8*>(m_code_buffer->GetFreeFarCodePointer()) + m_far_emitter.GetCursorOffset();
}

Value CodeGenerator::GetValueInHostRegister(const Value& value, bool allow_zero_register /* = true */)
{
  if (value.IsInHostRegister())
    return Value::FromHostReg(&m_register_cache, value.host_reg, value.size);

  if (value.HasConstantValue(0) && allow_zero_register)
    return Value::FromHostReg(&m_register_cache, static_cast<HostReg>(31), value.size);

  Value new_value = m_register_cache.AllocateScratch(value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

Value CodeGenerator::GetValueInHostOrScratchRegister(const Value& value, bool allow_zero_register /* = true */)
{
  if (value.IsInHostRegister())
    return Value::FromHostReg(&m_register_cache, value.host_reg, value.size);

  if (value.HasConstantValue(0) && allow_zero_register)
    return Value::FromHostReg(&m_register_cache, static_cast<HostReg>(31), value.size);

  Value new_value = Value::FromHostReg(&m_register_cache, RSCRATCH, value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

void CodeGenerator::EmitBeginBlock()
{
  m_emit->Sub(a64::sp, a64::sp, FUNCTION_STACK_SIZE);

  // Save the link register, since we'll be calling functions.
  const bool link_reg_allocated = m_register_cache.AllocateHostReg(30);
  DebugAssert(link_reg_allocated);
  UNREFERENCED_VARIABLE(link_reg_allocated);
  m_register_cache.AssumeCalleeSavedRegistersAreSaved();

  // Store the CPU struct pointer. TODO: make this better.
  const bool cpu_reg_allocated = m_register_cache.AllocateHostReg(RCPUPTR);
  DebugAssert(cpu_reg_allocated);
  UNREFERENCED_VARIABLE(cpu_reg_allocated);

  // If there's loadstore instructions, preload the fastmem base.
  if (m_block->contains_loadstore_instructions)
  {
    const bool fastmem_reg_allocated = m_register_cache.AllocateHostReg(RMEMBASEPTR);
    Assert(fastmem_reg_allocated);
    m_emit->Ldr(GetFastmemBasePtrReg(), a64::MemOperand(GetCPUPtrReg(), offsetof(State, fastmem_base)));
  }
}

void CodeGenerator::EmitEndBlock()
{
  if (m_block->contains_loadstore_instructions)
    m_register_cache.FreeHostReg(RMEMBASEPTR);

  m_register_cache.FreeHostReg(RCPUPTR);
  m_register_cache.PopCalleeSavedRegisters(true);

  m_emit->Add(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  // m_emit->b(GetPCDisplacement(GetCurrentCodePointer(), s_dispatcher_return_address));
  m_emit->Ret();
}

void CodeGenerator::EmitExceptionExit()
{
  // ensure all unflushed registers are written back
  m_register_cache.FlushAllGuestRegisters(false, false);

  // the interpreter load delay might have its own value, but we'll overwrite it here anyway
  // technically RaiseException() and FlushPipeline() have already been called, but that should be okay
  m_register_cache.FlushLoadDelay(false);

  m_register_cache.PopCalleeSavedRegisters(false);

  m_emit->Add(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  // m_emit->b(GetPCDisplacement(GetCurrentCodePointer(), s_dispatcher_return_address));
  m_emit->Ret();
}

void CodeGenerator::EmitExceptionExitOnBool(const Value& value)
{
  Assert(!value.IsConstant() && value.IsInHostRegister());

  m_register_cache.PushState();

  // TODO: This is... not great.
  a64::Label skip_branch;
  m_emit->Cbz(GetHostReg64(value.host_reg), &skip_branch);
  EmitBranch(GetCurrentFarCodePointer());
  m_emit->Bind(&skip_branch);

  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();
}

void CodeGenerator::FinalizeBlock(CodeBlock::HostCodePointer* out_host_code, u32* out_host_code_size)
{
  m_near_emitter.FinalizeCode();
  m_far_emitter.FinalizeCode();

  *out_host_code = reinterpret_cast<CodeBlock::HostCodePointer>(m_code_buffer->GetFreeCodePointer());
  *out_host_code_size = static_cast<u32>(m_near_emitter.GetSizeOfCodeGenerated());

  m_code_buffer->CommitCode(static_cast<u32>(m_near_emitter.GetSizeOfCodeGenerated()));
  m_code_buffer->CommitFarCode(static_cast<u32>(m_far_emitter.GetSizeOfCodeGenerated()));

  m_near_emitter.Reset();
  m_far_emitter.Reset();
}

void CodeGenerator::EmitSignExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size)
{
  switch (to_size)
  {
    case RegSize_16:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->sxtb(GetHostReg16(to_reg), GetHostReg8(from_reg));
          m_emit->and_(GetHostReg16(to_reg), GetHostReg16(to_reg), 0xFFFF);
          return;
      }
    }
    break;

    case RegSize_32:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->sxtb(GetHostReg32(to_reg), GetHostReg8(from_reg));
          return;
        case RegSize_16:
          m_emit->sxth(GetHostReg32(to_reg), GetHostReg16(from_reg));
          return;
      }
    }
    break;
  }

  Panic("Unknown sign-extend combination");
}

void CodeGenerator::EmitZeroExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size)
{
  switch (to_size)
  {
    case RegSize_16:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->and_(GetHostReg16(to_reg), GetHostReg8(from_reg), 0xFF);
          return;
      }
    }
    break;

    case RegSize_32:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->and_(GetHostReg32(to_reg), GetHostReg8(from_reg), 0xFF);
          return;
        case RegSize_16:
          m_emit->and_(GetHostReg32(to_reg), GetHostReg16(from_reg), 0xFFFF);
          return;
      }
    }
    break;
  }

  Panic("Unknown sign-extend combination");
}

void CodeGenerator::EmitCopyValue(HostReg to_reg, const Value& value)
{
  // TODO: mov x, 0 -> xor x, x
  DebugAssert(value.IsConstant() || value.IsInHostRegister());

  switch (value.size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->Mov(GetHostReg32(to_reg), value.constant_value);
      else
        m_emit->Mov(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
        m_emit->Mov(GetHostReg64(to_reg), value.constant_value);
      else
        m_emit->Mov(GetHostReg64(to_reg), GetHostReg64(value.host_reg));
    }
    break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitAdd(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (value.size < RegSize_64)
    {
      if (set_flags)
        m_emit->adds(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
      else
        m_emit->add(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    }
    else
    {
      if (set_flags)
        m_emit->adds(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(value.host_reg));
      else
        m_emit->add(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(value.host_reg));
    }

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  const s64 constant_value = value.GetS64ConstantValue();
  if (a64::Assembler::IsImmAddSub(constant_value))
  {
    if (value.size < RegSize_64)
    {
      if (set_flags)
        m_emit->adds(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);
      else
        m_emit->add(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);
    }
    else
    {
      if (set_flags)
        m_emit->adds(GetHostReg64(to_reg), GetHostReg64(from_reg), constant_value);
      else
        m_emit->add(GetHostReg64(to_reg), GetHostReg64(from_reg), constant_value);
    }

    return;
  }

  // need a temporary
  Assert(from_reg != RSCRATCH);
  Value temp_value(Value::FromHostReg(&m_register_cache, RSCRATCH, value.size));
  if (value.size < RegSize_64)
    m_emit->Mov(GetHostReg32(temp_value.host_reg), constant_value);
  else
    m_emit->Mov(GetHostReg64(temp_value.host_reg), constant_value);
  EmitAdd(to_reg, from_reg, temp_value, set_flags);
}

void CodeGenerator::EmitSub(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (value.size < RegSize_64)
    {
      if (set_flags)
        m_emit->subs(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
      else
        m_emit->sub(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    }
    else
    {
      if (set_flags)
        m_emit->subs(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(value.host_reg));
      else
        m_emit->sub(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(value.host_reg));
    }

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  const s64 constant_value = value.GetS64ConstantValue();
  if (a64::Assembler::IsImmAddSub(value.constant_value))
  {
    if (value.size < RegSize_64)
    {
      if (set_flags)
        m_emit->subs(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);
      else
        m_emit->sub(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);
    }
    else
    {
      if (set_flags)
        m_emit->subs(GetHostReg64(to_reg), GetHostReg64(from_reg), constant_value);
      else
        m_emit->sub(GetHostReg64(to_reg), GetHostReg64(from_reg), constant_value);
    }

    return;
  }

  // need a temporary
  Assert(from_reg != RSCRATCH);
  Value temp_value(Value::FromHostReg(&m_register_cache, RSCRATCH, value.size));
  if (value.size < RegSize_64)
    m_emit->Mov(GetHostReg32(temp_value.host_reg), constant_value);
  else
    m_emit->Mov(GetHostReg64(temp_value.host_reg), constant_value);
  EmitSub(to_reg, from_reg, temp_value, set_flags);
}

void CodeGenerator::EmitCmp(HostReg to_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (value.size < RegSize_64)
      m_emit->cmp(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    else
      m_emit->cmp(GetHostReg64(to_reg), GetHostReg64(value.host_reg));

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  const s64 constant_value = value.GetS64ConstantValue();
  if (constant_value >= 0)
  {
    if (a64::Assembler::IsImmAddSub(constant_value))
    {
      if (value.size < RegSize_64)
        m_emit->cmp(GetHostReg32(to_reg), constant_value);
      else
        m_emit->cmp(GetHostReg64(to_reg), constant_value);

      return;
    }
  }
  else
  {
    if (a64::Assembler::IsImmAddSub(-constant_value))
    {
      if (value.size < RegSize_64)
        m_emit->cmn(GetHostReg32(to_reg), -constant_value);
      else
        m_emit->cmn(GetHostReg64(to_reg), -constant_value);

      return;
    }
  }

  // need a temporary
  Assert(to_reg != RSCRATCH);
  Value temp_value(Value::FromHostReg(&m_register_cache, RSCRATCH, value.size));
  if (value.size < RegSize_64)
    m_emit->Mov(GetHostReg32(temp_value.host_reg), constant_value);
  else
    m_emit->Mov(GetHostReg64(temp_value.host_reg), constant_value);
  EmitCmp(to_reg, temp_value);
}

void CodeGenerator::EmitMul(HostReg to_reg_hi, HostReg to_reg_lo, const Value& lhs, const Value& rhs,
                            bool signed_multiply)
{
  Value lhs_in_reg = GetValueInHostRegister(lhs);
  Value rhs_in_reg = GetValueInHostRegister(rhs);

  if (lhs.size < RegSize_64)
  {
    if (signed_multiply)
    {
      m_emit->smull(GetHostReg64(to_reg_lo), GetHostReg32(lhs_in_reg.host_reg), GetHostReg32(rhs_in_reg.host_reg));
      m_emit->asr(GetHostReg64(to_reg_hi), GetHostReg64(to_reg_lo), 32);
    }
    else
    {
      m_emit->umull(GetHostReg64(to_reg_lo), GetHostReg32(lhs_in_reg.host_reg), GetHostReg32(rhs_in_reg.host_reg));
      m_emit->lsr(GetHostReg64(to_reg_hi), GetHostReg64(to_reg_lo), 32);
    }
  }
  else
  {
    // TODO: Use mul + smulh
    Panic("Not implemented");
  }
}

void CodeGenerator::EmitDiv(HostReg to_reg_quotient, HostReg to_reg_remainder, HostReg num, HostReg denom, RegSize size,
                            bool signed_divide)
{
  // only 32-bit supported for now..
  Assert(size == RegSize_32);

  Value quotient_value;
  if (to_reg_quotient == HostReg_Count)
  {
    Assert(to_reg_quotient != RSCRATCH);
    quotient_value = Value::FromHostReg(&m_register_cache, RSCRATCH, size);
  }
  else
  {
    quotient_value.SetHostReg(&m_register_cache, to_reg_quotient, size);
  }

  if (signed_divide)
  {
    m_emit->sdiv(GetHostReg32(quotient_value), GetHostReg32(num), GetHostReg32(denom));
    if (to_reg_remainder != HostReg_Count)
    {
      m_emit->msub(GetHostReg32(to_reg_remainder), GetHostReg32(quotient_value), GetHostReg32(denom),
                   GetHostReg32(num));
    }
  }
  else
  {
    m_emit->udiv(GetHostReg32(quotient_value), GetHostReg32(num), GetHostReg32(denom));
    if (to_reg_remainder != HostReg_Count)
    {
      m_emit->msub(GetHostReg32(to_reg_remainder), GetHostReg32(quotient_value), GetHostReg32(denom),
                   GetHostReg32(num));
    }
  }
}

void CodeGenerator::EmitInc(HostReg to_reg, RegSize size)
{
  Panic("Not implemented");
#if 0
  switch (size)
  {
    case RegSize_8:
      m_emit->inc(GetHostReg8(to_reg));
      break;
    case RegSize_16:
      m_emit->inc(GetHostReg16(to_reg));
      break;
    case RegSize_32:
      m_emit->inc(GetHostReg32(to_reg));
      break;
    default:
      UnreachableCode();
      break;
  }
#endif
}

void CodeGenerator::EmitDec(HostReg to_reg, RegSize size)
{
  Panic("Not implemented");
#if 0
  switch (size)
  {
    case RegSize_8:
      m_emit->dec(GetHostReg8(to_reg));
      break;
    case RegSize_16:
      m_emit->dec(GetHostReg16(to_reg));
      break;
    case RegSize_32:
      m_emit->dec(GetHostReg32(to_reg));
      break;
    default:
      UnreachableCode();
      break;
  }
#endif
}

void CodeGenerator::EmitShl(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
    {
      if (amount_value.IsConstant())
        m_emit->lsl(GetHostReg32(to_reg), GetHostReg32(from_reg), amount_value.constant_value & 0x1F);
      else
        m_emit->lslv(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(amount_value));

      if (size == RegSize_8)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFF);
      else if (size == RegSize_16)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFFFF);
    }
    break;

    case RegSize_64:
    {
      if (amount_value.IsConstant())
        m_emit->lsl(GetHostReg64(to_reg), GetHostReg64(from_reg), amount_value.constant_value & 0x3F);
      else
        m_emit->lslv(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(amount_value));
    }
    break;
  }
}

void CodeGenerator::EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
    {
      if (amount_value.IsConstant())
        m_emit->lsr(GetHostReg32(to_reg), GetHostReg32(from_reg), amount_value.constant_value & 0x1F);
      else
        m_emit->lsrv(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(amount_value));

      if (size == RegSize_8)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFF);
      else if (size == RegSize_16)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFFFF);
    }
    break;

    case RegSize_64:
    {
      if (amount_value.IsConstant())
        m_emit->lsr(GetHostReg64(to_reg), GetHostReg64(to_reg), amount_value.constant_value & 0x3F);
      else
        m_emit->lsrv(GetHostReg64(to_reg), GetHostReg64(to_reg), GetHostReg64(amount_value));
    }
    break;
  }
}

void CodeGenerator::EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
    {
      if (amount_value.IsConstant())
        m_emit->asr(GetHostReg32(to_reg), GetHostReg32(from_reg), amount_value.constant_value & 0x1F);
      else
        m_emit->asrv(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(amount_value));

      if (size == RegSize_8)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFF);
      else if (size == RegSize_16)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFFFF);
    }
    break;

    case RegSize_64:
    {
      if (amount_value.IsConstant())
        m_emit->asr(GetHostReg64(to_reg), GetHostReg64(from_reg), amount_value.constant_value & 0x3F);
      else
        m_emit->asrv(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(amount_value));
    }
    break;
  }
}

static bool CanFitInBitwiseImmediate(const Value& value)
{
  const unsigned reg_size = (value.size < RegSize_64) ? 32 : 64;
  unsigned n, imm_s, imm_r;
  return a64::Assembler::IsImmLogical(s64(value.constant_value), reg_size, &n, &imm_s, &imm_r);
}

void CodeGenerator::EmitAnd(HostReg to_reg, HostReg from_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (value.size < RegSize_64)
      m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    else
      m_emit->and_(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(value.host_reg));

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    if (value.size < RegSize_64)
      m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), s64(value.constant_value));
    else
      m_emit->and_(GetHostReg64(to_reg), GetHostReg64(from_reg), s64(value.constant_value));

    return;
  }

  // need a temporary
  Assert(from_reg != RSCRATCH);
  Value temp_value(Value::FromHostReg(&m_register_cache, RSCRATCH, value.size));
  if (value.size < RegSize_64)
    m_emit->Mov(GetHostReg32(temp_value.host_reg), s64(value.constant_value));
  else
    m_emit->Mov(GetHostReg64(temp_value.host_reg), s64(value.constant_value));
  EmitAnd(to_reg, from_reg, temp_value);
}

void CodeGenerator::EmitOr(HostReg to_reg, HostReg from_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (value.size < RegSize_64)
      m_emit->orr(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    else
      m_emit->orr(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(value.host_reg));

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    if (value.size < RegSize_64)
      m_emit->orr(GetHostReg32(to_reg), GetHostReg32(from_reg), s64(value.constant_value));
    else
      m_emit->orr(GetHostReg64(to_reg), GetHostReg64(from_reg), s64(value.constant_value));

    return;
  }

  // need a temporary
  Assert(from_reg != RSCRATCH);
  Value temp_value(Value::FromHostReg(&m_register_cache, RSCRATCH, value.size));
  if (value.size < RegSize_64)
    m_emit->Mov(GetHostReg32(temp_value.host_reg), s64(value.constant_value));
  else
    m_emit->Mov(GetHostReg64(temp_value.host_reg), s64(value.constant_value));
  EmitOr(to_reg, from_reg, temp_value);
}

void CodeGenerator::EmitXor(HostReg to_reg, HostReg from_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (value.size < RegSize_64)
      m_emit->eor(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    else
      m_emit->eor(GetHostReg64(to_reg), GetHostReg64(from_reg), GetHostReg64(value.host_reg));

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    if (value.size < RegSize_64)
      m_emit->eor(GetHostReg32(to_reg), GetHostReg32(from_reg), s64(value.constant_value));
    else
      m_emit->eor(GetHostReg64(to_reg), GetHostReg64(from_reg), s64(value.constant_value));

    return;
  }

  // need a temporary
  Assert(from_reg != RSCRATCH);
  Value temp_value(Value::FromHostReg(&m_register_cache, RSCRATCH, value.size));
  if (value.size < RegSize_64)
    m_emit->Mov(GetHostReg32(temp_value.host_reg), s64(value.constant_value));
  else
    m_emit->Mov(GetHostReg64(temp_value.host_reg), s64(value.constant_value));
  EmitXor(to_reg, from_reg, temp_value);
}

void CodeGenerator::EmitTest(HostReg to_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (value.size < RegSize_64)
      m_emit->tst(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    else
      m_emit->tst(GetHostReg64(to_reg), GetHostReg64(value.host_reg));

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    if (value.size < RegSize_64)
      m_emit->tst(GetHostReg32(to_reg), s64(value.constant_value));
    else
      m_emit->tst(GetHostReg64(to_reg), s64(value.constant_value));

    return;
  }

  // need a temporary
  Assert(to_reg != RSCRATCH);
  Value temp_value(Value::FromHostReg(&m_register_cache, RSCRATCH, value.size));
  if (value.size < RegSize_64)
    m_emit->Mov(GetHostReg32(temp_value.host_reg), s64(value.constant_value));
  else
    m_emit->Mov(GetHostReg64(temp_value.host_reg), s64(value.constant_value));
  EmitTest(to_reg, temp_value);
}

void CodeGenerator::EmitNot(HostReg to_reg, RegSize size)
{
  switch (size)
  {
    case RegSize_8:
      m_emit->mvn(GetHostReg8(to_reg), GetHostReg8(to_reg));
      m_emit->and_(GetHostReg8(to_reg), GetHostReg8(to_reg), 0xFF);
      break;

    case RegSize_16:
      m_emit->mvn(GetHostReg16(to_reg), GetHostReg16(to_reg));
      m_emit->and_(GetHostReg16(to_reg), GetHostReg16(to_reg), 0xFFFF);
      break;

    case RegSize_32:
      m_emit->mvn(GetHostReg32(to_reg), GetHostReg32(to_reg));
      break;

    case RegSize_64:
      m_emit->mvn(GetHostReg64(to_reg), GetHostReg64(to_reg));
      break;

    default:
      break;
  }
}

void CodeGenerator::EmitSetConditionResult(HostReg to_reg, RegSize to_size, Condition condition)
{
  if (condition == Condition::Always)
  {
    if (to_size < RegSize_64)
      m_emit->Mov(GetHostReg32(to_reg), 1);
    else
      m_emit->Mov(GetHostReg64(to_reg), 1);

    return;
  }

  a64::Condition acond;
  switch (condition)
  {
    case Condition::NotEqual:
      acond = a64::ne;
      break;

    case Condition::Equal:
      acond = a64::eq;
      break;

    case Condition::Overflow:
      acond = a64::vs;
      break;

    case Condition::Greater:
      acond = a64::gt;
      break;

    case Condition::GreaterEqual:
      acond = a64::ge;
      break;

    case Condition::Less:
      acond = a64::lt;
      break;

    case Condition::LessEqual:
      acond = a64::le;
      break;

    case Condition::Negative:
      acond = a64::mi;
      break;

    case Condition::PositiveOrZero:
      acond = a64::pl;
      break;

    case Condition::Above:
      acond = a64::hi;
      break;

    case Condition::AboveEqual:
      acond = a64::cs;
      break;

    case Condition::Below:
      acond = a64::cc;
      break;

    case Condition::BelowEqual:
      acond = a64::ls;
      break;

    default:
      UnreachableCode();
      return;
  }

  if (to_size < RegSize_64)
    m_emit->cset(GetHostReg32(to_reg), acond);
  else
    m_emit->cset(GetHostReg64(to_reg), acond);
}

u32 CodeGenerator::PrepareStackForCall()
{
  m_register_cache.PushCallerSavedRegisters();
  return 0;
}

void CodeGenerator::RestoreStackAfterCall(u32 adjust_size)
{
  m_register_cache.PopCallerSavedRegisters();
}

void CodeGenerator::EmitCall(const void* ptr)
{
  const s64 displacement = GetPCDisplacement(GetCurrentCodePointer(), ptr);
  const bool use_blr = !vixl::IsInt26(displacement);
  if (use_blr)
  {
    m_emit->Mov(GetHostReg64(RSCRATCH), reinterpret_cast<uintptr_t>(ptr));
    m_emit->Blr(GetHostReg64(RSCRATCH));
  }
  else
  {
    m_emit->bl(displacement);
  }
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr)
{
  if (return_value)
    return_value->Discard();

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // actually call the function
  EmitCall(ptr);

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // copy out return value if requested
  if (return_value)
  {
    return_value->Undiscard();
    EmitCopyValue(return_value->GetHostRegister(), Value::FromHostReg(&m_register_cache, RRETURN, return_value->size));
  }
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1)
{
  if (return_value)
    return_value->Discard();

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);

  // actually call the function
  EmitCall(ptr);

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // copy out return value if requested
  if (return_value)
  {
    return_value->Undiscard();
    EmitCopyValue(return_value->GetHostRegister(), Value::FromHostReg(&m_register_cache, RRETURN, return_value->size));
  }
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2)
{
  if (return_value)
    return_value->Discard();

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);
  EmitCopyValue(RARG2, arg2);

  // actually call the function
  EmitCall(ptr);

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // copy out return value if requested
  if (return_value)
  {
    return_value->Undiscard();
    EmitCopyValue(return_value->GetHostRegister(), Value::FromHostReg(&m_register_cache, RRETURN, return_value->size));
  }
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2,
                                        const Value& arg3)
{
  if (return_value)
    m_register_cache.DiscardHostReg(return_value->GetHostRegister());

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);
  EmitCopyValue(RARG2, arg2);
  EmitCopyValue(RARG3, arg3);

  // actually call the function
  EmitCall(ptr);

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // copy out return value if requested
  if (return_value)
  {
    return_value->Undiscard();
    EmitCopyValue(return_value->GetHostRegister(), Value::FromHostReg(&m_register_cache, RRETURN, return_value->size));
  }
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr, const Value& arg1, const Value& arg2,
                                        const Value& arg3, const Value& arg4)
{
  if (return_value)
    return_value->Discard();

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);
  EmitCopyValue(RARG2, arg2);
  EmitCopyValue(RARG3, arg3);
  EmitCopyValue(RARG4, arg4);

  // actually call the function
  EmitCall(ptr);

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // copy out return value if requested
  if (return_value)
  {
    return_value->Undiscard();
    EmitCopyValue(return_value->GetHostRegister(), Value::FromHostReg(&m_register_cache, RRETURN, return_value->size));
  }
}

void CodeGenerator::EmitPushHostReg(HostReg reg, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - FUNCTION_CALL_SHADOW_SPACE - (position * 8));
  m_emit->str(GetHostReg64(reg), addr);
}

void CodeGenerator::EmitPushHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - FUNCTION_CALL_SHADOW_SPACE - ((position + 1) * 8));
  m_emit->stp(GetHostReg64(reg2), GetHostReg64(reg), addr);
}

void CodeGenerator::EmitPopHostReg(HostReg reg, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - FUNCTION_CALL_SHADOW_SPACE - (position * 8));
  m_emit->ldr(GetHostReg64(reg), addr);
}

void CodeGenerator::EmitPopHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - FUNCTION_CALL_SHADOW_SPACE - (position * 8));
  m_emit->ldp(GetHostReg64(reg2), GetHostReg64(reg), addr);
}

void CodeGenerator::EmitLoadCPUStructField(HostReg host_reg, RegSize guest_size, u32 offset)
{
  const s64 s_offset = static_cast<s64>(ZeroExtend64(offset));

  switch (guest_size)
  {
    case RegSize_8:
      m_emit->Ldrb(GetHostReg8(host_reg), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_16:
      m_emit->Ldrh(GetHostReg16(host_reg), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_32:
      m_emit->Ldr(GetHostReg32(host_reg), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_64:
      m_emit->Ldr(GetHostReg64(host_reg), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    default:
    {
      UnreachableCode();
    }
    break;
  }
}

void CodeGenerator::EmitStoreCPUStructField(u32 offset, const Value& value)
{
  const Value hr_value = GetValueInHostRegister(value);
  const s64 s_offset = static_cast<s64>(ZeroExtend64(offset));

  switch (value.size)
  {
    case RegSize_8:
      m_emit->Strb(GetHostReg8(hr_value), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_16:
      m_emit->Strh(GetHostReg16(hr_value), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_32:
      m_emit->Str(GetHostReg32(hr_value), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_64:
      m_emit->Str(GetHostReg64(hr_value), a64::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    default:
    {
      UnreachableCode();
    }
    break;
  }
}

void CodeGenerator::EmitAddCPUStructField(u32 offset, const Value& value)
{
  const s64 s_offset = static_cast<s64>(ZeroExtend64(offset));
  const a64::MemOperand o_offset(GetCPUPtrReg(), s_offset);

  Value real_value;
  if (value.IsInHostRegister())
  {
    real_value.SetHostReg(&m_register_cache, value.host_reg, value.size);
  }
  else
  {
    // do we need temporary storage for the constant, if it won't fit in an immediate?
    Assert(value.IsConstant());
    const s64 constant_value = value.GetS64ConstantValue();
    if (!a64::Assembler::IsImmAddSub(constant_value))
    {
      real_value.SetHostReg(&m_register_cache, RARG4, value.size);
      EmitCopyValue(real_value.host_reg, value);
    }
    else
    {
      real_value = value;
    }
  }

  // Don't need to mask here because we're storing back to memory.
  switch (value.size)
  {
    case RegSize_8:
    {
      m_emit->Ldrb(GetHostReg8(RSCRATCH), o_offset);
      if (real_value.IsConstant())
        m_emit->Add(GetHostReg8(RSCRATCH), GetHostReg8(RSCRATCH), real_value.GetS64ConstantValue());
      else
        m_emit->Add(GetHostReg8(RSCRATCH), GetHostReg8(RSCRATCH), GetHostReg8(real_value));
      m_emit->Strb(GetHostReg8(RSCRATCH), o_offset);
    }
    break;

    case RegSize_16:
    {
      m_emit->Ldrh(GetHostReg16(RSCRATCH), o_offset);
      if (real_value.IsConstant())
        m_emit->Add(GetHostReg16(RSCRATCH), GetHostReg16(RSCRATCH), real_value.GetS64ConstantValue());
      else
        m_emit->Add(GetHostReg16(RSCRATCH), GetHostReg16(RSCRATCH), GetHostReg16(real_value));
      m_emit->Strh(GetHostReg16(RSCRATCH), o_offset);
    }
    break;

    case RegSize_32:
    {
      m_emit->Ldr(GetHostReg32(RSCRATCH), o_offset);
      if (real_value.IsConstant())
        m_emit->Add(GetHostReg32(RSCRATCH), GetHostReg32(RSCRATCH), real_value.GetS64ConstantValue());
      else
        m_emit->Add(GetHostReg32(RSCRATCH), GetHostReg32(RSCRATCH), GetHostReg32(real_value));
      m_emit->Str(GetHostReg32(RSCRATCH), o_offset);
    }
    break;

    case RegSize_64:
    {
      m_emit->Ldr(GetHostReg64(RSCRATCH), o_offset);
      if (real_value.IsConstant())
        m_emit->Add(GetHostReg64(RSCRATCH), GetHostReg64(RSCRATCH), s64(real_value.constant_value));
      else
        m_emit->Add(GetHostReg64(RSCRATCH), GetHostReg64(RSCRATCH), GetHostReg64(real_value));
      m_emit->Str(GetHostReg64(RSCRATCH), o_offset);
    }
    break;

    default:
    {
      UnreachableCode();
    }
    break;
  }
}

void CodeGenerator::EmitLoadGuestRAMFastmem(const Value& address, RegSize size, Value& result)
{
  HostReg address_reg;
  a64::MemOperand actual_address;
  if (address.IsConstant())
  {
    m_emit->Mov(GetHostReg32(result.host_reg), address.constant_value);
    address_reg = result.host_reg;
  }
  else
  {
    address_reg = address.host_reg;
  }

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    switch (size)
    {
      case RegSize_8:
        m_emit->ldrb(GetHostReg32(result.host_reg), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      case RegSize_16:
        m_emit->ldrh(GetHostReg32(result.host_reg), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      case RegSize_32:
        m_emit->ldr(GetHostReg32(result.host_reg), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      default:
        UnreachableCode();
        break;
    }
  }
  else
  {
    m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), 12);
    m_emit->and_(GetHostReg32(RARG2), GetHostReg32(address_reg), HOST_PAGE_OFFSET_MASK);
    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(RARG1), a64::LSL, 3));

    switch (size)
    {
      case RegSize_8:
        m_emit->ldrb(GetHostReg32(result.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      case RegSize_16:
        m_emit->ldrh(GetHostReg32(result.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      case RegSize_32:
        m_emit->ldr(GetHostReg32(result.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      default:
        UnreachableCode();
        break;
    }
  }
}

void CodeGenerator::EmitLoadGuestMemoryFastmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                               Value& result)
{
  // fastmem
  LoadStoreBackpatchInfo bpi;
  bpi.address_host_reg = HostReg_Invalid;
  bpi.value_host_reg = result.host_reg;
  bpi.guest_pc = m_current_instruction->pc;

  HostReg address_reg;
  if (address.IsConstant())
  {
    m_emit->Mov(GetHostReg32(result.host_reg), address.constant_value);
    address_reg = result.host_reg;
  }
  else
  {
    address_reg = address.host_reg;
  }

  m_register_cache.InhibitAllocation();

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    bpi.host_pc = GetCurrentNearCodePointer();

    switch (size)
    {
      case RegSize_8:
        m_emit->ldrb(GetHostReg32(result.host_reg), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      case RegSize_16:
        m_emit->ldrh(GetHostReg32(result.host_reg), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      case RegSize_32:
        m_emit->ldr(GetHostReg32(result.host_reg), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      default:
        UnreachableCode();
        break;
    }
  }
  else
  {
    m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), 12);
    m_emit->and_(GetHostReg32(RARG2), GetHostReg32(address_reg), HOST_PAGE_OFFSET_MASK);
    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(RARG1), a64::LSL, 3));

    bpi.host_pc = GetCurrentNearCodePointer();

    switch (size)
    {
      case RegSize_8:
        m_emit->ldrb(GetHostReg32(result.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      case RegSize_16:
        m_emit->ldrh(GetHostReg32(result.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      case RegSize_32:
        m_emit->ldr(GetHostReg32(result.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      default:
        UnreachableCode();
        break;
    }
  }

  EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(Bus::RAM_READ_TICKS));

  bpi.host_code_size = static_cast<u32>(
    static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc)));

  // generate slowmem fallback
  bpi.host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();
  EmitLoadGuestMemorySlowmem(cbi, address, size, result, true);

  // return to the block code
  EmitBranch(GetCurrentNearCodePointer(), false);

  SwitchToNearCode();
  m_register_cache.UninhibitAllocation();

  m_block->loadstore_backpatch_info.push_back(bpi);
}

void CodeGenerator::EmitLoadGuestMemorySlowmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                               Value& result, bool in_far_code)
{
  if (g_settings.cpu_recompiler_memory_exceptions)
  {
    // NOTE: This can leave junk in the upper bits
    switch (size)
    {
      case RegSize_8:
        EmitFunctionCall(&result, &Thunks::ReadMemoryByte, address);
        break;

      case RegSize_16:
        EmitFunctionCall(&result, &Thunks::ReadMemoryHalfWord, address);
        break;

      case RegSize_32:
        EmitFunctionCall(&result, &Thunks::ReadMemoryWord, address);
        break;

      default:
        UnreachableCode();
        break;
    }

    m_register_cache.PushState();

    a64::Label load_okay;
    m_emit->Tbz(GetHostReg64(result.host_reg), 63, &load_okay);
    EmitBranch(GetCurrentFarCodePointer());
    m_emit->Bind(&load_okay);

    // load exception path
    if (!in_far_code)
      SwitchToFarCode();

    // cause_bits = (-result << 2) | BD | cop_n
    m_emit->neg(GetHostReg32(result.host_reg), GetHostReg32(result.host_reg));
    m_emit->lsl(GetHostReg32(result.host_reg), GetHostReg32(result.host_reg), 2);
    EmitOr(result.host_reg, result.host_reg,
           Value::FromConstantU32(Cop0Registers::CAUSE::MakeValueForException(
             static_cast<Exception>(0), cbi.is_branch_delay_slot, false, cbi.instruction.cop.cop_n)));
    EmitFunctionCall(nullptr, static_cast<void (*)(u32, u32)>(&CPU::RaiseException), result, GetCurrentInstructionPC());

    EmitExceptionExit();

    if (!in_far_code)
      SwitchToNearCode();

    m_register_cache.PopState();
  }
  else
  {
    switch (size)
    {
      case RegSize_8:
        EmitFunctionCall(&result, &Thunks::UncheckedReadMemoryByte, address);
        break;

      case RegSize_16:
        EmitFunctionCall(&result, &Thunks::UncheckedReadMemoryHalfWord, address);
        break;

      case RegSize_32:
        EmitFunctionCall(&result, &Thunks::UncheckedReadMemoryWord, address);
        break;

      default:
        UnreachableCode();
        break;
    }
  }
}

void CodeGenerator::EmitStoreGuestMemoryFastmem(const CodeBlockInstruction& cbi, const Value& address,
                                                const Value& value)
{
  Value value_in_hr = GetValueInHostRegister(value);

  // fastmem
  LoadStoreBackpatchInfo bpi;
  bpi.address_host_reg = HostReg_Invalid;
  bpi.value_host_reg = value.host_reg;
  bpi.guest_pc = m_current_instruction->pc;

  HostReg address_reg;
  if (address.IsConstant())
  {
    m_emit->Mov(GetHostReg32(RSCRATCH), address.constant_value);
    address_reg = RSCRATCH;
  }
  else
  {
    address_reg = address.host_reg;
  }

  m_register_cache.InhibitAllocation();
  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    bpi.host_pc = GetCurrentNearCodePointer();

    switch (value.size)
    {
      case RegSize_8:
        m_emit->strb(GetHostReg8(value_in_hr), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      case RegSize_16:
        m_emit->strh(GetHostReg16(value_in_hr), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      case RegSize_32:
        m_emit->str(GetHostReg32(value_in_hr), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(address_reg)));
        break;

      default:
        UnreachableCode();
        break;
    }
  }
  else
  {
    m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), 12);
    m_emit->and_(GetHostReg32(RARG2), GetHostReg32(address_reg), HOST_PAGE_OFFSET_MASK);
    m_emit->add(GetHostReg64(RARG3), GetFastmemBasePtrReg(), Bus::FASTMEM_LUT_NUM_PAGES * sizeof(u32*));
    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetHostReg64(RARG3), GetHostReg32(RARG1), a64::LSL, 3));

    bpi.host_pc = GetCurrentNearCodePointer();

    switch (value.size)
    {
      case RegSize_8:
        m_emit->strb(GetHostReg32(value_in_hr.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      case RegSize_16:
        m_emit->strh(GetHostReg32(value_in_hr.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      case RegSize_32:
        m_emit->str(GetHostReg32(value_in_hr.host_reg), a64::MemOperand(GetHostReg64(RARG1), GetHostReg32(RARG2)));
        break;

      default:
        UnreachableCode();
        break;
    }
  }

  bpi.host_code_size = static_cast<u32>(
    static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc)));

  // generate slowmem fallback
  bpi.host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();

  EmitStoreGuestMemorySlowmem(cbi, address, value_in_hr, true);

  // return to the block code
  EmitBranch(GetCurrentNearCodePointer(), false);

  SwitchToNearCode();
  m_register_cache.UninhibitAllocation();

  m_block->loadstore_backpatch_info.push_back(bpi);
}

void CodeGenerator::EmitStoreGuestMemorySlowmem(const CodeBlockInstruction& cbi, const Value& address,
                                                const Value& value, bool in_far_code)
{
  AddPendingCycles(true);

  Value value_in_hr = GetValueInHostRegister(value);

  if (g_settings.cpu_recompiler_memory_exceptions)
  {
    Assert(!in_far_code);

    Value result = m_register_cache.AllocateScratch(RegSize_32);
    switch (value.size)
    {
      case RegSize_8:
        EmitFunctionCall(&result, &Thunks::WriteMemoryByte, address, value_in_hr);
        break;

      case RegSize_16:
        EmitFunctionCall(&result, &Thunks::WriteMemoryHalfWord, address, value_in_hr);
        break;

      case RegSize_32:
        EmitFunctionCall(&result, &Thunks::WriteMemoryWord, address, value_in_hr);
        break;

      default:
        UnreachableCode();
        break;
    }

    m_register_cache.PushState();

    a64::Label store_okay;
    m_emit->Cbz(GetHostReg64(result.host_reg), &store_okay);
    EmitBranch(GetCurrentFarCodePointer());
    m_emit->Bind(&store_okay);

    // store exception path
    if (!in_far_code)
      SwitchToFarCode();

    // cause_bits = (result << 2) | BD | cop_n
    m_emit->lsl(GetHostReg32(result.host_reg), GetHostReg32(result.host_reg), 2);
    EmitOr(result.host_reg, result.host_reg,
           Value::FromConstantU32(Cop0Registers::CAUSE::MakeValueForException(
             static_cast<Exception>(0), cbi.is_branch_delay_slot, false, cbi.instruction.cop.cop_n)));
    EmitFunctionCall(nullptr, static_cast<void (*)(u32, u32)>(&CPU::RaiseException), result, GetCurrentInstructionPC());

    if (!in_far_code)
      EmitExceptionExit();
    SwitchToNearCode();

    m_register_cache.PopState();
  }
  else
  {
    switch (value.size)
    {
      case RegSize_8:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryByte, address, value_in_hr);
        break;

      case RegSize_16:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryHalfWord, address, value_in_hr);
        break;

      case RegSize_32:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryWord, address, value_in_hr);
        break;

      default:
        UnreachableCode();
        break;
    }
  }
}

bool CodeGenerator::BackpatchLoadStore(const LoadStoreBackpatchInfo& lbi)
{
  Log_DevPrintf("Backpatching %p (guest PC 0x%08X) to slowmem at %p", lbi.host_pc, lbi.guest_pc, lbi.host_slowmem_pc);

  // check jump distance
  const s64 jump_distance =
    static_cast<s64>(reinterpret_cast<intptr_t>(lbi.host_slowmem_pc) - reinterpret_cast<intptr_t>(lbi.host_pc));
  Assert(Common::IsAligned(jump_distance, 4));
  Assert(a64::Instruction::IsValidImmPCOffset(a64::UncondBranchType, jump_distance >> 2));

  // turn it into a jump to the slowmem handler
  vixl::aarch64::MacroAssembler emit(static_cast<vixl::byte*>(lbi.host_pc), lbi.host_code_size,
                                     a64::PositionDependentCode);
  emit.b(jump_distance >> 2);

  const s32 nops = (static_cast<s32>(lbi.host_code_size) - static_cast<s32>(emit.GetCursorOffset())) / 4;
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    emit.nop();

  JitCodeBuffer::FlushInstructionCache(lbi.host_pc, lbi.host_code_size);
  return true;
}

void CodeGenerator::EmitLoadGlobal(HostReg host_reg, RegSize size, const void* ptr)
{
  EmitLoadGlobalAddress(RSCRATCH, ptr);
  switch (size)
  {
    case RegSize_8:
      m_emit->Ldrb(GetHostReg8(host_reg), a64::MemOperand(GetHostReg64(RSCRATCH)));
      break;

    case RegSize_16:
      m_emit->Ldrh(GetHostReg16(host_reg), a64::MemOperand(GetHostReg64(RSCRATCH)));
      break;

    case RegSize_32:
      m_emit->Ldr(GetHostReg32(host_reg), a64::MemOperand(GetHostReg64(RSCRATCH)));
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitStoreGlobal(void* ptr, const Value& value)
{
  Value value_in_hr = GetValueInHostRegister(value);

  EmitLoadGlobalAddress(RSCRATCH, ptr);
  switch (value.size)
  {
    case RegSize_8:
      m_emit->Strb(GetHostReg8(value_in_hr), a64::MemOperand(GetHostReg64(RSCRATCH)));
      break;

    case RegSize_16:
      m_emit->Strh(GetHostReg16(value_in_hr), a64::MemOperand(GetHostReg64(RSCRATCH)));
      break;

    case RegSize_32:
      m_emit->Str(GetHostReg32(value_in_hr), a64::MemOperand(GetHostReg64(RSCRATCH)));
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitFlushInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_32);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(State, load_delay_reg));
  const a64::MemOperand load_delay_value(GetCPUPtrReg(), offsetof(State, load_delay_value));
  const a64::MemOperand regs_base(GetCPUPtrReg(), offsetof(State, regs.r[0]));

  a64::Label skip_flush;

  // reg = load_delay_reg
  m_emit->Ldrb(GetHostReg32(reg), load_delay_reg);

  // if load_delay_reg == Reg::count goto skip_flush
  m_emit->Cmp(GetHostReg32(reg), static_cast<u8>(Reg::count));
  m_emit->B(a64::eq, &skip_flush);

  // value = load_delay_value
  m_emit->Ldr(GetHostReg32(value), load_delay_value);

  // reg = offset(r[0] + reg << 2)
  m_emit->Lsl(GetHostReg32(reg), GetHostReg32(reg), 2);
  m_emit->Add(GetHostReg32(reg), GetHostReg32(reg), offsetof(State, regs.r[0]));

  // r[reg] = value
  m_emit->Str(GetHostReg32(value), a64::MemOperand(GetCPUPtrReg(), GetHostReg32(reg)));

  // load_delay_reg = Reg::count
  m_emit->Mov(GetHostReg32(reg), static_cast<u8>(Reg::count));
  m_emit->Strb(GetHostReg32(reg), load_delay_reg);

  m_emit->Bind(&skip_flush);
}

void CodeGenerator::EmitMoveNextInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_32);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(State, load_delay_reg));
  const a64::MemOperand load_delay_value(GetCPUPtrReg(), offsetof(State, load_delay_value));
  const a64::MemOperand next_load_delay_reg(GetCPUPtrReg(), offsetof(State, next_load_delay_reg));
  const a64::MemOperand next_load_delay_value(GetCPUPtrReg(), offsetof(State, next_load_delay_value));

  m_emit->Ldrb(GetHostReg32(reg), next_load_delay_reg);
  m_emit->Ldr(GetHostReg32(value), next_load_delay_value);
  m_emit->Strb(GetHostReg32(reg), load_delay_reg);
  m_emit->Str(GetHostReg32(value), load_delay_value);
  m_emit->Mov(GetHostReg32(reg), static_cast<u8>(Reg::count));
  m_emit->Strb(GetHostReg32(reg), next_load_delay_reg);
}

void CodeGenerator::EmitCancelInterpreterLoadDelayForReg(Reg reg)
{
  if (!m_load_delay_dirty)
    return;

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(State, load_delay_reg));
  Value temp = m_register_cache.AllocateScratch(RegSize_8);

  a64::Label skip_cancel;

  // if load_delay_reg != reg goto skip_cancel
  m_emit->Ldrb(GetHostReg8(temp), load_delay_reg);
  m_emit->Cmp(GetHostReg8(temp), static_cast<u8>(reg));
  m_emit->B(a64::ne, &skip_cancel);

  // load_delay_reg = Reg::count
  m_emit->Mov(GetHostReg8(temp), static_cast<u8>(Reg::count));
  m_emit->Strb(GetHostReg8(temp), load_delay_reg);

  m_emit->Bind(&skip_cancel);
}

void CodeGenerator::EmitBranch(const void* address, bool allow_scratch)
{
  const s64 jump_distance =
    static_cast<s64>(reinterpret_cast<intptr_t>(address) - reinterpret_cast<intptr_t>(GetCurrentCodePointer()));
  Assert(Common::IsAligned(jump_distance, 4));
  if (a64::Instruction::IsValidImmPCOffset(a64::UncondBranchType, jump_distance >> 2))
  {
    m_emit->b(jump_distance >> 2);
    return;
  }

  Assert(allow_scratch);

  m_emit->Mov(GetHostReg64(RSCRATCH), reinterpret_cast<uintptr_t>(address));
  m_emit->br(GetHostReg64(RSCRATCH));
}

void CodeGenerator::EmitBranch(LabelType* label)
{
  m_emit->B(label);
}

static a64::Condition TranslateCondition(Condition condition, bool invert)
{
  switch (condition)
  {
    case Condition::Always:
      return a64::nv;

    case Condition::NotEqual:
    case Condition::NotZero:
      return invert ? a64::eq : a64::ne;

    case Condition::Equal:
    case Condition::Zero:
      return invert ? a64::ne : a64::eq;

    case Condition::Overflow:
      return invert ? a64::vc : a64::vs;

    case Condition::Greater:
      return invert ? a64::le : a64::gt;

    case Condition::GreaterEqual:
      return invert ? a64::lt : a64::ge;

    case Condition::Less:
      return invert ? a64::ge : a64::lt;

    case Condition::LessEqual:
      return invert ? a64::gt : a64::le;

    case Condition::Negative:
      return invert ? a64::pl : a64::mi;

    case Condition::PositiveOrZero:
      return invert ? a64::mi : a64::pl;

    case Condition::Above:
      return invert ? a64::ls : a64::hi;

    case Condition::AboveEqual:
      return invert ? a64::cc : a64::cs;

    case Condition::Below:
      return invert ? a64::cs : a64::cc;

    case Condition::BelowEqual:
      return invert ? a64::hi : a64::ls;

    default:
      UnreachableCode();
      return a64::nv;
  }
}

void CodeGenerator::EmitConditionalBranch(Condition condition, bool invert, HostReg value, RegSize size,
                                          LabelType* label)
{
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
      Panic("Needs a comparison value");
      return;

    case Condition::Negative:
    case Condition::PositiveOrZero:
    {
      switch (size)
      {
        case RegSize_8:
          m_emit->tst(GetHostReg8(value), GetHostReg8(value));
          break;
        case RegSize_16:
          m_emit->tst(GetHostReg16(value), GetHostReg16(value));
          break;
        case RegSize_32:
          m_emit->tst(GetHostReg32(value), GetHostReg32(value));
          break;
        case RegSize_64:
          m_emit->tst(GetHostReg64(value), GetHostReg64(value));
          break;
        default:
          UnreachableCode();
          break;
      }

      EmitConditionalBranch(condition, invert, label);
      return;
    }

    case Condition::NotZero:
    {
      switch (size)
      {
        case RegSize_8:
          m_emit->cbnz(GetHostReg8(value), label);
          break;
        case RegSize_16:
          m_emit->cbz(GetHostReg16(value), label);
          break;
        case RegSize_32:
          m_emit->cbnz(GetHostReg32(value), label);
          break;
        case RegSize_64:
          m_emit->cbnz(GetHostReg64(value), label);
          break;
        default:
          UnreachableCode();
          break;
      }

      return;
    }

    case Condition::Zero:
    {
      switch (size)
      {
        case RegSize_8:
          m_emit->cbz(GetHostReg8(value), label);
          break;
        case RegSize_16:
          m_emit->cbz(GetHostReg16(value), label);
          break;
        case RegSize_32:
          m_emit->cbz(GetHostReg32(value), label);
          break;
        case RegSize_64:
          m_emit->cbz(GetHostReg64(value), label);
          break;
        default:
          UnreachableCode();
          break;
      }

      return;
    }

    case Condition::Always:
      m_emit->b(label);
      return;

    default:
      UnreachableCode();
      return;
  }
}

void CodeGenerator::EmitConditionalBranch(Condition condition, bool invert, HostReg lhs, const Value& rhs,
                                          LabelType* label)
{
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
      EmitCmp(lhs, rhs);
      EmitConditionalBranch(condition, invert, label);
      return;
    }

    case Condition::Negative:
    case Condition::PositiveOrZero:
    case Condition::NotZero:
    case Condition::Zero:
    {
      Assert(!rhs.IsValid() || (rhs.IsConstant() && rhs.GetS64ConstantValue() == 0));
      EmitConditionalBranch(condition, invert, lhs, rhs.size, label);
      return;
    }

    case Condition::Always:
      m_emit->b(label);
      return;

    default:
      UnreachableCode();
      return;
  }
}

void CodeGenerator::EmitConditionalBranch(Condition condition, bool invert, LabelType* label)
{
  if (condition == Condition::Always)
    m_emit->b(label);
  else
    m_emit->b(label, TranslateCondition(condition, invert));
}

void CodeGenerator::EmitBranchIfBitClear(HostReg reg, RegSize size, u8 bit, LabelType* label)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
      m_emit->tbz(GetHostReg32(reg), bit, label);
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitBindLabel(LabelType* label)
{
  m_emit->Bind(label);
}

void CodeGenerator::EmitLoadGlobalAddress(HostReg host_reg, const void* ptr)
{
  const void* current_code_ptr_page = reinterpret_cast<const void*>(
    reinterpret_cast<uintptr_t>(GetCurrentCodePointer()) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = GetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(ptr) & 0xFFFu);
  if (vixl::IsInt21(page_displacement) && a64::Assembler::IsImmLogical(page_offset, 64))
  {
    m_emit->adrp(GetHostReg64(host_reg), page_displacement);
    m_emit->orr(GetHostReg64(host_reg), GetHostReg64(host_reg), page_offset);
  }
  else
  {
    m_emit->Mov(GetHostReg64(host_reg), reinterpret_cast<uintptr_t>(ptr));
  }
}

CodeCache::DispatcherFunction CodeGenerator::CompileDispatcher()
{
  m_emit->sub(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  m_register_cache.ReserveCalleeSavedRegisters();
  const u32 stack_adjust = PrepareStackForCall();

  EmitLoadGlobalAddress(RCPUPTR, &g_state);

  a64::Label frame_done_loop;
  a64::Label exit_dispatcher;
  m_emit->Bind(&frame_done_loop);

  // if frame_done goto exit_dispatcher
  m_emit->ldrb(a64::w8, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, frame_done)));
  m_emit->tbnz(a64::w8, 0, &exit_dispatcher);

  // x8 <- sr
  a64::Label no_interrupt;
  m_emit->ldr(a64::w8, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, cop0_regs.sr.bits)));

  // if Iec == 0 then goto no_interrupt
  m_emit->tbz(a64::w8, 0, &no_interrupt);

  // x9 <- cause
  // x8 (sr) & cause
  m_emit->ldr(a64::w9, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, cop0_regs.cause.bits)));
  m_emit->and_(a64::w8, a64::w8, a64::w9);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  m_emit->tst(a64::w8, 0xFF00);
  m_emit->b(&no_interrupt, a64::eq);

  // we have an interrupt
  EmitCall(reinterpret_cast<const void*>(&DispatchInterrupt));

  // no interrupt or we just serviced it
  m_emit->Bind(&no_interrupt);

  // TimingEvents::UpdateCPUDowncount:
  // x8 <- head event->downcount
  // downcount <- x8
  EmitLoadGlobalAddress(8, TimingEvents::GetHeadEventPtr());
  m_emit->ldr(a64::x8, a64::MemOperand(a64::x8));
  m_emit->ldr(a64::w8, a64::MemOperand(a64::x8, offsetof(TimingEvent, m_downcount)));
  m_emit->str(a64::w8, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, downcount)));

  // main dispatch loop
  a64::Label main_loop;
  m_emit->Bind(&main_loop);
  s_dispatcher_return_address = GetCurrentCodePointer();

  // w8 <- pending_ticks
  // w9 <- downcount
  m_emit->ldr(a64::w8, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, pending_ticks)));
  m_emit->ldr(a64::w9, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, downcount)));

  // while downcount < pending_ticks
  a64::Label downcount_hit;
  m_emit->cmp(a64::w8, a64::w9);
  m_emit->b(&downcount_hit, a64::ge);

  // time to lookup the block
  // w8 <- pc
  m_emit->Mov(a64::w11, Bus::BIOS_BASE);
  m_emit->ldr(a64::w8, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, regs.pc)));

  // current_instruction_pc <- pc (eax)
  m_emit->str(a64::w8, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, current_instruction_pc)));

  // w9 <- (pc & RAM_MASK) >> 2
  m_emit->and_(a64::w9, a64::w8, Bus::RAM_MASK);
  m_emit->lsr(a64::w9, a64::w9, 2);

  // w10 <- ((pc & BIOS_MASK) >> 2) + FAST_MAP_RAM_SLOT_COUNT
  m_emit->and_(a64::w10, a64::w8, Bus::BIOS_MASK);
  m_emit->lsr(a64::w10, a64::w10, 2);
  m_emit->add(a64::w10, a64::w10, FAST_MAP_RAM_SLOT_COUNT);

  // if ((w8 (pc) & PHYSICAL_MEMORY_ADDRESS_MASK) >= BIOS_BASE) { use w10 as index }
  m_emit->and_(a64::w8, a64::w8, PHYSICAL_MEMORY_ADDRESS_MASK);
  m_emit->cmp(a64::w8, a64::w11);
  m_emit->csel(a64::w8, a64::w9, a64::w10, a64::lt);

  // ebx contains our index, rax <- fast_map[ebx * 8], rax(), continue
  EmitLoadGlobalAddress(9, CodeCache::GetFastMapPointer());
  m_emit->ldr(a64::x8, a64::MemOperand(a64::x9, a64::x8, a64::LSL, 3));
  m_emit->blr(a64::x8);

  // end while
  m_emit->Bind(&downcount_hit);

  // check events then for frame done
  m_emit->ldr(a64::w8, a64::MemOperand(GetHostReg64(RCPUPTR), offsetof(State, pending_ticks)));
  EmitLoadGlobalAddress(9, TimingEvents::GetHeadEventPtr());
  m_emit->ldr(a64::x9, a64::MemOperand(a64::x9));
  m_emit->ldr(a64::w9, a64::MemOperand(a64::x9, offsetof(TimingEvent, m_downcount)));
  m_emit->cmp(a64::w8, a64::w9);
  m_emit->b(&frame_done_loop, a64::lt);
  EmitCall(reinterpret_cast<const void*>(&TimingEvents::RunEvents));
  m_emit->b(&frame_done_loop);

  // all done
  m_emit->Bind(&exit_dispatcher);
  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->add(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  m_emit->ret();

  CodeBlock::HostCodePointer ptr;
  u32 code_size;
  FinalizeBlock(&ptr, &code_size);
  Log_DevPrintf("Dispatcher is %u bytes at %p", code_size, ptr);
  return reinterpret_cast<CodeCache::DispatcherFunction>(ptr);
}

CodeCache::SingleBlockDispatcherFunction CodeGenerator::CompileSingleBlockDispatcher()
{
  m_emit->sub(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  m_register_cache.ReserveCalleeSavedRegisters();
  const u32 stack_adjust = PrepareStackForCall();

  EmitLoadGlobalAddress(RCPUPTR, &g_state);

  m_emit->blr(GetHostReg64(RARG1));

  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->add(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  m_emit->ret();

  CodeBlock::HostCodePointer ptr;
  u32 code_size;
  FinalizeBlock(&ptr, &code_size);
  Log_DevPrintf("Dispatcher is %u bytes at %p", code_size, ptr);
  return reinterpret_cast<CodeCache::SingleBlockDispatcherFunction>(ptr);
}

} // namespace CPU::Recompiler
