#include "cpu_code_cache.h"
#include "bus.h"
#include "common/assert.h"
#include "common/log.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "settings.h"
#include "system.h"
#include "timing_event.h"
Log_SetChannel(CPU::CodeCache);

#ifdef WITH_RECOMPILER
#include "cpu_recompiler_code_generator.h"
#endif

namespace CPU::CodeCache {

constexpr bool USE_BLOCK_LINKING = true;

#ifdef WITH_RECOMPILER

// Currently remapping the code buffer doesn't work in macOS or Haiku.
#if !defined(__HAIKU__) && !defined(__APPLE__)
#define USE_STATIC_CODE_BUFFER 1
#endif

#if defined(AARCH32)
// Use a smaller code buffer size on AArch32 to have a better chance of being in range.
static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 16 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 8 * 1024 * 1024;
#else
static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 32 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 16 * 1024 * 1024;
#endif
static constexpr u32 CODE_WRITE_FAULT_THRESHOLD_FOR_SLOWMEM = 10;

#ifdef USE_STATIC_CODE_BUFFER
static constexpr u32 RECOMPILER_GUARD_SIZE = 4096;
alignas(Recompiler::CODE_STORAGE_ALIGNMENT) static u8
  s_code_storage[RECOMPILER_CODE_CACHE_SIZE + RECOMPILER_FAR_CODE_CACHE_SIZE];
#endif

static JitCodeBuffer s_code_buffer;

std::array<CodeBlock::HostCodePointer, FAST_MAP_TOTAL_SLOT_COUNT> s_fast_map;
DispatcherFunction s_asm_dispatcher;
SingleBlockDispatcherFunction s_single_block_asm_dispatcher;

ALWAYS_INLINE static u32 GetFastMapIndex(u32 pc)
{
  return ((pc & PHYSICAL_MEMORY_ADDRESS_MASK) >= Bus::BIOS_BASE) ?
           (FAST_MAP_RAM_SLOT_COUNT + ((pc & Bus::BIOS_MASK) >> 2)) :
           ((pc & Bus::RAM_MASK) >> 2);
}

static void CompileDispatcher();
static void FastCompileBlockFunction();

static void ResetFastMap()
{
  s_fast_map.fill(FastCompileBlockFunction);
}

static void SetFastMap(u32 pc, CodeBlock::HostCodePointer function)
{
  s_fast_map[GetFastMapIndex(pc)] = function;
}

#endif

using BlockMap = std::unordered_map<u32, CodeBlock*>;
using HostCodeMap = std::map<CodeBlock::HostCodePointer, CodeBlock*>;

void LogCurrentState();

/// Returns the block key for the current execution state.
static CodeBlockKey GetNextBlockKey();

/// Looks up the block in the cache if it's already been compiled.
static CodeBlock* LookupBlock(CodeBlockKey key);

/// Can the current block execute? This will re-validate the block if necessary.
/// The block can also be flushed if recompilation failed, so ignore the pointer if false is returned.
static bool RevalidateBlock(CodeBlock* block);

static bool CompileBlock(CodeBlock* block);
static void RemoveReferencesToBlock(CodeBlock* block);
static void AddBlockToPageMap(CodeBlock* block);
static void RemoveBlockFromPageMap(CodeBlock* block);

/// Link block from to to.
static void LinkBlock(CodeBlock* from, CodeBlock* to);

/// Unlink all blocks which point to this block, and any that this block links to.
static void UnlinkBlock(CodeBlock* block);

static void ClearState();

static BlockMap s_blocks;
static std::array<std::vector<CodeBlock*>, Bus::RAM_CODE_PAGE_COUNT> m_ram_block_map;

#ifdef WITH_RECOMPILER
static HostCodeMap s_host_code_map;

static void AddBlockToHostCodeMap(CodeBlock* block);
static void RemoveBlockFromHostCodeMap(CodeBlock* block);

static bool InitializeFastmem();
static void ShutdownFastmem();
static Common::PageFaultHandler::HandlerResult LUTPageFaultHandler(void* exception_pc, void* fault_address,
                                                                   bool is_write);
#ifdef WITH_MMAP_FASTMEM
static Common::PageFaultHandler::HandlerResult MMapPageFaultHandler(void* exception_pc, void* fault_address,
                                                                    bool is_write);
#endif
#endif // WITH_RECOMPILER

void Initialize()
{
  Assert(s_blocks.empty());

#ifdef WITH_RECOMPILER
  if (g_settings.IsUsingRecompiler())
  {
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

    if (g_settings.IsUsingFastmem() && !InitializeFastmem())
      Panic("Failed to initialize fastmem");

    ResetFastMap();
    CompileDispatcher();
  }
#endif
}

void ClearState()
{
  Bus::ClearRAMCodePageFlags();
  for (auto& it : m_ram_block_map)
    it.clear();

  for (const auto& it : s_blocks)
    delete it.second;

  s_blocks.clear();
#ifdef WITH_RECOMPILER
  s_host_code_map.clear();
  s_code_buffer.Reset();
  ResetFastMap();
#endif
}

void Shutdown()
{
  ClearState();
#ifdef WITH_RECOMPILER
  ShutdownFastmem();
  s_code_buffer.Destroy();
#endif
}

template<PGXPMode pgxp_mode>
static void ExecuteImpl()
{
  CodeBlockKey next_block_key;

  g_using_interpreter = false;
  g_state.frame_done = false;

  while (!g_state.frame_done)
  {
    if (HasPendingInterrupt())
    {
      SafeReadInstruction(g_state.regs.pc, &g_state.next_instruction.bits);
      DispatchInterrupt();
    }

    TimingEvents::UpdateCPUDowncount();

    next_block_key = GetNextBlockKey();
    while (g_state.pending_ticks < g_state.downcount)
    {
      CodeBlock* block = LookupBlock(next_block_key);
      if (!block)
      {
        InterpretUncachedBlock<pgxp_mode>();
        continue;
      }

    reexecute_block:
      Assert(!(HasPendingInterrupt()));

#if 0
      const u32 tick = TimingEvents::GetGlobalTickCounter() + CPU::GetPendingTicks();
      if (tick == 4188233674)
        __debugbreak();
#endif

#if 0
      LogCurrentState();
#endif

      if (g_settings.cpu_recompiler_icache)
        CheckAndUpdateICacheTags(block->icache_line_count, block->uncached_fetch_ticks);

      InterpretCachedBlock<pgxp_mode>(*block);

      if (g_state.pending_ticks >= g_state.downcount)
        break;
      else if (!USE_BLOCK_LINKING)
        continue;

      next_block_key = GetNextBlockKey();
      if (next_block_key.bits == block->key.bits)
      {
        // we can jump straight to it if there's no pending interrupts
        // ensure it's not a self-modifying block
        if (!block->invalidated || RevalidateBlock(block))
          goto reexecute_block;
      }
      else if (!block->invalidated)
      {
        // Try to find an already-linked block.
        // TODO: Don't need to dereference the block, just store a pointer to the code.
        for (CodeBlock* linked_block : block->link_successors)
        {
          if (linked_block->key.bits == next_block_key.bits)
          {
            if (linked_block->invalidated && !RevalidateBlock(linked_block))
            {
              // CanExecuteBlock can result in a block flush, so stop iterating here.
              break;
            }

            // Execute the linked block
            block = linked_block;
            goto reexecute_block;
          }
        }

        // No acceptable blocks found in the successor list, try a new one.
        CodeBlock* next_block = LookupBlock(next_block_key);
        if (next_block)
        {
          // Link the previous block to this new block if we find a new block.
          LinkBlock(block, next_block);
          block = next_block;
          goto reexecute_block;
        }
      }
    }

    TimingEvents::RunEvents();
  }

  // in case we switch to interpreter...
  g_state.regs.npc = g_state.regs.pc;
}

void Execute()
{
  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_pgxp_cpu)
      ExecuteImpl<PGXPMode::CPU>();
    else
      ExecuteImpl<PGXPMode::Memory>();
  }
  else
  {
    ExecuteImpl<PGXPMode::Disabled>();
  }
}

