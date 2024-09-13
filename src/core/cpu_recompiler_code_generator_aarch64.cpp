// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_code_cache_private.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#include "settings.h"
#include "timing_event.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/memmap.h"

#ifdef CPU_ARCH_ARM64

Log_SetChannel(CPU::Recompiler);

#ifdef ENABLE_HOST_DISASSEMBLY
#include "vixl/aarch64/disasm-aarch64.h"
#endif

namespace a64 = vixl::aarch64;

namespace CPU::Recompiler {
constexpr u64 FUNCTION_CALLEE_SAVED_SPACE_RESERVE = 80;  // 8 registers
constexpr u64 FUNCTION_CALLER_SAVED_SPACE_RESERVE = 144; // 18 registers -> 224 bytes
constexpr u64 FUNCTION_STACK_SIZE = FUNCTION_CALLEE_SAVED_SPACE_RESERVE + FUNCTION_CALLER_SAVED_SPACE_RESERVE;

static constexpr u32 TRAMPOLINE_AREA_SIZE = 4 * 1024;
static std::unordered_map<const void*, u32> s_trampoline_targets;
static u8* s_trampoline_start_ptr = nullptr;
static u32 s_trampoline_used = 0;
} // namespace CPU::Recompiler

bool CPU::Recompiler::armIsCallerSavedRegister(u32 id)
{
  // same on both linux and windows
  return (id <= 18);
}

void CPU::Recompiler::armEmitMov(a64::Assembler* armAsm, const a64::Register& rd, u64 imm)
{
  DebugAssert(vixl::IsUint32(imm) || vixl::IsInt32(imm) || rd.Is64Bits());
  DebugAssert(rd.GetCode() != a64::sp.GetCode());

  if (imm == 0)
  {
    armAsm->mov(rd, a64::Assembler::AppropriateZeroRegFor(rd));
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

  if (a64::Assembler::IsImmMovz(imm, reg_size) && !rd.IsSP())
  {
    // Immediate can be represented in a move zero instruction. Movz can't write
    // to the stack pointer.
    armAsm->movz(rd, imm);
    return;
  }
  else if (a64::Assembler::IsImmMovn(imm, reg_size) && !rd.IsSP())
  {
    // Immediate can be represented in a move negative instruction. Movn can't
    // write to the stack pointer.
    armAsm->movn(rd, rd.Is64Bits() ? ~imm : (~imm & a64::kWRegMask));
    return;
  }
  else if (a64::Assembler::IsImmLogical(imm, reg_size))
  {
    // Immediate can be represented in a logical orr instruction.
    DebugAssert(!rd.IsZero());
    armAsm->orr(rd, a64::Assembler::AppropriateZeroRegFor(rd), imm);
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

s64 CPU::Recompiler::armGetPCDisplacement(const void* current, const void* target)
{
  // pxAssert(Common::IsAlignedPow2(reinterpret_cast<size_t>(current), 4));
  // pxAssert(Common::IsAlignedPow2(reinterpret_cast<size_t>(target), 4));
  return static_cast<s64>((reinterpret_cast<ptrdiff_t>(target) - reinterpret_cast<ptrdiff_t>(current)) >> 2);
}

bool CPU::Recompiler::armIsInAdrpRange(vixl::aarch64::Assembler* armAsm, const void* addr)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  const void* current_code_ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(cur) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);

  return (vixl::IsInt21(page_displacement) &&
          (a64::Assembler::IsImmAddSub(page_offset) || a64::Assembler::IsImmLogical(page_offset, 64)));
}

void CPU::Recompiler::armMoveAddressToReg(a64::Assembler* armAsm, const a64::Register& reg, const void* addr)
{
  DebugAssert(reg.IsX());

  const void* cur = armAsm->GetCursorAddress<const void*>();
  const void* current_code_ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(cur) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);
  if (vixl::IsInt21(page_displacement) && a64::Assembler::IsImmAddSub(page_offset))
  {
    armAsm->adrp(reg, page_displacement);
    armAsm->add(reg, reg, page_offset);
  }
  else if (vixl::IsInt21(page_displacement) && a64::Assembler::IsImmLogical(page_offset, 64))
  {
    armAsm->adrp(reg, page_displacement);
    armAsm->orr(reg, reg, page_offset);
  }
  else
  {
    armEmitMov(armAsm, reg, reinterpret_cast<uintptr_t>(addr));
  }
}
void CPU::Recompiler::armEmitJmp(a64::Assembler* armAsm, const void* ptr, bool force_inline)
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

