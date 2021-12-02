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

static constexpr bool USE_BLOCK_LINKING = true;

// Fall blocks back to interpreter if we recompile more than 20 times within 100 frames.
static constexpr u32 RECOMPILE_FRAMES_TO_FALL_BACK_TO_INTERPRETER = 100;
static constexpr u32 RECOMPILE_COUNT_TO_FALL_BACK_TO_INTERPRETER = 20;
static constexpr u32 INVALIDATE_THRESHOLD_TO_DISABLE_LINKING = 10;

#ifdef WITH_RECOMPILER

// Currently remapping the code buffer doesn't work in macOS or Haiku.
#if !defined(__HAIKU__) && !defined(__APPLE__) && !defined(_UWP)
#define USE_STATIC_CODE_BUFFER 1
#endif

#if defined(CPU_AARCH32)
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
static FastMapTable s_fast_map[FAST_MAP_TABLE_COUNT];
static std::unique_ptr<CodeBlock::HostCodePointer[]> s_fast_map_pointers;

DispatcherFunction s_asm_dispatcher;
SingleBlockDispatcherFunction s_single_block_asm_dispatcher;

static FastMapTable DecodeFastMapPointer(u32 slot, FastMapTable ptr)
{
  if constexpr (sizeof(void*) == 8)
    return reinterpret_cast<FastMapTable>(reinterpret_cast<u8*>(ptr) + (static_cast<u64>(slot) << 17));
  else
    return reinterpret_cast<FastMapTable>(reinterpret_cast<u8*>(ptr) + (slot << 16));
}

static FastMapTable EncodeFastMapPointer(u32 slot, FastMapTable ptr)
{
  if constexpr (sizeof(void*) == 8)
    return reinterpret_cast<FastMapTable>(reinterpret_cast<u8*>(ptr) - (static_cast<u64>(slot) << 17));
  else
    return reinterpret_cast<FastMapTable>(reinterpret_cast<u8*>(ptr) - (slot << 16));
}

static CodeBlock::HostCodePointer* OffsetFastMapPointer(FastMapTable fake_ptr, u32 pc)
{
  u8* fake_byte_ptr = reinterpret_cast<u8*>(fake_ptr);
  if constexpr (sizeof(void*) == 8)
    return reinterpret_cast<CodeBlock::HostCodePointer*>(fake_byte_ptr + (static_cast<u64>(pc) << 1));
  else
    return reinterpret_cast<CodeBlock::HostCodePointer*>(fake_byte_ptr + pc);
}

static void CompileDispatcher();
static void FastCompileBlockFunction();
static void InvalidCodeFunction();

static constexpr u32 GetTableCount(u32 start, u32 end)
{
  return ((end >> FAST_MAP_TABLE_SHIFT) - (start >> FAST_MAP_TABLE_SHIFT)) + 1;
}

static void AllocateFastMapTables(u32 start, u32 end, FastMapTable& table_ptr)
{
  const u32 start_slot = start >> FAST_MAP_TABLE_SHIFT;
  const u32 count = GetTableCount(start, end);
  for (u32 i = 0; i < count; i++)
  {
    const u32 slot = start_slot + i;

    s_fast_map[slot] = EncodeFastMapPointer(slot, table_ptr);
    table_ptr += FAST_MAP_TABLE_SIZE;
  }
}

