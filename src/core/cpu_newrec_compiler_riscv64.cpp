// SPDX-FileCopyrightText: 2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cpu_newrec_compiler_riscv64.h"
#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "cpu_code_cache_private.h"
#include "cpu_core_private.h"
#include "cpu_pgxp.h"
#include "cpu_recompiler_thunks.h"
#include "gte.h"
#include "settings.h"
#include "timing_event.h"
#include <limits>

#ifdef CPU_ARCH_RISCV64

Log_SetChannel(CPU::NewRec);

#ifdef ENABLE_HOST_DISASSEMBLY
extern "C" {
#include "riscv-disas.h"
}
#endif

// For LW/SW/etc.
#define PTR(x) ((u32)(((u8*)(x)) - ((u8*)&g_state))), RSTATE

static constexpr u32 BLOCK_LINK_SIZE = 8; // auipc+jr

namespace CPU::NewRec {

using namespace biscuit;

using CPU::Recompiler::rvEmitCall;
using CPU::Recompiler::rvEmitDSExtW;
using CPU::Recompiler::rvEmitDUExtW;
using CPU::Recompiler::rvEmitJmp;
using CPU::Recompiler::rvEmitMov;
using CPU::Recompiler::rvEmitMov64;
using CPU::Recompiler::rvEmitSExtB;
using CPU::Recompiler::rvEmitSExtH;
using CPU::Recompiler::rvEmitUExtB;
using CPU::Recompiler::rvEmitUExtH;
using CPU::Recompiler::rvGetAddressImmediates;
using CPU::Recompiler::rvIsCallerSavedRegister;
using CPU::Recompiler::rvIsValidSExtITypeImm;
using CPU::Recompiler::rvMoveAddressToReg;

RISCV64Compiler s_instance;
Compiler* g_compiler = &s_instance;

} // namespace CPU::NewRec

bool CPU::Recompiler::rvIsCallerSavedRegister(u32 id)
{
  return (id == 1 || (id >= 3 && id < 8) || (id >= 10 && id <= 17) || (id >= 28 && id <= 31));
}

bool CPU::Recompiler::rvIsValidSExtITypeImm(u32 imm)
{
  return (static_cast<u32>((static_cast<s32>(imm) << 20) >> 20) == imm);
}

std::pair<s32, s32> CPU::Recompiler::rvGetAddressImmediates(const void* cur, const void* target)
{
  const s64 disp = static_cast<s64>(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cur));
  Assert(disp >= static_cast<s64>(std::numeric_limits<s32>::min()) &&
         disp <= static_cast<s64>(std::numeric_limits<s32>::max()));

  const s64 hi = disp + 0x800;
  const s64 lo = disp - (hi & 0xFFFFF000);
  return std::make_pair(static_cast<s32>(hi >> 12), static_cast<s32>((lo << 52) >> 52));
}

void CPU::Recompiler::rvMoveAddressToReg(biscuit::Assembler* rvAsm, const biscuit::GPR& reg, const void* addr)
{
  const auto [hi, lo] = rvGetAddressImmediates(rvAsm->GetCursorPointer(), addr);
  rvAsm->AUIPC(reg, hi);
  rvAsm->ADDI(reg, reg, lo);
}

void CPU::Recompiler::rvEmitMov(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, u32 imm)
{
  // Borrowed from biscuit, but doesn't emit an ADDI if the lower 12 bits are zero.
  const u32 lower = imm & 0xFFF;
  const u32 upper = (imm & 0xFFFFF000) >> 12;
  const s32 simm = static_cast<s32>(imm);
  if (rvIsValidSExtITypeImm(simm))
  {
    rvAsm->ADDI(rd, biscuit::zero, static_cast<s32>(lower));
  }
  else
  {
    const bool needs_increment = (lower & 0x800) != 0;
    const u32 upper_imm = needs_increment ? upper + 1 : upper;
    rvAsm->LUI(rd, upper_imm);
    rvAsm->ADDI(rd, rd, static_cast<int32_t>(lower));
  }
}

void CPU::Recompiler::rvEmitMov64(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, const biscuit::GPR& scratch,
                                  u64 imm)
{
  // TODO: Make better..
  rvEmitMov(rvAsm, rd, static_cast<u32>(imm >> 32));
  rvEmitMov(rvAsm, scratch, static_cast<u32>(imm));
  rvAsm->SLLI64(rd, rd, 32);
  rvAsm->SLLI64(scratch, scratch, 32);
  rvAsm->SRLI64(scratch, scratch, 32);
  rvAsm->ADD(rd, rd, scratch);
}

u32 CPU::Recompiler::rvEmitJmp(biscuit::Assembler* rvAsm, const void* ptr, const biscuit::GPR& link_reg)
{
  // TODO: use J if displacement is <1MB,  needs a bool because backpatch must be 8 bytes
  const auto [hi, lo] = rvGetAddressImmediates(rvAsm->GetCursorPointer(), ptr);
  rvAsm->AUIPC(RSCRATCH, hi);
  rvAsm->JALR(link_reg, lo, RSCRATCH);
  return 8;
}

u32 CPU::Recompiler::rvEmitCall(biscuit::Assembler* rvAsm, const void* ptr)
{
  return rvEmitJmp(rvAsm, ptr, biscuit::ra);
}

void CPU::Recompiler::rvEmitSExtB(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvAsm->SLLI(rd, rs, 24);
  rvAsm->SRAIW(rd, rd, 24);
}

void CPU::Recompiler::rvEmitUExtB(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvAsm->ANDI(rd, rs, 0xFF);
}

void CPU::Recompiler::rvEmitSExtH(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvAsm->SLLI(rd, rs, 16);
  rvAsm->SRAIW(rd, rd, 16);
}

void CPU::Recompiler::rvEmitUExtH(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvAsm->SLLI(rd, rs, 16);
  rvAsm->SRLI(rd, rd, 16);
}

void CPU::Recompiler::rvEmitDSExtW(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvAsm->ADDIW(rd, rs, 0);
}

void CPU::Recompiler::rvEmitDUExtW(biscuit::Assembler* rvAsm, const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvAsm->SLLI64(rd, rs, 32);
  rvAsm->SRLI64(rd, rd, 32);
}

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  const u8* cur = static_cast<const u8*>(start);
  const u8* end = cur + size;
  char buf[256];
  while (cur < end)
  {
    rv_inst inst;
    size_t instlen;
    inst_fetch(cur, &inst, &instlen);
    disasm_inst(buf, std::size(buf), rv64, static_cast<u64>(reinterpret_cast<uintptr_t>(cur)), inst);
    Log_DebugPrintf("\t0x%016" PRIx64 "\t%s", static_cast<u64>(reinterpret_cast<uintptr_t>(cur)), buf);
    cur += instlen;
  }
#else
  Log_ErrorPrint("Not compiled with ENABLE_HOST_DISASSEMBLY.");
#endif
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
#ifdef ENABLE_HOST_DISASSEMBLY
  const u8* cur = static_cast<const u8*>(start);
  const u8* end = cur + size;
  u32 icount = 0;
  while (cur < end)
  {
    rv_inst inst;
    size_t instlen;
    inst_fetch(cur, &inst, &instlen);
    cur += instlen;
    icount++;
  }
  return icount;
#else
  Log_ErrorPrint("Not compiled with ENABLE_HOST_DISASSEMBLY.");
  return 0;
#endif
}

u32 CPU::CodeCache::EmitASMFunctions(void* code, u32 code_size)
{
  using namespace CPU::Recompiler;
  using namespace biscuit;

  Assembler actual_asm(static_cast<u8*>(code), code_size);
  Assembler* rvAsm = &actual_asm;

  Label dispatch;

  g_enter_recompiler = reinterpret_cast<decltype(g_enter_recompiler)>(rvAsm->GetCursorPointer());
  {
    // TODO: reserve some space for saving caller-saved registers

    // Need the CPU state for basically everything :-)
    rvMoveAddressToReg(rvAsm, RSTATE, &g_state);

    // Fastmem setup
    if (IsUsingFastmem())
      rvAsm->LD(RMEMBASE, PTR(&g_state.fastmem_base));

    // Downcount isn't set on entry, so we need to initialize it
    rvMoveAddressToReg(rvAsm, RARG1, TimingEvents::GetHeadEventPtr());
    rvAsm->LD(RARG1, 0, RARG1);
    rvAsm->LW(RARG1, offsetof(TimingEvent, m_downcount), RARG1);
    rvAsm->SW(RARG1, PTR(&g_state.downcount));

    // Fall through to event dispatcher
  }

  // check events then for frame done
  g_check_events_and_dispatch = rvAsm->GetCursorPointer();
  {
    Label skip_event_check;
    rvAsm->LW(RARG1, PTR(&g_state.pending_ticks));
    rvAsm->LW(RARG2, PTR(&g_state.downcount));
    rvAsm->BLTU(RARG1, RARG2, &skip_event_check);

    g_run_events_and_dispatch = rvAsm->GetCursorPointer();
    rvEmitCall(rvAsm, reinterpret_cast<const void*>(&TimingEvents::RunEvents));

    rvAsm->Bind(&skip_event_check);
  }

  // TODO: align?
  g_dispatcher = rvAsm->GetCursorPointer();
  {
    rvAsm->Bind(&dispatch);

    // x9 <- s_fast_map[pc >> 16]
    rvAsm->LWU(RARG1, PTR(&g_state.pc));
    rvMoveAddressToReg(rvAsm, RARG3, g_code_lut.data());
    rvAsm->SRLI(RARG2, RARG1, 16);
    rvAsm->SLLI(RARG1, RARG1, 1);
    rvAsm->SLLI(RARG2, RARG2, 3);
    rvAsm->ADD(RARG2, RARG2, RARG3);
    rvAsm->LD(RARG2, 0, RARG2);

    // blr(x9[pc * 2]) (fast_map[pc >> 2])
    rvAsm->ADD(RARG1, RARG1, RARG2);
    rvAsm->LD(RARG1, 0, RARG1);
    rvAsm->JR(RARG1);
  }

  g_compile_or_revalidate_block = rvAsm->GetCursorPointer();
  {
    rvAsm->LW(RARG1, PTR(&g_state.pc));
    rvEmitCall(rvAsm, reinterpret_cast<const void*>(&CompileOrRevalidateBlock));
    rvAsm->J(&dispatch);
  }

  g_discard_and_recompile_block = rvAsm->GetCursorPointer();
  {
    rvAsm->LW(RARG1, PTR(&g_state.pc));
    rvEmitCall(rvAsm, reinterpret_cast<const void*>(&DiscardAndRecompileBlock));
    rvAsm->J(&dispatch);
  }

  g_interpret_block = rvAsm->GetCursorPointer();
  {
    rvEmitCall(rvAsm, CodeCache::GetInterpretUncachedBlockFunction());
    rvAsm->J(&dispatch);
  }

  // TODO: align?

  return static_cast<u32>(rvAsm->GetCodeBuffer().GetSizeInBytes());
}

