// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"

#include "cpu_code_cache_private.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#include "settings.h"
#include "timing_event.h"

#ifdef CPU_ARCH_ARM32

Log_SetChannel(CPU::Recompiler);

#ifdef ENABLE_HOST_DISASSEMBLY
#include "vixl/aarch32/disasm-aarch32.h"
#include <iostream>
#endif

namespace a32 = vixl::aarch32;

namespace CPU::Recompiler {
constexpr u32 FUNCTION_CALLEE_SAVED_SPACE_RESERVE = 80;  // 8 registers
constexpr u32 FUNCTION_CALLER_SAVED_SPACE_RESERVE = 144; // 18 registers -> 224 bytes
constexpr u32 FUNCTION_STACK_SIZE = FUNCTION_CALLEE_SAVED_SPACE_RESERVE + FUNCTION_CALLER_SAVED_SPACE_RESERVE;

static constexpr u32 TRAMPOLINE_AREA_SIZE = 4 * 1024;
static std::unordered_map<const void*, u32> s_trampoline_targets;
static u8* s_trampoline_start_ptr = nullptr;
static u32 s_trampoline_used = 0;
} // namespace CPU::Recompiler

bool CPU::Recompiler::armIsCallerSavedRegister(u32 id)
{
  return ((id >= 0 && id <= 3) ||  // r0-r3
          (id == 12 || id == 14)); // sp, pc
}

s32 CPU::Recompiler::armGetPCDisplacement(const void* current, const void* target)
{
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(current), 4));
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(target), 4));
  return static_cast<s32>((reinterpret_cast<ptrdiff_t>(target) - reinterpret_cast<ptrdiff_t>(current)));
}

bool CPU::Recompiler::armIsPCDisplacementInImmediateRange(s32 displacement)
{
  return (displacement >= -33554432 && displacement <= 33554428);
}

void CPU::Recompiler::armEmitMov(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& rd, u32 imm)
{
  if (vixl::IsUintN(16, imm))
  {
    armAsm->mov(vixl::aarch32::al, rd, imm & 0xffff);
    return;
  }

  armAsm->mov(vixl::aarch32::al, rd, imm & 0xffff);
  armAsm->movt(vixl::aarch32::al, rd, imm >> 16);
}

void CPU::Recompiler::armMoveAddressToReg(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& reg,
                                          const void* addr)
{
  armEmitMov(armAsm, reg, static_cast<u32>(reinterpret_cast<uintptr_t>(addr)));
}

void CPU::Recompiler::armEmitJmp(vixl::aarch32::Assembler* armAsm, const void* ptr, bool force_inline)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  s32 displacement = armGetPCDisplacement(cur, ptr);
  bool use_bx = !armIsPCDisplacementInImmediateRange(displacement);
  if (use_bx && !force_inline)
  {
    if (u8* trampoline = armGetJumpTrampoline(ptr); trampoline)
    {
      displacement = armGetPCDisplacement(cur, trampoline);
      use_bx = !armIsPCDisplacementInImmediateRange(displacement);
    }
  }

  if (use_bx)
  {
    armMoveAddressToReg(armAsm, RSCRATCH, ptr);
    armAsm->bx(RSCRATCH);
  }
  else
  {
    a32::Label label(displacement + armAsm->GetCursorOffset());
    armAsm->b(&label);
  }
}

void CPU::Recompiler::armEmitCall(vixl::aarch32::Assembler* armAsm, const void* ptr, bool force_inline)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  s32 displacement = armGetPCDisplacement(cur, ptr);
  bool use_blx = !armIsPCDisplacementInImmediateRange(displacement);
  if (use_blx && !force_inline)
  {
    if (u8* trampoline = armGetJumpTrampoline(ptr); trampoline)
    {
      displacement = armGetPCDisplacement(cur, trampoline);
      use_blx = !armIsPCDisplacementInImmediateRange(displacement);
    }
  }

  if (use_blx)
  {
    armMoveAddressToReg(armAsm, RSCRATCH, ptr);
    armAsm->blx(RSCRATCH);
  }
  else
  {
    a32::Label label(displacement + armAsm->GetCursorOffset());
    armAsm->bl(&label);
  }
}

