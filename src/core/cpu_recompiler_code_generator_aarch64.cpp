#include "YBaseLib/Log.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
Log_SetChannel(CPU::Recompiler);

namespace a64 = vixl::aarch64;

namespace CPU::Recompiler {

constexpr HostReg RCPUPTR = 19;
constexpr HostReg RRETURN = 0;
constexpr HostReg RARG1 = 0;
constexpr HostReg RARG2 = 1;
constexpr HostReg RARG3 = 2;
constexpr HostReg RARG4 = 3;
constexpr u64 FUNCTION_CALL_STACK_ALIGNMENT = 16;
constexpr u64 FUNCTION_CALL_SHADOW_SPACE = 32;
constexpr u64 FUNCTION_CALLEE_SAVED_SPACE_RESERVE = 80;  // 8 registers
constexpr u64 FUNCTION_CALLER_SAVED_SPACE_RESERVE = 144; // 18 registers -> 224 bytes
constexpr u64 FUNCTION_STACK_SIZE =
  FUNCTION_CALLEE_SAVED_SPACE_RESERVE + FUNCTION_CALLER_SAVED_SPACE_RESERVE + FUNCTION_CALL_SHADOW_SPACE;

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

CodeGenerator::CodeGenerator(Core* cpu, JitCodeBuffer* code_buffer, const ASMFunctions& asm_functions)
  : m_cpu(cpu), m_code_buffer(code_buffer), m_asm_functions(asm_functions), m_register_cache(*this),
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
    {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17});
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

