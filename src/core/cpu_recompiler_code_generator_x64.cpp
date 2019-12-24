#include "YBaseLib/Log.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
Log_SetChannel(CPU::Recompiler);

namespace CPU::Recompiler {

#if defined(ABI_WIN64)
constexpr HostReg RCPUPTR = Xbyak::Operand::RBP;
constexpr HostReg RRETURN = Xbyak::Operand::RAX;
constexpr HostReg RARG1 = Xbyak::Operand::RCX;
constexpr HostReg RARG2 = Xbyak::Operand::RDX;
constexpr HostReg RARG3 = Xbyak::Operand::R8;
constexpr HostReg RARG4 = Xbyak::Operand::R9;
constexpr u32 FUNCTION_CALL_SHADOW_SPACE = 32;
constexpr u64 FUNCTION_CALL_STACK_ALIGNMENT = 16;
#elif defined(ABI_SYSV)
constexpr HostReg RCPUPTR = Xbyak::Operand::RBP;
constexpr HostReg RRETURN = Xbyak::Operand::RAX;
constexpr HostReg RARG1 = Xbyak::Operand::RDI;
constexpr HostReg RARG2 = Xbyak::Operand::RSI;
constexpr HostReg RARG3 = Xbyak::Operand::RDX;
constexpr HostReg RARG4 = Xbyak::Operand::RCX;
constexpr u32 FUNCTION_CALL_SHADOW_SPACE = 0;
constexpr u64 FUNCTION_CALL_STACK_ALIGNMENT = 16;
#endif

static const Xbyak::Reg8 GetHostReg8(HostReg reg)
{
  return Xbyak::Reg8(reg, reg >= Xbyak::Operand::SPL);
}

static const Xbyak::Reg8 GetHostReg8(const Value& value)
{
  DebugAssert(value.size == RegSize_8 && value.IsInHostRegister());
  return Xbyak::Reg8(value.host_reg, value.host_reg >= Xbyak::Operand::SPL);
}

static const Xbyak::Reg16 GetHostReg16(HostReg reg)
{
  return Xbyak::Reg16(reg);
}

static const Xbyak::Reg16 GetHostReg16(const Value& value)
{
  DebugAssert(value.size == RegSize_16 && value.IsInHostRegister());
  return Xbyak::Reg16(value.host_reg);
}

static const Xbyak::Reg32 GetHostReg32(HostReg reg)
{
  return Xbyak::Reg32(reg);
}

static const Xbyak::Reg32 GetHostReg32(const Value& value)
{
  DebugAssert(value.size == RegSize_32 && value.IsInHostRegister());
  return Xbyak::Reg32(value.host_reg);
}

static const Xbyak::Reg64 GetHostReg64(HostReg reg)
{
  return Xbyak::Reg64(reg);
}

static const Xbyak::Reg64 GetHostReg64(const Value& value)
{
  DebugAssert(value.size == RegSize_64 && value.IsInHostRegister());
  return Xbyak::Reg64(value.host_reg);
}

static const Xbyak::Reg64 GetCPUPtrReg()
{
  return GetHostReg64(RCPUPTR);
}

CodeGenerator::CodeGenerator(Core* cpu, JitCodeBuffer* code_buffer, const ASMFunctions& asm_functions)
  : m_cpu(cpu), m_code_buffer(code_buffer), m_asm_functions(asm_functions), m_register_cache(*this),
    m_near_emitter(code_buffer->GetFreeCodeSpace(), code_buffer->GetFreeCodePointer()),
    m_far_emitter(code_buffer->GetFreeFarCodeSpace(), code_buffer->GetFreeFarCodePointer()), m_emit(&m_near_emitter)
{
  InitHostRegs();
}

CodeGenerator::~CodeGenerator() = default;

const char* CodeGenerator::GetHostRegName(HostReg reg, RegSize size /*= HostPointerSize*/)
{
  static constexpr std::array<const char*, HostReg_Count> reg8_names = {
    {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"}};
  static constexpr std::array<const char*, HostReg_Count> reg16_names = {
    {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"}};
  static constexpr std::array<const char*, HostReg_Count> reg32_names = {{"eax", "ecx", "edx", "ebx", "esp", "ebp",
                                                                          "esi", "edi", "r8d", "r9d", "r10d", "r11d",
                                                                          "r12d", "r13d", "r14d", "r15d"}};
  static constexpr std::array<const char*, HostReg_Count> reg64_names = {
    {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"}};
  if (reg >= static_cast<HostReg>(HostReg_Count))
    return "";

  switch (size)
  {
    case RegSize_8:
      return reg8_names[reg];
    case RegSize_16:
      return reg16_names[reg];
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
#if defined(ABI_WIN64)
  // TODO: function calls mess up the parameter registers if we use them.. fix it
  // allocate nonvolatile before volatile
  m_register_cache.SetHostRegAllocationOrder(
    {Xbyak::Operand::RBX, Xbyak::Operand::RBP, Xbyak::Operand::RDI, Xbyak::Operand::RSI, /*Xbyak::Operand::RSP, */
     Xbyak::Operand::R12, Xbyak::Operand::R13, Xbyak::Operand::R14, Xbyak::Operand::R15, /*Xbyak::Operand::RCX,
     Xbyak::Operand::RDX, Xbyak::Operand::R8, Xbyak::Operand::R9, */
     Xbyak::Operand::R10, Xbyak::Operand::R11,
     /*Xbyak::Operand::RAX*/});
  m_register_cache.SetCallerSavedHostRegs({Xbyak::Operand::RAX, Xbyak::Operand::RCX, Xbyak::Operand::RDX,
                                           Xbyak::Operand::R8, Xbyak::Operand::R9, Xbyak::Operand::R10,
                                           Xbyak::Operand::R11});
  m_register_cache.SetCalleeSavedHostRegs({Xbyak::Operand::RBX, Xbyak::Operand::RBP, Xbyak::Operand::RDI,
                                           Xbyak::Operand::RSI, Xbyak::Operand::RSP, Xbyak::Operand::R12,
                                           Xbyak::Operand::R13, Xbyak::Operand::R14, Xbyak::Operand::R15});
  m_register_cache.SetCPUPtrHostReg(RCPUPTR);
#elif defined(ABI_SYSV)
  m_register_cache.SetHostRegAllocationOrder(
    {Xbyak::Operand::RBX, /*Xbyak::Operand::RSP, */ Xbyak::Operand::RBP, Xbyak::Operand::R12, Xbyak::Operand::R13,
     Xbyak::Operand::R14, Xbyak::Operand::R15,
     /*Xbyak::Operand::RAX, */ /*Xbyak::Operand::RDI, */ /*Xbyak::Operand::RSI, */
     /*Xbyak::Operand::RDX, */ /*Xbyak::Operand::RCX, */ Xbyak::Operand::R8, Xbyak::Operand::R9, Xbyak::Operand::R10,
     Xbyak::Operand::R11});
  m_register_cache.SetCallerSavedHostRegs({Xbyak::Operand::RAX, Xbyak::Operand::RDI, Xbyak::Operand::RSI,
                                           Xbyak::Operand::RDX, Xbyak::Operand::RCX, Xbyak::Operand::R8,
                                           Xbyak::Operand::R9, Xbyak::Operand::R10, Xbyak::Operand::R11});
  m_register_cache.SetCalleeSavedHostRegs({Xbyak::Operand::RBX, Xbyak::Operand::RSP, Xbyak::Operand::RBP,
                                           Xbyak::Operand::R12, Xbyak::Operand::R13, Xbyak::Operand::R14,
                                           Xbyak::Operand::R15});
  m_register_cache.SetCPUPtrHostReg(RCPUPTR);
#endif
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
  return m_near_emitter.getCurr<void*>();
}

void* CodeGenerator::GetCurrentFarCodePointer() const
{
  return m_far_emitter.getCurr<void*>();
}

Value CodeGenerator::GetValueInHostRegister(const Value& value)
{
  if (value.IsInHostRegister())
    return Value(value.regcache, value.host_reg, value.size, ValueFlags::Valid | ValueFlags::InHostRegister);

  Value new_value = m_register_cache.AllocateScratch(value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

void CodeGenerator::EmitBeginBlock()
{
  // Store the CPU struct pointer.
  const bool cpu_reg_allocated = m_register_cache.AllocateHostReg(RCPUPTR);
  DebugAssert(cpu_reg_allocated);
  m_emit->mov(GetCPUPtrReg(), GetHostReg64(RARG1));
}

void CodeGenerator::EmitEndBlock()
{
  m_register_cache.FreeHostReg(RCPUPTR);
  m_register_cache.PopCalleeSavedRegisters(true);

  m_emit->ret();
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
  m_emit->ret();
}

void CodeGenerator::EmitExceptionExitOnBool(const Value& value)
{
  Assert(!value.IsConstant() && value.IsInHostRegister());

  m_emit->test(GetHostReg8(value), GetHostReg8(value));
  m_emit->jnz(GetCurrentFarCodePointer());

  m_register_cache.PushState();

  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();
}

void CodeGenerator::FinalizeBlock(CodeBlock::HostCodePointer* out_host_code, u32* out_host_code_size)
{
  m_near_emitter.ready();
  m_far_emitter.ready();

  const u32 near_size = static_cast<u32>(m_near_emitter.getSize());
  const u32 far_size = static_cast<u32>(m_far_emitter.getSize());
  *out_host_code = m_near_emitter.getCode<CodeBlock::HostCodePointer>();
  *out_host_code_size = near_size;
  m_code_buffer->CommitCode(near_size);
  m_code_buffer->CommitFarCode(far_size);

  m_near_emitter.reset();
  m_far_emitter.reset();
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

  Panic("Unknown sign-extend combination");
}

void CodeGenerator::EmitCopyValue(HostReg to_reg, const Value& value)
{
  // TODO: mov x, 0 -> xor x, x
  DebugAssert(value.IsConstant() || value.IsInHostRegister());

  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.HasConstantValue(0))
        m_emit->xor_(GetHostReg8(to_reg), GetHostReg8(to_reg));
      else if (value.IsConstant())
        m_emit->mov(GetHostReg8(to_reg), value.constant_value);
      else
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (value.HasConstantValue(0))
        m_emit->xor_(GetHostReg16(to_reg), GetHostReg16(to_reg));
      else if (value.IsConstant())
        m_emit->mov(GetHostReg16(to_reg), value.constant_value);
      else
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (value.HasConstantValue(0))
        m_emit->xor_(GetHostReg32(to_reg), GetHostReg32(to_reg));
      else if (value.IsConstant())
        m_emit->mov(GetHostReg32(to_reg), value.constant_value);
      else
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (value.HasConstantValue(0))
        m_emit->xor_(GetHostReg64(to_reg), GetHostReg64(to_reg));
      else if (value.IsConstant())
        m_emit->mov(GetHostReg64(to_reg), value.constant_value);
      else
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(value.host_reg));
    }
    break;
  }
}

void CodeGenerator::EmitAdd(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());

  switch (value.size)
  {
    case RegSize_8:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (value.IsConstant())
        m_emit->add(GetHostReg8(to_reg), SignExtend32(Truncate8(value.constant_value)));
      else
        m_emit->add(GetHostReg8(to_reg), GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (value.IsConstant())
        m_emit->add(GetHostReg16(to_reg), SignExtend32(Truncate16(value.constant_value)));
      else
        m_emit->add(GetHostReg16(to_reg), GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (value.IsConstant())
        m_emit->add(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->add(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

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
}

void CodeGenerator::EmitSub(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());

  switch (value.size)
  {
    case RegSize_8:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (value.IsConstant())
        m_emit->sub(GetHostReg8(to_reg), SignExtend32(Truncate8(value.constant_value)));
      else
        m_emit->sub(GetHostReg8(to_reg), GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (value.IsConstant())
        m_emit->sub(GetHostReg16(to_reg), SignExtend32(Truncate16(value.constant_value)));
      else
        m_emit->sub(GetHostReg16(to_reg), GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (value.IsConstant())
        m_emit->sub(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->sub(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

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
}

void CodeGenerator::EmitCmp(HostReg to_reg, const Value& value)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());

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
}

void CodeGenerator::EmitMul(HostReg to_reg_hi, HostReg to_reg_lo, const Value& lhs, const Value& rhs,
                            bool signed_multiply)
{
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
}

void CodeGenerator::EmitInc(HostReg to_reg, RegSize size)
{
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
}

void CodeGenerator::EmitDec(HostReg to_reg, RegSize size)
{
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
}

void CodeGenerator::EmitShl(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value)
{
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
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg8(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg8(to_reg), m_emit->cl);
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg16(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg16(to_reg), m_emit->cl);
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg32(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg32(to_reg), m_emit->cl);
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

      if (amount_value.IsConstant())
        m_emit->shl(GetHostReg64(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shl(GetHostReg64(to_reg), m_emit->cl);
    }
    break;
  }

  if (save_cl)
    m_emit->pop(m_emit->rcx);
}

void CodeGenerator::EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value)
{
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
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg8(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg8(to_reg), m_emit->cl);
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg16(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg16(to_reg), m_emit->cl);
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg32(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg32(to_reg), m_emit->cl);
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

      if (amount_value.IsConstant())
        m_emit->shr(GetHostReg64(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->shr(GetHostReg64(to_reg), m_emit->cl);
    }
    break;
  }

  if (save_cl)
    m_emit->pop(m_emit->rcx);
}

void CodeGenerator::EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value)
{
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
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg8(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg8(to_reg), m_emit->cl);
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg16(to_reg), Truncate8(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg16(to_reg), m_emit->cl);
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg32(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg32(to_reg), m_emit->cl);
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

      if (amount_value.IsConstant())
        m_emit->sar(GetHostReg64(to_reg), Truncate32(amount_value.constant_value));
      else
        m_emit->sar(GetHostReg64(to_reg), m_emit->cl);
    }
    break;
  }

  if (save_cl)
    m_emit->pop(m_emit->rcx);
}

void CodeGenerator::EmitAnd(HostReg to_reg, HostReg from_reg, const Value& value)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (value.IsConstant())
        m_emit->and_(GetHostReg8(to_reg), Truncate32(value.constant_value & UINT32_C(0xFF)));
      else
        m_emit->and_(GetHostReg8(to_reg), GetHostReg8(value));
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (value.IsConstant())
        m_emit->and_(GetHostReg16(to_reg), Truncate32(value.constant_value & UINT32_C(0xFFFF)));
      else
        m_emit->and_(GetHostReg16(to_reg), GetHostReg16(value));
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (value.IsConstant())
        m_emit->and_(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(value));
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

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
}

void CodeGenerator::EmitOr(HostReg to_reg, HostReg from_reg, const Value& value)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (value.IsConstant())
        m_emit->or_(GetHostReg8(to_reg), Truncate32(value.constant_value & UINT32_C(0xFF)));
      else
        m_emit->or_(GetHostReg8(to_reg), GetHostReg8(value));
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (value.IsConstant())
        m_emit->or_(GetHostReg16(to_reg), Truncate32(value.constant_value & UINT32_C(0xFFFF)));
      else
        m_emit->or_(GetHostReg16(to_reg), GetHostReg16(value));
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (value.IsConstant())
        m_emit->or_(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->or_(GetHostReg32(to_reg), GetHostReg32(value));
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

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
}

void CodeGenerator::EmitXor(HostReg to_reg, HostReg from_reg, const Value& value)
{
  DebugAssert(value.IsConstant() || value.IsInHostRegister());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg8(to_reg), GetHostReg8(from_reg));

      if (value.IsConstant())
        m_emit->xor_(GetHostReg8(to_reg), Truncate32(value.constant_value & UINT32_C(0xFF)));
      else
        m_emit->xor_(GetHostReg8(to_reg), GetHostReg8(value));
    }
    break;

    case RegSize_16:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg16(to_reg), GetHostReg16(from_reg));

      if (value.IsConstant())
        m_emit->xor_(GetHostReg16(to_reg), Truncate32(value.constant_value & UINT32_C(0xFFFF)));
      else
        m_emit->xor_(GetHostReg16(to_reg), GetHostReg16(value));
    }
    break;

    case RegSize_32:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg32(to_reg), GetHostReg32(from_reg));

      if (value.IsConstant())
        m_emit->xor_(GetHostReg32(to_reg), Truncate32(value.constant_value));
      else
        m_emit->xor_(GetHostReg32(to_reg), GetHostReg32(value));
    }
    break;

    case RegSize_64:
    {
      if (to_reg != from_reg)
        m_emit->mov(GetHostReg64(to_reg), GetHostReg64(from_reg));

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
}

void CodeGenerator::EmitTest(HostReg to_reg, const Value& value)
{
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
}

void CodeGenerator::EmitNot(HostReg to_reg, RegSize size)
{
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
}

void CodeGenerator::EmitSetConditionResult(HostReg to_reg, RegSize to_size, Condition condition)
{
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
}

u32 CodeGenerator::PrepareStackForCall()
{
  // we assume that the stack is unaligned at this point
  const u32 num_callee_saved = m_register_cache.GetActiveCalleeSavedRegisterCount();
  const u32 num_caller_saved = m_register_cache.PushCallerSavedRegisters();
  const u32 current_offset = 8 + (num_callee_saved + num_caller_saved) * 8;
  const u32 aligned_offset = Common::AlignUp(current_offset + FUNCTION_CALL_SHADOW_SPACE, 16);
  const u32 adjust_size = aligned_offset - current_offset;
  if (adjust_size > 0)
    m_emit->sub(m_emit->rsp, adjust_size);

  return adjust_size;
}

void CodeGenerator::RestoreStackAfterCall(u32 adjust_size)
{
  if (adjust_size > 0)
    m_emit->add(m_emit->rsp, adjust_size);

  m_register_cache.PopCallerSavedRegisters();
}

void CodeGenerator::EmitFunctionCallPtr(Value* return_value, const void* ptr)
{
  if (return_value)
    return_value->Discard();

  // shadow space allocate
  const u32 adjust_size = PrepareStackForCall();

  // actually call the function
  m_emit->mov(GetHostReg64(RRETURN), reinterpret_cast<size_t>(ptr));
  m_emit->call(GetHostReg64(RRETURN));

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
  if (Xbyak::inner::IsInInt32(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr())))
  {
    m_emit->call(ptr);
  }
  else
  {
    m_emit->mov(GetHostReg64(RRETURN), reinterpret_cast<size_t>(ptr));
    m_emit->call(GetHostReg64(RRETURN));
  }

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
  if (Xbyak::inner::IsInInt32(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr())))
  {
    m_emit->call(ptr);
  }
  else
  {
    m_emit->mov(GetHostReg64(RRETURN), reinterpret_cast<size_t>(ptr));
    m_emit->call(GetHostReg64(RRETURN));
  }

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
  if (Xbyak::inner::IsInInt32(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr())))
  {
    m_emit->call(ptr);
  }
  else
  {
    m_emit->mov(GetHostReg64(RRETURN), reinterpret_cast<size_t>(ptr));
    m_emit->call(GetHostReg64(RRETURN));
  }

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
  if (Xbyak::inner::IsInInt32(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr())))
  {
    m_emit->call(ptr);
  }
  else
  {
    m_emit->mov(GetHostReg64(RRETURN), reinterpret_cast<size_t>(ptr));
    m_emit->call(GetHostReg64(RRETURN));
  }

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
  m_emit->push(GetHostReg64(reg));
}