static void AllocateFastMap()
{
  static constexpr VirtualMemoryAddress ranges[][2] = {
    {0x00000000, 0x00800000}, // RAM
    {0x1F000000, 0x1F800000}, // EXP1
    {0x1FC00000, 0x1FC80000}, // BIOS

    {0x80000000, 0x80800000}, // RAM
    {0x9F000000, 0x9F800000}, // EXP1
    {0x9FC00000, 0x9FC80000}, // BIOS

    {0xA0000000, 0xA0800000}, // RAM
    {0xBF000000, 0xBF800000}, // EXP1
    {0xBFC00000, 0xBFC80000}  // BIOS
  };

  u32 num_tables = 1; // unreachable table
  for (u32 i = 0; i < countof(ranges); i++)
    num_tables += GetTableCount(ranges[i][0], ranges[i][1]);

  const u32 num_slots = FAST_MAP_TABLE_SIZE * num_tables;
  if (!s_fast_map_pointers)
    s_fast_map_pointers = std::make_unique<CodeBlock::HostCodePointer[]>(num_slots);

  FastMapTable table_ptr = s_fast_map_pointers.get();
  FastMapTable table_ptr_end = table_ptr + num_slots;

  // Fill the first table with invalid/unreachable.
  for (u32 i = 0; i < FAST_MAP_TABLE_SIZE; i++)
    table_ptr[i] = InvalidCodeFunction;

  // And the remaining with block compile pointers.
  for (u32 i = FAST_MAP_TABLE_SIZE; i < num_slots; i++)
    table_ptr[i] = FastCompileBlockFunction;

  // Mark everything as unreachable to begin with.
  for (u32 i = 0; i < FAST_MAP_TABLE_COUNT; i++)
    s_fast_map[i] = EncodeFastMapPointer(i, table_ptr);
  table_ptr += FAST_MAP_TABLE_SIZE;

  // Allocate ranges.
  for (u32 i = 0; i < countof(ranges); i++)
    AllocateFastMapTables(ranges[i][0], ranges[i][1], table_ptr);

  Assert(table_ptr == table_ptr_end);
}

static void ResetFastMap()
{
  if (!s_fast_map_pointers)
    return;

  for (u32 i = 0; i < FAST_MAP_TABLE_COUNT; i++)
  {
    FastMapTable ptr = DecodeFastMapPointer(i, s_fast_map[i]);
    if (ptr == s_fast_map_pointers.get())
      continue;

    for (u32 j = 0; j < FAST_MAP_TABLE_SIZE; j++)
      ptr[j] = FastCompileBlockFunction;
  }
}

static void FreeFastMap()
{
  std::memset(s_fast_map, 0, sizeof(s_fast_map));
  s_fast_map_pointers.reset();
}

