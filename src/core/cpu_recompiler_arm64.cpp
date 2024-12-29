// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_recompiler_arm64.h"
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

#ifdef CPU_ARCH_ARM64

#include "vixl/aarch64/constants-aarch64.h"

#ifdef ENABLE_HOST_DISASSEMBLY
#include "vixl/aarch64/disasm-aarch64.h"
#endif

LOG_CHANNEL(Recompiler);

#define PTR(x) vixl::aarch64::MemOperand(RSTATE, (((u8*)(x)) - ((u8*)&g_state)))

#define RWRET vixl::aarch64::w0
#define RXRET vixl::aarch64::x0
#define RWARG1 vixl::aarch64::w0
#define RXARG1 vixl::aarch64::x0
#define RWARG2 vixl::aarch64::w1
#define RXARG2 vixl::aarch64::x1
#define RWARG3 vixl::aarch64::w2
#define RXARG3 vixl::aarch64::x2
#define RWSCRATCH vixl::aarch64::w16
#define RXSCRATCH vixl::aarch64::x16
#define RSTATE vixl::aarch64::x19
#define RMEMBASE vixl::aarch64::x20

static bool armIsCallerSavedRegister(u32 id);
static s64 armGetPCDisplacement(const void* current, const void* target);
static bool armIsInAdrpRange(vixl::aarch64::Assembler* armAsm, const void* addr);
static void armMoveAddressToReg(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg, const void* addr);
static void armEmitMov(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& rd, u64 imm);
static void armEmitJmp(vixl::aarch64::Assembler* armAsm, const void* ptr, bool force_inline);
static void armEmitCall(vixl::aarch64::Assembler* armAsm, const void* ptr, bool force_inline);
static void armEmitCondBranch(vixl::aarch64::Assembler* armAsm, vixl::aarch64::Condition cond, const void* ptr);
static void armEmitFarLoad(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg, const void* addr,
                           bool sign_extend_word = false);
static void armEmitFarStore(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg, const void* addr,
                            const vixl::aarch64::Register& tempreg = RXSCRATCH);
static u8* armGetJumpTrampoline(const void* target);
static void armAlignCode(vixl::aarch64::Assembler* armAsm, size_t alignment);

static constexpr u32 TRAMPOLINE_AREA_SIZE = 4 * 1024;
static std::unordered_map<const void*, u32> s_trampoline_targets;
static u8* s_trampoline_start_ptr = nullptr;
static u32 s_trampoline_used = 0;

namespace CPU {

using namespace vixl::aarch64;

static ARM64Recompiler s_instance;
Recompiler* g_compiler = &s_instance;

} // namespace CPU

bool armIsCallerSavedRegister(u32 id)
{
  // same on both linux and windows
  return (id <= 18);
}

void armEmitMov(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& rd, u64 imm)
{
  // From vixl macro assembler.
  DebugAssert(vixl::IsUint32(imm) || vixl::IsInt32(imm) || rd.Is64Bits());
  DebugAssert(rd.GetCode() != vixl::aarch64::sp.GetCode());

  if (imm == 0)
  {
    armAsm->mov(rd, vixl::aarch64::Assembler::AppropriateZeroRegFor(rd));
    return;
  }

  // The worst case for size is mov 64-bit immediate to sp:
  //  * up to 4 instructions to materialise the constant
  //  * 1 instruction to move to sp

  // Immediates on Aarch64 can be produced using an initial value, and zero to
  // three move keep operations.
  //
  // Initial values can be generated with:
  //  1. 64-bit move zero (movz).
  //  2. 32-bit move inverted (movn).
  //  3. 64-bit move inverted.
  //  4. 32-bit orr immediate.
  //  5. 64-bit orr immediate.
  // Move-keep may then be used to modify each of the 16-bit half words.
  //
  // The code below supports all five initial value generators, and
  // applying move-keep operations to move-zero and move-inverted initial
  // values.

  // Try to move the immediate in one instruction, and if that fails, switch to
  // using multiple instructions.
  const unsigned reg_size = rd.GetSizeInBits();

  if (vixl::aarch64::Assembler::IsImmMovz(imm, reg_size) && !rd.IsSP())
  {
    // Immediate can be represented in a move zero instruction. Movz can't write
    // to the stack pointer.
    armAsm->movz(rd, imm);
    return;
  }
  else if (vixl::aarch64::Assembler::IsImmMovn(imm, reg_size) && !rd.IsSP())
  {
    // Immediate can be represented in a move negative instruction. Movn can't
    // write to the stack pointer.
    armAsm->movn(rd, rd.Is64Bits() ? ~imm : (~imm & vixl::aarch64::kWRegMask));
    return;
  }
  else if (vixl::aarch64::Assembler::IsImmLogical(imm, reg_size))
  {
    // Immediate can be represented in a logical orr instruction.
    DebugAssert(!rd.IsZero());
    armAsm->orr(rd, vixl::aarch64::Assembler::AppropriateZeroRegFor(rd), imm);
    return;
  }

  // Generic immediate case. Imm will be represented by
  //   [imm3, imm2, imm1, imm0], where each imm is 16 bits.
  // A move-zero or move-inverted is generated for the first non-zero or
  // non-0xffff immX, and a move-keep for subsequent non-zero immX.

  uint64_t ignored_halfword = 0;
  bool invert_move = false;
  // If the number of 0xffff halfwords is greater than the number of 0x0000
  // halfwords, it's more efficient to use move-inverted.
  if (vixl::CountClearHalfWords(~imm, reg_size) > vixl::CountClearHalfWords(imm, reg_size))
  {
    ignored_halfword = 0xffff;
    invert_move = true;
  }

  // Iterate through the halfwords. Use movn/movz for the first non-ignored
  // halfword, and movk for subsequent halfwords.
  DebugAssert((reg_size % 16) == 0);
  bool first_mov_done = false;
  for (unsigned i = 0; i < (reg_size / 16); i++)
  {
    uint64_t imm16 = (imm >> (16 * i)) & 0xffff;
    if (imm16 != ignored_halfword)
    {
      if (!first_mov_done)
      {
        if (invert_move)
          armAsm->movn(rd, ~imm16 & 0xffff, 16 * i);
        else
          armAsm->movz(rd, imm16, 16 * i);
        first_mov_done = true;
      }
      else
      {
        // Construct a wider constant.
        armAsm->movk(rd, imm16, 16 * i);
      }
    }
  }

  DebugAssert(first_mov_done);
}

s64 armGetPCDisplacement(const void* current, const void* target)
{
  // pxAssert(Common::IsAlignedPow2(reinterpret_cast<size_t>(current), 4));
  // pxAssert(Common::IsAlignedPow2(reinterpret_cast<size_t>(target), 4));
  return static_cast<s64>((reinterpret_cast<ptrdiff_t>(target) - reinterpret_cast<ptrdiff_t>(current)) >> 2);
}

bool armIsInAdrpRange(vixl::aarch64::Assembler* armAsm, const void* addr)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  const void* current_code_ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(cur) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);

  return (vixl::IsInt21(page_displacement) && (vixl::aarch64::Assembler::IsImmAddSub(page_offset) ||
                                               vixl::aarch64::Assembler::IsImmLogical(page_offset, 64)));
}

void armMoveAddressToReg(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg, const void* addr)
{
  DebugAssert(reg.IsX());

  const void* cur = armAsm->GetCursorAddress<const void*>();
  const void* current_code_ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(cur) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);
  if (vixl::IsInt21(page_displacement) && vixl::aarch64::Assembler::IsImmAddSub(page_offset))
  {
    armAsm->adrp(reg, page_displacement);
    armAsm->add(reg, reg, page_offset);
  }
  else if (vixl::IsInt21(page_displacement) && vixl::aarch64::Assembler::IsImmLogical(page_offset, 64))
  {
    armAsm->adrp(reg, page_displacement);
    armAsm->orr(reg, reg, page_offset);
  }
  else
  {
    armEmitMov(armAsm, reg, reinterpret_cast<uintptr_t>(addr));
  }
}

void armEmitJmp(vixl::aarch64::Assembler* armAsm, const void* ptr, bool force_inline)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  s64 displacement = armGetPCDisplacement(cur, ptr);
  bool use_blr = !vixl::IsInt26(displacement);
  bool use_trampoline = use_blr && !armIsInAdrpRange(armAsm, ptr);
  if (use_blr && use_trampoline && !force_inline)
  {
    if (u8* trampoline = armGetJumpTrampoline(ptr); trampoline)
    {
      displacement = armGetPCDisplacement(cur, trampoline);
      use_blr = !vixl::IsInt26(displacement);
    }
  }

  if (use_blr)
  {
    armMoveAddressToReg(armAsm, RXSCRATCH, ptr);
    armAsm->br(RXSCRATCH);
  }
  else
  {
    armAsm->b(displacement);
  }
}

void armEmitCall(vixl::aarch64::Assembler* armAsm, const void* ptr, bool force_inline)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  s64 displacement = armGetPCDisplacement(cur, ptr);
  bool use_blr = !vixl::IsInt26(displacement);
  bool use_trampoline = use_blr && !armIsInAdrpRange(armAsm, ptr);
  if (use_blr && use_trampoline && !force_inline)
  {
    if (u8* trampoline = armGetJumpTrampoline(ptr); trampoline)
    {
      displacement = armGetPCDisplacement(cur, trampoline);
      use_blr = !vixl::IsInt26(displacement);
    }
  }

  if (use_blr)
  {
    armMoveAddressToReg(armAsm, RXSCRATCH, ptr);
    armAsm->blr(RXSCRATCH);
  }
  else
  {
    armAsm->bl(displacement);
  }
}

