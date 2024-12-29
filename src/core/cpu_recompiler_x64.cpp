// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_recompiler_x64.h"
#include "cpu_code_cache_private.h"
#include "cpu_core_private.h"
#include "cpu_pgxp.h"
#include "gte.h"
#include "settings.h"
#include "timing_event.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include <limits>

#ifdef CPU_ARCH_X64

#ifdef ENABLE_HOST_DISASSEMBLY
#include "Zycore/Format.h"
#include "Zycore/Status.h"
#include "Zydis/Zydis.h"
#endif

LOG_CHANNEL(Recompiler);

#define RMEMBASE cg->rbx
#define RSTATE cg->rbp

// #define PTR(x) (cg->rip + (x))
#define PTR(x) (RSTATE + (((u8*)(x)) - ((u8*)&g_state)))

// PGXP TODO: LWL etc, MFC0
// PGXP TODO: Spyro 1 level gates have issues.

static constexpr u32 FUNCTION_ALIGNMENT = 16;
static constexpr u32 BACKPATCH_JMP_SIZE = 5;

static bool IsCallerSavedRegister(u32 id);

// ABI selection
#if defined(_WIN32)

#define RWRET Xbyak::Reg32(Xbyak::Operand::EAX)
#define RWARG1 Xbyak::Reg32(Xbyak::Operand::RCX)
#define RWARG2 Xbyak::Reg32(Xbyak::Operand::RDX)
#define RWARG3 Xbyak::Reg32(Xbyak::Operand::R8D)
#define RWARG4 Xbyak::Reg32(Xbyak::Operand::R9D)
#define RXRET Xbyak::Reg64(Xbyak::Operand::RAX)
#define RXARG1 Xbyak::Reg64(Xbyak::Operand::RCX)
#define RXARG2 Xbyak::Reg64(Xbyak::Operand::RDX)
#define RXARG3 Xbyak::Reg64(Xbyak::Operand::R8)
#define RXARG4 Xbyak::Reg64(Xbyak::Operand::R9)

// on win32, we need to reserve an additional 32 bytes shadow space when calling out to C
static constexpr u32 STACK_SHADOW_SIZE = 32;

#elif defined(__linux__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__FreeBSD__)

#define RWRET Xbyak::Reg32(Xbyak::Operand::EAX)
#define RWARG1 Xbyak::Reg32(Xbyak::Operand::EDI)
#define RWARG2 Xbyak::Reg32(Xbyak::Operand::ESI)
#define RWARG3 Xbyak::Reg32(Xbyak::Operand::EDX)
#define RWARG4 Xbyak::Reg32(Xbyak::Operand::ECX)
#define RXRET Xbyak::Reg64(Xbyak::Operand::RAX)
#define RXARG1 Xbyak::Reg64(Xbyak::Operand::RDI)
#define RXARG2 Xbyak::Reg64(Xbyak::Operand::RSI)
#define RXARG3 Xbyak::Reg64(Xbyak::Operand::RDX)
#define RXARG4 Xbyak::Reg64(Xbyak::Operand::RCX)

static constexpr u32 STACK_SHADOW_SIZE = 0;

#else

#error Unknown ABI.

#endif

namespace CPU {

using namespace Xbyak;

static X64Recompiler s_instance;
Recompiler* g_compiler = &s_instance;

} // namespace CPU

bool IsCallerSavedRegister(u32 id)
{
#ifdef _WIN32
  // The x64 ABI considers the registers RAX, RCX, RDX, R8, R9, R10, R11, and XMM0-XMM5 volatile.
  return (id <= 2 || (id >= 8 && id <= 11));
#else
  // rax, rdi, rsi, rdx, rcx, r8, r9, r10, r11 are scratch registers.
  return (id <= 2 || id == 6 || id == 7 || (id >= 8 && id <= 11));
#endif
}

u32 CPU::CodeCache::EmitASMFunctions(void* code, u32 code_size)
{
  using namespace Xbyak;

#ifdef _WIN32
  // Shadow space for Win32
  constexpr u32 stack_size = 32 + 8;
#else
  // Stack still needs to be aligned
  constexpr u32 stack_size = 8;
#endif

  DebugAssert(g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler);

  CodeGenerator acg(code_size, static_cast<u8*>(code));
  CodeGenerator* cg = &acg;

  Label dispatch;
  Label exit_recompiler;

  g_enter_recompiler = reinterpret_cast<decltype(g_enter_recompiler)>(const_cast<u8*>(cg->getCurr()));
  {
    // Don't need to save registers, because we fastjmp out when execution is interrupted.
    cg->sub(cg->rsp, stack_size);

    // CPU state pointer
    cg->lea(cg->rbp, cg->qword[cg->rip + &g_state]);

    // newrec preloads fastmem base
    if (CodeCache::IsUsingFastmem())
      cg->mov(cg->rbx, cg->qword[PTR(&g_state.fastmem_base)]);

    // Fall through to event dispatcher
  }

  // check events then for frame done
  cg->align(FUNCTION_ALIGNMENT);
  g_check_events_and_dispatch = cg->getCurr();
  {
    cg->mov(RWARG1, cg->dword[PTR(&g_state.pending_ticks)]);
    cg->cmp(RWARG1, cg->dword[PTR(&g_state.downcount)]);
    cg->jl(dispatch);

    g_run_events_and_dispatch = cg->getCurr();
    cg->call(reinterpret_cast<const void*>(&TimingEvents::RunEvents));
  }

  cg->align(FUNCTION_ALIGNMENT);
  g_dispatcher = cg->getCurr();
  {
    cg->L(dispatch);

    // rcx <- s_fast_map[pc >> 16]
    cg->mov(RWARG1, cg->dword[PTR(&g_state.pc)]);
    cg->lea(RXARG2, cg->dword[PTR(g_code_lut.data())]);
    cg->mov(RWARG3, RWARG1);
    cg->shr(RWARG3, LUT_TABLE_SHIFT);
    cg->mov(RXARG2, cg->qword[RXARG2 + RXARG3 * 8]);
    cg->and_(RWARG1, (LUT_TABLE_SIZE - 1) << 2); // 0xFFFC

    // call(rcx[pc * 2]) (fast_map[pc >> 2])
    cg->jmp(cg->qword[RXARG2 + RXARG1 * 2]);
  }

  cg->align(FUNCTION_ALIGNMENT);
  g_compile_or_revalidate_block = cg->getCurr();
  {
    cg->mov(RWARG1, cg->dword[PTR(&g_state.pc)]);
    cg->call(&CompileOrRevalidateBlock);
    cg->jmp(dispatch);
  }

  cg->align(FUNCTION_ALIGNMENT);
  g_discard_and_recompile_block = cg->getCurr();
  {
    cg->mov(RWARG1, cg->dword[PTR(&g_state.pc)]);
    cg->call(&DiscardAndRecompileBlock);
    cg->jmp(dispatch);
  }

  cg->align(FUNCTION_ALIGNMENT);
  g_interpret_block = cg->getCurr();
  {
    cg->call(CodeCache::GetInterpretUncachedBlockFunction());
    cg->jmp(dispatch);
  }

  return static_cast<u32>(cg->getSize());
}

u32 CPU::CodeCache::EmitJump(void* code, const void* dst, bool flush_icache)
{
  u8* ptr = static_cast<u8*>(code);
  *(ptr++) = 0xE9; // jmp

  const ptrdiff_t disp = (reinterpret_cast<uintptr_t>(dst) - reinterpret_cast<uintptr_t>(code)) - 5;
  DebugAssert(disp >= static_cast<ptrdiff_t>(std::numeric_limits<s32>::min()) &&
              disp <= static_cast<ptrdiff_t>(std::numeric_limits<s32>::max()));

  const s32 disp32 = static_cast<s32>(disp);
  std::memcpy(ptr, &disp32, sizeof(disp32));
  return 5;
}

void CPU::CodeCache::EmitAlignmentPadding(void* dst, size_t size)
{
  // Copied from Xbyak nop(), to avoid constructing a CodeGenerator.
  static const uint8_t nopTbl[9][9] = {
    {0x90},
    {0x66, 0x90},
    {0x0F, 0x1F, 0x00},
    {0x0F, 0x1F, 0x40, 0x00},
    {0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  const size_t n = sizeof(nopTbl) / sizeof(nopTbl[0]);
  u8* dst_ptr = static_cast<u8*>(dst);
  while (size > 0)
  {
    size_t len = (std::min)(n, size);
    const uint8_t* seq = nopTbl[len - 1];
    std::memcpy(dst_ptr, seq, len);
    dst_ptr += len;
    size -= len;
  }
}

#ifdef ENABLE_HOST_DISASSEMBLY

static ZydisFormatterFunc s_old_print_address;

static ZyanStatus ZydisFormatterPrintAddressAbsolute(const ZydisFormatter* formatter, ZydisFormatterBuffer* buffer,
                                                     ZydisFormatterContext* context)
{
  using namespace CPU;

  ZyanU64 address;
  ZYAN_CHECK(ZydisCalcAbsoluteAddress(context->instruction, context->operand, context->runtime_address, &address));

  char buf[128];
  u32 len = 0;

#define A(x) static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(x))

  if (address >= A(Bus::g_ram) && address < A(Bus::g_ram + Bus::g_ram_size))
  {
    len = snprintf(buf, sizeof(buf), "g_ram+0x%08X", static_cast<u32>(address - A(Bus::g_ram)));
  }
  else if (address >= A(&g_state.regs) &&
           address < A(reinterpret_cast<const u8*>(&g_state.regs) + sizeof(CPU::Registers)))
  {
    len = snprintf(buf, sizeof(buf), "g_state.regs.%s",
                   GetRegName(static_cast<CPU::Reg>(((address - A(&g_state.regs.r[0])) / 4u))));
  }
  else if (address >= A(&g_state.cop0_regs) &&
           address < A(reinterpret_cast<const u8*>(&g_state.cop0_regs) + sizeof(CPU::Cop0Registers)))
  {
    for (const DebuggerRegisterListEntry& rle : g_debugger_register_list)
    {
      if (address == static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(rle.value_ptr)))
      {
        len = snprintf(buf, sizeof(buf), "g_state.cop0_regs.%s", rle.name);
        break;
      }
    }
  }
  else if (address >= A(&g_state.gte_regs) &&
           address < A(reinterpret_cast<const u8*>(&g_state.gte_regs) + sizeof(GTE::Regs)))
  {
    for (const DebuggerRegisterListEntry& rle : g_debugger_register_list)
    {
      if (address == static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(rle.value_ptr)))
      {
        len = snprintf(buf, sizeof(buf), "g_state.gte_regs.%s", rle.name);
        break;
      }
    }
  }
  else if (address == A(&g_state.load_delay_reg))
  {
    len = snprintf(buf, sizeof(buf), "g_state.load_delay_reg");
  }
  else if (address == A(&g_state.next_load_delay_reg))
  {
    len = snprintf(buf, sizeof(buf), "g_state.next_load_delay_reg");
  }
  else if (address == A(&g_state.load_delay_value))
  {
    len = snprintf(buf, sizeof(buf), "g_state.load_delay_value");
  }
  else if (address == A(&g_state.next_load_delay_value))
  {
    len = snprintf(buf, sizeof(buf), "g_state.next_load_delay_value");
  }
  else if (address == A(&g_state.pending_ticks))
  {
    len = snprintf(buf, sizeof(buf), "g_state.pending_ticks");
  }
  else if (address == A(&g_state.downcount))
  {
    len = snprintf(buf, sizeof(buf), "g_state.downcount");
  }

#undef A

  if (len > 0)
  {
    ZYAN_CHECK(ZydisFormatterBufferAppend(buffer, ZYDIS_TOKEN_SYMBOL));
    ZyanString* string;
    ZYAN_CHECK(ZydisFormatterBufferGetString(buffer, &string));
    return ZyanStringAppendFormat(string, "&%s", buf);
  }

  return s_old_print_address(formatter, buffer, context);
}

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
  ZydisDecoder disas_decoder;
  ZydisFormatter disas_formatter;
  ZydisDecodedInstruction disas_instruction;
  ZydisDecodedOperand disas_operands[ZYDIS_MAX_OPERAND_COUNT];
  ZydisDecoderInit(&disas_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&disas_formatter, ZYDIS_FORMATTER_STYLE_INTEL);
  s_old_print_address = (ZydisFormatterFunc)&ZydisFormatterPrintAddressAbsolute;
  ZydisFormatterSetHook(&disas_formatter, ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS, (const void**)&s_old_print_address);

  const u8* ptr = static_cast<const u8*>(start);
  TinyString hex;
  ZyanUSize remaining = size;
  while (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&disas_decoder, ptr, remaining, &disas_instruction, disas_operands)))
  {
    char buffer[256];
    if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&disas_formatter, &disas_instruction, disas_operands,
                                                     ZYDIS_MAX_OPERAND_COUNT, buffer, sizeof(buffer),
                                                     static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(ptr)), nullptr)))
    {
      hex.clear();
      for (u32 i = 0; i < 10; i++)
      {
        if (i < disas_instruction.length)
          hex.append_format(" {:02X}", ptr[i]);
        else
          hex.append("   ");
      }
      DEBUG_LOG("  {:016X} {} {}", static_cast<u64>(reinterpret_cast<uintptr_t>(ptr)), hex, buffer);
    }

    ptr += disas_instruction.length;
    remaining -= disas_instruction.length;
  }
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
  ZydisDecoder disas_decoder;
  ZydisDecodedInstruction disas_instruction;
  ZydisDecoderContext disas_context;
  ZydisDecoderInit(&disas_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

  const u8* ptr = static_cast<const u8*>(start);
  ZyanUSize remaining = size;
  u32 inst_count = 0;
  while (
    ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&disas_decoder, &disas_context, ptr, remaining, &disas_instruction)))
  {
    ptr += disas_instruction.length;
    remaining -= disas_instruction.length;
    inst_count++;
  }

  return inst_count;
}

