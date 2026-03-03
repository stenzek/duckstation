// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_recompiler_loongarch64.h"
#include "cpu_code_cache_private.h"
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

#ifdef CPU_ARCH_LOONGARCH64

LOG_CHANNEL(Recompiler);

#define OFFS(x) ((u32)(((u8*)(x)) - ((u8*)&g_state)))

static constexpr u32 BLOCK_LINK_SIZE = 8; // pcaddu18i + jirl

#define RRET LA_A0
#define RARG1 LA_A0
#define RARG2 LA_A1
#define RARG3 LA_A2
#define RSCRATCH LA_T8
#define RSTATE LA_S7
#define RMEMBASE LA_S8

static bool laIsCallerSavedRegister(u32 id);
static bool laIsValidSImm12(u32 imm);
static bool laIsValidUImm12(u32 imm);
static std::pair<s32, s32> laGetAddressImmediates12(const void* cur, const void* target);
static void laMoveAddressToReg(lagoon_assembler_t* laAsm, la_gpr_t reg, const void* addr);
static void laEmitMov(lagoon_assembler_t* laAsm, la_gpr_t rd, u32 imm);
static void laEmitMov64(lagoon_assembler_t* laAsm, la_gpr_t rd, u64 imm);
static u32 laEmitJmp(lagoon_assembler_t* laAsm, const void* ptr, la_gpr_t link_reg = LA_ZERO);
static u32 laEmitCall(lagoon_assembler_t* laAsm, const void* ptr);
static void laEmitFarLoad(lagoon_assembler_t* laAsm, la_gpr_t reg, const void* addr, bool sign_extend_word = false);
static void laEmitFarStore(lagoon_assembler_t* laAsm, la_gpr_t reg, const void* addr, la_gpr_t tempreg = RSCRATCH);
static void laEmitSExtB(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs);  // -> word
static void laEmitUExtB(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs);  // -> word
static void laEmitSExtH(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs);  // -> word
static void laEmitUExtH(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs);  // -> word
static void laEmitDSExtW(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs); // -> doubleword
static void laEmitDUExtW(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs); // -> doubleword

namespace CPU {

using namespace CPU;

LoongArch64Recompiler s_instance;
Recompiler* g_compiler = &s_instance;

} // namespace CPU

bool laIsCallerSavedRegister(u32 id)
{
  return id == 1 || (id >= 4 && id <= 20);
}

bool laIsValidSImm12(u32 imm)
{
  const s32 simm = static_cast<s32>(imm);
  return (simm >= -2048 && simm <= 2047);
}

bool laIsValidUImm12(u32 imm)
{
  return (imm <= 4095);
}

std::pair<s32, s32> laGetAddressImmediates12(const void* cur, const void* target)
{
  const s64 disp = static_cast<s64>(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cur));
  Assert(disp >= static_cast<s64>(std::numeric_limits<s32>::min()) &&
         disp <= static_cast<s64>(std::numeric_limits<s32>::max()));

  const s64 hi = disp + 0x800;
  const s64 lo = disp - (hi & 0xFFFFF000);
  return std::make_pair(static_cast<s32>(hi >> 12), static_cast<s32>((lo << 52) >> 52));
}

std::pair<s32, s32> laGetAddressImmediates18(const void* cur, const void* target)
{
  const s64 disp = static_cast<s64>(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cur));
  Assert(disp >= static_cast<s64>(std::numeric_limits<s32>::min()) &&
         disp <= static_cast<s64>(std::numeric_limits<s32>::max()));

  const s64 hi = disp + 0x20000;
  const s64 lo = disp - (hi & 0xFFFC0000);
  return std::make_pair(static_cast<s32>(hi >> 18), static_cast<s32>((lo << 46) >> 46));
}

void laMoveAddressToReg(lagoon_assembler_t* laAsm, la_gpr_t reg, const void* addr)
{
  const auto [hi, lo] = laGetAddressImmediates12(laAsm->cursor, addr);
  la_pcaddu12i(laAsm, reg, hi);
  la_addi_d(laAsm, reg, reg, lo);
}

void laEmitMov(lagoon_assembler_t* laAsm, la_gpr_t rd, u32 imm)
{
  la_load_immediate32(laAsm, rd, static_cast<s32>(imm));
}

void laEmitMov64(lagoon_assembler_t* laAsm, la_gpr_t rd, u64 imm)
{
  la_load_immediate64(laAsm, rd, static_cast<s64>(imm));
}

u32 laEmitJmp(lagoon_assembler_t* laAsm, const void* ptr, la_gpr_t link_reg)
{
  const auto [hi, lo] = laGetAddressImmediates18(laAsm->cursor, ptr);
  la_pcaddu18i(laAsm, RSCRATCH, hi);
  la_jirl(laAsm, link_reg, RSCRATCH, lo);
  return 8;
}

u32 laEmitCall(lagoon_assembler_t* laAsm, const void* ptr)
{
  return laEmitJmp(laAsm, ptr, LA_RA);
}

void laEmitFarLoad(lagoon_assembler_t* laAsm, la_gpr_t reg, const void* addr, bool sign_extend_word)
{
  const auto [hi, lo] = laGetAddressImmediates12(laAsm->cursor, addr);
  la_pcaddu12i(laAsm, reg, hi);
  if (sign_extend_word)
    la_ld_w(laAsm, reg, reg, lo);
  else
    la_ld_wu(laAsm, reg, reg, lo);
}

[[maybe_unused]] void laEmitFarStore(lagoon_assembler_t* laAsm, la_gpr_t reg, const void* addr, la_gpr_t tempreg)
{
  const auto [hi, lo] = laGetAddressImmediates12(laAsm->cursor, addr);
  la_pcaddu12i(laAsm, tempreg, hi);
  la_st_w(laAsm, reg, tempreg, lo);
}

void laEmitSExtB(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs)
{
  la_ext_w_b(laAsm, rd, rs);
}

void laEmitUExtB(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs)
{
  la_andi(laAsm, rd, rs, 0xFF);
}

void laEmitSExtH(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs)
{
  la_ext_w_h(laAsm, rd, rs);
}

void laEmitUExtH(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs)
{
  la_bstrpick_d(laAsm, rd, rs, 15, 0);
}

void laEmitDSExtW(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs)
{
  la_addi_w(laAsm, rd, rs, 0);
}

void laEmitDUExtW(lagoon_assembler_t* laAsm, la_gpr_t rd, la_gpr_t rs)
{
  la_bstrpick_d(laAsm, rd, rs, 31, 0);
}

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  const u32* code = static_cast<const u32*>(start);
  const u32 count = size / 4;
  char buf[256];
  for (u32 i = 0; i < count; i++)
  {
    lagoon_insn_t insn;
    la_disasm_one(*(code + i), &insn);
    la_insn_to_str(&insn, buf, sizeof(buf));
    INFO_LOG("\t0x{:016X}\t{}", reinterpret_cast<uintptr_t>(code + i), buf);
  }
#else
  ERROR_LOG("Not compiled with ENABLE_HOST_DISASSEMBLY.");
#endif
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  return size / 4;
#else
  ERROR_LOG("Not compiled with ENABLE_HOST_DISASSEMBLY.");
  return size / 4;
#endif
}

u32 CPU::CodeCache::EmitASMFunctions(void* code, u32 code_size)
{
  lagoon_assembler_t asm_obj;
  lagoon_assembler_t* laAsm = &asm_obj;
  la_init_assembler(laAsm, static_cast<u8*>(code), code_size);

  lagoon_label_t dispatch = {};
  lagoon_label_t run_events_and_dispatch = {};

  g_enter_recompiler = reinterpret_cast<decltype(g_enter_recompiler)>(laAsm->cursor);
  {
    // TODO: reserve some space for saving caller-saved registers

    // Need the CPU state for basically everything :-)
    laMoveAddressToReg(laAsm, RSTATE, &g_state);
    // Fastmem setup
    if (IsUsingFastmem())
      la_ld_d(laAsm, RMEMBASE, RSTATE, OFFS(&g_state.fastmem_base));

    // Fall through to event dispatcher
  }

  // check events then for frame done
  {
    lagoon_label_t skip_event_check = {};
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    la_ld_w(laAsm, RARG2, RSTATE, OFFS(&g_state.downcount));
    la_bltu(laAsm, RARG1, RARG2, la_label(laAsm, &skip_event_check));

    la_bind(laAsm, &run_events_and_dispatch);
    g_run_events_and_dispatch = laAsm->cursor;
    laEmitCall(laAsm, reinterpret_cast<const void*>(&TimingEvents::RunEvents));

    la_bind(laAsm, &skip_event_check);
    la_label_free(laAsm, &skip_event_check);
  }

  // TODO: align?
  g_dispatcher = laAsm->cursor;
  {
    la_bind(laAsm, &dispatch);

    // x9 <- s_fast_map[pc >> 16]
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pc));
    laMoveAddressToReg(laAsm, RARG3, g_code_lut.data());
    la_srli_w(laAsm, RARG2, RARG1, 16);
    la_slli_d(laAsm, RARG2, RARG2, 3);
    la_add_d(laAsm, RARG2, RARG2, RARG3);
    la_ld_d(laAsm, RARG2, RARG2, 0);
    la_slli_d(laAsm, RARG1, RARG1, 48); // idx = (pc & 0xFFFF) >> 2
    la_srli_d(laAsm, RARG1, RARG1, 50);
    la_slli_d(laAsm, RARG1, RARG1, 3);

    // blr(x9[pc * 2]) (fast_map[idx])
    la_add_d(laAsm, RARG1, RARG1, RARG2);
    la_ld_d(laAsm, RARG1, RARG1, 0);
    la_jirl(laAsm, LA_ZERO, RARG1, 0);
  }

  g_compile_or_revalidate_block = laAsm->cursor;
  {
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pc));
    laEmitCall(laAsm, reinterpret_cast<const void*>(&CompileOrRevalidateBlock));
    la_b(laAsm, la_label(laAsm, &dispatch));
  }

  g_discard_and_recompile_block = laAsm->cursor;
  {
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pc));
    laEmitCall(laAsm, reinterpret_cast<const void*>(&DiscardAndRecompileBlock));
    la_b(laAsm, la_label(laAsm, &dispatch));
  }

  g_interpret_block = laAsm->cursor;
  {
    laEmitCall(laAsm, CodeCache::GetInterpretUncachedBlockFunction());
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    la_ld_w(laAsm, RARG2, RSTATE, OFFS(&g_state.downcount));
    la_bge(laAsm, RARG1, RARG2, la_label(laAsm, &run_events_and_dispatch));
    la_b(laAsm, la_label(laAsm, &dispatch));
  }

  la_label_free(laAsm, &dispatch);
  la_label_free(laAsm, &run_events_and_dispatch);

  // TODO: align?

  return static_cast<u32>(laAsm->cursor - laAsm->buffer);
}