void CPU::Recompiler::armEmitCall(a64::Assembler* armAsm, const void* ptr, bool force_inline)
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

void CPU::Recompiler::armEmitCondBranch(a64::Assembler* armAsm, a64::Condition cond, const void* ptr)
{
  const s64 jump_distance = static_cast<s64>(reinterpret_cast<intptr_t>(ptr) -
                                             reinterpret_cast<intptr_t>(armAsm->GetCursorAddress<const void*>()));
  // pxAssert(Common::IsAligned(jump_distance, 4));

  if (a64::Instruction::IsValidImmPCOffset(a64::CondBranchType, jump_distance >> 2))
  {
    armAsm->b(jump_distance >> 2, cond);
  }
  else
  {
    a64::Label branch_not_taken;
    armAsm->b(&branch_not_taken, InvertCondition(cond));

    const s64 new_jump_distance = static_cast<s64>(reinterpret_cast<intptr_t>(ptr) -
                                                   reinterpret_cast<intptr_t>(armAsm->GetCursorAddress<const void*>()));
    armAsm->b(new_jump_distance >> 2);
    armAsm->bind(&branch_not_taken);
  }
}

void CPU::Recompiler::armEmitFarLoad(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg,
                                     const void* addr, bool sign_extend_word)
{
  const void* cur = armAsm->GetCursorAddress<const void*>();
  const void* current_code_ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(cur) & ~static_cast<uintptr_t>(0xFFF));
  const void* ptr_page =
    reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(0xFFF));
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
  const u32 page_offset = static_cast<u32>(reinterpret_cast<uintptr_t>(addr) & 0xFFFu);
  a64::MemOperand memop;

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

void CPU::Recompiler::armEmitFarStore(vixl::aarch64::Assembler* armAsm, const vixl::aarch64::Register& reg,
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
  a64::Assembler armAsm(start, TRAMPOLINE_AREA_SIZE - offset);
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

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  class MyDisassembler : public a64::Disassembler
  {
  protected:
    void ProcessOutput(const a64::Instruction* instr) override
    {
      DEBUG_LOG("0x{:016X}  {:08X}\t\t{}", reinterpret_cast<uint64_t>(instr), instr->GetInstructionBits(), GetOutput());
    }
  };

  a64::Decoder decoder;
  MyDisassembler disas;
  decoder.AppendVisitor(&disas);
  decoder.Decode(static_cast<const a64::Instruction*>(start),
                 reinterpret_cast<const a64::Instruction*>(static_cast<const u8*>(start) + size));
#else
  ERROR_LOG("Not compiled with ENABLE_HOST_DISASSEMBLY.");
#endif
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
  return size / a64::kInstructionSize;
}

