#include "cpu_code_cache.h"
#include "common/log.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "system.h"
Log_SetChannel(CPU::CodeCache);

#ifdef WITH_RECOMPILER
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#endif

namespace CPU {

constexpr bool USE_BLOCK_LINKING = true;

static constexpr u32 RECOMPILER_CODE_CACHE_SIZE = 32 * 1024 * 1024;
static constexpr u32 RECOMPILER_FAR_CODE_CACHE_SIZE = 32 * 1024 * 1024;
#ifdef WITH_RECOMPILER
static u8 s_code_buffer[RECOMPILER_CODE_CACHE_SIZE + RECOMPILER_FAR_CODE_CACHE_SIZE];
#endif

CodeCache::CodeCache() = default;

CodeCache::~CodeCache()
{
  Flush();
}

void CodeCache::Initialize(Core* core, Bus* bus, bool use_recompiler)
{
  m_core = core;
  m_bus = bus;

#ifdef WITH_RECOMPILER
  m_use_recompiler = use_recompiler;
  // m_code_buffer = std::make_unique<JitCodeBuffer>(RECOMPILER_CODE_CACHE_SIZE, RECOMPILER_FAR_CODE_CACHE_SIZE);
  m_code_buffer = std::make_unique<JitCodeBuffer>(s_code_buffer, sizeof(s_code_buffer), RECOMPILER_FAR_CODE_CACHE_SIZE);
  m_asm_functions = std::make_unique<Recompiler::ASMFunctions>();
  m_asm_functions->Generate(m_code_buffer.get());
#else
  m_use_recompiler = false;
#endif
}

void CodeCache::Execute()
{
  CodeBlockKey next_block_key = GetNextBlockKey();

  while (m_core->m_pending_ticks < m_core->m_downcount)
  {
    if (m_core->HasPendingInterrupt())
    {
      // TODO: Fill in m_next_instruction...
      m_core->SafeReadMemoryWord(m_core->m_regs.pc, &m_core->m_next_instruction.bits);
      m_core->DispatchInterrupt();
      next_block_key = GetNextBlockKey();
    }

    CodeBlock* block = LookupBlock(next_block_key);
    if (!block)
    {
      Log_WarningPrintf("Falling back to uncached interpreter at 0x%08X", m_core->GetRegs().pc);
      InterpretUncachedBlock();
      continue;
    }

  reexecute_block:

#if 0
    const u32 tick = g_system->GetGlobalTickCounter() + m_core->GetPendingTicks();
    if (tick == 61033207)
      __debugbreak();
#endif

#if 0
    LogCurrentState();
#endif

    if (m_use_recompiler)
      block->host_code(m_core);
    else
      InterpretCachedBlock(*block);

    if (m_core->m_pending_ticks >= m_core->m_downcount)
      break;
    else if (m_core->HasPendingInterrupt() || !USE_BLOCK_LINKING)
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

  // in case we switch to interpreter...
  m_core->m_regs.npc = m_core->m_regs.pc;
}

void CodeCache::SetUseRecompiler(bool enable)
{
#ifdef WITH_RECOMPILER
  if (m_use_recompiler == enable)
    return;

  m_use_recompiler = enable;
  Flush();
#endif
}

void CodeCache::Flush()
{
  if (m_bus)
    m_bus->ClearRAMCodePageFlags();
  for (auto& it : m_ram_block_map)
    it.clear();

  for (const auto& it : m_blocks)
    delete it.second;
  m_blocks.clear();
#ifdef WITH_RECOMPILER
  m_code_buffer->Reset();
#endif
}

void CodeCache::LogCurrentState()
{
  const auto& regs = m_core->m_regs;
  WriteToExecutionLog("tick=%u pc=%08X zero=%08X at=%08X v0=%08X v1=%08X a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X "
                      "t1=%08X t2=%08X t3=%08X t4=%08X t5=%08X t6=%08X t7=%08X s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X "
                      "s5=%08X s6=%08X s7=%08X t8=%08X t9=%08X k0=%08X k1=%08X gp=%08X sp=%08X fp=%08X ra=%08X ldr=%s "
                      "ldv=%08X\n",
                      g_system->GetGlobalTickCounter() + m_core->GetPendingTicks(), regs.pc, regs.zero, regs.at,
                      regs.v0, regs.v1, regs.a0, regs.a1, regs.a2, regs.a3, regs.t0, regs.t1, regs.t2, regs.t3, regs.t4,
                      regs.t5, regs.t6, regs.t7, regs.s0, regs.s1, regs.s2, regs.s3, regs.s4, regs.s5, regs.s6, regs.s7,
                      regs.t8, regs.t9, regs.k0, regs.k1, regs.gp, regs.sp, regs.fp, regs.ra,
                      (m_core->m_next_load_delay_reg == Reg::count) ? "NONE" :
                                                                      GetRegName(m_core->m_next_load_delay_reg),
                      (m_core->m_next_load_delay_reg == Reg::count) ? 0 : m_core->m_next_load_delay_value);
}

CodeBlockKey CodeCache::GetNextBlockKey() const
{
  CodeBlockKey key = {};
  key.SetPC(m_core->GetRegs().pc);
  key.user_mode = m_core->InUserMode();
  return key;
}

CodeBlock* CodeCache::LookupBlock(CodeBlockKey key)
{
  BlockMap::iterator iter = m_blocks.find(key.bits);
  if (iter != m_blocks.end())
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
  }
  else
  {
    Log_ErrorPrintf("Failed to compile block at PC=0x%08X", key.GetPC());
    delete block;
    block = nullptr;
  }

  iter = m_blocks.emplace(key.bits, block).first;
  return block;
}

bool CodeCache::RevalidateBlock(CodeBlock* block)
{
  for (const CodeBlockInstruction& cbi : block->instructions)
  {
    u32 new_code = 0;
    m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(cbi.pc & PHYSICAL_MEMORY_ADDRESS_MASK,
                                                                          new_code);
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
  return true;

recompile:
  block->instructions.clear();
  if (!CompileBlock(block))
  {
    Log_WarningPrintf("Failed to recompile block 0x%08X - flushing.", block->GetPC());
    FlushBlock(block);
    return false;
  }

  // re-add to page map again
  if (block->IsInRAM())
    AddBlockToPageMap(block);

  return true;
}

bool CodeCache::CompileBlock(CodeBlock* block)
{
  u32 pc = block->GetPC();
  bool is_branch_delay_slot = false;
  bool is_load_delay_slot = false;

#if 0
  if (pc == 0x0005aa90)
    __debugbreak();
#endif

  for (;;)
  {
    CodeBlockInstruction cbi = {};

    const PhysicalMemoryAddress phys_addr = pc & PHYSICAL_MEMORY_ADDRESS_MASK;
    if (!m_bus->IsCacheableAddress(phys_addr) ||
        m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(phys_addr, cbi.instruction.bits) < 0 ||
        !IsInvalidInstruction(cbi.instruction))
    {
      break;
    }

    cbi.pc = pc;
    cbi.is_branch_delay_slot = is_branch_delay_slot;
    cbi.is_load_delay_slot = is_load_delay_slot;
    cbi.is_branch_instruction = IsBranchInstruction(cbi.instruction);
    cbi.is_load_instruction = IsMemoryLoadInstruction(cbi.instruction);
    cbi.is_store_instruction = IsMemoryStoreInstruction(cbi.instruction);
    cbi.has_load_delay = InstructionHasLoadDelay(cbi.instruction);
    cbi.can_trap = CanInstructionTrap(cbi.instruction, m_core->InUserMode());

    // instruction is decoded now
    block->instructions.push_back(cbi);
    pc += sizeof(cbi.instruction.bits);

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
      CPU::DisassembleInstruction(&disasm, cbi.pc, cbi.instruction.bits, nullptr);
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
  if (m_use_recompiler)
  {
    // Ensure we're not going to run out of space while compiling this block.
    if (m_code_buffer->GetFreeCodeSpace() <
          (block->instructions.size() * Recompiler::MAX_NEAR_HOST_BYTES_PER_INSTRUCTION) ||
        m_code_buffer->GetFreeFarCodeSpace() <
          (block->instructions.size() * Recompiler::MAX_FAR_HOST_BYTES_PER_INSTRUCTION))
    {
      Log_WarningPrintf("Out of code space, flushing all blocks.");
      Flush();
    }

    Recompiler::CodeGenerator codegen(m_core, m_code_buffer.get(), *m_asm_functions.get());
    if (!codegen.CompileBlock(block, &block->host_code, &block->host_code_size))
    {
      Log_ErrorPrintf("Failed to compile host code for block at 0x%08X", block->key.GetPC());
      return false;
    }
  }
#endif

  return true;
}

void CodeCache::InvalidateBlocksWithPageIndex(u32 page_index)
{
  DebugAssert(page_index < CPU_CODE_CACHE_PAGE_COUNT);
  auto& blocks = m_ram_block_map[page_index];
  for (CodeBlock* block : blocks)
  {
    // Invalidate forces the block to be checked again.
    Log_DebugPrintf("Invalidating block at 0x%08X", block->GetPC());
    block->invalidated = true;
  }

  // Block will be re-added next execution.
  blocks.clear();
  m_bus->ClearRAMCodePage(page_index);
}

void CodeCache::FlushBlock(CodeBlock* block)
{
  BlockMap::iterator iter = m_blocks.find(block->key.GetPC());
  Assert(iter != m_blocks.end() && iter->second == block);
  Log_DevPrintf("Flushing block at address 0x%08X", block->GetPC());

  // if it's been invalidated it won't be in the page map
  if (block->invalidated)
    RemoveBlockFromPageMap(block);

  m_blocks.erase(iter);
  delete block;
}

void CodeCache::AddBlockToPageMap(CodeBlock* block)
{
  if (!block->IsInRAM())
    return;

  const u32 start_page = block->GetStartPageIndex();
  const u32 end_page = block->GetEndPageIndex();
  for (u32 page = start_page; page <= end_page; page++)
  {
    m_ram_block_map[page].push_back(block);
    m_bus->SetRAMCodePage(page);
  }
}

void CodeCache::RemoveBlockFromPageMap(CodeBlock* block)
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

void CodeCache::LinkBlock(CodeBlock* from, CodeBlock* to)
{
  Log_DebugPrintf("Linking block %p(%08x) to %p(%08x)", from, from->GetPC(), to, to->GetPC());
  from->link_successors.push_back(to);
  to->link_predecessors.push_back(from);
}

void CodeCache::UnlinkBlock(CodeBlock* block)
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

void CodeCache::InterpretCachedBlock(const CodeBlock& block)
{
  // set up the state so we've already fetched the instruction
  DebugAssert(m_core->m_regs.pc == block.GetPC());

  m_core->m_regs.npc = block.GetPC() + 4;

  for (const CodeBlockInstruction& cbi : block.instructions)
  {
    m_core->m_pending_ticks++;

    // now executing the instruction we previously fetched
    m_core->m_current_instruction.bits = cbi.instruction.bits;
    m_core->m_current_instruction_pc = cbi.pc;
    m_core->m_current_instruction_in_branch_delay_slot = cbi.is_branch_delay_slot;
    m_core->m_current_instruction_was_branch_taken = m_core->m_branch_was_taken;
    m_core->m_branch_was_taken = false;
    m_core->m_exception_raised = false;

    // update pc
    m_core->m_regs.pc = m_core->m_regs.npc;
    m_core->m_regs.npc += 4;

    // execute the instruction we previously fetched
    m_core->ExecuteInstruction();

    // next load delay
    m_core->UpdateLoadDelay();

    if (m_core->m_exception_raised)
      break;
  }

  // cleanup so the interpreter can kick in if needed
  m_core->m_next_instruction_is_branch_delay_slot = false;
}

void CodeCache::InterpretUncachedBlock()
{
  Panic("Fixme with regards to re-fetching PC");

  // At this point, pc contains the last address executed (in the previous block). The instruction has not been fetched
  // yet. pc shouldn't be updated until the fetch occurs, that way the exception occurs in the delay slot.
  bool in_branch_delay_slot = false;
  for (;;)
  {
    m_core->m_pending_ticks++;

    // now executing the instruction we previously fetched
    m_core->m_current_instruction.bits = m_core->m_next_instruction.bits;
    m_core->m_current_instruction_pc = m_core->m_regs.pc;
    m_core->m_current_instruction_in_branch_delay_slot = m_core->m_next_instruction_is_branch_delay_slot;
    m_core->m_current_instruction_was_branch_taken = m_core->m_branch_was_taken;
    m_core->m_next_instruction_is_branch_delay_slot = false;
    m_core->m_branch_was_taken = false;
    m_core->m_exception_raised = false;

    // Fetch the next instruction, except if we're in a branch delay slot. The "fetch" is done in the next block.
    if (!m_core->FetchInstruction())
      break;

    // execute the instruction we previously fetched
    m_core->ExecuteInstruction();

    // next load delay
    m_core->UpdateLoadDelay();

    const bool branch = IsBranchInstruction(m_core->m_current_instruction);
    if (m_core->m_exception_raised || (!branch && in_branch_delay_slot) ||
        IsExitBlockInstruction(m_core->m_current_instruction))
    {
      break;
    }

    in_branch_delay_slot = branch;
  }
}

} // namespace CPU
