#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#include "settings.h"
#include "timing_event.h"
Log_SetChannel(Recompiler::CodeGenerator);

namespace CPU::Recompiler {

#if defined(ABI_WIN64)
constexpr HostReg RCPUPTR = Xbyak::Operand::RBP;
constexpr HostReg RMEMBASEPTR = Xbyak::Operand::RBX;
constexpr HostReg RRETURN = Xbyak::Operand::RAX;
constexpr HostReg RARG1 = Xbyak::Operand::RCX;
constexpr HostReg RARG2 = Xbyak::Operand::RDX;
constexpr HostReg RARG3 = Xbyak::Operand::R8;
constexpr HostReg RARG4 = Xbyak::Operand::R9;
constexpr u32 FUNCTION_CALL_SHADOW_SPACE = 32;
#elif defined(ABI_SYSV)
constexpr HostReg RCPUPTR = Xbyak::Operand::RBP;
constexpr HostReg RMEMBASEPTR = Xbyak::Operand::RBX;
constexpr HostReg RRETURN = Xbyak::Operand::RAX;
constexpr HostReg RARG1 = Xbyak::Operand::RDI;
constexpr HostReg RARG2 = Xbyak::Operand::RSI;
constexpr HostReg RARG3 = Xbyak::Operand::RDX;
constexpr HostReg RARG4 = Xbyak::Operand::RCX;
constexpr u32 FUNCTION_CALL_SHADOW_SPACE = 0;
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

static const Xbyak::Reg64 GetFastmemBasePtrReg()
{
  return GetHostReg64(RMEMBASEPTR);
}

CodeGenerator::CodeGenerator(JitCodeBuffer* code_buffer)
  : m_code_buffer(code_buffer), m_register_cache(*this),
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
#endif

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
  return m_near_emitter.getCurr<void*>();
}

void* CodeGenerator::GetCurrentFarCodePointer() const
{
  return m_far_emitter.getCurr<void*>();
}

