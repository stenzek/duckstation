// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "bus.h"
#include "cpu_code_cache_private.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "host.h"
#include "settings.h"
#include "system.h"
#include "timing_event.h"

#include "util/page_fault_handler.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/memmap.h"

LOG_CHANNEL(CodeCache);

// Enable dumping of recompiled block code size statistics.
// #define DUMP_CODE_SIZE_STATS 1

// Enable profiling of JIT blocks.
// #define ENABLE_RECOMPILER_PROFILING 1

#ifdef ENABLE_RECOMPILER
#include "cpu_recompiler.h"
#endif

#include <map>
#include <unordered_set>
#include <zlib.h>

namespace CPU::CodeCache {

using LUTRangeList = std::array<std::pair<VirtualMemoryAddress, VirtualMemoryAddress>, 9>;
using PageProtectionArray = std::array<PageProtectionInfo, Bus::RAM_8MB_CODE_PAGE_COUNT>;
using BlockInstructionInfoPair = std::pair<Instruction, InstructionInfo>;
using BlockInstructionList = std::vector<BlockInstructionInfoPair>;

// Switch to manual protection if we invalidate more than 4 times within 60 frames.
// Fall blocks back to interpreter if we recompile more than 3 times within 15 frames.
// The interpreter fallback is set before the manual protection switch, so that if it's just a single block
// which is constantly getting mutated, we won't hurt the performance of the rest in the page.
static constexpr u32 RECOMPILE_COUNT_FOR_INTERPRETER_FALLBACK = 3;
static constexpr u32 RECOMPILE_FRAMES_FOR_INTERPRETER_FALLBACK = 15;
static constexpr u32 INVALIDATE_COUNT_FOR_MANUAL_PROTECTION = 4;
static constexpr u32 INVALIDATE_FRAMES_FOR_MANUAL_PROTECTION = 60;

static void AllocateLUTs();
static void DeallocateLUTs();
static void ResetCodeLUT();
static void SetCodeLUT(u32 pc, const void* function);
static void InvalidateBlock(Block* block, BlockState new_state);
static void ClearBlocks();

static Block* LookupBlock(u32 pc);
static Block* CreateBlock(u32 pc, const BlockInstructionList& instructions, const BlockMetadata& metadata);
static bool HasBlockLUT(u32 pc);
static bool IsBlockCodeCurrent(const Block* block);
static bool RevalidateBlock(Block* block);
static PageProtectionMode GetProtectionModeForPC(u32 pc);
static PageProtectionMode GetProtectionModeForBlock(const Block* block);
static bool ReadBlockInstructions(u32 start_pc, BlockInstructionList* instructions, BlockMetadata* metadata);
static void FillBlockRegInfo(Block* block);
static void CopyRegInfo(InstructionInfo* dst, const InstructionInfo* src);
static void SetRegAccess(InstructionInfo* inst, Reg reg, bool write);
static void AddBlockToPageList(Block* block);
static void RemoveBlockFromPageList(Block* block);

static Block* CreateCachedInterpreterBlock(u32 pc);
[[noreturn]] static void ExecuteCachedInterpreter();
template<PGXPMode pgxp_mode>
[[noreturn]] static void ExecuteCachedInterpreterImpl();

// Fast map provides lookup from PC to function
// Function pointers are offset so that you don't need to subtract
CodeLUTArray g_code_lut;
static BlockLUTArray s_block_lut;
static std::unique_ptr<const void*[]> s_lut_code_pointers;
static std::unique_ptr<Block*[]> s_lut_block_pointers;
static PageProtectionArray s_page_protection = {};
static std::vector<Block*> s_blocks;

// for compiling - reuse to avoid allocations
static BlockInstructionList s_block_instructions;

static void BacklinkBlocks(u32 pc, const void* dst);
static void UnlinkBlockExits(Block* block);
static void ResetCodeBuffer();

static void CompileASMFunctions();
static bool CompileBlock(Block* block);
static PageFaultHandler::HandlerResult HandleFastmemException(void* exception_pc, void* fault_address, bool is_write);
static void BackpatchLoadStore(void* host_pc, const LoadstoreBackpatchInfo& info);
static void RemoveBackpatchInfoForRange(const void* host_code, u32 size);

static BlockLinkMap s_block_links;
static std::map<const void*, LoadstoreBackpatchInfo> s_fastmem_backpatch_info;
static std::unordered_set<u32> s_fastmem_faulting_pcs;

NORETURN_FUNCTION_POINTER void (*g_enter_recompiler)();
const void* g_compile_or_revalidate_block;
const void* g_check_events_and_dispatch;
const void* g_run_events_and_dispatch;
const void* g_dispatcher;
const void* g_interpret_block;
const void* g_discard_and_recompile_block;

#ifdef ENABLE_RECOMPILER_PROFILING

PerfScope MIPSPerfScope("MIPS");

#endif

#if defined(CPU_ARCH_ARM32)
// Use a smaller code buffer size on AArch32 to have a better chance of being in range.
static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 16 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 4 * 1024 * 1024;
#else
static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 48 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 16 * 1024 * 1024;
#endif

// On Linux ARM32/ARM64, we use a dedicated section in the ELF for storing code. This is because without
// ASLR, or on certain ASLR offsets, the sbrk() heap ends up immediately following the text/data sections,
// which means there isn't a large enough gap to fit within range on ARM32. Also enable it for Android,
// because MAP_FIXED_NOREPLACE may not exist on older kernels.
#if (defined(__linux__) && (defined(CPU_ARCH_ARM32) || defined(CPU_ARCH_ARM64))) || defined(__ANDROID__)
#define USE_CODE_BUFFER_SECTION 1
#ifdef __clang__
#pragma clang section bss = ".jitstorage"
__attribute__((aligned(MAX_HOST_PAGE_SIZE))) static u8 s_code_buffer_ptr[RECOMPILER_CODE_CACHE_SIZE];
#pragma clang section bss = ""
#endif
#else
static u8* s_code_buffer_ptr = nullptr;
#endif

static u8* s_code_ptr = nullptr;
static u8* s_free_code_ptr = nullptr;
static u32 s_code_size = 0;
static u32 s_code_used = 0;

static u8* s_far_code_ptr = nullptr;
static u8* s_free_far_code_ptr = nullptr;
static u32 s_far_code_size = 0;
static u32 s_far_code_used = 0;

#ifdef DUMP_CODE_SIZE_STATS
static u32 s_total_instructions_compiled = 0;
static u32 s_total_host_instructions_emitted = 0;
static u32 s_total_host_code_used_by_instructions = 0;
#endif
} // namespace CPU::CodeCache

bool CPU::CodeCache::IsUsingRecompiler()
{
  return (g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler);
}

bool CPU::CodeCache::IsUsingFastmem()
{
  return (g_settings.cpu_fastmem_mode != CPUFastmemMode::Disabled);
}

bool CPU::CodeCache::ProcessStartup(Error* error)
{
#ifdef USE_CODE_BUFFER_SECTION
  const u8* module_base = static_cast<const u8*>(MemMap::GetBaseAddress());
  INFO_LOG("Using JIT buffer section of size {} at {} (0x{:X} bytes / {} MB away)", sizeof(s_code_buffer_ptr),
           static_cast<void*>(s_code_buffer_ptr), std::abs(static_cast<ptrdiff_t>(s_code_buffer_ptr - module_base)),
           (std::abs(static_cast<ptrdiff_t>(s_code_buffer_ptr - module_base)) + (1024 * 1024 - 1)) / (1024 * 1024));
  const bool code_buffer_allocated =
    MemMap::MemProtect(s_code_buffer_ptr, RECOMPILER_CODE_CACHE_SIZE, PageProtect::ReadWriteExecute);
#else
  s_code_buffer_ptr = static_cast<u8*>(MemMap::AllocateJITMemory(RECOMPILER_CODE_CACHE_SIZE));
  const bool code_buffer_allocated = (s_code_buffer_ptr != nullptr);
#endif
  if (!code_buffer_allocated) [[unlikely]]
  {
    Error::SetStringView(error, "Failed to allocate code storage. The log may contain more information, you will need "
                                "to run DuckStation with -earlyconsole in the command line.");
    return false;
  }

  AllocateLUTs();

  if (!PageFaultHandler::Install(error))
    return false;

  return true;
}