void CodeGenerator::EmitPopHostReg(HostReg reg, u32 position)
{
  m_emit->pop(GetHostReg64(reg));
}

void CodeGenerator::EmitLoadCPUStructField(HostReg host_reg, RegSize guest_size, u32 offset)
{
  switch (guest_size)
  {
    case RegSize_8:
      m_emit->mov(GetHostReg8(host_reg), m_emit->byte[GetCPUPtrReg() + offset]);
      break;

    case RegSize_16:
      m_emit->mov(GetHostReg16(host_reg), m_emit->word[GetCPUPtrReg() + offset]);
      break;

    case RegSize_32:
      m_emit->mov(GetHostReg32(host_reg), m_emit->dword[GetCPUPtrReg() + offset]);
      break;

    case RegSize_64:
      m_emit->mov(GetHostReg64(host_reg), m_emit->qword[GetCPUPtrReg() + offset]);
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
  DebugAssert(value.IsInHostRegister() || value.IsConstant());
  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant())
        m_emit->mov(m_emit->byte[GetCPUPtrReg() + offset], value.constant_value);
      else
        m_emit->mov(m_emit->byte[GetCPUPtrReg() + offset], GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant())
        m_emit->mov(m_emit->word[GetCPUPtrReg() + offset], value.constant_value);
      else
        m_emit->mov(m_emit->word[GetCPUPtrReg() + offset], GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant())
        m_emit->mov(m_emit->dword[GetCPUPtrReg() + offset], value.constant_value);
      else
        m_emit->mov(m_emit->dword[GetCPUPtrReg() + offset], GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant())
      {
        // we need a temporary to load the value if it doesn't fit in 32-bits
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          EmitCopyValue(temp.host_reg, value);
          m_emit->mov(m_emit->qword[GetCPUPtrReg() + offset], GetHostReg64(temp.host_reg));
        }
        else
        {
          m_emit->mov(m_emit->qword[GetCPUPtrReg() + offset], value.constant_value);
        }
      }
      else
      {
        m_emit->mov(m_emit->qword[GetCPUPtrReg() + offset], GetHostReg64(value.host_reg));
      }
    }
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
  switch (value.size)
  {
    case RegSize_8:
    {
      if (value.IsConstant() && value.constant_value == 1)
        m_emit->inc(m_emit->byte[GetCPUPtrReg() + offset]);
      else if (value.IsConstant())
        m_emit->add(m_emit->byte[GetCPUPtrReg() + offset], Truncate32(value.constant_value));
      else
        m_emit->add(m_emit->byte[GetCPUPtrReg() + offset], GetHostReg8(value.host_reg));
    }
    break;

    case RegSize_16:
    {
      if (value.IsConstant() && value.constant_value == 1)
        m_emit->inc(m_emit->word[GetCPUPtrReg() + offset]);
      else if (value.IsConstant())
        m_emit->add(m_emit->word[GetCPUPtrReg() + offset], Truncate32(value.constant_value));
      else
        m_emit->add(m_emit->word[GetCPUPtrReg() + offset], GetHostReg16(value.host_reg));
    }
    break;

    case RegSize_32:
    {
      if (value.IsConstant() && value.constant_value == 1)
        m_emit->inc(m_emit->dword[GetCPUPtrReg() + offset]);
      else if (value.IsConstant())
        m_emit->add(m_emit->dword[GetCPUPtrReg() + offset], Truncate32(value.constant_value));
      else
        m_emit->add(m_emit->dword[GetCPUPtrReg() + offset], GetHostReg32(value.host_reg));
    }
    break;

    case RegSize_64:
    {
      if (value.IsConstant() && value.constant_value == 1)
      {
        m_emit->inc(m_emit->qword[GetCPUPtrReg() + offset]);
      }
      else if (value.IsConstant())
      {
        // we need a temporary to load the value if it doesn't fit in 32-bits
        if (!Xbyak::inner::IsInInt32(value.constant_value))
        {
          Value temp = m_register_cache.AllocateScratch(RegSize_64);
          EmitCopyValue(temp.host_reg, value);
          m_emit->add(m_emit->qword[GetCPUPtrReg() + offset], GetHostReg64(temp.host_reg));
        }
        else
        {
          m_emit->add(m_emit->qword[GetCPUPtrReg() + offset], Truncate32(value.constant_value));
        }
      }
      else
      {
        m_emit->add(m_emit->qword[GetCPUPtrReg() + offset], GetHostReg64(value.host_reg));
      }
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

  m_emit->test(GetHostReg64(result.host_reg), GetHostReg64(result.host_reg));
  m_emit->js(GetCurrentFarCodePointer());

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

  m_register_cache.PushState();

  m_emit->test(GetHostReg8(result), GetHostReg8(result));
  m_emit->jz(GetCurrentFarCodePointer());

  // store exception path
  SwitchToFarCode();
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();
}

void CodeGenerator::EmitFlushInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_8);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  auto load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(Core, m_load_delay_reg)];
  auto load_delay_value = m_emit->dword[GetCPUPtrReg() + offsetof(Core, m_load_delay_value)];
  auto reg_ptr = m_emit->dword[GetCPUPtrReg() + offsetof(Core, m_regs.r[0]) + GetHostReg64(reg.host_reg) * 4];

  Xbyak::Label skip_flush;

  // reg = load_delay_reg
  m_emit->movzx(GetHostReg32(reg.host_reg), load_delay_reg);

  // if load_delay_reg == Reg::count goto skip_flush
  m_emit->cmp(GetHostReg32(reg.host_reg), static_cast<u8>(Reg::count));
  m_emit->je(skip_flush);

  // r[reg] = load_delay_value
  m_emit->mov(GetHostReg32(value), load_delay_value);
  m_emit->mov(reg_ptr, GetHostReg32(value));

  // load_delay_reg = Reg::count
  m_emit->mov(load_delay_reg, static_cast<u8>(Reg::count));

  m_emit->L(skip_flush);
}