#ifdef WITH_RECOMPILER

void CompileDispatcher()
{
  {
    Recompiler::CodeGenerator cg(&s_code_buffer);
    s_asm_dispatcher = cg.CompileDispatcher();
  }
  {
    Recompiler::CodeGenerator cg(&s_code_buffer);
    s_single_block_asm_dispatcher = cg.CompileSingleBlockDispatcher();
  }
}

CodeBlock::HostCodePointer* GetFastMapPointer()
{
  return s_fast_map.data();
}

void ExecuteRecompiler()
{
  g_using_interpreter = false;
  g_state.frame_done = false;

#if 0
  while (!g_state.frame_done)
  {
    if (HasPendingInterrupt())
    {
      SafeReadInstruction(g_state.regs.pc, &g_state.next_instruction.bits);
      DispatchInterrupt();
    }

    TimingEvents::UpdateCPUDowncount();

    while (g_state.pending_ticks < g_state.downcount)
    {
      const u32 pc = g_state.regs.pc;
      g_state.current_instruction_pc = pc;
      const u32 fast_map_index = GetFastMapIndex(pc);
      s_single_block_asm_dispatcher[fast_map_index]();
    }

    TimingEvents::RunEvents();
  }
#else
  s_asm_dispatcher();
#endif

  // in case we switch to interpreter...
  g_state.regs.npc = g_state.regs.pc;
}