static void SetFastMap(u32 pc, CodeBlock::HostCodePointer function)
{
  if (!s_fast_map_pointers)
    return;

  const u32 slot = pc >> FAST_MAP_TABLE_SHIFT;
  FastMapTable encoded_ptr = s_fast_map[slot];

  const FastMapTable table_ptr = DecodeFastMapPointer(slot, encoded_ptr);
  Assert(table_ptr != nullptr && table_ptr != s_fast_map_pointers.get());

  CodeBlock::HostCodePointer* ptr = OffsetFastMapPointer(encoded_ptr, pc);
  *ptr = function;
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

/// Link block from to to. Returns the successor index.
static void LinkBlock(CodeBlock* from, CodeBlock* to, void* host_pc, void* host_resolve_pc, u32 host_pc_size);

/// Unlink all blocks which point to this block, and any that this block links to.
static void UnlinkBlock(CodeBlock* block);

static void ClearState();

static BlockMap s_blocks;
static std::array<std::vector<CodeBlock*>, Bus::RAM_8MB_CODE_PAGE_COUNT> m_ram_block_map;

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

    AllocateFastMap();

    if (g_settings.IsUsingFastmem() && !InitializeFastmem())
      Panic("Failed to initialize fastmem");

    CompileDispatcher();
    ResetFastMap();
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
  FreeFastMap();
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
        next_block_key = GetNextBlockKey();
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
        for (const CodeBlock::LinkInfo& li : block->link_successors)
        {
          CodeBlock* linked_block = li.block;
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
          LinkBlock(block, next_block, nullptr, nullptr, 0);
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
  s_code_buffer.WriteProtect(false);

  {
    Recompiler::CodeGenerator cg(&s_code_buffer);
    s_asm_dispatcher = cg.CompileDispatcher();
  }
  {
    Recompiler::CodeGenerator cg(&s_code_buffer);
    s_single_block_asm_dispatcher = cg.CompileSingleBlockDispatcher();
  }

  s_code_buffer.WriteProtect(true);
}

FastMapTable* GetFastMapPointer()
{
  return s_fast_map;
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
#if 0
      LogCurrentState();
#endif

      const u32 pc = g_state.regs.pc;
      s_single_block_asm_dispatcher(s_fast_map[pc >> 16][pc >> 2]);
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

    AllocateFastMap();
    CompileDispatcher();
    ResetFastMap();
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

// assumes it has already been unlinked
static void FallbackExistingBlockToInterpreter(CodeBlock* block)
{
  // Replace with null so we don't try to compile it again.
  s_blocks.emplace(block->key.bits, nullptr);
  delete block;
}

CodeBlock* LookupBlock(CodeBlockKey key)
{
  BlockMap::iterator iter = s_blocks.find(key.bits);
  if (iter != s_blocks.end())
  {
    // ensure it hasn't been invalidated
    CodeBlock* existing_block = iter->second;
    if (!existing_block || !existing_block->invalidated)
      return existing_block;

    // if compilation fails or we're forced back to the interpreter, bail out
    if (RevalidateBlock(existing_block))
      return existing_block;
    else
      return nullptr;
  }

  CodeBlock* block = new CodeBlock(key);
  block->recompile_frame_number = System::GetFrameNumber();

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

  const u32 frame_number = System::GetFrameNumber();
  const u32 frame_diff = frame_number - block->recompile_frame_number;
  if (frame_diff <= RECOMPILE_FRAMES_TO_FALL_BACK_TO_INTERPRETER)
  {
    block->recompile_count++;

    if (block->recompile_count >= RECOMPILE_COUNT_TO_FALL_BACK_TO_INTERPRETER)
    {
      Log_PerfPrintf("Block 0x%08X has been recompiled %u times in %u frames, falling back to interpreter",
                     block->GetPC(), block->recompile_count, frame_diff);

      FallbackExistingBlockToInterpreter(block);
      return false;
    }
  }
  else
  {
    // It's been a while since this block was modified, so it's all good.
    block->recompile_frame_number = frame_number;
    block->recompile_count = 0;
  }

  block->instructions.clear();

  if (!CompileBlock(block))
  {
    Log_PerfPrintf("Failed to recompile block 0x%08X, falling back to interpreter.", block->GetPC());
    FallbackExistingBlockToInterpreter(block);
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
    cbi.is_direct_branch_instruction = IsDirectBranchInstruction(cbi.instruction);
    cbi.is_unconditional_branch_instruction = IsUnconditionalBranchInstruction(cbi.instruction);
    cbi.is_load_instruction = IsMemoryLoadInstruction(cbi.instruction);
    cbi.is_store_instruction = IsMemoryStoreInstruction(cbi.instruction);
    cbi.has_load_delay = InstructionHasLoadDelay(cbi.instruction);
    cbi.can_trap = CanInstructionTrap(cbi.instruction, InUserMode());
    cbi.is_direct_branch_instruction = IsDirectBranchInstruction(cbi.instruction);

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
      const CodeBlockInstruction& prev_cbi = block->instructions.back();
      if (!prev_cbi.is_unconditional_branch_instruction || !prev_cbi.is_direct_branch_instruction)
      {
        Log_WarningPrintf("Conditional or indirect branch delay slot at %08X, skipping block", cbi.pc);
        return false;
      }
      if (!IsDirectBranchInstruction(cbi.instruction))
      {
        Log_WarningPrintf("Indirect branch in delay slot at %08X, skipping block", cbi.pc);
        return false;
      }

      // change the pc for the second branch's delay slot, it comes from the first branch
      pc = GetDirectBranchTarget(prev_cbi.instruction, prev_cbi.pc);
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

    s_code_buffer.WriteProtect(false);
    Recompiler::CodeGenerator codegen(&s_code_buffer);
    const bool compile_result = codegen.CompileBlock(block, &block->host_code, &block->host_code_size);
    s_code_buffer.WriteProtect(true);

    if (!compile_result)
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
  {
    s_single_block_asm_dispatcher(block->host_code);
    return;
  }

  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_pgxp_cpu)
      InterpretUncachedBlock<PGXPMode::CPU>();
    else
      InterpretUncachedBlock<PGXPMode::Memory>();
  }
  else
  {
    InterpretUncachedBlock<PGXPMode::Disabled>();
  }
}

void InvalidCodeFunction()
{
  Log_ErrorPrintf("Trying to execute invalid code at 0x%08X", g_state.regs.pc);
  if (g_settings.gpu_pgxp_enable)
  {
    if (g_settings.gpu_pgxp_cpu)
      InterpretUncachedBlock<PGXPMode::CPU>();
    else
      InterpretUncachedBlock<PGXPMode::Memory>();
  }
  else
  {
    InterpretUncachedBlock<PGXPMode::Disabled>();
  }
}

#endif

static void InvalidateBlock(CodeBlock* block)
{
  // Invalidate forces the block to be checked again.
  Log_DebugPrintf("Invalidating block at 0x%08X", block->GetPC());
  block->invalidated = true;

  if (block->can_link)
  {
    const u32 frame_number = System::GetFrameNumber();
    const u32 frame_diff = frame_number - block->invalidate_frame_number;
    if (frame_diff <= INVALIDATE_THRESHOLD_TO_DISABLE_LINKING)
    {
      Log_DevPrintf("Block 0x%08X has been invalidated in %u frames, disabling linking", block->GetPC(), frame_diff);
      block->can_link = false;
    }
    else
    {
      // It's been a while since this block was modified, so it's all good.
      block->invalidate_frame_number = frame_number;
    }
  }

  UnlinkBlock(block);

#ifdef WITH_RECOMPILER
  SetFastMap(block->GetPC(), FastCompileBlockFunction);
#endif
}

void InvalidateBlocksWithPageIndex(u32 page_index)
{
  DebugAssert(page_index < Bus::RAM_8MB_CODE_PAGE_COUNT);
  auto& blocks = m_ram_block_map[page_index];
  for (CodeBlock* block : blocks)
    InvalidateBlock(block);

  // Block will be re-added next execution.
  blocks.clear();
  Bus::ClearRAMCodePage(page_index);
}

void InvalidateAll()
{
  for (auto& it : s_blocks)
  {
    CodeBlock* block = it.second;
    if (block && !block->invalidated)
      InvalidateBlock(block);
  }

  Bus::ClearRAMCodePageFlags();
  for (auto& it : m_ram_block_map)
    it.clear();
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

void LinkBlock(CodeBlock* from, CodeBlock* to, void* host_pc, void* host_resolve_pc, u32 host_pc_size)
{
  Log_DebugPrintf("Linking block %p(%08x) to %p(%08x)", from, from->GetPC(), to, to->GetPC());

  CodeBlock::LinkInfo li;
  li.block = to;
  li.host_pc = host_pc;
  li.host_resolve_pc = host_resolve_pc;
  li.host_pc_size = host_pc_size;
  from->link_successors.push_back(li);

  li.block = from;
  to->link_predecessors.push_back(li);

#ifdef WITH_RECOMPILER
  // apply in code
  if (host_pc)
  {
    Log_ProfilePrintf("Backpatching %p(%08x) to jump to block %p (%08x)", host_pc, from->GetPC(), to, to->GetPC());
    s_code_buffer.WriteProtect(false);
    Recompiler::CodeGenerator::BackpatchBranch(host_pc, host_pc_size, reinterpret_cast<void*>(to->host_code));
    s_code_buffer.WriteProtect(true);
  }
#endif
}

void UnlinkBlock(CodeBlock* block)
{
  if (block->link_predecessors.empty() && block->link_successors.empty())
    return;

#ifdef WITH_RECOMPILER
  if (g_settings.IsUsingRecompiler() && g_settings.cpu_recompiler_block_linking)
    s_code_buffer.WriteProtect(false);
#endif

  for (CodeBlock::LinkInfo& li : block->link_predecessors)
  {
    auto iter = std::find_if(li.block->link_successors.begin(), li.block->link_successors.end(),
                             [block](const CodeBlock::LinkInfo& li) { return li.block == block; });
    Assert(iter != li.block->link_successors.end());

#ifdef WITH_RECOMPILER
    // Restore blocks linked to this block back to the resolver
    if (li.host_pc)
    {
      Log_ProfilePrintf("Backpatching %p(%08x) [predecessor] to jump to resolver", li.host_pc, li.block->GetPC());
      Recompiler::CodeGenerator::BackpatchBranch(li.host_pc, li.host_pc_size, li.host_resolve_pc);
    }
#endif

    li.block->link_successors.erase(iter);
  }
  block->link_predecessors.clear();

  for (CodeBlock::LinkInfo& li : block->link_successors)
  {
    auto iter = std::find_if(li.block->link_predecessors.begin(), li.block->link_predecessors.end(),
                             [block](const CodeBlock::LinkInfo& li) { return li.block == block; });
    Assert(iter != li.block->link_predecessors.end());

#ifdef WITH_RECOMPILER
    // Restore blocks we're linking to back to the resolver, since the successor won't be linked to us to backpatch if
    // it changes.
    if (li.host_pc)
    {
      Log_ProfilePrintf("Backpatching %p(%08x) [successor] to jump to resolver", li.host_pc, li.block->GetPC());
      Recompiler::CodeGenerator::BackpatchBranch(li.host_pc, li.host_pc_size, li.host_resolve_pc);
    }
#endif

    // Don't have to do anything special for successors - just let the successor know it's no longer linked.
    li.block->link_predecessors.erase(iter);
  }
  block->link_successors.clear();

#ifdef WITH_RECOMPILER
  if (g_settings.IsUsingRecompiler() && g_settings.cpu_recompiler_block_linking)
    s_code_buffer.WriteProtect(true);
#endif
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

  s_code_buffer.ReserveCode(Common::PageFaultHandler::GetHandlerCodeSize());

  if (!Common::PageFaultHandler::InstallHandler(&s_host_code_map, s_code_buffer.GetCodePointer(),
                                                s_code_buffer.GetTotalSize(), handler))
  {
    Log_ErrorPrintf("Failed to install page fault handler");
    return false;
  }

  Bus::UpdateFastmemViews(mode);
  CPU::UpdateFastmemBase();
  return true;
}

void ShutdownFastmem()
{
  Common::PageFaultHandler::RemoveHandler(&s_host_code_map);
  Bus::UpdateFastmemViews(CPUFastmemMode::Disabled);
  CPU::UpdateFastmemBase();
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
      s_code_buffer.WriteProtect(false);
      const bool backpatch_result = Recompiler::CodeGenerator::BackpatchLoadStore(lbi);
      s_code_buffer.WriteProtect(true);
      if (backpatch_result)
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
      s_code_buffer.WriteProtect(false);
      const bool backpatch_result = Recompiler::CodeGenerator::BackpatchLoadStore(lbi);
      s_code_buffer.WriteProtect(true);
      if (backpatch_result)
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

#ifdef WITH_RECOMPILER

void CPU::Recompiler::Thunks::ResolveBranch(CodeBlock* block, void* host_pc, void* host_resolve_pc, u32 host_pc_size)
{
  using namespace CPU::CodeCache;

  CodeBlockKey key = GetNextBlockKey();
  CodeBlock* successor_block = LookupBlock(key);
  if (!successor_block || (successor_block->invalidated && !RevalidateBlock(successor_block)) || !block->can_link ||
      !successor_block->can_link)
  {
    // just turn it into a return to the dispatcher instead.
    s_code_buffer.WriteProtect(false);
    CodeGenerator::BackpatchReturn(host_pc, host_pc_size);
    s_code_buffer.WriteProtect(true);
  }
  else
  {
    // link blocks!
    LinkBlock(block, successor_block, host_pc, host_resolve_pc, host_pc_size);
  }
}

void CPU::Recompiler::Thunks::LogPC(u32 pc)
{
#if 0
  CPU::CodeCache::LogCurrentState();
#endif
#if 0
  if (TimingEvents::GetGlobalTickCounter() + GetPendingTicks() == 382856482)
    __debugbreak();
#endif
}

#endif // WITH_RECOMPILER