void CodeGenerator::EmitMoveNextInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_8);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  auto load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(Core, m_load_delay_reg)];
  auto load_delay_value = m_emit->dword[GetCPUPtrReg() + offsetof(Core, m_load_delay_value)];
  auto next_load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(Core, m_next_load_delay_reg)];
  auto next_load_delay_value = m_emit->dword[GetCPUPtrReg() + offsetof(Core, m_next_load_delay_value)];

  m_emit->mov(GetHostReg32(value), next_load_delay_value);
  m_emit->mov(GetHostReg8(reg), next_load_delay_reg);
  m_emit->mov(load_delay_value, GetHostReg32(value));
  m_emit->mov(load_delay_reg, GetHostReg8(reg));
  m_emit->mov(next_load_delay_reg, static_cast<u8>(Reg::count));
}

void CodeGenerator::EmitCancelInterpreterLoadDelayForReg(Reg reg)
{
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
}

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

  Xbyak::Label skip_branch;
  if (condition != Condition::Always)
  {
    // condition is inverted because we want the case for skipping it
    EmitConditionalJump(condition, true, m_emit, skip_branch);
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

    m_register_cache.PushState();

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
  m_emit->L(skip_branch);

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
      m_emit->bt(GetHostReg8(reg), bit);
      m_emit->jnc(*label);
      break;

    case RegSize_16:
      m_emit->bt(GetHostReg16(reg), bit);
      m_emit->jnc(*label);
      break;

    case RegSize_32:
      m_emit->bt(GetHostReg32(reg), bit);
      m_emit->jnc(*label);
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitBindLabel(LabelType* label)
{
  m_emit->L(*label);
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

  m_register_cache.PushState();

  const void* far_code_ptr = GetCurrentFarCodePointer();
  EmitConditionalJump(condition, false, m_emit, far_code_ptr);

  SwitchToFarCode();
  EmitFunctionCall(nullptr, &Thunks::RaiseException, m_register_cache.GetCPUPtr(),
                   Value::FromConstantU8(static_cast<u8>(excode)));
  EmitExceptionExit();
  SwitchToNearCode();

  m_register_cache.PopState();
}

void ASMFunctions::Generate(JitCodeBuffer* code_buffer) {}

} // namespace CPU::Recompiler