void CPU::Recompiler::armEmitCondBranch(vixl::aarch32::Assembler* armAsm, vixl::aarch32::Condition cond, const void* ptr)
{
  const s32 displacement = armGetPCDisplacement(armAsm->GetCursorAddress<const void*>(), ptr);
  if (!armIsPCDisplacementInImmediateRange(displacement))
  {
    armMoveAddressToReg(armAsm, RSCRATCH, ptr);
    armAsm->blx(cond, RSCRATCH);
  }
  else
  {
    a32::Label label(displacement + armAsm->GetCursorOffset());
    armAsm->b(cond, &label);
  }
}

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  a32::PrintDisassembler dis(std::cout, 0);
  dis.SetCodeAddress(reinterpret_cast<uintptr_t>(start));
  dis.DisassembleA32Buffer(static_cast<const u32*>(start), size);
#else
  Log_ErrorPrint("Not compiled with ENABLE_HOST_DISASSEMBLY.");
#endif
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
  return size / a32::kA32InstructionSizeInBytes;
}

u32 CPU::CodeCache::EmitJump(void* code, const void* dst, bool flush_icache)
{
  using namespace vixl::aarch32;
  using namespace CPU::Recompiler;

  const s32 disp = armGetPCDisplacement(code, dst);
  DebugAssert(armIsPCDisplacementInImmediateRange(disp));

  // A32 jumps are silly.
  {
    vixl::aarch32::Assembler emit(static_cast<vixl::byte*>(code), kA32InstructionSizeInBytes, a32::A32);
    a32::Label label(disp);
    emit.b(&label);
  }

  if (flush_icache)
    JitCodeBuffer::FlushInstructionCache(code, kA32InstructionSizeInBytes);

  return kA32InstructionSizeInBytes;
}

u8* CPU::Recompiler::armGetJumpTrampoline(const void* target)
{
  auto it = s_trampoline_targets.find(target);
  if (it != s_trampoline_targets.end())
    return s_trampoline_start_ptr + it->second;

  // align to 16 bytes?
  const u32 offset = s_trampoline_used; // Common::AlignUpPow2(s_trampoline_used, 16);

  // 4 movs plus a jump
  if (TRAMPOLINE_AREA_SIZE - offset < 20)
  {
    Panic("Ran out of space in constant pool");
    return nullptr;
  }

  u8* start = s_trampoline_start_ptr + offset;
  a32::Assembler armAsm(start, TRAMPOLINE_AREA_SIZE - offset);
  armMoveAddressToReg(&armAsm, RSCRATCH, target);
  armAsm.bx(RSCRATCH);

  const u32 size = static_cast<u32>(armAsm.GetSizeOfCodeGenerated());
  DebugAssert(size < 20);
  s_trampoline_targets.emplace(target, offset);
  s_trampoline_used = offset + static_cast<u32>(size);

  JitCodeBuffer::FlushInstructionCache(start, size);
  return start;
}