void CPU::CodeCache::ProcessShutdown()
{
  DeallocateLUTs();

#ifndef USE_CODE_BUFFER_SECTION
  MemMap::ReleaseJITMemory(s_code_buffer_ptr, RECOMPILER_CODE_CACHE_SIZE);
#endif
}

void CPU::CodeCache::Reset()
{
  ClearBlocks();

  if (IsUsingRecompiler())
  {
    ResetCodeBuffer();
    CompileASMFunctions();
    ResetCodeLUT();
  }
}

void CPU::CodeCache::Shutdown()
{
  ClearBlocks();
}

void CPU::CodeCache::Execute()
{
  if (IsUsingRecompiler())
  {
    g_enter_recompiler();
    UnreachableCode();
  }
  else
  {
    ExecuteCachedInterpreter();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MARK: - Block Management
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace CPU::CodeCache {
static constexpr u32 GetLUTTableCount(u32 start, u32 end)
{
  return ((end >> LUT_TABLE_SHIFT) - (start >> LUT_TABLE_SHIFT)) + 1;
}

static constexpr LUTRangeList GetLUTRanges()
{
  const LUTRangeList ranges = {{
    {0x00000000, 0x00800000}, // RAM
    {0x1F000000, 0x1F060000}, // EXP1
    {0x1FC00000, 0x1FC80000}, // BIOS

    {0x80000000, 0x80800000}, // RAM
    {0x9F000000, 0x9F060000}, // EXP1
    {0x9FC00000, 0x9FC80000}, // BIOS

    {0xA0000000, 0xA0800000}, // RAM
    {0xBF000000, 0xBF060000}, // EXP1
    {0xBFC00000, 0xBFC80000}  // BIOS
  }};
  return ranges;
}

static constexpr u32 GetLUTSlotCount(bool include_unreachable)
{
  u32 tables = include_unreachable ? 1 : 0; // unreachable table
  for (const auto& [start, end] : GetLUTRanges())
    tables += GetLUTTableCount(start, end);

  return tables * LUT_TABLE_SIZE;
}
} // namespace CPU::CodeCache

void CPU::CodeCache::AllocateLUTs()
{
  constexpr u32 num_code_slots = GetLUTSlotCount(true);
  constexpr u32 num_block_slots = GetLUTSlotCount(false);

  Assert(!s_lut_code_pointers && !s_lut_block_pointers);
  s_lut_code_pointers = std::make_unique<const void*[]>(num_code_slots);
  s_lut_block_pointers = std::make_unique<Block*[]>(num_block_slots);
  std::memset(s_lut_block_pointers.get(), 0, sizeof(Block*) * num_block_slots);

  CodeLUT code_table_ptr = s_lut_code_pointers.get();
  Block** block_table_ptr = s_lut_block_pointers.get();
  CodeLUT const code_table_ptr_end = code_table_ptr + num_code_slots;
  Block** const block_table_ptr_end = block_table_ptr + num_block_slots;

  // Make the unreachable table jump to the invalid code callback.
  MemsetPtrs(code_table_ptr, static_cast<const void*>(nullptr), LUT_TABLE_COUNT);

  // Mark everything as unreachable to begin with.
  for (u32 i = 0; i < LUT_TABLE_COUNT; i++)
  {
    g_code_lut[i] = code_table_ptr;
    s_block_lut[i] = nullptr;
  }

  // Exclude unreachable.
  code_table_ptr += LUT_TABLE_SIZE;

  // Allocate ranges.
  for (const auto& [start, end] : GetLUTRanges())
  {
    const u32 start_slot = start >> LUT_TABLE_SHIFT;
    const u32 count = GetLUTTableCount(start, end);
    for (u32 i = 0; i < count; i++)
    {
      const u32 slot = start_slot + i;

      g_code_lut[slot] = code_table_ptr;
      code_table_ptr += LUT_TABLE_SIZE;

      s_block_lut[slot] = block_table_ptr;
      block_table_ptr += LUT_TABLE_SIZE;
    }
  }

  Assert(code_table_ptr == code_table_ptr_end);
  Assert(block_table_ptr == block_table_ptr_end);
}

void CPU::CodeCache::DeallocateLUTs()
{
  s_lut_block_pointers.reset();
  s_lut_code_pointers.reset();
}

void CPU::CodeCache::ResetCodeLUT()
{
  // Make the unreachable table jump to the invalid code callback.
  MemsetPtrs(s_lut_code_pointers.get(), g_interpret_block, LUT_TABLE_COUNT);

  for (u32 i = 0; i < LUT_TABLE_COUNT; i++)
  {
    // Don't overwrite anything bound to unreachable.
    CodeLUT ptr = g_code_lut[i];
    if (ptr == s_lut_code_pointers.get())
      continue;

    MemsetPtrs(ptr, g_compile_or_revalidate_block, LUT_TABLE_SIZE);
  }
}

void CPU::CodeCache::SetCodeLUT(u32 pc, const void* function)
{
  const u32 table = pc >> LUT_TABLE_SHIFT;
  const u32 idx = (pc & 0xFFFF) >> 2;
  DebugAssert(g_code_lut[table] != s_lut_code_pointers.get());
  g_code_lut[table][idx] = function;
}

CPU::CodeCache::Block* CPU::CodeCache::LookupBlock(u32 pc)
{
  const u32 table = pc >> LUT_TABLE_SHIFT;
  if (!s_block_lut[table])
    return nullptr;

  const u32 idx = (pc & 0xFFFF) >> 2;
  return s_block_lut[table][idx];
}

bool CPU::CodeCache::HasBlockLUT(u32 pc)
{
  const u32 table = pc >> LUT_TABLE_SHIFT;
  return (s_block_lut[table] != nullptr);
}

CPU::CodeCache::Block* CPU::CodeCache::CreateBlock(u32 pc, const BlockInstructionList& instructions,
                                                   const BlockMetadata& metadata)
{
  const u32 size = static_cast<u32>(instructions.size());
  const u32 table = pc >> LUT_TABLE_SHIFT;
  Assert(s_block_lut[table]);

  // retain from old block
  const u32 frame_number = System::GetFrameNumber();
  u32 recompile_frame = System::GetFrameNumber();
  u8 recompile_count = 0;

  const u32 idx = (pc & 0xFFFF) >> 2;
  Block* block = s_block_lut[table][idx];
  if (block)
  {
    // shouldn't be in the page list.. since we should come here after invalidating
    Assert(!block->next_block_in_page);

    // keep recompile stats before resetting, that way we actually count recompiles
    recompile_frame = block->compile_frame;
    recompile_count = block->compile_count;

    // if it has the same number of instructions, we can reuse it
    if (block->size != size)
    {
      // this sucks.. hopefully won't happen very often
      // TODO: allocate max size, allow shrink but not grow
      auto it = std::find(s_blocks.begin(), s_blocks.end(), block);
      Assert(it != s_blocks.end());
      s_blocks.erase(it);

      block->~Block();
      Common::AlignedFree(block);
      block = nullptr;
    }
  }

  if (!block)
  {
    block = static_cast<Block*>(Common::AlignedMalloc(
      sizeof(Block) + (sizeof(Instruction) * size) + (sizeof(InstructionInfo) * size), alignof(Block)));
    Assert(block);
    new (block) Block();
    s_blocks.push_back(block);
  }

  block->pc = pc;
  block->size = size;
  block->host_code = nullptr;
  block->next_block_in_page = nullptr;
  block->num_exit_links = 0;
  block->state = BlockState::Valid;
  block->flags = metadata.flags;
  block->protection = GetProtectionModeForBlock(block);
  block->uncached_fetch_ticks = metadata.uncached_fetch_ticks;
  block->icache_line_count = metadata.icache_line_count;
  block->host_code_size = 0;
  block->compile_frame = recompile_frame;
  block->compile_count = recompile_count + 1;

  // copy instructions/info
  {
    const std::pair<Instruction, InstructionInfo>* ip = instructions.data();
    Instruction* dsti = block->Instructions();
    InstructionInfo* dstii = block->InstructionsInfo();

    for (u32 i = 0; i < size; i++, ip++, dsti++, dstii++)
    {
      dsti->bits = ip->first.bits;
      *dstii = ip->second;
    }
  }

  s_block_lut[table][idx] = block;

  // if the block is being recompiled too often, leave it in the list, but don't compile it.
  const u32 frame_delta = frame_number - recompile_frame;
  if (frame_delta >= RECOMPILE_FRAMES_FOR_INTERPRETER_FALLBACK)
  {
    block->compile_frame = frame_number;
    block->compile_count = 1;
  }
  else if (block->compile_count >= RECOMPILE_COUNT_FOR_INTERPRETER_FALLBACK)
  {
    DEV_LOG("{} recompiles in {} frames to block 0x{:08X}, not caching.", block->compile_count, frame_delta, block->pc);
    block->size = 0;
  }

  // cached interpreter creates empty blocks when falling back
  if (block->size == 0)
  {
    block->state = BlockState::FallbackToInterpreter;
    block->protection = PageProtectionMode::Unprotected;
    return block;
  }

  // populate backpropogation information for liveness queries
  FillBlockRegInfo(block);

  // add it to the tracking list for its page
  AddBlockToPageList(block);

  return block;
}

bool CPU::CodeCache::IsBlockCodeCurrent(const Block* block)
{
  // blocks shouldn't be wrapping..
  const PhysicalMemoryAddress phys_addr = VirtualAddressToPhysical(block->pc);
  DebugAssert((phys_addr + (sizeof(Instruction) * block->size)) <= Bus::g_ram_size);

  // can just do a straight memcmp..
  return (std::memcmp(Bus::g_ram + phys_addr, block->Instructions(), sizeof(Instruction) * block->size) == 0);
}

bool CPU::CodeCache::RevalidateBlock(Block* block)
{
  DebugAssert(block->state != BlockState::Valid);
  DebugAssert(AddressInRAM(block->pc) || block->state == BlockState::NeedsRecompile);

  if (block->state >= BlockState::NeedsRecompile)
    return false;

  // Protection may have changed if we didn't execute before it got invalidated again. e.g. THPS2.
  if (block->protection != GetProtectionModeForBlock(block))
    return false;

  if (!IsBlockCodeCurrent(block))
  {
    // changed, needs recompiling
    DEBUG_LOG("Block at PC {:08X} has changed and needs recompiling", block->pc);
    return false;
  }

  block->state = BlockState::Valid;
  AddBlockToPageList(block);
  return true;
}

void CPU::CodeCache::AddBlockToPageList(Block* block)
{
  DebugAssert(block->size > 0);
  if (!AddressInRAM(block->pc) || block->protection != PageProtectionMode::WriteProtected)
    return;

  const u32 page_idx = block->StartPageIndex();
  PageProtectionInfo& entry = s_page_protection[page_idx];
  Bus::SetRAMCodePage(page_idx);

  if (entry.last_block_in_page)
  {
    entry.last_block_in_page->next_block_in_page = block;
    entry.last_block_in_page = block;
  }
  else
  {
    entry.first_block_in_page = block;
    entry.last_block_in_page = block;
  }
}

void CPU::CodeCache::RemoveBlockFromPageList(Block* block)
{
  DebugAssert(block->size > 0);
  if (!AddressInRAM(block->pc) || block->protection != PageProtectionMode::WriteProtected)
    return;

  const u32 page_idx = block->StartPageIndex();
  PageProtectionInfo& entry = s_page_protection[page_idx];

  // unlink from list
  Block* prev_block = nullptr;
  Block* cur_block = entry.first_block_in_page;
  while (cur_block)
  {
    if (cur_block != block)
    {
      prev_block = cur_block;
      cur_block = cur_block->next_block_in_page;
      continue;
    }

    if (prev_block)
      prev_block->next_block_in_page = cur_block->next_block_in_page;
    else
      entry.first_block_in_page = cur_block->next_block_in_page;
    if (!cur_block->next_block_in_page)
      entry.last_block_in_page = prev_block;

    cur_block->next_block_in_page = nullptr;
    break;
  }
}

void CPU::CodeCache::InvalidateBlocksWithPageIndex(u32 index)
{
  DebugAssert(index < Bus::RAM_8MB_CODE_PAGE_COUNT);
  Bus::ClearRAMCodePage(index);

  BlockState new_block_state = BlockState::Invalidated;
  PageProtectionInfo& ppi = s_page_protection[index];

  const u32 frame_number = System::GetFrameNumber();
  const u32 frame_delta = frame_number - ppi.invalidate_frame;
  ppi.invalidate_count++;

  if (frame_delta >= INVALIDATE_FRAMES_FOR_MANUAL_PROTECTION)
  {
    ppi.invalidate_count = 1;
    ppi.invalidate_frame = frame_number;
  }
  else if (ppi.invalidate_count > INVALIDATE_COUNT_FOR_MANUAL_PROTECTION)
  {
    DEV_LOG("{} invalidations in {} frames to page {} [0x{:08X} -> 0x{:08X}], switching to manual protection",
            ppi.invalidate_count, frame_delta, index, (index << HOST_PAGE_SHIFT), ((index + 1) << HOST_PAGE_SHIFT));
    ppi.mode = PageProtectionMode::ManualCheck;
    new_block_state = BlockState::NeedsRecompile;
  }

  if (!ppi.first_block_in_page)
    return;

  MemMap::BeginCodeWrite();

  Block* block = ppi.first_block_in_page;
  while (block)
  {
    InvalidateBlock(block, new_block_state);
    block = std::exchange(block->next_block_in_page, nullptr);
  }

  ppi.first_block_in_page = nullptr;
  ppi.last_block_in_page = nullptr;

  MemMap::EndCodeWrite();
}

CPU::CodeCache::PageProtectionMode CPU::CodeCache::GetProtectionModeForPC(u32 pc)
{
  if (!AddressInRAM(pc))
    return PageProtectionMode::Unprotected;

  const u32 page_idx = Bus::GetRAMCodePageIndex(pc);
  return s_page_protection[page_idx].mode;
}

CPU::CodeCache::PageProtectionMode CPU::CodeCache::GetProtectionModeForBlock(const Block* block)
{
  // if the block has a branch delay slot crossing a page, we must use manual protection.
  // no other way about it.
  if (block->HasFlag(BlockFlags::BranchDelaySpansPages))
    return PageProtectionMode::ManualCheck;

  return GetProtectionModeForPC(block->pc);
}

void CPU::CodeCache::InvalidateBlock(Block* block, BlockState new_state)
{
  if (block->state == BlockState::Valid)
  {
    SetCodeLUT(block->pc, g_compile_or_revalidate_block);
    BacklinkBlocks(block->pc, g_compile_or_revalidate_block);
  }

  block->state = new_state;
}

void CPU::CodeCache::InvalidateAllRAMBlocks()
{
  // TODO: maybe combine the backlink into one big instruction flush cache?
  MemMap::BeginCodeWrite();

  for (Block* block : s_blocks)
  {
    if (AddressInRAM(block->pc))
    {
      InvalidateBlock(block, BlockState::Invalidated);
      block->next_block_in_page = nullptr;
    }
  }

  for (PageProtectionInfo& ppi : s_page_protection)
  {
    ppi.first_block_in_page = nullptr;
    ppi.last_block_in_page = nullptr;
  }

  MemMap::EndCodeWrite();
  Bus::ClearRAMCodePageFlags();
}

void CPU::CodeCache::ClearBlocks()
{
  for (u32 i = 0; i < Bus::RAM_8MB_CODE_PAGE_COUNT; i++)
  {
    PageProtectionInfo& ppi = s_page_protection[i];
    if (ppi.mode == PageProtectionMode::WriteProtected && ppi.first_block_in_page)
      Bus::ClearRAMCodePage(i);

    ppi = {};
  }

  s_fastmem_backpatch_info.clear();
  s_fastmem_faulting_pcs.clear();
  s_block_links.clear();

  for (Block* block : s_blocks)
  {
    block->~Block();
    Common::AlignedFree(block);
  }
  s_blocks.clear();

  std::memset(s_lut_block_pointers.get(), 0, sizeof(Block*) * GetLUTSlotCount(false));
}

PageFaultHandler::HandlerResult PageFaultHandler::HandlePageFault(void* exception_pc, void* fault_address,
                                                                  bool is_write)
{
  if (static_cast<const u8*>(fault_address) >= Bus::g_ram &&
      static_cast<const u8*>(fault_address) < (Bus::g_ram + Bus::RAM_8MB_SIZE))
  {
    // Writing to protected RAM.
    DebugAssert(is_write);
    const u32 guest_address = static_cast<u32>(static_cast<const u8*>(fault_address) - Bus::g_ram);
    const u32 page_index = Bus::GetRAMCodePageIndex(guest_address);
    DEV_LOG("Page fault on protected RAM @ 0x{:08X} (page #{}), invalidating code cache.", guest_address, page_index);
    CPU::CodeCache::InvalidateBlocksWithPageIndex(page_index);
    return PageFaultHandler::HandlerResult::ContinueExecution;
  }

  return CPU::CodeCache::HandleFastmemException(exception_pc, fault_address, is_write);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MARK: - Cached Interpreter
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CPU::CodeCache::Block* CPU::CodeCache::CreateCachedInterpreterBlock(u32 pc)
{
  BlockMetadata metadata = {};
  ReadBlockInstructions(pc, &s_block_instructions, &metadata);
  return CreateBlock(pc, s_block_instructions, metadata);
}

template<PGXPMode pgxp_mode>
[[noreturn]] void CPU::CodeCache::ExecuteCachedInterpreterImpl()
{
#define CHECK_DOWNCOUNT()                                                                                              \
  if (g_state.pending_ticks >= g_state.downcount)                                                                      \
    break;

  if (g_state.pending_ticks >= g_state.downcount)
    TimingEvents::RunEvents();

  for (;;)
  {
    for (;;)
    {
#if 0
      LogCurrentState();
#endif
#if 0
      if ((g_state.pending_ticks + TimingEvents::GetGlobalTickCounter()) == 3301006214)
        __debugbreak();
#endif
      // Manually done because we don't want to compile blocks without a LUT.
      const u32 pc = g_state.pc;
      const u32 table = pc >> LUT_TABLE_SHIFT;
      Block* block;
      if (s_block_lut[table])
      {
        const u32 idx = (pc & 0xFFFF) >> 2;
        block = s_block_lut[table][idx];
      }
      else
      {
        // Likely invalid code...
        goto interpret_block;
      }

    reexecute_block:
      if (!block)
      {
        if ((block = CreateCachedInterpreterBlock(pc))->size == 0) [[unlikely]]
          goto interpret_block;
      }
      else
      {
        if (block->state == BlockState::FallbackToInterpreter) [[unlikely]]
          goto interpret_block;

        if ((block->state != BlockState::Valid && !RevalidateBlock(block)) ||
            (block->protection == PageProtectionMode::ManualCheck && !IsBlockCodeCurrent(block)))
        {
          if ((block = CreateCachedInterpreterBlock(pc))->size == 0) [[unlikely]]
            goto interpret_block;
        }
      }

      DebugAssert(!(HasPendingInterrupt()));
      if (block->HasFlag(BlockFlags::IsUsingICache))
      {
        CheckAndUpdateICacheTags(block->icache_line_count);
      }
      else if (block->HasFlag(BlockFlags::NeedsDynamicFetchTicks))
      {
        AddPendingTicks(static_cast<TickCount>(
          block->size * static_cast<u32>(*Bus::GetMemoryAccessTimePtr(block->pc & KSEG_MASK, MemoryAccessSize::Word))));
      }
      else
      {
        AddPendingTicks(block->uncached_fetch_ticks);
      }

      InterpretCachedBlock<pgxp_mode>(block);

      CHECK_DOWNCOUNT();

      // Handle self-looping blocks
      if (g_state.pc == block->pc)
        goto reexecute_block;
      else
        continue;

    interpret_block:
      InterpretUncachedBlock<pgxp_mode>();
      CHECK_DOWNCOUNT();
      continue;
    }

    TimingEvents::RunEvents();
  }
}

[[noreturn]] void CPU::CodeCache::ExecuteCachedInterpreter()
{
  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_pgxp_cpu)
      ExecuteCachedInterpreterImpl<PGXPMode::CPU>();
    else
      ExecuteCachedInterpreterImpl<PGXPMode::Memory>();
  }
  else
  {
    ExecuteCachedInterpreterImpl<PGXPMode::Disabled>();
  }
}

void CPU::CodeCache::LogCurrentState()
{
#if 0
  if (System::GetGlobalTickCounter() == 2546728915)
    __debugbreak();
#endif
#if 0
  if (System::GetGlobalTickCounter() < 2546729174)
    return;
#endif

  const auto& regs = g_state.regs;
  WriteToExecutionLog(
    "tick=%" PRIu64
    " dc=%u/%u pc=%08X at=%08X v0=%08X v1=%08X a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X t2=%08X t3=%08X t4=%08X "
    "t5=%08X t6=%08X t7=%08X s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X s6=%08X s7=%08X t8=%08X t9=%08X k0=%08X "
    "k1=%08X gp=%08X sp=%08X fp=%08X ra=%08X hi=%08X lo=%08X ldr=%s ldv=%08X cause=%08X sr=%08X gte=%08X\n",
    System::GetGlobalTickCounter(), g_state.pending_ticks, g_state.downcount, g_state.pc, regs.at, regs.v0, regs.v1,
    regs.a0, regs.a1, regs.a2, regs.a3, regs.t0, regs.t1, regs.t2, regs.t3, regs.t4, regs.t5, regs.t6, regs.t7, regs.s0,
    regs.s1, regs.s2, regs.s3, regs.s4, regs.s5, regs.s6, regs.s7, regs.t8, regs.t9, regs.k0, regs.k1, regs.gp, regs.sp,
    regs.fp, regs.ra, regs.hi, regs.lo,
    (g_state.next_load_delay_reg == Reg::count) ? "NONE" : GetRegName(g_state.next_load_delay_reg),
    (g_state.next_load_delay_reg == Reg::count) ? 0 : g_state.next_load_delay_value, g_state.cop0_regs.cause.bits,
    g_state.cop0_regs.sr.bits, static_cast<u32>(crc32(0, (const Bytef*)&g_state.gte_regs, sizeof(g_state.gte_regs))));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MARK: - Block Compilation: Shared Code
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool CPU::CodeCache::ReadBlockInstructions(u32 start_pc, BlockInstructionList* instructions, BlockMetadata* metadata)
{
  // TODO: Jump to other block if it exists at this pc?

  const PageProtectionMode protection = GetProtectionModeForPC(start_pc);
  const bool use_icache = CPU::IsCachedAddress(start_pc);
  const bool dynamic_fetch_ticks =
    (!use_icache && Bus::GetMemoryAccessTimePtr(VirtualAddressToPhysical(start_pc), MemoryAccessSize::Word) != nullptr);
  u32 pc = start_pc;
  bool is_branch_delay_slot = false;
  bool is_load_delay_slot = false;

#if 0
  if (pc == 0x0005aa90)
    __debugbreak();
#endif

  instructions->clear();
  metadata->icache_line_count = 0;
  metadata->uncached_fetch_ticks = 0;
  metadata->flags = use_icache ? BlockFlags::IsUsingICache :
                                 (dynamic_fetch_ticks ? BlockFlags::NeedsDynamicFetchTicks : BlockFlags::None);

  u32 last_cache_line = ICACHE_LINES;
  u32 last_page = (protection == PageProtectionMode::WriteProtected) ? Bus::GetRAMCodePageIndex(start_pc) : 0;

  for (;;)
  {
    if (protection == PageProtectionMode::WriteProtected)
    {
      const u32 this_page = Bus::GetRAMCodePageIndex(pc);
      if (this_page != last_page)
      {
        // if we're just crossing the page and not in a branch delay slot, jump directly to the next block
        if (!is_branch_delay_slot)
        {
          DEV_LOG("Breaking block 0x{:08X} at 0x{:08X} due to page crossing", start_pc, pc);
          metadata->flags |= BlockFlags::SpansPages;
          break;
        }
        else
        {
          // otherwise, we need to use manual protection in case the delay slot changes.
          // may as well keep going then, since we're doing manual check anyways.
          DEV_LOG("Block 0x{:08X} has branch delay slot crossing page at 0x{:08X}, forcing manual protection", start_pc,
                  pc);
          metadata->flags |= BlockFlags::BranchDelaySpansPages;
        }
      }
    }

    Instruction instruction;
    if (!SafeReadInstruction(pc, &instruction.bits) || !IsValidInstruction(instruction))
    {
      // Away to the int you go!
      ERROR_LOG("Instruction read failed at PC=0x{:08X}, truncating block.", pc);

      // If the last instruction was a branch, we need the delay slot in the block to compile it.
      if (is_branch_delay_slot)
        instructions->pop_back();

      break;
    }

    InstructionInfo info;
    std::memset(&info, 0, sizeof(info));

    info.is_branch_delay_slot = is_branch_delay_slot;
    info.is_load_delay_slot = is_load_delay_slot;
    info.is_branch_instruction = IsBranchInstruction(instruction);
    info.is_direct_branch_instruction = IsDirectBranchInstruction(instruction);
    info.is_unconditional_branch_instruction = IsUnconditionalBranchInstruction(instruction);
    info.is_load_instruction = IsMemoryLoadInstruction(instruction);
    info.is_store_instruction = IsMemoryStoreInstruction(instruction);
    info.has_load_delay = InstructionHasLoadDelay(instruction);

    if (use_icache)
    {
      if (g_settings.cpu_recompiler_icache)
      {
        const u32 icache_line = GetICacheLine(pc);
        if (icache_line != last_cache_line)
        {
          metadata->icache_line_count++;
          last_cache_line = icache_line;
        }
      }
    }
    else if (!dynamic_fetch_ticks)
    {
      metadata->uncached_fetch_ticks += GetInstructionReadTicks(pc);
    }

    if (info.is_load_instruction || info.is_store_instruction)
      metadata->flags |= BlockFlags::ContainsLoadStoreInstructions;

    pc += sizeof(Instruction);

    if (is_branch_delay_slot && info.is_branch_instruction)
    {
      const BlockInstructionInfoPair& prev = instructions->back();
      if (!prev.second.is_unconditional_branch_instruction || !prev.second.is_direct_branch_instruction)
      {
        WARNING_LOG("Conditional or indirect branch delay slot at {:08X}, skipping block", pc);
        return false;
      }
      if (!IsDirectBranchInstruction(instruction))
      {
        WARNING_LOG("Indirect branch in delay slot at {:08X}, skipping block", pc);
        return false;
      }

      // we _could_ fetch the delay slot from the first branch's target, but it's probably in a different
      // page, and that's an invalidation nightmare. so just fallback to the int, this is very rare anyway.
      WARNING_LOG("Direct branch in delay slot at {:08X}, skipping block", pc);
      return false;
    }

    // instruction is decoded now
    instructions->emplace_back(instruction, info);

    // if we're in a branch delay slot, the block is now done
    // except if this is a branch in a branch delay slot, then we grab the one after that, and so on...
    if (is_branch_delay_slot && !info.is_branch_instruction)
      break;

    // if this is a branch, we grab the next instruction (delay slot), and then exit
    is_branch_delay_slot = info.is_branch_instruction;

    // same for load delay
    is_load_delay_slot = info.has_load_delay;

    // is this a non-branchy exit? (e.g. syscall)
    if (IsExitBlockInstruction(instruction))
      break;
  }

  if (instructions->empty())
  {
    WARNING_LOG("Empty block compiled at 0x{:08X}", start_pc);
    return false;
  }

  instructions->back().second.is_last_instruction = true;

#if defined(_DEBUG) || defined(_DEVEL)
  SmallString disasm;
  u32 disasm_pc = start_pc;
  DEBUG_LOG("Block at 0x{:08X}", start_pc);
  DEBUG_LOG(" Uncached fetch ticks: {}", metadata->uncached_fetch_ticks);
  DEBUG_LOG(" ICache line count: {}", metadata->icache_line_count);
  for (const auto& cbi : *instructions)
  {
    CPU::DisassembleInstruction(&disasm, disasm_pc, cbi.first.bits);
    DEBUG_LOG("[{} {} 0x{:08X}] {:08X} {}", cbi.second.is_branch_delay_slot ? "BD" : "  ",
              cbi.second.is_load_delay_slot ? "LD" : "  ", disasm_pc, cbi.first.bits, disasm);
    disasm_pc += sizeof(Instruction);
  }
#endif

  return true;
}

void CPU::CodeCache::CopyRegInfo(InstructionInfo* dst, const InstructionInfo* src)
{
  std::memcpy(dst->reg_flags, src->reg_flags, sizeof(dst->reg_flags));
  std::memcpy(dst->read_reg, src->read_reg, sizeof(dst->read_reg));
}

void CPU::CodeCache::SetRegAccess(InstructionInfo* inst, Reg reg, bool write)
{
  if (reg == Reg::zero)
    return;

  if (!write)
  {
    for (u32 i = 0; i < std::size(inst->read_reg); i++)
    {
      if (inst->read_reg[i] == Reg::zero)
      {
        inst->read_reg[i] = reg;
        break;
      }
    }
  }
  else
  {
#if 0
    for (u32 i = 0; i < std::size(inst->write_reg); i++)
    {
      if (inst->write_reg[i] == Reg::zero)
      {
        inst->write_reg[i] = reg;
        break;
      }
    }
#endif
  }
}

#define BackpropSetReads(reg)                                                                                          \
  do                                                                                                                   \
  {                                                                                                                    \
    if (!(inst->reg_flags[static_cast<u8>(reg)] & RI_USED))                                                            \
      inst->reg_flags[static_cast<u8>(reg)] |= RI_LASTUSE;                                                             \
    prev->reg_flags[static_cast<u8>(reg)] |= RI_LIVE | RI_USED;                                                        \
    inst->reg_flags[static_cast<u8>(reg)] |= RI_USED;                                                                  \
    SetRegAccess(inst, reg, false);                                                                                    \
  } while (0)

#define BackpropSetWrites(reg)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    prev->reg_flags[static_cast<u8>(reg)] &= ~(RI_LIVE | RI_USED);                                                     \
    if (!(inst->reg_flags[static_cast<u8>(reg)] & RI_USED))                                                            \
      inst->reg_flags[static_cast<u8>(reg)] |= RI_LASTUSE;                                                             \
    inst->reg_flags[static_cast<u8>(reg)] |= RI_USED;                                                                  \
    SetRegAccess(inst, reg, true);                                                                                     \
  } while (0)

// TODO: memory loads should be delayed one instruction because of stupid load delays.
#define BackpropSetWritesDelayed(reg) BackpropSetWrites(reg)

void CPU::CodeCache::FillBlockRegInfo(Block* block)
{
  const Instruction* iinst = block->Instructions() + (block->size - 1);
  InstructionInfo* const start = block->InstructionsInfo();
  InstructionInfo* inst = start + (block->size - 1);
  std::memset(inst->reg_flags, RI_LIVE, sizeof(inst->reg_flags));
  std::memset(inst->read_reg, 0, sizeof(inst->read_reg));
  // std::memset(inst->write_reg, 0, sizeof(inst->write_reg));

  while (inst != start)
  {
    InstructionInfo* prev = inst - 1;
    CopyRegInfo(prev, inst);

    const Reg rs = iinst->r.rs;
    const Reg rt = iinst->r.rt;

    switch (iinst->op)
    {
      case InstructionOp::funct:
      {
        const Reg rd = iinst->r.rd;

        switch (iinst->r.funct)
        {
          case InstructionFunct::sll:
          case InstructionFunct::srl:
          case InstructionFunct::sra:
            BackpropSetWrites(rd);
            BackpropSetReads(rt);
            break;

          case InstructionFunct::sllv:
          case InstructionFunct::srlv:
          case InstructionFunct::srav:
          case InstructionFunct::add:
          case InstructionFunct::addu:
          case InstructionFunct::sub:
          case InstructionFunct::subu:
          case InstructionFunct::and_:
          case InstructionFunct::or_:
          case InstructionFunct::xor_:
          case InstructionFunct::nor:
          case InstructionFunct::slt:
          case InstructionFunct::sltu:
            BackpropSetWrites(rd);
            BackpropSetReads(rt);
            BackpropSetReads(rs);
            break;

          case InstructionFunct::jr:
            BackpropSetReads(rs);
            break;

          case InstructionFunct::jalr:
            BackpropSetReads(rs);
            BackpropSetWrites(rd);
            break;

          case InstructionFunct::mfhi:
            BackpropSetWrites(rd);
            BackpropSetReads(Reg::hi);
            break;

          case InstructionFunct::mflo:
            BackpropSetWrites(rd);
            BackpropSetReads(Reg::lo);
            break;

          case InstructionFunct::mthi:
            BackpropSetWrites(Reg::hi);
            BackpropSetReads(rs);
            break;

          case InstructionFunct::mtlo:
            BackpropSetWrites(Reg::lo);
            BackpropSetReads(rs);
            break;

          case InstructionFunct::mult:
          case InstructionFunct::multu:
          case InstructionFunct::div:
          case InstructionFunct::divu:
            BackpropSetWrites(Reg::hi);
            BackpropSetWrites(Reg::lo);
            BackpropSetReads(rs);
            BackpropSetReads(rt);
            break;

          case InstructionFunct::syscall:
          case InstructionFunct::break_:
            break;

          default:
            ERROR_LOG("Unknown funct {}", static_cast<u32>(iinst->r.funct.GetValue()));
            break;
        }
      }
      break;

      case InstructionOp::b:
      {
        if ((static_cast<u8>(iinst->i.rt.GetValue()) & u8(0x1E)) == u8(0x10))
          BackpropSetWrites(Reg::ra);
        BackpropSetReads(rs);
      }
      break;

      case InstructionOp::j:
        break;

      case InstructionOp::jal:
        BackpropSetWrites(Reg::ra);
        break;

      case InstructionOp::beq:
      case InstructionOp::bne:
        BackpropSetReads(rs);
        BackpropSetReads(rt);
        break;

      case InstructionOp::blez:
      case InstructionOp::bgtz:
        BackpropSetReads(rs);
        break;

      case InstructionOp::addi:
      case InstructionOp::addiu:
      case InstructionOp::slti:
      case InstructionOp::sltiu:
      case InstructionOp::andi:
      case InstructionOp::ori:
      case InstructionOp::xori:
        BackpropSetWrites(rt);
        BackpropSetReads(rs);
        break;

      case InstructionOp::lui:
        BackpropSetWrites(rt);
        break;

      case InstructionOp::lb:
      case InstructionOp::lh:
      case InstructionOp::lw:
      case InstructionOp::lbu:
      case InstructionOp::lhu:
        BackpropSetWritesDelayed(rt);
        BackpropSetReads(rs);
        break;

      case InstructionOp::lwl:
      case InstructionOp::lwr:
        BackpropSetWritesDelayed(rt);
        BackpropSetReads(rs);
        BackpropSetReads(rt);
        break;

      case InstructionOp::sb:
      case InstructionOp::sh:
      case InstructionOp::swl:
      case InstructionOp::sw:
      case InstructionOp::swr:
        BackpropSetReads(rt);
        BackpropSetReads(rs);
        break;

      case InstructionOp::cop0:
      case InstructionOp::cop2:
      {
        if (iinst->cop.IsCommonInstruction())
        {
          switch (iinst->cop.CommonOp())
          {
            case CopCommonInstruction::mfcn:
            case CopCommonInstruction::cfcn:
              BackpropSetWritesDelayed(rt);
              break;

            case CopCommonInstruction::mtcn:
            case CopCommonInstruction::ctcn:
              BackpropSetReads(rt);
              break;
          }
        }
        break;

        case InstructionOp::lwc2:
        case InstructionOp::swc2:
          BackpropSetReads(rs);
          BackpropSetReads(rt);
          break;

        default:
          ERROR_LOG("Unknown op {}", static_cast<u32>(iinst->op.GetValue()));
          break;
      }
    } // end switch

    inst--;
    iinst--;
  } // end while
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MARK: - Recompiler Glue
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CPU::CodeCache::CompileOrRevalidateBlock(u32 start_pc)
{
  // TODO: this doesn't currently handle when the cache overflows...
  DebugAssert(IsUsingRecompiler());
  MemMap::BeginCodeWrite();

  Block* block = LookupBlock(start_pc);
  if (block)
  {
    // we should only be here if the block got invalidated
    DebugAssert(block->state != BlockState::Valid);
    if (RevalidateBlock(block))
    {
      DebugAssert(block->host_code);
      SetCodeLUT(start_pc, block->host_code);
      BacklinkBlocks(start_pc, block->host_code);
      MemMap::EndCodeWrite();
      return;
    }

    // remove outward links from this block, since we're recompiling it
    UnlinkBlockExits(block);

    // clean up backpatch info so it doesn't keep growing indefinitely
    if (block->HasFlag(BlockFlags::ContainsLoadStoreInstructions))
      RemoveBackpatchInfoForRange(block->host_code, block->host_code_size);
  }

  BlockMetadata metadata = {};
  if (!ReadBlockInstructions(start_pc, &s_block_instructions, &metadata))
  {
    ERROR_LOG("Failed to read block at 0x{:08X}, falling back to uncached interpreter", start_pc);
    SetCodeLUT(start_pc, g_interpret_block);
    BacklinkBlocks(start_pc, g_interpret_block);
    MemMap::EndCodeWrite();
    return;
  }

  // Ensure we're not going to run out of space while compiling this block.
  // We could definitely do better here...
  const u32 block_size = static_cast<u32>(s_block_instructions.size());
  const u32 free_code_space = GetFreeCodeSpace();
  const u32 free_far_code_space = GetFreeFarCodeSpace();
  if (free_code_space < (block_size * Recompiler::MAX_NEAR_HOST_BYTES_PER_INSTRUCTION) ||
      free_code_space < Recompiler::MIN_CODE_RESERVE_FOR_BLOCK ||
      free_far_code_space < Recompiler::MIN_CODE_RESERVE_FOR_BLOCK)
  {
    ERROR_LOG("Out of code space while compiling {:08X}. Resetting code cache.", start_pc);
    CodeCache::Reset();
  }

  if ((block = CreateBlock(start_pc, s_block_instructions, metadata)) == nullptr || block->size == 0 ||
      !CompileBlock(block))
  {
    ERROR_LOG("Failed to compile block at 0x{:08X}, falling back to uncached interpreter", start_pc);
    SetCodeLUT(start_pc, g_interpret_block);
    BacklinkBlocks(start_pc, g_interpret_block);
    MemMap::EndCodeWrite();
    return;
  }

  SetCodeLUT(start_pc, block->host_code);
  BacklinkBlocks(start_pc, block->host_code);
  MemMap::EndCodeWrite();
}

void CPU::CodeCache::DiscardAndRecompileBlock(u32 start_pc)
{
  MemMap::BeginCodeWrite();

  DEV_LOG("Discard block {:08X} with manual protection", start_pc);
  Block* block = LookupBlock(start_pc);
  DebugAssert(block && block->state == BlockState::Valid);
  InvalidateBlock(block, BlockState::NeedsRecompile);
  CompileOrRevalidateBlock(start_pc);

  MemMap::EndCodeWrite();
}

const void* CPU::CodeCache::CreateBlockLink(Block* block, void* code, u32 newpc)
{
  // self-linking should be handled by the caller
  DebugAssert(newpc != block->pc);

  const void* dst = g_dispatcher;
  if (g_settings.cpu_recompiler_block_linking)
  {
    const Block* next_block = LookupBlock(newpc);
    if (next_block)
    {
      dst = (next_block->state == BlockState::Valid) ?
              next_block->host_code :
              ((next_block->state == BlockState::FallbackToInterpreter) ? g_interpret_block :
                                                                          g_compile_or_revalidate_block);
      DebugAssert(dst);
    }
    else
    {
      dst = HasBlockLUT(newpc) ? g_compile_or_revalidate_block : g_interpret_block;
    }

    BlockLinkMap::iterator iter = s_block_links.emplace(newpc, code);
    DebugAssert(block->num_exit_links < MAX_BLOCK_EXIT_LINKS);
    block->exit_links[block->num_exit_links++] = iter;
  }

  DEBUG_LOG("Linking {} with dst pc {:08X} to {}{}", code, newpc, dst,
            (dst == g_compile_or_revalidate_block) ? "[compiler]" : "");
  return dst;
}

const void* CPU::CodeCache::CreateSelfBlockLink(Block* block, void* code, const void* block_start)
{
  const void* dst = g_dispatcher;
  if (g_settings.cpu_recompiler_block_linking)
  {
    dst = block_start;

    BlockLinkMap::iterator iter = s_block_links.emplace(block->pc, code);
    DebugAssert(block->num_exit_links < MAX_BLOCK_EXIT_LINKS);
    block->exit_links[block->num_exit_links++] = iter;
  }

  DEBUG_LOG("Self linking {} with dst pc {:08X} to {}", code, block->pc, dst);
  return dst;
}

void CPU::CodeCache::BacklinkBlocks(u32 pc, const void* dst)
{
  if (!g_settings.cpu_recompiler_block_linking)
    return;

  const auto link_range = s_block_links.equal_range(pc);
  for (auto it = link_range.first; it != link_range.second; ++it)
  {
    DEBUG_LOG("Backlinking {} with dst pc {:08X} to {}{}", it->second, pc, dst,
              (dst == g_compile_or_revalidate_block) ? "[compiler]" : "");
    EmitJump(it->second, dst, true);
  }
}

void CPU::CodeCache::UnlinkBlockExits(Block* block)
{
  const u32 num_exit_links = block->num_exit_links;
  for (u32 i = 0; i < num_exit_links; i++)
    s_block_links.erase(block->exit_links[i]);
  block->num_exit_links = 0;
}

void CPU::CodeCache::ResetCodeBuffer()
{
  if (s_code_used > 0 || s_far_code_used > 0)
  {
    MemMap::BeginCodeWrite();

    if (s_code_used > 0)
    {
      std::memset(s_code_ptr, 0, s_code_used);
      MemMap::FlushInstructionCache(s_code_ptr, s_code_used);
    }

    if (s_far_code_used > 0)
    {
      std::memset(s_far_code_ptr, 0, s_far_code_used);
      MemMap::FlushInstructionCache(s_far_code_ptr, s_far_code_used);
    }

    MemMap::EndCodeWrite();
  }

  s_code_ptr = static_cast<u8*>(s_code_buffer_ptr);
  s_free_code_ptr = s_code_ptr;
  s_code_size = RECOMPILER_CODE_CACHE_SIZE - RECOMPILER_FAR_CODE_CACHE_SIZE;
  s_code_used = 0;

  // Use half the far code size when memory exceptions aren't enabled. It's only used for backpatching.
  const u32 far_code_size = (!g_settings.cpu_recompiler_memory_exceptions) ? (RECOMPILER_FAR_CODE_CACHE_SIZE / 2) :
                                                                             RECOMPILER_FAR_CODE_CACHE_SIZE;
  s_far_code_size = far_code_size;
  s_far_code_ptr = (far_code_size > 0) ? (static_cast<u8*>(s_code_ptr) + s_code_size) : nullptr;
  s_free_far_code_ptr = s_far_code_ptr;
  s_far_code_used = 0;
}

u8* CPU::CodeCache::GetFreeCodePointer()
{
  return s_free_code_ptr;
}

u32 CPU::CodeCache::GetFreeCodeSpace()
{
  return s_code_size - s_code_used;
}

void CPU::CodeCache::CommitCode(u32 length)
{
  if (length == 0) [[unlikely]]
    return;

  MemMap::FlushInstructionCache(s_free_code_ptr, length);

  Assert(length <= (s_code_size - s_code_used));
  s_free_code_ptr += length;
  s_code_used += length;
}

u8* CPU::CodeCache::GetFreeFarCodePointer()
{
  return s_free_far_code_ptr;
}

u32 CPU::CodeCache::GetFreeFarCodeSpace()
{
  return s_far_code_size - s_far_code_used;
}

void CPU::CodeCache::CommitFarCode(u32 length)
{
  if (length == 0) [[unlikely]]
    return;

  MemMap::FlushInstructionCache(s_free_far_code_ptr, length);

  Assert(length <= (s_far_code_size - s_far_code_used));
  s_free_far_code_ptr += length;
  s_far_code_used += length;
}

void CPU::CodeCache::AlignCode(u32 alignment)
{
  DebugAssert(Common::IsPow2(alignment));
  const u32 num_padding_bytes =
    std::min(static_cast<u32>(Common::AlignUpPow2(reinterpret_cast<uintptr_t>(s_free_code_ptr), alignment) -
                              reinterpret_cast<uintptr_t>(s_free_code_ptr)),
             GetFreeCodeSpace());

  if (num_padding_bytes > 0)
    EmitAlignmentPadding(s_free_code_ptr, num_padding_bytes);

  s_free_code_ptr += num_padding_bytes;
  s_code_used += num_padding_bytes;
}

const void* CPU::CodeCache::GetInterpretUncachedBlockFunction()
{
  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_pgxp_cpu)
      return reinterpret_cast<const void*>(InterpretUncachedBlock<PGXPMode::CPU>);
    else
      return reinterpret_cast<const void*>(InterpretUncachedBlock<PGXPMode::Memory>);
  }
  else
  {
    return reinterpret_cast<const void*>(InterpretUncachedBlock<PGXPMode::Disabled>);
  }
}