#endif

void Reinitialize()
{
  ClearState();

#ifdef WITH_RECOMPILER

  ShutdownFastmem();
  s_code_buffer.Destroy();

  if (g_settings.IsUsingRecompiler())
  {

#ifdef USE_STATIC_CODE_BUFFER
    if (!s_code_buffer.Initialize(s_code_storage, sizeof(s_code_storage), RECOMPILER_FAR_CODE_CACHE_SIZE,
                                  RECOMPILER_GUARD_SIZE))
#else
    if (!s_code_buffer.Allocate(RECOMPILER_CODE_CACHE_SIZE, RECOMPILER_FAR_CODE_CACHE_SIZE))
#endif
    {
      Panic("Failed to initialize code space");
    }

    if (g_settings.IsUsingFastmem() && !InitializeFastmem())
      Panic("Failed to initialize fastmem");

    ResetFastMap();
    CompileDispatcher();
  }
#endif
}

void Flush()
{
  ClearState();
#ifdef WITH_RECOMPILER
  if (g_settings.IsUsingRecompiler())
    CompileDispatcher();
#endif
}

void LogCurrentState()
{
  const auto& regs = g_state.regs;
  WriteToExecutionLog("tick=%u pc=%08X zero=%08X at=%08X v0=%08X v1=%08X a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X "
                      "t1=%08X t2=%08X t3=%08X t4=%08X t5=%08X t6=%08X t7=%08X s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X "
                      "s5=%08X s6=%08X s7=%08X t8=%08X t9=%08X k0=%08X k1=%08X gp=%08X sp=%08X fp=%08X ra=%08X ldr=%s "
                      "ldv=%08X\n",
                      TimingEvents::GetGlobalTickCounter() + GetPendingTicks(), regs.pc, regs.zero, regs.at, regs.v0,
                      regs.v1, regs.a0, regs.a1, regs.a2, regs.a3, regs.t0, regs.t1, regs.t2, regs.t3, regs.t4, regs.t5,
                      regs.t6, regs.t7, regs.s0, regs.s1, regs.s2, regs.s3, regs.s4, regs.s5, regs.s6, regs.s7, regs.t8,
                      regs.t9, regs.k0, regs.k1, regs.gp, regs.sp, regs.fp, regs.ra,
                      (g_state.next_load_delay_reg == Reg::count) ? "NONE" : GetRegName(g_state.next_load_delay_reg),
                      (g_state.next_load_delay_reg == Reg::count) ? 0 : g_state.next_load_delay_value);
}

CodeBlockKey GetNextBlockKey()
{
  CodeBlockKey key = {};
  key.SetPC(g_state.regs.pc);
  key.user_mode = InUserMode();
  return key;
}

CodeBlock* LookupBlock(CodeBlockKey key)
{
  BlockMap::iterator iter = s_blocks.find(key.bits);
  if (iter != s_blocks.end())
  {
    // ensure it hasn't been invalidated
    CodeBlock* existing_block = iter->second;
    if (!existing_block || !existing_block->invalidated || RevalidateBlock(existing_block))
      return existing_block;
  }

  CodeBlock* block = new CodeBlock(key);
  if (CompileBlock(block))
  {
    // add it to the page map if it's in ram
    AddBlockToPageMap(block);

#ifdef WITH_RECOMPILER
    SetFastMap(block->GetPC(), block->host_code);
    AddBlockToHostCodeMap(block);
#endif
  }
  else
  {
    Log_ErrorPrintf("Failed to compile block at PC=0x%08X", key.GetPC());
    delete block;
    block = nullptr;
  }

  s_blocks.emplace(key.bits, block);
  return block;
}