#else

void CPU::CodeCache::DisassembleAndLogHostCode(const void* start, u32 size)
{
  ERROR_LOG("Not compiled with ENABLE_HOST_DISASSEMBLY.");
}

u32 CPU::CodeCache::GetHostInstructionCount(const void* start, u32 size)
{
  ERROR_LOG("Not compiled with ENABLE_HOST_DISASSEMBLY.");
  return 0;
}

#endif // ENABLE_HOST_DISASSEMBLY

CPU::X64Recompiler::X64Recompiler() = default;

CPU::X64Recompiler::~X64Recompiler() = default;

void CPU::X64Recompiler::Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space, u8* far_code_buffer,
                               u32 far_code_space)
{
  Recompiler::Reset(block, code_buffer, code_buffer_space, far_code_buffer, far_code_space);

  // TODO: don't recreate this every time..
  DebugAssert(!m_emitter && !m_far_emitter && !cg);
  m_emitter = std::make_unique<Xbyak::CodeGenerator>(code_buffer_space, code_buffer);
  m_far_emitter = std::make_unique<Xbyak::CodeGenerator>(far_code_space, far_code_buffer);
  cg = m_emitter.get();

  // Need to wipe it out so it's correct when toggling fastmem.
  m_host_regs = {};

  const u32 membase_idx = CodeCache::IsUsingFastmem() ? static_cast<u32>(RMEMBASE.getIdx()) : NUM_HOST_REGS;
  const u32 cpu_idx = static_cast<u32>(RSTATE.getIdx());
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];

    if (i == static_cast<u32>(RWRET.getIdx()) || i == static_cast<u32>(RWARG1.getIdx()) ||
        i == static_cast<u32>(RWARG2.getIdx()) || i == static_cast<u32>(RWARG3.getIdx()) ||
        i == static_cast<u32>(cg->rsp.getIdx()) || i == cpu_idx || i == membase_idx ||
        i == static_cast<u32>(cg->ecx.getIdx()) /* keep ecx free for shifts, maybe use BMI? */)
    {
      continue;
    }

    ra.flags = HR_USABLE | (IsCallerSavedRegister(i) ? 0 : HR_CALLEE_SAVED);
  }
}

void CPU::X64Recompiler::SwitchToFarCode(bool emit_jump, void (Xbyak::CodeGenerator::*jump_op)(const void*))
{
  DebugAssert(cg == m_emitter.get());
  if (emit_jump)
  {
    const void* fcptr = m_far_emitter->getCurr<const void*>();
    (jump_op) ? (cg->*jump_op)(fcptr) : cg->jmp(fcptr);
  }
  cg = m_far_emitter.get();
}

void CPU::X64Recompiler::SwitchToNearCode(bool emit_jump, void (Xbyak::CodeGenerator::*jump_op)(const void*))
{
  DebugAssert(cg == m_far_emitter.get());
  if (emit_jump)
  {
    const void* fcptr = m_emitter->getCurr<const void*>();
    (jump_op) ? (cg->*jump_op)(fcptr) : cg->jmp(fcptr);
  }
  cg = m_emitter.get();
}

void CPU::X64Recompiler::BeginBlock()
{
  Recompiler::BeginBlock();

#if 0
  if (m_block->pc == 0xBFC06F0C)
  {
    //__debugbreak();
    cg->db(0xcc);
  }
#endif

#if 0
  cg->nop();
  cg->mov(RWARG1, m_block->pc);
  cg->nop();
#endif
}

void CPU::X64Recompiler::GenerateBlockProtectCheck(const u8* ram_ptr, const u8* shadow_ptr, u32 size)
{
  // store it first to reduce code size, because we can offset
  cg->mov(RXARG1, static_cast<size_t>(reinterpret_cast<uintptr_t>(ram_ptr)));
  cg->mov(RXARG2, static_cast<size_t>(reinterpret_cast<uintptr_t>(shadow_ptr)));

  bool first = true;
  u32 offset = 0;
  while (size >= 16)
  {
    const Xbyak::Xmm& dst = first ? cg->xmm0 : cg->xmm1;
    cg->movups(dst, cg->xword[RXARG1 + offset]);
    cg->pcmpeqd(dst, cg->xword[RXARG2 + offset]);
    if (!first)
      cg->pand(cg->xmm0, dst);
    else
      first = false;

    offset += 16;
    size -= 16;
  }

  // TODO: better codegen for 16 byte aligned blocks
  if (!first)
  {
    cg->movmskps(cg->eax, cg->xmm0);
    cg->cmp(cg->eax, 0xf);
    cg->jne(CodeCache::g_discard_and_recompile_block);
  }

  while (size >= 8)
  {
    cg->mov(RXARG3, cg->qword[RXARG1 + offset]);
    cg->cmp(RXARG3, cg->qword[RXARG2 + offset]);
    cg->jne(CodeCache::g_discard_and_recompile_block);
    offset += 8;
    size -= 8;
  }

  while (size >= 4)
  {
    cg->mov(RWARG3, cg->dword[RXARG1 + offset]);
    cg->cmp(RWARG3, cg->dword[RXARG2 + offset]);
    cg->jne(CodeCache::g_discard_and_recompile_block);
    offset += 4;
    size -= 4;
  }

  DebugAssert(size == 0);
}

void CPU::X64Recompiler::GenerateICacheCheckAndUpdate()
{
  if (!m_block->HasFlag(CodeCache::BlockFlags::IsUsingICache))
  {
    if (m_block->HasFlag(CodeCache::BlockFlags::NeedsDynamicFetchTicks))
    {
      cg->mov(cg->eax, m_block->size);
      cg->mul(cg->dword[cg->rip + GetFetchMemoryAccessTimePtr()]);
      cg->add(cg->dword[PTR(&g_state.pending_ticks)], cg->eax);
    }
    else
    {
      cg->add(cg->dword[PTR(&g_state.pending_ticks)], static_cast<u32>(m_block->uncached_fetch_ticks));
    }
  }
  else if (m_block->icache_line_count > 0)
  {
    // RAM to ROM is not contiguous, therefore the cost will be the same across the entire block.
    VirtualMemoryAddress current_pc = m_block->pc & ICACHE_TAG_ADDRESS_MASK;
    const TickCount fill_ticks = GetICacheFillTicks(current_pc);
    if (fill_ticks <= 0)
      return;

    cg->lea(RXARG1, cg->dword[PTR(&g_state.icache_tags)]);
    cg->xor_(RWARG2, RWARG2);
    cg->mov(RWARG4, fill_ticks);

    // TODO: Vectorize this...
    for (u32 i = 0; i < m_block->icache_line_count; i++, current_pc += ICACHE_LINE_SIZE)
    {
      const VirtualMemoryAddress tag = GetICacheTagForAddress(current_pc);

      const u32 line = GetICacheLine(current_pc);
      const u32 offset = (line * sizeof(u32));

      cg->xor_(RWARG3, RWARG3);
      cg->cmp(cg->dword[RXARG1 + offset], tag);
      cg->mov(cg->dword[RXARG1 + offset], tag);
      cg->cmovne(RWARG3, RWARG4);
      cg->add(RWARG2, RWARG3);
    }

    cg->add(cg->dword[PTR(&g_state.pending_ticks)], RWARG2);
  }
}

void CPU::X64Recompiler::GenerateCall(const void* func, s32 arg1reg /*= -1*/, s32 arg2reg /*= -1*/,
                                      s32 arg3reg /*= -1*/)
{
  if (arg1reg >= 0 && arg1reg != static_cast<s32>(RXARG1.getIdx()))
    cg->mov(RXARG1, Reg64(arg1reg));
  if (arg2reg >= 0 && arg2reg != static_cast<s32>(RXARG2.getIdx()))
    cg->mov(RXARG2, Reg64(arg2reg));
  if (arg3reg >= 0 && arg3reg != static_cast<s32>(RXARG3.getIdx()))
    cg->mov(RXARG3, Reg64(arg3reg));
  cg->call(func);
}

void CPU::X64Recompiler::EndBlock(const std::optional<u32>& newpc, bool do_event_test)
{
  if (newpc.has_value())
  {
    if (m_dirty_pc || m_compiler_pc != newpc)
      cg->mov(cg->dword[PTR(&g_state.pc)], newpc.value());
  }
  m_dirty_pc = false;

  // flush regs
  Flush(FLUSH_END_BLOCK);
  EndAndLinkBlock(newpc, do_event_test, false);
}

void CPU::X64Recompiler::EndBlockWithException(Exception excode)
{
  // flush regs, but not pc, it's going to get overwritten
  // flush cycles because of the GTE instruction stuff...
  Flush(FLUSH_END_BLOCK | FLUSH_FOR_EXCEPTION | FLUSH_FOR_C_CALL);

  // TODO: flush load delay
  // TODO: break for pcdrv

  cg->mov(RWARG1, Cop0Registers::CAUSE::MakeValueForException(excode, m_current_instruction_branch_delay_slot, false,
                                                              inst->cop.cop_n));
  cg->mov(RWARG2, m_current_instruction_pc);
  cg->call(static_cast<void (*)(u32, u32)>(&CPU::RaiseException));
  m_dirty_pc = false;

  EndAndLinkBlock(std::nullopt, true, false);
}