void CPU::CodeCache::CompileASMFunctions()
{
  MemMap::BeginCodeWrite();

#ifdef DUMP_CODE_SIZE_STATS
  s_total_instructions_compiled = 0;
  s_total_host_instructions_emitted = 0;
  s_total_host_code_used_by_instructions = 0;
#endif

  const u32 asm_size = EmitASMFunctions(GetFreeCodePointer(), GetFreeCodeSpace());

#ifdef ENABLE_RECOMPILER_PROFILING
  MIPSPerfScope.Register(GetFreeCodePointer(), asm_size, "ASMFunctions");
#endif

  CommitCode(asm_size);
  MemMap::EndCodeWrite();
}

bool CPU::CodeCache::CompileBlock(Block* block)
{
  const void* host_code = nullptr;
  u32 host_code_size = 0;
  u32 host_far_code_size = 0;

#ifdef ENABLE_RECOMPILER
  if (g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler)
    host_code = g_compiler->CompileBlock(block, &host_code_size, &host_far_code_size);
#endif

  block->host_code = host_code;
  block->host_code_size = host_code_size;

  if (!host_code)
  {
    ERROR_LOG("Failed to compile host code for block at 0x{:08X}", block->pc);
    block->state = BlockState::FallbackToInterpreter;
    return false;
  }

#ifdef DUMP_CODE_SIZE_STATS
  const u32 host_instructions = GetHostInstructionCount(host_code, host_code_size);
  s_total_instructions_compiled += block->size;
  s_total_host_instructions_emitted += host_instructions;
  s_total_host_code_used_by_instructions += host_code_size;

  DEV_LOG(
    "0x{:08X}: {}/{}b for {}b ({}i), blowup: {:.2f}x, cache: {:.2f}%/{:.2f}%, ipi: {:.2f}/{:.2f}, bpi: {:.2f}/{:.2f}",
    block->pc, host_code_size, host_far_code_size, block->size * 4, block->size,
    static_cast<float>(host_code_size) / static_cast<float>(block->size * 4),
    (static_cast<float>(s_code_used) / static_cast<float>(s_code_size)) * 100.0f,
    (static_cast<float>(s_far_code_used) / static_cast<float>(s_far_code_size)) * 100.0f,
    static_cast<float>(host_instructions) / static_cast<float>(block->size),
    static_cast<float>(s_total_host_instructions_emitted) / static_cast<float>(s_total_instructions_compiled),
    static_cast<float>(block->host_code_size) / static_cast<float>(block->size),
    static_cast<float>(s_total_host_code_used_by_instructions) / static_cast<float>(s_total_instructions_compiled));
#endif

#if 0
  Log_DebugPrint("***HOST CODE**");
  DisassembleAndLogHostCode(host_code, host_code_size);
#endif

#ifdef ENABLE_RECOMPILER_PROFILING
  MIPSPerfScope.RegisterPC(host_code, host_code_size, block->pc);
#endif

  return true;
}