void CPU::CodeCache::EmitAlignmentPadding(void* dst, size_t size)
{
  constexpr u8 padding_value = 0x00;
  std::memset(dst, padding_value, size);
}

u32 CPU::CodeCache::EmitJump(void* code, const void* dst, bool flush_icache)
{
  {
    lagoon_assembler_t assembler;
    la_init_assembler(&assembler, static_cast<u8*>(code), BLOCK_LINK_SIZE);
    laEmitCall(&assembler, dst);

    DebugAssert(static_cast<size_t>(assembler.cursor - assembler.buffer) <= BLOCK_LINK_SIZE);
    if (la_get_remaining_buffer_size(&assembler) > 0)
      la_andi(&assembler, LA_ZERO, LA_ZERO, 0); // NOP
  }

  if (flush_icache)
    MemMap::FlushInstructionCache(code, BLOCK_LINK_SIZE);

  return BLOCK_LINK_SIZE;
}

CPU::LoongArch64Recompiler::LoongArch64Recompiler() = default;

CPU::LoongArch64Recompiler::~LoongArch64Recompiler() = default;

const void* CPU::LoongArch64Recompiler::GetCurrentCodePointer()
{
  return laAsm->cursor;
}

void CPU::LoongArch64Recompiler::Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space,
                                       u8* far_code_buffer, u32 far_code_space)
{
  Recompiler::Reset(block, code_buffer, code_buffer_space, far_code_buffer, far_code_space);

  DebugAssert(!laAsm);
  la_init_assembler(&m_emitter, code_buffer, code_buffer_space);
  la_init_assembler(&m_far_emitter, far_code_buffer, far_code_space);
  laAsm = &m_emitter;

  // Need to wipe it out so it's correct when toggling fastmem.
  m_host_regs = {};

  const u32 membase_idx = CodeCache::IsUsingFastmem() ? RMEMBASE : NUM_HOST_REGS;
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& hra = m_host_regs[i];

    // Reserved: zero(0), ra(1), tp(2), sp(3), r21(reserved)
    if (i == RARG1 || i == RARG2 || i == RARG3 || i == RSCRATCH || i == RSTATE || i == membase_idx || i < 4 || i == 21)
    {
      continue;
    }

    hra.flags = HR_USABLE | (laIsCallerSavedRegister(i) ? 0 : HR_CALLEE_SAVED);
  }
}

void CPU::LoongArch64Recompiler::SwitchToFarCode(bool emit_jump, LaBranchCondition cond, la_gpr_t rs1, la_gpr_t rs2)
{
  DebugAssert(laAsm == &m_emitter);
  if (emit_jump)
  {
    const void* target = m_far_emitter.cursor;
    if (cond != LaBranchCondition::None)
    {
      lagoon_label_t skip = {};
      switch (cond)
      {
        case LaBranchCondition::EQ:
          la_bne(laAsm, rs1, rs2, la_label(laAsm, &skip));
          break;
        case LaBranchCondition::NE:
          la_beq(laAsm, rs1, rs2, la_label(laAsm, &skip));
          break;
        case LaBranchCondition::LT:
          la_bge(laAsm, rs1, rs2, la_label(laAsm, &skip));
          break;
        case LaBranchCondition::GE:
          la_blt(laAsm, rs1, rs2, la_label(laAsm, &skip));
          break;
        case LaBranchCondition::LTU:
          la_bgeu(laAsm, rs1, rs2, la_label(laAsm, &skip));
          break;
        case LaBranchCondition::GEU:
          la_bltu(laAsm, rs1, rs2, la_label(laAsm, &skip));
          break;
        default:
          break;
      }
      laEmitJmp(laAsm, target);
      la_bind(laAsm, &skip);
      la_label_free(laAsm, &skip);
    }
    else
    {
      laEmitCall(laAsm, target);
    }
  }
  laAsm = &m_far_emitter;
}

void CPU::LoongArch64Recompiler::SwitchToNearCode(bool emit_jump)
{
  DebugAssert(laAsm == &m_far_emitter);
  if (emit_jump)
    laEmitJmp(laAsm, m_emitter.cursor);
  laAsm = &m_emitter;
}

void CPU::LoongArch64Recompiler::EmitMov(la_gpr_t dst, u32 val)
{
  laEmitMov(laAsm, dst, val);
}

void CPU::LoongArch64Recompiler::EmitCall(const void* ptr)
{
  laEmitCall(laAsm, ptr);
}

void CPU::LoongArch64Recompiler::SafeImmSImm12(la_gpr_t rd, la_gpr_t rs, u32 imm, LaRRSImmOp iop, LaRRROp rop)
{
  DebugAssert(rd != RSCRATCH && rs != RSCRATCH);

  if (laIsValidSImm12(imm))
  {
    iop(laAsm, rd, rs, imm);
    return;
  }

  laEmitMov(laAsm, RSCRATCH, imm);
  rop(laAsm, rd, rs, RSCRATCH);
}

void CPU::LoongArch64Recompiler::SafeImmUImm12(la_gpr_t rd, la_gpr_t rs, u32 imm, LaRRUImmOp iop, LaRRROp rop)
{
  DebugAssert(rd != RSCRATCH && rs != RSCRATCH);

  if (laIsValidUImm12(imm))
  {
    iop(laAsm, rd, rs, imm);
    return;
  }

  laEmitMov(laAsm, RSCRATCH, imm);
  rop(laAsm, rd, rs, RSCRATCH);
}

void CPU::LoongArch64Recompiler::SafeADDI(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  SafeImmSImm12(rd, rs, imm, la_addi_d, la_add_d);
}

void CPU::LoongArch64Recompiler::SafeADDIW(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  SafeImmSImm12(rd, rs, imm, la_addi_w, la_add_w);
}

void CPU::LoongArch64Recompiler::SafeSUBIW(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  const u32 nimm = static_cast<u32>(-static_cast<s32>(imm));
  SafeImmSImm12(rd, rs, nimm, la_addi_w, la_add_w);
}

void CPU::LoongArch64Recompiler::SafeANDI(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  SafeImmUImm12(rd, rs, imm, la_andi, la_and);
}

void CPU::LoongArch64Recompiler::SafeORI(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  SafeImmUImm12(rd, rs, imm, la_ori, la_or);
}

void CPU::LoongArch64Recompiler::SafeXORI(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  SafeImmUImm12(rd, rs, imm, la_xori, la_xor);
}

void CPU::LoongArch64Recompiler::SafeSLTI(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  SafeImmSImm12(rd, rs, imm, la_slti, la_slt);
}

void CPU::LoongArch64Recompiler::SafeSLTIU(la_gpr_t rd, la_gpr_t rs, u32 imm)
{
  SafeImmSImm12(rd, rs, imm, la_sltui, la_sltu);
}

void CPU::LoongArch64Recompiler::EmitSExtB(la_gpr_t rd, la_gpr_t rs)
{
  laEmitSExtB(laAsm, rd, rs);
}

void CPU::LoongArch64Recompiler::EmitUExtB(la_gpr_t rd, la_gpr_t rs)
{
  laEmitUExtB(laAsm, rd, rs);
}

void CPU::LoongArch64Recompiler::EmitSExtH(la_gpr_t rd, la_gpr_t rs)
{
  laEmitSExtH(laAsm, rd, rs);
}

void CPU::LoongArch64Recompiler::EmitUExtH(la_gpr_t rd, la_gpr_t rs)
{
  laEmitUExtH(laAsm, rd, rs);
}

void CPU::LoongArch64Recompiler::EmitDSExtW(la_gpr_t rd, la_gpr_t rs)
{
  laEmitDSExtW(laAsm, rd, rs);
}

void CPU::LoongArch64Recompiler::EmitDUExtW(la_gpr_t rd, la_gpr_t rs)
{
  laEmitDUExtW(laAsm, rd, rs);
}

void CPU::LoongArch64Recompiler::GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size)
{
  // store it first to reduce code size, because we can offset
  laEmitMov64(laAsm, RARG1, static_cast<u64>(reinterpret_cast<uintptr_t>(ram_ptr)));
  laEmitMov64(laAsm, RARG2, static_cast<u64>(reinterpret_cast<uintptr_t>(shadow_ptr)));

  u32 offset = 0;
  lagoon_label_t block_changed = {};

  while (size >= 8)
  {
    la_ld_d(laAsm, RARG3, RARG1, offset);
    la_ld_d(laAsm, RSCRATCH, RARG2, offset);
    la_bne(laAsm, RARG3, RSCRATCH, la_label(laAsm, &block_changed));
    offset += 8;
    size -= 8;
  }

  while (size >= 4)
  {
    la_ld_w(laAsm, RARG3, RARG1, offset);
    la_ld_w(laAsm, RSCRATCH, RARG2, offset);
    la_bne(laAsm, RARG3, RSCRATCH, la_label(laAsm, &block_changed));
    offset += 4;
    size -= 4;
  }

  DebugAssert(size == 0);

  lagoon_label_t block_unchanged = {};
  la_b(laAsm, la_label(laAsm, &block_unchanged));
  la_bind(laAsm, &block_changed);
  laEmitJmp(laAsm, CodeCache::g_discard_and_recompile_block);
  la_bind(laAsm, &block_unchanged);
  la_label_free(laAsm, &block_changed);
  la_label_free(laAsm, &block_unchanged);
}

