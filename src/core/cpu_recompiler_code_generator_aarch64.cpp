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
constexpr u64 FUNCTION_CALLEE_SAVED_SPACE_RESERVE = 80;     // 8 registers
constexpr u64 FUNCTION_CALLER_SAVED_SPACE_RESERVE = 144;    // 18 registers -> 224 bytes
constexpr u64 FUNCTION_STACK_SIZE = FUNCTION_CALLEE_SAVED_SPACE_RESERVE +
                                    FUNCTION_CALLER_SAVED_SPACE_RESERVE +
                                    FUNCTION_CALL_SHADOW_SPACE;

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

  // TODO: This is... not great.
  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  a64::Label skip_branch;
  m_emit->Cbz(GetHostReg64(value.host_reg), &skip_branch);
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<intptr_t>(GetCurrentFarCodePointer()));
  m_emit->Br(GetHostReg64(temp));
  m_emit->Bind(&skip_branch);

  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();
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
  Panic("Not implemented");
#if 0
  switch (to_size)
  {
    case RegSize_16:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->movsx(GetHostReg16(to_reg), GetHostReg8(from_reg));
          return;
      }
    }
    break;

    case RegSize_32:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->movsx(GetHostReg32(to_reg), GetHostReg8(from_reg));
          return;
        case RegSize_16:
          m_emit->movsx(GetHostReg32(to_reg), GetHostReg16(from_reg));
          return;
      }
    }
    break;
  }
#endif

  Panic("Unknown sign-extend combination");
}

void CodeGenerator::EmitZeroExtend(HostReg to_reg, RegSize to_size, HostReg from_reg, RegSize from_size)
{
    Panic("Not implemented");
#if 0
  switch (to_size)
  {
    case RegSize_16:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->movzx(GetHostReg16(to_reg), GetHostReg8(from_reg));
          return;
      }
    }
    break;

    case RegSize_32:
    {
      switch (from_size)
      {
        case RegSize_8:
          m_emit->movzx(GetHostReg32(to_reg), GetHostReg8(from_reg));
          return;
        case RegSize_16:
          m_emit->movzx(GetHostReg32(to_reg), GetHostReg16(from_reg));
          return;
      }
    }
    break;
  }
#endif

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

void CodeGenerator::EmitAdd(HostReg to_reg, const Value& value, bool set_flags)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
    Panic("Not implemented");

