// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "bus.h"
#include "common/bitfield.h"
#include "common/perf_scope.h"
#include "cpu_code_cache.h"
#include "cpu_core_private.h"
#include "cpu_types.h"

#include "util/jit_code_buffer.h"
#include "util/page_fault_handler.h"

#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace CPU::CodeCache {

enum : u32
{
  LUT_TABLE_COUNT = 0x10000,
  LUT_TABLE_SIZE = 0x10000 / sizeof(u32), // 16384, one for each PC
  LUT_TABLE_SHIFT = 16,

  MAX_BLOCK_EXIT_LINKS = 2,
};

using CodeLUT = const void**;
using CodeLUTArray = std::array<CodeLUT, LUT_TABLE_COUNT>;
using BlockLinkMap = std::unordered_multimap<u32, void*>; // TODO: try ordered?

enum RegInfoFlags : u8
{
  RI_LIVE = (1 << 0),
  RI_USED = (1 << 1),
  RI_LASTUSE = (1 << 2),
};

struct InstructionInfo
{
  u32 pc; // TODO: Remove this, old recs still depend on it.

  bool is_branch_instruction : 1;
  bool is_direct_branch_instruction : 1;
  bool is_unconditional_branch_instruction : 1;
  bool is_branch_delay_slot : 1;
  bool is_load_instruction : 1;
  bool is_store_instruction : 1;
  bool is_load_delay_slot : 1;
  bool is_last_instruction : 1;
  bool has_load_delay : 1;
  bool can_trap : 1;

  u8 reg_flags[static_cast<u8>(Reg::count)];
  // Reg write_reg[3];
  Reg read_reg[3];

  // If unset, values which are not live will not be written back to memory.
  // Tends to break stuff at the moment.
  static constexpr bool WRITE_DEAD_VALUES = true;

  /// Returns true if the register is used later in the block, and this isn't the last instruction to use it.
  /// In other words, the register is worth keeping in a host register/caching it.
  inline bool UsedTest(Reg reg) const { return (reg_flags[static_cast<u8>(reg)] & (RI_USED | RI_LASTUSE)) == RI_USED; }

  /// Returns true if the value should be computed/written back.
  /// Basically, this means it's either used before it's overwritten, or not overwritten by the end of the block.
  inline bool LiveTest(Reg reg) const
  {
    return WRITE_DEAD_VALUES || ((reg_flags[static_cast<u8>(reg)] & RI_LIVE) != 0);
  }

  /// Returns true if the register can be renamed into another.
  inline bool RenameTest(Reg reg) const { return (reg == Reg::zero || !UsedTest(reg) || !LiveTest(reg)); }

  /// Returns true if this instruction reads this register.
  inline bool ReadsReg(Reg reg) const { return (read_reg[0] == reg || read_reg[1] == reg || read_reg[2] == reg); }
};

enum class BlockState : u8
{
  Valid,
  Invalidated,
  NeedsRecompile,
  FallbackToInterpreter
};

enum class BlockFlags : u8
{
  None = 0,
  ContainsLoadStoreInstructions = (1 << 0),
  SpansPages = (1 << 1),
  BranchDelaySpansPages = (1 << 2),
};
IMPLEMENT_ENUM_CLASS_BITWISE_OPERATORS(BlockFlags);

enum class PageProtectionMode : u8
{
  WriteProtected,
  ManualCheck,
  Unprotected,
};

struct BlockMetadata
{
  TickCount uncached_fetch_ticks;
  u32 icache_line_count;
  BlockFlags flags;
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // C4324: 'CPU::CodeCache::Block': structure was padded due to alignment specifier)
#endif

struct alignas(16) Block
{
  u32 pc;
  u32 size; // in guest instructions
  const void* host_code;

  // links to previous/next block within page
  Block* next_block_in_page;

  BlockLinkMap::iterator exit_links[MAX_BLOCK_EXIT_LINKS];
  u8 num_exit_links;

  // TODO: Move up so it's part of the same cache line
  BlockState state;
  BlockFlags flags;
  PageProtectionMode protection;

  TickCount uncached_fetch_ticks;
  u32 icache_line_count;

  u32 host_code_size;
  u32 compile_frame;
  u8 compile_count;

  // followed by Instruction * size, InstructionRegInfo * size
  ALWAYS_INLINE const Instruction* Instructions() const { return reinterpret_cast<const Instruction*>(this + 1); }
  ALWAYS_INLINE Instruction* Instructions() { return reinterpret_cast<Instruction*>(this + 1); }