void CPU::X64Recompiler::EndAndLinkBlock(const std::optional<u32>& newpc, bool do_event_test, bool force_run_events)
{
  // event test
  // pc should've been flushed
  DebugAssert(!m_dirty_pc && !m_block_ended);
  m_block_ended = true;

  // TODO: try extracting this to a function

  // save cycles for event test
  const TickCount cycles = std::exchange(m_cycles, 0);

  // fast path when not doing an event test
  if (!do_event_test && m_gte_done_cycle <= cycles)
  {
    if (cycles == 1)
      cg->inc(cg->dword[PTR(&g_state.pending_ticks)]);
    else if (cycles > 0)
      cg->add(cg->dword[PTR(&g_state.pending_ticks)], cycles);

    if (force_run_events)
    {
      cg->jmp(CodeCache::g_run_events_and_dispatch);
      return;
    }
  }
  else
  {
    // pending_ticks += cycles
    // if (pending_ticks >= downcount) { dispatch_event(); }
    if (do_event_test || cycles > 0 || m_gte_done_cycle > cycles)
      cg->mov(RWARG1, cg->dword[PTR(&g_state.pending_ticks)]);
    if (cycles > 0)
      cg->add(RWARG1, cycles);
    if (m_gte_done_cycle > cycles)
    {
      cg->mov(RWARG2, RWARG1);
      ((m_gte_done_cycle - cycles) == 1) ? cg->inc(RWARG2) : cg->add(RWARG2, m_gte_done_cycle - cycles);
      cg->mov(cg->dword[PTR(&g_state.gte_completion_tick)], RWARG2);
    }
    if (do_event_test)
      cg->cmp(RWARG1, cg->dword[PTR(&g_state.downcount)]);
    if (cycles > 0)
      cg->mov(cg->dword[PTR(&g_state.pending_ticks)], RWARG1);
    if (do_event_test)
      cg->jge(CodeCache::g_run_events_and_dispatch);
  }

  // jump to dispatcher or next block
  if (!newpc.has_value())
  {
    cg->jmp(CodeCache::g_dispatcher);
  }
  else
  {
    const void* target = (newpc.value() == m_block->pc) ?
                           CodeCache::CreateSelfBlockLink(m_block, cg->getCurr<void*>(), cg->getCode()) :
                           CodeCache::CreateBlockLink(m_block, cg->getCurr<void*>(), newpc.value());
    cg->jmp(target, CodeGenerator::T_NEAR);
  }
}

const void* CPU::X64Recompiler::EndCompile(u32* code_size, u32* far_code_size)
{
  const void* code = m_emitter->getCode();
  *code_size = static_cast<u32>(m_emitter->getSize());
  *far_code_size = static_cast<u32>(m_far_emitter->getSize());
  cg = nullptr;
  m_far_emitter.reset();
  m_emitter.reset();
  return code;
}

const void* CPU::X64Recompiler::GetCurrentCodePointer()
{
  return cg->getCurr();
}

const char* CPU::X64Recompiler::GetHostRegName(u32 reg) const
{
  static constexpr std::array<const char*, 16> reg64_names = {
    {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"}};
  return (reg < reg64_names.size()) ? reg64_names[reg] : "UNKNOWN";
}

void CPU::X64Recompiler::LoadHostRegWithConstant(u32 reg, u32 val)
{
  cg->mov(Reg32(reg), val);
}

void CPU::X64Recompiler::LoadHostRegFromCPUPointer(u32 reg, const void* ptr)
{
  cg->mov(Reg32(reg), cg->dword[PTR(ptr)]);
}

void CPU::X64Recompiler::StoreHostRegToCPUPointer(u32 reg, const void* ptr)
{
  cg->mov(cg->dword[PTR(ptr)], Reg32(reg));
}

void CPU::X64Recompiler::StoreConstantToCPUPointer(u32 val, const void* ptr)
{
  cg->mov(cg->dword[PTR(ptr)], val);
}

void CPU::X64Recompiler::CopyHostReg(u32 dst, u32 src)
{
  if (src != dst)
    cg->mov(Reg32(dst), Reg32(src));
}

Xbyak::Address CPU::X64Recompiler::MipsPtr(Reg r) const
{
  DebugAssert(r < Reg::count);
  return cg->dword[PTR(&g_state.regs.r[static_cast<u32>(r)])];
}

Xbyak::Reg32 CPU::X64Recompiler::CFGetRegD(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_d);
  return Reg32(cf.host_d);
}

Xbyak::Reg32 CPU::X64Recompiler::CFGetRegS(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_s);
  return Reg32(cf.host_s);
}

Xbyak::Reg32 CPU::X64Recompiler::CFGetRegT(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_t);
  return Reg32(cf.host_t);
}

Xbyak::Reg32 CPU::X64Recompiler::CFGetRegLO(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_lo);
  return Reg32(cf.host_lo);
}

Xbyak::Reg32 CPU::X64Recompiler::CFGetRegHI(CompileFlags cf) const
{
  DebugAssert(cf.valid_host_hi);
  return Reg32(cf.host_hi);
}

Xbyak::Reg32 CPU::X64Recompiler::MoveSToD(CompileFlags cf)
{
  DebugAssert(cf.valid_host_d);
  DebugAssert(!cf.valid_host_t || cf.host_t != cf.host_d);

  const Reg32 rd = CFGetRegD(cf);
  MoveSToReg(rd, cf);

  return rd;
}

Xbyak::Reg32 CPU::X64Recompiler::MoveSToT(CompileFlags cf)
{
  DebugAssert(cf.valid_host_t);

  const Reg32 rt = CFGetRegT(cf);
  if (cf.valid_host_s)
  {
    const Reg32 rs = CFGetRegS(cf);
    if (rt != rs)
      cg->mov(rt, rs);
  }
  else if (cf.const_s)
  {
    if (const u32 cv = GetConstantRegU32(cf.MipsS()); cv != 0)
      cg->mov(rt, cv);
    else
      cg->xor_(rt, rt);
  }
  else
  {
    cg->mov(rt, MipsPtr(cf.MipsS()));
  }

  return rt;
}

Xbyak::Reg32 CPU::X64Recompiler::MoveTToD(CompileFlags cf)
{
  DebugAssert(cf.valid_host_d);
  DebugAssert(!cf.valid_host_s || cf.host_s != cf.host_d);

  const Reg32 rd = CFGetRegD(cf);
  MoveTToReg(rd, cf);
  return rd;
}

void CPU::X64Recompiler::MoveSToReg(const Xbyak::Reg32& dst, CompileFlags cf)
{
  if (cf.valid_host_s)
  {
    if (cf.host_s != static_cast<u32>(dst.getIdx()))
      cg->mov(dst, Reg32(cf.host_s));
  }
  else if (cf.const_s)
  {
    const u32 cv = GetConstantRegU32(cf.MipsS());
    if (cv == 0)
      cg->xor_(dst, dst);
    else
      cg->mov(dst, cv);
  }
  else
  {
    cg->mov(dst, cg->dword[PTR(&g_state.regs.r[cf.mips_s])]);
  }
}

void CPU::X64Recompiler::MoveTToReg(const Xbyak::Reg32& dst, CompileFlags cf)
{
  if (cf.valid_host_t)
  {
    if (cf.host_t != static_cast<u32>(dst.getIdx()))
      cg->mov(dst, Reg32(cf.host_t));
  }
  else if (cf.const_t)
  {
    const u32 cv = GetConstantRegU32(cf.MipsT());
    if (cv == 0)
      cg->xor_(dst, dst);
    else
      cg->mov(dst, cv);
  }
  else
  {
    cg->mov(dst, cg->dword[PTR(&g_state.regs.r[cf.mips_t])]);
  }
}

void CPU::X64Recompiler::MoveMIPSRegToReg(const Xbyak::Reg32& dst, Reg reg)
{
  DebugAssert(reg < Reg::count);
  if (const std::optional<u32> hreg = CheckHostReg(0, Recompiler::HR_TYPE_CPU_REG, reg))
    cg->mov(dst, Reg32(hreg.value()));
  else if (HasConstantReg(reg))
    cg->mov(dst, GetConstantRegU32(reg));
  else
    cg->mov(dst, MipsPtr(reg));
}

void CPU::X64Recompiler::GeneratePGXPCallWithMIPSRegs(const void* func, u32 arg1val, Reg arg2reg /* = Reg::count */,
                                                      Reg arg3reg /* = Reg::count */)
{
  DebugAssert(g_settings.gpu_pgxp_enable);

  Flush(FLUSH_FOR_C_CALL);

  if (arg2reg != Reg::count)
    MoveMIPSRegToReg(RWARG2, arg2reg);
  if (arg3reg != Reg::count)
    MoveMIPSRegToReg(RWARG3, arg3reg);

  cg->mov(RWARG1, arg1val);
  cg->call(func);
}

void CPU::X64Recompiler::Flush(u32 flags)
{
  Recompiler::Flush(flags);

  if (flags & FLUSH_PC && m_dirty_pc)
  {
    cg->mov(cg->dword[PTR(&g_state.pc)], m_compiler_pc);
    m_dirty_pc = false;
  }

  if (flags & FLUSH_INSTRUCTION_BITS)
  {
    cg->mov(cg->dword[PTR(&g_state.current_instruction.bits)], inst->bits);
    cg->mov(cg->dword[PTR(&g_state.current_instruction_pc)], m_current_instruction_pc);
    cg->mov(cg->byte[PTR(&g_state.current_instruction_in_branch_delay_slot)], m_current_instruction_branch_delay_slot);
  }

  if (flags & FLUSH_LOAD_DELAY_FROM_STATE && m_load_delay_dirty)
  {
    // This sucks :(
    // TODO: make it a function?
    cg->movzx(RWARG1, cg->byte[PTR(&g_state.load_delay_reg)]);
    cg->mov(RWARG2, cg->dword[PTR(&g_state.load_delay_value)]);
    cg->mov(cg->dword[PTR(&g_state.regs.r[0]) + RXARG1 * 4], RWARG2);
    cg->mov(cg->byte[PTR(&g_state.load_delay_reg)], static_cast<u8>(Reg::count));
    m_load_delay_dirty = false;
  }

  if (flags & FLUSH_LOAD_DELAY && m_load_delay_register != Reg::count)
  {
    if (m_load_delay_value_register != NUM_HOST_REGS)
      FreeHostReg(m_load_delay_value_register);

    cg->mov(cg->byte[PTR(&g_state.load_delay_reg)], static_cast<u8>(m_load_delay_register));
    m_load_delay_register = Reg::count;
    m_load_delay_dirty = true;
  }

  if (flags & FLUSH_GTE_STALL_FROM_STATE && m_dirty_gte_done_cycle)
  {
    // May as well flush cycles while we're here.
    // GTE spanning blocks is very rare, we _could_ disable this for speed.
    cg->mov(RWARG1, cg->dword[PTR(&g_state.pending_ticks)]);
    cg->mov(RWARG2, cg->dword[PTR(&g_state.gte_completion_tick)]);
    if (m_cycles > 0)
    {
      (m_cycles == 1) ? cg->inc(RWARG1) : cg->add(RWARG1, m_cycles);
      m_cycles = 0;
    }
    cg->cmp(RWARG2, RWARG1);
    cg->cmova(RWARG1, RWARG2);
    cg->mov(cg->dword[PTR(&g_state.pending_ticks)], RWARG1);
    m_dirty_gte_done_cycle = false;
  }

  if (flags & FLUSH_GTE_DONE_CYCLE && m_gte_done_cycle > m_cycles)
  {
    cg->mov(RWARG1, cg->dword[PTR(&g_state.pending_ticks)]);

    // update cycles at the same time
    if (flags & FLUSH_CYCLES && m_cycles > 0)
    {
      (m_cycles == 1) ? cg->inc(RWARG1) : cg->add(RWARG1, m_cycles);
      cg->mov(cg->dword[PTR(&g_state.pending_ticks)], RWARG1);
      m_gte_done_cycle -= m_cycles;
      m_cycles = 0;
    }

    (m_gte_done_cycle == 1) ? cg->inc(RWARG1) : cg->add(RWARG1, m_gte_done_cycle);
    cg->mov(cg->dword[PTR(&g_state.gte_completion_tick)], RWARG1);
    m_gte_done_cycle = 0;
    m_dirty_gte_done_cycle = true;
  }

  if (flags & FLUSH_CYCLES && m_cycles > 0)
  {
    (m_cycles == 1) ? cg->inc(cg->dword[PTR(&g_state.pending_ticks)]) :
                      cg->add(cg->dword[PTR(&g_state.pending_ticks)], m_cycles);
    m_gte_done_cycle = std::max<TickCount>(m_gte_done_cycle - m_cycles, 0);
    m_cycles = 0;
  }
}