u32 CPU::CodeCache::EmitJump(void* code, const void* dst, bool flush_icache)
{
  using namespace a64;
  using namespace CPU::Recompiler;

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
  using namespace CPU::Recompiler;

#define PTR(x) a64::MemOperand(RSTATE, (s64)(((u8*)(x)) - ((u8*)&g_state)))

  Assembler actual_asm(static_cast<u8*>(code), code_size);
  Assembler* armAsm = &actual_asm;

#ifdef VIXL_DEBUG
  vixl::CodeBufferCheckScope asm_check(armAsm, code_size, vixl::CodeBufferCheckScope::kDontReserveBufferSpace);
#endif

  Label dispatch;

  g_enter_recompiler = armAsm->GetCursorAddress<decltype(g_enter_recompiler)>();
  {
    // reserve some space for saving caller-saved registers
    armAsm->sub(sp, sp, CPU::Recompiler::FUNCTION_STACK_SIZE);

    // Need the CPU state for basically everything :-)
    armMoveAddressToReg(armAsm, RSTATE, &g_state);

    // Fastmem setup, oldrec doesn't need it
    if (IsUsingFastmem() && g_settings.cpu_execution_mode != CPUExecutionMode::Recompiler)
      armAsm->ldr(RMEMBASE, PTR(&g_state.fastmem_base));

    // Fall through to event dispatcher
  }

  // check events then for frame done
  g_check_events_and_dispatch = armAsm->GetCursorAddress<const void*>();
  {
    Label skip_event_check;
    armAsm->ldr(RWARG1, PTR(&g_state.pending_ticks));
    armAsm->ldr(RWARG2, PTR(&g_state.downcount));
    armAsm->cmp(RWARG1, RWARG2);
    armAsm->b(&skip_event_check, lt);

    g_run_events_and_dispatch = armAsm->GetCursorAddress<const void*>();
    armEmitCall(armAsm, reinterpret_cast<const void*>(&TimingEvents::RunEvents), true);

    armAsm->bind(&skip_event_check);
  }

  // TODO: align?
  g_dispatcher = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->bind(&dispatch);

    // x9 <- s_fast_map[pc >> 16]
    armAsm->ldr(RWARG1, PTR(&g_state.pc));
    armMoveAddressToReg(armAsm, RXARG3, g_code_lut.data());
    armAsm->lsr(RWARG2, RWARG1, 16);
    armAsm->lsr(RWARG1, RWARG1, 2);
    armAsm->ldr(RXARG2, MemOperand(RXARG3, RXARG2, LSL, 3));

    // blr(x9[pc * 2]) (fast_map[pc >> 2])
    armAsm->ldr(RXARG1, MemOperand(RXARG2, RXARG1, LSL, 3));
    armAsm->blr(RXARG1);
  }

  g_compile_or_revalidate_block = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->ldr(RWARG1, PTR(&g_state.pc));
    armEmitCall(armAsm, reinterpret_cast<const void*>(&CompileOrRevalidateBlock), true);
    armAsm->b(&dispatch);
  }

  g_discard_and_recompile_block = armAsm->GetCursorAddress<const void*>();
  {
    armAsm->ldr(RWARG1, PTR(&g_state.pc));
    armEmitCall(armAsm, reinterpret_cast<const void*>(&DiscardAndRecompileBlock), true);
    armAsm->b(&dispatch);
  }

  g_interpret_block = armAsm->GetCursorAddress<const void*>();
  {
    armEmitCall(armAsm, reinterpret_cast<const void*>(GetInterpretUncachedBlockFunction()), true);
    armAsm->b(&dispatch);
  }

  armAsm->FinalizeCode();

  // TODO: align?
  s_trampoline_targets.clear();
  s_trampoline_start_ptr = static_cast<u8*>(code) + armAsm->GetCursorOffset();
  s_trampoline_used = 0;

#undef PTR
  return static_cast<u32>(armAsm->GetCursorOffset()) + TRAMPOLINE_AREA_SIZE;
}