u32 CPU::CodeCache::EmitJump(void* code, const void* dst, bool flush_icache)
{
  // TODO: get rid of assembler construction here
  {
    biscuit::Assembler assembler(static_cast<u8*>(code), BLOCK_LINK_SIZE);
    CPU::Recompiler::rvEmitCall(&assembler, dst);

    DebugAssert(assembler.GetCodeBuffer().GetSizeInBytes() <= BLOCK_LINK_SIZE);
    if (assembler.GetCodeBuffer().GetRemainingBytes() > 0)
      assembler.NOP();
  }

  if (flush_icache)
    JitCodeBuffer::FlushInstructionCache(code, BLOCK_LINK_SIZE);

  return BLOCK_LINK_SIZE;
}

CPU::NewRec::RISCV64Compiler::RISCV64Compiler() = default;

CPU::NewRec::RISCV64Compiler::~RISCV64Compiler() = default;

const void* CPU::NewRec::RISCV64Compiler::GetCurrentCodePointer()
{
  return rvAsm->GetCursorPointer();
}

void CPU::NewRec::RISCV64Compiler::Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space,
                                         u8* far_code_buffer, u32 far_code_space)
{
  Compiler::Reset(block, code_buffer, code_buffer_space, far_code_buffer, far_code_space);

  // TODO: don't recreate this every time..
  DebugAssert(!m_emitter && !m_far_emitter && !rvAsm);
  m_emitter = std::make_unique<Assembler>(code_buffer, code_buffer_space);
  m_far_emitter = std::make_unique<Assembler>(far_code_buffer, far_code_space);
  rvAsm = m_emitter.get();

  // Need to wipe it out so it's correct when toggling fastmem.
  m_host_regs = {};

  const u32 membase_idx = CodeCache::IsUsingFastmem() ? RMEMBASE.Index() : NUM_HOST_REGS;
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& hra = m_host_regs[i];

    if (i == RARG1.Index() || i == RARG2.Index() || i == RARG3.Index() || i == RSCRATCH.Index() ||
        i == RSTATE.Index() || i == membase_idx || i < 5 /* zero, ra, sp, gp, tp */)
    {
      continue;
    }

    hra.flags = HR_USABLE | (rvIsCallerSavedRegister(i) ? 0 : HR_CALLEE_SAVED);
  }
}

void CPU::NewRec::RISCV64Compiler::SwitchToFarCode(
  bool emit_jump,
  void (biscuit::Assembler::*inverted_cond)(biscuit::GPR, biscuit::GPR, biscuit::Label*) /* = nullptr */,
  const biscuit::GPR& rs1 /* = biscuit::zero */, const biscuit::GPR& rs2 /* = biscuit::zero */)
{
  DebugAssert(rvAsm == m_emitter.get());
  if (emit_jump)
  {
    const void* target = m_far_emitter->GetCursorPointer();
    if (inverted_cond)
    {
      Label skip;
      (rvAsm->*inverted_cond)(rs1, rs2, &skip);
      rvEmitJmp(rvAsm, target);
      rvAsm->Bind(&skip);
    }
    else
    {
      rvEmitCall(rvAsm, target);
    }
  }
  rvAsm = m_far_emitter.get();
}

void CPU::NewRec::RISCV64Compiler::SwitchToNearCode(bool emit_jump)
{
  DebugAssert(rvAsm == m_far_emitter.get());
  if (emit_jump)
    rvEmitJmp(rvAsm, m_emitter->GetCursorPointer());
  rvAsm = m_emitter.get();
}

void CPU::NewRec::RISCV64Compiler::EmitMov(const biscuit::GPR& dst, u32 val)
{
  rvEmitMov(rvAsm, dst, val);
}

void CPU::NewRec::RISCV64Compiler::EmitCall(const void* ptr)
{
  rvEmitCall(rvAsm, ptr);
}

void CPU::NewRec::RISCV64Compiler::SafeImmSExtIType(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm,
                                                    void (biscuit::Assembler::*iop)(GPR, GPR, u32),
                                                    void (biscuit::Assembler::*rop)(GPR, GPR, GPR))
{
  DebugAssert(rd != RSCRATCH && rs != RSCRATCH);

  if (rvIsValidSExtITypeImm(imm))
  {
    (rvAsm->*iop)(rd, rs, imm);
    return;
  }

  rvEmitMov(rvAsm, RSCRATCH, imm);
  (rvAsm->*rop)(rd, rs, RSCRATCH);
}

void CPU::NewRec::RISCV64Compiler::SafeADDI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  SafeImmSExtIType(rd, rs, imm, reinterpret_cast<void (biscuit::Assembler::*)(GPR, GPR, u32)>(&Assembler::ADDI),
                   &Assembler::ADD);
}

void CPU::NewRec::RISCV64Compiler::SafeADDIW(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  SafeImmSExtIType(rd, rs, imm, reinterpret_cast<void (biscuit::Assembler::*)(GPR, GPR, u32)>(&Assembler::ADDIW),
                   &Assembler::ADDW);
}

void CPU::NewRec::RISCV64Compiler::SafeSUBIW(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  const u32 nimm = static_cast<u32>(-static_cast<s32>(imm));
  SafeImmSExtIType(rd, rs, nimm, reinterpret_cast<void (biscuit::Assembler::*)(GPR, GPR, u32)>(&Assembler::ADDIW),
                   &Assembler::ADDW);
}

void CPU::NewRec::RISCV64Compiler::SafeANDI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  SafeImmSExtIType(rd, rs, imm, &Assembler::ANDI, &Assembler::AND);
}

void CPU::NewRec::RISCV64Compiler::SafeORI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  SafeImmSExtIType(rd, rs, imm, &Assembler::ORI, &Assembler::OR);
}

void CPU::NewRec::RISCV64Compiler::SafeXORI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  SafeImmSExtIType(rd, rs, imm, &Assembler::XORI, &Assembler::XOR);
}

void CPU::NewRec::RISCV64Compiler::SafeSLTI(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  SafeImmSExtIType(rd, rs, imm, reinterpret_cast<void (biscuit::Assembler::*)(GPR, GPR, u32)>(&Assembler::SLTI),
                   &Assembler::SLT);
}

void CPU::NewRec::RISCV64Compiler::SafeSLTIU(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm)
{
  SafeImmSExtIType(rd, rs, imm, reinterpret_cast<void (biscuit::Assembler::*)(GPR, GPR, u32)>(&Assembler::SLTIU),
                   &Assembler::SLTU);
}

void CPU::NewRec::RISCV64Compiler::EmitSExtB(const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvEmitSExtB(rvAsm, rd, rs);
}

void CPU::NewRec::RISCV64Compiler::EmitUExtB(const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvEmitUExtB(rvAsm, rd, rs);
}

void CPU::NewRec::RISCV64Compiler::EmitSExtH(const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvEmitSExtH(rvAsm, rd, rs);
}

void CPU::NewRec::RISCV64Compiler::EmitUExtH(const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvEmitUExtH(rvAsm, rd, rs);
}

void CPU::NewRec::RISCV64Compiler::EmitDSExtW(const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvEmitDSExtW(rvAsm, rd, rs);
}

void CPU::NewRec::RISCV64Compiler::EmitDUExtW(const biscuit::GPR& rd, const biscuit::GPR& rs)
{
  rvEmitDUExtW(rvAsm, rd, rs);
}

void CPU::NewRec::RISCV64Compiler::GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size)
{
  // store it first to reduce code size, because we can offset
  // TODO: 64-bit displacement is needed :/
  // rvMoveAddressToReg(rvAsm, RARG1, ram_ptr);
  // rvMoveAddressToReg(rvAsm, RARG2, shadow_ptr);
  rvEmitMov64(rvAsm, RARG1, RSCRATCH, static_cast<u64>(reinterpret_cast<uintptr_t>(ram_ptr)));
  rvEmitMov64(rvAsm, RARG2, RSCRATCH, static_cast<u64>(reinterpret_cast<uintptr_t>(shadow_ptr)));

  u32 offset = 0;
  Label block_changed;

  while (size >= 8)
  {
    rvAsm->LD(RARG3, offset, RARG1);
    rvAsm->LD(RSCRATCH, offset, RARG2);
    rvAsm->BNE(RARG3, RSCRATCH, &block_changed);
    offset += 8;
    size -= 8;
  }

  while (size >= 4)
  {
    rvAsm->LWU(RARG3, offset, RARG1);
    rvAsm->LWU(RSCRATCH, offset, RARG2);
    rvAsm->BNE(RARG3, RSCRATCH, &block_changed);
    offset += 4;
    size -= 4;
  }

  DebugAssert(size == 0);

  Label block_unchanged;
  rvAsm->J(&block_unchanged);
  rvAsm->Bind(&block_changed);
  rvEmitJmp(rvAsm, CodeCache::g_discard_and_recompile_block);
  rvAsm->Bind(&block_unchanged);
}