void CPU::LoongArch64Recompiler::GenerateICacheCheckAndUpdate()
{
  if (!m_block->HasFlag(CodeCache::BlockFlags::IsUsingICache))
  {
    if (m_block->HasFlag(CodeCache::BlockFlags::NeedsDynamicFetchTicks))
    {
      laEmitFarLoad(laAsm, RARG2, GetFetchMemoryAccessTimePtr());
      la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
      laEmitMov(laAsm, RARG3, m_block->size);
      la_mul_w(laAsm, RARG2, RARG2, RARG3);
      la_add_d(laAsm, RARG1, RARG1, RARG2);
      la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    }
    else
    {
      la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
      SafeADDIW(RARG1, RARG1, static_cast<u32>(m_block->uncached_fetch_ticks));
      la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    }
  }
  else if (m_block->icache_line_count > 0)
  {
    const auto& ticks_reg = RARG1;
    const auto& current_tag_reg = RARG2;
    const auto& existing_tag_reg = RARG3;

    // start of block, nothing should be using this
    const auto& maddr_reg = LA_T0;
    DebugAssert(!IsHostRegAllocated(maddr_reg));

    VirtualMemoryAddress current_pc = m_block->pc & ICACHE_TAG_ADDRESS_MASK;
    la_ld_w(laAsm, ticks_reg, RSTATE, OFFS(&g_state.pending_ticks));
    laEmitMov(laAsm, current_tag_reg, current_pc);

    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const TickCount fill_ticks = GetICacheFillTicks(current_pc);
      if (fill_ticks <= 0)
        continue;

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = OFFSETOF(State, icache_tags) + (line * sizeof(u32));

      // Offsets must fit in signed 12 bits.
      lagoon_label_t cache_hit = {};
      if (offset >= 2048)
      {
        SafeADDI(maddr_reg, RSTATE, offset);
        la_ld_w(laAsm, existing_tag_reg, maddr_reg, 0);
        la_beq(laAsm, existing_tag_reg, current_tag_reg, la_label(laAsm, &cache_hit));
        la_st_w(laAsm, current_tag_reg, maddr_reg, 0);
      }
      else
      {
        la_ld_w(laAsm, existing_tag_reg, RSTATE, offset);
        la_beq(laAsm, existing_tag_reg, current_tag_reg, la_label(laAsm, &cache_hit));
        la_st_w(laAsm, current_tag_reg, RSTATE, offset);
      }

      SafeADDIW(ticks_reg, ticks_reg, static_cast<u32>(fill_ticks));
      la_bind(laAsm, &cache_hit);
      la_label_free(laAsm, &cache_hit);

      if (i != (m_block->icache_line_count - 1))
        SafeADDIW(current_tag_reg, current_tag_reg, ICACHE_LINE_SIZE);
    }

    la_st_w(laAsm, ticks_reg, RSTATE, OFFS(&g_state.pending_ticks));
  }
}

void CPU::LoongArch64Recompiler::GenerateCall(const void* func, s32 arg1reg /*= -1*/, s32 arg2reg /*= -1*/,
                                              s32 arg3reg /*= -1*/)
{
  if (arg1reg >= 0 && arg1reg != static_cast<s32>(RARG1))
    la_or(laAsm, RARG1, static_cast<la_gpr_t>(arg1reg), LA_ZERO);
  if (arg2reg >= 0 && arg2reg != static_cast<s32>(RARG2))
    la_or(laAsm, RARG2, static_cast<la_gpr_t>(arg2reg), LA_ZERO);
  if (arg3reg >= 0 && arg3reg != static_cast<s32>(RARG3))
    la_or(laAsm, RARG3, static_cast<la_gpr_t>(arg3reg), LA_ZERO);
  EmitCall(func);
}

void CPU::LoongArch64Recompiler::EndBlock(const std::optional<u32>& newpc, bool do_event_test)
{
  if (newpc.has_value())
  {
    if (m_dirty_pc || m_compiler_pc != newpc)
    {
      EmitMov(RSCRATCH, newpc.value());
      la_st_w(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.pc));
    }
  }
  m_dirty_pc = false;

  // flush regs
  Flush(FLUSH_END_BLOCK);
  EndAndLinkBlock(newpc, do_event_test, false);
}

void CPU::LoongArch64Recompiler::EndBlockWithException(Exception excode)
{
  // flush regs, but not pc, it's going to get overwritten
  // flush cycles because of the GTE instruction stuff...
  Flush(FLUSH_END_BLOCK | FLUSH_FOR_EXCEPTION | FLUSH_FOR_C_CALL);

  // TODO: flush load delay

  EmitMov(RARG1, Cop0Registers::CAUSE::MakeValueForException(excode, m_current_instruction_branch_delay_slot, false,
                                                             inst->cop.cop_n));
  EmitMov(RARG2, m_current_instruction_pc);
  if (excode != Exception::BP)
  {
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
  }
  else
  {
    EmitMov(RARG3, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&CPU::RaiseBreakException));
  }
  m_dirty_pc = false;

  EndAndLinkBlock(std::nullopt, true, false);
}

void CPU::LoongArch64Recompiler::EndAndLinkBlock(const std::optional<u32>& newpc, bool do_event_test,
                                                 bool force_run_events)
{
  // event test
  // pc should've been flushed
  DebugAssert(!m_dirty_pc && !m_block_ended);
  m_block_ended = true;

  // TODO: try extracting this to a function
  // TODO: move the cycle flush in here..

  // save cycles for event test
  const TickCount cycles = std::exchange(m_cycles, 0);

  // pending_ticks += cycles
  // if (pending_ticks >= downcount) { dispatch_event(); }
  if (do_event_test || m_gte_done_cycle > cycles || cycles > 0)
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
  if (do_event_test)
    la_ld_w(laAsm, RARG2, RSTATE, OFFS(&g_state.downcount));
  if (cycles > 0)
  {
    SafeADDIW(RARG1, RARG1, cycles);
    la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
  }
  if (m_gte_done_cycle > cycles)
  {
    SafeADDIW(RARG3, RARG1, m_gte_done_cycle - cycles);
    la_st_w(laAsm, RARG3, RSTATE, OFFS(&g_state.gte_completion_tick));
  }

  if (do_event_test)
  {
    // TODO: see if we can do a far jump somehow with this..
    lagoon_label_t cont = {};
    la_blt(laAsm, RARG1, RARG2, la_label(laAsm, &cont));
    laEmitJmp(laAsm, CodeCache::g_run_events_and_dispatch);
    la_bind(laAsm, &cont);
    la_label_free(laAsm, &cont);
  }

  // jump to dispatcher or next block
  if (force_run_events)
  {
    laEmitJmp(laAsm, CodeCache::g_run_events_and_dispatch);
  }
  else if (!newpc.has_value())
  {
    laEmitJmp(laAsm, CodeCache::g_dispatcher);
  }
  else
  {
    const void* target = (newpc.value() == m_block->pc) ?
                           CodeCache::CreateSelfBlockLink(m_block, laAsm->cursor, laAsm->buffer) :
                           CodeCache::CreateBlockLink(m_block, laAsm->cursor, newpc.value());
    laEmitJmp(laAsm, target);
  }
}

const void* CPU::LoongArch64Recompiler::EndCompile(u32* code_size, u32* far_code_size)
{
  u8* const code = m_emitter.buffer;
  *code_size = static_cast<u32>(m_emitter.cursor - m_emitter.buffer);
  *far_code_size = static_cast<u32>(m_far_emitter.cursor - m_far_emitter.buffer);
  laAsm = nullptr;
  return code;
}

const char* CPU::LoongArch64Recompiler::GetHostRegName(u32 reg) const
{
  static constexpr std::array<const char*, 32> reg64_names = {
    {"$zero", "$ra", "$tp", "$sp", "$a0", "$a1", "$a2", "$a3", "$a4", "$a5", "$a6",
     "$a7",   "$t0", "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7", "$t8", "$r21",
     "$fp",   "$s0", "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7", "$s8"}};
  return (reg < reg64_names.size()) ? reg64_names[reg] : "UNKNOWN";
}

void CPU::LoongArch64Recompiler::LoadHostRegWithConstant(u32 reg, u32 val)
{
  EmitMov(static_cast<la_gpr_t>(reg), val);
}

void CPU::LoongArch64Recompiler::LoadHostRegFromCPUPointer(u32 reg, const void* ptr)
{
  la_ld_w(laAsm, static_cast<la_gpr_t>(reg), RSTATE, OFFS(ptr));
}

void CPU::LoongArch64Recompiler::StoreHostRegToCPUPointer(u32 reg, const void* ptr)
{
  la_st_w(laAsm, static_cast<la_gpr_t>(reg), RSTATE, OFFS(ptr));
}

void CPU::LoongArch64Recompiler::StoreConstantToCPUPointer(u32 val, const void* ptr)
{
  if (val == 0)
  {
    la_st_w(laAsm, LA_ZERO, RSTATE, OFFS(ptr));
    return;
  }

  EmitMov(RSCRATCH, val);
  la_st_w(laAsm, RSCRATCH, RSTATE, OFFS(ptr));
}

void CPU::LoongArch64Recompiler::CopyHostReg(u32 dst, u32 src)
{
  if (src != dst)
    la_or(laAsm, static_cast<la_gpr_t>(dst), static_cast<la_gpr_t>(src), LA_ZERO);
}

void CPU::LoongArch64Recompiler::AssertRegOrConstS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s || cf.const_s);
}

void CPU::LoongArch64Recompiler::AssertRegOrConstT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t || cf.const_t);
}

la_gpr_t CPU::LoongArch64Recompiler::CFGetSafeRegS(CompileFlags cf, la_gpr_t temp_reg)
{
  if (cf.valid_host_s)
  {
    return static_cast<la_gpr_t>(cf.host_s);
  }
  else if (cf.const_s)
  {
    if (HasConstantRegValue(cf.MipsS(), 0))
      return LA_ZERO;

    EmitMov(temp_reg, GetConstantRegU32(cf.MipsS()));
    return temp_reg;
  }
  else
  {
    WARNING_LOG("Hit memory path in CFGetSafeRegS() for {}", GetRegName(cf.MipsS()));
    la_ld_w(laAsm, temp_reg, RSTATE, OFFS(&g_state.regs.r[cf.mips_s]));
    return temp_reg;
  }
}

la_gpr_t CPU::LoongArch64Recompiler::CFGetSafeRegT(CompileFlags cf, la_gpr_t temp_reg)
{
  if (cf.valid_host_t)
  {
    return static_cast<la_gpr_t>(cf.host_t);
  }
  else if (cf.const_t)
  {
    if (HasConstantRegValue(cf.MipsT(), 0))
      return LA_ZERO;

    EmitMov(temp_reg, GetConstantRegU32(cf.MipsT()));
    return temp_reg;
  }
  else
  {
    WARNING_LOG("Hit memory path in CFGetSafeRegT() for {}", GetRegName(cf.MipsT()));
    la_ld_w(laAsm, temp_reg, RSTATE, OFFS(&g_state.regs.r[cf.mips_t]));
    return temp_reg;
  }
}

la_gpr_t CPU::LoongArch64Recompiler::CFGetRegD(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_d);
  return static_cast<la_gpr_t>(cf.host_d);
}

la_gpr_t CPU::LoongArch64Recompiler::CFGetRegS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s);
  return static_cast<la_gpr_t>(cf.host_s);
}

la_gpr_t CPU::LoongArch64Recompiler::CFGetRegT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t);
  return static_cast<la_gpr_t>(cf.host_t);
}

la_gpr_t CPU::LoongArch64Recompiler::CFGetRegLO(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_lo);
  return static_cast<la_gpr_t>(cf.host_lo);
}