void CPU::X64Recompiler::Compile_Fallback()
{
  WARNING_LOG("Compiling instruction fallback at PC=0x{:08X}, instruction=0x{:08X}", m_current_instruction_pc,
              inst->bits);

  Flush(FLUSH_FOR_INTERPRETER);

  cg->call(&CPU::RecompilerThunks::InterpretInstruction);

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
}

void CPU::X64Recompiler::CheckBranchTarget(const Xbyak::Reg32& pcreg)
{
  if (!g_settings.cpu_recompiler_memory_exceptions)
    return;

  cg->test(pcreg, 0x3);
  SwitchToFarCode(true, &CodeGenerator::jnz);

  BackupHostState();
  EndBlockWithException(Exception::AdEL);

  RestoreHostState();
  SwitchToNearCode(false);
}

void CPU::X64Recompiler::Compile_jr(CompileFlags cf)
{
  if (!cf.valid_host_s)
    cg->mov(RWARG1, MipsPtr(cf.MipsS()));

  const Reg32 pcreg = cf.valid_host_s ? CFGetRegS(cf) : RWARG1;
  CheckBranchTarget(pcreg);

  cg->mov(cg->dword[PTR(&g_state.pc)], pcreg);

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::X64Recompiler::Compile_jalr(CompileFlags cf)
{
  if (!cf.valid_host_s)
    cg->mov(RWARG1, MipsPtr(cf.MipsS()));

  const Reg32 pcreg = cf.valid_host_s ? CFGetRegS(cf) : RWARG1;

  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetBranchReturnAddress(cf));

  CheckBranchTarget(pcreg);
  cg->mov(cg->dword[PTR(&g_state.pc)], pcreg);

  CompileBranchDelaySlot(false);
  EndBlock(std::nullopt, true);
}

void CPU::X64Recompiler::Compile_bxx(CompileFlags cf, BranchCondition cond)
{
  const u32 taken_pc = GetConditionalBranchTarget(cf);

  Flush(FLUSH_FOR_BRANCH);

  DebugAssert(cf.valid_host_s);

  // MipsT() here should equal zero for zero branches.
  DebugAssert(cond == BranchCondition::Equal || cond == BranchCondition::NotEqual || cf.MipsT() == Reg::zero);

  // TODO: Swap this back to near once instructions don't blow up
  constexpr CodeGenerator::LabelType type = CodeGenerator::T_NEAR;
  Label taken;
  switch (cond)
  {
    case BranchCondition::Equal:
    case BranchCondition::NotEqual:
    {
      // we should always have S, maybe not T
      // TODO: if it's zero, we can just do test rs, rs
      if (cf.valid_host_t)
        cg->cmp(CFGetRegS(cf), CFGetRegT(cf));
      else if (cf.const_t)
        cg->cmp(CFGetRegS(cf), GetConstantRegU32(cf.MipsT()));
      else
        cg->cmp(CFGetRegS(cf), MipsPtr(cf.MipsT()));

      (cond == BranchCondition::Equal) ? cg->je(taken, type) : cg->jne(taken, type);
    }
    break;

    case BranchCondition::GreaterThanZero:
    {
      cg->cmp(CFGetRegS(cf), 0);
      cg->jg(taken, type);
    }
    break;

    case BranchCondition::GreaterEqualZero:
    {
      cg->test(CFGetRegS(cf), CFGetRegS(cf));
      cg->jns(taken, type);
    }
    break;

    case BranchCondition::LessThanZero:
    {
      cg->test(CFGetRegS(cf), CFGetRegS(cf));
      cg->js(taken, type);
    }
    break;

    case BranchCondition::LessEqualZero:
    {
      cg->cmp(CFGetRegS(cf), 0);
      cg->jle(taken, type);
    }
    break;
  }

  BackupHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(m_compiler_pc, true);

  cg->L(taken);

  RestoreHostState();
  if (!cf.delay_slot_swapped)
    CompileBranchDelaySlot();

  EndBlock(taken_pc, true);
}

void CPU::X64Recompiler::Compile_addi(CompileFlags cf)
{
  const Reg32 rt = MoveSToT(cf);
  if (const u32 imm = inst->i.imm_sext32(); imm != 0)
  {
    cg->add(rt, imm);
    if (g_settings.cpu_recompiler_memory_exceptions)
    {
      DebugAssert(cf.valid_host_t);
      TestOverflow(rt);
    }
  }
}

void CPU::X64Recompiler::Compile_addiu(CompileFlags cf)
{
  const Reg32 rt = MoveSToT(cf);
  if (const u32 imm = inst->i.imm_sext32(); imm != 0)
    cg->add(rt, imm);
}

void CPU::X64Recompiler::Compile_slti(CompileFlags cf)
{
  Compile_slti(cf, true);
}

void CPU::X64Recompiler::Compile_sltiu(CompileFlags cf)
{
  Compile_slti(cf, false);
}

void CPU::X64Recompiler::Compile_slti(CompileFlags cf, bool sign)
{
  const Reg32 rt = cf.valid_host_t ? CFGetRegT(cf) : RWARG1;

  // Case where T == S, can't use xor because it changes flags
  if (!cf.valid_host_t || !cf.valid_host_s || cf.host_t != cf.host_s)
    cg->xor_(rt, rt);

  if (cf.valid_host_s)
    cg->cmp(CFGetRegS(cf), inst->i.imm_sext32());
  else
    cg->cmp(MipsPtr(cf.MipsS()), inst->i.imm_sext32());

  if (cf.valid_host_t && cf.valid_host_s && cf.host_t == cf.host_s)
    cg->mov(rt, 0);

  sign ? cg->setl(rt.cvt8()) : cg->setb(rt.cvt8());

  if (!cf.valid_host_t)
    cg->mov(MipsPtr(cf.MipsT()), rt);
}

void CPU::X64Recompiler::Compile_andi(CompileFlags cf)
{
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
  {
    const Reg32 rt = MoveSToT(cf);
    cg->and_(rt, imm);
  }
  else
  {
    const Reg32 rt = CFGetRegT(cf);
    cg->xor_(rt, rt);
  }
}