Value CodeGenerator::GetValueInHostRegister(const Value& value, bool allow_zero_register /* = true */)
{
  if (value.IsInHostRegister())
    return Value(value.regcache, value.host_reg, value.size, ValueFlags::Valid | ValueFlags::InHostRegister);

  Value new_value = m_register_cache.AllocateScratch(value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

Value CodeGenerator::GetValueInHostOrScratchRegister(const Value& value, bool allow_zero_register /* = true */)
{
  if (value.IsInHostRegister())
    return Value(value.regcache, value.host_reg, value.size, ValueFlags::Valid | ValueFlags::InHostRegister);

  Value new_value = m_register_cache.AllocateScratch(value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

void CodeGenerator::EmitBeginBlock(bool allocate_registers /* = true */)
{
  if (allocate_registers)
  {
    m_register_cache.AssumeCalleeSavedRegistersAreSaved();

    // Store the CPU struct pointer.
    const bool cpu_reg_allocated = m_register_cache.AllocateHostReg(RCPUPTR);
    DebugAssert(cpu_reg_allocated);
    UNREFERENCED_VARIABLE(cpu_reg_allocated);
    // m_emit->mov(GetCPUPtrReg(), reinterpret_cast<size_t>(&g_state));

    // If there's loadstore instructions, preload the fastmem base.
    if (m_block->contains_loadstore_instructions)
    {
      const bool fastmem_reg_allocated = m_register_cache.AllocateHostReg(RMEMBASEPTR);
      DebugAssert(fastmem_reg_allocated);
      UNREFERENCED_VARIABLE(fastmem_reg_allocated);
      m_emit->mov(GetFastmemBasePtrReg(), m_emit->qword[GetCPUPtrReg() + offsetof(CPU::State, fastmem_base)]);
    }
  }
}

void CodeGenerator::EmitEndBlock(bool free_registers /* = true */, bool emit_return /* = true */)
{
  if (free_registers)
  {
    m_register_cache.FreeHostReg(RCPUPTR);
    if (m_block->contains_loadstore_instructions)
      m_register_cache.FreeHostReg(RMEMBASEPTR);

    m_register_cache.PopCalleeSavedRegisters(true);
  }

  if (emit_return)
    m_emit->ret();
}

void CodeGenerator::EmitExceptionExit()
{
  AddPendingCycles(false);

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

void CodeGenerator::EmitDiv(HostReg to_reg_quotient, HostReg to_reg_remainder, HostReg num, HostReg denom, RegSize size,
                            bool signed_divide)
{
  const bool save_eax = (to_reg_quotient != Xbyak::Operand::RAX && to_reg_remainder != Xbyak::Operand::RAX);
  const bool save_edx = (to_reg_quotient != Xbyak::Operand::RDX && to_reg_remainder != Xbyak::Operand::RDX);

  if (save_eax)
    m_emit->push(m_emit->rax);

  if (save_edx)
    m_emit->push(m_emit->rdx);

  // unsupported cases.. for now
  Assert(num != Xbyak::Operand::RDX && num != Xbyak::Operand::RAX);
  if (num != Xbyak::Operand::RAX)
    EmitCopyValue(Xbyak::Operand::RAX, Value::FromHostReg(&m_register_cache, num, size));

  if (size == RegSize_8)
  {
    if (signed_divide)
    {
      m_emit->cbw();
      m_emit->idiv(GetHostReg8(denom));
    }
    else
    {
      m_emit->xor_(m_emit->dx, m_emit->dx);
      m_emit->div(GetHostReg8(denom));
    }
  }
  else if (size == RegSize_16)
  {
    if (signed_divide)
    {
      m_emit->cwd();
      m_emit->idiv(GetHostReg16(denom));
    }
    else
    {
      m_emit->xor_(m_emit->edx, m_emit->edx);
      m_emit->div(GetHostReg16(denom));
    }
  }
  else if (size == RegSize_32)
  {
    if (signed_divide)
    {
      m_emit->cdq();
      m_emit->idiv(GetHostReg32(denom));
    }
    else
    {
      m_emit->xor_(m_emit->rdx, m_emit->edx);
      m_emit->div(GetHostReg32(denom));
    }
  }
  else
  {
    if (signed_divide)
      m_emit->idiv(GetHostReg64(denom));
    else
      m_emit->div(GetHostReg64(denom));
  }

  if (to_reg_quotient == Xbyak::Operand::RAX && to_reg_remainder == Xbyak::Operand::RDX)
  {
    // ideal case: registers are the ones we want: don't have to do anything
  }
  else if (to_reg_quotient == Xbyak::Operand::RDX && to_reg_remainder == Xbyak::Operand::RAX)
  {
    // what we want, but swapped, so exchange them
    m_emit->xchg(m_emit->rax, m_emit->rdx);
  }
  else if (to_reg_quotient != Xbyak::Operand::RAX && to_reg_quotient != Xbyak::Operand::RDX &&
           to_reg_remainder != Xbyak::Operand::RAX && to_reg_remainder != Xbyak::Operand::RDX)
  {
    // store to the registers we want.. this could be optimized better
    if (static_cast<u32>(to_reg_quotient) != HostReg_Count)
      m_emit->mov(GetHostReg64(to_reg_quotient), m_emit->rax);
    if (static_cast<u32>(to_reg_remainder) != HostReg_Count)
      m_emit->mov(GetHostReg64(to_reg_remainder), m_emit->rdx);
  }
  else
  {
    // store to the registers we want.. this could be optimized better
    if (static_cast<u32>(to_reg_quotient) != HostReg_Count)
    {
      m_emit->push(m_emit->rax);
      m_emit->pop(GetHostReg64(to_reg_quotient));
    }
    if (static_cast<u32>(to_reg_remainder) != HostReg_Count)
    {
      m_emit->push(m_emit->rdx);
      m_emit->pop(GetHostReg64(to_reg_remainder));
    }
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

void CodeGenerator::EmitShl(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
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

void CodeGenerator::EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
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

void CodeGenerator::EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked /* = true */)
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

void CodeGenerator::EmitCall(const void* ptr)
{
  if (Xbyak::inner::IsInInt32(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr())))
  {
    m_emit->call(ptr);
  }
  else
  {
    m_emit->mov(GetHostReg64(RRETURN), reinterpret_cast<size_t>(ptr));
    m_emit->call(GetHostReg64(RRETURN));
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
  m_emit->push(GetHostReg64(reg));
}

void CodeGenerator::EmitPushHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  m_emit->push(GetHostReg64(reg));
  m_emit->push(GetHostReg64(reg2));
}

void CodeGenerator::EmitPopHostReg(HostReg reg, u32 position)
{
  m_emit->pop(GetHostReg64(reg));
}

void CodeGenerator::EmitPopHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  m_emit->pop(GetHostReg64(reg2));
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

void CodeGenerator::EmitLoadGuestRAMFastmem(const Value& address, RegSize size, Value& result)
{
  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    // can't store displacements > 0x80000000 in-line
    const Value* actual_address = &address;
    if (address.IsConstant() && address.constant_value >= 0x80000000)
    {
      actual_address = &result;
      m_emit->mov(GetHostReg32(result.host_reg), address.constant_value);
    }

    // TODO: movsx/zx inline here
    switch (size)
    {
      case RegSize_8:
      {
        if (actual_address->IsConstant())
        {
          m_emit->mov(GetHostReg8(result.host_reg),
                      m_emit->byte[GetFastmemBasePtrReg() + actual_address->constant_value]);
        }
        else
        {
          m_emit->mov(GetHostReg8(result.host_reg),
                      m_emit->byte[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)]);
        }
      }
      break;

      case RegSize_16:
      {
        if (actual_address->IsConstant())
        {
          m_emit->mov(GetHostReg16(result.host_reg),
                      m_emit->word[GetFastmemBasePtrReg() + actual_address->constant_value]);
        }
        else
        {
          m_emit->mov(GetHostReg16(result.host_reg),
                      m_emit->word[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)]);
        }
      }
      break;

      case RegSize_32:
      {
        if (actual_address->IsConstant())
        {
          m_emit->mov(GetHostReg32(result.host_reg),
                      m_emit->dword[GetFastmemBasePtrReg() + actual_address->constant_value]);
        }
        else
        {
          m_emit->mov(GetHostReg32(result.host_reg),
                      m_emit->dword[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)]);
        }
      }
      break;
    }
  }
  else
  {
    // TODO: We could mask the LSBs here for unaligned protection.
    EmitCopyValue(RARG1, address);
    m_emit->mov(GetHostReg32(RARG2), GetHostReg32(RARG1));
    m_emit->shr(GetHostReg32(RARG1), 12);
    m_emit->and_(GetHostReg32(RARG2), HOST_PAGE_OFFSET_MASK);
    m_emit->mov(GetHostReg64(RARG1), m_emit->qword[GetFastmemBasePtrReg() + GetHostReg64(RARG1) * 8]);

    switch (size)
    {
      case RegSize_8:
        m_emit->mov(GetHostReg8(result.host_reg), m_emit->byte[GetHostReg64(RARG1) + GetHostReg64(RARG2)]);
        break;

      case RegSize_16:
        m_emit->mov(GetHostReg16(result.host_reg), m_emit->word[GetHostReg64(RARG1) + GetHostReg64(RARG2)]);
        break;

      case RegSize_32:
        m_emit->mov(GetHostReg32(result.host_reg), m_emit->dword[GetHostReg64(RARG1) + GetHostReg64(RARG2)]);
        break;
    }
  }
}

void CodeGenerator::EmitLoadGuestMemoryFastmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                               Value& result)
{
  // fastmem
  LoadStoreBackpatchInfo bpi;
  bpi.host_pc = GetCurrentNearCodePointer();
  bpi.address_host_reg = HostReg_Invalid;
  bpi.value_host_reg = result.host_reg;
  bpi.guest_pc = m_current_instruction->pc;

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    // can't store displacements > 0x80000000 in-line
    const Value* actual_address = &address;
    if (address.IsConstant() && address.constant_value >= 0x80000000)
    {
      actual_address = &result;
      m_emit->mov(GetHostReg32(result.host_reg), address.constant_value);
      bpi.host_pc = GetCurrentNearCodePointer();
    }

    m_register_cache.InhibitAllocation();

    switch (size)
    {
      case RegSize_8:
      {
        if (actual_address->IsConstant())
        {
          m_emit->mov(GetHostReg8(result.host_reg),
                      m_emit->byte[GetFastmemBasePtrReg() + actual_address->constant_value]);
        }
        else
        {
          m_emit->mov(GetHostReg8(result.host_reg),
                      m_emit->byte[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)]);
        }
      }
      break;

      case RegSize_16:
      {
        if (actual_address->IsConstant())
        {
          m_emit->mov(GetHostReg16(result.host_reg),
                      m_emit->word[GetFastmemBasePtrReg() + actual_address->constant_value]);
        }
        else
        {
          m_emit->mov(GetHostReg16(result.host_reg),
                      m_emit->word[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)]);
        }
      }
      break;

      case RegSize_32:
      {
        if (actual_address->IsConstant())
        {
          m_emit->mov(GetHostReg32(result.host_reg),
                      m_emit->dword[GetFastmemBasePtrReg() + actual_address->constant_value]);
        }
        else
        {
          m_emit->mov(GetHostReg32(result.host_reg),
                      m_emit->dword[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)]);
        }
      }
      break;
    }
  }
  else
  {
    m_register_cache.InhibitAllocation();

    // TODO: We could mask the LSBs here for unaligned protection.
    EmitCopyValue(RARG1, address);
    m_emit->mov(GetHostReg32(RARG2), GetHostReg32(RARG1));
    m_emit->shr(GetHostReg32(RARG1), 12);
    m_emit->and_(GetHostReg32(RARG2), HOST_PAGE_OFFSET_MASK);
    m_emit->mov(GetHostReg64(RARG1), m_emit->qword[GetFastmemBasePtrReg() + GetHostReg64(RARG1) * 8]);
    bpi.host_pc = GetCurrentNearCodePointer();

    switch (size)
    {
      case RegSize_8:
        m_emit->mov(GetHostReg8(result.host_reg), m_emit->byte[GetHostReg64(RARG1) + GetHostReg64(RARG2)]);
        break;

      case RegSize_16:
        m_emit->mov(GetHostReg16(result.host_reg), m_emit->word[GetHostReg64(RARG1) + GetHostReg64(RARG2)]);
        break;

      case RegSize_32:
        m_emit->mov(GetHostReg32(result.host_reg), m_emit->dword[GetHostReg64(RARG1) + GetHostReg64(RARG2)]);
        break;
    }
  }

  // insert nops, we need at least 5 bytes for a relative jump
  const u32 fastmem_size =
    static_cast<u32>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc));
  const u32 nops = (fastmem_size < 5 ? 5 - fastmem_size : 0);
  for (u32 i = 0; i < nops; i++)
    m_emit->nop();

  bpi.host_code_size = static_cast<u32>(
    static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc)));

  // generate slowmem fallback
  m_far_emitter.align(16);
  bpi.host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();

  // we add the ticks *after* the add here, since we counted incorrectly, then correct for it below
  DebugAssert(m_delayed_cycles_add > 0);
  EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));
  m_delayed_cycles_add += Bus::RAM_READ_TICKS;

  EmitLoadGuestMemorySlowmem(cbi, address, size, result, true);

  EmitAddCPUStructField(offsetof(State, pending_ticks),
                        Value::FromConstantU32(static_cast<u32>(-m_delayed_cycles_add)));

  // return to the block code
  m_emit->jmp(GetCurrentNearCodePointer());

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

    m_emit->test(GetHostReg64(result.host_reg), GetHostReg64(result.host_reg));
    m_emit->js(GetCurrentFarCodePointer());

    m_register_cache.PushState();

    // load exception path
    if (!in_far_code)
      SwitchToFarCode();

    // cause_bits = (-result << 2) | BD | cop_n
    m_emit->neg(GetHostReg32(result.host_reg));
    m_emit->shl(GetHostReg32(result.host_reg), 2);
    m_emit->or_(GetHostReg32(result.host_reg),
                Cop0Registers::CAUSE::MakeValueForException(static_cast<Exception>(0), cbi.is_branch_delay_slot, false,
                                                            cbi.instruction.cop.cop_n));
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