void CPU::CodeCache::AddLoadStoreInfo(void* code_address, u32 code_size, u32 guest_pc, const void* thunk_address)
{
  DebugAssert(code_size < std::numeric_limits<u8>::max());

  auto iter = s_fastmem_backpatch_info.find(code_address);
  if (iter != s_fastmem_backpatch_info.end())
    s_fastmem_backpatch_info.erase(iter);

  LoadstoreBackpatchInfo info;
  info.thunk_address = thunk_address;
  info.guest_pc = guest_pc;
  info.guest_block = 0;
  info.code_size = static_cast<u8>(code_size);
  s_fastmem_backpatch_info.emplace(code_address, info);
}

void CPU::CodeCache::AddLoadStoreInfo(void* code_address, u32 code_size, u32 guest_pc, u32 guest_block,
                                      TickCount cycles, u32 gpr_bitmask, u8 address_register, u8 data_register,
                                      MemoryAccessSize size, bool is_signed, bool is_load)
{
  DebugAssert(code_size < std::numeric_limits<u8>::max());
  DebugAssert(cycles >= 0 && cycles < std::numeric_limits<u16>::max());

  auto iter = s_fastmem_backpatch_info.find(code_address);
  if (iter != s_fastmem_backpatch_info.end())
    s_fastmem_backpatch_info.erase(iter);

  LoadstoreBackpatchInfo info;
  info.thunk_address = nullptr;
  info.guest_pc = guest_pc;
  info.guest_block = guest_block;
  info.gpr_bitmask = gpr_bitmask;
  info.cycles = static_cast<u16>(cycles);
  info.address_register = address_register;
  info.data_register = data_register;
  info.size = static_cast<u16>(size);
  info.is_signed = is_signed;
  info.is_load = is_load;
  info.code_size = static_cast<u8>(code_size);
  s_fastmem_backpatch_info.emplace(code_address, info);
}