namespace CPU::Recompiler {

constexpr HostReg RCPUPTR = 19;
constexpr HostReg RMEMBASEPTR = 20;
constexpr HostReg RRETURN = 0;
constexpr HostReg RARG1 = 0;
constexpr HostReg RARG2 = 1;
constexpr HostReg RARG3 = 2;
constexpr HostReg RARG4 = 3;
constexpr HostReg RSCRATCH = 8;

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

CodeGenerator::CodeGenerator()
  : m_register_cache(*this), m_near_emitter(static_cast<vixl::byte*>(CPU::CodeCache::GetFreeCodePointer()),
                                            CPU::CodeCache::GetFreeCodeSpace(), a64::PositionDependentCode),
    m_far_emitter(static_cast<vixl::byte*>(CPU::CodeCache::GetFreeFarCodePointer()),
                  CPU::CodeCache::GetFreeFarCodeSpace(), a64::PositionDependentCode),
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

void* CodeGenerator::GetStartNearCodePointer() const
{
  return static_cast<u8*>(CPU::CodeCache::GetFreeCodePointer());
}

void* CodeGenerator::GetCurrentNearCodePointer() const
{
  return static_cast<u8*>(CPU::CodeCache::GetFreeCodePointer()) + m_near_emitter.GetCursorOffset();
}

void* CodeGenerator::GetCurrentFarCodePointer() const
{
  return static_cast<u8*>(CPU::CodeCache::GetFreeFarCodePointer()) + m_far_emitter.GetCursorOffset();
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

void CodeGenerator::EmitBeginBlock(bool allocate_registers /* = true */)
{
  if (allocate_registers)
  {
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
    if (m_block->HasFlag(CodeCache::BlockFlags::ContainsLoadStoreInstructions))
    {
      const bool fastmem_reg_allocated = m_register_cache.AllocateHostReg(RMEMBASEPTR);
      Assert(fastmem_reg_allocated);
      m_emit->Ldr(GetFastmemBasePtrReg(), a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, fastmem_base)));
    }
  }
}

void CodeGenerator::EmitEndBlock(bool free_registers, const void* jump_to)
{
  if (free_registers)
  {
    if (m_block->HasFlag(CodeCache::BlockFlags::ContainsLoadStoreInstructions))
      m_register_cache.FreeHostReg(RMEMBASEPTR);

    m_register_cache.FreeHostReg(RCPUPTR);
    m_register_cache.FreeHostReg(30); // lr

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
  a64::Label skip_branch;
  m_emit->Cbz(GetHostReg64(value.host_reg), &skip_branch);
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

  const void* code = CPU::CodeCache::GetFreeCodePointer();
  *out_host_code_size = static_cast<u32>(m_near_emitter.GetSizeOfCodeGenerated());
  *out_host_far_code_size = static_cast<u32>(m_far_emitter.GetSizeOfCodeGenerated());

  CPU::CodeCache::CommitCode(static_cast<u32>(m_near_emitter.GetSizeOfCodeGenerated()));
  CPU::CodeCache::CommitFarCode(static_cast<u32>(m_far_emitter.GetSizeOfCodeGenerated()));

  m_near_emitter.Reset();
  m_far_emitter.Reset();

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

        default:
          break;
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

        default:
          break;
      }
    }
    break;

    default:
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

        default:
          break;
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

        default:
          break;
      }
    }
    break;

    default:
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
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - (position * 8));
  m_emit->str(GetHostReg64(reg), addr);
}

void CodeGenerator::EmitPushHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - ((position + 1) * 8));
  m_emit->stp(GetHostReg64(reg2), GetHostReg64(reg), addr);
}

void CodeGenerator::EmitPopHostReg(HostReg reg, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - (position * 8));
  m_emit->ldr(GetHostReg64(reg), addr);
}

void CodeGenerator::EmitPopHostRegPair(HostReg reg, HostReg reg2, u32 position)
{
  const a64::MemOperand addr(a64::sp, FUNCTION_STACK_SIZE - (position * 8));
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

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
  {
    m_emit->lsr(GetHostReg64(RARG1), GetHostReg32(address_reg), Bus::FASTMEM_LUT_PAGE_SHIFT);
    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg64(RARG1), a64::LSL, 3));
  }

  const a64::XRegister membase =
    (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT) ? GetHostReg64(RARG1) : GetFastmemBasePtrReg();

  switch (size)
  {
    case RegSize_8:
      m_emit->ldrb(GetHostReg32(result.host_reg), a64::MemOperand(membase, GetHostReg32(address_reg)));
      break;

    case RegSize_16:
      m_emit->ldrh(GetHostReg32(result.host_reg), a64::MemOperand(membase, GetHostReg32(address_reg)));
      break;

    case RegSize_32:
      m_emit->ldr(GetHostReg32(result.host_reg), a64::MemOperand(membase, GetHostReg32(address_reg)));
      break;

    default:
      UnreachableCode();
      break;
  }
}