void armEmitCondBranch(vixl::aarch64::Assembler* armAsm, vixl::aarch64::Condition cond, const void* ptr)
{
  const s64 jump_distance = static_cast<s64>(reinterpret_cast<intptr_t>(ptr) -
                                             reinterpret_cast<intptr_t>(armAsm->GetCursorAddress<const void*>()));
  // pxAssert(Common::IsAligned(jump_distance, 4));

  if (vixl::aarch64::Instruction::IsValidImmPCOffset(vixl::aarch64::CondBranchType, jump_distance >> 2))
  {
    armAsm->b(jump_distance >> 2, cond);
  }
  else
  {
    vixl::aarch64::Label branch_not_taken;
    armAsm->b(&branch_not_taken, InvertCondition(cond));

    const s64 new_jump_distance = static_cast<s64>(reinterpret_cast<intptr_t>(ptr) -
                                                   reinterpret_cast<intptr_t>(armAsm->GetCursorAddress<const void*>()));
    armAsm->b(new_jump_distance >> 2);
    armAsm->bind(&branch_not_taken);
  }
}

void armEmitFarLoad(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg, const void* addr,
                    bool sign_extend_word)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  const void* current_code_ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(cur) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);
  vixl::aarch64::MemOperand memop;

  const vixl::aarch64::Register xreg = reg.X();
  if (vixl::IsInt21(page_displacement))
  {
    armAsm->adrp(xreg, page_displacement);
    memop = vixl::aarch64::MemOperand(xreg, static_cast<int64_t>(page_offset));
  }
  else
  {
    armMoveAddressToReg(armAsm, xreg, addr);
    memop = vixl::aarch64::MemOperand(xreg);
  }

  if (sign_extend_word)
    armAsm->ldrsw(reg, memop);
  else
    armAsm->ldr(reg, memop);
}

[[maybe_unused]] void armEmitFarStore(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg,
                                      const void* addr, const vixl::aarch64::Register& tempreg)
{
  DebugAssert(tempreg.IsX());

  const void* cur = armAsm->GetCursorAddress<const void*>();
  const void* current_code_ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(cur) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);

  if (vixl::IsInt21(page_displacement))
  {
    armAsm->adrp(tempreg, page_displacement);
    armAsm->str(reg, vixl::aarch64::MemOperand(tempreg, static_cast<int64_t>(page_offset)));
  }
  else
  {
    armMoveAddressToReg(armAsm, tempreg, addr);
    armAsm->str(reg, vixl::aarch64::MemOperand(tempreg));
  }
}

u8* armGetJumpTrampoline(const void* target)
{
  auto it = s_trampoline_targets.find(target);
  if (it != s_trampoline_targets.end())
    return s_trampoline_start_ptr + it->second;

  // align to 16 bytes?
  const u32 offset = Common::AlignUpPow2(s_trampoline_used, CPU::Recompiler::FUNCTION_ALIGNMENT);

  // 4 movs plus a jump
  if (TRAMPOLINE_AREA_SIZE - offset < 20)
  {
    Panic("Ran out of space in constant pool");
    return nullptr;
  }

  u8* start = s_trampoline_start_ptr + offset;
  vixl::aarch64::Assembler armAsm(start, TRAMPOLINE_AREA_SIZE - offset);
#ifdef VIXL_DEBUG
  vixl::CodeBufferCheckScope armAsmCheck(&armAsm, TRAMPOLINE_AREA_SIZE - offset,
                                         vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
#endif
  armMoveAddressToReg(&armAsm, RXSCRATCH, target);
  armAsm.br(RXSCRATCH);
  armAsm.FinalizeCode();

  const u32 size = static_cast<u32>(armAsm.GetSizeOfCodeGenerated());
  DebugAssert(size < 20);
  s_trampoline_targets.emplace(target, offset);
  s_trampoline_used = offset + static_cast<u32>(size);

  MemMap::FlushInstructionCache(start, size);
  return start;
}

void armAlignCode(vixl::aarch64::Assembler* armAsm, size_t alignment)
{
  size_t addr = armAsm->GetCursorAddress<size_t>();
  const size_t end_addr = Common::AlignUpPow2(addr, alignment);
  while (addr != end_addr)
  {
    armAsm->nop();
    addr += vixl::aarch64::kInstructionSize;
  }
}

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  class MyDisassembler : public vixl::aarch64::Disassembler
  {
  protected:
    void ProcessOutput(const vixl::aarch64::Instruction* instr) override
    {
      DEBUG_LOG("0x{:016X}  {:08X}\t\t{}", reinterpret_cast<uint64_t>(instr), instr->GetInstructionBits(), GetOutput());
    }
  };

  vixl::aarch64::Decoder decoder;
  MyDisassembler disas;
  decoder.AppendVisitor(&disas);
  decoder.Decode(static_cast<const vixl::aarch64::Instruction*>(start),
                 reinterpret_cast<const vixl::aarch64::Instruction*>(static_cast<const u8*>(start) + size));
#else
  ERROR_LOG("Not compiled with ENABLE_HOST_DISASSEMBLY.");
#endif
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
  return size / vixl::aarch64::kInstructionSize;
}

u32 CPU::CodeCache::EmitJump(void* code, const void* dst, bool flush_icache)
{
  using namespace vixl::aarch64;

  const s64 disp = armGetPCDisplacement(code, dst);
  DebugAssert(vixl::IsInt26(disp));

  const u32 new_code = B | Assembler::ImmUncondBranch(disp);
  std::memcpy(code, &new_code, sizeof(new_code));
  if (flush_icache)
    MemMap::FlushInstructionCache(code, kInstructionSize);

  return kInstructionSize;
}

u32 CPU::CodeCache::EmitASMFunctions(void* code, u32 code_size)
{
  using namespace vixl::aarch64;

  Assembler actual_asm(static_cast<u8*>(code), code_size);
  Assembler* RESTRICT armAsm = &actual_asm;

#ifdef VIXL_DEBUG
  vixl::CodeBufferCheckScope asm_check(armAsm, code_size, vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
#endif

  Label dispatch;

  g_enter_recompiler = armAsm->GetCursorAddress<decltype(g_enter_recompiler)>();
  {
    // Need the CPU state for basically everything :-)
    armMoveAddressToReg(armAsm, RSTATE, &g_state);

    // Fastmem setup, oldrec doesn't need it
    if (IsUsingFastmem())
      armAsm->ldr(RMEMBASE, PTR(&g_state.fastmem_base));

    // Fall through to event dispatcher
  }

  // check events then for frame done
  armAlignCode(armAsm, Recompiler::FUNCTION_ALIGNMENT);
  g_check_events_and_dispatch = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));
    armAsm->ldr(RWARG2, PTR(&g_state.downcount));
    armAsm->cmp(RWARG1, RWARG2);
    armAsm->b(&dispatch, lt);

    g_run_events_and_dispatch = armAsm->GetCursorAddress<const void*>();
    armEmitCall(armAsm, reinterpret_cast<const void*>(&TimingEvents::RunEvents), true);
  }

  armAlignCode(armAsm, Recompiler::FUNCTION_ALIGNMENT);
  g_dispatcher = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->bind(&dispatch);

    // x9 <- s_fast_map[pc >> 16]
    armAsm->ldr(RWARG1, PTR(&g_state.pc));
    armMoveAddressToReg(armAsm, RXARG3, g_code_lut.data());
    armAsm->lsr(RWARG2, RWARG1, 16);
    armAsm->ubfx(RWARG1, RWARG1, 2, 14);
    armAsm->ldr(RXARG2, MemOperand(RXARG3, RXARG2, LSL, 3));

    // blr(x9[pc * 2]) (fast_map[pc >> 2])
    armAsm->ldr(RXARG1, MemOperand(RXARG2, RXARG1, LSL, 3));
    armAsm->br(RXARG1);
  }

  armAlignCode(armAsm, Recompiler::FUNCTION_ALIGNMENT);
  g_compile_or_revalidate_block = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->ldr(RWARG1, PTR(&g_state.pc));
    armEmitCall(armAsm, reinterpret_cast<const void*>(&CompileOrRevalidateBlock), true);
    armAsm->b(&dispatch);
  }

  armAlignCode(armAsm, Recompiler::FUNCTION_ALIGNMENT);
  g_discard_and_recompile_block = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->ldr(RWARG1, PTR(&g_state.pc));
    armEmitCall(armAsm, reinterpret_cast<const void*>(&DiscardAndRecompileBlock), true);
    armAsm->b(&dispatch);
  }

  armAlignCode(armAsm, Recompiler::FUNCTION_ALIGNMENT);
  g_interpret_block = armAsm->GetCursorAddress<const void*>();
  {
    armEmitCall(armAsm, reinterpret_cast<const void*>(GetInterpretUncachedBlockFunction()), true);
    armAsm->b(&dispatch);
  }

  armAsm->FinalizeCode();

  s_trampoline_targets.clear();
  s_trampoline_start_ptr = static_cast<u8*>(code) + armAsm->GetCursorOffset();
  s_trampoline_used = 0;

  return static_cast<u32>(armAsm->GetCursorOffset()) + TRAMPOLINE_AREA_SIZE;
}

void CPU::CodeCache::EmitAlignmentPadding(void* dst, size_t size)
{
  constexpr u8 padding_value = 0x00;
  std::memset(dst, padding_value, size);
}

CPU::ARM64Recompiler::ARM64Recompiler() : m_emitter(PositionDependentCode), m_far_emitter(PositionIndependentCode)
{
}

CPU::ARM64Recompiler::~ARM64Recompiler() = default;

const void* CPU::ARM64Recompiler::GetCurrentCodePointer()
{
  return armAsm->GetCursorAddress<const void*>();
}