#if 0

  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->add(GetHostReg8(to_reg), SignExtend32(Truncate8(value.constant_value)));
      else
        m_emit->add(GetHostReg8(to_reg), GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->add(GetHostReg16(to_reg), SignExtend32(Truncate16(value.constant_value)));
      else
        m_emit->add(GetHostReg16(to_reg), GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->add(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->add(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          m_emit->mov(GetHostReg64(temp.host_reg), value.constant_value);
          m_emit->add(GetHostReg64(to_reg), GetHostReg64(temp.host_reg));
        }
        else
        {
          m_emit->add(GetHostReg64(to_reg), Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->add(GetHostReg64(to_reg), GetHostReg64(value.host_reg));
      }
    }
    break;
  }
#endif
}

void CodeGenerator::EmitSub(HostReg to_reg, const Value& value, bool set_flags)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
    Panic("Not implemented");

#if 0

  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->sub(GetHostReg8(to_reg), SignExtend32(Truncate8(value.constant_value)));
      else
        m_emit->sub(GetHostReg8(to_reg), GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->sub(GetHostReg16(to_reg), SignExtend32(Truncate16(value.constant_value)));
      else
        m_emit->sub(GetHostReg16(to_reg), GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->sub(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->sub(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          m_emit->mov(GetHostReg64(temp.host_reg), value.constant_value);
          m_emit->sub(GetHostReg64(to_reg), GetHostReg64(temp.host_reg));
        }
        else
        {
          m_emit->sub(GetHostReg64(to_reg), Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->sub(GetHostReg64(to_reg), GetHostReg64(value.host_reg));
      }
    }
    break;
  }
#endif
}

void CodeGenerator::EmitCmp(HostReg to_reg, const Value& value)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  Panic("Not implemented");
#if 0

  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->cmp(GetHostReg8(to_reg), SignExtend32(Truncate8(value.constant_value)));
      else
        m_emit->cmp(GetHostReg8(to_reg), GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->cmp(GetHostReg16(to_reg), SignExtend32(Truncate16(value.constant_value)));
      else
        m_emit->cmp(GetHostReg16(to_reg), GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->cmp(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->cmp(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          m_emit->mov(GetHostReg64(temp.host_reg), value.constant_value);
          m_emit->cmp(GetHostReg64(to_reg), GetHostReg64(temp.host_reg));
        }
        else
        {
          m_emit->cmp(GetHostReg64(to_reg), Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->cmp(GetHostReg64(to_reg), GetHostReg64(value.host_reg));
      }
    }
    break;
  }
#endif
}

void CodeGenerator::EmitMul(HostReg to_reg_hi, HostReg to_reg_lo, const Value& lhs, const Value& rhs,
                            bool signed_multiply)
{
    Panic("Not implemented");
#if 0
  const bool save_eax = (to_reg_hi != Xbyak::Operand::RAX && to_reg_lo != Xbyak::Operand::RAX);
  const bool save_edx = (to_reg_hi != Xbyak::Operand::RDX && to_reg_lo != Xbyak::Operand::RDX);

  if (save_eax)
    m_emit->push(m_emit->rax);

  if (save_edx)
    m_emit->push(m_emit->rdx);

#define DO_MUL(src)                                                                                                    \
  if (lhs.size == RegSize_8)                                                                                           \
    signed_multiply ? m_emit->imul(src.changeBit(8)) : m_emit->mul(src.changeBit(8));                                  \
  else if (lhs.size == RegSize_16)                                                                                     \
    signed_multiply ? m_emit->imul(src.changeBit(16)) : m_emit->mul(src.changeBit(16));                                \
  else if (lhs.size == RegSize_32)                                                                                     \
    signed_multiply ? m_emit->imul(src.changeBit(32)) : m_emit->mul(src.changeBit(32));                                \
  else                                                                                                                 \
    signed_multiply ? m_emit->imul(src.changeBit(64)) : m_emit->mul(src.changeBit(64));

  // x*x
  if (lhs.IsInHostRegister() && rhs.IsInHostRegister() && lhs.GetHostRegister() == rhs.GetHostRegister())
  {
    if (lhs.GetHostRegister() != Xbyak::Operand::RAX)
      EmitCopyValue(Xbyak::Operand::RAX, lhs);

    DO_MUL(m_emit->rax);
  }
  else if (lhs.IsInHostRegister() && lhs.GetHostRegister() == Xbyak::Operand::RAX)
  {
    if (!rhs.IsInHostRegister())
    {
      EmitCopyValue(Xbyak::Operand::RDX, rhs);
      DO_MUL(m_emit->rdx);
    }
    else
    {
      DO_MUL(GetHostReg64(rhs));
    }
  }
  else if (rhs.IsInHostRegister() && rhs.GetHostRegister() == Xbyak::Operand::RAX)
  {
    if (!lhs.IsInHostRegister())
    {
      EmitCopyValue(Xbyak::Operand::RDX, lhs);
      DO_MUL(m_emit->rdx);
    }
    else
    {
      DO_MUL(GetHostReg64(lhs));
    }
  }
  else
  {
    if (lhs.IsInHostRegister())
    {
      EmitCopyValue(Xbyak::Operand::RAX, rhs);
      if (lhs.size == RegSize_8)
        signed_multiply ? m_emit->imul(GetHostReg8(lhs)) : m_emit->mul(GetHostReg8(lhs));
      else if (lhs.size == RegSize_16)
        signed_multiply ? m_emit->imul(GetHostReg16(lhs)) : m_emit->mul(GetHostReg16(lhs));
      else if (lhs.size == RegSize_32)
        signed_multiply ? m_emit->imul(GetHostReg32(lhs)) : m_emit->mul(GetHostReg32(lhs));
      else
        signed_multiply ? m_emit->imul(GetHostReg64(lhs)) : m_emit->mul(GetHostReg64(lhs));
    }
    else if (rhs.IsInHostRegister())
    {
      EmitCopyValue(Xbyak::Operand::RAX, lhs);
      if (lhs.size == RegSize_8)
        signed_multiply ? m_emit->imul(GetHostReg8(rhs)) : m_emit->mul(GetHostReg8(rhs));
      else if (lhs.size == RegSize_16)
        signed_multiply ? m_emit->imul(GetHostReg16(rhs)) : m_emit->mul(GetHostReg16(rhs));
      else if (lhs.size == RegSize_32)
        signed_multiply ? m_emit->imul(GetHostReg32(rhs)) : m_emit->mul(GetHostReg32(rhs));
      else
        signed_multiply ? m_emit->imul(GetHostReg64(rhs)) : m_emit->mul(GetHostReg64(rhs));
    }
    else
    {
      EmitCopyValue(Xbyak::Operand::RAX, lhs);
      EmitCopyValue(Xbyak::Operand::RDX, rhs);
      DO_MUL(m_emit->rdx);
    }
  }

#undef DO_MUL

  if (to_reg_hi == Xbyak::Operand::RDX && to_reg_lo == Xbyak::Operand::RAX)
  {
    // ideal case: registers are the ones we want: don't have to do anything
  }
  else if (to_reg_hi == Xbyak::Operand::RAX && to_reg_lo == Xbyak::Operand::RDX)
  {
    // what we want, but swapped, so exchange them
    m_emit->xchg(m_emit->rax, m_emit->rdx);
  }
  else
  {
    // store to the registers we want.. this could be optimized better
    m_emit->push(m_emit->rdx);
    m_emit->push(m_emit->rax);
    m_emit->pop(GetHostReg64(to_reg_lo));
    m_emit->pop(GetHostReg64(to_reg_hi));
  }

  // restore original contents
  if (save_edx)
    m_emit->pop(m_emit->rdx);

  if (save_eax)
    m_emit->pop(m_emit->rax);
#endif
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

void CodeGenerator::EmitShl(HostReg to_reg, RegSize size, const Value& amount_value)
{
    Panic("Not implemented");
#if 0
  DebugAssert(amount_value.IsConstant() || amount_value.IsInHostRegister());

  // We have to use CL for the shift amount :(
  const bool save_cl = (!amount_value.IsConstant() && m_register_cache.IsHostRegInUse(Xbyak::Operand::RCX) &&
                        (!amount_value.IsInHostRegister() || amount_value.host_reg != Xbyak::Operand::RCX));
  if (save_cl)
    m_emit->push(m_emit->rcx);

  if (!amount_value.IsConstant())
    m_emit->mov(m_emit->cl, GetHostReg8(amount_value.host_reg));

  switch (size)
  {
    case RegSize_8:
    {
      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg8(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg8(to_reg), m_emit->cl);
    }
    break;

    case RegSize_16:
    {
      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg16(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg16(to_reg), m_emit->cl);
    }
    break;

    case RegSize_32:
    {
      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg32(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg32(to_reg), m_emit->cl);
    }
    break;

    case RegSize_64:
    {
      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg64(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg64(to_reg), m_emit->cl);
    }
    break;
  }

  if (save_cl)
    m_emit->pop(m_emit->rcx);
#endif
}

void CodeGenerator::EmitShr(HostReg to_reg, RegSize size, const Value& amount_value)
{
    Panic("Not implemented");
#if 0
  DebugAssert(amount_value.IsConstant() || amount_value.IsInHostRegister());

  // We have to use CL for the shift amount :(
  const bool save_cl = (!amount_value.IsConstant() && m_register_cache.IsHostRegInUse(Xbyak::Operand::RCX) &&
                        (!amount_value.IsInHostRegister() || amount_value.host_reg != Xbyak::Operand::RCX));
  if (save_cl)
    m_emit->push(m_emit->rcx);

  if (!amount_value.IsConstant())
    m_emit->mov(m_emit->cl, GetHostReg8(amount_value.host_reg));

  switch (size)
  {
    case RegSize_8:
    {
      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg8(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg8(to_reg), m_emit->cl);
    }
    break;

    case RegSize_16:
    {
      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg16(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg16(to_reg), m_emit->cl);
    }
    break;

    case RegSize_32:
    {
      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg32(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg32(to_reg), m_emit->cl);
    }
    break;

    case RegSize_64:
    {
      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg64(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg64(to_reg), m_emit->cl);
    }
    break;
  }

  if (save_cl)
    m_emit->pop(m_emit->rcx);
#endif
}

void CodeGenerator::EmitSar(HostReg to_reg, RegSize size, const Value& amount_value)
{
    Panic("Not implemented");
#if 0
  DebugAssert(amount_value.IsConstant() || amount_value.IsInHostRegister());

  // We have to use CL for the shift amount :(
  const bool save_cl = (!amount_value.IsConstant() && m_register_cache.IsHostRegInUse(Xbyak::Operand::RCX) &&
                        (!amount_value.IsInHostRegister() || amount_value.host_reg != Xbyak::Operand::RCX));
  if (save_cl)
    m_emit->push(m_emit->rcx);

  if (!amount_value.IsConstant())
    m_emit->mov(m_emit->cl, GetHostReg8(amount_value.host_reg));

  switch (size)
  {
    case RegSize_8:
    {
      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg8(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg8(to_reg), m_emit->cl);
    }
    break;

    case RegSize_16:
    {
      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg16(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg16(to_reg), m_emit->cl);
    }
    break;

    case RegSize_32:
    {
      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg32(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg32(to_reg), m_emit->cl);
    }
    break;

    case RegSize_64:
    {
      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg64(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg64(to_reg), m_emit->cl);
    }
    break;
  }

  if (save_cl)
    m_emit->pop(m_emit->rcx);
#endif
}

void CodeGenerator::EmitAnd(HostReg to_reg, const Value& value)
{
    Panic("Not implemented");
#if 0
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->and_(GetHostReg8(to_reg), Truncate32(value.constant_value & UINT32_C(0xFF)));
      else
        m_emit->and_(GetHostReg8(to_reg), GetHostReg8(value));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->and_(GetHostReg16(to_reg), Truncate32(value.constant_value & UINT32_C(0xFFFF)));
      else
        m_emit->and_(GetHostReg16(to_reg), GetHostReg16(value));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->and_(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(value));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          m_emit->mov(GetHostReg64(temp), value.constant_value);
          m_emit->and_(GetHostReg64(to_reg), GetHostReg64(temp));
        }
        else
        {
          m_emit->and_(GetHostReg64(to_reg), Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->and_(GetHostReg64(to_reg), GetHostReg64(value));
      }
    }
    break;
  }
#endif
}

void CodeGenerator::EmitOr(HostReg to_reg, const Value& value)
{
    Panic("Not implemented");
#if 0
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->or_(GetHostReg8(to_reg), Truncate32(value.constant_value & UINT32_C(0xFF)));
      else
        m_emit->or_(GetHostReg8(to_reg), GetHostReg8(value));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->or_(GetHostReg16(to_reg), Truncate32(value.constant_value & UINT32_C(0xFFFF)));
      else
        m_emit->or_(GetHostReg16(to_reg), GetHostReg16(value));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->or_(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->or_(GetHostReg32(to_reg), GetHostReg32(value));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          m_emit->mov(GetHostReg64(temp), value.constant_value);
          m_emit->or_(GetHostReg64(to_reg), GetHostReg64(temp));
        }
        else
        {
          m_emit->or_(GetHostReg64(to_reg), Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->or_(GetHostReg64(to_reg), GetHostReg64(value));
      }
    }
    break;
  }
#endif
}

void CodeGenerator::EmitXor(HostReg to_reg, const Value& value)
{
    Panic("Not implemented");
#if 0
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->xor_(GetHostReg8(to_reg), Truncate32(value.constant_value & UINT32_C(0xFF)));
      else
        m_emit->xor_(GetHostReg8(to_reg), GetHostReg8(value));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->xor_(GetHostReg16(to_reg), Truncate32(value.constant_value & UINT32_C(0xFFFF)));
      else
        m_emit->xor_(GetHostReg16(to_reg), GetHostReg16(value));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->xor_(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->xor_(GetHostReg32(to_reg), GetHostReg32(value));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          m_emit->mov(GetHostReg64(temp), value.constant_value);
          m_emit->xor_(GetHostReg64(to_reg), GetHostReg64(temp));
        }
        else
        {
          m_emit->xor_(GetHostReg64(to_reg), Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->xor_(GetHostReg64(to_reg), GetHostReg64(value));
      }
    }
    break;
  }
#endif
}