void CPU::X64Recompiler::Compile_ori(CompileFlags cf)
{
  const Reg32 rt = MoveSToT(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    cg->or_(rt, imm);
}

void CPU::X64Recompiler::Compile_xori(CompileFlags cf)
{
  const Reg32 rt = MoveSToT(cf);
  if (const u32 imm = inst->i.imm_zext32(); imm != 0)
    cg->xor_(rt, imm);
}

void CPU::X64Recompiler::Compile_sll(CompileFlags cf)
{
  const Reg32 rd = MoveTToD(cf);
  if (inst->r.shamt > 0)
    cg->shl(rd, inst->r.shamt);
}

void CPU::X64Recompiler::Compile_srl(CompileFlags cf)
{
  const Reg32 rd = MoveTToD(cf);
  if (inst->r.shamt > 0)
    cg->shr(rd, inst->r.shamt);
}

void CPU::X64Recompiler::Compile_sra(CompileFlags cf)
{
  const Reg32 rd = MoveTToD(cf);
  if (inst->r.shamt > 0)
    cg->sar(rd, inst->r.shamt);
}

void CPU::X64Recompiler::Compile_variable_shift(CompileFlags cf,
                                                void (Xbyak::CodeGenerator::*op)(const Xbyak::Operand&,
                                                                                 const Xbyak::Reg8&),
                                                void (Xbyak::CodeGenerator::*op_const)(const Xbyak::Operand&, int))
{
  const Reg32 rd = CFGetRegD(cf);
  if (!cf.const_s)
  {
    MoveSToReg(cg->ecx, cf);
    MoveTToReg(rd, cf);
    (cg->*op)(rd, cg->cl);
  }
  else
  {
    MoveTToReg(rd, cf);
    (cg->*op_const)(rd, GetConstantRegU32(cf.MipsS()));
  }
}

void CPU::X64Recompiler::Compile_sllv(CompileFlags cf)
{
  Compile_variable_shift(cf, &CodeGenerator::shl, &CodeGenerator::shl);
}

void CPU::X64Recompiler::Compile_srlv(CompileFlags cf)
{
  Compile_variable_shift(cf, &CodeGenerator::shr, &CodeGenerator::shr);
}

void CPU::X64Recompiler::Compile_srav(CompileFlags cf)
{
  Compile_variable_shift(cf, &CodeGenerator::sar, &CodeGenerator::sar);
}

void CPU::X64Recompiler::Compile_mult(CompileFlags cf, bool sign)
{
  // RAX/RDX shouldn't be allocatable..
  DebugAssert(!(m_host_regs[Xbyak::Operand::RAX].flags & HR_USABLE) &&
              !(m_host_regs[Xbyak::Operand::RDX].flags & HR_USABLE));

  MoveSToReg(cg->eax, cf);
  if (cf.valid_host_t)
  {
    sign ? cg->imul(CFGetRegT(cf)) : cg->mul(CFGetRegT(cf));
  }
  else if (cf.const_t)
  {
    cg->mov(cg->edx, GetConstantRegU32(cf.MipsT()));
    sign ? cg->imul(cg->edx) : cg->mul(cg->edx);
  }
  else
  {
    sign ? cg->imul(MipsPtr(cf.MipsT())) : cg->mul(MipsPtr(cf.MipsT()));
  }

  // TODO: skip writeback if it's not needed
  if (cf.valid_host_lo)
    cg->mov(CFGetRegLO(cf), cg->eax);
  else
    cg->mov(MipsPtr(Reg::lo), cg->eax);
  if (cf.valid_host_lo)
    cg->mov(CFGetRegHI(cf), cg->edx);
  else
    cg->mov(MipsPtr(Reg::hi), cg->edx);
}

void CPU::X64Recompiler::Compile_mult(CompileFlags cf)
{
  Compile_mult(cf, true);
}

void CPU::X64Recompiler::Compile_multu(CompileFlags cf)
{
  Compile_mult(cf, false);
}

void CPU::X64Recompiler::Compile_div(CompileFlags cf)
{
  // not supported without registers for now..
  DebugAssert(cf.valid_host_lo && cf.valid_host_hi);

  const Reg32 rt = cf.valid_host_t ? CFGetRegT(cf) : cg->ecx;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const Reg32 rlo = CFGetRegLO(cf);
  const Reg32 rhi = CFGetRegHI(cf);

  MoveSToReg(cg->eax, cf);
  cg->cdq();

  Label done;
  Label not_divide_by_zero;
  cg->test(rt, rt);
  cg->jnz(not_divide_by_zero, CodeGenerator::T_SHORT);
  cg->test(cg->eax, cg->eax);
  cg->mov(rhi, cg->eax); // hi = num
  cg->mov(rlo, 1);
  cg->mov(cg->eax, static_cast<u32>(-1));
  cg->cmovns(rlo, cg->eax); // lo = s >= 0 ? -1 : 1
  cg->jmp(done, CodeGenerator::T_SHORT);

  cg->L(not_divide_by_zero);
  Label not_unrepresentable;
  cg->cmp(cg->eax, 0x80000000u);
  cg->jne(not_unrepresentable, CodeGenerator::T_SHORT);
  cg->cmp(rt, static_cast<u32>(-1));
  cg->jne(not_unrepresentable, CodeGenerator::T_SHORT);

  cg->mov(rlo, 0x80000000u);
  cg->xor_(rhi, rhi);
  cg->jmp(done, CodeGenerator::T_SHORT);

  cg->L(not_unrepresentable);

  cg->idiv(rt);
  cg->mov(rlo, cg->eax);
  cg->mov(rhi, cg->edx);

  cg->L(done);
}

void CPU::X64Recompiler::Compile_divu(CompileFlags cf)
{
  // not supported without registers for now..
  DebugAssert(cf.valid_host_lo && cf.valid_host_hi);

  const Reg32 rt = cf.valid_host_t ? CFGetRegT(cf) : cg->ecx;
  if (!cf.valid_host_t)
    MoveTToReg(rt, cf);

  const Reg32 rlo = CFGetRegLO(cf);
  const Reg32 rhi = CFGetRegHI(cf);

  MoveSToReg(cg->eax, cf);
  cg->xor_(cg->edx, cg->edx);

  Label done;
  Label not_divide_by_zero;
  cg->test(rt, rt);
  cg->jnz(not_divide_by_zero, CodeGenerator::T_SHORT);
  cg->mov(rlo, static_cast<u32>(-1));
  cg->mov(rhi, cg->eax);
  cg->jmp(done, CodeGenerator::T_SHORT);

  cg->L(not_divide_by_zero);
  cg->div(rt);
  cg->mov(rlo, cg->eax);
  cg->mov(rhi, cg->edx);

  cg->L(done);
}

void CPU::X64Recompiler::TestOverflow(const Xbyak::Reg32& result)
{
  SwitchToFarCode(true, &Xbyak::CodeGenerator::jo);

  BackupHostState();

  // toss the result
  ClearHostReg(result.getIdx());

  EndBlockWithException(Exception::Ov);

  RestoreHostState();

  SwitchToNearCode(false);
}

void CPU::X64Recompiler::Compile_dst_op(CompileFlags cf,
                                        void (Xbyak::CodeGenerator::*op)(const Xbyak::Operand&, const Xbyak::Operand&),
                                        void (Xbyak::CodeGenerator::*op_const)(const Xbyak::Operand&, u32),
                                        bool commutative, bool overflow)
{
  if (cf.valid_host_s && cf.valid_host_t)
  {
    if (cf.host_d == cf.host_s)
    {
      (cg->*op)(CFGetRegD(cf), CFGetRegT(cf));
    }
    else if (cf.host_d == cf.host_t)
    {
      if (commutative)
      {
        (cg->*op)(CFGetRegD(cf), CFGetRegS(cf));
      }
      else
      {
        cg->mov(RWARG1, CFGetRegT(cf));
        cg->mov(CFGetRegD(cf), CFGetRegS(cf));
        (cg->*op)(CFGetRegD(cf), RWARG1);
      }
    }
    else
    {
      cg->mov(CFGetRegD(cf), CFGetRegS(cf));
      (cg->*op)(CFGetRegD(cf), CFGetRegT(cf));
    }
  }
  else if (commutative && (cf.const_s || cf.const_t))
  {
    const Reg32 rd = CFGetRegD(cf);
    (cf.const_s) ? MoveTToReg(rd, cf) : MoveSToReg(rd, cf);
    if (const u32 cv = GetConstantRegU32(cf.const_s ? cf.MipsS() : cf.MipsT()); cv != 0)
      (cg->*op_const)(CFGetRegD(cf), cv);
    else
      overflow = false;
  }
  else if (cf.const_s)
  {
    // need to backup T?
    if (cf.valid_host_d && cf.valid_host_t && cf.host_d == cf.host_t)
    {
      cg->mov(RWARG1, CFGetRegT(cf));
      MoveSToReg(CFGetRegD(cf), cf);
      (cg->*op)(CFGetRegD(cf), RWARG1);
    }
    else
    {
      MoveSToReg(CFGetRegD(cf), cf);
      (cg->*op)(CFGetRegD(cf), CFGetRegT(cf));
    }
  }
  else if (cf.const_t)
  {
    MoveSToReg(CFGetRegD(cf), cf);
    if (const u32 cv = GetConstantRegU32(cf.MipsT()); cv != 0)
      (cg->*op_const)(CFGetRegD(cf), cv);
    else
      overflow = false;
  }
  else if (cf.valid_host_s)
  {
    if (cf.host_d != cf.host_s)
      cg->mov(CFGetRegD(cf), CFGetRegS(cf));
    (cg->*op)(CFGetRegD(cf), MipsPtr(cf.MipsT()));
  }
  else if (cf.valid_host_t)
  {
    if (cf.host_d != cf.host_t)
      cg->mov(CFGetRegD(cf), CFGetRegT(cf));
    (cg->*op)(CFGetRegD(cf), MipsPtr(cf.MipsS()));
  }
  else
  {
    cg->mov(CFGetRegD(cf), MipsPtr(cf.MipsS()));
    (cg->*op)(CFGetRegD(cf), MipsPtr(cf.MipsT()));
  }

  if (overflow)
  {
    DebugAssert(cf.valid_host_d);
    TestOverflow(CFGetRegD(cf));
  }
}

void CPU::X64Recompiler::Compile_add(CompileFlags cf)
{
  Compile_dst_op(cf, &CodeGenerator::add, &CodeGenerator::add, true, g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::X64Recompiler::Compile_addu(CompileFlags cf)
{
  Compile_dst_op(cf, &CodeGenerator::add, &CodeGenerator::add, true, false);
}

void CPU::X64Recompiler::Compile_sub(CompileFlags cf)
{
  Compile_dst_op(cf, &CodeGenerator::sub, &CodeGenerator::sub, false, g_settings.cpu_recompiler_memory_exceptions);
}

void CPU::X64Recompiler::Compile_subu(CompileFlags cf)
{
  Compile_dst_op(cf, &CodeGenerator::sub, &CodeGenerator::sub, false, false);
}

void CPU::X64Recompiler::Compile_and(CompileFlags cf)
{
  // special cases - and with self -> self, and with 0 -> 0
  const Reg32 regd = CFGetRegD(cf);
  if (cf.MipsS() == cf.MipsT())
  {
    MoveSToReg(regd, cf);
    return;
  }
  else if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0))
  {
    cg->xor_(regd, regd);
    return;
  }

  Compile_dst_op(cf, &CodeGenerator::and_, &CodeGenerator::and_, true, false);
}

void CPU::X64Recompiler::Compile_or(CompileFlags cf)
{
  // or/nor with 0 -> no effect
  const Reg32 regd = CFGetRegD(cf);
  if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0) || cf.MipsS() == cf.MipsT())
  {
    cf.const_s ? MoveTToReg(regd, cf) : MoveSToReg(regd, cf);
    return;
  }

  Compile_dst_op(cf, &CodeGenerator::or_, &CodeGenerator::or_, true, false);
}

void CPU::X64Recompiler::Compile_xor(CompileFlags cf)
{
  const Reg32 regd = CFGetRegD(cf);
  if (cf.MipsS() == cf.MipsT())
  {
    // xor with self -> zero
    cg->xor_(regd, regd);
    return;
  }
  else if (HasConstantRegValue(cf.MipsS(), 0) || HasConstantRegValue(cf.MipsT(), 0))
  {
    // xor with zero -> no effect
    cf.const_s ? MoveTToReg(regd, cf) : MoveSToReg(regd, cf);
    return;
  }

  Compile_dst_op(cf, &CodeGenerator::xor_, &CodeGenerator::xor_, true, false);
}

void CPU::X64Recompiler::Compile_nor(CompileFlags cf)
{
  Compile_or(cf);
  cg->not_(CFGetRegD(cf));
}

void CPU::X64Recompiler::Compile_slt(CompileFlags cf)
{
  Compile_slt(cf, true);
}

void CPU::X64Recompiler::Compile_sltu(CompileFlags cf)
{
  Compile_slt(cf, false);
}

void CPU::X64Recompiler::Compile_slt(CompileFlags cf, bool sign)
{
  const Reg32 rd = CFGetRegD(cf);
  const Reg32 rs = cf.valid_host_s ? CFGetRegS(cf) : RWARG1;
  const Reg32 rt = cf.valid_host_t ? CFGetRegT(cf) : RWARG1;
  if (!cf.valid_host_s)
    MoveSToReg(rs, cf);

  // Case where D == S, can't use xor because it changes flags
  // TODO: swap and reverse op for constants
  if (rd != rs && rd != rt)
    cg->xor_(rd, rd);

  if (cf.valid_host_t)
    cg->cmp(rs, CFGetRegT(cf));
  else if (cf.const_t)
    cg->cmp(rs, GetConstantRegU32(cf.MipsT()));
  else
    cg->cmp(rs, MipsPtr(cf.MipsT()));

  if (rd == rs || rd == rt)
    cg->mov(rd, 0);

  sign ? cg->setl(rd.cvt8()) : cg->setb(rd.cvt8());
}

Xbyak::Reg32
CPU::X64Recompiler::ComputeLoadStoreAddressArg(CompileFlags cf, const std::optional<VirtualMemoryAddress>& address,
                                               const std::optional<const Xbyak::Reg32>& reg /* = std::nullopt */)
{
  const u32 imm = inst->i.imm_sext32();
  if (cf.valid_host_s && imm == 0 && !reg.has_value())
    return CFGetRegS(cf);

  const Reg32 dst = reg.has_value() ? reg.value() : RWARG1;
  if (address.has_value())
  {
    cg->mov(dst, address.value());
  }
  else
  {
    if (cf.valid_host_s)
    {
      if (const Reg32 src = CFGetRegS(cf); src != dst)
        cg->mov(dst, CFGetRegS(cf));
    }
    else
    {
      cg->mov(dst, MipsPtr(cf.MipsS()));
    }

    if (imm != 0)
      cg->add(dst, inst->i.imm_sext32());
  }

  return dst;
}