Value CodeGenerator::GetValueInHostRegister(const Value& value)
{
  if (value.IsInHostRegister())
    return Value::FromHostReg(&m_register_cache, value.host_reg, value.size);

  if (value.HasConstantValue(0))
    return Value::FromHostReg(&m_register_cache, static_cast<HostReg>(31), value.size);

  Value new_value = m_register_cache.AllocateScratch(value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

void CodeGenerator::EmitBeginBlock()
{
  m_emit->Sub(a64::sp, a64::sp, FUNCTION_STACK_SIZE);

  // Save the link register, since we'll be calling functions.
  const bool link_reg_allocated = m_register_cache.AllocateHostReg(30);
  DebugAssert(link_reg_allocated);

  // Store the CPU struct pointer.
  const bool cpu_reg_allocated = m_register_cache.AllocateHostReg(RCPUPTR);
  DebugAssert(cpu_reg_allocated);
  m_emit->Mov(GetCPUPtrReg(), GetHostReg64(RARG1));
}

void CodeGenerator::EmitEndBlock()
{
  m_register_cache.FreeHostReg(RCPUPTR);
  m_register_cache.PopCalleeSavedRegisters(true);

  m_emit->Add(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  m_emit->Ret();
}

void CodeGenerator::EmitExceptionExit()
{
  // toss away our PC value since we're jumping to the exception handler
  m_register_cache.InvalidateGuestRegister(Reg::pc);

  // ensure all unflushed registers are written back
  m_register_cache.FlushAllGuestRegisters(false, false);

  // the interpreter load delay might have its own value, but we'll overwrite it here anyway
  // technically RaiseException() and FlushPipeline() have already been called, but that should be okay
  m_register_cache.FlushLoadDelay(false);

  m_register_cache.PopCalleeSavedRegisters(false);

  m_emit->Add(a64::sp, a64::sp, FUNCTION_STACK_SIZE);
  m_emit->Ret();
}

void CodeGenerator::EmitExceptionExitOnBool(const Value& value)
{
  Assert(!value.IsConstant() && value.IsInHostRegister());

  m_register_cache.PushState();

  // TODO: This is... not great.
  a64::Label skip_branch;
  m_emit->Cbz(GetHostReg64(value.host_reg), &skip_branch);
  {
    Value temp = m_register_cache.AllocateScratch(RegSize_64);
    m_emit->Mov(GetHostReg64(temp), reinterpret_cast<intptr_t>(GetCurrentFarCodePointer()));
    m_emit->Br(GetHostReg64(temp));
  }
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
  *out_host_code_size = m_near_emitter.GetSizeOfCodeGenerated();

  m_code_buffer->CommitCode(m_near_emitter.GetSizeOfCodeGenerated());
  m_code_buffer->CommitFarCode(m_far_emitter.GetSizeOfCodeGenerated());

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
  Value temp_value = m_register_cache.AllocateScratch(value.size);
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
  Value temp_value = m_register_cache.AllocateScratch(value.size);
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
  if (a64::Assembler::IsImmAddSub(constant_value))
  {
    if (value.size < RegSize_64)
      m_emit->cmp(GetHostReg32(to_reg), constant_value);
    else
      m_emit->cmp(GetHostReg64(to_reg), constant_value);

    return;
  }

  // need a temporary
  Value temp_value = m_register_cache.AllocateScratch(value.size);
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

void CodeGenerator::EmitShl(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value)
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

void CodeGenerator::EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value)
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

void CodeGenerator::EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value)
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
  Value temp_value = m_register_cache.AllocateScratch(value.size);
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
  Value temp_value = m_register_cache.AllocateScratch(value.size);
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
  Value temp_value = m_register_cache.AllocateScratch(value.size);
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
  Value temp_value = m_register_cache.AllocateScratch(value.size);
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

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr)
{
  if (return_value)
    return_value->Discard();

  // must be allocated before the stack push
  Value temp = m_register_cache.AllocateScratch(RegSize_64);

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // actually call the function
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // must happen after the stack push
  temp.ReleaseAndClear();

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

  // must be allocated before the stack push
  Value temp = m_register_cache.AllocateScratch(RegSize_64);

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);

  // actually call the function
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // must happen after the stack push
  temp.ReleaseAndClear();

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

  // must be allocated before the stack push
  Value temp = m_register_cache.AllocateScratch(RegSize_64);

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);
  EmitCopyValue(RARG2, arg2);

  // actually call the function
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // must happen after the stack push
  temp.ReleaseAndClear();

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

  // must be allocated before the stack push
  Value temp = m_register_cache.AllocateScratch(RegSize_64);

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);
  EmitCopyValue(RARG2, arg2);
  EmitCopyValue(RARG3, arg3);

  // actually call the function
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // must happen after the stack push
  temp.ReleaseAndClear();

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

  // must be allocated before the stack push
  Value temp = m_register_cache.AllocateScratch(RegSize_64);

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // push arguments
  EmitCopyValue(RARG1, arg1);
  EmitCopyValue(RARG2, arg2);
  EmitCopyValue(RARG3, arg3);
  EmitCopyValue(RARG4, arg4);

  // actually call the function
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));

  // shadow space release
  RestoreStackAfterCall(adjust_size);

  // must happen after the stack push
  temp.ReleaseAndClear();

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
  m_emit->Str(GetHostReg64(reg), addr);
}