void CodeGenerator::EmitStoreGuestMemoryFastmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                                const Value& value)
{
  // fastmem
  LoadStoreBackpatchInfo bpi;
  bpi.host_pc = GetCurrentNearCodePointer();
  bpi.address_host_reg = HostReg_Invalid;
  bpi.value_host_reg = value.host_reg;
  bpi.guest_pc = m_current_instruction->pc;

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    // can't store displacements > 0x80000000 in-line
    const Value* actual_address = &address;
    Value temp_address;
    if (address.IsConstant() && address.constant_value >= 0x80000000)
    {
      temp_address.SetHostReg(&m_register_cache, RRETURN, RegSize_32);
      actual_address = &temp_address;
      m_emit->mov(GetHostReg32(temp_address), address.constant_value);
      bpi.host_pc = GetCurrentNearCodePointer();
    }

    m_register_cache.InhibitAllocation();

    switch (size)
    {
      case RegSize_8:
      {
        if (actual_address->IsConstant())
        {
          if (value.IsConstant())
          {
            m_emit->mov(m_emit->byte[GetFastmemBasePtrReg() + actual_address->constant_value],
                        value.constant_value & 0xFFu);
          }
          else
          {
            m_emit->mov(m_emit->byte[GetFastmemBasePtrReg() + actual_address->constant_value],
                        GetHostReg8(value.host_reg));
          }
        }
        else
        {
          if (value.IsConstant())
          {
            m_emit->mov(m_emit->byte[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)],
                        value.constant_value & 0xFFu);
          }
          else
          {
            m_emit->mov(m_emit->byte[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)],
                        GetHostReg8(value.host_reg));
          }
        }
      }
      break;

      case RegSize_16:
      {
        if (actual_address->IsConstant())
        {
          if (value.IsConstant())
          {
            m_emit->mov(m_emit->word[GetFastmemBasePtrReg() + actual_address->constant_value],
                        value.constant_value & 0xFFFFu);
          }
          else
          {
            m_emit->mov(m_emit->word[GetFastmemBasePtrReg() + actual_address->constant_value],
                        GetHostReg16(value.host_reg));
          }
        }
        else
        {
          if (value.IsConstant())
          {
            m_emit->mov(m_emit->word[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)],
                        value.constant_value & 0xFFFFu);
          }
          else
          {
            m_emit->mov(m_emit->word[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)],
                        GetHostReg16(value.host_reg));
          }
        }
      }
      break;

      case RegSize_32:
      {
        if (actual_address->IsConstant())
        {
          if (value.IsConstant())
          {
            m_emit->mov(m_emit->dword[GetFastmemBasePtrReg() + actual_address->constant_value], value.constant_value);
          }
          else
          {
            m_emit->mov(m_emit->dword[GetFastmemBasePtrReg() + actual_address->constant_value],
                        GetHostReg32(value.host_reg));
          }
        }
        else
        {
          if (value.IsConstant())
          {
            m_emit->mov(m_emit->dword[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)],
                        value.constant_value);
          }
          else
          {
            m_emit->mov(m_emit->dword[GetFastmemBasePtrReg() + GetHostReg64(actual_address->host_reg)],
                        GetHostReg32(value.host_reg));
          }
        }
      }
      break;
    }
  }
  else
  {
    m_register_cache.InhibitAllocation();

    // TODO: We could mask the LSBs here for unaligned protection.
    EmitCopyValue(RARG1, address);
    m_emit->mov(GetHostReg32(RARG2), GetHostReg32(RARG1));
    m_emit->shr(GetHostReg32(RARG1), 12);
    m_emit->and_(GetHostReg32(RARG2), HOST_PAGE_OFFSET_MASK);
    m_emit->mov(GetHostReg64(RARG1),
                m_emit->qword[GetFastmemBasePtrReg() + GetHostReg64(RARG1) * 8 + (Bus::FASTMEM_LUT_NUM_PAGES * 8)]);
    bpi.host_pc = GetCurrentNearCodePointer();

    switch (size)
    {
      case RegSize_8:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->byte[GetHostReg64(RARG1) + GetHostReg64(RARG2)], value.constant_value & 0xFFu);
        else
          m_emit->mov(m_emit->byte[GetHostReg64(RARG1) + GetHostReg64(RARG2)], GetHostReg8(value.host_reg));
      }
      break;

      case RegSize_16:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->word[GetHostReg64(RARG1) + GetHostReg64(RARG2)], value.constant_value & 0xFFFFu);
        else
          m_emit->mov(m_emit->word[GetHostReg64(RARG1) + GetHostReg64(RARG2)], GetHostReg16(value.host_reg));
      }
      break;

      case RegSize_32:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->dword[GetHostReg64(RARG1) + GetHostReg64(RARG2)], value.constant_value);
        else
          m_emit->mov(m_emit->dword[GetHostReg64(RARG1) + GetHostReg64(RARG2)], GetHostReg32(value.host_reg));
      }
      break;
    }
  }

  // insert nops, we need at least 5 bytes for a relative jump
  const u32 fastmem_size =
    static_cast<u32>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc));
  const u32 nops = (fastmem_size < 5 ? 5 - fastmem_size : 0);
  for (u32 i = 0; i < nops; i++)
    m_emit->nop();

  bpi.host_code_size = static_cast<u32>(
    static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc)));

  // generate slowmem fallback
  m_far_emitter.align();
  bpi.host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();

  DebugAssert(m_delayed_cycles_add > 0);
  EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));

  EmitStoreGuestMemorySlowmem(cbi, address, size, value, true);

  EmitAddCPUStructField(offsetof(State, pending_ticks),
                        Value::FromConstantU32(static_cast<u32>(-m_delayed_cycles_add)));

  // return to the block code
  m_emit->jmp(GetCurrentNearCodePointer());

  SwitchToNearCode();
  m_register_cache.UninhibitAllocation();

  m_block->loadstore_backpatch_info.push_back(bpi);
}