void CPU::ARM64Recompiler::Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space, u8* far_code_buffer,
                                 u32 far_code_space)
{
  Recompiler::Reset(block, code_buffer, code_buffer_space, far_code_buffer, far_code_space);

  // TODO: don't recreate this every time..
  DebugAssert(!armAsm);
  m_emitter.GetBuffer()->Reset(code_buffer, code_buffer_space);
  m_far_emitter.GetBuffer()->Reset(far_code_buffer, far_code_space);
  armAsm = &m_emitter;

#ifdef VIXL_DEBUG
  m_emitter_check = std::make_unique<vixl::CodeBufferCheckScope>(&m_emitter, code_buffer_space,
                                                                 vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
  m_far_emitter_check = std::make_unique<vixl::CodeBufferCheckScope>(
    &m_far_emitter, far_code_space, vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
#endif

  // Need to wipe it out so it's correct when toggling fastmem.
  m_host_regs = {};

  const u32 membase_idx = CodeCache::IsUsingFastmem() ? RMEMBASE.GetCode() : NUM_HOST_REGS;
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];

    if (i == RWARG1.GetCode() || i == RWARG1.GetCode() || i == RWARG2.GetCode() || i == RWARG3.GetCode() ||
        i == RWSCRATCH.GetCode() || i == RSTATE.GetCode() || i == membase_idx || i == x18.GetCode() || i >= 30)
    {
      continue;
    }

    ra.flags = HR_USABLE | (armIsCallerSavedRegister(i) ? 0 : HR_CALLEE_SAVED);
  }
}

void CPU::ARM64Recompiler::SwitchToFarCode(bool emit_jump, vixl::aarch64::Condition cond)
{
  DebugAssert(armAsm == &m_emitter);
  if (emit_jump)
  {
    const s64 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>());
    if (cond != Condition::al)
    {
      if (vixl::IsInt19(disp))
      {
        armAsm->b(disp, cond);
      }
      else
      {
        Label skip;
        armAsm->b(&skip, vixl::aarch64::InvertCondition(cond));
        armAsm->b(armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>()));
        armAsm->bind(&skip);
      }
    }
    else
    {
      armAsm->b(disp);
    }
  }
  armAsm = &m_far_emitter;
}

void CPU::ARM64Recompiler::SwitchToFarCodeIfBitSet(const vixl::aarch64::Register& reg, u32 bit)
{
  const s64 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>());
  if (vixl::IsInt14(disp))
  {
    armAsm->tbnz(reg, bit, disp);
  }
  else
  {
    Label skip;
    armAsm->tbz(reg, bit, &skip);
    armAsm->b(armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>()));
    armAsm->bind(&skip);
  }

  armAsm = &m_far_emitter;
}

void CPU::ARM64Recompiler::SwitchToFarCodeIfRegZeroOrNonZero(const vixl::aarch64::Register& reg, bool nonzero)
{
  const s64 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>());
  if (vixl::IsInt19(disp))
  {
    nonzero ? armAsm->cbnz(reg, disp) : armAsm->cbz(reg, disp);
  }
  else
  {
    Label skip;
    nonzero ? armAsm->cbz(reg, &skip) : armAsm->cbnz(reg, &skip);
    armAsm->b(armGetPCDisplacement(GetCurrentCodePointer(), m_far_emitter.GetCursorAddress<const void*>()));
    armAsm->bind(&skip);
  }

  armAsm = &m_far_emitter;
}

void CPU::ARM64Recompiler::SwitchToNearCode(bool emit_jump, vixl::aarch64::Condition cond)
{
  DebugAssert(armAsm == &m_far_emitter);
  if (emit_jump)
  {
    const s64 disp = armGetPCDisplacement(GetCurrentCodePointer(), m_emitter.GetCursorAddress<const void*>());
    (cond != Condition::al) ? armAsm->b(disp, cond) : armAsm->b(disp);
  }
  armAsm = &m_emitter;
}

void CPU::ARM64Recompiler::EmitMov(const vixl::aarch64::Register& dst, u32 val)
{
  armEmitMov(armAsm, dst, val);
}

void CPU::ARM64Recompiler::EmitCall(const void* ptr, bool force_inline /*= false*/)
{
  armEmitCall(armAsm, ptr, force_inline);
}

vixl::aarch64::Operand CPU::ARM64Recompiler::armCheckAddSubConstant(s32 val)
{
  if (Assembler::IsImmAddSub(val))
    return vixl::aarch64::Operand(static_cast<int64_t>(val));

  EmitMov(RWSCRATCH, static_cast<u32>(val));
  return vixl::aarch64::Operand(RWSCRATCH);
}

vixl::aarch64::Operand CPU::ARM64Recompiler::armCheckAddSubConstant(u32 val)
{
  return armCheckAddSubConstant(static_cast<s32>(val));
}

vixl::aarch64::Operand CPU::ARM64Recompiler::armCheckCompareConstant(s32 val)
{
  if (Assembler::IsImmConditionalCompare(val))
    return vixl::aarch64::Operand(static_cast<int64_t>(val));

  EmitMov(RWSCRATCH, static_cast<u32>(val));
  return vixl::aarch64::Operand(RWSCRATCH);
}

vixl::aarch64::Operand CPU::ARM64Recompiler::armCheckLogicalConstant(u32 val)
{
  if (Assembler::IsImmLogical(val, 32))
    return vixl::aarch64::Operand(static_cast<s64>(static_cast<u64>(val)));

  EmitMov(RWSCRATCH, val);
  return vixl::aarch64::Operand(RWSCRATCH);
}

void CPU::ARM64Recompiler::BeginBlock()
{
  Recompiler::BeginBlock();
}

void CPU::ARM64Recompiler::GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size)
{
  // store it first to reduce code size, because we can offset
  armMoveAddressToReg(armAsm, RXARG1, ram_ptr);
  armMoveAddressToReg(armAsm, RXARG2, shadow_ptr);

  bool first = true;
  u32 offset = 0;
  Label block_changed;

  while (size >= 16)
  {
    const VRegister vtmp = v2.V4S();
    const VRegister dst = first ? v0.V4S() : v1.V4S();
    armAsm->ldr(dst, MemOperand(RXARG1, offset));
    armAsm->ldr(vtmp, MemOperand(RXARG2, offset));
    armAsm->cmeq(dst, dst, vtmp);
    if (!first)
      armAsm->and_(v0.V16B(), v0.V16B(), dst.V16B());
    else
      first = false;

    offset += 16;
    size -= 16;
  }

  if (!first)
  {
    // TODO: make sure this doesn't choke on ffffffff
    armAsm->uminv(s0, v0.V4S());
    armAsm->fcmp(s0, 0.0);
    armAsm->b(&block_changed, eq);
  }

  while (size >= 8)
  {
    armAsm->ldr(RXARG3, MemOperand(RXARG1, offset));
    armAsm->ldr(RXSCRATCH, MemOperand(RXARG2, offset));
    armAsm->cmp(RXARG3, RXSCRATCH);
    armAsm->b(&block_changed, ne);
    offset += 8;
    size -= 8;
  }

  while (size >= 4)
  {
    armAsm->ldr(RWARG3, MemOperand(RXARG1, offset));
    armAsm->ldr(RWSCRATCH, MemOperand(RXARG2, offset));
    armAsm->cmp(RWARG3, RWSCRATCH);
    armAsm->b(&block_changed, ne);
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

void CPU::ARM64Recompiler::GenerateICacheCheckAndUpdate()
{
  if (!m_block->HasFlag(CodeCache::BlockFlags::IsUsingICache))
  {
    if (m_block->HasFlag(CodeCache::BlockFlags::NeedsDynamicFetchTicks))
    {
      armEmitFarLoad(armAsm, RWARG2, GetFetchMemoryAccessTimePtr());
      armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));
      armEmitMov(armAsm, RWARG3, m_block->size);
      armAsm->mul(RWARG2, RWARG2, RWARG3);
      armAsm->add(RWARG1, RWARG1, RWARG2);
      armAsm->str(RWARG1, PTR(&g_state.pending_ticks));
    }
    else
    {
      armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));
      armAsm->add(RWARG1, RWARG1, armCheckAddSubConstant(static_cast<u32>(m_block->uncached_fetch_ticks)));
      armAsm->str(RWARG1, PTR(&g_state.pending_ticks));
    }
  }
  else if (m_block->icache_line_count > 0)
  {
    const auto& ticks_reg = RWARG1;
    const auto& current_tag_reg = RWARG2;
    const auto& existing_tag_reg = RWARG3;
    const auto& fill_ticks_reg = w4;
    const auto& ticks_to_add_reg = w5;

    VirtualMemoryAddress current_pc = m_block->pc & ICACHE_TAG_ADDRESS_MASK;
    const TickCount fill_ticks = GetICacheFillTicks(current_pc);
    if (fill_ticks <= 0)
      return;

    armAsm->ldr(ticks_reg, PTR(&g_state.pending_ticks));
    armEmitMov(armAsm, current_tag_reg, current_pc);
    armEmitMov(armAsm, fill_ticks_reg, fill_ticks);

    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const u32 line = GetICacheLine(current_pc);
      const u32 offset = OFFSETOF(State, icache_tags) + (line * sizeof(u32));

      Label cache_hit;
      armAsm->ldr(existing_tag_reg, MemOperand(RSTATE, offset));
      armAsm->str(current_tag_reg, MemOperand(RSTATE, offset));
      armAsm->cmp(existing_tag_reg, current_tag_reg);
      armAsm->csel(ticks_to_add_reg, fill_ticks_reg, wzr, ne);
      armAsm->add(ticks_reg, ticks_reg, ticks_to_add_reg);

      if (i != (m_block->icache_line_count - 1))
        armAsm->add(current_tag_reg, current_tag_reg, armCheckAddSubConstant(ICACHE_LINE_SIZE));
    }

    armAsm->str(ticks_reg, PTR(&g_state.pending_ticks));
  }
}

