// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_recompiler_arm32.h"
#include "cpu_core_private.h"
#include "cpu_pgxp.h"
#include "gte.h"
#include "settings.h"
#include "timing_event.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/memmap.h"
#include "common/string_util.h"

#include <limits>

#ifdef CPU_ARCH_ARM32

#include "vixl/aarch32/constants-aarch32.h"
#include "vixl/aarch32/instructions-aarch32.h"

#ifdef ENABLE_HOST_DISASSEMBLY
#include "vixl/aarch32/disasm-aarch32.h"
#include <iostream>
#endif

LOG_CHANNEL(Recompiler);

#define PTR(x) vixl::aarch32::MemOperand(RSTATE, (((u8*)(x)) - ((u8*)&g_state)))
#define RMEMBASE vixl::aarch32::r3

static constexpr u32 FUNCTION_CALLEE_SAVED_SPACE_RESERVE = 80;  // 8 registers
static constexpr u32 FUNCTION_CALLER_SAVED_SPACE_RESERVE = 144; // 18 registers -> 224 bytes
static constexpr u32 FUNCTION_STACK_SIZE = FUNCTION_CALLEE_SAVED_SPACE_RESERVE + FUNCTION_CALLER_SAVED_SPACE_RESERVE;

#define RRET vixl::aarch32::r0
#define RRETHI vixl::aarch32::r1
#define RARG1 vixl::aarch32::r0
#define RARG2 vixl::aarch32::r1
#define RARG3 vixl::aarch32::r2
#define RSCRATCH vixl::aarch32::r12
#define RSTATE vixl::aarch32::r4

static bool armIsCallerSavedRegister(u32 id);
static s32 armGetPCDisplacement(const void* current, const void* target);
static bool armIsPCDisplacementInImmediateRange(s32 displacement);
static void armMoveAddressToReg(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& reg, const void* addr);
static void armEmitMov(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& rd, u32 imm);
static void armEmitJmp(vixl::aarch32::Assembler* armAsm, const void* ptr, bool force_inline);
static void armEmitCall(vixl::aarch32::Assembler* armAsm, const void* ptr, bool force_inline);
static void armEmitCondBranch(vixl::aarch32::Assembler* armAsm, vixl::aarch32::Condition cond, const void* ptr);
static void armEmitFarLoad(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& reg, const void* addr);
static void armEmitFarStore(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& reg, const void* addr,
                            const vixl::aarch32::Register& tempreg = RSCRATCH);
static u8* armGetJumpTrampoline(const void* target);

static constexpr u32 TRAMPOLINE_AREA_SIZE = 4 * 1024;
static std::unordered_map<const void*, u32> s_trampoline_targets;
static u8* s_trampoline_start_ptr = nullptr;
static u32 s_trampoline_used = 0;

namespace CPU {

using namespace vixl::aarch32;

static ARM32Recompiler s_instance;
Recompiler* g_compiler = &s_instance;

} // namespace CPU

bool armIsCallerSavedRegister(u32 id)
{
  return ((id >= 0 && id <= 3) ||  // r0-r3
          (id == 12 || id == 14)); // sp, pc
}

s32 armGetPCDisplacement(const void* current, const void* target)
{
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(current), 4));
  Assert(Common::IsAlignedPow2(reinterpret_cast<size_t>(target), 4));
  return static_cast<s32>((reinterpret_cast<ptrdiff_t>(target) - reinterpret_cast<ptrdiff_t>(current)));
}

bool armIsPCDisplacementInImmediateRange(s32 displacement)
{
  return (displacement >= -33554432 && displacement <= 33554428);
}

void armEmitMov(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& rd, u32 imm)
{
  if (vixl::IsUintN(16, imm))
  {
    armAsm->mov(vixl::aarch32::al, rd, imm & 0xffff);
    return;
  }

  armAsm->mov(vixl::aarch32::al, rd, imm & 0xffff);
  armAsm->movt(vixl::aarch32::al, rd, imm >> 16);
}

void armMoveAddressToReg(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& reg, const void* addr)
{
  armEmitMov(armAsm, reg, static_cast<u32>(reinterpret_cast<uintptr_t>(addr)));
}

void armEmitJmp(vixl::aarch32::Assembler* armAsm, const void* ptr, bool force_inline)
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
    vixl::aarch32::Label label(displacement + armAsm->GetCursorOffset());
    armAsm->b(&label);
  }
}

void armEmitCall(vixl::aarch32::Assembler* armAsm, const void* ptr, bool force_inline)
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
    vixl::aarch32::Label label(displacement + armAsm->GetCursorOffset());
    armAsm->bl(&label);
  }
}

void armEmitCondBranch(vixl::aarch32::Assembler* armAsm, vixl::aarch32::Condition cond, const void* ptr)
{
  const s32 displacement = armGetPCDisplacement(armAsm->GetCursorAddress<const void*>(), ptr);
  if (!armIsPCDisplacementInImmediateRange(displacement))
  {
    armMoveAddressToReg(armAsm, RSCRATCH, ptr);
    armAsm->blx(cond, RSCRATCH);
  }
  else
  {
    vixl::aarch32::Label label(displacement + armAsm->GetCursorOffset());
    armAsm->b(cond, &label);
  }
}

void armEmitFarLoad(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& reg, const void* addr)
{
  armMoveAddressToReg(armAsm, reg, addr);
  armAsm->ldr(reg, vixl::aarch32::MemOperand(reg));
}

void armEmitFarStore(vixl::aarch32::Assembler* armAsm, const vixl::aarch32::Register& reg, const void* addr,
                     const vixl::aarch32::Register& tempreg)
{
  armMoveAddressToReg(armAsm, tempreg, addr);
  armAsm->str(reg, vixl::aarch32::MemOperand(tempreg));
}

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  vixl::aarch32::PrintDisassembler dis(std::cout, 0);
  dis.SetCodeAddress(reinterpret_cast<uintptr_t>(start));
  dis.DisassembleA32Buffer(static_cast<const u32*>(start), size);
#else
  ERROR_LOG("Not compiled with ENABLE_HOST_DISASSEMBLY.");
#endif
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
  return size / vixl::aarch32::kA32InstructionSizeInBytes;
}

u32 CPU::CodeCache::EmitJump(void* code, const void* dst, bool flush_icache)
{
  using namespace vixl::aarch32;

  const s32 disp = armGetPCDisplacement(code, dst);
  DebugAssert(armIsPCDisplacementInImmediateRange(disp));

  // A32 jumps are silly.
  {
    Assembler emit(static_cast<vixl::byte*>(code), kA32InstructionSizeInBytes, A32);
    Label label(disp);
    emit.b(&label);
  }

  if (flush_icache)
    MemMap::FlushInstructionCache(code, kA32InstructionSizeInBytes);

  return kA32InstructionSizeInBytes;
}

u8* armGetJumpTrampoline(const void* target)
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
  vixl::aarch32::Assembler armAsm(start, TRAMPOLINE_AREA_SIZE - offset);
  armMoveAddressToReg(&armAsm, RSCRATCH, target);
  armAsm.bx(RSCRATCH);

  const u32 size = static_cast<u32>(armAsm.GetSizeOfCodeGenerated());
  DebugAssert(size < 20);
  s_trampoline_targets.emplace(target, offset);
  s_trampoline_used = offset + static_cast<u32>(size);

  MemMap::FlushInstructionCache(start, size);
  return start;
}

u32 CPU::CodeCache::EmitASMFunctions(void* code, u32 code_size)
{
  using namespace vixl::aarch32;

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

  return static_cast<u32>(armAsm->GetCursorOffset()) /* + TRAMPOLINE_AREA_SIZE*/;
}

CPU::ARM32Recompiler::ARM32Recompiler() : m_emitter(A32), m_far_emitter(A32)
{
}

CPU::ARM32Recompiler::~ARM32Recompiler() = default;

const void* CPU::ARM32Recompiler::GetCurrentCodePointer()
{
  return armAsm->GetCursorAddress<const void*>();
}

void CPU::ARM32Recompiler::Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space, u8* far_code_buffer,
                                 u32 far_code_space)
{
  Recompiler::Reset(block, code_buffer, code_buffer_space, far_code_buffer, far_code_space);

  // TODO: don't recreate this every time..
  DebugAssert(!armAsm);
  m_emitter.GetBuffer()->Reset(code_buffer, code_buffer_space);
  m_far_emitter.GetBuffer()->Reset(far_code_buffer, far_code_space);
  armAsm = &m_emitter;

#ifdef VIXL_DEBUG
  m_emitter_check = std::make_unique<vixl::CodeBufferCheckScope>(m_emitter.get(), code_buffer_space,
                                                                 vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
  m_far_emitter_check = std::make_unique<vixl::CodeBufferCheckScope>(
    m_far_emitter.get(), far_code_space, vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
#endif

  // Need to wipe it out so it's correct when toggling fastmem.
  m_host_regs = {};

  const u32 membase_idx =
    (CodeCache::IsUsingFastmem() && block->HasFlag(CodeCache::BlockFlags::ContainsLoadStoreInstructions)) ?
      RMEMBASE.GetCode() :
      NUM_HOST_REGS;
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];

    if (i == RARG1.GetCode() || i == RARG2.GetCode() || i == RARG3.GetCode() || i == RSCRATCH.GetCode() ||
        i == RSTATE.GetCode() || i == membase_idx || i == sp.GetCode() || i == pc.GetCode())
    {
      continue;
    }

    ra.flags = HR_USABLE | (armIsCallerSavedRegister(i) ? 0 : HR_CALLEE_SAVED);
  }
}