la_gpr_t CPU::LoongArch64Recompiler::CFGetRegHI(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_hi);
  return static_cast<la_gpr_t>(cf.host_hi);
}

void CPU::LoongArch64Recompiler::MoveSToReg(la_gpr_t dst, CompileFlags cf)
{
  if (cf.valid_host_s)
  {
    if (cf.host_s != dst)
      la_or(laAsm, dst, static_cast<la_gpr_t>(cf.host_s), LA_ZERO);
  }
  else if (cf.const_s)
  {
    EmitMov(dst, GetConstantRegU32(cf.MipsS()));
  }
  else
  {
    WARNING_LOG("Hit memory path in MoveSToReg() for {}", GetRegName(cf.MipsS()));
    la_ld_w(laAsm, dst, RSTATE, OFFS(&g_state.regs.r[cf.mips_s]));
  }
}

void CPU::LoongArch64Recompiler::MoveTToReg(la_gpr_t dst, CompileFlags cf)
{
  if (cf.valid_host_t)
  {
    if (cf.host_t != dst)
      la_or(laAsm, dst, static_cast<la_gpr_t>(cf.host_t), LA_ZERO);
  }
  else if (cf.const_t)
  {
    EmitMov(dst, GetConstantRegU32(cf.MipsT()));
  }
  else
  {
    WARNING_LOG("Hit memory path in MoveTToReg() for {}", GetRegName(cf.MipsT()));
    la_ld_w(laAsm, dst, RSTATE, OFFS(&g_state.regs.r[cf.mips_t]));
  }
}

void CPU::LoongArch64Recompiler::MoveMIPSRegToReg(la_gpr_t dst, Reg reg, bool ignore_load_delays)
{
  DebugAssert(reg < Reg::count);
  if (ignore_load_delays && m_load_delay_register == reg)
  {
    if (m_load_delay_value_register == NUM_HOST_REGS)
      la_ld_w(laAsm, dst, RSTATE, OFFS(&g_state.load_delay_value));
    else
      la_or(laAsm, dst, static_cast<la_gpr_t>(m_load_delay_value_register), LA_ZERO);
  }
  else if (const std::optional<u32> hreg = CheckHostReg(0, Recompiler::HR_TYPE_CPU_REG, reg))
  {
    la_or(laAsm, dst, static_cast<la_gpr_t>(hreg.value()), LA_ZERO);
  }
  else if (HasConstantReg(reg))
  {
    EmitMov(dst, GetConstantRegU32(reg));
  }
  else
  {
    la_ld_w(laAsm, dst, RSTATE, OFFS(&g_state.regs.r[static_cast<u8>(reg)]));
  }
}

void CPU::LoongArch64Recompiler::GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val,
                                                              Reg arg2reg /* = Reg::count */,
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

void CPU::LoongArch64Recompiler::Flush(u32 flags)
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
    Panic("Not implemented");
  }

  if (flags & FLUSH_LOAD_DELAY_FROM_STATE && m_load_delay_dirty)
  {
    // This sucks :(
    // TODO: make it a function?
    la_ld_bu(laAsm, RARG1, RSTATE, OFFS(&g_state.load_delay_reg));
    la_ld_w(laAsm, RARG2, RSTATE, OFFS(&g_state.load_delay_value));
    la_slli_d(laAsm, RARG1, RARG1, 2); // *4
    la_add_d(laAsm, RARG1, RARG1, RSTATE);
    la_st_w(laAsm, RARG2, RARG1, OFFSETOF(CPU::State, regs.r[0]));
    la_addi_d(laAsm, RSCRATCH, LA_ZERO, static_cast<u8>(Reg::count));
    la_st_b(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.load_delay_reg));
    m_load_delay_dirty = false;
  }

  if (flags & FLUSH_LOAD_DELAY && m_load_delay_register != Reg::count)
  {
    if (m_load_delay_value_register != NUM_HOST_REGS)
      FreeHostReg(m_load_delay_value_register);

    EmitMov(RSCRATCH, static_cast<u8>(m_load_delay_register));
    la_st_b(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.load_delay_reg));
    m_load_delay_register = Reg::count;
    m_load_delay_dirty = true;
  }

  if (flags & FLUSH_GTE_STALL_FROM_STATE && m_dirty_gte_done_cycle)
  {
    // May as well flush cycles while we're here.
    // GTE spanning blocks is very rare, we _could_ disable this for speed.
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    la_ld_w(laAsm, RARG2, RSTATE, OFFS(&g_state.gte_completion_tick));
    if (m_cycles > 0)
    {
      SafeADDIW(RARG1, RARG1, m_cycles);
      la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
      m_cycles = 0;
    }

    lagoon_label_t no_stall = {};
    la_bge(laAsm, RARG1, RARG2, la_label(laAsm, &no_stall));
    la_or(laAsm, RARG1, RARG2, LA_ZERO);
    la_bind(laAsm, &no_stall);
    la_label_free(laAsm, &no_stall);
    la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    m_dirty_gte_done_cycle = false;
  }

  if (flags & FLUSH_GTE_DONE_CYCLE && m_gte_done_cycle > m_cycles)
  {
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));

    // update cycles at the same time
    if (flags & FLUSH_CYCLES && m_cycles > 0)
    {
      SafeADDIW(RARG1, RARG1, m_cycles);
      la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
      m_gte_done_cycle -= m_cycles;
      m_cycles = 0;
    }

    SafeADDIW(RARG1, RARG1, m_gte_done_cycle);
    la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.gte_completion_tick));
    m_gte_done_cycle = 0;
    m_dirty_gte_done_cycle = true;
  }

  if (flags & FLUSH_CYCLES && m_cycles > 0)
  {
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    SafeADDIW(RARG1, RARG1, m_cycles);
    la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pending_ticks));
    m_gte_done_cycle = std::max<TickCount>(m_gte_done_cycle - m_cycles, 0);
    m_cycles = 0;
  }
}

void CPU::LoongArch64Recompiler::Compile_Fallback()
{
  WARNING_LOG("Compiling instruction fallback at PC=0x{:08X}, instruction=0x{:08X}", m_current_instruction_pc,
              inst->bits);

  Flush(FLUSH_FOR_INTERPRETER);

  Panic("Fixme");
}

void CPU::LoongArch64Recompiler::CheckBranchTarget(la_gpr_t pcreg)
{
  if (!g_settings.cpu_recompiler_memory_exceptions)
    return;

  DebugAssert(pcreg != RSCRATCH);
  la_andi(laAsm, RSCRATCH, pcreg, 0x3);
  SwitchToFarCode(true, LaBranchCondition::NE, RSCRATCH, LA_ZERO);

  BackupHostState();
  EndBlockWithException(Exception::AdEL);

  RestoreHostState();
  SwitchToNearCode(false);
}