void CodeGenerator::EmitStoreGuestMemorySlowmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                                const Value& value, bool in_far_code)
{
  if (g_settings.cpu_recompiler_memory_exceptions)
  {
    Assert(!in_far_code);

    Value result = m_register_cache.AllocateScratch(RegSize_32);
    switch (size)
    {
      case RegSize_8:
        EmitFunctionCall(&result, &Thunks::WriteMemoryByte, address, value);
        break;

      case RegSize_16:
        EmitFunctionCall(&result, &Thunks::WriteMemoryHalfWord, address, value);
        break;

      case RegSize_32:
        EmitFunctionCall(&result, &Thunks::WriteMemoryWord, address, value);
        break;

      default:
        UnreachableCode();
        break;
    }

    m_register_cache.PushState();

    m_emit->test(GetHostReg32(result), GetHostReg32(result));
    m_emit->jnz(GetCurrentFarCodePointer());

    // store exception path
    if (!in_far_code)
      SwitchToFarCode();

    // cause_bits = (result << 2) | BD | cop_n
    m_emit->shl(GetHostReg32(result), 2);
    m_emit->or_(GetHostReg32(result),
                Cop0Registers::CAUSE::MakeValueForException(static_cast<Exception>(0), cbi.is_branch_delay_slot, false,
                                                            cbi.instruction.cop.cop_n));
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
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryByte, address, value);
        break;

      case RegSize_16:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryHalfWord, address, value);
        break;

      case RegSize_32:
        EmitFunctionCall(nullptr, &Thunks::UncheckedWriteMemoryWord, address, value);
        break;

      default:
        UnreachableCode();
        break;
    }
  }
}