bool RevalidateBlock(CodeBlock* block)
{
  for (const CodeBlockInstruction& cbi : block->instructions)
  {
    u32 new_code = 0;
    SafeReadInstruction(cbi.pc, &new_code);
    if (cbi.instruction.bits != new_code)
    {
      Log_DebugPrintf("Block 0x%08X changed at PC 0x%08X - %08X to %08X - recompiling.", block->GetPC(), cbi.pc,
                      cbi.instruction.bits, new_code);
      goto recompile;
    }
  }

  // re-add it to the page map since it's still up-to-date
  block->invalidated = false;
  AddBlockToPageMap(block);
#ifdef WITH_RECOMPILER
  SetFastMap(block->GetPC(), block->host_code);
#endif
  return true;

recompile:
  // remove any references to the block from the lookup table.
  // this is an edge case where compiling causes a flush-all due to no space,
  // and we don't want to nuke the block we're compiling...
  RemoveReferencesToBlock(block);

#ifdef WITH_RECOMPILER
  RemoveBlockFromHostCodeMap(block);
#endif

  block->instructions.clear();
  if (!CompileBlock(block))
  {
    Log_WarningPrintf("Failed to recompile block 0x%08X - flushing.", block->GetPC());
    delete block;
    return false;
  }

  AddBlockToPageMap(block);

#ifdef WITH_RECOMPILER
  // re-add to page map again
  SetFastMap(block->GetPC(), block->host_code);
  AddBlockToHostCodeMap(block);
#endif

  // re-insert into the block map since we removed it earlier.
  s_blocks.emplace(block->key.bits, block);
  return true;
}

