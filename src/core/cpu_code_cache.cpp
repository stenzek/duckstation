// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "bus.h"
#include "cpu_code_cache_private.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "cpu_recompiler_types.h"
#include "settings.h"
#include "system.h"
#include "timing_event.h"

#include "common/assert.h"
#include "common/intrin.h"
#include "common/log.h"
#include "common/memmap.h"

Log_SetChannel(CPU::CodeCache);

#ifdef ENABLE_RECOMPILER
#include "cpu_recompiler_code_generator.h"
#endif

#ifdef ENABLE_NEWREC
#include "cpu_newrec_compiler.h"
#endif

#include <unordered_set>
#include <zlib.h>

namespace CPU::CodeCache {

using LUTRangeList = std::array<std::pair<VirtualMemoryAddress, VirtualMemoryAddress>, 9>;
using PageProtectionArray = std::array<PageProtectionInfo, Bus::RAM_8MB_CODE_PAGE_COUNT>;
using BlockInstructionInfoPair = std::pair<Instruction, InstructionInfo>;
using BlockInstructionList = std::vector<BlockInstructionInfoPair>;

// Switch to manual protection if we invalidate more than 4 times within 20 frames.
// Fall blocks back to interpreter if we recompile more than 3 times within 15 frames.
// The interpreter fallback is set before the manual protection switch, so that if it's just a single block
// which is constantly getting mutated, we won't hurt the performance of the rest in the page.
static constexpr u32 RECOMPILE_COUNT_FOR_INTERPRETER_FALLBACK = 3;
static constexpr u32 RECOMPILE_FRAMES_FOR_INTERPRETER_FALLBACK = 15;
static constexpr u32 INVALIDATE_COUNT_FOR_MANUAL_PROTECTION = 4;
static constexpr u32 INVALIDATE_FRAMES_FOR_MANUAL_PROTECTION = 20;

static CodeLUT DecodeCodeLUTPointer(u32 slot, CodeLUT ptr);
static CodeLUT EncodeCodeLUTPointer(u32 slot, CodeLUT ptr);
static CodeLUT OffsetCodeLUTPointer(CodeLUT fake_ptr, u32 pc);

static void AllocateLUTs();
static void DeallocateLUTs();
static void ResetCodeLUT();
static void SetCodeLUT(u32 pc, const void* function);
static void InvalidateBlock(Block* block, BlockState new_state);
static void ClearBlocks();

static Block* LookupBlock(u32 pc);
static Block* CreateBlock(u32 pc, const BlockInstructionList& instructions, const BlockMetadata& metadata);
static bool IsBlockCodeCurrent(const Block* block);
static bool RevalidateBlock(Block* block);
PageProtectionMode GetProtectionModeForPC(u32 pc);
PageProtectionMode GetProtectionModeForBlock(const Block* block);
static bool ReadBlockInstructions(u32 start_pc, BlockInstructionList* instructions, BlockMetadata* metadata);
static void FillBlockRegInfo(Block* block);
static void CopyRegInfo(InstructionInfo* dst, const InstructionInfo* src);
static void SetRegAccess(InstructionInfo* inst, Reg reg, bool write);
static void AddBlockToPageList(Block* block);
static void RemoveBlockFromPageList(Block* block);

static Common::PageFaultHandler::HandlerResult ExceptionHandler(void* exception_pc, void* fault_address, bool is_write);

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

#ifdef ENABLE_RECOMPILER_SUPPORT

static void BacklinkBlocks(u32 pc, const void* dst);
static void UnlinkBlockExits(Block* block);

static void ClearASMFunctions();
static void CompileASMFunctions();
static bool CompileBlock(Block* block);
static Common::PageFaultHandler::HandlerResult HandleFastmemException(void* exception_pc, void* fault_address,
                                                                      bool is_write);
static void BackpatchLoadStore(void* host_pc, const LoadstoreBackpatchInfo& info);

static BlockLinkMap s_block_links;
static std::unordered_map<const void*, LoadstoreBackpatchInfo> s_fastmem_backpatch_info;
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

// Currently remapping the code buffer doesn't work in macOS. TODO: Make dynamic instead...
#ifndef __APPLE__
#define USE_STATIC_CODE_BUFFER 1
#endif

#if defined(CPU_ARCH_ARM32)
// Use a smaller code buffer size on AArch32 to have a better chance of being in range.
static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 16 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 8 * 1024 * 1024;
#else
static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 32 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 16 * 1024 * 1024;
#endif

#ifdef USE_STATIC_CODE_BUFFER
static constexpr u32 RECOMPILER_GUARD_SIZE = 4096;
alignas(HOST_PAGE_SIZE) static u8 s_code_storage[RECOMPILER_CODE_CACHE_SIZE + RECOMPILER_FAR_CODE_CACHE_SIZE];
#endif

static JitCodeBuffer s_code_buffer;

#ifdef _DEBUG
static u32 s_total_instructions_compiled = 0;
static u32 s_total_host_instructions_emitted = 0;
#endif

#endif // ENABLE_RECOMPILER_SUPPORT
} // namespace CPU::CodeCache