void CodeGenerator::EmitTest(HostReg to_reg, const Value& value)
{
    Panic("Not implemented");
#if 0
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->test(GetHostReg8(to_reg), Truncate32(value.constant_value & UINT32_C(0xFF)));
      else
        m_emit->test(GetHostReg8(to_reg), GetHostReg8(value));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->test(GetHostReg16(to_reg), Truncate32(value.constant_value & UINT32_C(0xFFFF)));
      else
        m_emit->test(GetHostReg16(to_reg), GetHostReg16(value));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->test(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->test(GetHostReg32(to_reg), GetHostReg32(value));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          m_emit->mov(GetHostReg64(temp), value.constant_value);
          m_emit->test(GetHostReg64(to_reg), GetHostReg64(temp));
        }
        else
        {
          m_emit->test(GetHostReg64(to_reg), Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->test(GetHostReg64(to_reg), GetHostReg64(value));
      }
    }
    break;
  }
#endif
}

void CodeGenerator::EmitNot(HostReg to_reg, RegSize size)
{
    Panic("Not implemented");
#if 0
  switch (size)
  {
    case RegSize_8:
      m_emit->not_(GetHostReg8(to_reg));
      break;

    case RegSize_16:
      m_emit->not_(GetHostReg16(to_reg));
      break;

    case RegSize_32:
      m_emit->not_(GetHostReg32(to_reg));
      break;

    case RegSize_64:
      m_emit->not_(GetHostReg64(to_reg));
      break;

    default:
      break;
  }
#endif
}