void CodeGenerator::EmitLoadGuestMemoryFastmem(Instruction instruction, const CodeCache::InstructionInfo& info,
                                               const Value& address, RegSize size, Value& result)
{
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

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
  {
    m_emit->lsr(GetHostReg64(RARG1), GetHostReg32(address_reg), Bus::FASTMEM_LUT_PAGE_SHIFT);
    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg64(RARG1), a64::LSL, 3));
  }

  const a64::XRegister membase =
    (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT) ? GetHostReg64(RARG1) : GetFastmemBasePtrReg();

  m_register_cache.InhibitAllocation();

  void* host_pc = GetCurrentNearCodePointer();

  switch (size)
  {
    case RegSize_8:
      m_emit->ldrb(GetHostReg32(result.host_reg), a64::MemOperand(membase, GetHostReg32(address_reg)));
      break;

    case RegSize_16:
      m_emit->ldrh(GetHostReg32(result.host_reg), a64::MemOperand(membase, GetHostReg32(address_reg)));
      break;

    case RegSize_32:
      m_emit->ldr(GetHostReg32(result.host_reg), a64::MemOperand(membase, GetHostReg32(address_reg)));
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
  EmitAddCPUStructField(OFFSETOF(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));
  m_delayed_cycles_add += Bus::RAM_READ_TICKS;

  EmitLoadGuestMemorySlowmem(instruction, info, address, size, result, true);

  EmitAddCPUStructField(OFFSETOF(State, pending_ticks),
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
  Value value_in_hr = GetValueInHostRegister(value);

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

  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
  {
    m_emit->lsr(GetHostReg64(RARG1), GetHostReg32(address_reg), Bus::FASTMEM_LUT_PAGE_SHIFT);
    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg64(RARG1), a64::LSL, 3));
  }

  const a64::XRegister membase =
    (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT) ? GetHostReg64(RARG1) : GetFastmemBasePtrReg();

  // fastmem
  void* host_pc = GetCurrentNearCodePointer();

  m_register_cache.InhibitAllocation();

  switch (size)
  {
    case RegSize_8:
      m_emit->strb(GetHostReg32(value_in_hr), a64::MemOperand(membase, GetHostReg32(address_reg)));
      break;

    case RegSize_16:
      m_emit->strh(GetHostReg32(value_in_hr), a64::MemOperand(membase, GetHostReg32(address_reg)));
      break;

    case RegSize_32:
      m_emit->str(GetHostReg32(value_in_hr), a64::MemOperand(membase, GetHostReg32(address_reg)));
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
  EmitAddCPUStructField(OFFSETOF(State, pending_ticks), Value::FromConstantU32(static_cast<u32>(m_delayed_cycles_add)));

  EmitStoreGuestMemorySlowmem(instruction, info, address, size, value_in_hr, true);

  EmitAddCPUStructField(OFFSETOF(State, pending_ticks),
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

void CodeGenerator::EmitUpdateFastmemBase()
{
  m_emit->Ldr(GetFastmemBasePtrReg(), a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, fastmem_base)));
}