void CPU::ARM64Recompiler::GenerateCall(const void* func, s32 arg1reg /*= -1*/, s32 arg2reg /*= -1*/,
                                        s32 arg3reg /*= -1*/)
{
  if (arg1reg >= 0 && arg1reg != static_cast<s32>(RXARG1.GetCode()))
    armAsm->mov(RXARG1, XRegister(arg1reg));
  if (arg2reg >= 0 && arg2reg != static_cast<s32>(RXARG2.GetCode()))
    armAsm->mov(RXARG2, XRegister(arg2reg));
  if (arg3reg >= 0 && arg3reg != static_cast<s32>(RXARG3.GetCode()))
    armAsm->mov(RXARG3, XRegister(arg3reg));
  EmitCall(func);
}

void CPU::ARM64Recompiler::EndBlock(const std::optional<u32>& newpc, bool do_event_test)
{
  if (newpc.has_value())
  {
    if (m_dirty_pc || m_compiler_pc != newpc)
    {
      EmitMov(RWSCRATCH, newpc.value());
      armAsm->str(RWSCRATCH, PTR(&g_state.pc));
    }
  }
  m_dirty_pc = false;

  // flush regs
  Flush(FLUSH_END_BLOCK);
  EndAndLinkBlock(newpc, do_event_test, false);
}

void CPU::ARM64Recompiler::EndBlockWithException(Exception excode)
{
  // flush regs, but not pc, it's going to get overwritten
  // flush cycles because of the GTE instruction stuff...
  Flush(FLUSH_END_BLOCK | FLUSH_FOR_EXCEPTION | FLUSH_FOR_C_CALL);

  // TODO: flush load delay
  // TODO: break for pcdrv

  EmitMov(RWARG1, Cop0Registers::CAUSE::MakeValueForException(excode, m_current_instruction_branch_delay_slot, false,
                                                              inst->cop.cop_n));
  EmitMov(RWARG2, m_current_instruction_pc);
  EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
  m_dirty_pc = false;

  EndAndLinkBlock(std::nullopt, true, false);
}

void CPU::ARM64Recompiler::EndAndLinkBlock(const std::optional<u32>& newpc, bool do_event_test, bool force_run_events)
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
    armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));
  if (do_event_test)
    armAsm->ldr(RWARG2, PTR(&g_state.downcount));
  if (cycles > 0)
    armAsm->add(RWARG1, RWARG1, armCheckAddSubConstant(cycles));
  if (m_gte_done_cycle > cycles)
  {
    armAsm->add(RWARG2, RWARG1, armCheckAddSubConstant(m_gte_done_cycle - cycles));
    armAsm->str(RWARG2, PTR(&g_state.gte_completion_tick));
  }
  if (do_event_test)
    armAsm->cmp(RWARG1, RWARG2);
  if (cycles > 0)
    armAsm->str(RWARG1, PTR(&g_state.pending_ticks));
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
    const void* target = (newpc.value() == m_block->pc) ?
                           CodeCache::CreateSelfBlockLink(m_block, armAsm->GetCursorAddress<void*>(),
                                                          armAsm->GetBuffer()->GetStartAddress<const void*>()) :
                           CodeCache::CreateBlockLink(m_block, armAsm->GetCursorAddress<void*>(), newpc.value());
    armEmitJmp(armAsm, target, true);
  }
}

const void* CPU::ARM64Recompiler::EndCompile(u32* code_size, u32* far_code_size)
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

const char* CPU::ARM64Recompiler::GetHostRegName(u32 reg) const
{
  static constexpr std::array<const char*, 32> reg64_names = {
    {"x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
     "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "fp",  "lr",  "sp"}};
  return (reg < reg64_names.size()) ? reg64_names[reg] : "UNKNOWN";
}

void CPU::ARM64Recompiler::LoadHostRegWithConstant(u32 reg, u32 val)
{
  EmitMov(WRegister(reg), val);
}

void CPU::ARM64Recompiler::LoadHostRegFromCPUPointer(u32 reg, const void* ptr)
{
  armAsm->ldr(WRegister(reg), PTR(ptr));
}

void CPU::ARM64Recompiler::StoreHostRegToCPUPointer(u32 reg, const void* ptr)
{
  armAsm->str(WRegister(reg), PTR(ptr));
}

void CPU::ARM64Recompiler::StoreConstantToCPUPointer(u32 val, const void* ptr)
{
  if (val == 0)
  {
    armAsm->str(wzr, PTR(ptr));
    return;
  }

  EmitMov(RWSCRATCH, val);
  armAsm->str(RWSCRATCH, PTR(ptr));
}

void CPU::ARM64Recompiler::CopyHostReg(u32 dst, u32 src)
{
  if (src != dst)
    armAsm->mov(WRegister(dst), WRegister(src));
}

void CPU::ARM64Recompiler::AssertRegOrConstS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s || cf.const_s);
}

void CPU::ARM64Recompiler::AssertRegOrConstT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t || cf.const_t);
}

vixl::aarch64::MemOperand CPU::ARM64Recompiler::MipsPtr(Reg r) const
{
  DebugAssert(r < Reg::count);
  return PTR(&g_state.regs.r[static_cast<u32>(r)]);
}

vixl::aarch64::Register CPU::ARM64Recompiler::CFGetRegD(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_d);
  return WRegister(cf.host_d);
}

vixl::aarch64::Register CPU::ARM64Recompiler::CFGetRegS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s);
  return WRegister(cf.host_s);
}

vixl::aarch64::Register CPU::ARM64Recompiler::CFGetRegT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t);
  return WRegister(cf.host_t);
}

vixl::aarch64::Register CPU::ARM64Recompiler::CFGetRegLO(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_lo);
  return WRegister(cf.host_lo);
}

vixl::aarch64::Register CPU::ARM64Recompiler::CFGetRegHI(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_hi);
  return WRegister(cf.host_hi);
}

void CPU::ARM64Recompiler::MoveSToReg(const vixl::aarch64::Register& dst, CompileFlags cf)
{
  DebugAssert(dst.IsW());
  if (cf.valid_host_s)
  {
    if (cf.host_s != dst.GetCode())
      armAsm->mov(dst, WRegister(cf.host_s));
  }
  else if (cf.const_s)
  {
    const u32 cv = GetConstantRegU32(cf.MipsS());
    if (cv == 0)
      armAsm->mov(dst, wzr);
    else
      EmitMov(dst, cv);
  }
  else
  {
    WARNING_LOG("Hit memory path in MoveSToReg() for {}", GetRegName(cf.MipsS()));
    armAsm->ldr(dst, PTR(&g_state.regs.r[cf.mips_s]));
  }
}

void CPU::ARM64Recompiler::MoveTToReg(const vixl::aarch64::Register& dst, CompileFlags cf)
{
  DebugAssert(dst.IsW());
  if (cf.valid_host_t)
  {
    if (cf.host_t != dst.GetCode())
      armAsm->mov(dst, WRegister(cf.host_t));
  }
  else if (cf.const_t)
  {
    const u32 cv = GetConstantRegU32(cf.MipsT());
    if (cv == 0)
      armAsm->mov(dst, wzr);
    else
      EmitMov(dst, cv);
  }
  else
  {
    WARNING_LOG("Hit memory path in MoveTToReg() for {}", GetRegName(cf.MipsT()));
    armAsm->ldr(dst, PTR(&g_state.regs.r[cf.mips_t]));
  }
}

void CPU::ARM64Recompiler::MoveMIPSRegToReg(const vixl::aarch64::Register& dst, Reg reg)
{
  DebugAssert(reg < Reg::count && dst.IsW());
  if (const std::optional<u32> hreg = CheckHostReg(0, Recompiler::HR_TYPE_CPU_REG, reg))
    armAsm->mov(dst, WRegister(hreg.value()));
  else if (HasConstantReg(reg))
    EmitMov(dst, GetConstantRegU32(reg));
  else
    armAsm->ldr(dst, MipsPtr(reg));
}

void CPU::ARM64Recompiler::GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val, Reg arg2reg /* = Reg::count */,
                                                        Reg arg3reg /* = Reg::count */)
{
  DebugAssert(g_settings.gpu_pgxp_enable);

  Flush(FLUSH_FOR_C_CALL);

  if (arg2reg != Reg::count)
    MoveMIPSRegToReg(RWARG2, arg2reg);
  if (arg3reg != Reg::count)
    MoveMIPSRegToReg(RWARG3, arg3reg);

  EmitMov(RWARG1, arg1val);
  EmitCall(func);
}