PageFaultHandler::HandlerResult CPU::CodeCache::HandleFastmemException(void* exception_pc, void* fault_address,
                                                                       bool is_write)
{
  PhysicalMemoryAddress guest_address;

#ifdef ENABLE_MMAP_FASTMEM
  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    if (static_cast<u8*>(fault_address) < static_cast<u8*>(g_state.fastmem_base) ||
        (static_cast<u8*>(fault_address) - static_cast<u8*>(g_state.fastmem_base)) >=
          static_cast<ptrdiff_t>(Bus::FASTMEM_ARENA_SIZE))
    {
      return PageFaultHandler::HandlerResult::ExecuteNextHandler;
    }

    guest_address = static_cast<PhysicalMemoryAddress>(
      static_cast<ptrdiff_t>(static_cast<u8*>(fault_address) - static_cast<u8*>(g_state.fastmem_base)));

    // if we're writing to ram, let it go through a few times, and use manual block protection to sort it out
    // TODO: path for manual protection to return back to read-only pages
    if (!g_state.cop0_regs.sr.Isc && GetSegmentForAddress(guest_address) != CPU::Segment::KSEG2 &&
        AddressInRAM(guest_address))
    {
      DebugAssert(is_write);
      DEV_LOG("Ignoring fault due to RAM write @ 0x{:08X}", guest_address);
      InvalidateBlocksWithPageIndex(Bus::GetRAMCodePageIndex(guest_address));
      return PageFaultHandler::HandlerResult::ContinueExecution;
    }
  }
  else