void CPU::LoongArch64Recompiler::Compile_jr(CompileFlags cf)
{
  const la_gpr_t pcreg = CFGetRegS(cf);
  CheckBranchTarget(pcreg);

  la_st_w(laAsm, pcreg, RSTATE, OFFS(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::LoongArch64Recompiler::Compile_jalr(CompileFlags cf)
{
  const la_gpr_t pcreg = CFGetRegS(cf);
  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetBranchReturnAddress(cf));

  CheckBranchTarget(pcreg);
  la_st_w(laAsm, pcreg, RSTATE, OFFS(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::LoongArch64Recompiler::Compile_bxx(CompileFlags cf, BranchCondition cond)
{
  AssertRegOrConstS(cf);

  const u32 taken_pc = GetConditionalBranchTarget(cf);

  Flush(FLUSH_FOR_BRANCH);

  DebugAssert(cf.valid_host_s);

  // MipsT() here should equal zero for zero branches.
  DebugAssert(cond == BranchCondition::Equal || cond == BranchCondition::NotEqual || cf.MipsT() == Reg::zero);

  lagoon_label_t taken = {};
  const la_gpr_t rs = CFGetRegS(cf);
  switch (cond)
  {
    case BranchCondition::Equal:
    case BranchCondition::NotEqual:
    {
      AssertRegOrConstT(cf);
      if (cf.const_t && HasConstantRegValue(cf.MipsT(), 0))
      {
        (cond == BranchCondition::Equal) ? la_beqz(laAsm, rs, la_label(laAsm, &taken)) :
                                           la_bnez(laAsm, rs, la_label(laAsm, &taken));
      }
      else
      {
        const la_gpr_t rt = cf.valid_host_t ? CFGetRegT(cf) : RARG1;
        if (!cf.valid_host_t)
          MoveTToReg(RARG1, cf);
        if (cond == Recompiler::BranchCondition::Equal)
          la_beq(laAsm, rs, rt, la_label(laAsm, &taken));
        else
          la_bne(laAsm, rs, rt, la_label(laAsm, &taken));
      }
    }
    break;

    case BranchCondition::GreaterThanZero:
    {
      la_blt(laAsm, LA_ZERO, rs, la_label(laAsm, &taken));
    }
    break;

    case BranchCondition::GreaterEqualZero:
    {
      la_bge(laAsm, rs, LA_ZERO, la_label(laAsm, &taken));
    }
    break;

    case BranchCondition::LessThanZero:
    {
      la_blt(laAsm, rs, LA_ZERO, la_label(laAsm, &taken));
    }
    break;

    case BranchCondition::LessEqualZero:
    {
      la_bge(laAsm, LA_ZERO, rs, la_label(laAsm, &taken));
    }
    break;
  }

  BackupHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(m_compiler_pc, true);

  la_bind(laAsm, &taken);
  la_label_free(laAsm, &taken);

  RestoreHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(taken_pc, true);
}

void CPU::LoongArch64Recompiler::Compile_addi(CompileFlags cf, bool overflow)
{
  const la_gpr_t rs = CFGetRegS(cf);
  const la_gpr_t rt = CFGetRegT(cf);
  if (const u32 imm = inst->i.imm_sext32(); imm != 0)
  {
    if (!overflow)
    {
      SafeADDIW(rt, rs, imm);
    }
    else
    {
      SafeADDI(RARG1, rs, imm);
      SafeADDIW(rt, rs, imm);
      TestOverflow(RARG1, rt, rt);
    }
  }
  else if (rt != rs)
  {
    la_or(laAsm, rt, rs, LA_ZERO);
  }
}

void CPU::LoongArch64Recompiler::Compile_addi(CompileFlags cf)
{
  Compile_addi(cf, g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::LoongArch64Recompiler::Compile_addiu(CompileFlags cf)
{
  Compile_addi(cf, false);
}

void CPU::LoongArch64Recompiler::Compile_slti(CompileFlags cf)
{
  Compile_slti(cf, true);
}

void CPU::LoongArch64Recompiler::Compile_sltiu(CompileFlags cf)
{
  Compile_slti(cf, false);
}

void CPU::LoongArch64Recompiler::Compile_slti(CompileFlags cf, bool sign)
{
  if (sign)
    SafeSLTI(CFGetRegT(cf), CFGetRegS(cf), inst->i.imm_sext32());
  else
    SafeSLTIU(CFGetRegT(cf), CFGetRegS(cf), inst->i.imm_sext32());
}

void CPU::LoongArch64Recompiler::Compile_andi(CompileFlags cf)
{
  const la_gpr_t rt = CFGetRegT(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    SafeANDI(rt, CFGetRegS(cf), imm);
  else
    EmitMov(rt, 0);
}

void CPU::LoongArch64Recompiler::Compile_ori(CompileFlags cf)
{
  const la_gpr_t rt = CFGetRegT(cf);
  const la_gpr_t rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    SafeORI(rt, rs, imm);
  else if (rt != rs)
    la_or(laAsm, rt, rs, LA_ZERO);
}

void CPU::LoongArch64Recompiler::Compile_xori(CompileFlags cf)
{
  const la_gpr_t rt = CFGetRegT(cf);
  const la_gpr_t rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    SafeXORI(rt, rs, imm);
  else if (rt != rs)
    la_or(laAsm, rt, rs, LA_ZERO);
}

void CPU::LoongArch64Recompiler::Compile_shift(CompileFlags cf, LaRRROp op, LaRRUImmOp op_const)
{
  const la_gpr_t rd = CFGetRegD(cf);
  const la_gpr_t rt = CFGetRegT(cf);
  if (inst->r.shamt > 0)
    op_const(laAsm, rd, rt, inst->r.shamt);
  else if (rd != rt)
    la_or(laAsm, rd, rt, LA_ZERO);
}

void CPU::LoongArch64Recompiler::Compile_sll(CompileFlags cf)
{
  Compile_shift(cf, la_sll_w, la_slli_w);
}

void CPU::LoongArch64Recompiler::Compile_srl(CompileFlags cf)
{
  Compile_shift(cf, la_srl_w, la_srli_w);
}

void CPU::LoongArch64Recompiler::Compile_sra(CompileFlags cf)
{
  Compile_shift(cf, la_sra_w, la_srai_w);
}

void CPU::LoongArch64Recompiler::Compile_variable_shift(CompileFlags cf, LaRRROp op, LaRRUImmOp op_const)
{
  const la_gpr_t rd = CFGetRegD(cf);

  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const la_gpr_t rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  if (cf.const_s)
  {
    if (const u32 shift = GetConstantRegU32(cf.MipsS()); shift != 0)
      op_const(laAsm, rd, rt, shift & 31u);
    else if (rd != rt)
      la_or(laAsm, rd, rt, LA_ZERO);
  }
  else
  {
    op(laAsm, rd, rt, CFGetRegS(cf));
  }
}

void CPU::LoongArch64Recompiler::Compile_sllv(CompileFlags cf)
{
  Compile_variable_shift(cf, la_sll_w, la_slli_w);
}

void CPU::LoongArch64Recompiler::Compile_srlv(CompileFlags cf)
{
  Compile_variable_shift(cf, la_srl_w, la_srli_w);
}

void CPU::LoongArch64Recompiler::Compile_srav(CompileFlags cf)
{
  Compile_variable_shift(cf, la_sra_w, la_srai_w);
}

void CPU::LoongArch64Recompiler::Compile_mult(CompileFlags cf, bool sign)
{
  const la_gpr_t rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const la_gpr_t rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  // TODO: if lo/hi gets killed, we can use a 32-bit multiply
  const la_gpr_t lo = CFGetRegLO(cf);
  const la_gpr_t hi = CFGetRegHI(cf);

  if (sign)
  {
    la_mul_d(laAsm, lo, rs, rt);
    la_srai_d(laAsm, hi, lo, 32);
    EmitDSExtW(lo, lo);
  }
  else
  {
    EmitDUExtW(RARG1, rs);
    EmitDUExtW(RARG2, rt);
    la_mul_d(laAsm, lo, RARG1, RARG2);
    la_srai_d(laAsm, hi, lo, 32);
    EmitDSExtW(lo, lo);
  }
}

void CPU::LoongArch64Recompiler::Compile_mult(CompileFlags cf)
{
  Compile_mult(cf, true);
}

void CPU::LoongArch64Recompiler::Compile_multu(CompileFlags cf)
{
  Compile_mult(cf, false);
}

void CPU::LoongArch64Recompiler::Compile_div(CompileFlags cf)
{
  const la_gpr_t rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const la_gpr_t rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const la_gpr_t rlo = CFGetRegLO(cf);
  const la_gpr_t rhi = CFGetRegHI(cf);

  lagoon_label_t done = {};
  lagoon_label_t not_divide_by_zero = {};
  la_bnez(laAsm, rt, la_label(laAsm, &not_divide_by_zero));
  la_or(laAsm, rhi, rs, LA_ZERO); // hi = num
  la_srai_d(laAsm, rlo, rs, 63);
  la_andi(laAsm, rlo, rlo, 2);
  la_addi_d(laAsm, rlo, rlo, -1); // lo = s >= 0 ? -1 : 1
  la_b(laAsm, la_label(laAsm, &done));

  la_bind(laAsm, &not_divide_by_zero);
  la_label_free(laAsm, &not_divide_by_zero);

  lagoon_label_t not_unrepresentable = {};
  EmitMov(RSCRATCH, static_cast<u32>(-1));
  la_bne(laAsm, rt, RSCRATCH, la_label(laAsm, &not_unrepresentable));
  EmitMov(rlo, 0x80000000u);
  la_bne(laAsm, rs, rlo, la_label(laAsm, &not_unrepresentable));
  EmitMov(rhi, 0);
  la_b(laAsm, la_label(laAsm, &done));

  la_bind(laAsm, &not_unrepresentable);
  la_label_free(laAsm, &not_unrepresentable);

  la_div_w(laAsm, rlo, rs, rt);
  la_mod_w(laAsm, rhi, rs, rt);

  la_bind(laAsm, &done);
  la_label_free(laAsm, &done);
}

void CPU::LoongArch64Recompiler::Compile_divu(CompileFlags cf)
{
  const la_gpr_t rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const la_gpr_t rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const la_gpr_t rlo = CFGetRegLO(cf);
  const la_gpr_t rhi = CFGetRegHI(cf);

  // Semantics match? :-)
  la_div_wu(laAsm, rlo, rs, rt);
  la_mod_wu(laAsm, rhi, rs, rt);
}

void CPU::LoongArch64Recompiler::TestOverflow(la_gpr_t long_res, la_gpr_t res, la_gpr_t reg_to_discard)
{
  SwitchToFarCode(true, LaBranchCondition::NE, long_res, res);

  BackupHostState();

  // toss the result
  ClearHostReg(reg_to_discard);

  EndBlockWithException(Exception::Ov);

  RestoreHostState();

  SwitchToNearCode(false);
}

void CPU::LoongArch64Recompiler::Compile_dst_op(CompileFlags cf, LaRRROp op,
                                                void (LoongArch64Recompiler::*op_const)(la_gpr_t rd, la_gpr_t rs,
                                                                                        u32 imm),
                                                LaRRROp op_long, bool commutative, bool overflow)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const la_gpr_t rd = CFGetRegD(cf);

  if (overflow)
  {
    const la_gpr_t rs = CFGetSafeRegS(cf, RARG1);
    const la_gpr_t rt = CFGetSafeRegT(cf, RARG2);
    op_long(laAsm, RARG3, rs, rt);
    op(laAsm, rd, rs, rt);
    TestOverflow(RARG3, rd, rd);
    return;
  }

  if (cf.valid_host_s && cf.valid_host_t)
  {
    op(laAsm, rd, CFGetRegS(cf), CFGetRegT(cf));
  }
  else if (commutative && (cf.const_s || cf.const_t))
  {
    const la_gpr_t src = cf.const_s ? CFGetRegT(cf) : CFGetRegS(cf);
    if (const u32 cv = GetConstantRegU32(cf.const_s ? cf.MipsS() : cf.MipsT()); cv != 0)
    {
      (this->*op_const)(rd, src, cv);
    }
    else
    {
      if (rd != src)
        la_or(laAsm, rd, src, LA_ZERO);
      overflow = false;
    }
  }
  else if (cf.const_s)
  {
    if (HasConstantRegValue(cf.MipsS(), 0))
    {
      op(laAsm, rd, LA_ZERO, CFGetRegT(cf));
    }
    else
    {
      EmitMov(RSCRATCH, GetConstantRegU32(cf.MipsS()));
      op(laAsm, rd, RSCRATCH, CFGetRegT(cf));
    }
  }
  else if (cf.const_t)
  {
    const la_gpr_t rs = CFGetRegS(cf);
    if (const u32 cv = GetConstantRegU32(cf.const_s ? cf.MipsS() : cf.MipsT()); cv != 0)
    {
      (this->*op_const)(rd, rs, cv);
    }
    else
    {
      if (rd != rs)
        la_or(laAsm, rd, rs, LA_ZERO);
      overflow = false;
    }
  }
}

void CPU::LoongArch64Recompiler::Compile_add(CompileFlags cf)
{
  Compile_dst_op(cf, la_add_w, &LoongArch64Recompiler::SafeADDIW, la_add_d, true,
                 g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::LoongArch64Recompiler::Compile_addu(CompileFlags cf)
{
  Compile_dst_op(cf, la_add_w, &LoongArch64Recompiler::SafeADDIW, la_add_d, true, false);
}

void CPU::LoongArch64Recompiler::Compile_sub(CompileFlags cf)
{
  Compile_dst_op(cf, la_sub_w, &LoongArch64Recompiler::SafeSUBIW, la_sub_d, false,
                 g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::LoongArch64Recompiler::Compile_subu(CompileFlags cf)
{
  Compile_dst_op(cf, la_sub_w, &LoongArch64Recompiler::SafeSUBIW, la_sub_d, false, false);
}

void CPU::LoongArch64Recompiler::Compile_and(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // special cases - and with self -> self, and with 0 -> 0
  const la_gpr_t regd = CFGetRegD(cf);
  if (cf.MipsS() == cf.MipsT())
  {
    la_or(laAsm, regd, CFGetRegS(cf), LA_ZERO);
    return;
  }
  else if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0))
  {
    EmitMov(regd, 0);
    return;
  }

  Compile_dst_op(cf, la_and, &LoongArch64Recompiler::SafeANDI, la_and, true, false);
}

void CPU::LoongArch64Recompiler::Compile_or(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // or/nor with 0 -> no effect
  const la_gpr_t regd = CFGetRegD(cf);
  if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0) || cf.MipsS() == cf.MipsT())
  {
    cf.const_s ? MoveTToReg(regd, cf) : MoveSToReg(regd, cf);
    return;
  }

  Compile_dst_op(cf, la_or, &LoongArch64Recompiler::SafeORI, la_or, true, false);
}

void CPU::LoongArch64Recompiler::Compile_xor(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const la_gpr_t regd = CFGetRegD(cf);
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

  Compile_dst_op(cf, la_xor, &LoongArch64Recompiler::SafeXORI, la_xor, true, false);
}

void CPU::LoongArch64Recompiler::Compile_nor(CompileFlags cf)
{
  Compile_or(cf);
  la_nor(laAsm, CFGetRegD(cf), CFGetRegD(cf), LA_ZERO);
}

void CPU::LoongArch64Recompiler::Compile_slt(CompileFlags cf)
{
  Compile_slt(cf, true);
}

void CPU::LoongArch64Recompiler::Compile_sltu(CompileFlags cf)
{
  Compile_slt(cf, false);
}

void CPU::LoongArch64Recompiler::Compile_slt(CompileFlags cf, bool sign)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const la_gpr_t rd = CFGetRegD(cf);
  const la_gpr_t rs = CFGetSafeRegS(cf, RARG1);

  if (cf.const_t && laIsValidSImm12(GetConstantRegU32(cf.MipsT())))
  {
    if (sign)
      la_slti(laAsm, rd, rs, GetConstantRegS32(cf.MipsT()));
    else
      la_sltui(laAsm, rd, rs, GetConstantRegS32(cf.MipsT()));
  }
  else
  {
    const la_gpr_t rt = CFGetSafeRegT(cf, RARG2);
    if (sign)
      la_slt(laAsm, rd, rs, rt);
    else
      la_sltu(laAsm, rd, rs, rt);
  }
}

la_gpr_t CPU::LoongArch64Recompiler::ComputeLoadStoreAddressArg(CompileFlags cf,
                                                                const std::optional<VirtualMemoryAddress>& address,
                                                                const std::optional<la_gpr_t>& reg)
{
  const u32 imm = inst->i.imm_sext32();
  if (cf.valid_host_s && imm == 0 && !reg.has_value())
    return CFGetRegS(cf);

  const la_gpr_t dst = reg.has_value() ? reg.value() : RARG1;
  if (address.has_value())
  {
    EmitMov(dst, address.value());
  }
  else if (imm == 0)
  {
    if (cf.valid_host_s)
    {
      if (const la_gpr_t src = CFGetRegS(cf); src != dst)
        la_or(laAsm, dst, src, LA_ZERO);
    }
    else
    {
      la_ld_w(laAsm, dst, RSTATE, OFFS(&g_state.regs.r[cf.mips_s]));
    }
  }
  else
  {
    if (cf.valid_host_s)
    {
      SafeADDIW(dst, CFGetRegS(cf), inst->i.imm_sext32());
    }
    else
    {
      la_ld_w(laAsm, dst, RSTATE, OFFS(&g_state.regs.r[cf.mips_s]));
      SafeADDIW(dst, dst, inst->i.imm_sext32());
    }
  }

  return dst;
}

template<typename RegAllocFn>
la_gpr_t CPU::LoongArch64Recompiler::GenerateLoad(la_gpr_t addr_reg, MemoryAccessSize size, bool sign, bool use_fastmem,
                                                  const RegAllocFn& dst_reg_alloc)
{
  if (use_fastmem)
  {
    m_cycles += Bus::RAM_READ_TICKS;

    // TODO: Make this better. If we're loading the address from state, we can use LD_WU instead, and skip this.
    // TODO: LUT fastmem
    const la_gpr_t dst = dst_reg_alloc();
    // Zero-extend address to 64-bit
    la_bstrpick_d(laAsm, RSCRATCH, addr_reg, 31, 0);

    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg != RARG3);
      la_srli_d(laAsm, RARG3, RSCRATCH, Bus::FASTMEM_LUT_PAGE_SHIFT);
      la_slli_d(laAsm, RARG3, RARG3, 3);
      la_add_d(laAsm, RARG3, RARG3, RMEMBASE);
      la_ld_d(laAsm, RARG3, RARG3, 0);
      la_add_d(laAsm, RSCRATCH, RSCRATCH, RARG3);
    }
    else
    {
      la_add_d(laAsm, RSCRATCH, RSCRATCH, RMEMBASE);
    }

    u8* start = laAsm->cursor;
    switch (size)
    {
      case MemoryAccessSize::Byte:
        sign ? la_ld_b(laAsm, dst, RSCRATCH, 0) : la_ld_bu(laAsm, dst, RSCRATCH, 0);
        break;

      case MemoryAccessSize::HalfWord:
        sign ? la_ld_h(laAsm, dst, RSCRATCH, 0) : la_ld_hu(laAsm, dst, RSCRATCH, 0);
        break;

      case MemoryAccessSize::Word:
        la_ld_w(laAsm, dst, RSCRATCH, 0);
        break;
    }

    // We need a nop, because the slowmem jump might be more than 1MB away.
    la_andi(laAsm, LA_ZERO, LA_ZERO, 0); // NOP

    AddLoadStoreInfo(start, 8, addr_reg, dst, size, sign, true);
    return dst;
  }

  if (addr_reg != RARG1)
    la_or(laAsm, RARG1, addr_reg, LA_ZERO);

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
    la_srli_d(laAsm, RSCRATCH, RRET, 63);
    SwitchToFarCode(true, LaBranchCondition::NE, RSCRATCH, LA_ZERO);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const la_gpr_t temp = static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED));
    la_sub_d(laAsm, temp, LA_ZERO, RRET);
    la_slli_w(laAsm, temp, temp, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (-result << 2) | BD | cop_n
    SafeORI(RARG1, temp,
            Cop0Registers::CAUSE::MakeValueForException(
              static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n));
    EmitMov(RARG2, m_current_instruction_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    FreeHostReg(temp);
    EndBlock(std::nullopt, true);

    RestoreHostState();
    SwitchToNearCode(false);
  }

  const la_gpr_t dst_reg = dst_reg_alloc();
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      sign ? EmitSExtB(dst_reg, RRET) : EmitUExtB(dst_reg, RRET);
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      sign ? EmitSExtH(dst_reg, RRET) : EmitUExtH(dst_reg, RRET);
    }
    break;
    case MemoryAccessSize::Word:
    {
      // Need to undo the zero-extend.
      if (checked)
        laEmitDSExtW(laAsm, dst_reg, RRET);
      else if (dst_reg != RRET)
        la_or(laAsm, dst_reg, RRET, LA_ZERO);
    }
    break;
  }

  return dst_reg;
}

void CPU::LoongArch64Recompiler::GenerateStore(la_gpr_t addr_reg, la_gpr_t value_reg, MemoryAccessSize size,
                                               bool use_fastmem)
{
  if (use_fastmem)
  {
    DebugAssert(value_reg != RSCRATCH);
    // Zero-extend address to 64-bit
    la_bstrpick_d(laAsm, RSCRATCH, addr_reg, 31, 0);

    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg != RARG3);
      la_srli_d(laAsm, RARG3, RSCRATCH, Bus::FASTMEM_LUT_PAGE_SHIFT);
      la_slli_d(laAsm, RARG3, RARG3, 3);
      la_add_d(laAsm, RARG3, RARG3, RMEMBASE);
      la_ld_d(laAsm, RARG3, RARG3, 0);
      la_add_d(laAsm, RSCRATCH, RSCRATCH, RARG3);
    }
    else
    {
      la_add_d(laAsm, RSCRATCH, RSCRATCH, RMEMBASE);
    }

    u8* start = laAsm->cursor;
    switch (size)
    {
      case MemoryAccessSize::Byte:
        la_st_b(laAsm, value_reg, RSCRATCH, 0);
        break;

      case MemoryAccessSize::HalfWord:
        la_st_h(laAsm, value_reg, RSCRATCH, 0);
        break;

      case MemoryAccessSize::Word:
        la_st_w(laAsm, value_reg, RSCRATCH, 0);
        break;
    }

    // We need a nop, because the slowmem jump might be more than 1MB away.
    la_andi(laAsm, LA_ZERO, LA_ZERO, 0); // NOP

    AddLoadStoreInfo(start, 8, addr_reg, value_reg, size, false, false);
    return;
  }

  if (addr_reg != RARG1)
    la_or(laAsm, RARG1, addr_reg, LA_ZERO);
  if (value_reg != RARG2)
    la_or(laAsm, RARG2, value_reg, LA_ZERO);

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
    SwitchToFarCode(true, LaBranchCondition::NE, RRET, LA_ZERO);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const la_gpr_t temp = static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED));
    la_slli_w(laAsm, temp, RRET, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (result << 2) | BD | cop_n
    SafeORI(RARG1, temp,
            Cop0Registers::CAUSE::MakeValueForException(
              static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n));
    EmitMov(RARG2, m_current_instruction_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    FreeHostReg(temp);
    EndBlock(std::nullopt, true);

    RestoreHostState();
    SwitchToNearCode(false);
  }
}