  ALWAYS_INLINE const InstructionInfo* InstructionsInfo() const
  {
    return reinterpret_cast<const InstructionInfo*>(Instructions() + size);
  }
  ALWAYS_INLINE InstructionInfo* InstructionsInfo()
  {
    return reinterpret_cast<InstructionInfo*>(Instructions() + size);
  }

  // returns true if the block has a given flag
  ALWAYS_INLINE bool HasFlag(BlockFlags flag) const { return ((flags & flag) != BlockFlags::None); }

  // returns the page index for the start of the block
  ALWAYS_INLINE u32 StartPageIndex() const { return Bus::GetRAMCodePageIndex(pc); }

  // returns the page index for the last instruction in the block (inclusive)
  ALWAYS_INLINE u32 EndPageIndex() const { return Bus::GetRAMCodePageIndex(pc + ((size - 1) * sizeof(Instruction))); }

  // returns true if the block spans multiple pages
  ALWAYS_INLINE bool SpansPages() const { return StartPageIndex() != EndPageIndex(); }
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

using BlockLUTArray = std::array<Block**, LUT_TABLE_COUNT>;

struct LoadstoreBackpatchInfo
{
  union
  {
    struct
    {
      u32 gpr_bitmask;
      u16 cycles;
      u16 address_register : 5;
      u16 data_register : 5;
      u16 size : 2;
      u16 is_signed : 1;
      u16 is_load : 1;
    };

    const void* thunk_address; // only needed for oldrec
  };

  u32 guest_pc;
  u32 guest_block;
  u8 code_size;

  MemoryAccessSize AccessSize() const { return static_cast<MemoryAccessSize>(size); }
  u32 AccessSizeInBytes() const { return 1u << size; }
};
#ifdef CPU_ARCH_ARM32
static_assert(sizeof(LoadstoreBackpatchInfo) == 20);
#else
static_assert(sizeof(LoadstoreBackpatchInfo) == 24);
#endif

static inline bool AddressInRAM(VirtualMemoryAddress pc)
{
  return VirtualAddressToPhysical(pc) < Bus::g_ram_size;
}

struct PageProtectionInfo
{
  Block* first_block_in_page;
  Block* last_block_in_page;

  PageProtectionMode mode;
  u16 invalidate_count;
  u32 invalidate_frame;
};
static_assert(sizeof(PageProtectionInfo) == (sizeof(Block*) * 2 + 8));

template<PGXPMode pgxp_mode>
void InterpretCachedBlock(const Block* block);

template<PGXPMode pgxp_mode>
void InterpretUncachedBlock();

void LogCurrentState();

#if defined(ENABLE_RECOMPILER) || defined(ENABLE_NEWREC)
#define ENABLE_RECOMPILER_SUPPORT 1

#if defined(_DEBUG) || false
// Enable disassembly of host assembly code.
#define ENABLE_HOST_DISASSEMBLY 1
#endif

#if false
// Enable profiling of JIT blocks.
#define ENABLE_RECOMPILER_PROFILING 1
#endif

JitCodeBuffer& GetCodeBuffer();
const void* GetInterpretUncachedBlockFunction();

void CompileOrRevalidateBlock(u32 start_pc);
void DiscardAndRecompileBlock(u32 start_pc);
const void* CreateBlockLink(Block* from_block, void* code, u32 newpc);

void AddLoadStoreInfo(void* code_address, u32 code_size, u32 guest_pc, const void* thunk_address);
void AddLoadStoreInfo(void* code_address, u32 code_size, u32 guest_pc, u32 guest_block, TickCount cycles,
                      u32 gpr_bitmask, u8 address_register, u8 data_register, MemoryAccessSize size, bool is_signed,
                      bool is_load);
bool HasPreviouslyFaultedOnPC(u32 guest_pc);

u32 EmitASMFunctions(void* code, u32 code_size);
u32 EmitJump(void* code, const void* dst, bool flush_icache);

void DisassembleAndLogHostCode(const void* start, u32 size);
u32 GetHostInstructionCount(const void* start, u32 size);

extern CodeLUTArray g_code_lut;

extern NORETURN_FUNCTION_POINTER void (*g_enter_recompiler)();
extern const void* g_compile_or_revalidate_block;
extern const void* g_check_events_and_dispatch;
extern const void* g_run_events_and_dispatch;
extern const void* g_dispatcher;
extern const void* g_block_dispatcher;
extern const void* g_interpret_block;
extern const void* g_discard_and_recompile_block;

#ifdef ENABLE_RECOMPILER_PROFILING

extern PerfScope MIPSPerfScope;

#endif // ENABLE_RECOMPILER_PROFILING

#endif // ENABLE_RECOMPILER

} // namespace CPU::CodeCache