void CodeGenerator::EmitSetConditionResult(HostReg to_reg, RegSize to_size, Condition condition)
{
    Panic("Not implemented");
#if 0
  switch (condition)
  {
    case Condition::Always:
      m_emit->mov(GetHostReg8(to_reg), 1);
      break;

    case Condition::NotEqual:
      m_emit->setne(GetHostReg8(to_reg));
      break;

    case Condition::Equal:
      m_emit->sete(GetHostReg8(to_reg));
      break;

    case Condition::Overflow:
      m_emit->seto(GetHostReg8(to_reg));
      break;

    case Condition::Greater:
      m_emit->setg(GetHostReg8(to_reg));
      break;

    case Condition::GreaterEqual:
      m_emit->setge(GetHostReg8(to_reg));
      break;

    case Condition::Less:
      m_emit->setl(GetHostReg8(to_reg));
      break;

    case Condition::LessEqual:
      m_emit->setle(GetHostReg8(to_reg));
      break;

    case Condition::Negative:
      m_emit->sets(GetHostReg8(to_reg));
      break;

    case Condition::PositiveOrZero:
      m_emit->setns(GetHostReg8(to_reg));
      break;

    case Condition::Above:
      m_emit->seta(GetHostReg8(to_reg));
      break;

    case Condition::AboveEqual:
      m_emit->setae(GetHostReg8(to_reg));
      break;

    case Condition::Below:
      m_emit->setb(GetHostReg8(to_reg));
      break;

    case Condition::BelowEqual:
      m_emit->setbe(GetHostReg8(to_reg));
      break;

    default:
      UnreachableCode();
      break;
  }

  if (to_size != RegSize_8)
    EmitZeroExtend(to_reg, to_size, to_reg, RegSize_8);
#endif
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

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // actually call the function
  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));
  temp.ReleaseAndClear();

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
  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));
  temp.ReleaseAndClear();

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
  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));
  temp.ReleaseAndClear();

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
  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));
  temp.ReleaseAndClear();

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
  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  m_emit->Mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(ptr));
  m_emit->Blr(GetHostReg64(temp));
  temp.ReleaseAndClear();

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
        m_emit->Add(GetHostReg8(temp), GetHostReg8(temp), Truncate8(value.constant_value));
      else
        m_emit->Add(GetHostReg8(temp), GetHostReg8(temp), GetHostReg8(value));
      m_emit->Strb(GetHostReg8(temp), o_offset);
    }
    break;

    case RegSize_16:
    {
      m_emit->Ldrh(GetHostReg16(temp), o_offset);
      if (value.IsConstant())
        m_emit->Add(GetHostReg16(temp), GetHostReg16(temp), Truncate16(value.constant_value));
      else
        m_emit->Add(GetHostReg16(temp), GetHostReg16(temp), GetHostReg16(value));
      m_emit->Strh(GetHostReg16(temp), o_offset);
    }
    break;

    case RegSize_32:
    {
      m_emit->Ldr(GetHostReg32(temp), o_offset);
      if (value.IsConstant())
        m_emit->Add(GetHostReg32(temp), GetHostReg32(temp), Truncate32(value.constant_value));
      else
        m_emit->Add(GetHostReg32(temp), GetHostReg32(temp), GetHostReg32(value));
      m_emit->Str(GetHostReg32(temp), o_offset);
    }
    break;

    case RegSize_64:
    {
      m_emit->Ldr(GetHostReg64(temp), o_offset);
      if (value.IsConstant())
        m_emit->Add(GetHostReg64(temp), GetHostReg64(temp), value.constant_value);
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
    Panic("Not implemented");
#if 0
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

  m_emit->test(GetHostReg64(result.host_reg), GetHostReg64(result.host_reg));
  m_emit->js(GetCurrentFarCodePointer());

  // load exception path
  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();

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
#endif
}