template<typename RegAllocFn>
Xbyak::Reg32 CPU::X64Recompiler::GenerateLoad(const Xbyak::Reg32& addr_reg, MemoryAccessSize size, bool sign,
                                              bool use_fastmem, const RegAllocFn& dst_reg_alloc)
{
  if (use_fastmem)
  {
    m_cycles += Bus::RAM_READ_TICKS;

    const Reg32 dst = dst_reg_alloc();

    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg != RWARG3);
      cg->mov(RWARG3, addr_reg.cvt32());
      cg->shr(RXARG3, Bus::FASTMEM_LUT_PAGE_SHIFT);
      cg->mov(RXARG3, cg->qword[RMEMBASE + RXARG3 * 8]);
    }

    const Reg64 membase = (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT) ? RXARG3 : RMEMBASE;
    u8* start = cg->getCurr<u8*>();
    switch (size)
    {
      case MemoryAccessSize::Byte:
      {
        sign ? cg->movsx(dst, cg->byte[membase + addr_reg.cvt64()]) :
               cg->movzx(dst, cg->byte[membase + addr_reg.cvt64()]);
      }
      break;

      case MemoryAccessSize::HalfWord:
      {
        sign ? cg->movsx(dst, cg->word[membase + addr_reg.cvt64()]) :
               cg->movzx(dst, cg->word[membase + addr_reg.cvt64()]);
      }
      break;

      case MemoryAccessSize::Word:
      {
        cg->mov(dst, cg->word[membase + addr_reg.cvt64()]);
      }
      break;
    }

    u8* end = cg->getCurr<u8*>();
    while ((end - start) < BACKPATCH_JMP_SIZE)
    {
      cg->nop();
      end = cg->getCurr<u8*>();
    }

    AddLoadStoreInfo(start, static_cast<u32>(end - start), static_cast<u32>(addr_reg.getIdx()),
                     static_cast<u32>(dst.getIdx()), size, sign, true);
    return dst;
  }

  if (addr_reg != RWARG1)
    cg->mov(RWARG1, addr_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      cg->call(checked ? reinterpret_cast<const void*>(&RecompilerThunks::ReadMemoryByte) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      cg->call(checked ? reinterpret_cast<const void*>(&RecompilerThunks::ReadMemoryHalfWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      cg->call(checked ? reinterpret_cast<const void*>(&RecompilerThunks::ReadMemoryWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    cg->test(RXRET, RXRET);

    BackupHostState();
    SwitchToFarCode(true, &CodeGenerator::js);

    // flush regs, but not pc, it's going to get overwritten
    // flush cycles because of the GTE instruction stuff...
    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (-result << 2) | BD | cop_n
    cg->mov(RWARG1, RWRET);
    cg->neg(RWARG1);
    cg->shl(RWARG1, 2);
    cg->or_(RWARG1, Cop0Registers::CAUSE::MakeValueForException(
                      static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n));
    cg->mov(RWARG2, m_current_instruction_pc);
    cg->call(static_cast<void (*)(u32, u32)>(&CPU::RaiseException));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, true, false);

    SwitchToNearCode(false);
    RestoreHostState();
  }

  const Xbyak::Reg32 dst_reg = dst_reg_alloc();
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      sign ? cg->movsx(dst_reg, RWRET.cvt8()) : cg->movzx(dst_reg, RWRET.cvt8());
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      sign ? cg->movsx(dst_reg, RWRET.cvt16()) : cg->movzx(dst_reg, RWRET.cvt16());
    }
    break;
    case MemoryAccessSize::Word:
    {
      if (dst_reg != RWRET)
        cg->mov(dst_reg, RWRET);
    }
    break;
  }

  return dst_reg;
}

void CPU::X64Recompiler::GenerateStore(const Xbyak::Reg32& addr_reg, const Xbyak::Reg32& value_reg,
                                       MemoryAccessSize size, bool use_fastmem)
{
  if (use_fastmem)
  {
    if (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT)
    {
      DebugAssert(addr_reg != RWARG3 && value_reg != RWARG3);
      cg->mov(RWARG3, addr_reg.cvt32());
      cg->shr(RXARG3, Bus::FASTMEM_LUT_PAGE_SHIFT);
      cg->mov(RXARG3, cg->qword[RMEMBASE + RXARG3 * 8]);
    }

    const Reg64 membase = (g_settings.cpu_fastmem_mode == CPUFastmemMode::LUT) ? RXARG3 : RMEMBASE;
    u8* start = cg->getCurr<u8*>();
    switch (size)
    {
      case MemoryAccessSize::Byte:
        cg->mov(cg->byte[membase + addr_reg.cvt64()], value_reg.cvt8());
        break;

      case MemoryAccessSize::HalfWord:
        cg->mov(cg->word[membase + addr_reg.cvt64()], value_reg.cvt16());
        break;

      case MemoryAccessSize::Word:
        cg->mov(cg->word[membase + addr_reg.cvt64()], value_reg.cvt32());
        break;
    }

    u8* end = cg->getCurr<u8*>();
    while ((end - start) < BACKPATCH_JMP_SIZE)
    {
      cg->nop();
      end = cg->getCurr<u8*>();
    }

    AddLoadStoreInfo(start, static_cast<u32>(end - start), static_cast<u32>(addr_reg.getIdx()),
                     static_cast<u32>(value_reg.getIdx()), size, false, false);
    return;
  }

  if (addr_reg != RWARG1)
    cg->mov(RWARG1, addr_reg);
  if (value_reg != RWARG2)
    cg->mov(RWARG2, value_reg);

  const bool checked = g_settings.cpu_recompiler_memory_exceptions;
  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      cg->call(checked ? reinterpret_cast<const void*>(&RecompilerThunks::WriteMemoryByte) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      cg->call(checked ? reinterpret_cast<const void*>(&RecompilerThunks::WriteMemoryHalfWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      cg->call(checked ? reinterpret_cast<const void*>(&RecompilerThunks::WriteMemoryWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryWord));
    }
    break;
  }

  // TODO: turn this into an asm function instead
  if (checked)
  {
    cg->test(RWRET, RWRET);

    BackupHostState();
    SwitchToFarCode(true, &CodeGenerator::jnz);

    // flush regs, but not pc, it's going to get overwritten
    // flush cycles because of the GTE instruction stuff...
    Flush(FLUSH_FOR_C_CALL | FLUSH_FLUSH_MIPS_REGISTERS | FLUSH_FOR_EXCEPTION);

    // cause_bits = (result << 2) | BD | cop_n
    cg->mov(RWARG1, RWRET);
    cg->shl(RWARG1, 2);
    cg->or_(RWARG1, Cop0Registers::CAUSE::MakeValueForException(
                      static_cast<Exception>(0), m_current_instruction_branch_delay_slot, false, inst->cop.cop_n));
    cg->mov(RWARG2, m_current_instruction_pc);
    cg->call(reinterpret_cast<const void*>(static_cast<void (*)(u32, u32)>(&CPU::RaiseException)));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, true, false);

    SwitchToNearCode(false);
    RestoreHostState();
  }
}

void CPU::X64Recompiler::Compile_lxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                     const std::optional<VirtualMemoryAddress>& address)
{
  const std::optional<Reg32> addr_reg = g_settings.gpu_pgxp_enable ?
                                          std::optional<Reg32>(Reg32(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                          std::optional<Reg32>();
  FlushForLoadStore(address, false, use_fastmem);
  const Reg32 addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);

  const Reg32 data = GenerateLoad(addr, size, sign, use_fastmem, [this, cf]() {
    if (cf.MipsT() == Reg::zero)
      return RWRET;

    return Reg32(AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                                 EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, cf.MipsT()));
  });

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);

    cg->mov(RWARG1, inst->bits);
    cg->mov(RWARG2, addr);
    cg->mov(RWARG3, data);
    cg->call(s_pgxp_mem_load_functions[static_cast<u32>(size)][static_cast<u32>(sign)]);
    FreeHostReg(addr_reg.value().getIdx());
  }
}

void CPU::X64Recompiler::Compile_lwx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                     const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  const Reg32 addr = Reg32(AllocateTempHostReg(HR_CALLEE_SAVED));
  FlushForLoadStore(address, false, use_fastmem);

  // TODO: if address is constant, this can be simplified..

  // If we're coming from another block, just flush the load delay and hope for the best..
  if (m_load_delay_dirty)
    UpdateLoadDelay();

  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  cg->mov(RWARG1, addr);
  cg->and_(RWARG1, ~0x3u);
  GenerateLoad(RWARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RWRET; });

  if (inst->r.rt == Reg::zero)
  {
    FreeHostReg(addr.getIdx());
    return;
  }

  // lwl/lwr from a load-delayed value takes the new value, but it itself, is load delayed, so the original value is
  // never written back. NOTE: can't trust T in cf because of the flush
  const Reg rt = inst->r.rt;
  Reg32 value;
  if (m_load_delay_register == rt)
  {
    const u32 existing_ld_rt = (m_load_delay_value_register == NUM_HOST_REGS) ?
                                 AllocateHostReg(HR_MODE_READ, HR_TYPE_LOAD_DELAY_VALUE, rt) :
                                 m_load_delay_value_register;
    RenameHostReg(existing_ld_rt, HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt);
    value = Reg32(existing_ld_rt);
  }
  else
  {
    if constexpr (EMULATE_LOAD_DELAYS)
    {
      value = Reg32(AllocateHostReg(HR_MODE_WRITE, HR_TYPE_NEXT_LOAD_DELAY_VALUE, rt));
      if (HasConstantReg(rt))
        cg->mov(value, GetConstantRegU32(rt));
      else if (const std::optional<u32> rtreg = CheckHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt); rtreg.has_value())
        cg->mov(value, Reg32(rtreg.value()));
      else
        cg->mov(value, MipsPtr(rt));
    }
    else
    {
      value = Reg32(AllocateHostReg(HR_MODE_READ | HR_MODE_WRITE, HR_TYPE_CPU_REG, rt));
    }
  }

  DebugAssert(value != cg->ecx);
  cg->mov(cg->ecx, addr);
  cg->and_(cg->ecx, 3);
  cg->shl(cg->ecx, 3); // *8

  // TODO for other arch: reverse subtract
  DebugAssert(RWARG2 != cg->ecx);
  cg->mov(RWARG2, 24);
  cg->sub(RWARG2, cg->ecx);

  if (inst->op == InstructionOp::lwl)
  {
    // const u32 mask = UINT32_C(0x00FFFFFF) >> shift;
    // new_value = (value & mask) | (RWRET << (24 - shift));
    cg->mov(RWARG3, 0xFFFFFFu);
    cg->shr(RWARG3, cg->cl);
    cg->and_(value, RWARG3);
    cg->mov(cg->ecx, RWARG2);
    cg->shl(RWRET, cg->cl);
    cg->or_(value, RWRET);
  }
  else
  {
    // const u32 mask = UINT32_C(0xFFFFFF00) << (24 - shift);
    // new_value = (value & mask) | (RWRET >> shift);
    cg->shr(RWRET, cg->cl);
    cg->mov(RWARG3, 0xFFFFFF00u);
    cg->mov(cg->ecx, RWARG2);
    cg->shl(RWARG3, cg->cl);
    cg->and_(value, RWARG3);
    cg->or_(value, RWRET);
  }

  FreeHostReg(addr.getIdx());

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);

    DebugAssert(value != RWARG3);
    cg->mov(RWARG3, value);
    cg->mov(RWARG2, addr);
    cg->and_(RWARG2, ~0x3u);
    cg->mov(RWARG1, inst->bits);
    cg->call(reinterpret_cast<const void*>(&PGXP::CPU_LW));
  }
}