bool CompileBlock(CodeBlock* block)
{
  u32 pc = block->GetPC();
  bool is_branch_delay_slot = false;
  bool is_unconditional_branch_delay_slot = false;
  bool is_load_delay_slot = false;

#if 0
  if (pc == 0x0005aa90)
    __debugbreak();
#endif

  block->icache_line_count = 0;
  block->uncached_fetch_ticks = 0;
  block->contains_double_branches = false;
  block->contains_loadstore_instructions = false;

  u32 last_cache_line = ICACHE_LINES;

  for (;;)
  {
    CodeBlockInstruction cbi = {};
    if (!SafeReadInstruction(pc, &cbi.instruction.bits) || !IsInvalidInstruction(cbi.instruction))
      break;

    cbi.pc = pc;
    cbi.is_branch_delay_slot = is_branch_delay_slot;
    cbi.is_load_delay_slot = is_load_delay_slot;
    cbi.is_branch_instruction = IsBranchInstruction(cbi.instruction);
    cbi.is_unconditional_branch_instruction = IsUnconditionalBranchInstruction(cbi.instruction);
    cbi.is_load_instruction = IsMemoryLoadInstruction(cbi.instruction);
    cbi.is_store_instruction = IsMemoryStoreInstruction(cbi.instruction);
    cbi.has_load_delay = InstructionHasLoadDelay(cbi.instruction);
    cbi.can_trap = CanInstructionTrap(cbi.instruction, InUserMode());

    if (g_settings.cpu_recompiler_icache)
    {
      const u32 icache_line = GetICacheLine(pc);
      if (icache_line != last_cache_line)
      {
        block->icache_line_count++;
        last_cache_line = icache_line;
      }
      block->uncached_fetch_ticks += GetInstructionReadTicks(pc);
    }

    block->contains_loadstore_instructions |= cbi.is_load_instruction;
    block->contains_loadstore_instructions |= cbi.is_store_instruction;

    pc += sizeof(cbi.instruction.bits);

    if (is_branch_delay_slot && cbi.is_branch_instruction)
    {
      if (!is_unconditional_branch_delay_slot)
      {
        Log_WarningPrintf("Conditional branch delay slot at %08X, skipping block", cbi.pc);
        return false;
      }
      if (!IsDirectBranchInstruction(cbi.instruction))
      {
        Log_WarningPrintf("Indirect branch in delay slot at %08X, skipping block", cbi.pc);
        return false;
      }

      // change the pc for the second branch's delay slot, it comes from the first branch
      const CodeBlockInstruction& prev_cbi = block->instructions.back();
      pc = GetBranchInstructionTarget(prev_cbi.instruction, prev_cbi.pc);
      Log_DevPrintf("Double branch at %08X, using delay slot from %08X -> %08X", cbi.pc, prev_cbi.pc, pc);
    }

    // instruction is decoded now
    block->instructions.push_back(cbi);

    // if we're in a branch delay slot, the block is now done
    // except if this is a branch in a branch delay slot, then we grab the one after that, and so on...
    if (is_branch_delay_slot && !cbi.is_branch_instruction)
      break;

    // if this is a branch, we grab the next instruction (delay slot), and then exit
    is_branch_delay_slot = cbi.is_branch_instruction;
    is_unconditional_branch_delay_slot = cbi.is_unconditional_branch_instruction;

    // same for load delay
    is_load_delay_slot = cbi.has_load_delay;

    // is this a non-branchy exit? (e.g. syscall)
    if (IsExitBlockInstruction(cbi.instruction))
      break;
  }

  if (!block->instructions.empty())
  {
    block->instructions.back().is_last_instruction = true;

#ifdef _DEBUG
    SmallString disasm;
    Log_DebugPrintf("Block at 0x%08X", block->GetPC());
    for (const CodeBlockInstruction& cbi : block->instructions)
    {
      CPU::DisassembleInstruction(&disasm, cbi.pc, cbi.instruction.bits);
      Log_DebugPrintf("[%s %s 0x%08X] %08X %s", cbi.is_branch_delay_slot ? "BD" : "  ",
                      cbi.is_load_delay_slot ? "LD" : "  ", cbi.pc, cbi.instruction.bits, disasm.GetCharArray());
    }
#endif
  }
  else
  {
    Log_WarningPrintf("Empty block compiled at 0x%08X", block->key.GetPC());
    return false;
  }

#ifdef WITH_RECOMPILER
  if (g_settings.IsUsingRecompiler())
  {
    // Ensure we're not going to run out of space while compiling this block.
    if (s_code_buffer.GetFreeCodeSpace() <
          (block->instructions.size() * Recompiler::MAX_NEAR_HOST_BYTES_PER_INSTRUCTION) ||
        s_code_buffer.GetFreeFarCodeSpace() <
          (block->instructions.size() * Recompiler::MAX_FAR_HOST_BYTES_PER_INSTRUCTION))
    {
      Log_WarningPrintf("Out of code space, flushing all blocks.");
      Flush();
    }

    Recompiler::CodeGenerator codegen(&s_code_buffer);
    if (!codegen.CompileBlock(block, &block->host_code, &block->host_code_size))
    {
      Log_ErrorPrintf("Failed to compile host code for block at 0x%08X", block->key.GetPC());
      return false;
    }
  }
#endif

  return true;
}

#ifdef WITH_RECOMPILER

void FastCompileBlockFunction()
{
  CodeBlock* block = LookupBlock(GetNextBlockKey());
  if (block)
    s_single_block_asm_dispatcher(block->host_code);
  else if (g_settings.gpu_pgxp_enable)
    InterpretUncachedBlock<PGXPMode::Memory>();
  else
    InterpretUncachedBlock<PGXPMode::Disabled>();
}

#endif

void InvalidateBlocksWithPageIndex(u32 page_index)
{
  DebugAssert(page_index < Bus::RAM_CODE_PAGE_COUNT);
  auto& blocks = m_ram_block_map[page_index];
  for (CodeBlock* block : blocks)
  {
    // Invalidate forces the block to be checked again.
    Log_DebugPrintf("Invalidating block at 0x%08X", block->GetPC());
    block->invalidated = true;
#ifdef WITH_RECOMPILER
    SetFastMap(block->GetPC(), FastCompileBlockFunction);
#endif
  }

  // Block will be re-added next execution.
  blocks.clear();
  Bus::ClearRAMCodePage(page_index);
}

void RemoveReferencesToBlock(CodeBlock* block)
{
  BlockMap::iterator iter = s_blocks.find(block->key.GetPC());
  Assert(iter != s_blocks.end() && iter->second == block);

#ifdef WITH_RECOMPILER
  SetFastMap(block->GetPC(), FastCompileBlockFunction);
#endif

  // if it's been invalidated it won't be in the page map
  if (!block->invalidated)
    RemoveBlockFromPageMap(block);

  UnlinkBlock(block);
#ifdef WITH_RECOMPILER
  if (!block->invalidated)
    RemoveBlockFromHostCodeMap(block);
#endif

  s_blocks.erase(iter);
}