bool CPU::CodeCache::IsUsingAnyRecompiler()
{
#ifdef ENABLE_RECOMPILER_SUPPORT
  return (g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler ||
          g_settings.cpu_execution_mode == CPUExecutionMode::NewRec);
#else
  return false;
#endif
}

bool CPU::CodeCache::IsUsingFastmem()
{
  return IsUsingAnyRecompiler() && g_settings.cpu_fastmem_mode != CPUFastmemMode::Disabled;
}

void CPU::CodeCache::ProcessStartup()
{
  AllocateLUTs();

#ifdef ENABLE_RECOMPILER_SUPPORT
#ifdef USE_STATIC_CODE_BUFFER
  const bool has_buffer = s_code_buffer.Initialize(s_code_storage, sizeof(s_code_storage),
                                                   RECOMPILER_FAR_CODE_CACHE_SIZE, RECOMPILER_GUARD_SIZE);
#else
  const bool has_buffer = false;
#endif
  if (!has_buffer && !s_code_buffer.Allocate(RECOMPILER_CODE_CACHE_SIZE, RECOMPILER_FAR_CODE_CACHE_SIZE))
  {
    Panic("Failed to initialize code space");
  }
#endif

  if (!Common::PageFaultHandler::InstallHandler(ExceptionHandler))
    Panic("Failed to install page fault handler");
}

void CPU::CodeCache::ProcessShutdown()
{
  Common::PageFaultHandler::RemoveHandler(ExceptionHandler);

#ifdef ENABLE_RECOMPILER_SUPPORT
  s_code_buffer.Destroy();
#endif

  DeallocateLUTs();
}

void CPU::CodeCache::Initialize()
{
  Assert(s_blocks.empty());

#ifdef ENABLE_RECOMPILER_SUPPORT
  if (IsUsingAnyRecompiler())
  {
    s_code_buffer.Reset();
    CompileASMFunctions();
    ResetCodeLUT();
  }
#endif

  Bus::UpdateFastmemViews(IsUsingAnyRecompiler() ? g_settings.cpu_fastmem_mode : CPUFastmemMode::Disabled);
  CPU::UpdateMemoryPointers();
}

void CPU::CodeCache::Shutdown()
{
  ClearBlocks();

#ifdef ENABLE_RECOMPILER_SUPPORT
  ClearASMFunctions();
#endif

  Bus::UpdateFastmemViews(CPUFastmemMode::Disabled);
  CPU::UpdateMemoryPointers();
}

void CPU::CodeCache::Reset()
{
  ClearBlocks();

#ifdef ENABLE_RECOMPILER_SUPPORT
  if (IsUsingAnyRecompiler())
  {
    ClearASMFunctions();
    s_code_buffer.Reset();
    CompileASMFunctions();
    ResetCodeLUT();
  }
#endif
}