u32 CPU::CodeCache::EmitASMFunctions(void* code, u32 code_size)
{
  using namespace vixl::aarch32;
  using namespace CPU::Recompiler;

#define PTR(x) a32::MemOperand(RSTATE, (s32)(((u8*)(x)) - ((u8*)&g_state)))

  Assembler actual_asm(static_cast<u8*>(code), code_size);
  Assembler* armAsm = &actual_asm;

#ifdef VIXL_DEBUG
  vixl::CodeBufferCheckScope asm_check(armAsm, code_size, vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
#endif

  Label dispatch;

  g_enter_recompiler = armAsm->GetCursorAddress<decltype(g_enter_recompiler)>();
  {
    // reserve some space for saving caller-saved registers
    armAsm->sub(sp, sp, FUNCTION_STACK_SIZE);

    // Need the CPU state for basically everything :-)
    armMoveAddressToReg(armAsm, RSTATE, &g_state);
  }

  // check events then for frame done
  g_check_events_and_dispatch = armAsm->GetCursorAddress<const void*>();
  {
    Label skip_event_check;
    armAsm->ldr(RARG1, PTR(&g_state.pending_ticks));
    armAsm->ldr(RARG2, PTR(&g_state.downcount));
    armAsm->cmp(RARG1, RARG2);
    armAsm->b(lt, &skip_event_check);

    g_run_events_and_dispatch = armAsm->GetCursorAddress<const void*>();
    armEmitCall(armAsm, reinterpret_cast<const void*>(&TimingEvents::RunEvents), true);

    armAsm->bind(&skip_event_check);
  }

  // TODO: align?
  g_dispatcher = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->bind(&dispatch);

    // x9 <- s_fast_map[pc >> 16]
    armAsm->ldr(RARG1, PTR(&g_state.pc));
    armMoveAddressToReg(armAsm, RARG3, g_code_lut.data());
    armAsm->lsr(RARG2, RARG1, 16);
    armAsm->ldr(RARG2, MemOperand(RARG3, RARG2, LSL, 2));

    // blr(x9[pc * 2]) (fast_map[pc >> 2])
    armAsm->ldr(RARG1, MemOperand(RARG2, RARG1));
    armAsm->blx(RARG1);
  }

  g_compile_or_revalidate_block = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->ldr(RARG1, PTR(&g_state.pc));
    armEmitCall(armAsm, reinterpret_cast<const void*>(&CompileOrRevalidateBlock), true);
    armAsm->b(&dispatch);
  }

  g_discard_and_recompile_block = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->ldr(RARG1, PTR(&g_state.pc));
    armEmitCall(armAsm, reinterpret_cast<const void*>(&DiscardAndRecompileBlock), true);
    armAsm->b(&dispatch);
  }

  g_interpret_block = armAsm->GetCursorAddress<const void*>();
  {
    armEmitCall(armAsm, reinterpret_cast<const void*>(GetInterpretUncachedBlockFunction()), true);
    armAsm->b(&dispatch);
  }

  armAsm->FinalizeCode();

#if 0
  // TODO: align?
  s_trampoline_targets.clear();
  s_trampoline_start_ptr = static_cast<u8*>(code) + armAsm->GetCursorOffset();
  s_trampoline_used = 0;
#endif

#undef PTR
  return static_cast<u32>(armAsm->GetCursorOffset()) /* + TRAMPOLINE_AREA_SIZE*/;
}

// Macros aren't used with old-rec.
#undef RRET
#undef RARG1
#undef RARG2
#undef RARG3
#undef RSCRATCH
#undef RSTATE