void AddBlockToPageMap(CodeBlock* block)
{
  if (!block->IsInRAM())
    return;

  const u32 start_page = block->GetStartPageIndex();
  const u32 end_page = block->GetEndPageIndex();
  for (u32 page = start_page; page <= end_page; page++)
  {
    m_ram_block_map[page].push_back(block);
    Bus::SetRAMCodePage(page);
  }
}

void RemoveBlockFromPageMap(CodeBlock* block)
{
  if (!block->IsInRAM())
    return;

  const u32 start_page = block->GetStartPageIndex();
  const u32 end_page = block->GetEndPageIndex();
  for (u32 page = start_page; page <= end_page; page++)
  {
    auto& page_blocks = m_ram_block_map[page];
    auto page_block_iter = std::find(page_blocks.begin(), page_blocks.end(), block);
    Assert(page_block_iter != page_blocks.end());
    page_blocks.erase(page_block_iter);
  }
}

void LinkBlock(CodeBlock* from, CodeBlock* to)
{
  Log_DebugPrintf("Linking block %p(%08x) to %p(%08x)", from, from->GetPC(), to, to->GetPC());
  from->link_successors.push_back(to);
  to->link_predecessors.push_back(from);
}

void UnlinkBlock(CodeBlock* block)
{
  for (CodeBlock* predecessor : block->link_predecessors)
  {
    auto iter = std::find(predecessor->link_successors.begin(), predecessor->link_successors.end(), block);
    Assert(iter != predecessor->link_successors.end());
    predecessor->link_successors.erase(iter);
  }
  block->link_predecessors.clear();

  for (CodeBlock* successor : block->link_successors)
  {
    auto iter = std::find(successor->link_predecessors.begin(), successor->link_predecessors.end(), block);
    Assert(iter != successor->link_predecessors.end());
    successor->link_predecessors.erase(iter);
  }
  block->link_successors.clear();
}

#ifdef WITH_RECOMPILER

void AddBlockToHostCodeMap(CodeBlock* block)
{
  if (!g_settings.IsUsingRecompiler())
    return;

  auto ir = s_host_code_map.emplace(block->host_code, block);
  Assert(ir.second);
}

void RemoveBlockFromHostCodeMap(CodeBlock* block)
{
  if (!g_settings.IsUsingRecompiler())
    return;

  HostCodeMap::iterator hc_iter = s_host_code_map.find(block->host_code);
  Assert(hc_iter != s_host_code_map.end());
  s_host_code_map.erase(hc_iter);
}

bool InitializeFastmem()
{
  const CPUFastmemMode mode = g_settings.cpu_fastmem_mode;
  Assert(mode != CPUFastmemMode::Disabled);

#ifdef WITH_MMAP_FASTMEM
  const auto handler = (mode == CPUFastmemMode::MMap) ? MMapPageFaultHandler : LUTPageFaultHandler;
#else
  const auto handler = LUTPageFaultHandler;
  Assert(mode != CPUFastmemMode::MMap);
#endif

  if (!Common::PageFaultHandler::InstallHandler(&s_host_code_map, handler))
  {
    Log_ErrorPrintf("Failed to install page fault handler");
    return false;
  }

  Bus::UpdateFastmemViews(mode, g_state.cop0_regs.sr.Isc);
  return true;
}

void ShutdownFastmem()
{
  Common::PageFaultHandler::RemoveHandler(&s_host_code_map);
  Bus::UpdateFastmemViews(CPUFastmemMode::Disabled, false);
}

#ifdef WITH_MMAP_FASTMEM