void CodeGenerator::EmitUpdateFastmemBase()
{
  m_emit->mov(GetFastmemBasePtrReg(), m_emit->qword[GetCPUPtrReg() + offsetof(CPU::State, fastmem_base)]);
}

bool CodeGenerator::BackpatchLoadStore(const LoadStoreBackpatchInfo& lbi)
{
  Log_ProfilePrintf("Backpatching %p (guest PC 0x%08X) to slowmem", lbi.host_pc, lbi.guest_pc);

  // turn it into a jump to the slowmem handler
  Xbyak::CodeGenerator cg(lbi.host_code_size, lbi.host_pc);
  cg.jmp(lbi.host_slowmem_pc);

  const s32 nops = static_cast<s32>(lbi.host_code_size) -
                   static_cast<s32>(static_cast<ptrdiff_t>(cg.getCurr() - static_cast<u8*>(lbi.host_pc)));
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    cg.nop();

  JitCodeBuffer::FlushInstructionCache(lbi.host_pc, lbi.host_code_size);
  return true;
}

void CodeGenerator::BackpatchReturn(void* pc, u32 pc_size)
{
  Log_ProfilePrintf("Backpatching %p to return", pc);

  Xbyak::CodeGenerator cg(pc_size, pc);
  cg.ret();

  const s32 nops =
    static_cast<s32>(pc_size) - static_cast<s32>(static_cast<ptrdiff_t>(cg.getCurr() - static_cast<u8*>(pc)));
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    cg.nop();

  JitCodeBuffer::FlushInstructionCache(pc, pc_size);
}

void CodeGenerator::BackpatchBranch(void* pc, u32 pc_size, void* target)
{
  Log_ProfilePrintf("Backpatching %p to %p [branch]", pc, target);

  Xbyak::CodeGenerator cg(pc_size, pc);
  cg.jmp(target);

  // shouldn't have any nops
  const s32 nops =
    static_cast<s32>(pc_size) - static_cast<s32>(static_cast<ptrdiff_t>(cg.getCurr() - static_cast<u8*>(pc)));
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    cg.nop();

  JitCodeBuffer::FlushInstructionCache(pc, pc_size);
}

void CodeGenerator::EmitLoadGlobal(HostReg host_reg, RegSize size, const void* ptr)
{
  const s64 displacement =
    static_cast<s64>(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr())) + 2;
  if (Xbyak::inner::IsInInt32(static_cast<u64>(displacement)))
  {
    switch (size)
    {
      case RegSize_8:
        m_emit->mov(GetHostReg8(host_reg), m_emit->byte[m_emit->rip + ptr]);
        break;

      case RegSize_16:
        m_emit->mov(GetHostReg16(host_reg), m_emit->word[m_emit->rip + ptr]);
        break;

      case RegSize_32:
        m_emit->mov(GetHostReg32(host_reg), m_emit->dword[m_emit->rip + ptr]);
        break;

      case RegSize_64:
        m_emit->mov(GetHostReg64(host_reg), m_emit->qword[m_emit->rip + ptr]);
        break;

      default:
      {
        UnreachableCode();
      }
      break;
    }
  }
  else
  {
    Value temp = m_register_cache.AllocateScratch(RegSize_64);
    m_emit->mov(GetHostReg64(temp), reinterpret_cast<size_t>(ptr));
    switch (size)
    {
      case RegSize_8:
        m_emit->mov(GetHostReg8(host_reg), m_emit->byte[GetHostReg64(temp)]);
        break;

      case RegSize_16:
        m_emit->mov(GetHostReg16(host_reg), m_emit->word[GetHostReg64(temp)]);
        break;

      case RegSize_32:
        m_emit->mov(GetHostReg32(host_reg), m_emit->dword[GetHostReg64(temp)]);
        break;

      case RegSize_64:
        m_emit->mov(GetHostReg64(host_reg), m_emit->qword[GetHostReg64(temp)]);
        break;

      default:
      {
        UnreachableCode();
      }
      break;
    }
  }
}