void CPU::ARM32Recompiler::SwitchToFarCode(bool emit_jump, vixl::aarch32::ConditionType cond)
{
  DebugAssert(armAsm == &m_emitter);
  if (emit_jump)
  {
    const s32 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>());
    if (armIsPCDisplacementInImmediateRange(disp))
    {
      Label ldisp(armAsm->GetCursorOffset() + disp);
      armAsm->b(cond, &ldisp);
    }
    else if (cond != vixl::aarch32::al)
    {
      Label skip;
      armAsm->b(Condition(cond).Negate(), &skip);
      armEmitJmp(armAsm, m_far_emitter.GetCursorAddress<const void*>(), true);
      armAsm->bind(&skip);
    }
    else
    {
      armEmitJmp(armAsm, m_far_emitter.GetCursorAddress<const void*>(), true);
    }
  }
  armAsm = &m_far_emitter;
}

void CPU::ARM32Recompiler::SwitchToFarCodeIfBitSet(const vixl::aarch32::Register& reg, u32 bit)
{
  armAsm->tst(reg, 1u << bit);

  const s32 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>());
  if (armIsPCDisplacementInImmediateRange(disp))
  {
    Label ldisp(armAsm->GetCursorOffset() + disp);
    armAsm->b(ne, &ldisp);
  }
  else
  {
    Label skip;
    armAsm->b(eq, &skip);
    armEmitJmp(armAsm, m_far_emitter.GetCursorAddress<const void*>(), true);
    armAsm->bind(&skip);
  }

  armAsm = &m_far_emitter;
}

void CPU::ARM32Recompiler::SwitchToFarCodeIfRegZeroOrNonZero(const vixl::aarch32::Register& reg, bool nonzero)
{
  armAsm->cmp(reg, 0);

  const s32 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>());
  if (armIsPCDisplacementInImmediateRange(disp))
  {
    Label ldisp(armAsm->GetCursorOffset() + disp);
    nonzero ? armAsm->b(ne, &ldisp) : armAsm->b(eq, &ldisp);
  }
  else
  {
    Label skip;
    nonzero ? armAsm->b(eq, &skip) : armAsm->b(ne, &skip);
    armEmitJmp(armAsm, m_far_emitter.GetCursorAddress<const void*>(), true);
    armAsm->bind(&skip);
  }

  armAsm = &m_far_emitter;
}

void CPU::ARM32Recompiler::SwitchToNearCode(bool emit_jump, vixl::aarch32::ConditionType cond)
{
  DebugAssert(armAsm == &m_far_emitter);
  if (emit_jump)
  {
    const s32 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_emitter.GetCursorAddress<const void*>());
    if (armIsPCDisplacementInImmediateRange(disp))
    {
      Label ldisp(armAsm->GetCursorOffset() + disp);
      armAsm->b(cond, &ldisp);
    }
    else if (cond != vixl::aarch32::al)
    {
      Label skip;
      armAsm->b(Condition(cond).Negate(), &skip);
      armEmitJmp(armAsm, m_far_emitter.GetCursorAddress<const void*>(), true);
      armAsm->bind(&skip);
    }
    else
    {
      armEmitJmp(armAsm, m_far_emitter.GetCursorAddress<const void*>(), true);
    }
  }
  armAsm = &m_emitter;
}

void CPU::ARM32Recompiler::EmitMov(const vixl::aarch32::Register& dst, u32 val)
{
  armEmitMov(armAsm, dst, val);
}

void CPU::ARM32Recompiler::EmitCall(const void* ptr, bool force_inline /*= false*/)
{
  armEmitCall(armAsm, ptr, force_inline);
}

vixl::aarch32::Operand CPU::ARM32Recompiler::armCheckAddSubConstant(s32 val)
{
  if (ImmediateA32::IsImmediateA32(static_cast<u32>(val)))
    return vixl::aarch32::Operand(static_cast<int32_t>(val));

  EmitMov(RSCRATCH, static_cast<u32>(val));
  return vixl::aarch32::Operand(RSCRATCH);
}

vixl::aarch32::Operand CPU::ARM32Recompiler::armCheckAddSubConstant(u32 val)
{
  return armCheckAddSubConstant(static_cast<s32>(val));
}

vixl::aarch32::Operand CPU::ARM32Recompiler::armCheckCompareConstant(s32 val)
{
  return armCheckAddSubConstant(val);
}

vixl::aarch32::Operand CPU::ARM32Recompiler::armCheckLogicalConstant(u32 val)
{
  return armCheckAddSubConstant(val);
}

void CPU::ARM32Recompiler::BeginBlock()
{
  Recompiler::BeginBlock();
}

void CPU::ARM32Recompiler::GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size)
{
  // store it first to reduce code size, because we can offset
  armMoveAddressToReg(armAsm, RARG1, ram_ptr);
  armMoveAddressToReg(armAsm, RARG2, shadow_ptr);

  u32 offset = 0;
  Label block_changed;

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
    const VRegister vtmp = a32::v2.V4S();
    const VRegister dst = first ? a32::v0.V4S() : a32::v1.V4S();
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
    armAsm->uminv(a32::s0, a32::v0.V4S());
    armAsm->fcmp(a32::s0, 0.0);
    armAsm->b(&block_changed, a32::eq);
  }
#endif

  while (size >= 4)
  {
    armAsm->ldr(RARG3, MemOperand(RARG1, offset));
    armAsm->ldr(RSCRATCH, MemOperand(RARG2, offset));
    armAsm->cmp(RARG3, RSCRATCH);
    armAsm->b(ne, &block_changed);
    offset += 4;
    size -= 4;
  }

  DebugAssert(size == 0);

  Label block_unchanged;
  armAsm->b(&block_unchanged);
  armAsm->bind(&block_changed);
  armEmitJmp(armAsm, CodeCache::g_discard_and_recompile_block, false);
  armAsm->bind(&block_unchanged);
}

void CPU::ARM32Recompiler::GenerateICacheCheckAndUpdate()
{
  if (!m_block->HasFlag(CodeCache::BlockFlags::IsUsingICache))
  {
    if (m_block->HasFlag(CodeCache::BlockFlags::NeedsDynamicFetchTicks))
    {
      armEmitFarLoad(armAsm, RARG2, GetFetchMemoryAccessTimePtr());
      armAsm->ldr(RARG1, PTR(&g_state.pending_ticks));
      armEmitMov(armAsm, RARG3, m_block->size);
      armAsm->mul(RARG2, RARG2, RARG3);
      armAsm->add(RARG1, RARG1, RARG2);
      armAsm->str(RARG1, PTR(&g_state.pending_ticks));
    }
    else
    {
      armAsm->ldr(RARG1, PTR(&g_state.pending_ticks));
      armAsm->add(RARG1, RARG1, armCheckAddSubConstant(static_cast<u32>(m_block->uncached_fetch_ticks)));
      armAsm->str(RARG1, PTR(&g_state.pending_ticks));
    }
  }
  else if (m_block->icache_line_count > 0)
  {
    const auto& ticks_reg = RARG1;
    const auto& current_tag_reg = RARG2;
    const auto& existing_tag_reg = RARG3;

    VirtualMemoryAddress current_pc = m_block->pc & ICACHE_TAG_ADDRESS_MASK;
    armAsm->ldr(ticks_reg, PTR(&g_state.pending_ticks));
    armEmitMov(armAsm, current_tag_reg, current_pc);

    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const TickCount fill_ticks = GetICacheFillTicks(current_pc);
      if (fill_ticks <= 0)
        continue;

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = OFFSETOF(State, icache_tags) + (line * sizeof(u32));

      Label cache_hit;
      armAsm->ldr(existing_tag_reg, MemOperand(RSTATE, offset));
      armAsm->cmp(existing_tag_reg, current_tag_reg);
      armAsm->b(eq, &cache_hit);

      armAsm->str(current_tag_reg, MemOperand(RSTATE, offset));
      armAsm->add(ticks_reg, ticks_reg, armCheckAddSubConstant(static_cast<u32>(fill_ticks)));
      armAsm->bind(&cache_hit);

      if (i != (m_block->icache_line_count - 1))
        armAsm->add(current_tag_reg, current_tag_reg, armCheckAddSubConstant(ICACHE_LINE_SIZE));
    }

    armAsm->str(ticks_reg, PTR(&g_state.pending_ticks));
  }
}

void CPU::ARM32Recompiler::GenerateCall(const void* func, s32 arg1reg /*= -1*/, s32 arg2reg /*= -1*/,
                                        s32 arg3reg /*= -1*/)
{
  if (arg1reg >= 0 && arg1reg != static_cast<s32>(RARG1.GetCode()))
    armAsm->mov(RARG1, Register(arg1reg));
  if (arg2reg >= 0 && arg2reg != static_cast<s32>(RARG2.GetCode()))
    armAsm->mov(RARG2, Register(arg2reg));
  if (arg3reg >= 0 && arg3reg != static_cast<s32>(RARG3.GetCode()))
    armAsm->mov(RARG3, Register(arg3reg));
  EmitCall(func);
}

void CPU::ARM32Recompiler::EndBlock(const std::optional<u32>& newpc, bool do_event_test)
{
  if (newpc.has_value())
  {
    if (m_dirty_pc || m_compiler_pc != newpc)
    {
      EmitMov(RSCRATCH, newpc.value());
      armAsm->str(RSCRATCH, PTR(&g_state.pc));
    }
  }
  m_dirty_pc = false;

  // flush regs
  Flush(FLUSH_END_BLOCK);
  EndAndLinkBlock(newpc, do_event_test, false);
}

void CPU::ARM32Recompiler::EndBlockWithException(Exception excode)
{
  // flush regs, but not pc, it's going to get overwritten
  // flush cycles because of the GTE instruction stuff...
  Flush(FLUSH_END_BLOCK | FLUSH_FOR_EXCEPTION | FLUSH_FOR_C_CALL);

  // TODO: flush load delay
  // TODO: break for pcdrv

  EmitMov(RARG1, Cop0Registers::CAUSE::MakeValueForException(excode, m_current_instruction_branch_delay_slot, false,
                                                             inst->cop.cop_n));
  EmitMov(RARG2, m_current_instruction_pc);
  EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
  m_dirty_pc = false;

  EndAndLinkBlock(std::nullopt, true, false);
}