Common::PageFaultHandler::HandlerResult MMapPageFaultHandler(void* exception_pc, void* fault_address, bool is_write)
{
  if (static_cast<u8*>(fault_address) < g_state.fastmem_base ||
      (static_cast<u8*>(fault_address) - g_state.fastmem_base) >= static_cast<ptrdiff_t>(Bus::FASTMEM_REGION_SIZE))
  {
    return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
  }

  const PhysicalMemoryAddress fastmem_address =
    static_cast<PhysicalMemoryAddress>(static_cast<ptrdiff_t>(static_cast<u8*>(fault_address) - g_state.fastmem_base));

  Log_DevPrintf("Page fault handler invoked at PC=%p Address=%p %s, fastmem offset 0x%08X", exception_pc, fault_address,
                is_write ? "(write)" : "(read)", fastmem_address);

  // use upper_bound to find the next block after the pc
  HostCodeMap::iterator upper_iter =
    s_host_code_map.upper_bound(reinterpret_cast<CodeBlock::HostCodePointer>(exception_pc));
  if (upper_iter == s_host_code_map.begin())
    return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;

  // then decrement it by one to (hopefully) get the block we want
  upper_iter--;

  // find the loadstore info in the code block
  CodeBlock* block = upper_iter->second;
  for (auto bpi_iter = block->loadstore_backpatch_info.begin(); bpi_iter != block->loadstore_backpatch_info.end();
       ++bpi_iter)
  {
    Recompiler::LoadStoreBackpatchInfo& lbi = *bpi_iter;
    if (lbi.host_pc == exception_pc)
    {
      if (is_write && !g_state.cop0_regs.sr.Isc && Bus::IsRAMAddress(fastmem_address))
      {
        // this is probably a code page, since we aren't going to fault due to requiring fastmem on RAM.
        const u32 code_page_index = Bus::GetRAMCodePageIndex(fastmem_address);
        if (Bus::IsRAMCodePage(code_page_index))
        {
          if (++lbi.fault_count < CODE_WRITE_FAULT_THRESHOLD_FOR_SLOWMEM)
          {
            InvalidateBlocksWithPageIndex(code_page_index);
            return Common::PageFaultHandler::HandlerResult::ContinueExecution;
          }
          else
          {
            Log_DevPrintf("Backpatching code write at %p (%08X) address %p (%08X) to slowmem after threshold",
                          exception_pc, lbi.guest_pc, fault_address, fastmem_address);
          }
        }
      }

      // found it, do fixup
      if (Recompiler::CodeGenerator::BackpatchLoadStore(lbi))
      {
        // remove the backpatch entry since we won't be coming back to this one
        block->loadstore_backpatch_info.erase(bpi_iter);
        return Common::PageFaultHandler::HandlerResult::ContinueExecution;
      }
      else
      {
        Log_ErrorPrintf("Failed to backpatch %p in block 0x%08X", exception_pc, block->GetPC());
        return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
      }
    }
  }

  // we didn't find the pc in our list..
  Log_ErrorPrintf("Loadstore PC not found for %p in block 0x%08X", exception_pc, block->GetPC());
  return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
}

#endif

Common::PageFaultHandler::HandlerResult LUTPageFaultHandler(void* exception_pc, void* fault_address, bool is_write)
{
  // use upper_bound to find the next block after the pc
  HostCodeMap::iterator upper_iter =
    s_host_code_map.upper_bound(reinterpret_cast<CodeBlock::HostCodePointer>(exception_pc));
  if (upper_iter == s_host_code_map.begin())
    return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;

  // then decrement it by one to (hopefully) get the block we want
  upper_iter--;

  // find the loadstore info in the code block
  CodeBlock* block = upper_iter->second;
  for (auto bpi_iter = block->loadstore_backpatch_info.begin(); bpi_iter != block->loadstore_backpatch_info.end();
       ++bpi_iter)
  {
    Recompiler::LoadStoreBackpatchInfo& lbi = *bpi_iter;
    if (lbi.host_pc == exception_pc)
    {
      // found it, do fixup
      if (Recompiler::CodeGenerator::BackpatchLoadStore(lbi))
      {
        // remove the backpatch entry since we won't be coming back to this one
        block->loadstore_backpatch_info.erase(bpi_iter);
        return Common::PageFaultHandler::HandlerResult::ContinueExecution;
      }
      else
      {
        Log_ErrorPrintf("Failed to backpatch %p in block 0x%08X", exception_pc, block->GetPC());
        return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
      }
    }
  }

  // we didn't find the pc in our list..
  Log_ErrorPrintf("Loadstore PC not found for %p in block 0x%08X", exception_pc, block->GetPC());
  return Common::PageFaultHandler::HandlerResult::ExecuteNextHandler;
}

#endif // WITH_RECOMPILER

} // namespace CPU::CodeCache