void CPU::NewRec::RISCV64Compiler::GenerateICacheCheckAndUpdate()
{
  if (GetSegmentForAddress(m_block->pc) >= Segment::KSEG1)
  {
    rvAsm->LW(RARG1, PTR(&g_state.pending_ticks));
    SafeADDIW(RARG1, RARG1, static_cast<u32>(m_block->uncached_fetch_ticks));
    rvAsm->SW(RARG1, PTR(&g_state.pending_ticks));
  }
  else
  {
    const auto& ticks_reg = RARG1;
    const auto& current_tag_reg = RARG2;
    const auto& existing_tag_reg = RARG3;

    VirtualMemoryAddress current_pc = m_block->pc & ICACHE_TAG_ADDRESS_MASK;
    rvAsm->LW(ticks_reg, PTR(&g_state.pending_ticks));
    rvEmitMov(rvAsm, current_tag_reg, current_pc);

    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const TickCount fill_ticks = GetICacheFillTicks(current_pc);
      if (fill_ticks <= 0)
        continue;

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = offsetof(State, icache_tags) + (line * sizeof(u32));

      // TODO: Verify sign extension here...
      Label cache_hit;
      rvAsm->LW(existing_tag_reg, offset, RSTATE);
      rvAsm->BEQ(existing_tag_reg, current_tag_reg, &cache_hit);

      rvAsm->SW(current_tag_reg, offset, RSTATE);
      SafeADDIW(ticks_reg, ticks_reg, static_cast<u32>(fill_ticks));
      rvAsm->Bind(&cache_hit);

      if (i != (m_block->icache_line_count - 1))
        SafeADDIW(current_tag_reg, current_tag_reg, ICACHE_LINE_SIZE);
    }

    rvAsm->SW(ticks_reg, PTR(&g_state.pending_ticks));
  }
}

void CPU::NewRec::RISCV64Compiler::GenerateCall(const void* func, s32 arg1reg /*= -1*/, s32 arg2reg /*= -1*/,
                                                s32 arg3reg /*= -1*/)
{
  if (arg1reg >= 0 && arg1reg != static_cast<s32>(RARG1.Index()))
    rvAsm->MV(RARG1, GPR(arg1reg));
  if (arg1reg >= 0 && arg2reg != static_cast<s32>(RARG2.Index()))
    rvAsm->MV(RARG2, GPR(arg2reg));
  if (arg1reg >= 0 && arg3reg != static_cast<s32>(RARG3.Index()))
    rvAsm->MV(RARG3, GPR(arg3reg));
  EmitCall(func);
}

void CPU::NewRec::RISCV64Compiler::EndBlock(const std::optional<u32>& newpc, bool do_event_test)
{
  if (newpc.has_value())
  {
    if (m_dirty_pc || m_compiler_pc != newpc)
    {
      EmitMov(RSCRATCH, newpc.value());
      rvAsm->SW(RSCRATCH, PTR(&g_state.pc));
    }
  }
  m_dirty_pc = false;

  // flush regs
  Flush(FLUSH_END_BLOCK);
  EndAndLinkBlock(newpc, do_event_test, false);
}

void CPU::NewRec::RISCV64Compiler::EndBlockWithException(Exception excode)
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

void CPU::NewRec::RISCV64Compiler::EndAndLinkBlock(const std::optional<u32>& newpc, bool do_event_test,
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
    rvAsm->LW(RARG1, PTR(&g_state.pending_ticks));
  if (do_event_test)
    rvAsm->LW(RARG2, PTR(&g_state.downcount));
  if (cycles > 0)
  {
    SafeADDIW(RARG1, RARG1, cycles);
    rvAsm->SW(RARG1, PTR(&g_state.pending_ticks));
  }
  if (m_gte_done_cycle > cycles)
  {
    SafeADDIW(RARG2, RARG1, m_gte_done_cycle - cycles);
    rvAsm->SW(RARG1, PTR(&g_state.gte_completion_tick));
  }

  if (do_event_test)
  {
    // TODO: see if we can do a far jump somehow with this..
    Label cont;
    rvAsm->BLT(RARG1, RARG2, &cont);
    rvEmitJmp(rvAsm, CodeCache::g_run_events_and_dispatch);
    rvAsm->Bind(&cont);
  }

  // jump to dispatcher or next block
  if (force_run_events)
  {
    rvEmitJmp(rvAsm, CodeCache::g_run_events_and_dispatch);
  }
  else if (!newpc.has_value())
  {
    rvEmitJmp(rvAsm, CodeCache::g_dispatcher);
  }
  else
  {
    if (newpc.value() == m_block->pc)
    {
      // Special case: ourselves! No need to backlink then.
      Log_DebugPrintf("Linking block at %08X to self", m_block->pc);
      rvEmitJmp(rvAsm, rvAsm->GetBufferPointer(0));
    }
    else
    {
      const void* target = CreateBlockLink(m_block, rvAsm->GetCursorPointer(), newpc.value());
      rvEmitJmp(rvAsm, target);
    }
  }
}

const void* CPU::NewRec::RISCV64Compiler::EndCompile(u32* code_size, u32* far_code_size)
{
  u8* const code = m_emitter->GetBufferPointer(0);
  *code_size = static_cast<u32>(m_emitter->GetCodeBuffer().GetSizeInBytes());
  *far_code_size = static_cast<u32>(m_far_emitter->GetCodeBuffer().GetSizeInBytes());
  rvAsm = nullptr;
  m_far_emitter.reset();
  m_emitter.reset();
  return code;
}

const char* CPU::NewRec::RISCV64Compiler::GetHostRegName(u32 reg) const
{
  static constexpr std::array<const char*, 32> reg64_names = {
    {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
     "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"}};
  return (reg < reg64_names.size()) ? reg64_names[reg] : "UNKNOWN";
}

void CPU::NewRec::RISCV64Compiler::LoadHostRegWithConstant(u32 reg, u32 val)
{
  EmitMov(GPR(reg), val);
}

void CPU::NewRec::RISCV64Compiler::LoadHostRegFromCPUPointer(u32 reg, const void* ptr)
{
  rvAsm->LW(GPR(reg), PTR(ptr));
}

void CPU::NewRec::RISCV64Compiler::StoreHostRegToCPUPointer(u32 reg, const void* ptr)
{
  rvAsm->SW(GPR(reg), PTR(ptr));
}

void CPU::NewRec::RISCV64Compiler::StoreConstantToCPUPointer(u32 val, const void* ptr)
{
  if (val == 0)
  {
    rvAsm->SW(zero, PTR(ptr));
    return;
  }

  EmitMov(RSCRATCH, val);
  rvAsm->SW(RSCRATCH, PTR(ptr));
}

void CPU::NewRec::RISCV64Compiler::CopyHostReg(u32 dst, u32 src)
{
  if (src != dst)
    rvAsm->MV(GPR(dst), GPR(src));
}

void CPU::NewRec::RISCV64Compiler::AssertRegOrConstS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s || cf.const_s);
}

void CPU::NewRec::RISCV64Compiler::AssertRegOrConstT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t || cf.const_t);
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::CFGetSafeRegS(CompileFlags cf, const biscuit::GPR& temp_reg)
{
  if (cf.valid_host_s)
  {
    return GPR(cf.host_s);
  }
  else if (cf.const_s)
  {
    if (HasConstantRegValue(cf.MipsS(), 0))
      return zero;

    EmitMov(temp_reg, GetConstantRegU32(cf.MipsS()));
    return temp_reg;
  }
  else
  {
    Log_WarningPrintf("Hit memory path in CFGetSafeRegS() for %s", GetRegName(cf.MipsS()));
    rvAsm->LW(temp_reg, PTR(&g_state.regs.r[cf.mips_s]));
    return temp_reg;
  }
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::CFGetSafeRegT(CompileFlags cf, const biscuit::GPR& temp_reg)
{
  if (cf.valid_host_t)
  {
    return GPR(cf.host_t);
  }
  else if (cf.const_t)
  {
    if (HasConstantRegValue(cf.MipsT(), 0))
      return zero;

    EmitMov(temp_reg, GetConstantRegU32(cf.MipsT()));
    return temp_reg;
  }
  else
  {
    Log_WarningPrintf("Hit memory path in CFGetSafeRegT() for %s", GetRegName(cf.MipsT()));
    rvAsm->LW(temp_reg, PTR(&g_state.regs.r[cf.mips_t]));
    return temp_reg;
  }
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::CFGetRegD(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_d);
  return GPR(cf.host_d);
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::CFGetRegS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s);
  return GPR(cf.host_s);
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::CFGetRegT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t);
  return GPR(cf.host_t);
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::CFGetRegLO(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_lo);
  return GPR(cf.host_lo);
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::CFGetRegHI(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_hi);
  return GPR(cf.host_hi);
}

void CPU::NewRec::RISCV64Compiler::MoveSToReg(const biscuit::GPR& dst, CompileFlags cf)
{
  if (cf.valid_host_s)
  {
    if (cf.host_s != dst.Index())
      rvAsm->MV(dst, GPR(cf.host_s));
  }
  else if (cf.const_s)
  {
    EmitMov(dst, GetConstantRegU32(cf.MipsS()));
  }
  else
  {
    Log_WarningPrintf("Hit memory path in MoveSToReg() for %s", GetRegName(cf.MipsS()));
    rvAsm->LW(dst, PTR(&g_state.regs.r[cf.mips_s]));
  }
}

void CPU::NewRec::RISCV64Compiler::MoveTToReg(const biscuit::GPR& dst, CompileFlags cf)
{
  if (cf.valid_host_t)
  {
    if (cf.host_t != dst.Index())
      rvAsm->MV(dst, GPR(cf.host_t));
  }
  else if (cf.const_t)
  {
    EmitMov(dst, GetConstantRegU32(cf.MipsT()));
  }
  else
  {
    Log_WarningPrintf("Hit memory path in MoveTToReg() for %s", GetRegName(cf.MipsT()));
    rvAsm->LW(dst, PTR(&g_state.regs.r[cf.mips_t]));
  }
}