void CPU::ARM32Recompiler::EndAndLinkBlock(const std::optional<u32>& newpc, bool do_event_test, bool force_run_events)
{
  // event test
  // pc should've been flushed
  DebugAssert(!m_dirty_pc && !m_block_ended);
  m_block_ended = true;

  // TODO: try extracting this to a function

  // save cycles for event test
  const TickCount cycles = std::exchange(m_cycles, 0);

  // pending_ticks += cycles
  // if (pending_ticks >= downcount) { dispatch_event(); }
  if (do_event_test || m_gte_done_cycle > cycles || cycles > 0)
    armAsm->ldr(RARG1, PTR(&g_state.pending_ticks));
  if (do_event_test)
    armAsm->ldr(RARG2, PTR(&g_state.downcount));
  if (cycles > 0)
    armAsm->add(RARG1, RARG1, armCheckAddSubConstant(cycles));
  if (m_gte_done_cycle > cycles)
  {
    armAsm->add(RARG2, RARG1, armCheckAddSubConstant(m_gte_done_cycle - cycles));
    armAsm->str(RARG2, PTR(&g_state.gte_completion_tick));
  }
  if (do_event_test)
    armAsm->cmp(RARG1, RARG2);
  if (cycles > 0)
    armAsm->str(RARG1, PTR(&g_state.pending_ticks));
  if (do_event_test)
    armEmitCondBranch(armAsm, ge, CodeCache::g_run_events_and_dispatch);

  // jump to dispatcher or next block
  if (force_run_events)
  {
    armEmitJmp(armAsm, CodeCache::g_run_events_and_dispatch, false);
  }
  else if (!newpc.has_value())
  {
    armEmitJmp(armAsm, CodeCache::g_dispatcher, false);
  }
  else
  {
    if (newpc.value() == m_block->pc)
    {
      // Special case: ourselves! No need to backlink then.
      DEBUG_LOG("Linking block at {:08X} to self", m_block->pc);
      armEmitJmp(armAsm, armAsm->GetBuffer()->GetStartAddress<const void*>(), true);
    }
    else
    {
      const void* target = CodeCache::CreateBlockLink(m_block, armAsm->GetCursorAddress<void*>(), newpc.value());
      armEmitJmp(armAsm, target, true);
    }
  }
}

const void* CPU::ARM32Recompiler::EndCompile(u32* code_size, u32* far_code_size)
{
#ifdef VIXL_DEBUG
  m_emitter_check.reset();
  m_far_emitter_check.reset();
#endif

  m_emitter.FinalizeCode();
  m_far_emitter.FinalizeCode();

  u8* const code = m_emitter.GetBuffer()->GetStartAddress<u8*>();
  *code_size = static_cast<u32>(m_emitter.GetCursorOffset());
  *far_code_size = static_cast<u32>(m_far_emitter.GetCursorOffset());
  armAsm = nullptr;
  return code;
}

const char* CPU::ARM32Recompiler::GetHostRegName(u32 reg) const
{
  static constexpr std::array<const char*, 32> reg64_names = {
    {"x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
     "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "fp",  "lr",  "sp"}};
  return (reg < reg64_names.size()) ? reg64_names[reg] : "UNKNOWN";
}

void CPU::ARM32Recompiler::LoadHostRegWithConstant(u32 reg, u32 val)
{
  EmitMov(Register(reg), val);
}

void CPU::ARM32Recompiler::LoadHostRegFromCPUPointer(u32 reg, const void* ptr)
{
  armAsm->ldr(Register(reg), PTR(ptr));
}

void CPU::ARM32Recompiler::StoreHostRegToCPUPointer(u32 reg, const void* ptr)
{
  armAsm->str(Register(reg), PTR(ptr));
}

void CPU::ARM32Recompiler::StoreConstantToCPUPointer(u32 val, const void* ptr)
{
  EmitMov(RSCRATCH, val);
  armAsm->str(RSCRATCH, PTR(ptr));
}

void CPU::ARM32Recompiler::CopyHostReg(u32 dst, u32 src)
{
  if (src != dst)
    armAsm->mov(Register(dst), Register(src));
}

void CPU::ARM32Recompiler::AssertRegOrConstS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s || cf.const_s);
}

void CPU::ARM32Recompiler::AssertRegOrConstT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t || cf.const_t);
}

vixl::aarch32::MemOperand CPU::ARM32Recompiler::MipsPtr(Reg r) const
{
  DebugAssert(r < Reg::count);
  return PTR(&g_state.regs.r[static_cast<u32>(r)]);
}

vixl::aarch32::Register CPU::ARM32Recompiler::CFGetRegD(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_d);
  return Register(cf.host_d);
}

vixl::aarch32::Register CPU::ARM32Recompiler::CFGetRegS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s);
  return Register(cf.host_s);
}

vixl::aarch32::Register CPU::ARM32Recompiler::CFGetRegT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t);
  return Register(cf.host_t);
}

vixl::aarch32::Register CPU::ARM32Recompiler::CFGetRegLO(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_lo);
  return Register(cf.host_lo);
}

vixl::aarch32::Register CPU::ARM32Recompiler::CFGetRegHI(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_hi);
  return Register(cf.host_hi);
}

vixl::aarch32::Register CPU::ARM32Recompiler::GetMembaseReg()
{
  const u32 code = RMEMBASE.GetCode();
  if (!IsHostRegAllocated(code))
  {
    // Leave usable unset, so we don't try to allocate it later.
    m_host_regs[code].type = HR_TYPE_MEMBASE;
    m_host_regs[code].flags = HR_ALLOCATED;
    armAsm->ldr(RMEMBASE, PTR(&g_state.fastmem_base));
  }

  return RMEMBASE;
}

void CPU::ARM32Recompiler::MoveSToReg(const vixl::aarch32::Register& dst, CompileFlags cf)
{
  if (cf.valid_host_s)
  {
    if (cf.host_s != dst.GetCode())
      armAsm->mov(dst, Register(cf.host_s));
  }
  else if (cf.const_s)
  {
    const u32 cv = GetConstantRegU32(cf.MipsS());
    EmitMov(dst, cv);
  }
  else
  {
    WARNING_LOG("Hit memory path in MoveSToReg() for {}", GetRegName(cf.MipsS()));
    armAsm->ldr(dst, PTR(&g_state.regs.r[cf.mips_s]));
  }
}

void CPU::ARM32Recompiler::MoveTToReg(const vixl::aarch32::Register& dst, CompileFlags cf)
{
  if (cf.valid_host_t)
  {
    if (cf.host_t != dst.GetCode())
      armAsm->mov(dst, Register(cf.host_t));
  }
  else if (cf.const_t)
  {
    const u32 cv = GetConstantRegU32(cf.MipsT());
    EmitMov(dst, cv);
  }
  else
  {
    WARNING_LOG("Hit memory path in MoveTToReg() for {}", GetRegName(cf.MipsT()));
    armAsm->ldr(dst, PTR(&g_state.regs.r[cf.mips_t]));
  }
}

void CPU::ARM32Recompiler::MoveMIPSRegToReg(const vixl::aarch32::Register& dst, Reg reg)
{
  DebugAssert(reg < Reg::count);
  if (const std::optional<u32> hreg = CheckHostReg(0, Recompiler::HR_TYPE_CPU_REG, reg))
    armAsm->mov(dst, Register(hreg.value()));
  else if (HasConstantReg(reg))
    EmitMov(dst, GetConstantRegU32(reg));
  else
    armAsm->ldr(dst, MipsPtr(reg));
}

void CPU::ARM32Recompiler::GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val, Reg arg2reg /* = Reg::count */,
                                                        Reg arg3reg /* = Reg::count */)
{
  DebugAssert(g_settings.gpu_pgxp_enable);

  Flush(FLUSH_FOR_C_CALL);

  if (arg2reg != Reg::count)
    MoveMIPSRegToReg(RARG2, arg2reg);
  if (arg3reg != Reg::count)
    MoveMIPSRegToReg(RARG3, arg3reg);

  EmitMov(RARG1, arg1val);
  EmitCall(func);
}