void CodeGenerator::EmitPopHostReg(HostReg reg, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - FUNCTION_CALL_SHADOW_SPACE - (position * 8));
  m_emit->Ldr(GetHostReg64(reg), addr);
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
  DebugAssert(value.IsInHostRegister() || value.IsConstant());

  const s64 s_offset = static_cast<s64>(ZeroExtend64(offset));
  const a64::MemOperand o_offset(GetCPUPtrReg(), s_offset);

  // Don't need to mask here because we're storing back to memory.
  Value temp = m_register_cache.AllocateScratch(value.size);
  switch (value.size)
  {
    case RegSize_8:
    {
      m_emit->Ldrb(GetHostReg8(temp), o_offset);
      if (value.IsConstant())
        m_emit->Add(GetHostReg8(temp), GetHostReg8(temp), value.GetS64ConstantValue());
      else
        m_emit->Add(GetHostReg8(temp), GetHostReg8(temp), GetHostReg8(value));
      m_emit->Strb(GetHostReg8(temp), o_offset);
    }
    break;

    case RegSize_16:
    {
      m_emit->Ldrh(GetHostReg16(temp), o_offset);
      if (value.IsConstant())
        m_emit->Add(GetHostReg16(temp), GetHostReg16(temp), value.GetS64ConstantValue());
      else
        m_emit->Add(GetHostReg16(temp), GetHostReg16(temp), GetHostReg16(value));
      m_emit->Strh(GetHostReg16(temp), o_offset);
    }
    break;

    case RegSize_32:
    {
      m_emit->Ldr(GetHostReg32(temp), o_offset);
      if (value.IsConstant())
        m_emit->Add(GetHostReg32(temp), GetHostReg32(temp), value.GetS64ConstantValue());
      else
        m_emit->Add(GetHostReg32(temp), GetHostReg32(temp), GetHostReg32(value));
      m_emit->Str(GetHostReg32(temp), o_offset);
    }
    break;

    case RegSize_64:
    {
      m_emit->Ldr(GetHostReg64(temp), o_offset);
      if (value.IsConstant())
        m_emit->Add(GetHostReg64(temp), GetHostReg64(temp), s64(value.constant_value));
      else
        m_emit->Add(GetHostReg64(temp), GetHostReg64(temp), GetHostReg64(value));
      m_emit->Str(GetHostReg64(temp), o_offset);
    }
    break;

    default:
    {
      UnreachableCode();
    }
    break;
  }
}

Value CodeGenerator::EmitLoadGuestMemory(const Value& address, RegSize size)
{
  // We need to use the full 64 bits here since we test the sign bit result.
  Value result = m_register_cache.AllocateScratch(RegSize_64);

  // NOTE: This can leave junk in the upper bits
  switch (size)
  {
    case RegSize_8:
      EmitFunctionCall(&result, &Thunks::ReadMemoryByte, m_register_cache.GetCPUPtr(), address);
      break;

    case RegSize_16:
      EmitFunctionCall(&result, &Thunks::ReadMemoryHalfWord, m_register_cache.GetCPUPtr(), address);
      break;

    case RegSize_32:
      EmitFunctionCall(&result, &Thunks::ReadMemoryWord, m_register_cache.GetCPUPtr(), address);
      break;

    default:
      UnreachableCode();
      break;
  }

  a64::Label load_okay;
  m_emit->Tbz(GetHostReg64(result.host_reg), 63, &load_okay);
  m_emit->Mov(GetHostReg64(result.host_reg), reinterpret_cast<intptr_t>(GetCurrentFarCodePointer()));
  m_emit->Br(GetHostReg64(result.host_reg));
  m_emit->Bind(&load_okay);

  m_register_cache.PushState();

  // load exception path
  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();

  // Downcast to ignore upper 56/48/32 bits. This should be a noop.
  switch (size)
  {
    case RegSize_8:
      ConvertValueSizeInPlace(&result, RegSize_8, false);
      break;

    case RegSize_16:
      ConvertValueSizeInPlace(&result, RegSize_16, false);
      break;

    case RegSize_32:
      ConvertValueSizeInPlace(&result, RegSize_32, false);
      break;

    default:
      UnreachableCode();
      break;
  }

  return result;
}

void CodeGenerator::EmitStoreGuestMemory(const Value& address, const Value& value)
{
  Value result = m_register_cache.AllocateScratch(RegSize_8);

  switch (value.size)
  {
    case RegSize_8:
      EmitFunctionCall(&result, &Thunks::WriteMemoryByte, m_register_cache.GetCPUPtr(), address, value);
      break;

    case RegSize_16:
      EmitFunctionCall(&result, &Thunks::WriteMemoryHalfWord, m_register_cache.GetCPUPtr(), address, value);
      break;

    case RegSize_32:
      EmitFunctionCall(&result, &Thunks::WriteMemoryWord, m_register_cache.GetCPUPtr(), address, value);
      break;

    default:
      UnreachableCode();
      break;
  }

  a64::Label store_okay;
  m_emit->Cbnz(GetHostReg64(result.host_reg), &store_okay);
  m_emit->Mov(GetHostReg64(result.host_reg), reinterpret_cast<intptr_t>(GetCurrentFarCodePointer()));
  m_emit->Br(GetHostReg64(result.host_reg));
  m_emit->Bind(&store_okay);

  m_register_cache.PushState();

  // store exception path
  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();
}