void CPU::ARM64Recompiler::Flush(u32 flags)
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
    EmitMov(RWARG1, inst->bits);
    EmitMov(RWARG2, m_current_instruction_pc);
    EmitMov(RWARG3, m_current_instruction_branch_delay_slot);
    armAsm->str(RWARG1, PTR(&g_state.current_instruction.bits));
    armAsm->str(RWARG2, PTR(&g_state.current_instruction_pc));
    armAsm->strb(RWARG3, PTR(&g_state.current_instruction_in_branch_delay_slot));
  }

  if (flags & FLUSH_LOAD_DELAY_FROM_STATE && m_load_delay_dirty)
  {
    // This sucks :(
    // TODO: make it a function?
    armAsm->ldrb(RWARG1, PTR(&g_state.load_delay_reg));
    armAsm->ldr(RWARG2, PTR(&g_state.load_delay_value));
    EmitMov(RWSCRATCH, OFFSETOF(CPU::State, regs.r[0]));
    armAsm->add(RWARG1, RWSCRATCH, vixl::aarch64::Operand(RWARG1, LSL, 2));
    armAsm->str(RWARG2, MemOperand(RSTATE, RXARG1));
    EmitMov(RWSCRATCH, static_cast<u8>(Reg::count));
    armAsm->strb(RWSCRATCH, PTR(&g_state.load_delay_reg));
    m_load_delay_dirty = false;
  }

  if (flags & FLUSH_LOAD_DELAY && m_load_delay_register != Reg::count)
  {
    if (m_load_delay_value_register != NUM_HOST_REGS)
      FreeHostReg(m_load_delay_value_register);

    EmitMov(RWSCRATCH, static_cast<u8>(m_load_delay_register));
    armAsm->strb(RWSCRATCH, PTR(&g_state.load_delay_reg));
    m_load_delay_register = Reg::count;
    m_load_delay_dirty = true;
  }

  if (flags & FLUSH_GTE_STALL_FROM_STATE && m_dirty_gte_done_cycle)
  {
    // May as well flush cycles while we're here.
    // GTE spanning blocks is very rare, we _could_ disable this for speed.
    armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));
    armAsm->ldr(RWARG2, PTR(&g_state.gte_completion_tick));
    if (m_cycles > 0)
    {
      armAsm->add(RWARG1, RWARG1, armCheckAddSubConstant(m_cycles));
      m_cycles = 0;
    }
    armAsm->cmp(RWARG2, RWARG1);
    armAsm->csel(RWARG1, RWARG2, RWARG1, hs);
    armAsm->str(RWARG1, PTR(&g_state.pending_ticks));
    m_dirty_gte_done_cycle = false;
  }

  if (flags & FLUSH_GTE_DONE_CYCLE && m_gte_done_cycle > m_cycles)
  {
    armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));

    // update cycles at the same time
    if (flags & FLUSH_CYCLES && m_cycles > 0)
    {
      armAsm->add(RWARG1, RWARG1, armCheckAddSubConstant(m_cycles));
      armAsm->str(RWARG1, PTR(&g_state.pending_ticks));
      m_gte_done_cycle -= m_cycles;
      m_cycles = 0;
    }

    armAsm->add(RWARG1, RWARG1, armCheckAddSubConstant(m_gte_done_cycle));
    armAsm->str(RWARG1, PTR(&g_state.gte_completion_tick));
    m_gte_done_cycle = 0;
    m_dirty_gte_done_cycle = true;
  }

  if (flags & FLUSH_CYCLES && m_cycles > 0)
  {
    armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));
    armAsm->add(RWARG1, RWARG1, armCheckAddSubConstant(m_cycles));
    armAsm->str(RWARG1, PTR(&g_state.pending_ticks));
    m_gte_done_cycle = std::max<TickCount>(m_gte_done_cycle - m_cycles, 0);
    m_cycles = 0;
  }
}

void CPU::ARM64Recompiler::Compile_Fallback()
{
  WARNING_LOG("Compiling instruction fallback at PC=0x{:08X}, instruction=0x{:08X}", m_current_instruction_pc,
              inst->bits);

  Flush(FLUSH_FOR_INTERPRETER);

  EmitCall(reinterpret_cast<const void*>(&CPU::RecompilerThunks::InterpretInstruction));

  // TODO: make me less garbage
  // TODO: this is wrong, it flushes the load delay on the same cycle when we return.
  // but nothing should be going through here..
  Label no_load_delay;
  armAsm->ldrb(RWARG1, PTR(&g_state.next_load_delay_reg));
  armAsm->cmp(RWARG1, static_cast<u8>(Reg::count));
  armAsm->b(&no_load_delay, eq);
  armAsm->ldr(RWARG2, PTR(&g_state.next_load_delay_value));
  armAsm->strb(RWARG1, PTR(&g_state.load_delay_reg));
  armAsm->str(RWARG2, PTR(&g_state.load_delay_value));
  EmitMov(RWARG1, static_cast<u32>(Reg::count));
  armAsm->strb(RWARG1, PTR(&g_state.next_load_delay_reg));
  armAsm->bind(&no_load_delay);

  m_load_delay_dirty = EMULATE_LOAD_DELAYS;
}

void CPU::ARM64Recompiler::CheckBranchTarget(const vixl::aarch64::Register& pcreg)
{
  DebugAssert(pcreg.IsW());
  if (!g_settings.cpu_recompiler_memory_exceptions)
    return;

  armAsm->tst(pcreg, armCheckLogicalConstant(0x3));
  SwitchToFarCode(true, ne);

  BackupHostState();
  EndBlockWithException(Exception::AdEL);

  RestoreHostState();
  SwitchToNearCode(false);
}

void CPU::ARM64Recompiler::Compile_jr(CompileFlags cf)
{
  const Register pcreg = CFGetRegS(cf);
  CheckBranchTarget(pcreg);

  armAsm->str(pcreg, PTR(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::ARM64Recompiler::Compile_jalr(CompileFlags cf)
{
  const Register pcreg = CFGetRegS(cf);
  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetBranchReturnAddress(cf));

  CheckBranchTarget(pcreg);
  armAsm->str(pcreg, PTR(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::ARM64Recompiler::Compile_bxx(CompileFlags cf, BranchCondition cond)
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
      if (cf.const_t && HasConstantRegValue(cf.MipsT(), 0))
      {
        (cond == BranchCondition::Equal) ? armAsm->cbz(rs, &taken) : armAsm->cbnz(rs, &taken);
      }
      else
      {
        if (cf.valid_host_t)
          armAsm->cmp(rs, CFGetRegT(cf));
        else if (cf.const_t)
          armAsm->cmp(rs, armCheckCompareConstant(GetConstantRegU32(cf.MipsT())));

        armAsm->b(&taken, (cond == BranchCondition::Equal) ? eq : ne);
      }
    }
    break;

    case BranchCondition::GreaterThanZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(&taken, gt);
    }
    break;

    case BranchCondition::GreaterEqualZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(&taken, ge);
    }
    break;

    case BranchCondition::LessThanZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(&taken, lt);
    }
    break;

    case BranchCondition::LessEqualZero:
    {
      armAsm->cmp(rs, 0);
      armAsm->b(&taken, le);
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

void CPU::ARM64Recompiler::Compile_addi(CompileFlags cf, bool overflow)
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

void CPU::ARM64Recompiler::Compile_addi(CompileFlags cf)
{
  Compile_addi(cf, g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::ARM64Recompiler::Compile_addiu(CompileFlags cf)
{
  Compile_addi(cf, false);
}

void CPU::ARM64Recompiler::Compile_slti(CompileFlags cf)
{
  Compile_slti(cf, true);
}

void CPU::ARM64Recompiler::Compile_sltiu(CompileFlags cf)
{
  Compile_slti(cf, false);
}

void CPU::ARM64Recompiler::Compile_slti(CompileFlags cf, bool sign)
{
  armAsm->cmp(CFGetRegS(cf), armCheckCompareConstant(static_cast<s32>(inst->i.imm_sext32())));
  armAsm->cset(CFGetRegT(cf), sign ? lt : lo);
}

void CPU::ARM64Recompiler::Compile_andi(CompileFlags cf)
{
  const Register rt = CFGetRegT(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    armAsm->and_(rt, CFGetRegS(cf), armCheckLogicalConstant(imm));
  else
    armAsm->mov(rt, wzr);
}

void CPU::ARM64Recompiler::Compile_ori(CompileFlags cf)
{
  const Register rt = CFGetRegT(cf);
  const Register rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    armAsm->orr(rt, rs, armCheckLogicalConstant(imm));
  else if (rt.GetCode() != rs.GetCode())
    armAsm->mov(rt, rs);
}

void CPU::ARM64Recompiler::Compile_xori(CompileFlags cf)
{
  const Register rt = CFGetRegT(cf);
  const Register rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    armAsm->eor(rt, rs, armCheckLogicalConstant(imm));
  else if (rt.GetCode() != rs.GetCode())
    armAsm->mov(rt, rs);
}

void CPU::ARM64Recompiler::Compile_shift(CompileFlags cf,
                                         void (vixl::aarch64::Assembler::*op)(const vixl::aarch64::Register&,
                                                                              const vixl::aarch64::Register&, unsigned))
{
  const Register rd = CFGetRegD(cf);
  const Register rt = CFGetRegT(cf);
  if (inst->r.shamt > 0)
    (armAsm->*op)(rd, rt, inst->r.shamt);
  else if (rd.GetCode() != rt.GetCode())
    armAsm->mov(rd, rt);
}

void CPU::ARM64Recompiler::Compile_sll(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::lsl);
}

void CPU::ARM64Recompiler::Compile_srl(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::lsr);
}

void CPU::ARM64Recompiler::Compile_sra(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::asr);
}

void CPU::ARM64Recompiler::Compile_variable_shift(
  CompileFlags cf,
  void (vixl::aarch64::Assembler::*op)(const vixl::aarch64::Register&, const vixl::aarch64::Register&,
                                       const vixl::aarch64::Register&),
  void (vixl::aarch64::Assembler::*op_const)(const vixl::aarch64::Register&, const vixl::aarch64::Register&, unsigned))
{
  const Register rd = CFGetRegD(cf);

  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RWARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  if (cf.const_s)
  {
    if (const u32 shift = GetConstantRegU32(cf.MipsS()); shift != 0)
      (armAsm->*op_const)(rd, rt, shift);
    else if (rd.GetCode() != rt.GetCode())
      armAsm->mov(rd, rt);
  }
  else
  {
    (armAsm->*op)(rd, rt, CFGetRegS(cf));
  }
}

void CPU::ARM64Recompiler::Compile_sllv(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::lslv, &Assembler::lsl);
}

void CPU::ARM64Recompiler::Compile_srlv(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::lsrv, &Assembler::lsr);
}

void CPU::ARM64Recompiler::Compile_srav(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::asrv, &Assembler::asr);
}

void CPU::ARM64Recompiler::Compile_mult(CompileFlags cf, bool sign)
{
  const Register rs = cf.valid_host_s ? CFGetRegS(cf) : RWARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RWARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  // TODO: if lo/hi gets killed, we can use a 32-bit multiply
  const Register lo = CFGetRegLO(cf);
  const Register hi = CFGetRegHI(cf);

  (sign) ? armAsm->smull(lo.X(), rs, rt) : armAsm->umull(lo.X(), rs, rt);
  armAsm->lsr(hi.X(), lo.X(), 32);
}

void CPU::ARM64Recompiler::Compile_mult(CompileFlags cf)
{
  Compile_mult(cf, true);
}