void CPU::ARM32Recompiler::Flush(u32 flags)
{
  Recompiler::Flush(flags);

  if (flags & FLUSH_PC && m_dirty_pc)
  {
    StoreConstantToCPUPointer(m_compiler_pc, &g_state.pc);
    m_dirty_pc = false;
  }

  if (flags & FLUSH_INSTRUCTION_BITS)
  {
    // This sucks, but it's only used for fallbacks.
    EmitMov(RARG1, inst->bits);
    EmitMov(RARG2, m_current_instruction_pc);
    EmitMov(RARG3, m_current_instruction_branch_delay_slot);
    armAsm->str(RARG1, PTR(&g_state.current_instruction.bits));
    armAsm->str(RARG2, PTR(&g_state.current_instruction_pc));
    armAsm->strb(RARG3, PTR(&g_state.current_instruction_in_branch_delay_slot));
  }

  if (flags & FLUSH_LOAD_DELAY_FROM_STATE && m_load_delay_dirty)
  {
    // This sucks :(
    // TODO: make it a function?
    armAsm->ldrb(RARG1, PTR(&g_state.load_delay_reg));
    armAsm->ldr(RARG2, PTR(&g_state.load_delay_value));
    EmitMov(RSCRATCH, OFFSETOF(CPU::State, regs.r[0]));
    armAsm->add(RARG1, RSCRATCH, vixl::aarch32::Operand(RARG1, LSL, 2));
    armAsm->str(RARG2, MemOperand(RSTATE, RARG1));
    EmitMov(RSCRATCH, static_cast<u8>(Reg::count));
    armAsm->strb(RSCRATCH, PTR(&g_state.load_delay_reg));
    m_load_delay_dirty = false;
  }

  if (flags & FLUSH_LOAD_DELAY && m_load_delay_register != Reg::count)
  {
    if (m_load_delay_value_register != NUM_HOST_REGS)
      FreeHostReg(m_load_delay_value_register);

    EmitMov(RSCRATCH, static_cast<u8>(m_load_delay_register));
    armAsm->strb(RSCRATCH, PTR(&g_state.load_delay_reg));
    m_load_delay_register = Reg::count;
    m_load_delay_dirty = true;
  }

  if (flags & FLUSH_GTE_STALL_FROM_STATE && m_dirty_gte_done_cycle)
  {
    // May as well flush cycles while we're here.
    // GTE spanning blocks is very rare, we _could_ disable this for speed.
    armAsm->ldr(RARG1, PTR(&g_state.pending_ticks));
    armAsm->ldr(RARG2, PTR(&g_state.gte_completion_tick));
    if (m_cycles > 0)
    {
      armAsm->add(RARG1, RARG1, armCheckAddSubConstant(m_cycles));
      m_cycles = 0;
    }
    armAsm->cmp(RARG2, RARG1);
    armAsm->mov(hs, RARG1, RARG2);
    armAsm->str(RARG1, PTR(&g_state.pending_ticks));
    m_dirty_gte_done_cycle = false;
  }

  if (flags & FLUSH_GTE_DONE_CYCLE && m_gte_done_cycle > m_cycles)
  {
    armAsm->ldr(RARG1, PTR(&g_state.pending_ticks));

    // update cycles at the same time
    if (flags & FLUSH_CYCLES && m_cycles > 0)
    {
      armAsm->add(RARG1, RARG1, armCheckAddSubConstant(m_cycles));
      armAsm->str(RARG1, PTR(&g_state.pending_ticks));
      m_gte_done_cycle -= m_cycles;
      m_cycles = 0;
    }

    armAsm->add(RARG1, RARG1, armCheckAddSubConstant(m_gte_done_cycle));
    armAsm->str(RARG1, PTR(&g_state.gte_completion_tick));
    m_gte_done_cycle = 0;
    m_dirty_gte_done_cycle = true;
  }

  if (flags & FLUSH_CYCLES && m_cycles > 0)
  {
    armAsm->ldr(RARG1, PTR(&g_state.pending_ticks));
    armAsm->add(RARG1, RARG1, armCheckAddSubConstant(m_cycles));
    armAsm->str(RARG1, PTR(&g_state.pending_ticks));
    m_gte_done_cycle = std::max<TickCount>(m_gte_done_cycle - m_cycles, 0);
    m_cycles = 0;
  }
}

void CPU::ARM32Recompiler::Compile_Fallback()
{
  WARNING_LOG("Compiling instruction fallback at PC=0x{:08X}, instruction=0x{:08X}", iinfo->pc, inst->bits);

  Flush(FLUSH_FOR_INTERPRETER);

  EmitCall(reinterpret_cast<const void*>(&CPU::RecompilerThunks::InterpretInstruction));

  // TODO: make me less garbage
  // TODO: this is wrong, it flushes the load delay on the same cycle when we return.
  // but nothing should be going through here..
  Label no_load_delay;
  armAsm->ldrb(RARG1, PTR(&g_state.next_load_delay_reg));
  armAsm->cmp(RARG1, static_cast<u8>(Reg::count));
  armAsm->b(eq, &no_load_delay);
  armAsm->ldr(RARG2, PTR(&g_state.next_load_delay_value));
  armAsm->strb(RARG1, PTR(&g_state.load_delay_reg));
  armAsm->str(RARG2, PTR(&g_state.load_delay_value));
  EmitMov(RARG1, static_cast<u32>(Reg::count));
  armAsm->strb(RARG1, PTR(&g_state.next_load_delay_reg));
  armAsm->bind(&no_load_delay);

  m_load_delay_dirty = EMULATE_LOAD_DELAYS;
}

void CPU::ARM32Recompiler::CheckBranchTarget(const vixl::aarch32::Register& pcreg)
{
  if (!g_settings.cpu_recompiler_memory_exceptions)
    return;

  armAsm->tst(pcreg, armCheckLogicalConstant(0x3));
  SwitchToFarCode(true, ne);

  BackupHostState();
  EndBlockWithException(Exception::AdEL);

  RestoreHostState();
  SwitchToNearCode(false);
}

void CPU::ARM32Recompiler::Compile_jr(CompileFlags cf)
{
  const Register pcreg = CFGetRegS(cf);
  CheckBranchTarget(pcreg);

  armAsm->str(pcreg, PTR(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::ARM32Recompiler::Compile_jalr(CompileFlags cf)
{
  const Register pcreg = CFGetRegS(cf);
  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetBranchReturnAddress(cf));

  CheckBranchTarget(pcreg);
  armAsm->str(pcreg, PTR(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::ARM32Recompiler::Compile_bxx(CompileFlags cf, BranchCondition cond)
{
  AssertRegOrConstS(cf);

  const u32 taken_pc = GetConditionalBranchTarget(cf);

  Flush(FLUSH_FOR_BRANCH);

  DebugAssert(cf.valid_host_s);

  // MipsT() here should equal zero for zero branches.
  DebugAssert(cond == BranchCondition::Equal || cond == BranchCondition::NotEqual || cf.MipsT() == Reg::zero);

  Label taken;
  const Register rs = CFGetRegS(cf);
  switch (cond)
  {
    case BranchCondition::Equal:
    case BranchCondition::NotEqual:
    {
      AssertRegOrConstT(cf);
      if (cf.valid_host_t)
        armAsm->cmp(rs, CFGetRegT(cf));
      else if (cf.const_t)
        armAsm->cmp(rs, armCheckCompareConstant(GetConstantRegU32(cf.MipsT())));

      armAsm->b((cond == BranchCondition::Equal) ? eq : ne, &taken);
    }
    break;

    case BranchCondition::GreaterThanZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(gt, &taken);
    }
    break;

    case BranchCondition::GreaterEqualZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(ge, &taken);
    }
    break;

    case BranchCondition::LessThanZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(lt, &taken);
    }
    break;

    case BranchCondition::LessEqualZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(le, &taken);
    }
    break;
  }

  BackupHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(m_compiler_pc, true);

  armAsm->bind(&taken);

  RestoreHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(taken_pc, true);
}

void CPU::ARM32Recompiler::Compile_addi(CompileFlags cf, bool overflow)
{
  const Register rs = CFGetRegS(cf);
  const Register rt = CFGetRegT(cf);
  if (const u32 imm = inst->i.imm_sext32(); imm != 0)
  {
    if (!overflow)
    {
      armAsm->add(rt, rs, armCheckAddSubConstant(imm));
    }
    else
    {
      armAsm->adds(rt, rs, armCheckAddSubConstant(imm));
      TestOverflow(rt);
    }
  }
  else if (rt.GetCode() != rs.GetCode())
  {
    armAsm->mov(rt, rs);
  }
}

void CPU::ARM32Recompiler::Compile_addi(CompileFlags cf)
{
  Compile_addi(cf, g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::ARM32Recompiler::Compile_addiu(CompileFlags cf)
{
  Compile_addi(cf, false);
}

void CPU::ARM32Recompiler::Compile_slti(CompileFlags cf)
{
  Compile_slti(cf, true);
}

void CPU::ARM32Recompiler::Compile_sltiu(CompileFlags cf)
{
  Compile_slti(cf, false);
}

void CPU::ARM32Recompiler::Compile_slti(CompileFlags cf, bool sign)
{
  const Register rs = CFGetRegS(cf);
  const Register rt = CFGetRegT(cf);
  armAsm->cmp(rs, armCheckCompareConstant(static_cast<s32>(inst->i.imm_sext32())));
  armAsm->mov(sign ? ge : hs, rt, 0);
  armAsm->mov(sign ? lt : lo, rt, 1);
}

void CPU::ARM32Recompiler::Compile_andi(CompileFlags cf)
{
  const Register rt = CFGetRegT(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    armAsm->and_(rt, CFGetRegS(cf), armCheckLogicalConstant(imm));
  else
    EmitMov(rt, 0);
}

void CPU::ARM32Recompiler::Compile_ori(CompileFlags cf)
{
  const Register rt = CFGetRegT(cf);
  const Register rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    armAsm->orr(rt, rs, armCheckLogicalConstant(imm));
  else if (rt.GetCode() != rs.GetCode())
    armAsm->mov(rt, rs);
}

void CPU::ARM32Recompiler::Compile_xori(CompileFlags cf)
{
  const Register rt = CFGetRegT(cf);
  const Register rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    armAsm->eor(rt, rs, armCheckLogicalConstant(imm));
  else if (rt.GetCode() != rs.GetCode())
    armAsm->mov(rt, rs);
}

void CPU::ARM32Recompiler::Compile_shift(CompileFlags cf,
                                         void (vixl::aarch32::Assembler::*op)(vixl::aarch32::Register,
                                                                              vixl::aarch32::Register, const Operand&))
{
  const Register rd = CFGetRegD(cf);
  const Register rt = CFGetRegT(cf);
  if (inst->r.shamt > 0)
    (armAsm->*op)(rd, rt, inst->r.shamt.GetValue());
  else if (rd.GetCode() != rt.GetCode())
    armAsm->mov(rd, rt);
}

void CPU::ARM32Recompiler::Compile_sll(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::lsl);
}

void CPU::ARM32Recompiler::Compile_srl(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::lsr);
}

void CPU::ARM32Recompiler::Compile_sra(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::asr);
}

void CPU::ARM32Recompiler::Compile_variable_shift(CompileFlags cf,
                                                  void (vixl::aarch32::Assembler::*op)(vixl::aarch32::Register,
                                                                                       vixl::aarch32::Register,
                                                                                       const Operand&))
{
  const Register rd = CFGetRegD(cf);

  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  if (cf.const_s)
  {
    if (const u32 shift = GetConstantRegU32(cf.MipsS()); shift != 0)
      (armAsm->*op)(rd, rt, shift & 0x1Fu);
    else if (rd.GetCode() != rt.GetCode())
      armAsm->mov(rd, rt);
  }
  else
  {
    armAsm->and_(RSCRATCH, CFGetRegS(cf), 0x1Fu);
    (armAsm->*op)(rd, rt, RSCRATCH);
  }
}

void CPU::ARM32Recompiler::Compile_sllv(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::lsl);
}