void CodeGenerator::BackpatchLoadStore(void* host_pc, const CodeCache::LoadstoreBackpatchInfo& lbi)
{
  DEV_LOG("Backpatching {} (guest PC 0x{:08X}) to slowmem at {}", host_pc, lbi.guest_pc, lbi.thunk_address);

  // check jump distance
  const s64 jump_distance =
    static_cast<s64>(reinterpret_cast<intptr_t>(lbi.thunk_address) - reinterpret_cast<intptr_t>(host_pc));
  Assert(Common::IsAligned(jump_distance, 4));
  Assert(a64::Instruction::IsValidImmPCOffset(a64::UncondBranchType, jump_distance >> 2));

  // turn it into a jump to the slowmem handler
  vixl::aarch64::MacroAssembler emit(static_cast<vixl::byte*>(host_pc), lbi.code_size, a64::PositionDependentCode);
  emit.b(jump_distance >> 2);

  const s32 nops = (static_cast<s32>(lbi.code_size) - static_cast<s32>(emit.GetCursorOffset())) / 4;
  Assert(nops >= 0);
  for (s32 i = 0; i < nops; i++)
    emit.nop();

  MemMap::FlushInstructionCache(host_pc, lbi.code_size);
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

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), OFFSETOF(State, load_delay_reg));
  const a64::MemOperand load_delay_value(GetCPUPtrReg(), OFFSETOF(State, load_delay_value));
  const a64::MemOperand regs_base(GetCPUPtrReg(), OFFSETOF(State, regs.r[0]));

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
  m_emit->Add(GetHostReg32(reg), GetHostReg32(reg), OFFSETOF(State, regs.r[0]));

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

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), OFFSETOF(State, load_delay_reg));
  const a64::MemOperand load_delay_value(GetCPUPtrReg(), OFFSETOF(State, load_delay_value));
  const a64::MemOperand next_load_delay_reg(GetCPUPtrReg(), OFFSETOF(State, next_load_delay_reg));
  const a64::MemOperand next_load_delay_value(GetCPUPtrReg(), OFFSETOF(State, next_load_delay_value));

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

  const a64::MemOperand load_delay_reg(GetCPUPtrReg(), OFFSETOF(State, load_delay_reg));
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

void CodeGenerator::EmitICacheCheckAndUpdate()
{
  if (!m_block->HasFlag(CodeCache::BlockFlags::IsUsingICache))
  {
    if (m_block->HasFlag(CodeCache::BlockFlags::NeedsDynamicFetchTicks))
    {
      armEmitFarLoad(m_emit, RWARG2, GetFetchMemoryAccessTimePtr());
      m_emit->Ldr(RWARG1, a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, pending_ticks)));
      m_emit->Mov(RWARG3, m_block->size);
      m_emit->Mul(RWARG2, RWARG2, RWARG3);
      m_emit->Add(RWARG1, RWARG1, RWARG2);
      m_emit->Str(RWARG1, a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, pending_ticks)));
    }
    else
    {
      EmitAddCPUStructField(OFFSETOF(State, pending_ticks),
                            Value::FromConstantU32(static_cast<u32>(m_block->uncached_fetch_ticks)));
    }
  }
  else if (m_block->icache_line_count > 0)
  {
    const auto& ticks_reg = a64::w0;
    const auto& current_tag_reg = a64::w1;
    const auto& existing_tag_reg = a64::w2;

    VirtualMemoryAddress current_pc = m_pc & ICACHE_TAG_ADDRESS_MASK;
    m_emit->Ldr(ticks_reg, a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, pending_ticks)));
    m_emit->Mov(current_tag_reg, current_pc);

    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const TickCount fill_ticks = GetICacheFillTicks(current_pc);
      if (fill_ticks <= 0)
        continue;

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = OFFSETOF(State, icache_tags) + (line * sizeof(u32));

      a64::Label cache_hit;
      m_emit->Ldr(existing_tag_reg, a64::MemOperand(GetCPUPtrReg(), offset));
      m_emit->Cmp(existing_tag_reg, current_tag_reg);
      m_emit->B(&cache_hit, a64::eq);

      m_emit->Str(current_tag_reg, a64::MemOperand(GetCPUPtrReg(), offset));
      EmitAdd(0, 0, Value::FromConstantU32(static_cast<u32>(fill_ticks)), false);
      m_emit->Bind(&cache_hit);

      if (i != (m_block->icache_line_count - 1))
        m_emit->Add(current_tag_reg, current_tag_reg, ICACHE_LINE_SIZE);
    }

    m_emit->Str(ticks_reg, a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, pending_ticks)));
  }
}

