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

// #include "vixl/aarch32/disasm-aarch32.h"
// #include <iostream>

namespace a32 = vixl::aarch32;

namespace CPU::Recompiler {

constexpr HostReg RCPUPTR = 4;
constexpr HostReg RRETURN = 0;
constexpr HostReg RARG1 = 0;
constexpr HostReg RARG2 = 1;
constexpr HostReg RARG3 = 2;
constexpr HostReg RARG4 = 3;
constexpr HostReg RSCRATCH = 12;
constexpr u32 FUNCTION_CALL_SHADOW_SPACE = 32;
constexpr u32 FUNCTION_CALLEE_SAVED_SPACE_RESERVE = 80;  // 8 registers
constexpr u32 FUNCTION_CALLER_SAVED_SPACE_RESERVE = 144; // 18 registers -> 224 bytes
constexpr u32 FUNCTION_STACK_SIZE =
  FUNCTION_CALLEE_SAVED_SPACE_RESERVE + FUNCTION_CALLER_SAVED_SPACE_RESERVE + FUNCTION_CALL_SHADOW_SPACE;

// PC we return to after the end of the block
static void* s_dispatcher_return_address;

static s32 GetPCDisplacement(const void* current, const void* target)
{
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(current), 4));
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(target), 4));
  return static_cast<s32>((reinterpret_cast<ptrdiff_t>(target) - reinterpret_cast<ptrdiff_t>(current)));
}

static bool IsPCDisplacementInImmediateRange(s32 displacement)
{
  return (displacement >= -33554432 && displacement <= 33554428);
}

static const a32::Register GetHostReg8(HostReg reg)
{
  return a32::Register(reg);
}

static const a32::Register GetHostReg8(const Value& value)
{
  DebugAssert(value.size == RegSize_8 && value.IsInHostRegister());
  return a32::Register(value.host_reg);
}

static const a32::Register GetHostReg16(HostReg reg)
{
  return a32::Register(reg);
}

static const a32::Register GetHostReg16(const Value& value)
{
  DebugAssert(value.size == RegSize_16 && value.IsInHostRegister());
  return a32::Register(value.host_reg);
}

static const a32::Register GetHostReg32(HostReg reg)
{
  return a32::Register(reg);
}

static const a32::Register GetHostReg32(const Value& value)
{
  DebugAssert(value.size == RegSize_32 && value.IsInHostRegister());
  return a32::Register(value.host_reg);
}

static const a32::Register GetCPUPtrReg()
{
  return GetHostReg32(RCPUPTR);
}

CodeGenerator::CodeGenerator(JitCodeBuffer* code_buffer)
  : m_code_buffer(code_buffer), m_register_cache(*this),
    m_near_emitter(static_cast<vixl::byte*>(code_buffer->GetFreeCodePointer()), code_buffer->GetFreeCodeSpace(),
                   a32::A32),
    m_far_emitter(static_cast<vixl::byte*>(code_buffer->GetFreeFarCodePointer()), code_buffer->GetFreeFarCodeSpace(),
                  a32::A32),
    m_emit(&m_near_emitter)
{
  InitHostRegs();
}

CodeGenerator::~CodeGenerator() = default;