void CPU::ARM32Recompiler::Compile_srlv(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::lsr);
}

void CPU::ARM32Recompiler::Compile_srav(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::asr);
}

void CPU::ARM32Recompiler::Compile_mult(CompileFlags cf, bool sign)
{
  const Register rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  // TODO: if lo/hi gets killed, we can use a 32-bit multiply
  const Register lo = CFGetRegLO(cf);
  const Register hi = CFGetRegHI(cf);

  (sign) ? armAsm->smull(lo, hi, rs, rt) : armAsm->umull(lo, hi, rs, rt);
}

void CPU::ARM32Recompiler::Compile_mult(CompileFlags cf)
{
  Compile_mult(cf, true);
}

void CPU::ARM32Recompiler::Compile_multu(CompileFlags cf)
{
  Compile_mult(cf, false);
}

void CPU::ARM32Recompiler::Compile_div(CompileFlags cf)
{
  const Register rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const Register rlo = CFGetRegLO(cf);
  const Register rhi = CFGetRegHI(cf);

  // TODO: This could be slightly more optimal
  Label done;
  Label not_divide_by_zero;
  armAsm->cmp(rt, 0);
  armAsm->b(ne, &not_divide_by_zero);
  armAsm->mov(rhi, rs); // hi = num
  EmitMov(rlo, 1);
  EmitMov(RSCRATCH, static_cast<u32>(-1));
  armAsm->cmp(rs, 0);
  armAsm->mov(ge, rlo, RSCRATCH); // lo = s >= 0 ? -1 : 1
  armAsm->b(&done);

  armAsm->bind(&not_divide_by_zero);
  Label not_unrepresentable;
  armAsm->cmp(rs, armCheckCompareConstant(static_cast<s32>(0x80000000u)));
  armAsm->b(ne, &not_unrepresentable);
  armAsm->cmp(rt, armCheckCompareConstant(-1));
  armAsm->b(ne, &not_unrepresentable);

  EmitMov(rlo, 0x80000000u);
  EmitMov(rhi, 0);
  armAsm->b(&done);

  armAsm->bind(&not_unrepresentable);

  armAsm->sdiv(rlo, rs, rt);

  // TODO: skip when hi is dead
  armAsm->mls(rhi, rlo, rt, rs);

  armAsm->bind(&done);
}

void CPU::ARM32Recompiler::Compile_divu(CompileFlags cf)
{
  const Register rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const Register rlo = CFGetRegLO(cf);
  const Register rhi = CFGetRegHI(cf);

  Label done;
  Label not_divide_by_zero;
  armAsm->cmp(rt, 0);
  armAsm->b(ne, &not_divide_by_zero);
  EmitMov(rlo, static_cast<u32>(-1));
  armAsm->mov(rhi, rs);
  armAsm->b(&done);

  armAsm->bind(&not_divide_by_zero);

  armAsm->udiv(rlo, rs, rt);

  // TODO: skip when hi is dead
  armAsm->mls(rhi, rlo, rt, rs);

  armAsm->bind(&done);
}

void CPU::ARM32Recompiler::TestOverflow(const vixl::aarch32::Register& result)
{
  SwitchToFarCode(true, vs);

  BackupHostState();

  // toss the result
  ClearHostReg(result.GetCode());

  EndBlockWithException(Exception::Ov);

  RestoreHostState();

  SwitchToNearCode(false);
}

void CPU::ARM32Recompiler::Compile_dst_op(CompileFlags cf,
                                          void (vixl::aarch32::Assembler::*op)(vixl::aarch32::Register,
                                                                               vixl::aarch32::Register, const Operand&),
                                          bool commutative, bool logical, bool overflow)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const Register rd = CFGetRegD(cf);
  if (cf.valid_host_s && cf.valid_host_t)
  {
    (armAsm->*op)(rd, CFGetRegS(cf), CFGetRegT(cf));
  }
  else if (commutative && (cf.const_s || cf.const_t))
  {
    const Register src = cf.const_s ? CFGetRegT(cf) : CFGetRegS(cf);
    if (const u32 cv = GetConstantRegU32(cf.const_s ? cf.MipsS() : cf.MipsT()); cv != 0)
    {
      (armAsm->*op)(rd, src, logical ? armCheckLogicalConstant(cv) : armCheckAddSubConstant(cv));
    }
    else
    {
      if (rd.GetCode() != src.GetCode())
        armAsm->mov(rd, src);
      overflow = false;
    }
  }
  else if (cf.const_s)
  {
    EmitMov(RSCRATCH, GetConstantRegU32(cf.MipsS()));
    (armAsm->*op)(rd, RSCRATCH, CFGetRegT(cf));
  }
  else if (cf.const_t)
  {
    const Register rs = CFGetRegS(cf);
    if (const u32 cv = GetConstantRegU32(cf.const_s ? cf.MipsS() : cf.MipsT()); cv != 0)
    {
      (armAsm->*op)(rd, rs, logical ? armCheckLogicalConstant(cv) : armCheckAddSubConstant(cv));
    }
    else
    {
      if (rd.GetCode() != rs.GetCode())
        armAsm->mov(rd, rs);
      overflow = false;
    }
  }

  if (overflow)
    TestOverflow(rd);
}

void CPU::ARM32Recompiler::Compile_add(CompileFlags cf)
{
  if (g_settings.cpu_recompiler_memory_exceptions)
    Compile_dst_op(cf, &Assembler::adds, true, false, true);
  else
    Compile_dst_op(cf, &Assembler::add, true, false, false);
}

void CPU::ARM32Recompiler::Compile_addu(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::add, true, false, false);
}

void CPU::ARM32Recompiler::Compile_sub(CompileFlags cf)
{
  if (g_settings.cpu_recompiler_memory_exceptions)
    Compile_dst_op(cf, &Assembler::subs, false, false, true);
  else
    Compile_dst_op(cf, &Assembler::sub, false, false, false);
}

void CPU::ARM32Recompiler::Compile_subu(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::sub, false, false, false);
}

void CPU::ARM32Recompiler::Compile_and(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // special cases - and with self -> self, and with 0 -> 0
  const Register regd = CFGetRegD(cf);
  if (cf.MipsS() == cf.MipsT())
  {
    armAsm->mov(regd, CFGetRegS(cf));
    return;
  }
  else if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0))
  {
    EmitMov(regd, 0);
    return;
  }

  Compile_dst_op(cf, &Assembler::and_, true, true, false);
}

void CPU::ARM32Recompiler::Compile_or(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // or/nor with 0 -> no effect
  const Register regd = CFGetRegD(cf);
  if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0) || cf.MipsS() == cf.MipsT())
  {
    cf.const_s ? MoveTToReg(regd, cf) : MoveSToReg(regd, cf);
    return;
  }

  Compile_dst_op(cf, &Assembler::orr, true, true, false);
}

void CPU::ARM32Recompiler::Compile_xor(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const Register regd = CFGetRegD(cf);
  if (cf.MipsS() == cf.MipsT())
  {
    // xor with self -> zero
    EmitMov(regd, 0);
    return;
  }
  else if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0))
  {
    // xor with zero -> no effect
    cf.const_s ? MoveTToReg(regd, cf) : MoveSToReg(regd, cf);
    return;
  }

  Compile_dst_op(cf, &Assembler::eor, true, true, false);
}

void CPU::ARM32Recompiler::Compile_nor(CompileFlags cf)
{
  Compile_or(cf);
  armAsm->mvn(CFGetRegD(cf), CFGetRegD(cf));
}

void CPU::ARM32Recompiler::Compile_slt(CompileFlags cf)
{
  Compile_slt(cf, true);
}

void CPU::ARM32Recompiler::Compile_sltu(CompileFlags cf)
{
  Compile_slt(cf, false);
}

void CPU::ARM32Recompiler::Compile_slt(CompileFlags cf, bool sign)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // TODO: swap and reverse op for constants
  if (cf.const_s)
  {
    EmitMov(RSCRATCH, GetConstantRegS32(cf.MipsS()));
    armAsm->cmp(RSCRATCH, CFGetRegT(cf));
  }
  else if (cf.const_t)
  {
    armAsm->cmp(CFGetRegS(cf), armCheckCompareConstant(GetConstantRegS32(cf.MipsT())));
  }
  else
  {
    armAsm->cmp(CFGetRegS(cf), CFGetRegT(cf));
  }

  const Register rd = CFGetRegD(cf);
  armAsm->mov(sign ? ge : cs, rd, 0);
  armAsm->mov(sign ? lt : lo, rd, 1);
}

vixl::aarch32::Register
CPU::ARM32Recompiler::ComputeLoadStoreAddressArg(CompileFlags cf, const std::optional<VirtualMemoryAddress>& address,
                                                 const std::optional<const vixl::aarch32::Register>& reg)
{
  const u32 imm = inst->i.imm_sext32();
  if (cf.valid_host_s && imm == 0 && !reg.has_value())
    return CFGetRegS(cf);

  const Register dst = reg.has_value() ? reg.value() : RARG1;
  if (address.has_value())
  {
    EmitMov(dst, address.value());
  }
  else if (imm == 0)
  {
    if (cf.valid_host_s)
    {
      if (const Register src = CFGetRegS(cf); src.GetCode() != dst.GetCode())
        armAsm->mov(dst, CFGetRegS(cf));
    }
    else
    {
      armAsm->ldr(dst, MipsPtr(cf.MipsS()));
    }
  }
  else
  {
    if (cf.valid_host_s)
    {
      armAsm->add(dst, CFGetRegS(cf), armCheckAddSubConstant(static_cast<s32>(inst->i.imm_sext32())));
    }
    else
    {
      armAsm->ldr(dst, MipsPtr(cf.MipsS()));
      armAsm->add(dst, dst, armCheckAddSubConstant(static_cast<s32>(inst->i.imm_sext32())));
    }
  }

  return dst;
}