void CPU::NewRec::RISCV64Compiler::MoveMIPSRegToReg(const biscuit::GPR& dst, Reg reg)
{
  DebugAssert(reg < Reg::count);
  if (const std::optional<u32> hreg = CheckHostReg(0, Compiler::HR_TYPE_CPU_REG, reg))
    rvAsm->MV(dst, GPR(hreg.value()));
  else if (HasConstantReg(reg))
    EmitMov(dst, GetConstantRegU32(reg));
  else
    rvAsm->LW(dst, PTR(&g_state.regs.r[static_cast<u8>(reg)]));
}

void CPU::NewRec::RISCV64Compiler::GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val,
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

void CPU::NewRec::RISCV64Compiler::Flush(u32 flags)
{
  Compiler::Flush(flags);

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
    rvAsm->LBU(RARG1, PTR(&g_state.load_delay_reg));
    rvAsm->LW(RARG2, PTR(&g_state.load_delay_value));
    rvAsm->SLLI(RARG1, RARG1, 2); // *4
    rvAsm->ADD(RARG1, RARG1, RSTATE);
    rvAsm->SW(RARG2, offsetof(CPU::State, regs.r[0]), RARG1);
    rvAsm->LI(RSCRATCH, static_cast<u8>(Reg::count));
    rvAsm->SB(RSCRATCH, PTR(&g_state.load_delay_reg));
    m_load_delay_dirty = false;
  }

  if (flags & FLUSH_LOAD_DELAY && m_load_delay_register != Reg::count)
  {
    if (m_load_delay_value_register != NUM_HOST_REGS)
      FreeHostReg(m_load_delay_value_register);

    EmitMov(RSCRATCH, static_cast<u8>(m_load_delay_register));
    rvAsm->SB(RSCRATCH, PTR(&g_state.load_delay_reg));
    m_load_delay_register = Reg::count;
    m_load_delay_dirty = true;
  }

  if (flags & FLUSH_GTE_STALL_FROM_STATE && m_dirty_gte_done_cycle)
  {
    // May as well flush cycles while we're here.
    // GTE spanning blocks is very rare, we _could_ disable this for speed.
    rvAsm->LW(RARG1, PTR(&g_state.pending_ticks));
    rvAsm->LW(RARG2, PTR(&g_state.gte_completion_tick));
    if (m_cycles > 0)
    {
      SafeADDIW(RARG1, RARG1, m_cycles);
      m_cycles = 0;
    }
    Label no_stall;
    rvAsm->BGE(RARG1, RARG2, &no_stall);
    rvAsm->MV(RARG1, RARG2);
    rvAsm->Bind(&no_stall);
    rvAsm->SW(RARG1, PTR(&g_state.pending_ticks));
    m_dirty_gte_done_cycle = false;
  }

  if (flags & FLUSH_GTE_DONE_CYCLE && m_gte_done_cycle > m_cycles)
  {
    rvAsm->LW(RARG1, PTR(&g_state.pending_ticks));

    // update cycles at the same time
    if (flags & FLUSH_CYCLES && m_cycles > 0)
    {
      SafeADDIW(RARG1, RARG1, m_cycles);
      rvAsm->SW(RARG1, PTR(&g_state.pending_ticks));
      m_gte_done_cycle -= m_cycles;
      m_cycles = 0;
    }

    SafeADDIW(RARG1, RARG1, m_gte_done_cycle);
    rvAsm->SW(RARG1, PTR(&g_state.gte_completion_tick));
    m_gte_done_cycle = 0;
    m_dirty_gte_done_cycle = true;
  }

  if (flags & FLUSH_CYCLES && m_cycles > 0)
  {
    rvAsm->LW(RARG1, PTR(&g_state.pending_ticks));
    SafeADDIW(RARG1, RARG1, m_cycles);
    rvAsm->SW(RARG1, PTR(&g_state.pending_ticks));
    m_gte_done_cycle = std::max<TickCount>(m_gte_done_cycle - m_cycles, 0);
    m_cycles = 0;
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_Fallback()
{
  Flush(FLUSH_FOR_INTERPRETER);

#if 0
  cg->call(&CPU::Recompiler::Thunks::InterpretInstruction);

  // TODO: make me less garbage
  // TODO: this is wrong, it flushes the load delay on the same cycle when we return.
  // but nothing should be going through here..
  Label no_load_delay;
  cg->movzx(RWARG1, cg->byte[PTR(&g_state.next_load_delay_reg)]);
  cg->cmp(RWARG1, static_cast<u8>(Reg::count));
  cg->je(no_load_delay, CodeGenerator::T_SHORT);
  cg->mov(RWARG2, cg->dword[PTR(&g_state.next_load_delay_value)]);
  cg->mov(cg->byte[PTR(&g_state.load_delay_reg)], RWARG1);
  cg->mov(cg->dword[PTR(&g_state.load_delay_value)], RWARG2);
  cg->mov(cg->byte[PTR(&g_state.next_load_delay_reg)], static_cast<u32>(Reg::count));
  cg->L(no_load_delay);

  m_load_delay_dirty = EMULATE_LOAD_DELAYS;
#else
  Panic("Fixme");
#endif
}

void CPU::NewRec::RISCV64Compiler::CheckBranchTarget(const biscuit::GPR& pcreg)
{
  if (!g_settings.cpu_recompiler_memory_exceptions)
    return;

  DebugAssert(pcreg != RSCRATCH);
  rvAsm->ANDI(RSCRATCH, pcreg, 0x3);
  SwitchToFarCode(true, &Assembler::BEQ, RSCRATCH, zero);

  BackupHostState();
  EndBlockWithException(Exception::AdEL);

  RestoreHostState();
  SwitchToNearCode(false);
}

void CPU::NewRec::RISCV64Compiler::Compile_jr(CompileFlags cf)
{
  const GPR pcreg = CFGetRegS(cf);
  CheckBranchTarget(pcreg);

  rvAsm->SW(pcreg, PTR(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::NewRec::RISCV64Compiler::Compile_jalr(CompileFlags cf)
{
  const GPR pcreg = CFGetRegS(cf);
  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetBranchReturnAddress(cf));

  CheckBranchTarget(pcreg);
  rvAsm->SW(pcreg, PTR(&g_state.pc));

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::NewRec::RISCV64Compiler::Compile_bxx(CompileFlags cf, BranchCondition cond)
{
  AssertRegOrConstS(cf);

  const u32 taken_pc = GetConditionalBranchTarget(cf);

  Flush(FLUSH_FOR_BRANCH);

  DebugAssert(cf.valid_host_s);

  // MipsT() here should equal zero for zero branches.
  DebugAssert(cond == BranchCondition::Equal || cond == BranchCondition::NotEqual || cf.MipsT() == Reg::zero);

  Label taken;
  const GPR rs = CFGetRegS(cf);
  switch (cond)
  {
    case BranchCondition::Equal:
    case BranchCondition::NotEqual:
    {
      AssertRegOrConstT(cf);
      if (cf.const_t && HasConstantRegValue(cf.MipsT(), 0))
      {
        (cond == BranchCondition::Equal) ? rvAsm->BEQZ(rs, &taken) : rvAsm->BNEZ(rs, &taken);
      }
      else
      {
        const GPR rt = cf.valid_host_t ? CFGetRegT(cf) : RARG1;
        if (!cf.valid_host_t)
          MoveTToReg(RARG1, cf);
        if (cond == Compiler::BranchCondition::Equal)
          rvAsm->BEQ(rs, rt, &taken);
        else
          rvAsm->BNE(rs, rt, &taken);
      }
    }
    break;

    case BranchCondition::GreaterThanZero:
    {
      rvAsm->BGTZ(rs, &taken);
    }
    break;

    case BranchCondition::GreaterEqualZero:
    {
      rvAsm->BGEZ(rs, &taken);
    }
    break;

    case BranchCondition::LessThanZero:
    {
      rvAsm->BLTZ(rs, &taken);
    }
    break;

    case BranchCondition::LessEqualZero:
    {
      rvAsm->BLEZ(rs, &taken);
    }
    break;
  }

  BackupHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(m_compiler_pc, true);

  rvAsm->Bind(&taken);

  RestoreHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(taken_pc, true);
}

void CPU::NewRec::RISCV64Compiler::Compile_addi(CompileFlags cf, bool overflow)
{
  const GPR rs = CFGetRegS(cf);
  const GPR rt = CFGetRegT(cf);
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
  else if (rt.Index() != rs.Index())
  {
    rvAsm->MV(rt, rs);
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_addi(CompileFlags cf)
{
  Compile_addi(cf, g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::NewRec::RISCV64Compiler::Compile_addiu(CompileFlags cf)
{
  Compile_addi(cf, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_slti(CompileFlags cf)
{
  Compile_slti(cf, true);
}

void CPU::NewRec::RISCV64Compiler::Compile_sltiu(CompileFlags cf)
{
  Compile_slti(cf, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_slti(CompileFlags cf, bool sign)
{
  if (sign)
    SafeSLTI(CFGetRegT(cf), CFGetRegS(cf), inst->i.imm_sext32());
  else
    SafeSLTIU(CFGetRegT(cf), CFGetRegS(cf), inst->i.imm_sext32());
}

void CPU::NewRec::RISCV64Compiler::Compile_andi(CompileFlags cf)
{
  const GPR rt = CFGetRegT(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    SafeANDI(rt, CFGetRegS(cf), imm);
  else
    EmitMov(rt, 0);
}

void CPU::NewRec::RISCV64Compiler::Compile_ori(CompileFlags cf)
{
  const GPR rt = CFGetRegT(cf);
  const GPR rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    SafeORI(rt, rs, imm);
  else if (rt.Index() != rs.Index())
    rvAsm->MV(rt, rs);
}

void CPU::NewRec::RISCV64Compiler::Compile_xori(CompileFlags cf)
{
  const GPR rt = CFGetRegT(cf);
  const GPR rs = CFGetRegS(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    SafeXORI(rt, rs, imm);
  else if (rt.Index() != rs.Index())
    rvAsm->MV(rt, rs);
}

void CPU::NewRec::RISCV64Compiler::Compile_shift(
  CompileFlags cf, void (biscuit::Assembler::*op)(biscuit::GPR, biscuit::GPR, biscuit::GPR),
  void (biscuit::Assembler::*op_const)(biscuit::GPR, biscuit::GPR, unsigned))
{
  const GPR rd = CFGetRegD(cf);
  const GPR rt = CFGetRegT(cf);
  if (inst->r.shamt > 0)
    (rvAsm->*op_const)(rd, rt, inst->r.shamt);
  else if (rd.Index() != rt.Index())
    rvAsm->MV(rd, rt);
}

void CPU::NewRec::RISCV64Compiler::Compile_sll(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::SLLW, &Assembler::SLLIW);
}

void CPU::NewRec::RISCV64Compiler::Compile_srl(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::SRLW, &Assembler::SRLIW);
}

void CPU::NewRec::RISCV64Compiler::Compile_sra(CompileFlags cf)
{
  Compile_shift(cf, &Assembler::SRAW, &Assembler::SRAIW);
}

void CPU::NewRec::RISCV64Compiler::Compile_variable_shift(
  CompileFlags cf, void (biscuit::Assembler::*op)(biscuit::GPR, biscuit::GPR, biscuit::GPR),
  void (biscuit::Assembler::*op_const)(biscuit::GPR, biscuit::GPR, unsigned))
{
  const GPR rd = CFGetRegD(cf);

  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const GPR rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  if (cf.const_s)
  {
    if (const u32 shift = GetConstantRegU32(cf.MipsS()); shift != 0)
      (rvAsm->*op_const)(rd, rt, shift & 31u);
    else if (rd.Index() != rt.Index())
      rvAsm->MV(rd, rt);
  }
  else
  {
    (rvAsm->*op)(rd, rt, CFGetRegS(cf));
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_sllv(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::SLLW, &Assembler::SLLIW);
}

void CPU::NewRec::RISCV64Compiler::Compile_srlv(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::SRLW, &Assembler::SRLIW);
}

void CPU::NewRec::RISCV64Compiler::Compile_srav(CompileFlags cf)
{
  Compile_variable_shift(cf, &Assembler::SRAW, &Assembler::SRAIW);
}

void CPU::NewRec::RISCV64Compiler::Compile_mult(CompileFlags cf, bool sign)
{
  const GPR rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const GPR rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  // TODO: if lo/hi gets killed, we can use a 32-bit multiply
  const GPR lo = CFGetRegLO(cf);
  const GPR hi = CFGetRegHI(cf);

  if (sign)
  {
    rvAsm->MUL(lo, rs, rt);
    rvAsm->SRAI64(hi, lo, 32);
    EmitDSExtW(lo, lo);
  }
  else
  {
    // Need to make it unsigned.
    EmitDUExtW(RARG1, rs);
    EmitDUExtW(RARG2, rt);
    rvAsm->MUL(lo, RARG1, RARG2);
    rvAsm->SRAI64(hi, lo, 32);
    EmitDSExtW(lo, lo);
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_mult(CompileFlags cf)
{
  Compile_mult(cf, true);
}

void CPU::NewRec::RISCV64Compiler::Compile_multu(CompileFlags cf)
{
  Compile_mult(cf, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_div(CompileFlags cf)
{
  // 36 Volume I: RISC-V User-Level ISA V2.2
  const GPR rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const GPR rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const GPR rlo = CFGetRegLO(cf);
  const GPR rhi = CFGetRegHI(cf);

  Label done;
  Label not_divide_by_zero;
  rvAsm->BNEZ(rt, &not_divide_by_zero);
  rvAsm->MV(rhi, rs); // hi = num
  rvAsm->SRAI64(rlo, rs, 63);
  rvAsm->ANDI(rlo, rlo, 2);
  rvAsm->ADDI(rlo, rlo, -1); // lo = s >= 0 ? -1 : 1
  rvAsm->J(&done);

  rvAsm->Bind(&not_divide_by_zero);
  Label not_unrepresentable;
  EmitMov(RSCRATCH, static_cast<u32>(-1));
  rvAsm->BNE(rt, RSCRATCH, &not_unrepresentable);
  EmitMov(rlo, 0x80000000u);
  rvAsm->BNE(rs, rlo, &not_unrepresentable);
  EmitMov(rhi, 0);
  rvAsm->J(&done);

  rvAsm->Bind(&not_unrepresentable);

  rvAsm->DIVW(rlo, rs, rt);
  rvAsm->REMW(rhi, rs, rt);

  rvAsm->Bind(&done);
}

void CPU::NewRec::RISCV64Compiler::Compile_divu(CompileFlags cf)
{
  const GPR rs = cf.valid_host_s ? CFGetRegS(cf) : RARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  const GPR rt = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const GPR rlo = CFGetRegLO(cf);
  const GPR rhi = CFGetRegHI(cf);

  // Semantics match? :-)
  rvAsm->DIVUW(rlo, rs, rt);
  rvAsm->REMUW(rhi, rs, rt);
}

void CPU::NewRec::RISCV64Compiler::TestOverflow(const biscuit::GPR& long_res, const biscuit::GPR& res,
                                                const biscuit::GPR& reg_to_discard)
{
  SwitchToFarCode(true, &Assembler::BEQ, long_res, res);

  BackupHostState();

  // toss the result
  ClearHostReg(reg_to_discard.Index());

  EndBlockWithException(Exception::Ov);

  RestoreHostState();

  SwitchToNearCode(false);
}

void CPU::NewRec::RISCV64Compiler::Compile_dst_op(
  CompileFlags cf, void (biscuit::Assembler::*op)(biscuit::GPR, biscuit::GPR, biscuit::GPR),
  void (RISCV64Compiler::*op_const)(const biscuit::GPR& rd, const biscuit::GPR& rs, u32 imm),
  void (biscuit::Assembler::*op_long)(biscuit::GPR, biscuit::GPR, biscuit::GPR), bool commutative, bool overflow)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const GPR rd = CFGetRegD(cf);

  if (overflow)
  {
    const GPR rs = CFGetSafeRegS(cf, RARG1);
    const GPR rt = CFGetSafeRegT(cf, RARG2);
    (rvAsm->*op)(RARG3, rs, rt);
    (rvAsm->*op_long)(rd, rs, rt);
    TestOverflow(RARG3, rd, rd);
    return;
  }

  if (cf.valid_host_s && cf.valid_host_t)
  {
    (rvAsm->*op)(rd, CFGetRegS(cf), CFGetRegT(cf));
  }
  else if (commutative && (cf.const_s || cf.const_t))
  {
    const GPR src = cf.const_s ? CFGetRegT(cf) : CFGetRegS(cf);
    if (const u32 cv = GetConstantRegU32(cf.const_s ? cf.MipsS() : cf.MipsT()); cv != 0)
    {
      (this->*op_const)(rd, src, cv);
    }
    else
    {
      if (rd.Index() != src.Index())
        rvAsm->MV(rd, src);
      overflow = false;
    }
  }
  else if (cf.const_s)
  {
    if (HasConstantRegValue(cf.MipsS(), 0))
    {
      (rvAsm->*op)(rd, zero, CFGetRegT(cf));
    }
    else
    {
      EmitMov(RSCRATCH, GetConstantRegU32(cf.MipsS()));
      (rvAsm->*op)(rd, RSCRATCH, CFGetRegT(cf));
    }
  }
  else if (cf.const_t)
  {
    const GPR rs = CFGetRegS(cf);
    if (const u32 cv = GetConstantRegU32(cf.const_s ? cf.MipsS() : cf.MipsT()); cv != 0)
    {
      (this->*op_const)(rd, rs, cv);
    }
    else
    {
      if (rd.Index() != rs.Index())
        rvAsm->MV(rd, rs);
      overflow = false;
    }
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_add(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::ADDW, &RISCV64Compiler::SafeADDIW, &Assembler::ADD, true,
                 g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::NewRec::RISCV64Compiler::Compile_addu(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::ADDW, &RISCV64Compiler::SafeADDIW, &Assembler::ADD, true, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_sub(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::SUBW, &RISCV64Compiler::SafeSUBIW, &Assembler::SUB, false,
                 g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::NewRec::RISCV64Compiler::Compile_subu(CompileFlags cf)
{
  Compile_dst_op(cf, &Assembler::SUBW, &RISCV64Compiler::SafeSUBIW, &Assembler::SUB, false, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_and(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // special cases - and with self -> self, and with 0 -> 0
  const GPR regd = CFGetRegD(cf);
  if (cf.MipsS() == cf.MipsT())
  {
    rvAsm->MV(regd, CFGetRegS(cf));
    return;
  }
  else if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0))
  {
    EmitMov(regd, 0);
    return;
  }

  Compile_dst_op(cf, &Assembler::AND, &RISCV64Compiler::SafeANDI, &Assembler::AND, true, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_or(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  // or/nor with 0 -> no effect
  const GPR regd = CFGetRegD(cf);
  if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0) || cf.MipsS() == cf.MipsT())
  {
    cf.const_s ? MoveTToReg(regd, cf) : MoveSToReg(regd, cf);
    return;
  }

  Compile_dst_op(cf, &Assembler::OR, &RISCV64Compiler::SafeORI, &Assembler::OR, true, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_xor(CompileFlags cf)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const GPR regd = CFGetRegD(cf);
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

  Compile_dst_op(cf, &Assembler::XOR, &RISCV64Compiler::SafeXORI, &Assembler::XOR, true, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_nor(CompileFlags cf)
{
  Compile_or(cf);
  rvAsm->NOT(CFGetRegD(cf), CFGetRegD(cf));
}

void CPU::NewRec::RISCV64Compiler::Compile_slt(CompileFlags cf)
{
  Compile_slt(cf, true);
}

void CPU::NewRec::RISCV64Compiler::Compile_sltu(CompileFlags cf)
{
  Compile_slt(cf, false);
}

void CPU::NewRec::RISCV64Compiler::Compile_slt(CompileFlags cf, bool sign)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const GPR rd = CFGetRegD(cf);
  const GPR rs = CFGetSafeRegS(cf, RARG1);

  if (cf.const_t && rvIsValidSExtITypeImm(GetConstantRegU32(cf.MipsT())))
  {
    if (sign)
      rvAsm->SLTI(rd, rs, GetConstantRegS32(cf.MipsT()));
    else
      rvAsm->SLTIU(rd, rs, GetConstantRegS32(cf.MipsT()));
  }
  else
  {
    const GPR rt = CFGetSafeRegT(cf, RARG2);
    if (sign)
      rvAsm->SLT(rd, rs, rt);
    else
      rvAsm->SLTU(rd, rs, rt);
  }
}

biscuit::GPR CPU::NewRec::RISCV64Compiler::ComputeLoadStoreAddressArg(
  CompileFlags cf, const std::optional<VirtualMemoryAddress>& address, const std::optional<const biscuit::GPR>& reg)
{
  const u32 imm = inst->i.imm_sext32();
  if (cf.valid_host_s && imm == 0 && !reg.has_value())
    return CFGetRegS(cf);

  const GPR dst = reg.has_value() ? reg.value() : RARG1;
  if (address.has_value())
  {
    EmitMov(dst, address.value());
  }
  else if (imm == 0)
  {
    if (cf.valid_host_s)
    {
      if (const GPR src = CFGetRegS(cf); src.Index() != dst.Index())
        rvAsm->MV(dst, CFGetRegS(cf));
    }
    else
    {
      rvAsm->LW(dst, PTR(&g_state.regs.r[cf.mips_s]));
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
      rvAsm->LW(dst, PTR(&g_state.regs.r[cf.mips_s]));
      SafeADDIW(dst, dst, inst->i.imm_sext32());
    }
  }

  return dst;
}

template<typename RegAllocFn>
biscuit::GPR CPU::NewRec::RISCV64Compiler::GenerateLoad(const biscuit::GPR& addr_reg, MemoryAccessSize size, bool sign,
                                                        bool use_fastmem, const RegAllocFn& dst_reg_alloc)
{
  if (use_fastmem)
  {
    m_cycles += Bus::RAM_READ_TICKS;

    // TODO: Make this better. If we're loading the address from state, we can use LWU instead, and skip this.
    // TODO: LUT fastmem
    const GPR dst = dst_reg_alloc();
    rvAsm->SLLI64(RSCRATCH, addr_reg, 32);
    rvAsm->SRLI64(RSCRATCH, RSCRATCH, 32);

    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg.Index() != RARG3.Index());
      rvAsm->SRLI64(RARG3, RSCRATCH, Bus::FASTMEM_LUT_PAGE_SHIFT);
      rvAsm->SLLI64(RARG3, RARG3, 8);
      rvAsm->ADD(RARG3, RARG3, RMEMBASE);
      rvAsm->LD(RARG3, 0, RARG3);
      rvAsm->ADD(RSCRATCH, RSCRATCH, RARG3);
    }
    else
    {
      rvAsm->ADD(RSCRATCH, RSCRATCH, RMEMBASE);
    }

    u8* start = m_emitter->GetCursorPointer();
    switch (size)
    {
      case MemoryAccessSize::Byte:
        sign ? rvAsm->LB(dst, 0, RSCRATCH) : rvAsm->LBU(dst, 0, RSCRATCH);
        break;

      case MemoryAccessSize::HalfWord:
        sign ? rvAsm->LH(dst, 0, RSCRATCH) : rvAsm->LHU(dst, 0, RSCRATCH);
        break;

      case MemoryAccessSize::Word:
        rvAsm->LW(dst, 0, RSCRATCH);
        break;
    }

    // We need a nop, because the slowmem jump might be more than 1MB away.
    rvAsm->NOP();

    AddLoadStoreInfo(start, 8, addr_reg.Index(), dst.Index(), size, sign, true);
    return dst;
  }

  if (addr_reg.Index() != RARG1.Index())
    rvAsm->MV(RARG1, addr_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&Recompiler::Thunks::ReadMemoryByte) :
                         reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedReadMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&Recompiler::Thunks::ReadMemoryHalfWord) :
                         reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedReadMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&Recompiler::Thunks::ReadMemoryWord) :
                         reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedReadMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    rvAsm->SRLI64(RSCRATCH, RRET, 63);
    SwitchToFarCode(true, &Assembler::BEQ, RSCRATCH, zero);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const GPR temp = GPR(AllocateTempHostReg(HR_CALLEE_SAVED));
    rvAsm->NEG(temp, RRET);
    rvAsm->SLLIW(temp, temp, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (-result << 2) | BD | cop_n
    SafeORI(RARG1, temp,
            Cop0Registers::CAUSE::MakeValueForException(
              static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n));
    EmitMov(RARG2, m_current_instruction_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    FreeHostReg(temp.Index());
    EndBlock(std::nullopt, true);

    RestoreHostState();
    SwitchToNearCode(false);
  }

  const GPR dst_reg = dst_reg_alloc();
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
        rvEmitDSExtW(rvAsm, dst_reg, RRET);
      else if (dst_reg.Index() != RRET.Index())
        rvAsm->MV(dst_reg, RRET);
    }
    break;
  }

  return dst_reg;
}

void CPU::NewRec::RISCV64Compiler::GenerateStore(const biscuit::GPR& addr_reg, const biscuit::GPR& value_reg,
                                                 MemoryAccessSize size, bool use_fastmem)
{
  if (use_fastmem)
  {
    DebugAssert(value_reg != RSCRATCH);
    rvAsm->SLLI64(RSCRATCH, addr_reg, 32);
    rvAsm->SRLI64(RSCRATCH, RSCRATCH, 32);

    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg.Index() != RARG3.Index());
      rvAsm->SRLI64(RARG3, RSCRATCH, Bus::FASTMEM_LUT_PAGE_SHIFT);
      rvAsm->SLLI64(RARG3, RARG3, 8);
      rvAsm->ADD(RARG3, RARG3, RMEMBASE);
      rvAsm->LD(RARG3, 0, RARG3);
      rvAsm->ADD(RSCRATCH, RSCRATCH, RARG3);
    }
    else
    {
      rvAsm->ADD(RSCRATCH, RSCRATCH, RMEMBASE);
    }

    u8* start = m_emitter->GetCursorPointer();
    switch (size)
    {
      case MemoryAccessSize::Byte:
        rvAsm->SB(value_reg, 0, RSCRATCH);
        break;

      case MemoryAccessSize::HalfWord:
        rvAsm->SH(value_reg, 0, RSCRATCH);
        break;

      case MemoryAccessSize::Word:
        rvAsm->SW(value_reg, 0, RSCRATCH);
        break;
    }

    // We need a nop, because the slowmem jump might be more than 1MB away.
    rvAsm->NOP();

    AddLoadStoreInfo(start, 8, addr_reg.Index(), value_reg.Index(), size, false, false);
    return;
  }

  if (addr_reg.Index() != RARG1.Index())
    rvAsm->MV(RARG1, addr_reg);
  if (value_reg.Index() != RARG2.Index())
    rvAsm->MV(RARG2, value_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&Recompiler::Thunks::WriteMemoryByte) :
                         reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedWriteMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&Recompiler::Thunks::WriteMemoryHalfWord) :
                         reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedWriteMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      EmitCall(checked ? reinterpret_cast<const void*>(&Recompiler::Thunks::WriteMemoryWord) :
                         reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedWriteMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    SwitchToFarCode(true, &Assembler::BEQ, RRET, zero);
    BackupHostState();

    // Need to stash this in a temp because of the flush.
    const GPR temp = GPR(AllocateTempHostReg(HR_CALLEE_SAVED));
    rvAsm->SLLIW(temp, RRET, 2);

    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (result << 2) | BD | cop_n
    SafeORI(RARG1, temp,
            Cop0Registers::CAUSE::MakeValueForException(
              static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n));
    EmitMov(RARG2, m_current_instruction_pc);
    EmitCall(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    FreeHostReg(temp.Index());
    EndBlock(std::nullopt, true);

    RestoreHostState();
    SwitchToNearCode(false);
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_lxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                               const std::optional<VirtualMemoryAddress>& address)
{
  const std::optional<GPR> addr_reg = (g_settings.gpu_pgxp_enable && cf.MipsT() != Reg::zero) ?
                                        std::optional<GPR>(GPR(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                        std::optional<GPR>();
  FlushForLoadStore(address, false, use_fastmem);
  const GPR addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const GPR data = GenerateLoad(addr, size, sign, use_fastmem, [this, cf]() {
    if (cf.MipsT() == Reg::zero)
      return RRET;

    return GPR(AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                               EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, cf.MipsT()));
  });

  if (g_settings.gpu_pgxp_enable && cf.MipsT() != Reg::zero)
  {
    Flush(FLUSH_FOR_C_CALL);

    EmitMov(RARG1, inst->bits);
    rvAsm->MV(RARG2, addr);
    rvAsm->MV(RARG3, data);
    EmitCall(s_pgxp_mem_load_functions[static_cast<u32>(size)][static_cast<u32>(sign)]);
    FreeHostReg(addr_reg.value().Index());
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_lwx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                               const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  const GPR addr = GPR(AllocateTempHostReg(HR_CALLEE_SAVED));
  FlushForLoadStore(address, false, use_fastmem);

  // TODO: if address is constant, this can be simplified..

  // If we're coming from another block, just flush the load delay and hope for the best..
  if (m_load_delay_dirty)
    UpdateLoadDelay();

  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  rvAsm->ANDI(RARG1, addr, ~0x3u);
  GenerateLoad(RARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RRET; });

  if (inst->r.rt == Reg::zero)
  {
    FreeHostReg(addr.Index());
    return;
  }

  // lwl/lwr from a load-delayed value takes the new value, but it itself, is load delayed, so the original value is
  // never written back. NOTE: can't trust T in cf because of the flush
  const Reg rt = inst->r.rt;
  GPR value;
  if (m_load_delay_register == rt)
  {
    const u32 existing_ld_rt = (m_load_delay_value_register == NUM_HOST_REGS) ?
                                 AllocateHostReg(HR_MODE_READ, HR_TYPE_LOAD_DELAY_VALUE, rt) :
                                 m_load_delay_value_register;
    RenameHostReg(existing_ld_rt, HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt);
    value = GPR(existing_ld_rt);
  }
  else
  {
    if constexpr (EMULATE_LOAD_DELAYS)
    {
      value = GPR(AllocateHostReg(HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt));
      if (const std::optional<u32> rtreg = CheckHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt); rtreg.has_value())
        rvAsm->MV(value, GPR(rtreg.value()));
      else if (HasConstantReg(rt))
        EmitMov(value, GetConstantRegU32(rt));
      else
        rvAsm->LW(value, PTR(&g_state.regs.r[static_cast<u8>(rt)]));
    }
    else
    {
      value = GPR(AllocateHostReg(HR_MODE_READ | HR_MODE_WRITE, HR_TYPE_CPU_REG, rt));
    }
  }

  DebugAssert(value.Index() != RARG2.Index() && value.Index() != RARG3.Index());
  rvAsm->ANDI(RARG2, addr, 3);
  rvAsm->SLLIW(RARG2, RARG2, 3); // *8
  EmitMov(RARG3, 24);
  rvAsm->SUBW(RARG3, RARG3, RARG2);

  if (inst->op == InstructionOp::lwl)
  {
    // const u32 mask = UINT32_C(0x00FFFFFF) >> shift;
    // new_value = (value & mask) | (RWRET << (24 - shift));
    EmitMov(RSCRATCH, 0xFFFFFFu);
    rvAsm->SRLW(RSCRATCH, RSCRATCH, RARG2);
    rvAsm->AND(value, value, RSCRATCH);
    rvAsm->SLLW(RRET, RRET, RARG3);
    rvAsm->OR(value, value, RRET);
  }
  else
  {
    // const u32 mask = UINT32_C(0xFFFFFF00) << (24 - shift);
    // new_value = (value & mask) | (RWRET >> shift);
    rvAsm->SRLW(RRET, RRET, RARG2);
    EmitMov(RSCRATCH, 0xFFFFFF00u);
    rvAsm->SLLW(RSCRATCH, RSCRATCH, RARG3);
    rvAsm->AND(value, value, RSCRATCH);
    rvAsm->OR(value, value, RRET);
  }

  FreeHostReg(addr.Index());

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    rvAsm->MV(RARG3, value);
    rvAsm->ANDI(RARG2, addr, ~0x3u);
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LW));
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_lwc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                                const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, true);
  const std::optional<GPR> addr_reg =
    g_settings.gpu_pgxp_enable ? std::optional<GPR>(GPR(AllocateTempHostReg(HR_CALLEE_SAVED))) : std::optional<GPR>();
  FlushForLoadStore(address, false, use_fastmem);
  const GPR addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const GPR value = GenerateLoad(addr, MemoryAccessSize::Word, false, use_fastmem, [this, action]() {
    return (action == GTERegisterAccessAction::CallHandler && g_settings.gpu_pgxp_enable) ?
             GPR(AllocateTempHostReg(HR_CALLEE_SAVED)) :
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
      rvAsm->SW(value, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::SignExtend16:
    {
      EmitSExtH(RARG3, value);
      rvAsm->SW(RARG3, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::ZeroExtend16:
    {
      EmitUExtH(RARG3, value);
      rvAsm->SW(RARG3, PTR(ptr));
      break;
    }

    case GTERegisterAccessAction::CallHandler:
    {
      Flush(FLUSH_FOR_C_CALL);
      rvAsm->MV(RARG2, value);
      EmitMov(RARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::WriteRegister));
      break;
    }

    case GTERegisterAccessAction::PushFIFO:
    {
      // SXY0 <- SXY1
      // SXY1 <- SXY2
      // SXY2 <- SXYP
      DebugAssert(value.Index() != RARG2.Index() && value.Index() != RARG3.Index());
      rvAsm->LW(RARG2, PTR(&g_state.gte_regs.SXY1[0]));
      rvAsm->LW(RARG3, PTR(&g_state.gte_regs.SXY2[0]));
      rvAsm->SW(RARG2, PTR(&g_state.gte_regs.SXY0[0]));
      rvAsm->SW(RARG3, PTR(&g_state.gte_regs.SXY1[0]));
      rvAsm->SW(value, PTR(&g_state.gte_regs.SXY2[0]));
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
    rvAsm->MV(RARG3, value);
    if (value.Index() != RRET.Index())
      FreeHostReg(value.Index());
    rvAsm->MV(RARG2, addr);
    FreeHostReg(addr_reg.value().Index());
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_LWC2));
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_sxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                               const std::optional<VirtualMemoryAddress>& address)
{
  AssertRegOrConstS(cf);
  AssertRegOrConstT(cf);

  const std::optional<GPR> addr_reg =
    g_settings.gpu_pgxp_enable ? std::optional<GPR>(GPR(AllocateTempHostReg(HR_CALLEE_SAVED))) : std::optional<GPR>();
  FlushForLoadStore(address, true, use_fastmem);
  const GPR addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const GPR data = cf.valid_host_t ? CFGetRegT(cf) : RARG2;
  if (!cf.valid_host_t)
    MoveTToReg(RARG2, cf);

  GenerateStore(addr, data, size, use_fastmem);

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    MoveMIPSRegToReg(RARG3, cf.MipsT());
    rvAsm->MV(RARG2, addr);
    EmitMov(RARG1, inst->bits);
    EmitCall(s_pgxp_mem_store_functions[static_cast<u32>(size)]);
    FreeHostReg(addr_reg.value().Index());
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_swx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                               const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  // TODO: this can take over rt's value if it's no longer needed
  // NOTE: can't trust T in cf because of the alloc
  const GPR addr = GPR(AllocateTempHostReg(HR_CALLEE_SAVED));
  const GPR value = g_settings.gpu_pgxp_enable ? GPR(AllocateTempHostReg(HR_CALLEE_SAVED)) : RARG2;
  if (g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  FlushForLoadStore(address, true, use_fastmem);

  // TODO: if address is constant, this can be simplified..
  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  rvAsm->ANDI(RARG1, addr, ~0x3u);
  GenerateLoad(RARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RRET; });

  rvAsm->ANDI(RSCRATCH, addr, 3);
  rvAsm->SLLIW(RSCRATCH, RSCRATCH, 3); // *8
  rvAsm->ANDI(addr, addr, ~0x3u);

  // Need to load down here for PGXP-off, because it's in a volatile reg that can get overwritten by flush.
  if (!g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  if (inst->op == InstructionOp::swl)
  {
    // const u32 mem_mask = UINT32_C(0xFFFFFF00) << shift;
    // new_value = (RWRET & mem_mask) | (value >> (24 - shift));
    EmitMov(RARG3, 0xFFFFFF00u);
    rvAsm->SLLW(RARG3, RARG3, RSCRATCH);
    rvAsm->AND(RRET, RRET, RARG3);

    EmitMov(RARG3, 24);
    rvAsm->SUBW(RARG3, RARG3, RSCRATCH);
    rvAsm->SRLW(value, value, RARG3);
    rvAsm->OR(value, value, RRET);
  }
  else
  {
    // const u32 mem_mask = UINT32_C(0x00FFFFFF) >> (24 - shift);
    // new_value = (RWRET & mem_mask) | (value << shift);
    rvAsm->SLLW(value, value, RSCRATCH);

    EmitMov(RARG3, 24);
    rvAsm->SUBW(RARG3, RARG3, RSCRATCH);
    EmitMov(RSCRATCH, 0x00FFFFFFu);
    rvAsm->SRLW(RSCRATCH, RSCRATCH, RARG3);
    rvAsm->AND(RRET, RRET, RSCRATCH);
    rvAsm->OR(value, value, RRET);
  }

  if (!g_settings.gpu_pgxp_enable)
  {
    GenerateStore(addr, value, MemoryAccessSize::Word, use_fastmem);
    FreeHostReg(addr.Index());
  }
  else
  {
    GenerateStore(addr, value, MemoryAccessSize::Word, use_fastmem);

    Flush(FLUSH_FOR_C_CALL);
    rvAsm->MV(RARG3, value);
    FreeHostReg(value.Index());
    rvAsm->MV(RARG2, addr);
    FreeHostReg(addr.Index());
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SW));
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_swc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                                const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, false);
  const GPR addr = (g_settings.gpu_pgxp_enable || action == GTERegisterAccessAction::CallHandler) ?
                     GPR(AllocateTempHostReg(HR_CALLEE_SAVED)) :
                     RARG1;
  const GPR data = g_settings.gpu_pgxp_enable ? GPR(AllocateTempHostReg(HR_CALLEE_SAVED)) : RARG2;
  FlushForLoadStore(address, true, use_fastmem);
  ComputeLoadStoreAddressArg(cf, address, addr);

  switch (action)
  {
    case GTERegisterAccessAction::Direct:
    {
      rvAsm->LW(data, PTR(ptr));
    }
    break;

    case GTERegisterAccessAction::CallHandler:
    {
      // should already be flushed.. except in fastmem case
      Flush(FLUSH_FOR_C_CALL);
      EmitMov(RARG1, index);
      EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));
      rvAsm->MV(data, RRET);
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
    if (addr.Index() != RARG1.Index())
      FreeHostReg(addr.Index());
  }
  else
  {
    // TODO: This can be simplified because we don't need to validate in PGXP..
    Flush(FLUSH_FOR_C_CALL);
    rvAsm->MV(RARG3, data);
    FreeHostReg(data.Index());
    rvAsm->MV(RARG2, addr);
    FreeHostReg(addr.Index());
    EmitMov(RARG1, inst->bits);
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_SWC2));
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_mtc0(CompileFlags cf)
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
    Log_DebugPrintf("Ignoring write to read-only cop0 reg %u", static_cast<u32>(reg));
    return;
  }

  // for some registers, we need to test certain bits
  const bool needs_bit_test = (reg == Cop0Reg::SR);
  const GPR new_value = RARG1;
  const GPR old_value = RARG2;
  const GPR changed_bits = RARG3;
  const GPR mask_reg = RSCRATCH;

  // Load old value
  rvAsm->LW(old_value, PTR(ptr));

  // No way we fit this in an immediate..
  EmitMov(mask_reg, mask);

  // update value
  // TODO: This is creating pointless MV instructions.. why?
  if (cf.valid_host_t)
    rvAsm->AND(new_value, CFGetRegT(cf), mask_reg);
  else
    EmitMov(new_value, GetConstantRegU32(cf.MipsT()) & mask);

  if (needs_bit_test)
    rvAsm->XOR(changed_bits, old_value, new_value);
  rvAsm->NOT(mask_reg, mask_reg);
  rvAsm->AND(old_value, old_value, mask_reg);
  rvAsm->OR(new_value, old_value, new_value);
  rvAsm->SW(new_value, PTR(ptr));

  if (reg == Cop0Reg::SR)
  {
    // TODO: replace with register backup
    // We could just inline the whole thing..
    Flush(FLUSH_FOR_C_CALL);

    rvAsm->SRLIW(RSCRATCH, changed_bits, 16);
    rvAsm->ANDI(RSCRATCH, RSCRATCH, 1);
    SwitchToFarCode(true, &Assembler::BEQ, RSCRATCH, zero);
    rvAsm->ADDI(sp, sp, -16);
    rvAsm->SW(RARG1, 0, sp);
    EmitCall(reinterpret_cast<const void*>(&CPU::UpdateMemoryPointers));
    rvAsm->LW(RARG1, 0, sp);
    rvAsm->ADDI(sp, sp, 16);
    rvAsm->LD(RMEMBASE, PTR(&g_state.fastmem_base));
    SwitchToNearCode(true);

    TestInterrupts(RARG1);
  }
  else if (reg == Cop0Reg::CAUSE)
  {
    rvAsm->LW(RARG1, PTR(&g_state.cop0_regs.sr.bits));
    TestInterrupts(RARG1);
  }

  if (reg == Cop0Reg::DCIC && g_settings.cpu_recompiler_memory_exceptions)
  {
    // TODO: DCIC handling for debug breakpoints
    Log_WarningPrintf("TODO: DCIC handling for debug breakpoints");
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_rfe(CompileFlags cf)
{
  // shift mode bits right two, preserving upper bits
  rvAsm->LW(RARG1, PTR(&g_state.cop0_regs.sr.bits));
  rvAsm->SRLIW(RSCRATCH, RARG1, 2);
  rvAsm->ANDI(RSCRATCH, RSCRATCH, 0xf);
  rvAsm->ANDI(RARG1, RARG1, ~0xfu);
  rvAsm->OR(RARG1, RARG1, RSCRATCH);
  rvAsm->SW(RARG1, PTR(&g_state.cop0_regs.sr.bits));

  TestInterrupts(RARG1);
}

void CPU::NewRec::RISCV64Compiler::TestInterrupts(const biscuit::GPR& sr)
{
  DebugAssert(sr != RSCRATCH);

  // if Iec == 0 then goto no_interrupt
  Label no_interrupt;
  rvAsm->ANDI(RSCRATCH, sr, 1);
  rvAsm->BEQZ(RSCRATCH, &no_interrupt);

  // sr & cause
  rvAsm->LW(RSCRATCH, PTR(&g_state.cop0_regs.cause.bits));
  rvAsm->AND(sr, sr, RSCRATCH);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  rvAsm->SRLIW(sr, sr, 8);
  rvAsm->ANDI(sr, sr, 0xFF);
  SwitchToFarCode(true, &Assembler::BEQ, sr, zero);

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
    rvAsm->SW(biscuit::zero, PTR(&g_state.downcount));
    if (m_dirty_pc)
      rvAsm->SW(RARG1, PTR(&g_state.pc));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, false, true);
  }

  RestoreHostState();
  SwitchToNearCode(false);

  rvAsm->Bind(&no_interrupt);
}

void CPU::NewRec::RISCV64Compiler::Compile_mfc2(CompileFlags cf)
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
    rvAsm->LW(GPR(hreg), PTR(ptr));
  }
  else if (action == GTERegisterAccessAction::CallHandler)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, index);
    EmitCall(reinterpret_cast<const void*>(&GTE::ReadRegister));

    hreg = AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                           EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, rt);
    rvAsm->MV(GPR(hreg), RRET);
  }
  else
  {
    Panic("Unknown action");
  }

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    EmitMov(RARG1, inst->bits);
    rvAsm->MV(RARG2, GPR(hreg));
    EmitCall(reinterpret_cast<const void*>(&PGXP::CPU_MFC2));
  }
}

void CPU::NewRec::RISCV64Compiler::Compile_mtc2(CompileFlags cf)
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
      rvAsm->SW(CFGetRegT(cf), PTR(ptr));
  }
  else if (action == GTERegisterAccessAction::SignExtend16 || action == GTERegisterAccessAction::ZeroExtend16)
  {
    const bool sign = (action == GTERegisterAccessAction::SignExtend16);
    if (cf.valid_host_t)
    {
      sign ? EmitSExtH(RARG1, CFGetRegT(cf)) : EmitUExtH(RARG1, CFGetRegT(cf));
      rvAsm->SW(RARG1, PTR(ptr));
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
    DebugAssert(RRET.Index() != RARG2.Index() && RRET.Index() != RARG3.Index());
    rvAsm->LW(RARG2, PTR(&g_state.gte_regs.SXY1[0]));
    rvAsm->LW(RARG3, PTR(&g_state.gte_regs.SXY2[0]));
    rvAsm->SW(RARG2, PTR(&g_state.gte_regs.SXY0[0]));
    rvAsm->SW(RARG3, PTR(&g_state.gte_regs.SXY1[0]));
    if (cf.valid_host_t)
      rvAsm->SW(CFGetRegT(cf), PTR(&g_state.gte_regs.SXY2[0]));
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

void CPU::NewRec::RISCV64Compiler::Compile_cop2(CompileFlags cf)
{
  TickCount func_ticks;
  GTE::InstructionImpl func = GTE::GetInstructionImpl(inst->bits, &func_ticks);

  Flush(FLUSH_FOR_C_CALL);
  EmitMov(RARG1, inst->bits & GTE::Instruction::REQUIRED_BITS_MASK);
  EmitCall(reinterpret_cast<const void*>(func));

  AddGTETicks(func_ticks);
}

u32 CPU::NewRec::CompileLoadStoreThunk(void* thunk_code, u32 thunk_space, void* code_address, u32 code_size,
                                       TickCount cycles_to_add, TickCount cycles_to_remove, u32 gpr_bitmask,
                                       u8 address_register, u8 data_register, MemoryAccessSize size, bool is_signed,
                                       bool is_load)
{
  Assembler arm_asm(static_cast<u8*>(thunk_code), thunk_space);
  Assembler* rvAsm = &arm_asm;

  static constexpr u32 GPR_SIZE = 8;

  // save regs
  u32 num_gprs = 0;

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    if ((gpr_bitmask & (1u << i)) && rvIsCallerSavedRegister(i) && (!is_load || data_register != i))
      num_gprs++;
  }

  const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE);

  if (stack_size > 0)
  {
    rvAsm->ADDI(sp, sp, -static_cast<s32>(stack_size));

    u32 stack_offset = 0;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && rvIsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        rvAsm->SD(GPR(i), stack_offset, sp);
        stack_offset += GPR_SIZE;
      }
    }
  }

  if (cycles_to_add != 0)
  {
    // NOTE: we have to reload here, because memory writes can run DMA, which can screw with cycles
    Assert(rvIsValidSExtITypeImm(cycles_to_add));
    rvAsm->LW(RSCRATCH, PTR(&g_state.pending_ticks));
    rvAsm->ADDIW(RSCRATCH, RSCRATCH, cycles_to_add);
    rvAsm->SW(RSCRATCH, PTR(&g_state.pending_ticks));
  }

  if (address_register != RARG1.Index())
    rvAsm->MV(RARG1, GPR(address_register));

  if (!is_load)
  {
    if (data_register != RARG2.Index())
      rvAsm->MV(RARG2, GPR(data_register));
  }

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      rvEmitCall(rvAsm, is_load ? reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedReadMemoryByte) :
                                  reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedWriteMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      rvEmitCall(rvAsm, is_load ? reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedReadMemoryHalfWord) :
                                  reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedWriteMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      rvEmitCall(rvAsm, is_load ? reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedReadMemoryWord) :
                                  reinterpret_cast<const void*>(&Recompiler::Thunks::UncheckedWriteMemoryWord));
    }
    break;
  }

  if (is_load)
  {
    const GPR dst = GPR(data_register);
    switch (size)
    {
      case MemoryAccessSize::Byte:
      {
        is_signed ? rvEmitSExtB(rvAsm, dst, RRET) : rvEmitUExtB(rvAsm, dst, RRET);
      }
      break;
      case MemoryAccessSize::HalfWord:
      {
        is_signed ? rvEmitSExtH(rvAsm, dst, RRET) : rvEmitUExtH(rvAsm, dst, RRET);
      }
      break;
      case MemoryAccessSize::Word:
      {
        if (dst.Index() != RRET.Index())
          rvAsm->MV(dst, RRET);
      }
      break;
    }
  }

  if (cycles_to_remove != 0)
  {
    Assert(rvIsValidSExtITypeImm(-cycles_to_remove));
    rvAsm->LW(RSCRATCH, PTR(&g_state.pending_ticks));
    rvAsm->ADDIW(RSCRATCH, RSCRATCH, -cycles_to_remove);
    rvAsm->SW(RSCRATCH, PTR(&g_state.pending_ticks));
  }

  // restore regs
  if (stack_size > 0)
  {
    u32 stack_offset = 0;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && rvIsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        rvAsm->LD(GPR(i), stack_offset, sp);
        stack_offset += GPR_SIZE;
      }
    }

    rvAsm->ADDI(sp, sp, stack_size);
  }

  rvEmitJmp(rvAsm, static_cast<const u8*>(code_address) + code_size);

  return static_cast<u32>(rvAsm->GetCodeBuffer().GetSizeInBytes());
}

#endif // CPU_ARCH_RISCV64