void CPU::LoongArch64Recompiler::Compile_lxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                             const std::optional<VirtualMemoryAddress>& address)
{
  const std::optional<la_gpr_t> addr_reg =
    (g_settings.gpu_pgxp_enable && cf.MipsT() != Reg::zero) ?
      std::optional<la_gpr_t>(static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED))) :
      std::optional<la_gpr_t>();
  FlushForLoadStore(address, false, use_fastmem);
  const la_gpr_t addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const la_gpr_t data = GenerateLoad(addr, size, sign, use_fastmem, [this, cf]() {
    if (cf.MipsT() == Reg::zero)
      return RRET;

    return static_cast<la_gpr_t>(AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                                                 EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG,
                                                 cf.MipsT()));
  });

  if (g_settings.gpu_pgxp_enable && cf.MipsT() != Reg::zero)
  {
    Flush(FLUSH_FOR_C_CALL);

    EmitMov(RARG1, inst->bits);
    la_or(laAsm, RARG2, addr, LA_ZERO);
    la_or(laAsm, RARG3, data, LA_ZERO);
    EmitCall(s_pgxp_mem_load_functions[static_cast<u32>(size)][static_cast<u32>(sign)]);
    FreeHostReg(addr_reg.value());
  }
}

void CPU::LoongArch64Recompiler::Compile_lwx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                             const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  const la_gpr_t addr = static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED));
  FlushForLoadStore(address, false, use_fastmem);

  // TODO: if address is constant, this can be simplified..

  // If we're coming from another block, just flush the load delay and hope for the best..
  if (m_load_delay_dirty)
    UpdateLoadDelay();

  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);

  // Do PGXP first, it does its own load.
  if (g_settings.gpu_pgxp_enable && inst->r.rt != Reg::zero)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, inst->bits);
    la_or(laAsm, RARG2, addr, LA_ZERO);
    MoveMIPSRegToReg(RARG3, inst->r.rt, true);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LWx));
  }

  la_or(laAsm, RARG1, addr, LA_ZERO);
  la_bstrins_d(laAsm, RARG1, LA_ZERO, 1, 0); // addr & ~3
  GenerateLoad(RARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RRET; });

  if (inst->r.rt == Reg::zero)
  {
    FreeHostReg(addr);
    return;
  }

  // lwl/lwr from a load-delayed value takes the new value, but it itself, is load delayed, so the original value is
  // never written back. NOTE: can't trust T in cf because of the flush
  const Reg rt = inst->r.rt;
  la_gpr_t value;
  if (m_load_delay_register == rt)
  {
    const u32 existing_ld_rt = (m_load_delay_value_register == NUM_HOST_REGS) ?
                                 AllocateHostReg(HR_MODE_READ, HR_TYPE_LOAD_DELAY_VALUE, rt) :
                                 m_load_delay_value_register;
    RenameHostReg(existing_ld_rt, HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt);
    value = static_cast<la_gpr_t>(existing_ld_rt);
  }
  else
  {
    if constexpr (EMULATE_LOAD_DELAYS)
    {
      value = static_cast<la_gpr_t>(AllocateHostReg(HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt));
      if (const std::optional<u32> rtreg = CheckHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt); rtreg.has_value())
        la_or(laAsm, value, static_cast<la_gpr_t>(rtreg.value()), LA_ZERO);
      else if (HasConstantReg(rt))
        EmitMov(value, GetConstantRegU32(rt));
      else
        la_ld_w(laAsm, value, RSTATE, OFFS(&g_state.regs.r[static_cast<u8>(rt)]));
    }
    else
    {
      value = static_cast<la_gpr_t>(AllocateHostReg(HR_MODE_READ | HR_MODE_WRITE, HR_TYPE_CPU_REG, rt));
    }
  }

  DebugAssert(value != RARG2 && value != RARG3);
  la_andi(laAsm, RARG2, addr, 3);
  la_slli_w(laAsm, RARG2, RARG2, 3); // *8
  EmitMov(RARG3, 24);
  la_sub_w(laAsm, RARG3, RARG3, RARG2);

  if (inst->op == InstructionOp::lwl)
  {
    // const u32 mask = UINT32_C(0x00FFFFFF) >> shift;
    // new_value = (value & mask) | (RWRET << (24 - shift));
    EmitMov(RSCRATCH, 0xFFFFFFu);
    la_srl_w(laAsm, RSCRATCH, RSCRATCH, RARG2);
    la_and(laAsm, value, value, RSCRATCH);
    la_sll_w(laAsm, RRET, RRET, RARG3);
    la_or(laAsm, value, value, RRET);
  }
  else
  {
    // const u32 mask = UINT32_C(0xFFFFFF00) << (24 - shift);
    // new_value = (value & mask) | (RWRET >> shift);
    la_srl_w(laAsm, RRET, RRET, RARG2);
    EmitMov(RSCRATCH, 0xFFFFFF00u);
    la_sll_w(laAsm, RSCRATCH, RSCRATCH, RARG3);
    la_and(laAsm, value, value, RSCRATCH);
    la_or(laAsm, value, value, RRET);
  }

  FreeHostReg(addr);
}