template<typename RegAllocFn>
vixl::aarch32::Register CPU::ARM32Recompiler::GenerateLoad(const vixl::aarch32::Register& addr_reg,
                                                           MemoryAccessSize size, bool sign, bool use_fastmem,
                                                           const RegAllocFn& dst_reg_alloc)
{
  if (use_fastmem)
  {
    DebugAssert(g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT);
    m_cycles += Bus::RAM_READ_TICKS;

    const Register dst = dst_reg_alloc();
    const Register membase = GetMembaseReg();
    DebugAssert(addr_reg.GetCode() != RARG3.GetCode());
    armAsm->lsr(RARG3, addr_reg, Bus::FASTMEM_LUT_PAGE_SHIFT);
    armAsm->ldr(RARG3, MemOperand(membase, RARG3, LSL, 2));

    const MemOperand mem = MemOperand(RARG3, addr_reg);
    u8* start = armAsm->GetCursorAddress<u8*>();
    switch (size)
    {
      case MemoryAccessSize::Byte:
        sign ? armAsm->ldrsb(dst, mem) : armAsm->ldrb(dst, mem);
        break;

      case MemoryAccessSize::HalfWord:
        sign ? armAsm->ldrsh(dst, mem) : armAsm->ldrh(dst, mem);
        break;

      case MemoryAccessSize::Word:
        armAsm->ldr(dst, mem);
        break;
    }

    AddLoadStoreInfo(start, kA32InstructionSizeInBytes, addr_reg.GetCode(), dst.GetCode(), size, sign, true);
    return dst;
  }

  if (addr_reg.GetCode() != RARG1.GetCode())
    armAsm->mov(RARG1, addr_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&RecompilerThunks::ReadMemoryByte) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&RecompilerThunks::ReadMemoryHalfWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&RecompilerThunks::ReadMemoryWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    SwitchToFarCodeIfBitSet(RRETHI, 31);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const Register temp = Register(AllocateTempHostReg(HR_CALLEE_SAVED));
    armAsm->rsb(temp, RRETHI, 0);
    armAsm->lsl(temp, temp, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (-result << 2) | BD | cop_n
    armAsm->orr(RARG1, temp,
                armCheckLogicalConstant(Cop0Registers::CAUSE::MakeValueForException(
                  static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n)));
    EmitMov(RARG2, m_current_instruction_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    FreeHostReg(temp.GetCode());
    EndBlock(std::nullopt, true);

    RestoreHostState();
    SwitchToNearCode(false);
  }

  const Register dst_reg = dst_reg_alloc();
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      sign ? armAsm->sxtb(dst_reg, RRET) : armAsm->uxtb(dst_reg, RRET);
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      sign ? armAsm->sxth(dst_reg, RRET) : armAsm->uxth(dst_reg, RRET);
    }
    break;
    case MemoryAccessSize::Word:
    {
      if (dst_reg.GetCode() != RRET.GetCode())
        armAsm->mov(dst_reg, RRET);
    }
    break;
  }

  return dst_reg;
}

void CPU::ARM32Recompiler::GenerateStore(const vixl::aarch32::Register& addr_reg,
                                         const vixl::aarch32::Register& value_reg, MemoryAccessSize size,
                                         bool use_fastmem)
{
  if (use_fastmem)
  {
    DebugAssert(g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT);
    DebugAssert(addr_reg.GetCode() != RARG3.GetCode());
    const Register membase = GetMembaseReg();
    armAsm->lsr(RARG3, addr_reg, Bus::FASTMEM_LUT_PAGE_SHIFT);
    armAsm->ldr(RARG3, MemOperand(membase, RARG3, LSL, 2));

    const MemOperand mem = MemOperand(RARG3, addr_reg);
    u8* start = armAsm->GetCursorAddress<u8*>();
    switch (size)
    {
      case MemoryAccessSize::Byte:
        armAsm->strb(value_reg, mem);
        break;

      case MemoryAccessSize::HalfWord:
        armAsm->strh(value_reg, mem);
        break;

      case MemoryAccessSize::Word:
        armAsm->str(value_reg, mem);
        break;
    }
    AddLoadStoreInfo(start, kA32InstructionSizeInBytes, addr_reg.GetCode(), value_reg.GetCode(), size, false, false);
    return;
  }

  if (addr_reg.GetCode() != RARG1.GetCode())
    armAsm->mov(RARG1, addr_reg);
  if (value_reg.GetCode() != RARG2.GetCode())
    armAsm->mov(RARG2, value_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&RecompilerThunks::WriteMemoryByte) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&RecompilerThunks::WriteMemoryHalfWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&RecompilerThunks::WriteMemoryWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    SwitchToFarCodeIfRegZeroOrNonZero(RRET, true);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const Register temp = Register(AllocateTempHostReg(HR_CALLEE_SAVED));
    armAsm->lsl(temp, RRET, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (result << 2) | BD | cop_n
    armAsm->orr(RARG1, temp,
                armCheckLogicalConstant(Cop0Registers::CAUSE::MakeValueForException(
                  static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n)));
    EmitMov(RARG2, m_current_instruction_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    FreeHostReg(temp.GetCode());
    EndBlock(std::nullopt, true);

    RestoreHostState();
    SwitchToNearCode(false);
  }
}

void CPU::ARM32Recompiler::Compile_lxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  const std::optional<Register> addr_reg = g_settings.gpu_pgxp_enable ?
                                             std::optional<Register>(Register(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                             std::optional<Register>();
  FlushForLoadStore(address, false, use_fastmem);
  const Register addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Register data = GenerateLoad(addr, size, sign, use_fastmem, [this, cf]() {
    if (cf.MipsT() == Reg::zero)
      return RRET;

    return Register(AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                                    EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, cf.MipsT()));
  });

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);

    EmitMov(RARG1, inst->bits);
    armAsm->mov(RARG2, addr);
    armAsm->mov(RARG3, data);
    EmitCall(s_pgxp_mem_load_functions[static_cast<u32>(size)][static_cast<u32>(sign)]);
    FreeHostReg(addr_reg.value().GetCode());
  }
}

void CPU::ARM32Recompiler::Compile_lwx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  const Register addr = Register(AllocateTempHostReg(HR_CALLEE_SAVED));
  FlushForLoadStore(address, false, use_fastmem);

  // TODO: if address is constant, this can be simplified..

  // If we're coming from another block, just flush the load delay and hope for the best..
  if (m_load_delay_dirty)
    UpdateLoadDelay();

  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  armAsm->bic(RARG1, addr, 3);
  GenerateLoad(RARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RRET; });

  if (inst->r.rt == Reg::zero)
  {
    FreeHostReg(addr.GetCode());
    return;
  }

  // lwl/lwr from a load-delayed value takes the new value, but it itself, is load delayed, so the original value is
  // never written back. NOTE: can't trust T in cf because of the flush
  const Reg rt = inst->r.rt;
  Register value;
  if (m_load_delay_register == rt)
  {
    const u32 existing_ld_rt = (m_load_delay_value_register == NUM_HOST_REGS) ?
                                 AllocateHostReg(HR_MODE_READ, HR_TYPE_LOAD_DELAY_VALUE, rt) :
                                 m_load_delay_value_register;
    RenameHostReg(existing_ld_rt, HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt);
    value = Register(existing_ld_rt);
  }
  else
  {
    if constexpr (EMULATE_LOAD_DELAYS)
    {
      value = Register(AllocateHostReg(HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt));
      if (const std::optional<u32> rtreg = CheckHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt); rtreg.has_value())
        armAsm->mov(value, Register(rtreg.value()));
      else if (HasConstantReg(rt))
        EmitMov(value, GetConstantRegU32(rt));
      else
        armAsm->ldr(value, MipsPtr(rt));
    }
    else
    {
      value = Register(AllocateHostReg(HR_MODE_READ | HR_MODE_WRITE, HR_TYPE_CPU_REG, rt));
    }
  }

  DebugAssert(value.GetCode() != RARG2.GetCode() && value.GetCode() != RARG3.GetCode());
  armAsm->and_(RARG2, addr, 3);
  armAsm->lsl(RARG2, RARG2, 3); // *8
  EmitMov(RARG3, 24);
  armAsm->sub(RARG3, RARG3, RARG2);

  if (inst->op == InstructionOp::lwl)
  {
    // const u32 mask = UINT32_C(0x00FFFFFF) >> shift;
    // new_value = (value & mask) | (RWRET << (24 - shift));
    EmitMov(RSCRATCH, 0xFFFFFFu);
    armAsm->lsr(RSCRATCH, RSCRATCH, RARG2);
    armAsm->and_(value, value, RSCRATCH);
    armAsm->lsl(RRET, RRET, RARG3);
    armAsm->orr(value, value, RRET);
  }
  else
  {
    // const u32 mask = UINT32_C(0xFFFFFF00) << (24 - shift);
    // new_value = (value & mask) | (RWRET >> shift);
    armAsm->lsr(RRET, RRET, RARG2);
    EmitMov(RSCRATCH, 0xFFFFFF00u);
    armAsm->lsl(RSCRATCH, RSCRATCH, RARG3);
    armAsm->and_(value, value, RSCRATCH);
    armAsm->orr(value, value, RRET);
  }

  FreeHostReg(addr.GetCode());

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    armAsm->mov(RARG3, value);
    armAsm->bic(RARG2, addr, 3);
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LW));
  }
}