const char* CodeGenerator::GetHostRegName(HostReg reg, RegSize size /*= HostPointerSize*/)
{
  static constexpr std::array<const char*, HostReg_Count> reg_names = {
    {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"}};
  if (reg >= static_cast<HostReg>(HostReg_Count))
    return "";

  switch (size)
  {
    case RegSize_32:
      return reg_names[reg];
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
  // allocate nonvolatile before volatile
  // NOTE: vixl also uses r12 for the macro assembler
  m_register_cache.SetHostRegAllocationOrder({4, 5, 6, 7, 8, 9, 10, 11});
  m_register_cache.SetCallerSavedHostRegs({0, 1, 2, 3, 12});
  m_register_cache.SetCalleeSavedHostRegs({4, 5, 6, 7, 8, 9, 10, 11, 13, 14});
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

  Value new_value = m_register_cache.AllocateScratch(value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

Value CodeGenerator::GetValueInHostOrScratchRegister(const Value& value, bool allow_zero_register /* = true */)
{
  if (value.IsInHostRegister())
    return Value::FromHostReg(&m_register_cache, value.host_reg, value.size);

  Value new_value = Value::FromHostReg(&m_register_cache, RSCRATCH, value.size);
  EmitCopyValue(new_value.host_reg, value);
  return new_value;
}

void CodeGenerator::EmitBeginBlock(bool allocate_registers /* = true */)
{
  m_emit->sub(a32::sp, a32::sp, FUNCTION_STACK_SIZE);

  if (allocate_registers)
  {
    // Save the link register, since we'll be calling functions.
    const bool link_reg_allocated = m_register_cache.AllocateHostReg(14);
    DebugAssert(link_reg_allocated);
    UNREFERENCED_VARIABLE(link_reg_allocated);
    m_register_cache.AssumeCalleeSavedRegistersAreSaved();

    // Store the CPU struct pointer. TODO: make this better.
    const bool cpu_reg_allocated = m_register_cache.AllocateHostReg(RCPUPTR);
    // m_emit->Mov(GetCPUPtrReg(), reinterpret_cast<uintptr_t>(&g_state));
    DebugAssert(cpu_reg_allocated);
    UNREFERENCED_VARIABLE(cpu_reg_allocated);
  }
}

void CodeGenerator::EmitEndBlock(bool free_registers /* = true */, bool emit_return /* = true */)
{
  if (free_registers)
  {
    m_register_cache.FreeHostReg(RCPUPTR);
    m_register_cache.FreeHostReg(14);
    m_register_cache.PopCalleeSavedRegisters(true);
  }

  m_emit->add(a32::sp, a32::sp, FUNCTION_STACK_SIZE);

  if (emit_return)
  {
    // m_emit->b(GetPCDisplacement(GetCurrentCodePointer(), s_dispatcher_return_address));
    m_emit->bx(a32::lr);
  }
}

void CodeGenerator::EmitExceptionExit()
{
  // ensure all unflushed registers are written back
  m_register_cache.FlushAllGuestRegisters(false, false);

  // the interpreter load delay might have its own value, but we'll overwrite it here anyway
  // technically RaiseException() and FlushPipeline() have already been called, but that should be okay
  m_register_cache.FlushLoadDelay(false);

  m_register_cache.PopCalleeSavedRegisters(false);

  m_emit->add(a32::sp, a32::sp, FUNCTION_STACK_SIZE);
  // m_emit->b(GetPCDisplacement(GetCurrentCodePointer(), s_dispatcher_return_address));
  m_emit->bx(a32::lr);
}

void CodeGenerator::EmitExceptionExitOnBool(const Value& value)
{
  Assert(!value.IsConstant() && value.IsInHostRegister());

  m_register_cache.PushState();

  // TODO: This is... not great.
  a32::Label skip_branch;
  m_emit->tst(GetHostReg32(value.host_reg), 1);
  m_emit->b(a32::eq, &skip_branch);
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

  m_near_emitter = CodeEmitter(static_cast<vixl::byte*>(m_code_buffer->GetFreeCodePointer()),
                               m_code_buffer->GetFreeCodeSpace(), a32::A32);
  m_far_emitter = CodeEmitter(static_cast<vixl::byte*>(m_code_buffer->GetFreeFarCodePointer()),
                              m_code_buffer->GetFreeFarCodeSpace(), a32::A32);

#if 0
  a32::PrintDisassembler dis(std::cout, 0);
  dis.SetCodeAddress(reinterpret_cast<uintptr_t>(*out_host_code));
  dis.DisassembleA32Buffer(reinterpret_cast<u32*>(*out_host_code), *out_host_code_size);
#endif
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
        m_emit->Mov(GetHostReg32(to_reg), value.GetS32ConstantValue());
      else
        m_emit->Mov(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
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
    if (set_flags)
      m_emit->adds(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    else
      m_emit->add(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  const s32 constant_value = value.GetS32ConstantValue();
  if (a32::ImmediateA32::IsImmediateA32(static_cast<u32>(constant_value)))
  {
    if (set_flags)
      m_emit->adds(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);
    else
      m_emit->add(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);

    return;
  }

  // need a temporary
  m_emit->Mov(GetHostReg32(RSCRATCH), constant_value);
  if (set_flags)
    m_emit->adds(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
  else
    m_emit->add(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
}

void CodeGenerator::EmitSub(HostReg to_reg, HostReg from_reg, const Value& value, bool set_flags)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    if (set_flags)
      m_emit->subs(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    else
      m_emit->sub(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));

    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  const s32 constant_value = value.GetS32ConstantValue();
  if (a32::ImmediateA32::IsImmediateA32(static_cast<u32>(constant_value)))
  {
    if (set_flags)
      m_emit->subs(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);
    else
      m_emit->sub(GetHostReg32(to_reg), GetHostReg32(from_reg), constant_value);

    return;
  }

  // need a temporary
  m_emit->Mov(GetHostReg32(RSCRATCH), constant_value);
  if (set_flags)
    m_emit->subs(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
  else
    m_emit->sub(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
}

void CodeGenerator::EmitCmp(HostReg to_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    m_emit->cmp(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  const s32 constant_value = value.GetS32ConstantValue();
  if (constant_value >= 0)
  {
    if (a32::ImmediateA32::IsImmediateA32(static_cast<u32>(constant_value)))
    {
      m_emit->cmp(GetHostReg32(to_reg), constant_value);
      return;
    }
  }
  else
  {
    if (a32::ImmediateA32::IsImmediateA32(static_cast<u32>(-constant_value)))
    {
      m_emit->cmn(GetHostReg32(to_reg), -constant_value);
      return;
    }
  }

  // need a temporary
  m_emit->Mov(GetHostReg32(RSCRATCH), constant_value);
  m_emit->cmp(GetHostReg32(to_reg), GetHostReg32(RSCRATCH));
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
      m_emit->smull(GetHostReg32(to_reg_lo), GetHostReg32(to_reg_hi), GetHostReg32(lhs_in_reg.host_reg),
                    GetHostReg32(rhs_in_reg.host_reg));
    }
    else
    {
      m_emit->umull(GetHostReg32(to_reg_lo), GetHostReg32(to_reg_hi), GetHostReg32(lhs_in_reg.host_reg),
                    GetHostReg32(rhs_in_reg.host_reg));
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
    quotient_value.SetHostReg(&m_register_cache, RSCRATCH, size);
  else
    quotient_value.SetHostReg(&m_register_cache, to_reg_quotient, size);

  if (signed_divide)
  {
    m_emit->sdiv(GetHostReg32(quotient_value), GetHostReg32(num), GetHostReg32(denom));
    if (to_reg_remainder != HostReg_Count)
    {
      m_emit->mul(GetHostReg32(to_reg_remainder), GetHostReg32(quotient_value), GetHostReg32(denom));
      m_emit->sub(GetHostReg32(to_reg_remainder), GetHostReg32(num), GetHostReg32(to_reg_remainder));
    }
  }
  else
  {
    m_emit->udiv(GetHostReg32(quotient_value), GetHostReg32(num), GetHostReg32(denom));
    if (to_reg_remainder != HostReg_Count)
    {
      m_emit->mul(GetHostReg32(to_reg_remainder), GetHostReg32(quotient_value), GetHostReg32(denom));
      m_emit->sub(GetHostReg32(to_reg_remainder), GetHostReg32(num), GetHostReg32(to_reg_remainder));
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
                            bool assume_amount_masked)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
    {
      if (amount_value.IsConstant())
      {
        m_emit->lsl(GetHostReg32(to_reg), GetHostReg32(from_reg), static_cast<u32>(amount_value.constant_value & 0x1F));
      }
      else if (assume_amount_masked)
      {
        m_emit->lsl(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(amount_value));
      }
      else
      {
        m_emit->and_(GetHostReg32(RSCRATCH), GetHostReg32(amount_value), 0x1F);
        m_emit->lsl(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
      }

      if (size == RegSize_8)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFF);
      else if (size == RegSize_16)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFFFF);
    }
    break;
  }
}

void CodeGenerator::EmitShr(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
    {
      if (amount_value.IsConstant())
      {
        m_emit->lsr(GetHostReg32(to_reg), GetHostReg32(from_reg), static_cast<u32>(amount_value.constant_value & 0x1F));
      }
      else if (assume_amount_masked)
      {
        m_emit->lsr(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(amount_value));
      }
      else
      {
        m_emit->and_(GetHostReg32(RSCRATCH), GetHostReg32(amount_value), 0x1F);
        m_emit->lsr(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
      }

      if (size == RegSize_8)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFF);
      else if (size == RegSize_16)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFFFF);
    }
    break;
  }
}

void CodeGenerator::EmitSar(HostReg to_reg, HostReg from_reg, RegSize size, const Value& amount_value,
                            bool assume_amount_masked)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
    {
      if (amount_value.IsConstant())
      {
        m_emit->asr(GetHostReg32(to_reg), GetHostReg32(from_reg), static_cast<u32>(amount_value.constant_value & 0x1F));
      }
      else if (assume_amount_masked)
      {
        m_emit->asr(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(amount_value));
      }
      else
      {
        m_emit->and_(GetHostReg32(RSCRATCH), GetHostReg32(amount_value), 0x1F);
        m_emit->asr(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
      }

      if (size == RegSize_8)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFF);
      else if (size == RegSize_16)
        m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), 0xFFFF);
    }
    break;
  }
}

static bool CanFitInBitwiseImmediate(const Value& value)
{
  return a32::ImmediateA32::IsImmediateA32(static_cast<u32>(value.constant_value));
}

void CodeGenerator::EmitAnd(HostReg to_reg, HostReg from_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), s32(value.constant_value));
    return;
  }

  // need a temporary
  m_emit->Mov(GetHostReg32(RSCRATCH), s32(value.constant_value));
  m_emit->and_(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
}

void CodeGenerator::EmitOr(HostReg to_reg, HostReg from_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    m_emit->orr(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    m_emit->orr(GetHostReg32(to_reg), GetHostReg32(from_reg), s32(value.constant_value));
    return;
  }

  // need a temporary
  m_emit->Mov(GetHostReg32(RSCRATCH), s32(value.constant_value));
  m_emit->orr(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
}

void CodeGenerator::EmitXor(HostReg to_reg, HostReg from_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    m_emit->eor(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(value.host_reg));
    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    m_emit->eor(GetHostReg32(to_reg), GetHostReg32(from_reg), s32(value.constant_value));
    return;
  }

  // need a temporary
  m_emit->Mov(GetHostReg32(RSCRATCH), s32(value.constant_value));
  m_emit->eor(GetHostReg32(to_reg), GetHostReg32(from_reg), GetHostReg32(RSCRATCH));
}

void CodeGenerator::EmitTest(HostReg to_reg, const Value& value)
{
  Assert(value.IsConstant() || value.IsInHostRegister());

  // if it's in a host register already, this is easy
  if (value.IsInHostRegister())
  {
    m_emit->tst(GetHostReg32(to_reg), GetHostReg32(value.host_reg));
    return;
  }

  // do we need temporary storage for the constant, if it won't fit in an immediate?
  if (CanFitInBitwiseImmediate(value))
  {
    m_emit->tst(GetHostReg32(to_reg), s32(value.constant_value));
    return;
  }

  // need a temporary
  m_emit->Mov(GetHostReg32(RSCRATCH), s32(value.constant_value));
  m_emit->tst(GetHostReg32(to_reg), GetHostReg32(RSCRATCH));
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

    default:
      break;
  }
}

void CodeGenerator::EmitSetConditionResult(HostReg to_reg, RegSize to_size, Condition condition)
{
  if (condition == Condition::Always)
  {
    m_emit->Mov(GetHostReg32(to_reg), 1);
    return;
  }

  a32::Condition acond(a32::Condition::Never());
  switch (condition)
  {
    case Condition::NotEqual:
      acond = a32::ne;
      break;

    case Condition::Equal:
      acond = a32::eq;
      break;

    case Condition::Overflow:
      acond = a32::vs;
      break;

    case Condition::Greater:
      acond = a32::gt;
      break;

    case Condition::GreaterEqual:
      acond = a32::ge;
      break;

    case Condition::Less:
      acond = a32::lt;
      break;

    case Condition::LessEqual:
      acond = a32::le;
      break;

    case Condition::Negative:
      acond = a32::mi;
      break;

    case Condition::PositiveOrZero:
      acond = a32::pl;
      break;

    case Condition::Above:
      acond = a32::hi;
      break;

    case Condition::AboveEqual:
      acond = a32::cs;
      break;

    case Condition::Below:
      acond = a32::cc;
      break;

    case Condition::BelowEqual:
      acond = a32::ls;
      break;

    default:
      UnreachableCode();
      return;
  }

  m_emit->mov(GetHostReg32(to_reg), 0);
  m_emit->mov(acond, GetHostReg32(to_reg), 1);
}

u32 CodeGenerator::PrepareStackForCall()
{
  m_fastmem_load_base_in_register = false;
  m_fastmem_store_base_in_register = false;
  m_register_cache.PushCallerSavedRegisters();
  return 0;
}

void CodeGenerator::RestoreStackAfterCall(u32 adjust_size)
{
  m_register_cache.PopCallerSavedRegisters();
}

void CodeGenerator::EmitCall(const void* ptr)
{
  const s32 displacement = GetPCDisplacement(GetCurrentCodePointer(), ptr);
  if (!IsPCDisplacementInImmediateRange(displacement))
  {
    m_emit->Mov(GetHostReg32(RSCRATCH), reinterpret_cast<uintptr_t>(ptr));
    m_emit->blx(GetHostReg32(RSCRATCH));
  }
  else
  {
    a32::Label label(displacement + m_emit->GetCursorOffset());
    m_emit->bl(&label);
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
  const a32::MemOperand addr(a32::sp, FUNCTION_STACK_SIZE - FUNCTION_CALL_SHADOW_SPACE - (position * 4));
  m_emit->str(GetHostReg32(reg), addr);
}

void CodeGenerator::EmitPushHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  // TODO: Use stm?
  EmitPushHostReg(reg, position);
  EmitPushHostReg(reg2, position + 1);
}

void CodeGenerator::EmitPopHostReg(HostReg reg, u32 position)
{
  const a32::MemOperand addr(a32::sp, FUNCTION_STACK_SIZE - FUNCTION_CALL_SHADOW_SPACE - (position * 4));
  m_emit->ldr(GetHostReg32(reg), addr);
}

void CodeGenerator::EmitPopHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  // TODO: Use ldm?
  Assert(position > 0);
  EmitPopHostReg(reg2, position);
  EmitPopHostReg(reg, position - 1);
}

void CodeGenerator::EmitLoadCPUStructField(HostReg host_reg, RegSize guest_size, u32 offset)
{
  const s32 s_offset = static_cast<s32>(offset);

  switch (guest_size)
  {
    case RegSize_8:
      m_emit->ldrb(GetHostReg8(host_reg), a32::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_16:
      m_emit->ldrh(GetHostReg16(host_reg), a32::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_32:
      m_emit->ldr(GetHostReg32(host_reg), a32::MemOperand(GetCPUPtrReg(), s_offset));
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
  const Value hr_value = GetValueInHostOrScratchRegister(value);
  const s32 s_offset = static_cast<s32>(offset);

  switch (value.size)
  {
    case RegSize_8:
      m_emit->strb(GetHostReg8(hr_value), a32::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_16:
      m_emit->strh(GetHostReg16(hr_value), a32::MemOperand(GetCPUPtrReg(), s_offset));
      break;

    case RegSize_32:
      m_emit->str(GetHostReg32(hr_value), a32::MemOperand(GetCPUPtrReg(), s_offset));
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
  const s32 s_offset = static_cast<s32>(offset);
  const a32::MemOperand o_offset(GetCPUPtrReg(), s_offset);

  Value real_value;
  if (value.IsInHostRegister())
  {
    real_value.SetHostReg(&m_register_cache, value.host_reg, value.size);
  }
  else
  {
    // do we need temporary storage for the constant, if it won't fit in an immediate?
    Assert(value.IsConstant());
    const s32 constant_value = value.GetS32ConstantValue();
    if (!a32::ImmediateA32::IsImmediateA32(static_cast<u32>(constant_value)))
    {
      real_value.SetHostReg(&m_register_cache, RARG2, value.size);
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
      m_emit->Ldrb(GetHostReg8(RARG1), o_offset);
      if (real_value.IsConstant())
        m_emit->Add(GetHostReg8(RARG1), GetHostReg8(RARG1), real_value.GetS32ConstantValue());
      else
        m_emit->Add(GetHostReg8(RARG1), GetHostReg8(RARG1), GetHostReg8(real_value));
      m_emit->Strb(GetHostReg8(RARG1), o_offset);
    }
    break;

    case RegSize_16:
    {
      m_emit->Ldrh(GetHostReg16(RARG1), o_offset);
      if (real_value.IsConstant())
        m_emit->Add(GetHostReg16(RARG1), GetHostReg16(RARG1), real_value.GetS32ConstantValue());
      else
        m_emit->Add(GetHostReg16(RARG1), GetHostReg16(RARG1), GetHostReg16(real_value));
      m_emit->Strh(GetHostReg16(RARG1), o_offset);
    }
    break;

    case RegSize_32:
    {
      m_emit->Ldr(GetHostReg32(RARG1), o_offset);
      if (real_value.IsConstant())
        m_emit->Add(GetHostReg32(RARG1), GetHostReg32(RARG1), real_value.GetS32ConstantValue());
      else
        m_emit->Add(GetHostReg32(RARG1), GetHostReg32(RARG1), GetHostReg32(real_value));
      m_emit->Str(GetHostReg32(RARG1), o_offset);
    }
    break;

    default:
    {
      UnreachableCode();
    }
    break;
  }
}

Value CodeGenerator::GetFastmemLoadBase()
{
  Value val = Value::FromHostReg(&m_register_cache, RARG4, RegSize_32);
  if (!m_fastmem_load_base_in_register)
  {
    m_emit->ldr(GetHostReg32(val), a32::MemOperand(GetCPUPtrReg(), offsetof(CPU::State, fastmem_base)));
    m_fastmem_load_base_in_register = true;
  }

  return val;
}

Value CodeGenerator::GetFastmemStoreBase()
{
  Value val = Value::FromHostReg(&m_register_cache, RARG3, RegSize_32);
  if (!m_fastmem_store_base_in_register)
  {
    m_emit->ldr(GetHostReg32(val), a32::MemOperand(GetCPUPtrReg(), offsetof(CPU::State, fastmem_base)));
    m_emit->add(GetHostReg32(val), GetHostReg32(val), sizeof(u32*) * Bus::FASTMEM_LUT_NUM_PAGES);
    m_fastmem_store_base_in_register = true;
  }

  return val;
}

void CodeGenerator::EmitUpdateFastmemBase()
{
  if (m_fastmem_load_base_in_register)
  {
    Value val = Value::FromHostReg(&m_register_cache, RARG4, RegSize_32);
    m_emit->ldr(GetHostReg32(val), a32::MemOperand(GetCPUPtrReg(), offsetof(CPU::State, fastmem_base)));
  }

  if (m_fastmem_store_base_in_register)
  {
    Value val = Value::FromHostReg(&m_register_cache, RARG3, RegSize_32);
    m_emit->ldr(GetHostReg32(val), a32::MemOperand(GetCPUPtrReg(), offsetof(CPU::State, fastmem_base)));
    m_emit->add(GetHostReg32(val), GetHostReg32(val), sizeof(u32*) * Bus::FASTMEM_LUT_NUM_PAGES);
  }
}

void CodeGenerator::EmitLoadGuestRAMFastmem(const Value& address, RegSize size, Value& result)
{
  Value fastmem_base = GetFastmemLoadBase();

  HostReg address_reg;
  if (address.IsConstant())
  {
    m_emit->Mov(GetHostReg32(RSCRATCH), static_cast<u32>(address.constant_value));
    address_reg = RSCRATCH;
  }
  else
  {
    address_reg = address.host_reg;
  }

  m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), 12);
  m_emit->and_(GetHostReg32(RARG2), GetHostReg32(address_reg), HOST_PAGE_OFFSET_MASK);
  m_emit->ldr(GetHostReg32(RARG1),
              a32::MemOperand(GetHostReg32(fastmem_base), GetHostReg32(RARG1), a32::LSL, 2)); // pointer load

  switch (size)
  {
    case RegSize_8:
      m_emit->ldrb(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    case RegSize_16:
      m_emit->ldrh(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    case RegSize_32:
      m_emit->ldr(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    default:
      UnreachableCode();
      break;
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

  Value fastmem_base = GetFastmemLoadBase();

  HostReg address_reg;
  if (address.IsConstant())
  {
    m_emit->Mov(GetHostReg32(RSCRATCH), static_cast<u32>(address.constant_value));
    address_reg = RSCRATCH;
  }
  else
  {
    address_reg = address.host_reg;
  }

  m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), 12);
  m_emit->and_(GetHostReg32(RARG2), GetHostReg32(address_reg), HOST_PAGE_OFFSET_MASK);
  m_emit->ldr(GetHostReg32(RARG1),
              a32::MemOperand(GetHostReg32(fastmem_base), GetHostReg32(RARG1), a32::LSL, 2)); // pointer load

  m_register_cache.InhibitAllocation();
  bpi.host_pc = GetCurrentNearCodePointer();

  switch (size)
  {
    case RegSize_8:
      m_emit->ldrb(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    case RegSize_16:
      m_emit->ldrh(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    case RegSize_32:
      m_emit->ldr(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    default:
      UnreachableCode();
      break;
  }

  bpi.host_code_size = static_cast<u32>(
    static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc)));

  const bool old_store_fastmem_base = m_fastmem_store_base_in_register;

  // generate slowmem fallback
  bpi.host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();

  // we add the ticks *after* the add here, since we counted incorrectly, then correct for it below
  DebugAssert(m_delayed_cycles_add > 0);
  EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));
  m_delayed_cycles_add += Bus::RAM_READ_TICKS;

  EmitLoadGuestMemorySlowmem(cbi, address, size, result, true);

  EmitAddCPUStructField(offsetof(State, pending_ticks),
                        Value::FromConstantU32(static_cast<u32>(-m_delayed_cycles_add)));

  // restore fastmem base state for the next instruction
  if (old_store_fastmem_base)
    fastmem_base = GetFastmemStoreBase();
  fastmem_base = GetFastmemLoadBase();

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

    a32::Label load_okay;
    m_emit->tst(GetHostReg32(1), 1);
    m_emit->b(a32::ne, &load_okay);
    EmitBranch(GetCurrentFarCodePointer());
    m_emit->Bind(&load_okay);

    // load exception path
    if (!in_far_code)
      SwitchToFarCode();

    // cause_bits = (-result << 2) | BD | cop_n
    m_emit->rsb(GetHostReg32(result.host_reg), GetHostReg32(result.host_reg), 0);
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

void CodeGenerator::EmitStoreGuestMemoryFastmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                                const Value& value)
{
  LoadStoreBackpatchInfo bpi;
  bpi.address_host_reg = HostReg_Invalid;
  bpi.value_host_reg = value.host_reg;
  bpi.guest_pc = m_current_instruction->pc;

  Value fastmem_base = GetFastmemStoreBase();
  Value actual_value = GetValueInHostRegister(value);

  HostReg address_reg;
  if (address.IsConstant())
  {
    m_emit->Mov(GetHostReg32(RSCRATCH), static_cast<u32>(address.constant_value));
    address_reg = RSCRATCH;
  }
  else
  {
    address_reg = address.host_reg;
  }

  // TODO: if this gets backpatched, these instructions are wasted

  m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), 12);
  m_emit->and_(GetHostReg32(RARG2), GetHostReg32(address_reg), HOST_PAGE_OFFSET_MASK);
  m_emit->ldr(GetHostReg32(RARG1),
              a32::MemOperand(GetHostReg32(fastmem_base), GetHostReg32(RARG1), a32::LSL, 2)); // pointer load

  m_register_cache.InhibitAllocation();
  bpi.host_pc = GetCurrentNearCodePointer();

  switch (size)
  {
    case RegSize_8:
      m_emit->strb(GetHostReg32(actual_value.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    case RegSize_16:
      m_emit->strh(GetHostReg32(actual_value.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    case RegSize_32:
      m_emit->str(GetHostReg32(actual_value.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(RARG2)));
      break;

    default:
      UnreachableCode();
      break;
  }

  bpi.host_code_size = static_cast<u32>(
    static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(bpi.host_pc)));

  const bool old_load_fastmem_base = m_fastmem_load_base_in_register;

  // generate slowmem fallback
  bpi.host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();

  DebugAssert(m_delayed_cycles_add > 0);
  EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));

  EmitStoreGuestMemorySlowmem(cbi, address, size, actual_value, true);

  EmitAddCPUStructField(offsetof(State, pending_ticks),
                        Value::FromConstantU32(static_cast<u32>(-m_delayed_cycles_add)));

  // restore fastmem base state for the next instruction
  if (old_load_fastmem_base)
    fastmem_base = GetFastmemLoadBase();
  fastmem_base = GetFastmemStoreBase();

  // return to the block code
  EmitBranch(GetCurrentNearCodePointer(), false);

  SwitchToNearCode();
  m_register_cache.UninhibitAllocation();

  m_block->loadstore_backpatch_info.push_back(bpi);
}

void CodeGenerator::EmitStoreGuestMemorySlowmem(const CodeBlockInstruction& cbi, const Value& address, RegSize size,
                                                const Value& value, bool in_far_code)
{
  Value value_in_hr = GetValueInHostRegister(value);

  if (g_settings.cpu_recompiler_memory_exceptions)
  {
    Assert(!in_far_code);

    Value result = m_register_cache.AllocateScratch(RegSize_32);
    switch (size)
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

    a32::Label store_okay;
    m_emit->tst(GetHostReg32(result.host_reg), 1);
    m_emit->b(a32::eq, &store_okay);
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
    switch (size)
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

  // turn it into a jump to the slowmem handler
  vixl::aarch32::MacroAssembler emit(static_cast<vixl::byte*>(lbi.host_pc), lbi.host_code_size, a32::A32);

  // check jump distance
  const s32 displacement = GetPCDisplacement(lbi.host_pc, lbi.host_slowmem_pc);
  if (!IsPCDisplacementInImmediateRange(displacement))
  {
    emit.Mov(GetHostReg32(RSCRATCH), reinterpret_cast<uintptr_t>(lbi.host_slowmem_pc));
    emit.bx(GetHostReg32(RSCRATCH));
  }
  else
  {
    a32::Label label(displacement + emit.GetCursorOffset());
    emit.b(&label);
  }

  const s32 nops = (static_cast<s32>(lbi.host_code_size) - static_cast<s32>(emit.GetCursorOffset())) / 4;
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    emit.nop();

  JitCodeBuffer::FlushInstructionCache(lbi.host_pc, lbi.host_code_size);
  return true;
}

void CodeGenerator::BackpatchReturn(void* pc, u32 pc_size)
{
  Log_ProfilePrintf("Backpatching %p to return", pc);

  vixl::aarch32::MacroAssembler emit(static_cast<vixl::byte*>(pc), pc_size, a32::A32);
  emit.bx(a32::lr);

  const s32 nops = (static_cast<s32>(pc_size) - static_cast<s32>(emit.GetCursorOffset())) / 4;
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    emit.nop();

  JitCodeBuffer::FlushInstructionCache(pc, pc_size);
}

void CodeGenerator::BackpatchBranch(void* pc, u32 pc_size, void* target)
{
  Log_ProfilePrintf("Backpatching %p to %p [branch]", pc, target);

  vixl::aarch32::MacroAssembler emit(static_cast<vixl::byte*>(pc), pc_size, a32::A32);

  // check jump distance
  const s32 displacement = GetPCDisplacement(pc, target);
  if (!IsPCDisplacementInImmediateRange(displacement))
  {
    emit.Mov(GetHostReg32(RSCRATCH), reinterpret_cast<uintptr_t>(target));
    emit.bx(GetHostReg32(RSCRATCH));
  }
  else
  {
    a32::Label label(displacement + emit.GetCursorOffset());
    emit.b(&label);
  }

  // shouldn't have any nops
  const s32 nops = (static_cast<s32>(pc_size) - static_cast<s32>(emit.GetCursorOffset())) / 4;
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    emit.nop();

  JitCodeBuffer::FlushInstructionCache(pc, pc_size);
}

void CodeGenerator::EmitLoadGlobal(HostReg host_reg, RegSize size, const void* ptr)
{
  EmitLoadGlobalAddress(RSCRATCH, ptr);
  switch (size)
  {
    case RegSize_8:
      m_emit->Ldrb(GetHostReg8(host_reg), a32::MemOperand(GetHostReg32(RSCRATCH)));
      break;

    case RegSize_16:
      m_emit->Ldrh(GetHostReg16(host_reg), a32::MemOperand(GetHostReg32(RSCRATCH)));
      break;

    case RegSize_32:
      m_emit->Ldr(GetHostReg32(host_reg), a32::MemOperand(GetHostReg32(RSCRATCH)));
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
      m_emit->Strb(GetHostReg8(value_in_hr), a32::MemOperand(GetHostReg32(RSCRATCH)));
      break;

    case RegSize_16:
      m_emit->Strh(GetHostReg16(value_in_hr), a32::MemOperand(GetHostReg32(RSCRATCH)));
      break;

    case RegSize_32:
      m_emit->Str(GetHostReg32(value_in_hr), a32::MemOperand(GetHostReg32(RSCRATCH)));
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitFlushInterpreterLoadDelay()
{
  Value reg = Value::FromHostReg(&m_register_cache, 0, RegSize_32);
  Value value = Value::FromHostReg(&m_register_cache, 1, RegSize_32);

  const a32::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(State, load_delay_reg));
  const a32::MemOperand load_delay_value(GetCPUPtrReg(), offsetof(State, load_delay_value));
  const a32::MemOperand regs_base(GetCPUPtrReg(), offsetof(State, regs.r[0]));

  a32::Label skip_flush;

  // reg = load_delay_reg
  m_emit->Ldrb(GetHostReg32(reg), load_delay_reg);

  // if load_delay_reg == Reg::count goto skip_flush
  m_emit->Cmp(GetHostReg32(reg), static_cast<u8>(Reg::count));
  m_emit->B(a32::eq, &skip_flush);

  // value = load_delay_value
  m_emit->Ldr(GetHostReg32(value), load_delay_value);

  // reg = offset(r[0] + reg << 2)
  m_emit->Lsl(GetHostReg32(reg), GetHostReg32(reg), 2);
  m_emit->Add(GetHostReg32(reg), GetHostReg32(reg), offsetof(State, regs.r[0]));

  // r[reg] = value
  m_emit->Str(GetHostReg32(value), a32::MemOperand(GetCPUPtrReg(), GetHostReg32(reg)));

  // load_delay_reg = Reg::count
  m_emit->Mov(GetHostReg32(reg), static_cast<u8>(Reg::count));
  m_emit->Strb(GetHostReg32(reg), load_delay_reg);

  m_emit->Bind(&skip_flush);
}

void CodeGenerator::EmitMoveNextInterpreterLoadDelay()
{
  Value reg = Value::FromHostReg(&m_register_cache, 0, RegSize_32);
  Value value = Value::FromHostReg(&m_register_cache, 1, RegSize_32);

  const a32::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(State, load_delay_reg));
  const a32::MemOperand load_delay_value(GetCPUPtrReg(), offsetof(State, load_delay_value));
  const a32::MemOperand next_load_delay_reg(GetCPUPtrReg(), offsetof(State, next_load_delay_reg));
  const a32::MemOperand next_load_delay_value(GetCPUPtrReg(), offsetof(State, next_load_delay_value));

  m_emit->ldrb(GetHostReg32(reg), next_load_delay_reg);
  m_emit->ldr(GetHostReg32(value), next_load_delay_value);
  m_emit->strb(GetHostReg32(reg), load_delay_reg);
  m_emit->str(GetHostReg32(value), load_delay_value);
  m_emit->Mov(GetHostReg32(reg), static_cast<u8>(Reg::count));
  m_emit->strb(GetHostReg32(reg), next_load_delay_reg);
}

void CodeGenerator::EmitCancelInterpreterLoadDelayForReg(Reg reg)
{
  if (!m_load_delay_dirty)
    return;

  const a32::MemOperand load_delay_reg(GetCPUPtrReg(), offsetof(State, load_delay_reg));
  Value temp = Value::FromHostReg(&m_register_cache, RSCRATCH, RegSize_8);

  a32::Label skip_cancel;

  // if load_delay_reg != reg goto skip_cancel
  m_emit->ldrb(GetHostReg8(temp), load_delay_reg);
  m_emit->cmp(GetHostReg8(temp), static_cast<u8>(reg));
  m_emit->B(a32::ne, &skip_cancel);

  // load_delay_reg = Reg::count
  m_emit->Mov(GetHostReg8(temp), static_cast<u8>(Reg::count));
  m_emit->strb(GetHostReg8(temp), load_delay_reg);

  m_emit->Bind(&skip_cancel);
}

void CodeGenerator::EmitICacheCheckAndUpdate()
{
  if (GetSegmentForAddress(m_pc) >= Segment::KSEG1)
  {
    EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_block->uncached_fetch_ticks)));
  }
  else
  {
    const auto& ticks_reg = a32::r0;
    const auto& current_tag_reg = a32::r1;
    const auto& existing_tag_reg = a32::r2;

    VirtualMemoryAddress current_pc = m_pc & ICACHE_TAG_ADDRESS_MASK;
    m_emit->ldr(ticks_reg, a32::MemOperand(GetCPUPtrReg(), offsetof(State, pending_ticks)));
    m_emit->Mov(current_tag_reg, current_pc);

    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const TickCount fill_ticks = GetICacheFillTicks(current_pc);
      if (fill_ticks <= 0)
        continue;

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = offsetof(State, icache_tags) + (line * sizeof(u32));

      a32::Label cache_hit;
      m_emit->ldr(existing_tag_reg, a32::MemOperand(GetCPUPtrReg(), offset));
      m_emit->cmp(existing_tag_reg, current_tag_reg);
      m_emit->B(a32::eq, &cache_hit);

      m_emit->str(current_tag_reg, a32::MemOperand(GetCPUPtrReg(), offset));
      EmitAdd(0, 0, Value::FromConstantU32(static_cast<u32>(fill_ticks)), false);
      m_emit->Bind(&cache_hit);

      if (i != (m_block->icache_line_count - 1))
        m_emit->add(current_tag_reg, current_tag_reg, ICACHE_LINE_SIZE);
    }

    m_emit->str(ticks_reg, a32::MemOperand(GetCPUPtrReg(), offsetof(State, pending_ticks)));
  }
}

void CodeGenerator::EmitStallUntilGTEComplete()
{
  static_assert(offsetof(State, pending_ticks) + sizeof(u32) == offsetof(State, gte_completion_tick));

  m_emit->ldr(GetHostReg32(RARG1), a32::MemOperand(GetCPUPtrReg(), offsetof(State, pending_ticks)));
  m_emit->ldr(GetHostReg32(RARG2), a32::MemOperand(GetCPUPtrReg(), offsetof(State, gte_completion_tick)));

  if (m_delayed_cycles_add > 0)
  {
    m_emit->Add(GetHostReg32(RARG1), GetHostReg32(RARG1), static_cast<u32>(m_delayed_cycles_add));
    m_delayed_cycles_add = 0;
  }

  m_emit->cmp(GetHostReg32(RARG2), GetHostReg32(RARG1));
  m_emit->mov(a32::hi, GetHostReg32(RARG1), GetHostReg32(RARG2));
  m_emit->str(GetHostReg32(RARG1), a32::MemOperand(GetCPUPtrReg(), offsetof(State, pending_ticks)));
}

void CodeGenerator::EmitBranch(const void* address, bool allow_scratch)
{
  const s32 displacement = GetPCDisplacement(GetCurrentCodePointer(), address);
  if (IsPCDisplacementInImmediateRange(displacement))
  {
    a32::Label label(displacement + m_emit->GetCursorOffset());
    m_emit->b(&label);
    return;
  }

  m_emit->Mov(GetHostReg32(RSCRATCH), reinterpret_cast<uintptr_t>(address));
  m_emit->bx(GetHostReg32(RSCRATCH));
}

void CodeGenerator::EmitBranch(LabelType* label)
{
  m_emit->b(label);
}

static a32::Condition TranslateCondition(Condition condition, bool invert)
{
  switch (condition)
  {
    case Condition::Always:
      return a32::Condition::None();

    case Condition::NotEqual:
    case Condition::NotZero:
      return invert ? a32::eq : a32::ne;

    case Condition::Equal:
    case Condition::Zero:
      return invert ? a32::ne : a32::eq;

    case Condition::Overflow:
      return invert ? a32::vc : a32::vs;

    case Condition::Greater:
      return invert ? a32::le : a32::gt;

    case Condition::GreaterEqual:
      return invert ? a32::lt : a32::ge;

    case Condition::Less:
      return invert ? a32::ge : a32::lt;

    case Condition::LessEqual:
      return invert ? a32::gt : a32::le;

    case Condition::Negative:
      return invert ? a32::pl : a32::mi;

    case Condition::PositiveOrZero:
      return invert ? a32::mi : a32::pl;

    case Condition::Above:
      return invert ? a32::ls : a32::hi;

    case Condition::AboveEqual:
      return invert ? a32::cc : a32::cs;

    case Condition::Below:
      return invert ? a32::cs : a32::cc;

    case Condition::BelowEqual:
      return invert ? a32::hi : a32::ls;

    default:
      UnreachableCode();
      return a32::Condition::Never();
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
          m_emit->tst(GetHostReg8(value), GetHostReg8(value));
          m_emit->b(a32::ne, label);
          break;
        case RegSize_16:
          m_emit->tst(GetHostReg8(value), GetHostReg8(value));
          m_emit->b(a32::ne, label);
          break;
        case RegSize_32:
          m_emit->tst(GetHostReg8(value), GetHostReg8(value));
          m_emit->b(a32::ne, label);
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
          m_emit->tst(GetHostReg8(value), GetHostReg8(value));
          m_emit->b(a32::eq, label);
          break;
        case RegSize_16:
          m_emit->tst(GetHostReg8(value), GetHostReg8(value));
          m_emit->b(a32::eq, label);
          break;
        case RegSize_32:
          m_emit->tst(GetHostReg8(value), GetHostReg8(value));
          m_emit->b(a32::eq, label);
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
    m_emit->b(TranslateCondition(condition, invert), label);
}

void CodeGenerator::EmitBranchIfBitClear(HostReg reg, RegSize size, u8 bit, LabelType* label)
{
  switch (size)
  {
    case RegSize_8:
    case RegSize_16:
    case RegSize_32:
      m_emit->tst(GetHostReg32(reg), static_cast<s32>(1u << bit));
      m_emit->b(a32::eq, label);
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
  m_emit->Mov(GetHostReg32(host_reg), reinterpret_cast<uintptr_t>(ptr));
}

CodeCache::DispatcherFunction CodeGenerator::CompileDispatcher()
{
  m_emit->sub(a32::sp, a32::sp, FUNCTION_STACK_SIZE);
  m_register_cache.ReserveCalleeSavedRegisters();
  const u32 stack_adjust = PrepareStackForCall();

  EmitLoadGlobalAddress(RCPUPTR, &g_state);

  a32::Label frame_done_loop;
  a32::Label exit_dispatcher;
  m_emit->Bind(&frame_done_loop);

  // if frame_done goto exit_dispatcher
  m_emit->ldrb(a32::r0, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, frame_done)));
  m_emit->tst(a32::r0, 1);
  m_emit->b(a32::ne, &exit_dispatcher);

  // r0 <- sr
  a32::Label no_interrupt;
  m_emit->ldr(a32::r0, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, cop0_regs.sr.bits)));

  // if Iec == 0 then goto no_interrupt
  m_emit->tst(a32::r0, 1);
  m_emit->b(a32::eq, &no_interrupt);

  // r1 <- cause
  // r0 (sr) & cause
  m_emit->ldr(a32::r1, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, cop0_regs.cause.bits)));
  m_emit->and_(a32::r0, a32::r0, a32::r1);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  m_emit->tst(a32::r0, 0xFF00);
  m_emit->b(a32::eq, &no_interrupt);

  // we have an interrupt
  EmitCall(reinterpret_cast<const void*>(&DispatchInterrupt));

  // no interrupt or we just serviced it
  m_emit->Bind(&no_interrupt);

  // TimingEvents::UpdateCPUDowncount:
  // r0 <- head event->downcount
  // downcount <- r0
  EmitLoadGlobalAddress(0, TimingEvents::GetHeadEventPtr());
  m_emit->ldr(a32::r0, a32::MemOperand(a32::r0));
  m_emit->ldr(a32::r0, a32::MemOperand(a32::r0, offsetof(TimingEvent, m_downcount)));
  m_emit->str(a32::r0, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, downcount)));

  // main dispatch loop
  a32::Label main_loop;
  m_emit->Bind(&main_loop);
  s_dispatcher_return_address = GetCurrentCodePointer();

  // r0 <- pending_ticks
  // r1 <- downcount
  m_emit->ldr(a32::r0, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, pending_ticks)));
  m_emit->ldr(a32::r1, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, downcount)));

  // while downcount < pending_ticks
  a32::Label downcount_hit;
  m_emit->cmp(a32::r0, a32::r1);
  m_emit->b(a32::ge, &downcount_hit);

  // time to lookup the block
  // r0 <- pc
  m_emit->ldr(a32::r0, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, regs.pc)));

  // r1 <- s_fast_map[pc >> 16]
  EmitLoadGlobalAddress(2, CodeCache::GetFastMapPointer());
  m_emit->lsr(a32::r1, a32::r0, 16);
  m_emit->ldr(a32::r1, a32::MemOperand(a32::r2, a32::r1, a32::LSL, 2));

  // blr(r1[pc]) (fast_map[pc >> 2])
  m_emit->ldr(a32::r0, a32::MemOperand(a32::r1, a32::r0));
  m_emit->blx(a32::r0);

  // end while
  m_emit->Bind(&downcount_hit);

  // check events then for frame done
  m_emit->ldr(a32::r0, a32::MemOperand(GetHostReg32(RCPUPTR), offsetof(State, pending_ticks)));
  EmitLoadGlobalAddress(1, TimingEvents::GetHeadEventPtr());
  m_emit->ldr(a32::r1, a32::MemOperand(a32::r1));
  m_emit->ldr(a32::r1, a32::MemOperand(a32::r1, offsetof(TimingEvent, m_downcount)));
  m_emit->cmp(a32::r0, a32::r1);
  m_emit->b(a32::lt, &frame_done_loop);
  EmitCall(reinterpret_cast<const void*>(&TimingEvents::RunEvents));
  m_emit->b(&frame_done_loop);

  // all done
  m_emit->Bind(&exit_dispatcher);

  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->add(a32::sp, a32::sp, FUNCTION_STACK_SIZE);
  m_emit->bx(a32::lr);

  CodeBlock::HostCodePointer ptr;
  u32 code_size;
  FinalizeBlock(&ptr, &code_size);
  Log_DevPrintf("Dispatcher is %u bytes at %p", code_size, ptr);
  return reinterpret_cast<CodeCache::DispatcherFunction>(ptr);
}

CodeCache::SingleBlockDispatcherFunction CodeGenerator::CompileSingleBlockDispatcher()
{
  m_emit->sub(a32::sp, a32::sp, FUNCTION_STACK_SIZE);
  m_register_cache.ReserveCalleeSavedRegisters();
  const u32 stack_adjust = PrepareStackForCall();

  EmitLoadGlobalAddress(RCPUPTR, &g_state);

  m_emit->blx(GetHostReg32(RARG1));

  RestoreStackAfterCall(stack_adjust);
  m_register_cache.PopCalleeSavedRegisters(true);
  m_emit->add(a32::sp, a32::sp, FUNCTION_STACK_SIZE);
  m_emit->bx(a32::lr);

  CodeBlock::HostCodePointer ptr;
  u32 code_size;
  FinalizeBlock(&ptr, &code_size);
  Log_DevPrintf("Single block dispatcher is %u bytes at %p", code_size, ptr);
  return reinterpret_cast<CodeCache::SingleBlockDispatcherFunction>(ptr);
}

} // namespace CPU::Recompiler