void CPU::LoongArch64Recompiler::Compile_lwc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                              const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, true);
  const std::optional<la_gpr_t> addr_reg =
    g_settings.gpu_pgxp_enable ? std::optional<la_gpr_t>(static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                 std::optional<la_gpr_t>();
  FlushForLoadStore(address, false, use_fastmem);
  const la_gpr_t addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const la_gpr_t value = GenerateLoad(addr, MemoryAccessSize::Word, false, use_fastmem, [this, action = action]() {
    return (action == GTERegisterAccessAction::CallHandler && g_settings.gpu_pgxp_enable) ?
             static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED)) :
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
      la_st_w(laAsm, value, RSTATE, OFFS(ptr));
      break;
    }

    case GTERegisterAccessAction::SignExtend16:
    {
      EmitSExtH(RARG3, value);
      la_st_w(laAsm, RARG3, RSTATE, OFFS(ptr));
      break;
    }

    case GTERegisterAccessAction::ZeroExtend16:
    {
      EmitUExtH(RARG3, value);
      la_st_w(laAsm, RARG3, RSTATE, OFFS(ptr));
      break;
    }

    case GTERegisterAccessAction::CallHandler:
    {
      Flush(FLUSH_FOR_C_CALL);
      la_or(laAsm, RARG2, value, LA_ZERO);
      EmitMov(RARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::WriteRegister));
      break;
    }

    case GTERegisterAccessAction::PushFIFO:
    {
      // SXY0 <- SXY1
      // SXY1 <- SXY2
      // SXY2 <- SXYP
      DebugAssert(value != RARG2 && value != RARG3);
      la_ld_w(laAsm, RARG2, RSTATE, OFFS(&g_state.gte_regs.SXY1[0]));
      la_ld_w(laAsm, RARG3, RSTATE, OFFS(&g_state.gte_regs.SXY2[0]));
      la_st_w(laAsm, RARG2, RSTATE, OFFS(&g_state.gte_regs.SXY0[0]));
      la_st_w(laAsm, RARG3, RSTATE, OFFS(&g_state.gte_regs.SXY1[0]));
      la_st_w(laAsm, value, RSTATE, OFFS(&g_state.gte_regs.SXY2[0]));
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
    la_or(laAsm, RARG3, value, LA_ZERO);
    if (value != RRET)
      FreeHostReg(value);
    la_or(laAsm, RARG2, addr, LA_ZERO);
    FreeHostReg(addr_reg.value());
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LWC2));
  }
}

void CPU::LoongArch64Recompiler::Compile_sxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                             const std::optional<VirtualMemoryAddress>& address)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const std::optional<la_gpr_t> addr_reg =
    g_settings.gpu_pgxp_enable ? std::optional<la_gpr_t>(static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                 std::optional<la_gpr_t>();
  FlushForLoadStore(address, true, use_fastmem);
  const la_gpr_t addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const la_gpr_t data = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(RARG2, cf);

  GenerateStore(addr, data, size, use_fastmem);

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    MoveMIPSRegToReg(RARG3, cf.MipsT());
    la_or(laAsm, RARG2, addr, LA_ZERO);
    EmitMov(RARG1, inst->bits);
    EmitCall(s_pgxp_mem_store_functions[static_cast<u32>(size)]);
    FreeHostReg(addr_reg.value());
  }
}

void CPU::LoongArch64Recompiler::Compile_swx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                             const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  // TODO: this can take over rt's value if it's no longer needed
  // NOTE: can't trust T in cf because of the alloc
  const la_gpr_t addr = static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED));

  FlushForLoadStore(address, true, use_fastmem);

  // TODO: if address is constant, this can be simplified..
  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, inst->bits);
    la_or(laAsm, RARG2, addr, LA_ZERO);
    MoveMIPSRegToReg(RARG3, inst->r.rt);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SWx));
  }

  la_or(laAsm, RARG1, addr, LA_ZERO);
  la_bstrins_d(laAsm, RARG1, LA_ZERO, 1, 0); // addr & ~3
  GenerateLoad(RARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RRET; });

  la_andi(laAsm, RSCRATCH, addr, 3);
  la_slli_w(laAsm, RSCRATCH, RSCRATCH, 3);  // *8
  la_bstrins_d(laAsm, addr, LA_ZERO, 1, 0); // addr & ~3

  // Need to load down here for PGXP-off, because it's in a volatile reg that can get overwritten by flush.
  if (!g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(RARG2, inst->r.rt);

  if (inst->op == InstructionOp::swl)
  {
    // const u32 mem_mask = UINT32_C(0xFFFFFF00) << shift;
    // new_value = (RWRET & mem_mask) | (value >> (24 - shift));
    EmitMov(RARG3, 0xFFFFFF00u);
    la_sll_w(laAsm, RARG3, RARG3, RSCRATCH);
    la_and(laAsm, RRET, RRET, RARG3);

    EmitMov(RARG3, 24);
    la_sub_w(laAsm, RARG3, RARG3, RSCRATCH);
    la_srl_w(laAsm, RARG2, RARG2, RARG3);
    la_or(laAsm, RARG2, RARG2, RRET);
  }
  else
  {
    // const u32 mem_mask = UINT32_C(0x00FFFFFF) >> (24 - shift);
    // new_value = (RWRET & mem_mask) | (value << shift);
    la_sll_w(laAsm, RARG2, RARG2, RSCRATCH);

    EmitMov(RARG3, 24);
    la_sub_w(laAsm, RARG3, RARG3, RSCRATCH);
    EmitMov(RSCRATCH, 0x00FFFFFFu);
    la_srl_w(laAsm, RSCRATCH, RSCRATCH, RARG3);
    la_and(laAsm, RRET, RRET, RSCRATCH);
    la_or(laAsm, RARG2, RARG2, RRET);
  }

  GenerateStore(addr, RARG2, MemoryAccessSize::Word, use_fastmem);
  FreeHostReg(addr);
}

void CPU::LoongArch64Recompiler::Compile_swc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                              const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, false);
  const la_gpr_t addr = (g_settings.gpu_pgxp_enable || action == GTERegisterAccessAction::CallHandler) ?
                          static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED)) :
                          RARG1;
  const la_gpr_t data =
    g_settings.gpu_pgxp_enable ? static_cast<la_gpr_t>(AllocateTempHostReg(HR_CALLEE_SAVED)) : RARG2;
  FlushForLoadStore(address, true, use_fastmem);
  ComputeLoadStoreAddressArg(cf, address, addr);

  switch (action)
  {
    case GTERegisterAccessAction::Direct:
    {
      la_ld_w(laAsm, data, RSTATE, OFFS(ptr));
    }
    break;

    case GTERegisterAccessAction::CallHandler:
    {
      // should already be flushed.. except in fastmem case
      Flush(FLUSH_FOR_C_CALL);
      EmitMov(RARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));
      la_or(laAsm, data, RRET, LA_ZERO);
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
    if (addr != RARG1)
      FreeHostReg(addr);
  }
  else
  {
    // TODO: This can be simplified because we don't need to validate in PGXP..
    Flush(FLUSH_FOR_C_CALL);
    la_or(laAsm, RARG3, data, LA_ZERO);
    FreeHostReg(data);
    la_or(laAsm, RARG2, addr, LA_ZERO);
    FreeHostReg(addr);
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SWC2));
  }
}