void CPU::ARM32Recompiler::Compile_lwc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                        const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, true);
  const std::optional<Register> addr_reg = g_settings.gpu_pgxp_enable ?
                                             std::optional<Register>(Register(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                             std::optional<Register>();
  FlushForLoadStore(address, false, use_fastmem);
  const Register addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Register value = GenerateLoad(addr, MemoryAccessSize::Word, false, use_fastmem, [this, action = action]() {
    return (action == GTERegisterAccessAction::CallHandler && g_settings.gpu_pgxp_enable) ?
             Register(AllocateTempHostReg(HR_CALLEE_SAVED)) :
             RRET;
  });

  switch (action)
  {
    case GTERegisterAccessAction::Ignore:
    {
      break;
    }

    case GTERegisterAccessAction::Direct:
    {
      armAsm->str(value, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::SignExtend16:
    {
      armAsm->sxth(RARG3, value);
      armAsm->str(RARG3, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::ZeroExtend16:
    {
      armAsm->uxth(RARG3, value);
      armAsm->str(RARG3, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::CallHandler:
    {
      Flush(FLUSH_FOR_C_CALL);
      armAsm->mov(RARG2, value);
      EmitMov(RARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::WriteRegister));
      break;
    }

    case GTERegisterAccessAction::PushFIFO:
    {
      // SXY0 <- SXY1
      // SXY1 <- SXY2
      // SXY2 <- SXYP
      DebugAssert(value.GetCode() != RARG2.GetCode() && value.GetCode() != RARG3.GetCode());
      armAsm->ldr(RARG2, PTR(&g_state.gte_regs.SXY1[0]));
      armAsm->ldr(RARG3, PTR(&g_state.gte_regs.SXY2[0]));
      armAsm->str(RARG2, PTR(&g_state.gte_regs.SXY0[0]));
      armAsm->str(RARG3, PTR(&g_state.gte_regs.SXY1[0]));
      armAsm->str(value, PTR(&g_state.gte_regs.SXY2[0]));
      break;
    }

    default:
    {
      Panic("Unknown action");
      return;
    }
  }

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    armAsm->mov(RARG3, value);
    if (value.GetCode() != RRET.GetCode())
      FreeHostReg(value.GetCode());
    armAsm->mov(RARG2, addr);
    FreeHostReg(addr_reg.value().GetCode());
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LWC2));
  }
}

void CPU::ARM32Recompiler::Compile_sxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const std::optional<Register> addr_reg = g_settings.gpu_pgxp_enable ?
                                             std::optional<Register>(Register(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                             std::optional<Register>();
  FlushForLoadStore(address, true, use_fastmem);
  const Register addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Register data = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(RARG2, cf);

  GenerateStore(addr, data, size, use_fastmem);

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    MoveMIPSRegToReg(RARG3, cf.MipsT());
    armAsm->mov(RARG2, addr);
    EmitMov(RARG1, inst->bits);
    EmitCall(s_pgxp_mem_store_functions[static_cast<u32>(size)]);
    FreeHostReg(addr_reg.value().GetCode());
  }
}

void CPU::ARM32Recompiler::Compile_swx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  // TODO: this can take over rt's value if it's no longer needed
  // NOTE: can't trust T in cf because of the alloc
  const Register addr = Register(AllocateTempHostReg(HR_CALLEE_SAVED));
  const Register value = g_settings.gpu_pgxp_enable ? Register(AllocateTempHostReg(HR_CALLEE_SAVED)) : RARG2;
  if (g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  FlushForLoadStore(address, true, use_fastmem);

  // TODO: if address is constant, this can be simplified..
  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  armAsm->bic(RARG1, addr, 3);
  GenerateLoad(RARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RRET; });

  armAsm->and_(RSCRATCH, addr, 3);
  armAsm->lsl(RSCRATCH, RSCRATCH, 3); // *8
  armAsm->bic(addr, addr, 3);

  // Need to load down here for PGXP-off, because it's in a volatile reg that can get overwritten by flush.
  if (!g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  if (inst->op == InstructionOp::swl)
  {
    // const u32 mem_mask = UINT32_C(0xFFFFFF00) << shift;
    // new_value = (RWRET & mem_mask) | (value >> (24 - shift));
    EmitMov(RARG3, 0xFFFFFF00u);
    armAsm->lsl(RARG3, RARG3, RSCRATCH);
    armAsm->and_(RRET, RRET, RARG3);

    EmitMov(RARG3, 24);
    armAsm->sub(RARG3, RARG3, RSCRATCH);
    armAsm->lsr(value, value, RARG3);
    armAsm->orr(value, value, RRET);
  }
  else
  {
    // const u32 mem_mask = UINT32_C(0x00FFFFFF) >> (24 - shift);
    // new_value = (RWRET & mem_mask) | (value << shift);
    armAsm->lsl(value, value, RSCRATCH);

    EmitMov(RARG3, 24);
    armAsm->sub(RARG3, RARG3, RSCRATCH);
    EmitMov(RSCRATCH, 0x00FFFFFFu);
    armAsm->lsr(RSCRATCH, RSCRATCH, RARG3);
    armAsm->and_(RRET, RRET, RSCRATCH);
    armAsm->orr(value, value, RRET);
  }

  if (!g_settings.gpu_pgxp_enable)
  {
    GenerateStore(addr, value, MemoryAccessSize::Word, use_fastmem);
    FreeHostReg(addr.GetCode());
  }
  else
  {
    GenerateStore(addr, value, MemoryAccessSize::Word, use_fastmem);

    Flush(FLUSH_FOR_C_CALL);
    armAsm->mov(RARG3, value);
    FreeHostReg(value.GetCode());
    armAsm->mov(RARG2, addr);
    FreeHostReg(addr.GetCode());
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SW));
  }
}

void CPU::ARM32Recompiler::Compile_swc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                        const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, false);
  const Register addr = (g_settings.gpu_pgxp_enable || action == GTERegisterAccessAction::CallHandler) ?
                          Register(AllocateTempHostReg(HR_CALLEE_SAVED)) :
                          RARG1;
  const Register data = g_settings.gpu_pgxp_enable ? Register(AllocateTempHostReg(HR_CALLEE_SAVED)) : RARG2;
  FlushForLoadStore(address, true, use_fastmem);
  ComputeLoadStoreAddressArg(cf, address, addr);

  switch (action)
  {
    case GTERegisterAccessAction::Direct:
    {
      armAsm->ldr(data, PTR(ptr));
    }
    break;

    case GTERegisterAccessAction::CallHandler:
    {
      // should already be flushed.. except in fastmem case
      Flush(FLUSH_FOR_C_CALL);
      EmitMov(RARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));
      armAsm->mov(data, RRET);
    }
    break;

    default:
    {
      Panic("Unknown action");
    }
    break;
  }

  GenerateStore(addr, data, size, use_fastmem);
  if (!g_settings.gpu_pgxp_enable)
  {
    if (addr.GetCode() != RARG1.GetCode())
      FreeHostReg(addr.GetCode());
  }
  else
  {
    // TODO: This can be simplified because we don't need to validate in PGXP..
    Flush(FLUSH_FOR_C_CALL);
    armAsm->mov(RARG3, data);
    FreeHostReg(data.GetCode());
    armAsm->mov(RARG2, addr);
    FreeHostReg(addr.GetCode());
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SWC2));
  }
}

void CPU::ARM32Recompiler::Compile_mtc0(CompileFlags cf)
{
  // TODO: we need better constant setting here.. which will need backprop
  AssertRegOrConstT(cf);

  const Cop0Reg reg = static_cast<Cop0Reg>(MipsD());
  const u32* ptr = GetCop0RegPtr(reg);
  const u32 mask = GetCop0RegWriteMask(reg);
  if (!ptr)
  {
    Compile_Fallback();
    return;
  }

  if (mask == 0)
  {
    // if it's a read-only register, ignore
    DEBUG_LOG("Ignoring write to read-only cop0 reg {}", static_cast<u32>(reg));
    return;
  }

  // for some registers, we need to test certain bits
  const bool needs_bit_test = (reg == Cop0Reg::SR);
  const Register new_value = RARG1;
  const Register old_value = RARG2;
  const Register changed_bits = RARG3;
  const Register mask_reg = RSCRATCH;

  // Load old value
  armAsm->ldr(old_value, PTR(ptr));

  // No way we fit this in an immediate..
  EmitMov(mask_reg, mask);

  // update value
  if (cf.valid_host_t)
    armAsm->and_(new_value, CFGetRegT(cf), mask_reg);
  else
    EmitMov(new_value, GetConstantRegU32(cf.MipsT()) & mask);

  if (needs_bit_test)
    armAsm->eor(changed_bits, old_value, new_value);
  armAsm->bic(old_value, old_value, mask_reg);
  armAsm->orr(new_value, old_value, new_value);
  armAsm->str(new_value, PTR(ptr));

  if (reg == Cop0Reg::SR)
  {
    // TODO: replace with register backup
    // We could just inline the whole thing..
    Flush(FLUSH_FOR_C_CALL);

    SwitchToFarCodeIfBitSet(changed_bits, 16);
    armAsm->push(RegisterList(RARG1));
    EmitCall(reinterpret_cast<const void*>(&CPU::UpdateMemoryPointers));
    armAsm->pop(RegisterList(RARG1));
    if (CodeCache::IsUsingFastmem() && m_block->HasFlag(CodeCache::BlockFlags::ContainsLoadStoreInstructions) &&
        IsHostRegAllocated(RMEMBASE.GetCode()))
    {
      FreeHostReg(RMEMBASE.GetCode());
    }
    SwitchToNearCode(true);

    TestInterrupts(RARG1);
  }
  else if (reg == Cop0Reg::CAUSE)
  {
    armAsm->ldr(RARG1, PTR(&g_state.cop0_regs.sr.bits));
    TestInterrupts(RARG1);
  }

  if (reg == Cop0Reg::DCIC && g_settings.cpu_recompiler_memory_exceptions)
  {
    // TODO: DCIC handling for debug breakpoints
    WARNING_LOG("TODO: DCIC handling for debug breakpoints");
  }
}