void CPU::CodeCache::Execute()
{
#ifdef ENABLE_RECOMPILER_SUPPORT
  if (IsUsingAnyRecompiler())
  {
    g_enter_recompiler();
    UnreachableCode();
  }
  else
  {
    ExecuteCachedInterpreter();
  }
#else
  ExecuteCachedInterpreter();
#endif
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
    {0x1F000000, 0x1F800000}, // EXP1
    {0x1FC00000, 0x1FC80000}, // BIOS

    {0x80000000, 0x80800000}, // RAM
    {0x9F000000, 0x9F800000}, // EXP1
    {0x9FC00000, 0x9FC80000}, // BIOS

    {0xA0000000, 0xA0800000}, // RAM
    {0xBF000000, 0xBF800000}, // EXP1
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

CPU::CodeCache::CodeLUT CPU::CodeCache::DecodeCodeLUTPointer(u32 slot, CodeLUT ptr)
{
  if constexpr (sizeof(void*) == 8)
    return reinterpret_cast<CodeLUT>(reinterpret_cast<u8*>(ptr) + (static_cast<u64>(slot) << 17));
  else
    return reinterpret_cast<CodeLUT>(reinterpret_cast<u8*>(ptr) + (slot << 16));
}

CPU::CodeCache::CodeLUT CPU::CodeCache::EncodeCodeLUTPointer(u32 slot, CodeLUT ptr)
{
  if constexpr (sizeof(void*) == 8)
    return reinterpret_cast<CodeLUT>(reinterpret_cast<u8*>(ptr) - (static_cast<u64>(slot) << 17));
  else
    return reinterpret_cast<CodeLUT>(reinterpret_cast<u8*>(ptr) - (slot << 16));
}

CPU::CodeCache::CodeLUT CPU::CodeCache::OffsetCodeLUTPointer(CodeLUT fake_ptr, u32 pc)
{
  u8* fake_byte_ptr = reinterpret_cast<u8*>(fake_ptr);
  if constexpr (sizeof(void*) == 8)
    return reinterpret_cast<const void**>(fake_byte_ptr + (static_cast<u64>(pc) << 1));
  else
    return reinterpret_cast<const void**>(fake_byte_ptr + pc);
}

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
    g_code_lut[i] = EncodeCodeLUTPointer(i, code_table_ptr);
    s_block_lut[i] = nullptr;
  }
  code_table_ptr += LUT_TABLE_SIZE;

  // Allocate ranges.
  for (const auto& [start, end] : GetLUTRanges())
  {
    const u32 start_slot = start >> LUT_TABLE_SHIFT;
    const u32 count = GetLUTTableCount(start, end);
    for (u32 i = 0; i < count; i++)
    {
      const u32 slot = start_slot + i;

      g_code_lut[slot] = EncodeCodeLUTPointer(slot, code_table_ptr);
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
  if (!s_lut_code_pointers)
    return;

  // Make the unreachable table jump to the invalid code callback.
  MemsetPtrs(s_lut_code_pointers.get(), g_interpret_block, LUT_TABLE_COUNT);

  for (u32 i = 0; i < LUT_TABLE_COUNT; i++)
  {
    CodeLUT ptr = DecodeCodeLUTPointer(i, g_code_lut[i]);
    if (ptr == s_lut_code_pointers.get())
      continue;

    MemsetPtrs(ptr, g_compile_or_revalidate_block, LUT_TABLE_SIZE);
  }
}

void CPU::CodeCache::SetCodeLUT(u32 pc, const void* function)
{
  if (!s_lut_code_pointers)
    return;

  const u32 table = pc >> LUT_TABLE_SHIFT;
  CodeLUT encoded_ptr = g_code_lut[table];

#ifdef _DEBUG
  const CodeLUT table_ptr = DecodeCodeLUTPointer(table, encoded_ptr);
  DebugAssert(table_ptr != nullptr && table_ptr != s_lut_code_pointers.get());
#endif

  *OffsetCodeLUTPointer(encoded_ptr, pc) = function;
}

CPU::CodeCache::Block* CPU::CodeCache::LookupBlock(u32 pc)
{
  const u32 table = pc >> LUT_TABLE_SHIFT;
  if (!s_block_lut[table])
    return nullptr;

  const u32 idx = (pc & 0xFFFF) >> 2;
  return s_block_lut[table][idx];
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
      std::free(block);
      block = nullptr;
    }
  }

  if (!block)
  {
    block =
      static_cast<Block*>(std::malloc(sizeof(Block) + (sizeof(Instruction) * size) + (sizeof(InstructionInfo) * size)));
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
    Log_DevFmt("{} recompiles in {} frames to block 0x{:08X}, not caching.", block->compile_count, frame_delta,
               block->pc);
    block->size = 0;
  }

  // cached interpreter creates empty blocks when falling back
  if (block->size == 0)
  {
    block->state = BlockState::FallbackToInterpreter;
    block->protection = PageProtectionMode::Unprotected;
    return block;
  }

  // Old rec doesn't use backprop info, don't waste time filling it.
  if (g_settings.cpu_execution_mode == CPUExecutionMode::NewRec)
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
    Log_DebugPrintf("Block at PC %08X has changed and needs recompiling", block->pc);
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
    Log_DevFmt("{} invalidations in {} frames to page {} [0x{:08X} -> 0x{:08X}], switching to manual protection",
               ppi.invalidate_count, frame_delta, index, (index * HOST_PAGE_SIZE), ((index + 1) * HOST_PAGE_SIZE));
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
#ifdef ENABLE_RECOMPILER_SUPPORT
  if (block->state == BlockState::Valid)
  {
    SetCodeLUT(block->pc, g_compile_or_revalidate_block);
    BacklinkBlocks(block->pc, g_compile_or_revalidate_block);
  }