void CPU::ARM64Recompiler::Compile_multu(CompileFlags cf)
{
  Compile_mult(cf, false);
}

void CPU::ARM64Recompiler::Compile_div(CompileFlags cf)
{
  const Register rs = cf.valid_host_s ? CFGetRegS(cf) : RWARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RWARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const Register rlo = CFGetRegLO(cf);
  const Register rhi = CFGetRegHI(cf);

  // TODO: This could be slightly more optimal
  Label done;
  Label not_divide_by_zero;
  armAsm->cbnz(rt, &not_divide_by_zero);
  armAsm->mov(rhi, rs); // hi = num
  EmitMov(rlo, 1);
  EmitMov(RWSCRATCH, static_cast<u32>(-1));
  armAsm->cmp(rs, 0);
  armAsm->csel(rlo, RWSCRATCH, rlo, ge); // lo = s >= 0 ? -1 : 1
  armAsm->b(&done);

  armAsm->bind(&not_divide_by_zero);
  Label not_unrepresentable;
  armAsm->cmp(rs, armCheckCompareConstant(static_cast<s32>(0x80000000u)));
  armAsm->b(&not_unrepresentable, ne);
  armAsm->cmp(rt, armCheckCompareConstant(-1));
  armAsm->b(&not_unrepresentable, ne);

  EmitMov(rlo, 0x80000000u);
  EmitMov(rhi, 0);
  armAsm->b(&done);

  armAsm->bind(&not_unrepresentable);

  armAsm->sdiv(rlo, rs, rt);

  // TODO: skip when hi is dead
  armAsm->msub(rhi, rlo, rt, rs);

  armAsm->bind(&done);
}

void CPU::ARM64Recompiler::Compile_divu(CompileFlags cf)
{
  const Register rs = cf.valid_host_s ? CFGetRegS(cf) : RWARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const Register rt = cf.valid_host_t ? CFGetRegT(cf) : RWARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const Register rlo = CFGetRegLO(cf);
  const Register rhi = CFGetRegHI(cf);

  Label done;
  Label not_divide_by_zero;
  armAsm->cbnz(rt, &not_divide_by_zero);
  EmitMov(rlo, static_cast<u32>(-1));
  armAsm->mov(rhi, rs);
  armAsm->b(&done);

  armAsm->bind(&not_divide_by_zero);

  armAsm->udiv(rlo, rs, rt);

  // TODO: skip when hi is dead
  armAsm->msub(rhi, rlo, rt, rs);

  armAsm->bind(&done);
}

void CPU::ARM64Recompiler::TestOverflow(const vixl::aarch64::Register& result)
{
  DebugAssert(result.IsW());
  SwitchToFarCode(true, vs);

  BackupHostState();

  // toss the result
  ClearHostReg(result.GetCode());

  EndBlockWithException(Exception::Ov);

  RestoreHostState();

  SwitchToNearCode(false);
}

void CPU::ARM64Recompiler::Compile_dst_op(CompileFlags cf,
                                          void (vixl::aarch64::Assembler::*op)(const vixl::aarch64::Register&,
                                                                               const vixl::aarch64::Register&,
                                                                               const vixl::aarch64::Operand&),
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
    // TODO: Check where we can use wzr here
    EmitMov(RWSCRATCH, GetConstantRegU32(cf.MipsS()));
    (armAsm->*op)(rd, RWSCRATCH, CFGetRegT(cf));
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

void CPU::ARM64Recompiler::Compile_add(CompileFlags cf)
{
  if (g_settings.cpu_recompiler_memory_exceptions)
    Compile_dst_op(cf, &Assembler::adds, true, false, true);
  else
    Compile_dst_op(cf, &Assembler::add, true, false, false);
}

void CPU::ARM64Recompiler::Compile_addu(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::add, true, false, false);
}

void CPU::ARM64Recompiler::Compile_sub(CompileFlags cf)
{
  if (g_settings.cpu_recompiler_memory_exceptions)
    Compile_dst_op(cf, &Assembler::subs, false, false, true);
  else
    Compile_dst_op(cf, &Assembler::sub, false, false, false);
}

void CPU::ARM64Recompiler::Compile_subu(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::sub, false, false, false);
}

void CPU::ARM64Recompiler::Compile_and(CompileFlags cf)
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
    armAsm->mov(regd, wzr);
    return;
  }

  Compile_dst_op(cf, &Assembler::and_, true, true, false);
}

void CPU::ARM64Recompiler::Compile_or(CompileFlags cf)
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

void CPU::ARM64Recompiler::Compile_xor(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const Register regd = CFGetRegD(cf);
  if (cf.MipsS() == cf.MipsT())
  {
    // xor with self -> zero
    armAsm->mov(regd, wzr);
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

void CPU::ARM64Recompiler::Compile_nor(CompileFlags cf)
{
  Compile_or(cf);
  armAsm->mvn(CFGetRegD(cf), CFGetRegD(cf));
}

void CPU::ARM64Recompiler::Compile_slt(CompileFlags cf)
{
  Compile_slt(cf, true);
}

void CPU::ARM64Recompiler::Compile_sltu(CompileFlags cf)
{
  Compile_slt(cf, false);
}

void CPU::ARM64Recompiler::Compile_slt(CompileFlags cf, bool sign)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // TODO: swap and reverse op for constants
  if (cf.const_s)
  {
    EmitMov(RWSCRATCH, GetConstantRegS32(cf.MipsS()));
    armAsm->cmp(RWSCRATCH, CFGetRegT(cf));
  }
  else if (cf.const_t)
  {
    armAsm->cmp(CFGetRegS(cf), armCheckCompareConstant(GetConstantRegS32(cf.MipsT())));
  }
  else
  {
    armAsm->cmp(CFGetRegS(cf), CFGetRegT(cf));
  }

  armAsm->cset(CFGetRegD(cf), sign ? lt : lo);
}

vixl::aarch64::Register
CPU::ARM64Recompiler::ComputeLoadStoreAddressArg(CompileFlags cf, const std::optional<VirtualMemoryAddress>& address,
                                                 const std::optional<const vixl::aarch64::Register>& reg)
{
  const u32 imm = inst->i.imm_sext32();
  if (cf.valid_host_s && imm == 0 && !reg.has_value())
    return CFGetRegS(cf);

  const Register dst = reg.has_value() ? reg.value() : RWARG1;
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
vixl::aarch64::Register CPU::ARM64Recompiler::GenerateLoad(const vixl::aarch64::Register& addr_reg,
                                                           MemoryAccessSize size, bool sign, bool use_fastmem,
                                                           const RegAllocFn& dst_reg_alloc)
{
  DebugAssert(addr_reg.IsW());
  if (use_fastmem)
  {
    m_cycles += Bus::RAM_READ_TICKS;

    const Register dst = dst_reg_alloc();

    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg.GetCode() != RWARG3.GetCode());
      armAsm->lsr(RXARG3, addr_reg, Bus::FASTMEM_LUT_PAGE_SHIFT);
      armAsm->ldr(RXARG3, MemOperand(RMEMBASE, RXARG3, LSL, 3));
    }

    const MemOperand mem =
      MemOperand((g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT) ? RXARG3 : RMEMBASE, addr_reg.X());
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

    AddLoadStoreInfo(start, kInstructionSize, addr_reg.GetCode(), dst.GetCode(), size, sign, true);
    return dst;
  }

  if (addr_reg.GetCode() != RWARG1.GetCode())
    armAsm->mov(RWARG1, addr_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::ReadMemoryByte) :
                         reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedReadMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::ReadMemoryHalfWord) :
                         reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedReadMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::ReadMemoryWord) :
                         reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedReadMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    SwitchToFarCodeIfBitSet(RXRET, 63);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const WRegister temp = WRegister(AllocateTempHostReg(HR_CALLEE_SAVED));
    armAsm->neg(temp.X(), RXRET);
    armAsm->lsl(temp, temp, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (-result << 2) | BD | cop_n
    armAsm->orr(RWARG1, temp,
                armCheckLogicalConstant(Cop0Registers::CAUSE::MakeValueForException(
                  static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n)));
    EmitMov(RWARG2, m_current_instruction_pc);
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
      sign ? armAsm->sxtb(dst_reg, RWRET) : armAsm->uxtb(dst_reg, RWRET);
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      sign ? armAsm->sxth(dst_reg, RWRET) : armAsm->uxth(dst_reg, RWRET);
    }
    break;
    case MemoryAccessSize::Word:
    {
      if (dst_reg.GetCode() != RWRET.GetCode())
        armAsm->mov(dst_reg, RWRET);
    }
    break;
  }

  return dst_reg;
}

void CPU::ARM64Recompiler::GenerateStore(const vixl::aarch64::Register& addr_reg,
                                         const vixl::aarch64::Register& value_reg, MemoryAccessSize size,
                                         bool use_fastmem)
{
  DebugAssert(addr_reg.IsW() && value_reg.IsW());
  if (use_fastmem)
  {
    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg.GetCode() != RWARG3.GetCode());
      armAsm->lsr(RXARG3, addr_reg, Bus::FASTMEM_LUT_PAGE_SHIFT);
      armAsm->ldr(RXARG3, MemOperand(RMEMBASE, RXARG3, LSL, 3));
    }

    const MemOperand mem =
      MemOperand((g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT) ? RXARG3 : RMEMBASE, addr_reg.X());
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
    AddLoadStoreInfo(start, kInstructionSize, addr_reg.GetCode(), value_reg.GetCode(), size, false, false);
    return;
  }

  if (addr_reg.GetCode() != RWARG1.GetCode())
    armAsm->mov(RWARG1, addr_reg);
  if (value_reg.GetCode() != RWARG2.GetCode())
    armAsm->mov(RWARG2, value_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::WriteMemoryByte) :
                         reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedWriteMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::WriteMemoryHalfWord) :
                         reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedWriteMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::WriteMemoryWord) :
                         reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedWriteMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    SwitchToFarCodeIfRegZeroOrNonZero(RXRET, true);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const WRegister temp = WRegister(AllocateTempHostReg(HR_CALLEE_SAVED));
    armAsm->lsl(temp, RWRET, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (result << 2) | BD | cop_n
    armAsm->orr(RWARG1, temp,
                armCheckLogicalConstant(Cop0Registers::CAUSE::MakeValueForException(
                  static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n)));
    EmitMov(RWARG2, m_current_instruction_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    FreeHostReg(temp.GetCode());
    EndBlock(std::nullopt, true);

    RestoreHostState();
    SwitchToNearCode(false);
  }
}