namespace CPU::Recompiler {

constexpr HostReg RCPUPTR = 4;
constexpr HostReg RMEMBASEPTR = 3;
constexpr HostReg RRETURN = 0;
constexpr HostReg RARG1 = 0;
constexpr HostReg RARG2 = 1;
constexpr HostReg RARG3 = 2;
constexpr HostReg RARG4 = 3;
constexpr HostReg RSCRATCH = 12;

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

static const a32::Register GetFastmemBasePtrReg()
{
  return GetHostReg32(RMEMBASEPTR);
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

void* CodeGenerator::GetStartNearCodePointer() const
{
  return static_cast<u8*>(m_code_buffer->GetFreeCodePointer());
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

void CodeGenerator::EmitEndBlock(bool free_registers /* = true */, const void* jump_to)
{
  if (free_registers)
  {
    m_register_cache.FreeHostReg(RCPUPTR);
    m_register_cache.FreeHostReg(14);
    m_register_cache.PopCalleeSavedRegisters(true);
  }

  if (jump_to)
    armEmitJmp(m_emit, jump_to, true);
}

void CodeGenerator::EmitExceptionExit()
{
  // ensure all unflushed registers are written back
  m_register_cache.FlushAllGuestRegisters(false, false);

  // the interpreter load delay might have its own value, but we'll overwrite it here anyway
  // technically RaiseException() and FlushPipeline() have already been called, but that should be okay
  m_register_cache.FlushLoadDelay(false);

  m_register_cache.PopCalleeSavedRegisters(false);

  armEmitJmp(m_emit, CodeCache::g_check_events_and_dispatch, true);
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

const void* CodeGenerator::FinalizeBlock(u32* out_host_code_size, u32* out_host_far_code_size)
{
  m_near_emitter.FinalizeCode();
  m_far_emitter.FinalizeCode();

  const void* code = m_code_buffer->GetFreeCodePointer();
  *out_host_code_size = static_cast<u32>(m_near_emitter.GetSizeOfCodeGenerated());
  *out_host_far_code_size = static_cast<u32>(m_far_emitter.GetSizeOfCodeGenerated());

  m_code_buffer->CommitCode(static_cast<u32>(m_near_emitter.GetSizeOfCodeGenerated()));
  m_code_buffer->CommitFarCode(static_cast<u32>(m_far_emitter.GetSizeOfCodeGenerated()));

  m_near_emitter = CodeEmitter(static_cast<vixl::byte*>(m_code_buffer->GetFreeCodePointer()),
                               m_code_buffer->GetFreeCodeSpace(), a32::A32);
  m_far_emitter = CodeEmitter(static_cast<vixl::byte*>(m_code_buffer->GetFreeFarCodePointer()),
                              m_code_buffer->GetFreeFarCodeSpace(), a32::A32);

  return code;
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
  // We could use GetValueInHostRegister() here, but we run out of registers...
  // Value lhs_in_reg = GetValueInHostRegister(lhs);
  // Value rhs_in_reg = GetValueInHostRegister(rhs);
  const HostReg lhs_in_reg = lhs.IsInHostRegister() ? lhs.GetHostRegister() : (EmitCopyValue(RARG1, lhs), RARG1);
  const HostReg rhs_in_reg = rhs.IsInHostRegister() ? rhs.GetHostRegister() : (EmitCopyValue(RARG2, rhs), RARG2);

  if (lhs.size < RegSize_64)
  {
    if (signed_multiply)
    {
      m_emit->smull(GetHostReg32(to_reg_lo), GetHostReg32(to_reg_hi), GetHostReg32(lhs_in_reg),
                    GetHostReg32(rhs_in_reg));
    }
    else
    {
      m_emit->umull(GetHostReg32(to_reg_lo), GetHostReg32(to_reg_hi), GetHostReg32(lhs_in_reg),
                    GetHostReg32(rhs_in_reg));
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
  m_register_cache.PushCallerSavedRegisters();
  m_membase_loaded = false;
  return 0;
}

void CodeGenerator::RestoreStackAfterCall(u32 adjust_size)
{
  m_register_cache.PopCallerSavedRegisters();
}

void CodeGenerator::EmitCall(const void* ptr)
{
  armEmitCall(m_emit, ptr, false);
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
  const a32::MemOperand addr(a32::sp, FUNCTION_STACK_SIZE - (position * 4));
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
  const a32::MemOperand addr(a32::sp, FUNCTION_STACK_SIZE - (position * 4));
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

void CodeGenerator::EnsureMembaseLoaded()
{
  if (m_membase_loaded)
    return;

  m_emit->Ldr(GetFastmemBasePtrReg(), a32::MemOperand(GetCPUPtrReg(), offsetof(State, fastmem_base)));
  m_membase_loaded = true;
}

void CodeGenerator::EmitUpdateFastmemBase()
{
  m_membase_loaded = false;
}

void CodeGenerator::EmitLoadGuestRAMFastmem(const Value& address, RegSize size, Value& result)
{
  EnsureMembaseLoaded();

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

  m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), Bus::FASTMEM_LUT_PAGE_SHIFT);
  m_emit->ldr(GetHostReg32(RARG1),
              a32::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(RARG1), a32::LSL, 2)); // pointer load

  switch (size)
  {
    case RegSize_8:
      m_emit->ldrb(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    case RegSize_16:
      m_emit->ldrh(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    case RegSize_32:
      m_emit->ldr(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitLoadGuestMemoryFastmem(Instruction instruction, const CodeCache::InstructionInfo& info,
                                               const Value& address, RegSize size, Value& result)
{
  EnsureMembaseLoaded();

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

  m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), Bus::FASTMEM_LUT_PAGE_SHIFT);
  m_emit->ldr(GetHostReg32(RARG1),
              a32::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(RARG1), a32::LSL, 2)); // pointer load

  m_register_cache.InhibitAllocation();

  void* host_pc = GetCurrentNearCodePointer();

  switch (size)
  {
    case RegSize_8:
      m_emit->ldrb(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    case RegSize_16:
      m_emit->ldrh(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    case RegSize_32:
      m_emit->ldr(GetHostReg32(result.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    default:
      UnreachableCode();
      break;
  }

  const u32 host_code_size =
    static_cast<u32>(static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(host_pc)));

  // generate slowmem fallback
  const void* host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();

  // we add the ticks *after* the add here, since we counted incorrectly, then correct for it below
  DebugAssert(m_delayed_cycles_add > 0);
  EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));
  m_delayed_cycles_add += Bus::RAM_READ_TICKS;

  EmitLoadGuestMemorySlowmem(instruction, info, address, size, result, true);

  EmitAddCPUStructField(offsetof(State, pending_ticks),
                        Value::FromConstantU32(static_cast<u32>(-m_delayed_cycles_add)));

  // return to the block code
  EmitBranch(GetCurrentNearCodePointer(), false);

  SwitchToNearCode();
  m_register_cache.UninhibitAllocation();

  CPU::CodeCache::AddLoadStoreInfo(host_pc, host_code_size, info.pc, host_slowmem_pc);
}

void CodeGenerator::EmitLoadGuestMemorySlowmem(Instruction instruction, const CodeCache::InstructionInfo& info,
                                               const Value& address, RegSize size, Value& result, bool in_far_code)
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
             static_cast<Exception>(0), info.is_branch_delay_slot, false, instruction.cop.cop_n)));
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

void CodeGenerator::EmitStoreGuestMemoryFastmem(Instruction instruction, const CodeCache::InstructionInfo& info,
                                                const Value& address, RegSize size, const Value& value)
{
  EnsureMembaseLoaded();

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

  m_emit->lsr(GetHostReg32(RARG1), GetHostReg32(address_reg), Bus::FASTMEM_LUT_PAGE_SHIFT);
  m_emit->ldr(GetHostReg32(RARG1),
              a32::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(RARG1), a32::LSL, 2)); // pointer load

  m_register_cache.InhibitAllocation();

  void* host_pc = GetCurrentNearCodePointer();

  switch (size)
  {
    case RegSize_8:
      m_emit->strb(GetHostReg32(actual_value.host_reg),
                   a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    case RegSize_16:
      m_emit->strh(GetHostReg32(actual_value.host_reg),
                   a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    case RegSize_32:
      m_emit->str(GetHostReg32(actual_value.host_reg), a32::MemOperand(GetHostReg32(RARG1), GetHostReg32(address_reg)));
      break;

    default:
      UnreachableCode();
      break;
  }

  const u32 host_code_size =
    static_cast<u32>(static_cast<ptrdiff_t>(static_cast<u8*>(GetCurrentNearCodePointer()) - static_cast<u8*>(host_pc)));

  // generate slowmem fallback
  void* host_slowmem_pc = GetCurrentFarCodePointer();
  SwitchToFarCode();

  DebugAssert(m_delayed_cycles_add > 0);
  EmitAddCPUStructField(offsetof(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));

  EmitStoreGuestMemorySlowmem(instruction, info, address, size, actual_value, true);

  EmitAddCPUStructField(offsetof(State, pending_ticks),
                        Value::FromConstantU32(static_cast<u32>(-m_delayed_cycles_add)));

  // return to the block code
  EmitBranch(GetCurrentNearCodePointer(), false);

  SwitchToNearCode();
  m_register_cache.UninhibitAllocation();

  CPU::CodeCache::AddLoadStoreInfo(host_pc, host_code_size, info.pc, host_slowmem_pc);
}

void CodeGenerator::EmitStoreGuestMemorySlowmem(Instruction instruction, const CodeCache::InstructionInfo& info,
                                                const Value& address, RegSize size, const Value& value,
                                                bool in_far_code)
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
             static_cast<Exception>(0), info.is_branch_delay_slot, false, instruction.cop.cop_n)));
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

void CodeGenerator::BackpatchLoadStore(void* host_pc, const CodeCache::LoadstoreBackpatchInfo& lbi)
{
  Log_DevFmt("Backpatching {} (guest PC 0x{:08X}) to slowmem at {}", host_pc, lbi.guest_pc, lbi.thunk_address);

  // turn it into a jump to the slowmem handler
  vixl::aarch32::MacroAssembler emit(static_cast<vixl::byte*>(host_pc), lbi.code_size, a32::A32);

  // check jump distance
  const s32 displacement = armGetPCDisplacement(host_pc, lbi.thunk_address);
  if (!armIsPCDisplacementInImmediateRange(displacement))
  {
    armMoveAddressToReg(&emit, GetHostReg32(RSCRATCH), lbi.thunk_address);
    emit.bx(GetHostReg32(RSCRATCH));
  }
  else
  {
    a32::Label label(displacement + emit.GetCursorOffset());
    emit.b(&label);
  }

  const s32 nops = (static_cast<s32>(lbi.code_size) - static_cast<s32>(emit.GetCursorOffset())) / 4;
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    emit.nop();

  JitCodeBuffer::FlushInstructionCache(host_pc, lbi.code_size);
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
    EmitAddCPUStructField(offsetof(State, pending_ticks),
                          Value::FromConstantU32(static_cast<u32>(m_block->uncached_fetch_ticks)));
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

void CodeGenerator::EmitBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size)
{
  // store it first to reduce code size, because we can offset
  armMoveAddressToReg(m_emit, GetHostReg32(RARG1), ram_ptr);
  armMoveAddressToReg(m_emit, GetHostReg32(RARG2), shadow_ptr);

  u32 offset = 0;
  a32::Label block_changed;

#if 0
  /* TODO: Vectorize
#include <arm_neon.h>
#include <stdint.h>

bool foo(const void* a, const void* b)
{
    uint8x16_t v1 = vld1q_u8((const uint8_t*)a);
    uint8x16_t v2 = vld1q_u8((const uint8_t*)b);
    uint8x16_t v3 = vld1q_u8((const uint8_t*)a + 16);
    uint8x16_t v4 = vld1q_u8((const uint8_t*)a + 16);
    uint8x16_t r = vceqq_u8(v1, v2);
    uint8x16_t r2 = vceqq_u8(v2, v3);
    uint8x16_t r3 = vandq_u8(r, r2);
    uint32x2_t rr = vpmin_u32(vget_low_u32(vreinterpretq_u32_u8(r3)), vget_high_u32(vreinterpretq_u32_u8(r3)));
    if ((vget_lane_u32(rr, 0) & vget_lane_u32(rr, 1)) != 0xFFFFFFFFu)
        return false;
    else
        return true;
}
*/
  bool first = true;

  while (size >= 16)
  {
    const a32::VRegister vtmp = a32::v2.V4S();
    const a32::VRegister dst = first ? a32::v0.V4S() : a32::v1.V4S();
    m_emit->ldr(dst, a32::MemOperand(RXARG1, offset));
    m_emit->ldr(vtmp, a32::MemOperand(RXARG2, offset));
    m_emit->cmeq(dst, dst, vtmp);
    if (!first)
      m_emit->and_(dst.V16B(), dst.V16B(), vtmp.V16B());
    else
      first = false;

    offset += 16;
    size -= 16;
  }

  if (!first)
  {
    // TODO: make sure this doesn't choke on ffffffff
    m_emit->uminv(a32::s0, a32::v0.V4S());
    m_emit->fcmp(a32::s0, 0.0);
    m_emit->b(&block_changed, a32::eq);
  }
#endif

  while (size >= 4)
  {
    m_emit->ldr(GetHostReg32(RARG3), a32::MemOperand(GetHostReg32(RARG1), offset));
    m_emit->ldr(GetHostReg32(RARG4), a32::MemOperand(GetHostReg32(RARG2), offset));
    m_emit->cmp(GetHostReg32(RARG3), GetHostReg32(RARG4));
    m_emit->b(a32::ne, &block_changed);
    offset += 4;
    size -= 4;
  }

  DebugAssert(size == 0);

  a32::Label block_unchanged;
  m_emit->b(&block_unchanged);
  m_emit->bind(&block_changed);
  armEmitJmp(m_emit, CodeCache::g_discard_and_recompile_block, false);
  m_emit->bind(&block_unchanged);
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
  const s32 displacement = armGetPCDisplacement(GetCurrentCodePointer(), address);
  if (armIsPCDisplacementInImmediateRange(displacement))
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

} // namespace CPU::Recompiler

#endif // CPU_ARCH_ARM32