#endif

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

#ifdef ENABLE_RECOMPILER_SUPPORT
  s_fastmem_backpatch_info.clear();
  s_fastmem_faulting_pcs.clear();
  s_block_links.clear();
#endif

  for (Block* block : s_blocks)
  {
    block->~Block();
    std::free(block);
  }
  s_blocks.clear();

  std::memset(s_lut_block_pointers.get(), 0, sizeof(Block*) * GetLUTSlotCount(false));
}

Common::PageFaultHandler::HandlerResult CPU::CodeCache::ExceptionHandler(void* exception_pc, void* fault_address,
                                                                         bool is_write)
{
  if (static_cast<const u8*>(fault_address) >= Bus::g_ram &&
      static_cast<const u8*>(fault_address) < (Bus::g_ram + Bus::RAM_8MB_SIZE))
  {
    // Writing to protected RAM.
    DebugAssert(is_write);
    const u32 guest_address = static_cast<u32>(static_cast<const u8*>(fault_address) - Bus::g_ram);
    const u32 page_index = Bus::GetRAMCodePageIndex(guest_address);
    Log_DevFmt("Page fault on protected RAM @ 0x{:08X} (page #{}), invalidating code cache.", guest_address,
               page_index);
    InvalidateBlocksWithPageIndex(page_index);
    return Common::PageFaultHandler::HandlerResult::ContinueExecution;
  }

#ifdef ENABLE_RECOMPILER_SUPPORT
  return HandleFastmemException(exception_pc, fault_address, is_write);
#else
  return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
#endif
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

  for (;;)
  {
    TimingEvents::RunEvents();

    while (g_state.pending_ticks < g_state.downcount)
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

      // TODO: make DebugAssert
      Assert(!(HasPendingInterrupt()));

      if (g_settings.cpu_recompiler_icache)
        CheckAndUpdateICacheTags(block->icache_line_count, block->uncached_fetch_ticks);

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
  if ((TimingEvents::GetGlobalTickCounter() + GetPendingTicks()) == 2546728915)
    __debugbreak();
#endif
#if 0
  if ((TimingEvents::GetGlobalTickCounter() + GetPendingTicks()) < 2546729174)
    return;
#endif

  const auto& regs = g_state.regs;
  WriteToExecutionLog(
    "tick=%u dc=%u/%u pc=%08X at=%08X v0=%08X v1=%08X a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X t2=%08X t3=%08X "
    "t4=%08X t5=%08X t6=%08X t7=%08X s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X s6=%08X s7=%08X t8=%08X t9=%08X "
    "k0=%08X k1=%08X gp=%08X sp=%08X fp=%08X ra=%08X hi=%08X lo=%08X ldr=%s ldv=%08X cause=%08X sr=%08X gte=%08X\n",
    TimingEvents::GetGlobalTickCounter() + GetPendingTicks(), g_state.pending_ticks, g_state.downcount, g_state.pc,
    regs.at, regs.v0, regs.v1, regs.a0, regs.a1, regs.a2, regs.a3, regs.t0, regs.t1, regs.t2, regs.t3, regs.t4, regs.t5,
    regs.t6, regs.t7, regs.s0, regs.s1, regs.s2, regs.s3, regs.s4, regs.s5, regs.s6, regs.s7, regs.t8, regs.t9, regs.k0,
    regs.k1, regs.gp, regs.sp, regs.fp, regs.ra, regs.hi, regs.lo,
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
  metadata->flags = BlockFlags::None;

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
          Log_DevFmt("Breaking block 0x{:08X} at 0x{:08X} due to page crossing", start_pc, pc);
          metadata->flags |= BlockFlags::SpansPages;
          break;
        }
        else
        {
          // otherwise, we need to use manual protection in case the delay slot changes.
          // may as well keep going then, since we're doing manual check anyways.
          Log_DevFmt("Block 0x{:08X} has branch delay slot crossing page at 0x{:08X}, forcing manual protection",
                     start_pc, pc);
          metadata->flags |= BlockFlags::BranchDelaySpansPages;
        }
      }
    }

    Instruction instruction;
    if (!SafeReadInstruction(pc, &instruction.bits) || !IsInvalidInstruction(instruction))
      break;

    InstructionInfo info;
    std::memset(&info, 0, sizeof(info));

    info.pc = pc;
    info.is_branch_delay_slot = is_branch_delay_slot;
    info.is_load_delay_slot = is_load_delay_slot;
    info.is_branch_instruction = IsBranchInstruction(instruction);
    info.is_direct_branch_instruction = IsDirectBranchInstruction(instruction);
    info.is_unconditional_branch_instruction = IsUnconditionalBranchInstruction(instruction);
    info.is_load_instruction = IsMemoryLoadInstruction(instruction);
    info.is_store_instruction = IsMemoryStoreInstruction(instruction);
    info.has_load_delay = InstructionHasLoadDelay(instruction);
    info.can_trap = CanInstructionTrap(instruction, false /*InUserMode()*/);
    info.is_direct_branch_instruction = IsDirectBranchInstruction(instruction);

    if (g_settings.cpu_recompiler_icache)
    {
      const u32 icache_line = GetICacheLine(pc);
      if (icache_line != last_cache_line)
      {
        metadata->icache_line_count++;
        last_cache_line = icache_line;
      }
    }

    metadata->uncached_fetch_ticks += GetInstructionReadTicks(pc);
    if (info.is_load_instruction || info.is_store_instruction)
      metadata->flags |= BlockFlags::ContainsLoadStoreInstructions;

    pc += sizeof(Instruction);

    if (is_branch_delay_slot && info.is_branch_instruction)
    {
      const BlockInstructionInfoPair& prev = instructions->back();
      if (!prev.second.is_unconditional_branch_instruction || !prev.second.is_direct_branch_instruction)
      {
        Log_WarningPrintf("Conditional or indirect branch delay slot at %08X, skipping block", info.pc);
        return false;
      }
      if (!IsDirectBranchInstruction(instruction))
      {
        Log_WarningPrintf("Indirect branch in delay slot at %08X, skipping block", info.pc);
        return false;
      }

      // we _could_ fetch the delay slot from the first branch's target, but it's probably in a different
      // page, and that's an invalidation nightmare. so just fallback to the int, this is very rare anyway.
      Log_WarningPrintf("Direct branch in delay slot at %08X, skipping block", info.pc);
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
    Log_WarningFmt("Empty block compiled at 0x{:08X}", start_pc);
    return false;
  }

  instructions->back().second.is_last_instruction = true;