void CodeGenerator::EmitStoreGuestMemory(const Value& address, const Value& value)
{
    Panic("Not implemented");
#if 0
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

  Xbyak::Label store_okay;

  m_emit->test(GetHostReg8(result), GetHostReg8(result));
  m_emit->jz(GetCurrentFarCodePointer());

  // store exception path
  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();
#endif
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
    Panic("Not implemented");
#if 0
  if (!m_load_delay_dirty)
    return;

  auto load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(Core, m_load_delay_reg)];

  Xbyak::Label skip_cancel;

  // if load_delay_reg != reg goto skip_cancel
  m_emit->cmp(load_delay_reg, static_cast<u8>(reg));
  m_emit->jne(skip_cancel);

  // load_delay_reg = Reg::count
  m_emit->mov(load_delay_reg, static_cast<u8>(Reg::count));

  m_emit->L(skip_cancel);
#endif
}

#if 0
template<typename T>
static void EmitConditionalJump(Condition condition, bool invert, Xbyak::CodeGenerator* emit, const T& label)
{
  switch (condition)
  {
    case Condition::Always:
      emit->jmp(label);
      break;

    case Condition::NotEqual:
      invert ? emit->je(label) : emit->jne(label);
      break;

    case Condition::Equal:
      invert ? emit->jne(label) : emit->je(label);
      break;

    case Condition::Overflow:
      invert ? emit->jno(label) : emit->jo(label);
      break;

    case Condition::Greater:
      invert ? emit->jng(label) : emit->jg(label);
      break;

    case Condition::GreaterEqual:
      invert ? emit->jnge(label) : emit->jge(label);
      break;

    case Condition::Less:
      invert ? emit->jnl(label) : emit->jl(label);
      break;

    case Condition::LessEqual:
      invert ? emit->jnle(label) : emit->jle(label);
      break;

    case Condition::Negative:
      invert ? emit->jns(label) : emit->js(label);
      break;

    case Condition::PositiveOrZero:
      invert ? emit->js(label) : emit->jns(label);
      break;

    case Condition::Above:
      invert ? emit->jna(label) : emit->ja(label);
      break;

    case Condition::AboveEqual:
      invert ? emit->jnae(label) : emit->jae(label);
      break;

    case Condition::Below:
      invert ? emit->jnb(label) : emit->jb(label);
      break;

    case Condition::BelowEqual:
      invert ? emit->jnbe(label) : emit->jbe(label);
      break;

    default:
      UnreachableCode();
      break;
  }
}
#endif