void CodeGenerator::EmitBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size)
{
  // store it first to reduce code size, because we can offset
  armMoveAddressToReg(m_emit, RXARG1, ram_ptr);
  armMoveAddressToReg(m_emit, RXARG2, shadow_ptr);

  bool first = true;
  u32 offset = 0;
  a64::Label block_changed;

  while (size >= 16)
  {
    const a64::VRegister vtmp = a64::v2.V4S();
    const a64::VRegister dst = first ? a64::v0.V4S() : a64::v1.V4S();
    m_emit->ldr(dst, a64::MemOperand(RXARG1, offset));
    m_emit->ldr(vtmp, a64::MemOperand(RXARG2, offset));
    m_emit->cmeq(dst, dst, vtmp);
    if (!first)
      m_emit->and_(a64::v0.V16B(), a64::v0.V16B(), dst.V16B());
    else
      first = false;

    offset += 16;
    size -= 16;
  }

  if (!first)
  {
    // TODO: make sure this doesn't choke on ffffffff
    m_emit->uminv(a64::s0, a64::v0.V4S());
    m_emit->fcmp(a64::s0, 0.0);
    m_emit->b(&block_changed, a64::eq);
  }

  while (size >= 8)
  {
    m_emit->ldr(RXARG3, a64::MemOperand(RXARG1, offset));
    m_emit->ldr(RXSCRATCH, a64::MemOperand(RXARG2, offset));
    m_emit->cmp(RXARG3, RXSCRATCH);
    m_emit->b(&block_changed, a64::ne);
    offset += 8;
    size -= 8;
  }

  while (size >= 4)
  {
    m_emit->ldr(RWARG3, a64::MemOperand(RXARG1, offset));
    m_emit->ldr(RWSCRATCH, a64::MemOperand(RXARG2, offset));
    m_emit->cmp(RWARG3, RWSCRATCH);
    m_emit->b(&block_changed, a64::ne);
    offset += 4;
    size -= 4;
  }

  DebugAssert(size == 0);

  a64::Label block_unchanged;
  m_emit->b(&block_unchanged);
  m_emit->bind(&block_changed);
  armEmitJmp(m_emit, CodeCache::g_discard_and_recompile_block, false);
  m_emit->bind(&block_unchanged);
}

void CodeGenerator::EmitStallUntilGTEComplete()
{
  static_assert(OFFSETOF(State, pending_ticks) + sizeof(u32) == OFFSETOF(State, gte_completion_tick));
  m_emit->ldp(GetHostReg32(RARG1), GetHostReg32(RARG2),
              a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, pending_ticks)));

  if (m_delayed_cycles_add > 0)
  {
    m_emit->Add(GetHostReg32(RARG1), GetHostReg32(RARG1), static_cast<u32>(m_delayed_cycles_add));
    m_delayed_cycles_add = 0;
  }

  m_emit->cmp(GetHostReg32(RARG2), GetHostReg32(RARG1));
  m_emit->csel(GetHostReg32(RARG1), GetHostReg32(RARG2), GetHostReg32(RARG1), a64::Condition::hi);
  m_emit->str(GetHostReg32(RARG1), a64::MemOperand(GetCPUPtrReg(), OFFSETOF(State, pending_ticks)));
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
  const s64 page_displacement = armGetPCDisplacement(current_code_ptr_page, ptr_page) >> 10;
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

} // namespace CPU::Recompiler

#endif // CPU_ARCH_ARM64