void CPU::ARM32Recompiler::Compile_rfe(CompileFlags cf)
{
  // shift mode bits right two, preserving upper bits
  armAsm->ldr(RARG1, PTR(&g_state.cop0_regs.sr.bits));
  armAsm->bic(RARG2, RARG1, 15);
  armAsm->ubfx(RARG1, RARG1, 2, 4);
  armAsm->orr(RARG1, RARG1, RARG2);
  armAsm->str(RARG1, PTR(&g_state.cop0_regs.sr.bits));

  TestInterrupts(RARG1);
}

void CPU::ARM32Recompiler::TestInterrupts(const vixl::aarch32::Register& sr)
{
  // if Iec == 0 then goto no_interrupt
  Label no_interrupt;
  armAsm->tst(sr, 1);
  armAsm->b(eq, &no_interrupt);

  // sr & cause
  armAsm->ldr(RSCRATCH, PTR(&g_state.cop0_regs.cause.bits));
  armAsm->and_(sr, sr, RSCRATCH);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  armAsm->tst(sr, 0xFF00);

  SwitchToFarCode(true, ne);
  BackupHostState();

  // Update load delay, this normally happens at the end of an instruction, but we're finishing it early.
  UpdateLoadDelay();

  Flush(FLUSH_END_BLOCK | FLUSH_FOR_EXCEPTION | FLUSH_FOR_C_CALL);

  // Can't use EndBlockWithException() here, because it'll use the wrong PC.
  // Can't use RaiseException() on the fast path if we're the last instruction, because the next PC is unknown.
  if (!iinfo->is_last_instruction)
  {
    EmitMov(RARG1, Cop0Registers::CAUSE::MakeValueForException(Exception::INT, iinfo->is_branch_instruction, false,
                                                               (inst + 1)->cop.cop_n));
    EmitMov(RARG2, m_compiler_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, true, false);
  }
  else
  {
    EmitMov(RARG1, 0);
    if (m_dirty_pc)
      EmitMov(RARG2, m_compiler_pc);
    armAsm->str(RARG1, PTR(&g_state.downcount));
    if (m_dirty_pc)
      armAsm->str(RARG2, PTR(&g_state.pc));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, false, true);
  }

  RestoreHostState();
  SwitchToNearCode(false);

  armAsm->bind(&no_interrupt);
}

void CPU::ARM32Recompiler::Compile_mfc2(CompileFlags cf)
{
  const u32 index = inst->cop.Cop2Index();
  const Reg rt = inst->r.rt;

  const auto [ptr, action] = GetGTERegisterPointer(index, false);
  if (action == GTERegisterAccessAction::Ignore)
    return;

  u32 hreg;
  if (action == GTERegisterAccessAction::Direct)
  {
    hreg = AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                           EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, rt);
    armAsm->ldr(Register(hreg), PTR(ptr));
  }
  else if (action == GTERegisterAccessAction::CallHandler)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, index);
    EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));

    hreg = AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                           EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, rt);
    armAsm->mov(Register(hreg), RRET);
  }
  else
  {
    Panic("Unknown action");
    return;
  }

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, inst->bits);
    armAsm->mov(RARG2, Register(hreg));
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_MFC2));
  }
}

void CPU::ARM32Recompiler::Compile_mtc2(CompileFlags cf)
{
  const u32 index = inst->cop.Cop2Index();
  const auto [ptr, action] = GetGTERegisterPointer(index, true);
  if (action == GTERegisterAccessAction::Ignore)
    return;

  if (action == GTERegisterAccessAction::Direct)
  {
    if (cf.const_t)
      StoreConstantToCPUPointer(GetConstantRegU32(cf.MipsT()), ptr);
    else
      armAsm->str(CFGetRegT(cf), PTR(ptr));
  }
  else if (action == GTERegisterAccessAction::SignExtend16 || action == GTERegisterAccessAction::ZeroExtend16)
  {
    const bool sign = (action == GTERegisterAccessAction::SignExtend16);
    if (cf.valid_host_t)
    {
      sign ? armAsm->sxth(RARG1, CFGetRegT(cf)) : armAsm->uxth(RARG1, CFGetRegT(cf));
      armAsm->str(RARG1, PTR(ptr));
    }
    else if (cf.const_t)
    {
      const u16 cv = Truncate16(GetConstantRegU32(cf.MipsT()));
      StoreConstantToCPUPointer(sign ? ::SignExtend32(cv) : ::ZeroExtend32(cv), ptr);
    }
    else
    {
      Panic("Unsupported setup");
    }
  }
  else if (action == GTERegisterAccessAction::CallHandler)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, index);
    MoveTToReg(RARG2, cf);
    EmitCall(reinterpret_cast<const void*>(&GTE::WriteRegister));
  }
  else if (action == GTERegisterAccessAction::PushFIFO)
  {
    // SXY0 <- SXY1
    // SXY1 <- SXY2
    // SXY2 <- SXYP
    DebugAssert(RRET.GetCode() != RARG2.GetCode() && RRET.GetCode() != RARG3.GetCode());
    armAsm->ldr(RARG2, PTR(&g_state.gte_regs.SXY1[0]));
    armAsm->ldr(RARG3, PTR(&g_state.gte_regs.SXY2[0]));
    armAsm->str(RARG2, PTR(&g_state.gte_regs.SXY0[0]));
    armAsm->str(RARG3, PTR(&g_state.gte_regs.SXY1[0]));
    if (cf.valid_host_t)
      armAsm->str(CFGetRegT(cf), PTR(&g_state.gte_regs.SXY2[0]));
    else if (cf.const_t)
      StoreConstantToCPUPointer(GetConstantRegU32(cf.MipsT()), &g_state.gte_regs.SXY2[0]);
    else
      Panic("Unsupported setup");
  }
  else
  {
    Panic("Unknown action");
  }
}

void CPU::ARM32Recompiler::Compile_cop2(CompileFlags cf)
{
  TickCount func_ticks;
  GTE::InstructionImpl func = GTE::GetInstructionImpl(inst->bits, &func_ticks);

  Flush(FLUSH_FOR_C_CALL);
  EmitMov(RARG1, inst->bits & GTE::Instruction::REQUIRED_BITS_MASK);
  EmitCall(reinterpret_cast<const void*>(func));

  AddGTETicks(func_ticks);
}

u32 CPU::Recompiler::CompileLoadStoreThunk(void* thunk_code, u32 thunk_space, void* code_address, u32 code_size,
                                           TickCount cycles_to_add, TickCount cycles_to_remove, u32 gpr_bitmask,
                                           u8 address_register, u8 data_register, MemoryAccessSize size, bool is_signed,
                                           bool is_load)
{
  Assembler arm_asm(static_cast<u8*>(thunk_code), thunk_space);
  Assembler* armAsm = &arm_asm;

#ifdef VIXL_DEBUG
  vixl::CodeBufferCheckScope asm_check(armAsm, thunk_space, vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
#endif

  // save regs
  RegisterList save_regs;

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    if ((gpr_bitmask & (1u << i)) && armIsCallerSavedRegister(i) && (!is_load || data_register != i))
      save_regs.Combine(RegisterList(Register(i)));
  }

  if (!save_regs.IsEmpty())
    armAsm->push(save_regs);

  if (address_register != static_cast<u8>(RARG1.GetCode()))
    armAsm->mov(RARG1, Register(address_register));

  if (!is_load)
  {
    if (data_register != static_cast<u8>(RARG2.GetCode()))
      armAsm->mov(RARG2, Register(data_register));
  }

  if (cycles_to_add != 0)
  {
    // NOTE: we have to reload here, because memory writes can run DMA, which can screw with cycles
    armAsm->ldr(RARG3, PTR(&g_state.pending_ticks));
    if (!ImmediateA32::IsImmediateA32(cycles_to_add))
    {
      armEmitMov(armAsm, RSCRATCH, cycles_to_add);
      armAsm->add(RARG3, RARG3, RSCRATCH);
    }
    else
    {
      armAsm->add(RARG3, RARG3, cycles_to_add);
    }

    armAsm->str(RARG3, PTR(&g_state.pending_ticks));
  }

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      armEmitCall(armAsm,
                  is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryByte) :
                            reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryByte),
                  false);
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      armEmitCall(armAsm,
                  is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryHalfWord) :
                            reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryHalfWord),
                  false);
    }
    break;
    case MemoryAccessSize::Word:
    {
      armEmitCall(armAsm,
                  is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryWord) :
                            reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryWord),
                  false);
    }
    break;
  }

  if (is_load)
  {
    const Register dst = Register(data_register);
    switch (size)
    {
      case MemoryAccessSize::Byte:
      {
        is_signed ? armAsm->sxtb(dst, RRET) : armAsm->uxtb(dst, RRET);
      }
      break;
      case MemoryAccessSize::HalfWord:
      {
        is_signed ? armAsm->sxth(dst, RRET) : armAsm->uxth(dst, RRET);
      }
      break;
      case MemoryAccessSize::Word:
      {
        if (dst.GetCode() != RRET.GetCode())
          armAsm->mov(dst, RRET);
      }
      break;
    }
  }

  if (cycles_to_remove != 0)
  {
    armAsm->ldr(RARG3, PTR(&g_state.pending_ticks));
    if (!ImmediateA32::IsImmediateA32(cycles_to_remove))
    {
      armEmitMov(armAsm, RSCRATCH, cycles_to_remove);
      armAsm->sub(RARG3, RARG3, RSCRATCH);
    }
    else
    {
      armAsm->sub(RARG3, RARG3, cycles_to_remove);
    }
    armAsm->str(RARG3, PTR(&g_state.pending_ticks));
  }

  // restore regs
  if (!save_regs.IsEmpty())
    armAsm->pop(save_regs);

  armEmitJmp(armAsm, static_cast<const u8*>(code_address) + code_size, true);
  armAsm->FinalizeCode();

  return static_cast<u32>(armAsm->GetCursorOffset());
}

#endif // CPU_ARCH_ARM32