void CodeGenerator::EmitStoreGlobal(void* ptr, const Value& value)
{
  DebugAssert(value.IsInHostRegister() || value.IsConstant());

  const s64 displacement =
    static_cast<s64>(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr()));
  if (Xbyak::inner::IsInInt32(static_cast<u64>(displacement)))
  {
    switch (value.size)
    {
      case RegSize_8:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->byte[m_emit->rip + ptr], value.constant_value);
        else
          m_emit->mov(m_emit->byte[m_emit->rip + ptr], GetHostReg8(value.host_reg));
      }
      break;

      case RegSize_16:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->word[m_emit->rip + ptr], value.constant_value);
        else
          m_emit->mov(m_emit->word[m_emit->rip + ptr], GetHostReg16(value.host_reg));
      }
      break;

      case RegSize_32:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->dword[m_emit->rip + ptr], value.constant_value);
        else
          m_emit->mov(m_emit->dword[m_emit->rip + ptr], GetHostReg32(value.host_reg));
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
            m_emit->mov(m_emit->qword[m_emit->rip + ptr], GetHostReg64(temp.host_reg));
          }
          else
          {
            m_emit->mov(m_emit->qword[m_emit->rip + ptr], value.constant_value);
          }
        }
        else
        {
          m_emit->mov(m_emit->qword[m_emit->rip + ptr], GetHostReg64(value.host_reg));
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
  else
  {
    Value address_temp = m_register_cache.AllocateScratch(RegSize_64);
    m_emit->mov(GetHostReg64(address_temp), reinterpret_cast<size_t>(ptr));
    switch (value.size)
    {
      case RegSize_8:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->byte[GetHostReg64(address_temp)], value.constant_value);
        else
          m_emit->mov(m_emit->byte[GetHostReg64(address_temp)], GetHostReg8(value.host_reg));
      }
      break;

      case RegSize_16:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->word[GetHostReg64(address_temp)], value.constant_value);
        else
          m_emit->mov(m_emit->word[GetHostReg64(address_temp)], GetHostReg16(value.host_reg));
      }
      break;

      case RegSize_32:
      {
        if (value.IsConstant())
          m_emit->mov(m_emit->dword[GetHostReg64(address_temp)], value.constant_value);
        else
          m_emit->mov(m_emit->dword[GetHostReg64(address_temp)], GetHostReg32(value.host_reg));
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
            m_emit->mov(m_emit->qword[GetHostReg64(address_temp)], GetHostReg64(temp.host_reg));
          }
          else
          {
            m_emit->mov(m_emit->qword[GetHostReg64(address_temp)], value.constant_value);
          }
        }
        else
        {
          m_emit->mov(m_emit->qword[GetHostReg64(address_temp)], GetHostReg64(value.host_reg));
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
}

void CodeGenerator::EmitFlushInterpreterLoadDelay()
{
  Value reg = m_register_cache.AllocateScratch(RegSize_8);
  Value value = m_register_cache.AllocateScratch(RegSize_32);

  auto load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(State, load_delay_reg)];
  auto load_delay_value = m_emit->dword[GetCPUPtrReg() + offsetof(State, load_delay_value)];
  auto reg_ptr = m_emit->dword[GetCPUPtrReg() + offsetof(State, regs.r[0]) + GetHostReg64(reg.host_reg) * 4];

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

  auto load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(State, load_delay_reg)];
  auto load_delay_value = m_emit->dword[GetCPUPtrReg() + offsetof(State, load_delay_value)];
  auto next_load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(State, next_load_delay_reg)];
  auto next_load_delay_value = m_emit->dword[GetCPUPtrReg() + offsetof(State, next_load_delay_value)];

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

  auto load_delay_reg = m_emit->byte[GetCPUPtrReg() + offsetof(State, load_delay_reg)];

  Xbyak::Label skip_cancel;

  // if load_delay_reg != reg goto skip_cancel
  m_emit->cmp(load_delay_reg, static_cast<u8>(reg));
  m_emit->jne(skip_cancel);

  // load_delay_reg = Reg::count
  m_emit->mov(load_delay_reg, static_cast<u8>(Reg::count));

  m_emit->L(skip_cancel);
}

void CodeGenerator::EmitICacheCheckAndUpdate()
{
  if (GetSegmentForAddress(m_pc) >= Segment::KSEG1)
  {
    m_emit->add(m_emit->dword[GetCPUPtrReg() + offsetof(State, pending_ticks)],
                static_cast<u32>(m_block->uncached_fetch_ticks));
  }
  else
  {
    VirtualMemoryAddress current_pc = m_pc & ICACHE_TAG_ADDRESS_MASK;
    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const VirtualMemoryAddress tag = GetICacheTagForAddress(current_pc);
      const TickCount fill_ticks = GetICacheFillTicks(current_pc);
      if (fill_ticks <= 0)
        continue;

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = offsetof(State, icache_tags) + (line * sizeof(u32));
      Xbyak::Label cache_hit;

      m_emit->cmp(m_emit->dword[GetCPUPtrReg() + offset], tag);
      m_emit->je(cache_hit);
      m_emit->mov(m_emit->dword[GetCPUPtrReg() + offset], tag);
      m_emit->add(m_emit->dword[GetCPUPtrReg() + offsetof(State, pending_ticks)], static_cast<u32>(fill_ticks));
      m_emit->L(cache_hit);
    }
  }
}

void CodeGenerator::EmitStallUntilGTEComplete()
{
  m_emit->mov(GetHostReg32(RRETURN), m_emit->dword[GetCPUPtrReg() + offsetof(State, pending_ticks)]);
  m_emit->mov(GetHostReg32(RARG1), m_emit->dword[GetCPUPtrReg() + offsetof(State, gte_completion_tick)]);

  if (m_delayed_cycles_add > 0)
  {
    m_emit->add(GetHostReg32(RRETURN), static_cast<u32>(m_delayed_cycles_add));
    m_delayed_cycles_add = 0;
  }

  m_emit->cmp(GetHostReg32(RARG1), GetHostReg32(RRETURN));
  m_emit->cmova(GetHostReg32(RRETURN), GetHostReg32(RARG1));
  m_emit->mov(m_emit->dword[GetCPUPtrReg() + offsetof(State, pending_ticks)], GetHostReg32(RRETURN));
}