#ifdef _DEBUG
  SmallString disasm;
  Log_DebugPrintf("Block at 0x%08X", start_pc);
  for (const auto& cbi : *instructions)
  {
    CPU::DisassembleInstruction(&disasm, cbi.second.pc, cbi.first.bits);
    Log_DebugPrintf("[%s %s 0x%08X] %08X %s", cbi.second.is_branch_delay_slot ? "BD" : "  ",
                    cbi.second.is_load_delay_slot ? "LD" : "  ", cbi.second.pc, cbi.first.bits, disasm.c_str());
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
            Log_ErrorPrintf("Unknown funct %u", static_cast<u32>(iinst->r.funct.GetValue()));
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
          Log_ErrorPrintf("Unknown op %u", static_cast<u32>(iinst->r.funct.GetValue()));
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

#ifdef ENABLE_RECOMPILER_SUPPORT

void CPU::CodeCache::CompileOrRevalidateBlock(u32 start_pc)
{
  // TODO: this doesn't currently handle when the cache overflows...
  DebugAssert(IsUsingAnyRecompiler());
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
  }

  BlockMetadata metadata = {};
  if (!ReadBlockInstructions(start_pc, &s_block_instructions, &metadata))
  {
    Log_ErrorFmt("Failed to read block at 0x{:08X}, falling back to uncached interpreter", start_pc);
    SetCodeLUT(start_pc, g_interpret_block);
    BacklinkBlocks(start_pc, g_interpret_block);
    MemMap::EndCodeWrite();
    return;
  }

  // Ensure we're not going to run out of space while compiling this block.
  // We could definitely do better here... TODO: far code is no longer needed for newrec
  const u32 block_size = static_cast<u32>(s_block_instructions.size());
  if (s_code_buffer.GetFreeCodeSpace() < (block_size * Recompiler::MAX_NEAR_HOST_BYTES_PER_INSTRUCTION) ||
      s_code_buffer.GetFreeFarCodeSpace() < (block_size * Recompiler::MAX_FAR_HOST_BYTES_PER_INSTRUCTION))
  {
    Log_ErrorFmt("Out of code space while compiling {:08X}. Resetting code cache.", start_pc);
    CodeCache::Reset();
  }

  if ((block = CreateBlock(start_pc, s_block_instructions, metadata)) == nullptr || block->size == 0 ||
      !CompileBlock(block))
  {
    Log_ErrorFmt("Failed to compile block at 0x{:08X}, falling back to uncached interpreter", start_pc);
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

  Log_DevPrintf("Discard block %08X with manual protection", start_pc);
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
      dst = g_compile_or_revalidate_block;
    }

    BlockLinkMap::iterator iter = s_block_links.emplace(newpc, code);
    DebugAssert(block->num_exit_links < MAX_BLOCK_EXIT_LINKS);
    block->exit_links[block->num_exit_links++] = iter;
  }

  Log_DebugPrintf("Linking %p with dst pc %08X to %p%s", code, newpc, dst,
                  (dst == g_compile_or_revalidate_block) ? "[compiler]" : "");
  return dst;
}