void CPU::X64Recompiler::Compile_lwc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                      const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, true);
  const std::optional<Reg32> addr_reg = g_settings.gpu_pgxp_enable ?
                                          std::optional<Reg32>(Reg32(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                          std::optional<Reg32>();
  FlushForLoadStore(address, false, use_fastmem);
  const Reg32 addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Reg32 value = GenerateLoad(addr, MemoryAccessSize::Word, false, use_fastmem, [this, action = action]() {
    return (action == GTERegisterAccessAction::CallHandler && g_settings.gpu_pgxp_enable) ?
             Reg32(AllocateTempHostReg(HR_CALLEE_SAVED)) :
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
      cg->mov(cg->dword[PTR(ptr)], value);
      break;
    }

    case GTERegisterAccessAction::SignExtend16:
    {
      cg->movsx(RWARG3, value.cvt16());
      cg->mov(cg->dword[PTR(ptr)], RWARG3);
      break;
    }

    case GTERegisterAccessAction::ZeroExtend16:
    {
      cg->movzx(RWARG3, value.cvt16());
      cg->mov(cg->dword[PTR(ptr)], RWARG3);
      break;
    }

    case GTERegisterAccessAction::CallHandler:
    {
      Flush(FLUSH_FOR_C_CALL);
      cg->mov(RWARG2, value);
      cg->mov(RWARG1, index);
      cg->call(&GTE::WriteRegister);
      break;
    }

    case GTERegisterAccessAction::PushFIFO:
    {
      // SXY0 <- SXY1
      // SXY1 <- SXY2
      // SXY2 <- SXYP
      DebugAssert(value != RWARG1 && value != RWARG2);
      cg->mov(RWARG1, cg->dword[PTR(&g_state.gte_regs.SXY1[0])]);
      cg->mov(RWARG2, cg->dword[PTR(&g_state.gte_regs.SXY2[0])]);
      cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY0[0])], RWARG1);
      cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY1[0])], RWARG2);
      cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY2[0])], value);
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
    cg->mov(RWARG3, value);
    if (value != RWRET)
      FreeHostReg(value.getIdx());
    cg->mov(RWARG2, addr);
    FreeHostReg(addr_reg.value().getIdx());
    cg->mov(RWARG1, inst->bits);
    cg->call(reinterpret_cast<const void*>(&PGXP::CPU_LWC2));
  }
}

void CPU::X64Recompiler::Compile_sxx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                     const std::optional<VirtualMemoryAddress>& address)
{
  const std::optional<Reg32> addr_reg = g_settings.gpu_pgxp_enable ?
                                          std::optional<Reg32>(Reg32(AllocateTempHostReg(HR_CALLEE_SAVED))) :
                                          std::optional<Reg32>();
  FlushForLoadStore(address, true, use_fastmem);
  const Reg32 addr = ComputeLoadStoreAddressArg(cf, address, addr_reg);
  const Reg32 data = cf.valid_host_t ? CFGetRegT(cf) : RWARG2;
  if (!cf.valid_host_t)
    MoveTToReg(RWARG2, cf);

  GenerateStore(addr, data, size, use_fastmem);

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    MoveMIPSRegToReg(RWARG3, cf.MipsT());
    cg->mov(RWARG2, addr);
    cg->mov(RWARG1, inst->bits);
    cg->call(s_pgxp_mem_store_functions[static_cast<u32>(size)]);
    FreeHostReg(addr_reg.value().getIdx());
  }
}

void CPU::X64Recompiler::Compile_swx(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                     const std::optional<VirtualMemoryAddress>& address)
{
  DebugAssert(size == MemoryAccessSize::Word && !sign);

  // TODO: this can take over rt's value if it's no longer needed
  // NOTE: can't trust T in cf because of the alloc
  const Reg32 addr = Reg32(AllocateTempHostReg(HR_CALLEE_SAVED));
  const Reg32 value = g_settings.gpu_pgxp_enable ? Reg32(AllocateTempHostReg(HR_CALLEE_SAVED)) : RWARG2;
  if (g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  FlushForLoadStore(address, true, use_fastmem);

  // TODO: if address is constant, this can be simplified..
  // We'd need to be careful here if we weren't overwriting it..
  ComputeLoadStoreAddressArg(cf, address, addr);
  cg->mov(RWARG1, addr);
  cg->and_(RWARG1, ~0x3u);
  GenerateLoad(RWARG1, MemoryAccessSize::Word, false, use_fastmem, []() { return RWRET; });

  DebugAssert(value != cg->ecx);
  cg->mov(cg->ecx, addr);
  cg->and_(cg->ecx, 3);
  cg->shl(cg->ecx, 3); // *8
  cg->and_(addr, ~0x3u);

  // Need to load down here for PGXP-off, because it's in a volatile reg that can get overwritten by flush.
  if (!g_settings.gpu_pgxp_enable)
    MoveMIPSRegToReg(value, inst->r.rt);

  if (inst->op == InstructionOp::swl)
  {
    // const u32 mem_mask = UINT32_C(0xFFFFFF00) << shift;
    // new_value = (RWRET & mem_mask) | (value >> (24 - shift));
    cg->mov(RWARG3, 0xFFFFFF00u);
    cg->shl(RWARG3, cg->cl);
    cg->and_(RWRET, RWARG3);

    cg->mov(RWARG3, 24);
    cg->sub(RWARG3, cg->ecx);
    cg->mov(cg->ecx, RWARG3);
    cg->shr(value, cg->cl);
    cg->or_(value, RWRET);
  }
  else
  {
    // const u32 mem_mask = UINT32_C(0x00FFFFFF) >> (24 - shift);
    // new_value = (RWRET & mem_mask) | (value << shift);
    cg->shl(value, cg->cl);

    DebugAssert(RWARG3 != cg->ecx);
    cg->mov(RWARG3, 24);
    cg->sub(RWARG3, cg->ecx);
    cg->mov(cg->ecx, RWARG3);
    cg->mov(RWARG3, 0x00FFFFFFu);
    cg->shr(RWARG3, cg->cl);
    cg->and_(RWRET, RWARG3);
    cg->or_(value, RWRET);
  }

  if (!g_settings.gpu_pgxp_enable)
  {
    GenerateStore(addr, value, MemoryAccessSize::Word, use_fastmem);
    FreeHostReg(addr.getIdx());
  }
  else
  {
    GenerateStore(addr, value, MemoryAccessSize::Word, use_fastmem);

    Flush(FLUSH_FOR_C_CALL);
    cg->mov(RWARG3, value);
    FreeHostReg(value.getIdx());
    cg->mov(RWARG2, addr);
    FreeHostReg(addr.getIdx());
    cg->mov(RWARG1, inst->bits);
    cg->call(reinterpret_cast<const void*>(&PGXP::CPU_SW));
  }
}

void CPU::X64Recompiler::Compile_swc2(CompileFlags cf, MemoryAccessSize size, bool sign, bool use_fastmem,
                                      const std::optional<VirtualMemoryAddress>& address)
{
  const u32 index = static_cast<u32>(inst->r.rt.GetValue());
  const auto [ptr, action] = GetGTERegisterPointer(index, false);
  switch (action)
  {
    case GTERegisterAccessAction::Direct:
    {
      cg->mov(RWARG2, cg->dword[PTR(ptr)]);
    }
    break;

    case GTERegisterAccessAction::CallHandler:
    {
      // should already be flushed.. except in fastmem case
      Flush(FLUSH_FOR_C_CALL);
      cg->mov(RWARG1, index);
      cg->call(&GTE::ReadRegister);
      cg->mov(RWARG2, RWRET);
    }
    break;

    default:
    {
      Panic("Unknown action");
    }
    break;
  }

  // PGXP makes this a giant pain.
  if (!g_settings.gpu_pgxp_enable)
  {
    FlushForLoadStore(address, true, use_fastmem);
    const Reg32 addr = ComputeLoadStoreAddressArg(cf, address);
    GenerateStore(addr, RWARG2, size, use_fastmem);
    return;
  }

  // TODO: This can be simplified because we don't need to validate in PGXP..
  const Reg32 addr_reg = Reg32(AllocateTempHostReg(HR_CALLEE_SAVED));
  const Reg32 data_backup = Reg32(AllocateTempHostReg(HR_CALLEE_SAVED));
  FlushForLoadStore(address, true, use_fastmem);
  ComputeLoadStoreAddressArg(cf, address, addr_reg);
  cg->mov(data_backup, RWARG2);
  GenerateStore(addr_reg, RWARG2, size, use_fastmem);

  Flush(FLUSH_FOR_C_CALL);
  cg->mov(RWARG3, data_backup);
  cg->mov(RWARG2, addr_reg);
  cg->mov(RWARG1, inst->bits);
  cg->call(reinterpret_cast<const void*>(&PGXP::CPU_SWC2));
  FreeHostReg(addr_reg.getIdx());
  FreeHostReg(data_backup.getIdx());
}

void CPU::X64Recompiler::Compile_mtc0(CompileFlags cf)
{
  const Cop0Reg reg = static_cast<Cop0Reg>(MipsD());
  const u32* ptr = GetCop0RegPtr(reg);
  const u32 mask = GetCop0RegWriteMask(reg);
  if (!ptr)
  {
    Compile_Fallback();
    return;
  }

  // TODO: const apply mask
  const Reg32 rt = cf.valid_host_t ? CFGetRegT(cf) : RWARG1;
  const u32 constant_value = cf.const_t ? GetConstantRegU32(cf.MipsT()) : 0;
  if (mask == 0)
  {
    // if it's a read-only register, ignore
    DEBUG_LOG("Ignoring write to read-only cop0 reg {}", static_cast<u32>(reg));
    return;
  }

  // for some registers, we need to test certain bits
  const bool needs_bit_test = (reg == Cop0Reg::SR);
  const Reg32 changed_bits = RWARG3;

  // update value
  if (cf.valid_host_t)
  {
    cg->mov(RWARG1, rt);
    cg->mov(RWARG2, cg->dword[PTR(ptr)]);
    cg->and_(RWARG1, mask);
    if (needs_bit_test)
    {
      cg->mov(changed_bits, RWARG2);
      cg->xor_(changed_bits, RWARG1);
    }
    cg->and_(RWARG2, ~mask);
    cg->or_(RWARG2, RWARG1);
    cg->mov(cg->dword[PTR(ptr)], RWARG2);
  }
  else
  {
    cg->mov(RWARG2, cg->dword[PTR(ptr)]);
    if (needs_bit_test)
    {
      cg->mov(changed_bits, RWARG2);
      cg->xor_(changed_bits, constant_value & mask);
    }
    cg->and_(RWARG2, ~mask);
    cg->or_(RWARG2, constant_value & mask);
    cg->mov(cg->dword[PTR(ptr)], RWARG2);
  }

  if (reg == Cop0Reg::SR)
  {
    // TODO: replace with register backup
    // We could just inline the whole thing..
    Flush(FLUSH_FOR_C_CALL);

    Label caches_unchanged;
    cg->test(changed_bits, 1u << 16);
    cg->jz(caches_unchanged);
    cg->call(&CPU::UpdateMemoryPointers);
    cg->mov(RWARG2, cg->dword[PTR(ptr)]); // reload value for interrupt test below
    if (CodeCache::IsUsingFastmem())
      cg->mov(RMEMBASE, cg->qword[PTR(&g_state.fastmem_base)]);

    cg->L(caches_unchanged);

    TestInterrupts(RWARG2);
  }
  else if (reg == Cop0Reg::CAUSE)
  {
    cg->mov(RWARG1, cg->dword[PTR(&g_state.cop0_regs.sr.bits)]);
    TestInterrupts(RWARG1);
  }
  else if (reg == Cop0Reg::DCIC || reg == Cop0Reg::BPCM)
  {
    // need to check whether we're switching to debug mode
    Flush(FLUSH_FOR_C_CALL);
    cg->call(&CPU::UpdateDebugDispatcherFlag);
    cg->test(cg->al, cg->al);
    SwitchToFarCode(true, &Xbyak::CodeGenerator::jnz);
    BackupHostState();
    Flush(FLUSH_FOR_EARLY_BLOCK_EXIT);
    cg->call(&CPU::ExitExecution); // does not return
    RestoreHostState();
    SwitchToNearCode(false);
  }
}

void CPU::X64Recompiler::Compile_rfe(CompileFlags cf)
{
  // shift mode bits right two, preserving upper bits
  static constexpr u32 mode_bits_mask = UINT32_C(0b1111);
  cg->mov(RWARG1, cg->dword[PTR(&g_state.cop0_regs.sr.bits)]);
  cg->mov(RWARG2, RWARG1);
  cg->shr(RWARG2, 2);
  cg->and_(RWARG1, ~mode_bits_mask);
  cg->and_(RWARG2, mode_bits_mask);
  cg->or_(RWARG1, RWARG2);
  cg->mov(cg->dword[PTR(&g_state.cop0_regs.sr.bits)], RWARG1);

  TestInterrupts(RWARG1);
}

void CPU::X64Recompiler::TestInterrupts(const Xbyak::Reg32& sr)
{
  // if Iec == 0 then goto no_interrupt
  Label no_interrupt;

  cg->test(sr, 1);
  cg->jz(no_interrupt, CodeGenerator::T_NEAR);

  // sr & cause
  cg->and_(sr, cg->dword[PTR(&g_state.cop0_regs.cause.bits)]);

  // ((sr & cause) & 0xff00) == 0 goto no_interrupt
  cg->test(sr, 0xFF00);

  SwitchToFarCode(true, &CodeGenerator::jnz);
  BackupHostState();

  // Update load delay, this normally happens at the end of an instruction, but we're finishing it early.
  UpdateLoadDelay();

  Flush(FLUSH_END_BLOCK | FLUSH_FOR_EXCEPTION | FLUSH_FOR_C_CALL);

  // Can't use EndBlockWithException() here, because it'll use the wrong PC.
  // Can't use RaiseException() on the fast path if we're the last instruction, because the next PC is unknown.
  if (!iinfo->is_last_instruction)
  {
    cg->mov(RWARG1, Cop0Registers::CAUSE::MakeValueForException(Exception::INT, iinfo->is_branch_instruction, false,
                                                                (inst + 1)->cop.cop_n));
    cg->mov(RWARG2, m_compiler_pc);
    cg->call(static_cast<void (*)(u32, u32)>(&CPU::RaiseException));
    m_dirty_pc = false;
    EndAndLinkBlock(std::nullopt, true, false);
  }
  else
  {
    if (m_dirty_pc)
      cg->mov(cg->dword[PTR(&g_state.pc)], m_compiler_pc);
    m_dirty_pc = false;
    cg->mov(cg->dword[PTR(&g_state.downcount)], 0);
    EndAndLinkBlock(std::nullopt, false, true);
  }

  RestoreHostState();
  SwitchToNearCode(false);

  cg->L(no_interrupt);
}

void CPU::X64Recompiler::Compile_mfc2(CompileFlags cf)
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
    cg->mov(Reg32(hreg), cg->dword[PTR(ptr)]);
  }
  else if (action == GTERegisterAccessAction::CallHandler)
  {
    Flush(FLUSH_FOR_C_CALL);
    cg->mov(RWARG1, index);
    cg->call(&GTE::ReadRegister);

    hreg = AllocateHostReg(GetFlagsForNewLoadDelayedReg(),
                           EMULATE_LOAD_DELAYS ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG, rt);
    cg->mov(Reg32(hreg), RWRET);
  }
  else
  {
    Panic("Unknown action");
    return;
  }

  if (g_settings.gpu_pgxp_enable)
  {
    Flush(FLUSH_FOR_C_CALL);
    cg->mov(RWARG1, inst->bits);
    cg->mov(RWARG2, Reg32(hreg));
    cg->call(reinterpret_cast<const void*>(&PGXP::CPU_MFC2));
  }
}