void CodeGenerator::EmitBranch(const void* address, bool allow_scratch)
{
  const s64 jump_distance =
    static_cast<s64>(reinterpret_cast<intptr_t>(address) - reinterpret_cast<intptr_t>(GetCurrentCodePointer()));
  if (Xbyak::inner::IsInInt32(static_cast<u64>(jump_distance)))
  {
    m_emit->jmp(address);
    return;
  }

  Assert(allow_scratch);

  Value temp = m_register_cache.AllocateScratch(RegSize_64);
  m_emit->mov(GetHostReg64(temp), reinterpret_cast<uintptr_t>(address));
  m_emit->jmp(GetHostReg64(temp));
}

void CodeGenerator::EmitBranch(LabelType* label)
{
  m_emit->jmp(*label);
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
    case Condition::NotZero:
    case Condition::Zero:
    {
      switch (size)
      {
        case RegSize_8:
          m_emit->test(GetHostReg8(value), GetHostReg8(value));
          break;
        case RegSize_16:
          m_emit->test(GetHostReg16(value), GetHostReg16(value));
          break;
        case RegSize_32:
          m_emit->test(GetHostReg32(value), GetHostReg32(value));
          break;
        case RegSize_64:
          m_emit->test(GetHostReg64(value), GetHostReg64(value));
          break;
        default:
          UnreachableCode();
          break;
      }

      EmitConditionalBranch(condition, invert, label);
      return;
    }

    case Condition::Always:
      m_emit->jmp(*label);
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
      m_emit->jmp(*label);
      return;

    default:
      UnreachableCode();
      return;
  }
}