void CodeGenerator::EmitBranch(Condition condition, Reg lr_reg, Value&& branch_target)
{
    Panic("Not implemented");
#if 0
  // we have to always read the old PC.. when we can push/pop the register cache state this won't be needed
  Value old_npc;
  if (lr_reg != Reg::count)
    old_npc = m_register_cache.ReadGuestRegister(Reg::npc, false, true);

  // condition is inverted because we want the case for skipping it
  Xbyak::Label skip_branch;
  if (condition != Condition::Always)
    EmitConditionalJump(condition, true, m_emit, skip_branch);

  // save the old PC if we want to
  if (lr_reg != Reg::count)
  {
    // Can't cache because we have two branches. Load delay cancel is due to the immediate flush afterwards,
    // if we don't cancel it, at the end of the instruction the value we write can be overridden.
    EmitCancelInterpreterLoadDelayForReg(lr_reg);
    m_register_cache.WriteGuestRegister(lr_reg, std::move(old_npc));
    m_register_cache.FlushGuestRegister(lr_reg, true, true);
  }

  // we don't need to test the address of constant branches unless they're definitely misaligned, which would be
  // strange.
  if (!branch_target.IsConstant() || (branch_target.constant_value & 0x3) != 0)
  {
    if (branch_target.IsConstant())
    {
      Log_WarningPrintf("Misaligned constant target branch 0x%08X, this is strange",
                        Truncate32(branch_target.constant_value));
    }
    else
    {
      // check the alignment of the target
      m_emit->test(GetHostReg32(branch_target), 0x3);
      m_emit->jnz(GetCurrentFarCodePointer());
    }

    // exception exit for misaligned target
    SwitchToFarCode();
    EmitFunctionCall(nullptr, &Thunks::RaiseAddressException, m_register_cache.GetCPUPtr(), branch_target,
                     Value::FromConstantU8(0), Value::FromConstantU8(1));
    EmitExceptionExit();
    SwitchToNearCode();
  }

  // branch taken path - write new PC and flush it, since two branches
  m_register_cache.WriteGuestRegister(Reg::npc, std::move(branch_target));
  m_register_cache.FlushGuestRegister(Reg::npc, true, true);
  EmitStoreCPUStructField(offsetof(Core, m_current_instruction_was_branch_taken), Value::FromConstantU8(1));

  // converge point
  m_emit->L(skip_branch);
#endif
}