void CPU::CodeCache::BacklinkBlocks(u32 pc, const void* dst)
{
  if (!g_settings.cpu_recompiler_block_linking)
    return;

  const auto link_range = s_block_links.equal_range(pc);
  for (auto it = link_range.first; it != link_range.second; ++it)
  {
    Log_DebugPrintf("Backlinking %p with dst pc %08X to %p%s", it->second, pc, dst,
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

JitCodeBuffer& CPU::CodeCache::GetCodeBuffer()
{
  return s_code_buffer;
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

void CPU::CodeCache::ClearASMFunctions()
{
  g_enter_recompiler = nullptr;
  g_compile_or_revalidate_block = nullptr;
  g_check_events_and_dispatch = nullptr;
  g_run_events_and_dispatch = nullptr;
  g_dispatcher = nullptr;
  g_interpret_block = nullptr;
  g_discard_and_recompile_block = nullptr;

#ifdef _DEBUG
  s_total_instructions_compiled = 0;
  s_total_host_instructions_emitted = 0;
#endif
}

void CPU::CodeCache::CompileASMFunctions()
{
  MemMap::BeginCodeWrite();

  const u32 asm_size = EmitASMFunctions(s_code_buffer.GetFreeCodePointer(), s_code_buffer.GetFreeCodeSpace());

#ifdef ENABLE_RECOMPILER_PROFILING
  MIPSPerfScope.Register(s_code_buffer.GetFreeCodePointer(), asm_size, "ASMFunctions");
#endif

  s_code_buffer.CommitCode(asm_size);
  MemMap::EndCodeWrite();
}

bool CPU::CodeCache::CompileBlock(Block* block)
{
  const void* host_code = nullptr;
  u32 host_code_size = 0;
  u32 host_far_code_size = 0;

#ifdef ENABLE_RECOMPILER
  if (g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler)
  {
    Recompiler::CodeGenerator codegen(&s_code_buffer);
    host_code = codegen.CompileBlock(block, &host_code_size, &host_far_code_size);
  }
#endif
#ifdef ENABLE_NEWREC
  if (g_settings.cpu_execution_mode == CPUExecutionMode::NewRec)
    host_code = NewRec::g_compiler->CompileBlock(block, &host_code_size, &host_far_code_size);
#endif

  block->host_code = host_code;

  if (!host_code)
  {
    Log_ErrorFmt("Failed to compile host code for block at 0x{:08X}", block->pc);
    block->state = BlockState::FallbackToInterpreter;
    return false;
  }

#ifdef _DEBUG
  const u32 host_instructions = GetHostInstructionCount(host_code, host_code_size);
  s_total_instructions_compiled += block->size;
  s_total_host_instructions_emitted += host_instructions;

  Log_ProfileFmt("0x{:08X}: {}/{}b for {}b ({}i), blowup: {:.2f}x, cache: {:.2f}%/{:.2f}%, ipi: {:.2f}/{:.2f}",
                 block->pc, host_code_size, host_far_code_size, block->size * 4, block->size,
                 static_cast<float>(host_code_size) / static_cast<float>(block->size * 4), s_code_buffer.GetUsedPct(),
                 s_code_buffer.GetFarUsedPct(), static_cast<float>(host_instructions) / static_cast<float>(block->size),
                 static_cast<float>(s_total_host_instructions_emitted) /
                   static_cast<float>(s_total_instructions_compiled));
#else
  Log_ProfileFmt("0x{:08X}: {}/{}b for {}b ({} inst), blowup: {:.2f}x, cache: {:.2f}%/{:.2f}%", block->pc,
                 host_code_size, host_far_code_size, block->size * 4, block->size,
                 static_cast<float>(host_code_size) / static_cast<float>(block->size * 4), s_code_buffer.GetUsedPct(),
                 s_code_buffer.GetFarUsedPct());
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

Common::PageFaultHandler::HandlerResult CPU::CodeCache::HandleFastmemException(void* exception_pc, void* fault_address,
                                                                               bool is_write)
{
  // TODO: Catch general RAM writes, not just fastmem
  PhysicalMemoryAddress guest_address;

#ifdef ENABLE_MMAP_FASTMEM
  if (g_settings.cpu_fastmem_mode == CPUFastmemMode::MMap)
  {
    if (static_cast<u8*>(fault_address) < static_cast<u8*>(g_state.fastmem_base) ||
        (static_cast<u8*>(fault_address) - static_cast<u8*>(g_state.fastmem_base)) >=
          static_cast<ptrdiff_t>(Bus::FASTMEM_ARENA_SIZE))
    {
      return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
    }

    guest_address = static_cast<PhysicalMemoryAddress>(
      static_cast<ptrdiff_t>(static_cast<u8*>(fault_address) - static_cast<u8*>(g_state.fastmem_base)));
  }
  else
#endif
  {
    // LUT fastmem - we can't compute the address.
    guest_address = std::numeric_limits<PhysicalMemoryAddress>::max();
  }

  Log_DevFmt("Page fault handler invoked at PC={} Address={} {}, fastmem offset {:08X}", exception_pc, fault_address,
             is_write ? "(write)" : "(read)", guest_address);

  auto iter = s_fastmem_backpatch_info.find(exception_pc);
  if (iter == s_fastmem_backpatch_info.end())
  {
    Log_ErrorFmt("No backpatch info found for {}", exception_pc);
    return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
  }

  // if we're writing to ram, let it go through a few times, and use manual block protection to sort it out
  // TODO: path for manual protection to return back to read-only pages
  LoadstoreBackpatchInfo& info = iter->second;
  if (is_write && !g_state.cop0_regs.sr.Isc && AddressInRAM(guest_address))
  {
    Log_DevFmt("Ignoring fault due to RAM write @ 0x{:08X}", guest_address);
    InvalidateBlocksWithPageIndex(Bus::GetRAMCodePageIndex(guest_address));
    return Common::PageFaultHandler::HandlerResult::ContinueExecution;
  }

  Log_DevFmt("Backpatching {} at {}[{}] (pc {:08X} addr {:08X}): Bitmask {:08X} Addr {} Data {} Size {} Signed {:02X}",
             info.is_load ? "load" : "store", exception_pc, info.code_size, info.guest_pc, guest_address,
             info.gpr_bitmask, static_cast<unsigned>(info.address_register), static_cast<unsigned>(info.data_register),
             info.AccessSizeInBytes(), static_cast<unsigned>(info.is_signed));

  MemMap::BeginCodeWrite();

  BackpatchLoadStore(exception_pc, info);

  // queue block for recompilation later
  if (g_settings.cpu_execution_mode == CPUExecutionMode::NewRec)
  {
    Block* block = LookupBlock(info.guest_block);
    if (block)
    {
      // This is a bit annoying, we have to remove it from the page list if it's a RAM block.
      Log_DevFmt("Queuing block {:08X} for recompilation due to backpatch", block->pc);
      RemoveBlockFromPageList(block);
      InvalidateBlock(block, BlockState::NeedsRecompile);

      // Need to reset the recompile count, otherwise it'll get trolled into an interpreter fallback.
      block->compile_frame = System::GetFrameNumber();
      block->compile_count = 1;
    }
  }

  MemMap::EndCodeWrite();

  // and store the pc in the faulting list, so that we don't emit another fastmem loadstore
  s_fastmem_faulting_pcs.insert(info.guest_pc);
  s_fastmem_backpatch_info.erase(iter);
  return Common::PageFaultHandler::HandlerResult::ContinueExecution;
}

bool CPU::CodeCache::HasPreviouslyFaultedOnPC(u32 guest_pc)
{
  return (s_fastmem_faulting_pcs.find(guest_pc) != s_fastmem_faulting_pcs.end());
}

void CPU::CodeCache::BackpatchLoadStore(void* host_pc, const LoadstoreBackpatchInfo& info)
{
#ifdef ENABLE_RECOMPILER
  if (g_settings.cpu_execution_mode == CPUExecutionMode::Recompiler)
    Recompiler::CodeGenerator::BackpatchLoadStore(host_pc, info);
#endif
#ifdef ENABLE_NEWREC
  if (g_settings.cpu_execution_mode == CPUExecutionMode::NewRec)
    NewRec::BackpatchLoadStore(host_pc, info);
#endif
}

#endif // ENABLE_RECOMPILER_SUPPORT