void CodeGenerator::EmitFlushInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_32);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(Core, m_load_delay_reg));
  const a64::MemOperand load_delay_value(GetCPUPtrReg(), offsetof(Core, m_load_delay_value));
  const a64::MemOperand regs_base(GetCPUPtrReg(), offsetof(Core, m_regs.r[0]));

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
  m_emit->Add(GetHostReg32(reg), GetHostReg32(reg), offsetof(Core, m_regs.r[0]));

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

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(Core, m_load_delay_reg));
  const a64::MemOperand load_delay_value(GetCPUPtrReg(), offsetof(Core, m_load_delay_value));
  const a64::MemOperand next_load_delay_reg(GetCPUPtrReg(), offsetof(Core, m_next_load_delay_reg));
  const a64::MemOperand next_load_delay_value(GetCPUPtrReg(), offsetof(Core, m_next_load_delay_value));

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

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(Core, m_load_delay_reg));
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

  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(address));
  m_emit->br(GetHostReg64(temp));
}

template<typename T>
static void EmitConditionalJump(Condition condition, bool invert, a64::MacroAssembler* emit, const T& label)
{
  switch (condition)
  {
    case Condition::Always:
      emit->b(label);
      break;

    case Condition::NotEqual:
      invert ? emit->b(label, a64::eq) : emit->b(label, a64::ne);
      break;

    case Condition::Equal:
      invert ? emit->b(label, a64::ne) : emit->b(label, a64::eq);
      break;

    case Condition::Overflow:
      invert ? emit->b(label, a64::vc) : emit->b(label, a64::vs);
      break;

    case Condition::Greater:
      invert ? emit->b(label, a64::le) : emit->b(label, a64::gt);
      break;

    case Condition::GreaterEqual:
      invert ? emit->b(label, a64::lt) : emit->b(label, a64::ge);
      break;

    case Condition::Less:
      invert ? emit->b(label, a64::ge) : emit->b(label, a64::lt);
      break;

    case Condition::LessEqual:
      invert ? emit->b(label, a64::gt) : emit->b(label, a64::le);
      break;

    case Condition::Negative:
      invert ? emit->b(label, a64::pl) : emit->b(label, a64::mi);
      break;

    case Condition::PositiveOrZero:
      invert ? emit->b(label, a64::mi) : emit->b(label, a64::pl);
      break;

    case Condition::Above:
      invert ? emit->b(label, a64::ls) : emit->b(label, a64::hi);
      break;

    case Condition::AboveEqual:
      invert ? emit->b(label, a64::cc) : emit->b(label, a64::cs);
      break;

    case Condition::Below:
      invert ? emit->b(label, a64::cs) : emit->b(label, a64::cc);
      break;

    case Condition::BelowEqual:
      invert ? emit->b(label, a64::hi) : emit->b(label, a64::ls);
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitBranch(Condition condition, Reg lr_reg, Value&& branch_target)
{
  // ensure the lr register is flushed, since we want it's correct value after the branch
  // we don't want to invalidate it yet because of "jalr r0, r0", branch_target could be the lr_reg.
  if (lr_reg != Reg::count && lr_reg != Reg::zero)
    m_register_cache.FlushGuestRegister(lr_reg, false, true);

  // compute return address, which is also set as the new pc when the branch isn't taken
  Value new_pc;
  if (condition != Condition::Always || lr_reg != Reg::count)
  {
    new_pc = AddValues(m_register_cache.ReadGuestRegister(Reg::pc), Value::FromConstantU32(4), false);
    if (!new_pc.IsInHostRegister())
      new_pc = GetValueInHostRegister(new_pc);
  }

  a64::Label skip_branch;
  if (condition != Condition::Always)
  {
    // condition is inverted because we want the case for skipping it
    EmitConditionalJump(condition, true, m_emit, &skip_branch);
  }

  // save the old PC if we want to
  if (lr_reg != Reg::count && lr_reg != Reg::zero)
  {
    // Can't cache because we have two branches. Load delay cancel is due to the immediate flush afterwards,
    // if we don't cancel it, at the end of the instruction the value we write can be overridden.
    EmitCancelInterpreterLoadDelayForReg(lr_reg);
    EmitStoreGuestRegister(lr_reg, new_pc);
  }

  // we don't need to test the address of constant branches unless they're definitely misaligned, which would be
  // strange.
  if (!branch_target.IsConstant() || (branch_target.constant_value & 0x3) != 0)
  {
    m_register_cache.PushState();

    if (branch_target.IsConstant())
    {
      Log_WarningPrintf("Misaligned constant target branch 0x%08X, this is strange",
                        Truncate32(branch_target.constant_value));
    }
    else
    {
      // check the alignment of the target
      a64::Label branch_target_okay;
      m_emit->tst(GetHostReg32(branch_target), 0x3);
      m_emit->B(a64::eq, &branch_target_okay);

      Value far_code_addr = m_register_cache.AllocateScratch(RegSize_64);
      m_emit->Mov(GetHostReg64(far_code_addr), reinterpret_cast<intptr_t>(GetCurrentFarCodePointer()));
      m_emit->br(GetHostReg64(far_code_addr));
      m_emit->Bind(&branch_target_okay);
    }

    // exception exit for misaligned target
    SwitchToFarCode();
    EmitFunctionCall(nullptr, &Thunks::RaiseAddressException, m_register_cache.GetCPUPtr(), branch_target,
                     Value::FromConstantU8(0), Value::FromConstantU8(1));
    EmitExceptionExit();
    SwitchToNearCode();

    m_register_cache.PopState();
  }

  // branch taken path - change the return address/new pc
  if (condition != Condition::Always)
    EmitCopyValue(new_pc.GetHostRegister(), branch_target);

  // converge point
  m_emit->Bind(&skip_branch);

  // update pc
  if (condition != Condition::Always)
    m_register_cache.WriteGuestRegister(Reg::pc, std::move(new_pc));
  else
    m_register_cache.WriteGuestRegister(Reg::pc, std::move(branch_target));

  // now invalidate lr becuase it was possibly written in the branch, and we don't need branch_target anymore
  if (lr_reg != Reg::count && lr_reg != Reg::zero)
    m_register_cache.InvalidateGuestRegister(lr_reg);
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

void CodeGenerator::EmitRaiseException(Exception excode, Condition condition /* = Condition::Always */)
{
  if (condition == Condition::Always)
  {
    // no need to use far code if we're always raising the exception
    m_register_cache.InvalidateGuestRegister(Reg::pc);
    m_register_cache.FlushAllGuestRegisters(true, true);
    m_register_cache.FlushLoadDelay(true);

    EmitFunctionCall(nullptr, &Thunks::RaiseException, m_register_cache.GetCPUPtr(),
                     Value::FromConstantU8(static_cast<u8>(excode)));
    return;
  }

  a64::Label skip_raise_exception;
  EmitConditionalJump(condition, true, m_emit, &skip_raise_exception);

  m_register_cache.PushState();

  {
    Value far_code_addr = m_register_cache.AllocateScratch(RegSize_64);
    m_emit->Mov(GetHostReg64(far_code_addr), reinterpret_cast<intptr_t>(GetCurrentFarCodePointer()));
    m_emit->Br(GetHostReg64(far_code_addr));
  }

  m_emit->Bind(&skip_raise_exception);

  SwitchToFarCode();
  EmitFunctionCall(nullptr, &Thunks::RaiseException, m_register_cache.GetCPUPtr(),
                   Value::FromConstantU8(static_cast<u8>(excode)));
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();
}

void ASMFunctions::Generate(JitCodeBuffer* code_buffer) {}

} // namespace CPU::Recompiler