void CPU::LoongArch64Recompiler::Compile_mtc0(CompileFlags cf)
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
  const la_gpr_t new_value = RARG1;
  const la_gpr_t old_value = RARG2;
  const la_gpr_t changed_bits = RARG3;
  const la_gpr_t mask_reg = RSCRATCH;

  // Load old value
  la_ld_w(laAsm, old_value, RSTATE, OFFS(ptr));

  // No way we fit this in an immediate..
  EmitMov(mask_reg, mask);

  // update value
  if (cf.valid_host_t)
    la_and(laAsm, new_value, CFGetRegT(cf), mask_reg);
  else
    EmitMov(new_value, GetConstantRegU32(cf.MipsT()) & mask);

  if (needs_bit_test)
    la_xor(laAsm, changed_bits, old_value, new_value);
  la_nor(laAsm, mask_reg, mask_reg, LA_ZERO);
  la_and(laAsm, old_value, old_value, mask_reg);
  la_or(laAsm, new_value, old_value, new_value);
  la_st_w(laAsm, new_value, RSTATE, OFFS(ptr));

  if (reg == Cop0Reg::SR)
  {
    // TODO: replace with register backup
    // We could just inline the whole thing..
    Flush(FLUSH_FOR_C_CALL);

    lagoon_label_t caches_unchanged = {};
    la_srli_w(laAsm, RSCRATCH, changed_bits, 16);
    la_andi(laAsm, RSCRATCH, RSCRATCH, 1);
    la_beq(laAsm, RSCRATCH, LA_ZERO, la_label(laAsm, &caches_unchanged));
    EmitCall(reinterpret_cast<const void*>(&CPU::UpdateMemoryPointers));
    la_ld_w(laAsm, new_value, RSTATE, OFFS(ptr));
    if (CodeCache::IsUsingFastmem())
      la_ld_d(laAsm, RMEMBASE, RSTATE, OFFS(&g_state.fastmem_base));
    la_bind(laAsm, &caches_unchanged);
    la_label_free(laAsm, &caches_unchanged);

    TestInterrupts(RARG1);
  }
  else if (reg == Cop0Reg::CAUSE)
  {
    la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.cop0_regs.sr.bits));
    TestInterrupts(RARG1);
  }
  else if (reg == Cop0Reg::DCIC || reg == Cop0Reg::BPCM)
  {
    // need to check whether we're switching to debug mode
    Flush(FLUSH_FOR_C_CALL);
    EmitCall(reinterpret_cast<const void*>(&CPU::UpdateDebugDispatcherFlag));
    SwitchToFarCode(true, LaBranchCondition::NE, RRET, LA_ZERO);
    BackupHostState();
    Flush(FLUSH_FOR_EARLY_BLOCK_EXIT);
    EmitCall(reinterpret_cast<const void*>(&CPU::ExitExecution)); // does not return
    RestoreHostState();
    SwitchToNearCode(false);
  }
}

void CPU::LoongArch64Recompiler::Compile_rfe(CompileFlags cf)
{
  // shift mode bits right two, preserving upper bits
  la_ld_w(laAsm, RARG1, RSTATE, OFFS(&g_state.cop0_regs.sr.bits));
  la_srli_w(laAsm, RSCRATCH, RARG1, 2);
  la_andi(laAsm, RSCRATCH, RSCRATCH, 0xf);
  la_bstrins_d(laAsm, RARG1, LA_ZERO, 3, 0);
  la_or(laAsm, RARG1, RARG1, RSCRATCH);
  la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.cop0_regs.sr.bits));

  TestInterrupts(RARG1);
}

void CPU::LoongArch64Recompiler::TestInterrupts(la_gpr_t sr)
{
  DebugAssert(sr != RSCRATCH);

  // if Iec == 0 then goto no_interrupt
  lagoon_label_t no_interrupt = {};
  la_andi(laAsm, RSCRATCH, sr, 1);
  la_beqz(laAsm, RSCRATCH, la_label(laAsm, &no_interrupt));

  // sr & cause
  la_ld_w(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.cop0_regs.cause.bits));
  la_and(laAsm, sr, sr, RSCRATCH);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  la_srli_w(laAsm, sr, sr, 8);
  la_andi(laAsm, sr, sr, 0xFF);
  SwitchToFarCode(true, LaBranchCondition::NE, sr, LA_ZERO);

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
    if (m_dirty_pc)
      EmitMov(RARG1, m_compiler_pc);
    la_st_w(laAsm, LA_ZERO, RSTATE, OFFS(&g_state.downcount));
    if (m_dirty_pc)
      la_st_w(laAsm, RARG1, RSTATE, OFFS(&g_state.pc));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, false, true);
  }

  RestoreHostState();
  SwitchToNearCode(false);

  la_bind(laAsm, &no_interrupt);
  la_label_free(laAsm, &no_interrupt);
}

void CPU::LoongArch64Recompiler::Compile_mfc2(CompileFlags cf)
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
    la_ld_w(laAsm, static_cast<la_gpr_t>(hreg), RSTATE, OFFS(ptr));
  }
  else if (action == GTERegisterAccessAction::CallHandler)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, index);
    EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));

    hreg = AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                           EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, rt);
    la_or(laAsm, static_cast<la_gpr_t>(hreg), RRET, LA_ZERO);
  }
  else
  {
    Panic("Unknown action");
  }

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, inst->bits);
    la_or(laAsm, RARG2, static_cast<la_gpr_t>(hreg), LA_ZERO);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_MFC2));
  }
}

void CPU::LoongArch64Recompiler::Compile_mtc2(CompileFlags cf)
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
      la_st_w(laAsm, CFGetRegT(cf), RSTATE, OFFS(ptr));
  }
  else if (action == GTERegisterAccessAction::SignExtend16 || action == GTERegisterAccessAction::ZeroExtend16)
  {
    const bool sign = (action == GTERegisterAccessAction::SignExtend16);
    if (cf.valid_host_t)
    {
      sign ? EmitSExtH(RARG1, CFGetRegT(cf)) : EmitUExtH(RARG1, CFGetRegT(cf));
      la_st_w(laAsm, RARG1, RSTATE, OFFS(ptr));
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
    DebugAssert(RRET != RARG2 && RRET != RARG3);
    la_ld_w(laAsm, RARG2, RSTATE, OFFS(&g_state.gte_regs.SXY1[0]));
    la_ld_w(laAsm, RARG3, RSTATE, OFFS(&g_state.gte_regs.SXY2[0]));
    la_st_w(laAsm, RARG2, RSTATE, OFFS(&g_state.gte_regs.SXY0[0]));
    la_st_w(laAsm, RARG3, RSTATE, OFFS(&g_state.gte_regs.SXY1[0]));
    if (cf.valid_host_t)
      la_st_w(laAsm, CFGetRegT(cf), RSTATE, OFFS(&g_state.gte_regs.SXY2[0]));
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

void CPU::LoongArch64Recompiler::Compile_cop2(CompileFlags cf)
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
  lagoon_assembler_t la_asm;
  lagoon_assembler_t* laAsm = &la_asm;
  la_init_assembler(laAsm, static_cast<u8*>(thunk_code), thunk_space);

  static constexpr u32 GPR_SIZE = 8;

  // save regs
  u32 num_gprs = 0;

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    if ((gpr_bitmask & (1u << i)) && laIsCallerSavedRegister(i) && (!is_load || data_register != i))
      num_gprs++;
  }

  const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE);

  if (stack_size > 0)
  {
    la_addi_d(laAsm, LA_SP, LA_SP, -static_cast<s32>(stack_size));

    u32 stack_offset = 0;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && laIsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        la_st_d(laAsm, static_cast<la_gpr_t>(i), LA_SP, stack_offset);
        stack_offset += GPR_SIZE;
      }
    }
  }

  if (cycles_to_add != 0)
  {
    // NOTE: we have to reload here, because memory writes can run DMA, which can screw with cycles
    Assert(laIsValidSImm12(cycles_to_add));
    la_ld_w(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.pending_ticks));
    la_addi_w(laAsm, RSCRATCH, RSCRATCH, cycles_to_add);
    la_st_w(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.pending_ticks));
  }

  if (address_register != RARG1)
    la_or(laAsm, RARG1, static_cast<la_gpr_t>(address_register), LA_ZERO);

  if (!is_load)
  {
    if (data_register != RARG2)
      la_or(laAsm, RARG2, static_cast<la_gpr_t>(data_register), LA_ZERO);
  }

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      laEmitCall(laAsm, is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryByte) :
                                  reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      laEmitCall(laAsm, is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryHalfWord) :
                                  reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      laEmitCall(laAsm, is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryWord) :
                                  reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryWord));
    }
    break;
  }

  if (is_load)
  {
    const la_gpr_t dst = static_cast<la_gpr_t>(data_register);
    switch (size)
    {
      case MemoryAccessSize::Byte:
      {
        is_signed ? laEmitSExtB(laAsm, dst, RRET) : laEmitUExtB(laAsm, dst, RRET);
      }
      break;
      case MemoryAccessSize::HalfWord:
      {
        is_signed ? laEmitSExtH(laAsm, dst, RRET) : laEmitUExtH(laAsm, dst, RRET);
      }
      break;
      case MemoryAccessSize::Word:
      {
        if (dst != RRET)
          la_or(laAsm, dst, RRET, LA_ZERO);
      }
      break;
    }
  }

  if (cycles_to_remove != 0)
  {
    Assert(laIsValidSImm12(-cycles_to_remove));
    la_ld_w(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.pending_ticks));
    la_addi_w(laAsm, RSCRATCH, RSCRATCH, -cycles_to_remove);
    la_st_w(laAsm, RSCRATCH, RSTATE, OFFS(&g_state.pending_ticks));
  }

  // restore regs
  if (stack_size > 0)
  {
    u32 stack_offset = 0;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && laIsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        la_ld_d(laAsm, static_cast<la_gpr_t>(i), LA_SP, stack_offset);
        stack_offset += GPR_SIZE;
      }
    }

    la_addi_d(laAsm, LA_SP, LA_SP, stack_size);
  }

  laEmitJmp(laAsm, static_cast<const u8*>(code_address) + code_size);

  return static_cast<u32>(laAsm->cursor - laAsm->buffer);
}

#endif // CPU_ARCH_LOONGARCH64