void CodeGenerator::EmitRaiseException(Exception excode, Condition condition /* = Condition::Always */)
{
    Panic("Not implemented");
#if 0
  if (condition == Condition::Always)
  {
    // no need to use far code if we're always raising the exception
    EmitFunctionCall(nullptr, &Thunks::RaiseException, m_register_cache.GetCPUPtr(),
                     Value::FromConstantU8(static_cast<u8>(excode)));
    m_register_cache.FlushAllGuestRegisters(true, true);
    m_register_cache.FlushLoadDelay(true);

    // PC should be synced at this point. If we leave the 4 on here for this instruction, we mess up npc.
    Assert(m_delayed_pc_add == 4);
    m_delayed_pc_add = 0;
    return;
  }

  const void* far_code_ptr = GetCurrentFarCodePointer();
  EmitConditionalJump(condition, false, m_emit, far_code_ptr);

  SwitchToFarCode();
  EmitFunctionCall(nullptr, &Thunks::RaiseException, m_register_cache.GetCPUPtr(),
                   Value::FromConstantU8(static_cast<u8>(excode)));
  EmitExceptionExit();
  SwitchToNearCode();
#endif
}

void ASMFunctions::Generate(JitCodeBuffer* code_buffer)
{
}

} // namespace CPU::Recompiler