#endif
  {
    // LUT fastmem - we can't compute the address.
    guest_address = std::numeric_limits<PhysicalMemoryAddress>::max();
  }

  auto iter = s_fastmem_backpatch_info.find(exception_pc);
  if (iter == s_fastmem_backpatch_info.end())
    return PageFaultHandler::HandlerResult::ExecuteNextHandler;

  DEV_LOG("Page fault handler invoked at PC={} Address={} {}, fastmem offset {:08X}", exception_pc, fault_address,
          is_write ? "(write)" : "(read)", guest_address);

  LoadstoreBackpatchInfo& info = iter->second;
  DEV_LOG("Backpatching {} at {}[{}] (pc {:08X} addr {:08X}): Bitmask {:08X} Addr {} Data {} Size {} Signed {:02X}",
          info.is_load ? "load" : "store", exception_pc, info.code_size, info.guest_pc, guest_address, info.gpr_bitmask,
          static_cast<unsigned>(info.address_register), static_cast<unsigned>(info.data_register),
          info.AccessSizeInBytes(), static_cast<unsigned>(info.is_signed));

  MemMap::BeginCodeWrite();

  BackpatchLoadStore(exception_pc, info);

  // queue block for recompilation later
  Block* block = LookupBlock(info.guest_block);
  if (block)
  {
    // This is a bit annoying, we have to remove it from the page list if it's a RAM block.
    DEV_LOG("Queuing block {:08X} for recompilation due to backpatch", block->pc);
    RemoveBlockFromPageList(block);
    InvalidateBlock(block, BlockState::NeedsRecompile);

    // Need to reset the recompile count, otherwise it'll get trolled into an interpreter fallback.
    block->compile_frame = System::GetFrameNumber();
    block->compile_count = 1;
  }

  MemMap::EndCodeWrite();

  // and store the pc in the faulting list, so that we don't emit another fastmem loadstore
  s_fastmem_faulting_pcs.insert(info.guest_pc);
  s_fastmem_backpatch_info.erase(iter);
  return PageFaultHandler::HandlerResult::ContinueExecution;
}

bool CPU::CodeCache::HasPreviouslyFaultedOnPC(u32 guest_pc)
{
  return (s_fastmem_faulting_pcs.find(guest_pc) != s_fastmem_faulting_pcs.end());
}

void CPU::CodeCache::BackpatchLoadStore(void* host_pc, const LoadstoreBackpatchInfo& info)
{
#ifdef ENABLE_RECOMPILER
  if (g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler)
    Recompiler::BackpatchLoadStore(host_pc, info);
#endif
}

void CPU::CodeCache::RemoveBackpatchInfoForRange(const void* host_code, u32 size)
{
  const u8* start = static_cast<const u8*>(host_code);
  const u8* end = start + size;

  auto start_iter = s_fastmem_backpatch_info.lower_bound(start);
  if (start_iter == s_fastmem_backpatch_info.end())
    return;

  // this might point to another block, so bail out in that case
  if (start_iter->first >= end)
    return;

  // find the end point, or last instruction in the range
  auto end_iter = start_iter;
  do
  {
    ++end_iter;
  } while (end_iter != s_fastmem_backpatch_info.end() && end_iter->first < end);

  // erase the whole range at once
  s_fastmem_backpatch_info.erase(start_iter, end_iter);
}