void CPU::ARM64Recompiler::Compile_lxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  const std::optional<WRegister> addr_reg =
    g_settings.gpu_pgxp_enable ? std::optional<WRegister>(WRegister(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                 std::optional<WRegister>();
  FlushForLoadStore(address, false, use_fastmem);
  const Register addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Register data = GenerateLoad(addr, size, sign, use_fastmem, [this, cf]() -> Register {
    if (cf.MipsT() == Reg::zero)
      return RWRET;

    return WRegister(AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                                     EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG,
                                     cf.MipsT()));
  });

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);

    EmitMov(RWARG1, inst->bits);
    armAsm->mov(RWARG2, addr);
    armAsm->mov(RWARG3, data);
    EmitCall(s_pgxp_mem_load_functions[static_cast<u32>(size)][static_cast<u32>(sign)]);
    FreeHostReg(addr_reg.value().GetCode());
  }
}

void CPU::ARM64Recompiler::Compile_lwx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  const Register addr = WRegister(AllocateTempHostReg(HR_CALLEE_SAVED));
  FlushForLoadStore(address, false, use_fastmem);

  // TODO: if address is constant, this can be simplified..

  // If we're coming from another block, just flush the load delay and hope for the best..
  if (m_load_delay_dirty)
    UpdateLoadDelay();

  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  armAsm->and_(RWARG1, addr, armCheckLogicalConstant(~0x3u));
  GenerateLoad(RWARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RWRET; });

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
    value = WRegister(existing_ld_rt);
  }
  else
  {
    if constexpr (EMULATE_LOAD_DELAYS)
    {
      value = WRegister(AllocateHostReg(HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt));
      if (const std::optional<u32> rtreg = CheckHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt); rtreg.has_value())
        armAsm->mov(value, WRegister(rtreg.value()));
      else if (HasConstantReg(rt))
        EmitMov(value, GetConstantRegU32(rt));
      else
        armAsm->ldr(value, MipsPtr(rt));
    }
    else
    {
      value = WRegister(AllocateHostReg(HR_MODE_READ | HR_MODE_WRITE, HR_TYPE_CPU_REG, rt));
    }
  }

  DebugAssert(value.GetCode() != RWARG2.GetCode() && value.GetCode() != RWARG3.GetCode());
  armAsm->and_(RWARG2, addr, 3);
  armAsm->lsl(RWARG2, RWARG2, 3); // *8
  EmitMov(RWARG3, 24);
  armAsm->sub(RWARG3, RWARG3, RWARG2);

  if (inst->op == InstructionOp::lwl)
  {
    // const u32 mask = UINT32_C(0x00FFFFFF) >> shift;
    // new_value = (value & mask) | (RWRET << (24 - shift));
    EmitMov(RWSCRATCH, 0xFFFFFFu);
    armAsm->lsrv(RWSCRATCH, RWSCRATCH, RWARG2);
    armAsm->and_(value, value, RWSCRATCH);
    armAsm->lslv(RWRET, RWRET, RWARG3);
    armAsm->orr(value, value, RWRET);
  }
  else
  {
    // const u32 mask = UINT32_C(0xFFFFFF00) << (24 - shift);
    // new_value = (value & mask) | (RWRET >> shift);
    armAsm->lsrv(RWRET, RWRET, RWARG2);
    EmitMov(RWSCRATCH, 0xFFFFFF00u);
    armAsm->lslv(RWSCRATCH, RWSCRATCH, RWARG3);
    armAsm->and_(value, value, RWSCRATCH);
    armAsm->orr(value, value, RWRET);
  }

  FreeHostReg(addr.GetCode());

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    armAsm->mov(RWARG3, value);
    armAsm->and_(RWARG2, addr, armCheckLogicalConstant(~0x3u));
    EmitMov(RWARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LW));
  }
}

void CPU::ARM64Recompiler::Compile_lwc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                        const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, true);
  const std::optional<WRegister> addr_reg =
    g_settings.gpu_pgxp_enable ? std::optional<WRegister>(WRegister(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                 std::optional<WRegister>();
  FlushForLoadStore(address, false, use_fastmem);
  const Register addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Register value = GenerateLoad(addr, MemoryAccessSize::Word, false, use_fastmem, [this, action = action]() {
    return (action == GTERegisterAccessAction::CallHandler && g_settings.gpu_pgxp_enable) ?
             WRegister(AllocateTempHostReg(HR_CALLEE_SAVED)) :
             RWRET;
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
      armAsm->sxth(RWARG3, value);
      armAsm->str(RWARG3, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::ZeroExtend16:
    {
      armAsm->uxth(RWARG3, value);
      armAsm->str(RWARG3, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::CallHandler:
    {
      Flush(FLUSH_FOR_C_CALL);
      armAsm->mov(RWARG2, value);
      EmitMov(RWARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::WriteRegister));
      break;
    }

    case GTERegisterAccessAction::PushFIFO:
    {
      // SXY0 <- SXY1
      // SXY1 <- SXY2
      // SXY2 <- SXYP
      DebugAssert(value.GetCode() != RWARG2.GetCode() && value.GetCode() != RWARG3.GetCode());
      armAsm->ldr(RWARG2, PTR(&g_state.gte_regs.SXY1[0]));
      armAsm->ldr(RWARG3, PTR(&g_state.gte_regs.SXY2[0]));
      armAsm->str(RWARG2, PTR(&g_state.gte_regs.SXY0[0]));
      armAsm->str(RWARG3, PTR(&g_state.gte_regs.SXY1[0]));
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
    armAsm->mov(RWARG3, value);
    if (value.GetCode() != RWRET.GetCode())
      FreeHostReg(value.GetCode());
    armAsm->mov(RWARG2, addr);
    FreeHostReg(addr_reg.value().GetCode());
    EmitMov(RWARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LWC2));
  }
}

void CPU::ARM64Recompiler::Compile_sxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const std::optional<WRegister> addr_reg =
    g_settings.gpu_pgxp_enable ? std::optional<WRegister>(WRegister(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                 std::optional<WRegister>();
  FlushForLoadStore(address, true, use_fastmem);
  const Register addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Register data = cf.valid_host_t ? CFGetRegT(cf) : RWARG2;
  if (!cf.valid_host_t)
    MoveTToReg(RWARG2, cf);

  GenerateStore(addr, data, size, use_fastmem);

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    MoveMIPSRegToReg(RWARG3, cf.MipsT());
    armAsm->mov(RWARG2, addr);
    EmitMov(RWARG1, inst->bits);
    EmitCall(s_pgxp_mem_store_functions[static_cast<u32>(size)]);
    FreeHostReg(addr_reg.value().GetCode());
  }
}

void CPU::ARM64Recompiler::Compile_swx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                       const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  // TODO: this can take over rt's value if it's no longer needed
  // NOTE: can't trust T in cf because of the alloc
  const Register addr = WRegister(AllocateTempHostReg(HR_CALLEE_SAVED));
  const Register value = g_settings.gpu_pgxp_enable ? WRegister(AllocateTempHostReg(HR_CALLEE_SAVED)) : RWARG2;
  if (g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  FlushForLoadStore(address, true, use_fastmem);

  // TODO: if address is constant, this can be simplified..
  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  armAsm->and_(RWARG1, addr, armCheckLogicalConstant(~0x3u));
  GenerateLoad(RWARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RWRET; });

  armAsm->and_(RWSCRATCH, addr, 3);
  armAsm->lsl(RWSCRATCH, RWSCRATCH, 3); // *8
  armAsm->and_(addr, addr, armCheckLogicalConstant(~0x3u));

  // Need to load down here for PGXP-off, because it's in a volatile reg that can get overwritten by flush.
  if (!g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  if (inst->op == InstructionOp::swl)
  {
    // const u32 mem_mask = UINT32_C(0xFFFFFF00) << shift;
    // new_value = (RWRET & mem_mask) | (value >> (24 - shift));
    EmitMov(RWARG3, 0xFFFFFF00u);
    armAsm->lslv(RWARG3, RWARG3, RWSCRATCH);
    armAsm->and_(RWRET, RWRET, RWARG3);

    EmitMov(RWARG3, 24);
    armAsm->sub(RWARG3, RWARG3, RWSCRATCH);
    armAsm->lsrv(value, value, RWARG3);
    armAsm->orr(value, value, RWRET);
  }
  else
  {
    // const u32 mem_mask = UINT32_C(0x00FFFFFF) >> (24 - shift);
    // new_value = (RWRET & mem_mask) | (value << shift);
    armAsm->lslv(value, value, RWSCRATCH);

    EmitMov(RWARG3, 24);
    armAsm->sub(RWARG3, RWARG3, RWSCRATCH);
    EmitMov(RWSCRATCH, 0x00FFFFFFu);
    armAsm->lsrv(RWSCRATCH, RWSCRATCH, RWARG3);
    armAsm->and_(RWRET, RWRET, RWSCRATCH);
    armAsm->orr(value, value, RWRET);
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
    armAsm->mov(RWARG3, value);
    FreeHostReg(value.GetCode());
    armAsm->mov(RWARG2, addr);
    FreeHostReg(addr.GetCode());
    EmitMov(RWARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SW));
  }
}

void CPU::ARM64Recompiler::Compile_swc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                        const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, false);
  const Register addr = (g_settings.gpu_pgxp_enable || action == GTERegisterAccessAction::CallHandler) ?
                          WRegister(AllocateTempHostReg(HR_CALLEE_SAVED)) :
                          RWARG1;
  const Register data = g_settings.gpu_pgxp_enable ? WRegister(AllocateTempHostReg(HR_CALLEE_SAVED)) : RWARG2;
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
      EmitMov(RWARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));
      armAsm->mov(data, RWRET);
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
    if (addr.GetCode() != RWARG1.GetCode())
      FreeHostReg(addr.GetCode());
  }
  else
  {
    // TODO: This can be simplified because we don't need to validate in PGXP..
    Flush(FLUSH_FOR_C_CALL);
    armAsm->mov(RWARG3, data);
    FreeHostReg(data.GetCode());
    armAsm->mov(RWARG2, addr);
    FreeHostReg(addr.GetCode());
    EmitMov(RWARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SWC2));
  }
}