void CPU::X64Recompiler::Compile_mtc2(CompileFlags cf)
{
  const u32 index = inst->cop.Cop2Index();
  const auto [ptr, action] = GetGTERegisterPointer(index, true);
  if (action == GTERegisterAccessAction::Ignore)
    return;

  if (action == GTERegisterAccessAction::Direct)
  {
    if (cf.const_t)
    {
      cg->mov(cg->dword[PTR(ptr)], GetConstantRegU32(cf.MipsT()));
    }
    else if (cf.valid_host_t)
    {
      cg->mov(cg->dword[PTR(ptr)], CFGetRegT(cf));
    }
    else
    {
      cg->mov(RWARG1, MipsPtr(cf.MipsT()));
      cg->mov(cg->dword[PTR(ptr)], RWARG1);
    }
  }
  else if (action == GTERegisterAccessAction::SignExtend16 || action == GTERegisterAccessAction::ZeroExtend16)
  {
    const bool sign = (action == GTERegisterAccessAction::SignExtend16);
    if (cf.const_t)
    {
      const u16 cv = Truncate16(GetConstantRegU32(cf.MipsT()));
      cg->mov(cg->dword[PTR(ptr)], sign ? ::SignExtend32(cv) : ::ZeroExtend32(cv));
    }
    else if (cf.valid_host_t)
    {
      sign ? cg->movsx(RWARG1, Reg16(cf.host_t)) : cg->movzx(RWARG1, Reg16(cf.host_t));
      cg->mov(cg->dword[PTR(ptr)], RWARG1);
    }
    else
    {
      sign ? cg->movsx(RWARG1, cg->word[PTR(&g_state.regs.r[cf.mips_t])]) :
             cg->movzx(RWARG1, cg->word[PTR(&g_state.regs.r[cf.mips_t])]);
      cg->mov(cg->dword[PTR(ptr)], RWARG1);
    }
  }
  else if (action == GTERegisterAccessAction::CallHandler)
  {
    Flush(FLUSH_FOR_C_CALL);
    cg->mov(RWARG1, index);
    MoveTToReg(RWARG2, cf);
    cg->call(&GTE::WriteRegister);
  }
  else if (action == GTERegisterAccessAction::PushFIFO)
  {
    // SXY0 <- SXY1
    // SXY1 <- SXY2
    // SXY2 <- SXYP
    cg->mov(RWARG1, cg->dword[PTR(&g_state.gte_regs.SXY1[0])]);
    cg->mov(RWARG2, cg->dword[PTR(&g_state.gte_regs.SXY2[0])]);
    if (!cf.const_t && !cf.valid_host_t)
      cg->mov(RWARG3, MipsPtr(cf.MipsT()));
    cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY0[0])], RWARG1);
    cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY1[0])], RWARG2);
    if (cf.const_t)
      cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY2[0])], GetConstantRegU32(cf.MipsT()));
    else if (cf.valid_host_t)
      cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY2[0])], CFGetRegT(cf));
    else
      cg->mov(cg->dword[PTR(&g_state.gte_regs.SXY2[0])], RWARG3);
  }
  else
  {
    Panic("Unknown action");
  }
}

void CPU::X64Recompiler::Compile_cop2(CompileFlags cf)
{
  TickCount func_ticks;
  GTE::InstructionImpl func = GTE::GetInstructionImpl(inst->bits, &func_ticks);

  Flush(FLUSH_FOR_C_CALL);
  cg->mov(RWARG1, inst->bits & GTE::Instruction::REQUIRED_BITS_MASK);
  cg->call(reinterpret_cast<const void*>(func));

  AddGTETicks(func_ticks);
}

u32 CPU::Recompiler::CompileLoadStoreThunk(void* thunk_code, u32 thunk_space, void* code_address, u32 code_size,
                                           TickCount cycles_to_add, TickCount cycles_to_remove, u32 gpr_bitmask,
                                           u8 address_register, u8 data_register, MemoryAccessSize size, bool is_signed,
                                           bool is_load)
{
  CodeGenerator acg(thunk_space, thunk_code);
  CodeGenerator* cg = &acg;

  static constexpr u32 GPR_SIZE = 8;

  // save regs
  u32 num_gprs = 0;

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    if ((gpr_bitmask & (1u << i)) && IsCallerSavedRegister(i) && (!is_load || data_register != i))
      num_gprs++;
  }

  const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE);

  if (stack_size > 0)
  {
    cg->sub(cg->rsp, stack_size);

    u32 stack_offset = STACK_SHADOW_SIZE;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && IsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        cg->mov(cg->qword[cg->rsp + stack_offset], Reg64(i));
        stack_offset += GPR_SIZE;
      }
    }
  }

  if (cycles_to_add != 0)
    cg->add(cg->dword[PTR(&g_state.pending_ticks)], cycles_to_add);

  if (address_register != static_cast<u8>(RWARG1.getIdx()))
    cg->mov(RWARG1, Reg32(address_register));

  if (!is_load)
  {
    if (data_register != static_cast<u8>(RWARG2.getIdx()))
      cg->mov(RWARG2, Reg32(data_register));
  }

  switch (size)
  {
    case MemoryAccessSize::Byte:
    {
      cg->call(is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryByte) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryByte));
    }
    break;
    case MemoryAccessSize::HalfWord:
    {
      cg->call(is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryHalfWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryHalfWord));
    }
    break;
    case MemoryAccessSize::Word:
    {
      cg->call(is_load ? reinterpret_cast<const void*>(&RecompilerThunks::UncheckedReadMemoryWord) :
                         reinterpret_cast<const void*>(&RecompilerThunks::UncheckedWriteMemoryWord));
    }
    break;
  }

  if (is_load)
  {
    const Reg32 dst = Reg32(data_register);
    switch (size)
    {
      case MemoryAccessSize::Byte:
      {
        is_signed ? cg->movsx(dst, RWRET.cvt8()) : cg->movzx(dst, RWRET.cvt8());
      }
      break;
      case MemoryAccessSize::HalfWord:
      {
        is_signed ? cg->movsx(dst, RWRET.cvt16()) : cg->movzx(dst, RWRET.cvt16());
      }
      break;
      case MemoryAccessSize::Word:
      {
        if (dst != RWRET)
          cg->mov(dst, RWRET);
      }
      break;
    }
  }

  if (cycles_to_remove != 0)
    cg->sub(cg->dword[PTR(&g_state.pending_ticks)], cycles_to_remove);

  // restore regs
  if (stack_size > 0)
  {
    u32 stack_offset = STACK_SHADOW_SIZE;
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      if ((gpr_bitmask & (1u << i)) && IsCallerSavedRegister(i) && (!is_load || data_register != i))
      {
        cg->mov(Reg64(i), cg->qword[cg->rsp + stack_offset]);
        stack_offset += GPR_SIZE;
      }
    }

    cg->add(cg->rsp, stack_size);
  }

  cg->jmp(static_cast<const u8*>(code_address) + code_size);

  // fill the rest of it with nops, if any
  DebugAssert(code_size >= BACKPATCH_JMP_SIZE);
  if (code_size > BACKPATCH_JMP_SIZE)
    std::memset(static_cast<u8*>(code_address) + BACKPATCH_JMP_SIZE, 0x90, code_size - BACKPATCH_JMP_SIZE);

  return static_cast<u32>(cg->getSize());
}

#endif // CPU_ARCH_X64