void CodeGenerator::EmitConditionalBranch(Condition condition, bool invert, LabelType* label)
{
  switch (condition)
  {
    case Condition::Always:
      m_emit->jmp(*label);
      break;

    case Condition::NotEqual:
      invert ? m_emit->je(*label) : m_emit->jne(*label);
      break;

    case Condition::Equal:
      invert ? m_emit->jne(*label) : m_emit->je(*label);
      break;

    case Condition::Overflow:
      invert ? m_emit->jno(*label) : m_emit->jo(*label);
      break;

    case Condition::Greater:
      invert ? m_emit->jng(*label) : m_emit->jg(*label);
      break;

    case Condition::GreaterEqual:
      invert ? m_emit->jnge(*label) : m_emit->jge(*label);
      break;

    case Condition::Less:
      invert ? m_emit->jnl(*label) : m_emit->jl(*label);
      break;

    case Condition::LessEqual:
      invert ? m_emit->jnle(*label) : m_emit->jle(*label);
      break;

    case Condition::Negative:
      invert ? m_emit->jns(*label) : m_emit->js(*label);
      break;

    case Condition::PositiveOrZero:
      invert ? m_emit->js(*label) : m_emit->jns(*label);
      break;

    case Condition::Above:
      invert ? m_emit->jna(*label) : m_emit->ja(*label);
      break;

    case Condition::AboveEqual:
      invert ? m_emit->jnae(*label) : m_emit->jae(*label);
      break;

    case Condition::Below:
      invert ? m_emit->jnb(*label) : m_emit->jb(*label);
      break;

    case Condition::BelowEqual:
      invert ? m_emit->jnbe(*label) : m_emit->jbe(*label);
      break;

    case Condition::NotZero:
      invert ? m_emit->jz(*label) : m_emit->jnz(*label);
      break;

    case Condition::Zero:
      invert ? m_emit->jnz(*label) : m_emit->jz(*label);
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitBranchIfBitSet(HostReg reg, RegSize size, u8 bit, LabelType* label)
{
  if (bit < 8)
  {
    // same size, probably faster
    switch (size)
    {
      case RegSize_8:
        m_emit->test(GetHostReg8(reg), (1u << bit));
        m_emit->jnz(*label);
        break;

      case RegSize_16:
        m_emit->test(GetHostReg16(reg), (1u << bit));
        m_emit->jnz(*label);
        break;

      case RegSize_32:
        m_emit->test(GetHostReg32(reg), (1u << bit));
        m_emit->jnz(*label);
        break;

      default:
        UnreachableCode();
        break;
    }
  }
  else
  {
    switch (size)
    {
      case RegSize_8:
        m_emit->bt(GetHostReg8(reg), bit);
        m_emit->jc(*label);
        break;

      case RegSize_16:
        m_emit->bt(GetHostReg16(reg), bit);
        m_emit->jc(*label);
        break;

      case RegSize_32:
        m_emit->bt(GetHostReg32(reg), bit);
        m_emit->jc(*label);
        break;

      default:
        UnreachableCode();
        break;
    }
  }
}

void CodeGenerator::EmitBranchIfBitClear(HostReg reg, RegSize size, u8 bit, LabelType* label)
{
  if (bit < 8)
  {
    // same size, probably faster
    switch (size)
    {
      case RegSize_8:
        m_emit->test(GetHostReg8(reg), (1u << bit));
        m_emit->jz(*label);
        break;

      case RegSize_16:
        m_emit->test(GetHostReg16(reg), (1u << bit));
        m_emit->jz(*label);
        break;

      case RegSize_32:
        m_emit->test(GetHostReg32(reg), (1u << bit));
        m_emit->jz(*label);
        break;

      default:
        UnreachableCode();
        break;
    }
  }
  else
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
}

void CodeGenerator::EmitBindLabel(LabelType* label)
{
  m_emit->L(*label);
}

void CodeGenerator::EmitLoadGlobalAddress(HostReg host_reg, const void* ptr)
{
  const s64 displacement =
    static_cast<s64>(reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(m_emit->getCurr())) + 2;
  if (Xbyak::inner::IsInInt32(static_cast<u64>(displacement)))
    m_emit->lea(GetHostReg64(host_reg), m_emit->dword[m_emit->rip + ptr]);
  else
    m_emit->mov(GetHostReg64(host_reg), reinterpret_cast<size_t>(ptr));
}

CodeCache::DispatcherFunction CodeGenerator::CompileDispatcher()
{
  m_register_cache.ReserveCalleeSavedRegisters();
  const u32 stack_adjust = PrepareStackForCall();

  EmitLoadGlobalAddress(Xbyak::Operand::RBP, &g_state);

  Xbyak::Label frame_done_loop;
  Xbyak::Label exit_dispatcher;
  m_emit->L(frame_done_loop);

  // if frame_done goto exit_dispatcher
  m_emit->test(m_emit->byte[m_emit->rbp + offsetof(State, frame_done)], 1);
  m_emit->jnz(exit_dispatcher, Xbyak::CodeGenerator::T_NEAR);

  // eax <- sr
  Xbyak::Label no_interrupt;
  m_emit->mov(m_emit->eax, m_emit->dword[m_emit->rbp + offsetof(State, cop0_regs.sr.bits)]);

  // if Iec == 0 then goto no_interrupt
  m_emit->test(m_emit->eax, 1);
  m_emit->jz(no_interrupt);

  // sr & cause
  m_emit->and_(m_emit->eax, m_emit->dword[m_emit->rbp + offsetof(State, cop0_regs.cause.bits)]);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  m_emit->test(m_emit->eax, 0xFF00);
  m_emit->jz(no_interrupt);

  // we have an interrupt
  EmitCall(reinterpret_cast<const void*>(&DispatchInterrupt));

  // no interrupt or we just serviced it
  m_emit->L(no_interrupt);

  // TimingEvents::UpdateCPUDowncount:
  // eax <- head event->downcount
  // downcount <- eax
  EmitLoadGlobalAddress(Xbyak::Operand::RAX, TimingEvents::GetHeadEventPtr());
  m_emit->mov(m_emit->rax, m_emit->qword[m_emit->rax]);
  m_emit->mov(m_emit->eax, m_emit->dword[m_emit->rax + offsetof(TimingEvent, m_downcount)]);
  m_emit->mov(m_emit->dword[m_emit->rbp + offsetof(State, downcount)], m_emit->eax);

  // main dispatch loop
  Xbyak::Label main_loop;
  m_emit->align(16);
  m_emit->L(main_loop);

  // eax <- pending_ticks
  m_emit->mov(m_emit->eax, m_emit->dword[m_emit->rbp + offsetof(State, pending_ticks)]);

  // while eax < downcount
  Xbyak::Label downcount_hit;
  m_emit->cmp(m_emit->eax, m_emit->dword[m_emit->rbp + offsetof(State, downcount)]);
  m_emit->jge(downcount_hit);

  // time to lookup the block
  // eax <- pc
  m_emit->mov(m_emit->eax, m_emit->dword[m_emit->rbp + offsetof(State, regs.pc)]);

  // rcx <- s_fast_map[pc >> 16]
  EmitLoadGlobalAddress(Xbyak::Operand::RBX, CodeCache::GetFastMapPointer());
  m_emit->mov(m_emit->ecx, m_emit->eax);
  m_emit->shr(m_emit->ecx, 16);
  m_emit->mov(m_emit->rcx, m_emit->qword[m_emit->rbx + m_emit->rcx * 8]);

  // call(rcx[pc * 2]) (fast_map[pc >> 2])
  m_emit->call(m_emit->qword[m_emit->rcx + m_emit->rax * 2]);

  m_emit->jmp(main_loop);

  // end while
  m_emit->L(downcount_hit);

  // check events then for frame done
  EmitLoadGlobalAddress(Xbyak::Operand::RAX, TimingEvents::GetHeadEventPtr());
  m_emit->mov(m_emit->rax, m_emit->qword[m_emit->rax]);
  m_emit->mov(m_emit->eax, m_emit->dword[m_emit->rax + offsetof(TimingEvent, m_downcount)]);
  m_emit->cmp(m_emit->eax, m_emit->dword[m_emit->rbp + offsetof(State, pending_ticks)]);
  m_emit->jg(frame_done_loop);
  EmitCall(reinterpret_cast<const void*>(&TimingEvents::RunEvents));
  m_emit->jmp(frame_done_loop);

  // all done
  m_emit->L(exit_dispatcher);
  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->ret();

  CodeBlock::HostCodePointer ptr;
  u32 code_size;
  FinalizeBlock(&ptr, &code_size);
  Log_DevPrintf("Dispatcher is %u bytes at %p", code_size, ptr);
  return ptr;
}

CodeCache::SingleBlockDispatcherFunction CodeGenerator::CompileSingleBlockDispatcher()
{
  m_register_cache.ReserveCalleeSavedRegisters();
  const u32 stack_adjust = PrepareStackForCall();

  EmitLoadGlobalAddress(Xbyak::Operand::RBP, &g_state);

  m_emit->call(GetHostReg64(RARG1));

  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->ret();

  CodeBlock::HostCodePointer ptr;
  u32 code_size;
  FinalizeBlock(&ptr, &code_size);
  Log_DevPrintf("Single block dispatcher is %u bytes at %p", code_size, ptr);
  return reinterpret_cast<CodeCache::SingleBlockDispatcherFunction>(ptr);
}

} // namespace CPU::Recompiler