void CPU::ARM64Recompiler::Compile_mtc0(CompileFlags cf)
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
  const Register new_value = RWARG1;
  const Register old_value = RWARG2;
  const Register changed_bits = RWARG3;
  const Register mask_reg = RWSCRATCH;

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

    Label caches_unchanged;
    armAsm->tbz(changed_bits, 16, &caches_unchanged);
    EmitCall(reinterpret_cast<const void*>(&CPU::UpdateMemoryPointers));
    armAsm->ldr(RWARG1, PTR(ptr)); // reload value for interrupt test below
    if (CodeCache::IsUsingFastmem())
      armAsm->ldr(RMEMBASE, PTR(&g_state.fastmem_base));
    armAsm->bind(&caches_unchanged);

    TestInterrupts(RWARG1);
  }
  else if (reg == Cop0Reg::CAUSE)
  {
    armAsm->ldr(RWARG1, PTR(&g_state.cop0_regs.sr.bits));
    TestInterrupts(RWARG1);
  }
  else if (reg == Cop0Reg::DCIC || reg == Cop0Reg::BPCM)
  {
    // need to check whether we're switching to debug mode
    Flush(FLUSH_FOR_C_CALL);
    EmitCall(reinterpret_cast<const void*>(&CPU::UpdateDebugDispatcherFlag));
    SwitchToFarCodeIfRegZeroOrNonZero(RWRET, true);
    BackupHostState();
    Flush(FLUSH_FOR_EARLY_BLOCK_EXIT);
    EmitCall(reinterpret_cast<const void*>(&CPU::ExitExecution)); // does not return
    RestoreHostState();
    SwitchToNearCode(false);
  }
}

void CPU::ARM64Recompiler::Compile_rfe(CompileFlags cf)
{
  // shift mode bits right two, preserving upper bits
  armAsm->ldr(RWARG1, PTR(&g_state.cop0_regs.sr.bits));
  armAsm->bfxil(RWARG1, RWARG1, 2, 4);
  armAsm->str(RWARG1, PTR(&g_state.cop0_regs.sr.bits));

  TestInterrupts(RWARG1);
}

void CPU::ARM64Recompiler::TestInterrupts(const vixl::aarch64::Register& sr)
{
  DebugAssert(sr.IsW());

  // if Iec == 0 then goto no_interrupt
  Label no_interrupt;
  armAsm->tbz(sr, 0, &no_interrupt);

  // sr & cause
  armAsm->ldr(RWSCRATCH, PTR(&g_state.cop0_regs.cause.bits));
  armAsm->and_(sr, sr, RWSCRATCH);

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
    EmitMov(RWARG1, Cop0Registers::CAUSE::MakeValueForException(Exception::INT, iinfo->is_branch_instruction, false,
                                                                (inst + 1)->cop.cop_n));
    EmitMov(RWARG2, m_compiler_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, true, false);
  }
  else
  {
    if (m_dirty_pc)
      EmitMov(RWARG1, m_compiler_pc);
    armAsm->str(wzr, PTR(&g_state.downcount));
    if (m_dirty_pc)
      armAsm->str(RWARG1, PTR(&g_state.pc));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, false, true);
  }

  RestoreHostState();
  SwitchToNearCode(false);

  armAsm->bind(&no_interrupt);
}

void CPU::ARM64Recompiler::Compile_mfc2(CompileFlags cf)
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
    armAsm->ldr(WRegister(hreg), PTR(ptr));
  }
  else if (action == GTERegisterAccessAction::CallHandler)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RWARG1, index);
    EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));

    hreg = AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                           EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, rt);
    armAsm->mov(WRegister(hreg), RWRET);
  }
  else
  {
    Panic("Unknown action");
    return;
  }

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RWARG1, inst->bits);
    armAsm->mov(RWARG2, WRegister(hreg));
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_MFC2));
  }
}

void CPU::ARM64Recompiler::Compile_mtc2(CompileFlags cf)
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
      sign ? armAsm->sxth(RWARG1, CFGetRegT(cf)) : armAsm->uxth(RWARG1, CFGetRegT(cf));
      armAsm->str(RWARG1, PTR(ptr));
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
    EmitMov(RWARG1, index);
    MoveTToReg(RWARG2, cf);
    EmitCall(reinterpret_cast<const void*>(&GTE::WriteRegister));
  }
  else if (action == GTERegisterAccessAction::PushFIFO)
  {
    // SXY0 <- SXY1
    // SXY1 <- SXY2
    // SXY2 <- SXYP
    DebugAssert(RWRET.GetCode() != RWARG2.GetCode() && RWRET.GetCode() != RWARG3.GetCode());
    armAsm->ldr(RWARG2, PTR(&g_state.gte_regs.SXY1[0]));
    armAsm->ldr(RWARG3, PTR(&g_state.gte_regs.SXY2[0]));
    armAsm->str(RWARG2, PTR(&g_state.gte_regs.SXY0[0]));
    armAsm->str(RWARG3, PTR(&g_state.gte_regs.SXY1[0]));
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

void CPU::ARM64Recompiler::Compile_cop2(CompileFlags cf)
{
  TickCount func_ticks;
  GTE::InstructionImpl func = GTE::GetInstructionImpl(inst->bits, &func_ticks);

  Flush(FLUSH_FOR_C_CALL);
  EmitMov(RWARG1, inst->bits & GTE::Instruction::REQUIRED_BITS_MASK);
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

  static constexpr u32 GPR_SIZE = 8;

  // save regs
  u32 num_gprs = 0;

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    if ((gpr_bitmask & (1u << i)) && armIsCallerSavedRegister(i) && (!is_load || data_register != i))
      num_gprs++;
  }

  const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE);

  // TODO: use stp+ldp, vixl helper?

  if (stack_size > 0)
  {
    armAsm->sub(sp, sp, stack_size);

    u32 stack_offset = 0;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && armIsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        armAsm->str(XRegister(i), MemOperand(sp, stack_offset));
        stack_offset += GPR_SIZE;
      }
    }
  }

  if (cycles_to_add != 0)
  {
    // NOTE: we have to reload here, because memory writes can run DMA, which can screw with cycles
    Assert(Assembler::IsImmAddSub(cycles_to_add));
    armAsm->ldr(RWSCRATCH, PTR(&g_state.pending_ticks));
    armAsm->add(RWSCRATCH, RWSCRATCH, cycles_to_add);
    armAsm->str(RWSCRATCH, PTR(&g_state.pending_ticks));
  }

  if (address_register != static_cast<u8>(RWARG1.GetCode()))
    armAsm->mov(RWARG1, WRegister(address_register));

  if (!is_load)
  {
    if (data_register != static_cast<u8>(RWARG2.GetCode()))
      armAsm->mov(RWARG2, WRegister(data_register));
  }

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      armEmitCall(armAsm,
                  is_load ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedReadMemoryByte) :
                            reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedWriteMemoryByte),
                  false);
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      armEmitCall(armAsm,
                  is_load ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedReadMemoryHalfWord) :
                            reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedWriteMemoryHalfWord),
                  false);
    }
    break;
    case MemoryAccessSize::Word:
    {
      armEmitCall(armAsm,
                  is_load ? reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedReadMemoryWord) :
                            reinterpret_cast<const void*>(&CPU::RecompilerThunks::UncheckedWriteMemoryWord),
                  false);
    }
    break;
  }

  if (is_load)
  {
    const WRegister dst = WRegister(data_register);
    switch (size)
    {
      case MemoryAccessSize::Byte:
      {
        is_signed ? armAsm->sxtb(dst, RWRET) : armAsm->uxtb(dst, RWRET);
      }
      break;
      case MemoryAccessSize::HalfWord:
      {
        is_signed ? armAsm->sxth(dst, RWRET) : armAsm->uxth(dst, RWRET);
      }
      break;
      case MemoryAccessSize::Word:
      {
        if (dst.GetCode() != RWRET.GetCode())
          armAsm->mov(dst, RWRET);
      }
      break;
    }
  }

  if (cycles_to_remove != 0)
  {
    Assert(Assembler::IsImmAddSub(cycles_to_remove));
    armAsm->ldr(RWSCRATCH, PTR(&g_state.pending_ticks));
    armAsm->sub(RWSCRATCH, RWSCRATCH, cycles_to_remove);
    armAsm->str(RWSCRATCH, PTR(&g_state.pending_ticks));
  }

  // restore regs
  if (stack_size > 0)
  {
    u32 stack_offset = 0;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && armIsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        armAsm->ldr(XRegister(i), MemOperand(sp, stack_offset));
        stack_offset += GPR_SIZE;
      }
    }

    armAsm->add(sp, sp, stack_size);
  }

  armEmitJmp(armAsm, static_cast<const u8*>(code_address) + code_size, true);
  armAsm->FinalizeCode();

  return static_cast<u32>(armAsm->GetCursorOffset());
}

#endif // CPU_ARCH_ARM64
