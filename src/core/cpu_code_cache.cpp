#include "cpu_code_cache.h"
#include "YBaseLib/Log.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "cpu_recompiler_code_generator.h"
#include "cpu_recompiler_thunks.h"
#include "system.h"
#include <thread>
#include <chrono>
Log_SetChannel(CPU::CodeCache);

namespace CPU {

bool USE_CODE_CACHE = false;
bool USE_RECOMPILER = false;

CodeCache::CodeCache() = default;

CodeCache::~CodeCache() = default;

void CodeCache::Initialize(System* system, Core* core, Bus* bus)
{
  m_system = system;
  m_core = core;
  m_bus = bus;

  m_code_buffer = std::make_unique<JitCodeBuffer>();
  m_asm_functions = std::make_unique<Recompiler::ASMFunctions>();
  m_asm_functions->Generate(m_code_buffer.get());
}

void CodeCache::Execute()
{
  while (m_core->m_downcount >= 0)
  {
    if (m_core->HasPendingInterrupt())
    {
      // TODO: Fill in m_next_instruction...
      m_core->DispatchInterrupt();
    }

    m_current_block = GetNextBlock();
    if (!m_current_block)
    {
      Log_WarningPrintf("Falling back to uncached interpreter at 0x%08X", m_core->GetRegs().pc);
      InterpretUncachedBlock();
      continue;
    }

    if (USE_RECOMPILER)
      m_current_block->host_code(m_core);
    else
      InterpretCachedBlock(*m_current_block);

    if (m_current_block_flushed)
    {
      m_current_block_flushed = false;
      delete m_current_block;
    }

    m_current_block = nullptr;
  }
}

void CodeCache::Reset()
{
  m_bus->ClearRAMCodePageFlags();
  for (auto& it : m_ram_block_map)
    it.clear();

  m_blocks.clear();
  m_code_buffer->Reset();
}

const CPU::CodeBlock* CodeCache::GetNextBlock()
{
  const u32 address = m_bus->UnmirrorAddress(m_core->m_regs.pc & UINT32_C(0x1FFFFFFF));

  CodeBlockKey key = {};
  key.SetPC(address);
  key.user_mode = m_core->InUserMode();

  BlockMap::iterator iter = m_blocks.find(key.bits);
  if (iter != m_blocks.end())
    return iter->second;

  CodeBlock* block = new CodeBlock();
  block->key = key;
  if (CompileBlock(block))
  {
    // insert into the page map
    if (m_bus->IsRAMAddress(address))
    {
      const u32 start_page = block->GetStartPageIndex();
      const u32 end_page = block->GetEndPageIndex();
      for (u32 page = start_page; page < end_page; page++)
      {
        m_ram_block_map[page].push_back(block);
        m_bus->SetRAMCodePage(page);
      }
    }
  }
  else
  {
    Log_ErrorPrintf("Failed to compile block at PC=0x%08X", address);
  }

  iter = m_blocks.emplace(key.bits, block).first;
  return block;
}

bool CodeCache::CompileBlock(CodeBlock* block)
{
  u32 pc = block->GetPC();
  bool is_branch_delay_slot = false;
  bool is_load_delay_slot = false;

  for (;;)
  {
    CodeBlockInstruction cbi = {};
    if (!m_bus->IsCacheableAddress(pc) ||
        m_bus->DispatchAccess<MemoryAccessType::Read, MemoryAccessSize::Word>(pc, cbi.instruction.bits) < 0 ||
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

  if (USE_RECOMPILER)
  {
    // Ensure we're not going to run out of space while compiling this block.
    if (m_code_buffer->GetFreeCodeSpace() < (block->instructions.size() * Recompiler::MAX_HOST_BYTES_PER_INSTRUCTION))
    {
      Log_WarningPrintf("Out of code space, flushing all blocks.");
      Reset();
    }

    Recompiler::CodeGenerator codegen(m_core, m_code_buffer.get(), *m_asm_functions.get());
    if (!codegen.CompileBlock(block, &block->host_code, &block->host_code_size))
    {
      Log_ErrorPrintf("Failed to compile host code for block at 0x%08X", block->key.GetPC());
      return false;
    }
  }

  return true;
}

void CodeCache::FlushBlocksWithPageIndex(u32 page_index)
{
  DebugAssert(page_index < CPU_CODE_CACHE_PAGE_COUNT);
  auto& blocks = m_ram_block_map[page_index];
  while (!blocks.empty())
    FlushBlock(blocks.back());

  m_bus->ClearRAMCodePage(page_index);
}

void CodeCache::FlushBlock(CodeBlock* block)
{
  BlockMap::iterator iter = m_blocks.find(block->key.GetPC());
  Assert(iter != m_blocks.end() && iter->second == block);
  Log_DevPrintf("Flushing block at address 0x%08X", block->GetPC());

  // remove from the page map
  const u32 start_page = block->GetStartPageIndex();
  const u32 end_page = block->GetEndPageIndex();
  for (u32 page = start_page; page < end_page; page++)
  {
    auto& page_blocks = m_ram_block_map[page];
    auto page_block_iter = std::find(page_blocks.begin(), page_blocks.end(), block);
    Assert(page_block_iter != page_blocks.end());
    page_blocks.erase(page_block_iter);
  }

  // remove from block map
  m_blocks.erase(iter);

  // flushing block currently executing?
  if (m_current_block == block)
  {
    Log_WarningPrintf("Flushing currently-executing block 0x%08X", block->GetPC());
    m_current_block_flushed = true;
  }
  else
  {
    delete block;
  }
}

void CodeCache::InterpretCachedBlock(const CodeBlock& block)
{
  // set up the state so we've already fetched the instruction
  DebugAssert((m_core->m_regs.pc & PHYSICAL_MEMORY_ADDRESS_MASK) == block.GetPC());

  for (const CodeBlockInstruction& cbi : block.instructions)
  {
    m_core->m_pending_ticks += 1;
    m_core->m_downcount -= 1;

    // now executing the instruction we previously fetched
    m_core->m_current_instruction.bits = cbi.instruction.bits;
    m_core->m_current_instruction_pc = m_core->m_regs.pc;
    m_core->m_current_instruction_in_branch_delay_slot = cbi.is_branch_delay_slot;
    m_core->m_current_instruction_was_branch_taken = m_core->m_branch_was_taken;
    m_core->m_branch_was_taken = false;
    m_core->m_exception_raised = false;

    // update pc
    DebugAssert((m_core->m_regs.pc & PHYSICAL_MEMORY_ADDRESS_MASK) == cbi.pc);
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
  // At this point, pc contains the last address executed (in the previous block). The instruction has not been fetched
  // yet. pc shouldn't be updated until the fetch occurs, that way the exception occurs in the delay slot.
  bool in_branch_delay_slot = false;
  for (;;)
  {
    m_core->m_pending_ticks += 1;
    m_core->m_downcount -= 1;

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