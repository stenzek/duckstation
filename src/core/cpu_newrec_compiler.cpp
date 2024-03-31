// SPDX-FileCopyrightText: 2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cpu_newrec_compiler.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/small_string.h"
#include "cpu_code_cache.h"
#include "cpu_core_private.h"
#include "cpu_disasm.h"
#include "cpu_pgxp.h"
#include "settings.h"
#include <cstdint>
#include <limits>
Log_SetChannel(NewRec::Compiler);

// TODO: direct link skip delay slot check
// TODO: speculative constants
// TODO: std::bitset in msvc has bounds checks even in release...

const std::array<std::array<const void*, 2>, 3> CPU::NewRec::Compiler::s_pgxp_mem_load_functions = {
  {{{reinterpret_cast<const void*>(&PGXP::CPU_LBx), reinterpret_cast<const void*>(&PGXP::CPU_LBx)}},
   {{reinterpret_cast<const void*>(&PGXP::CPU_LHU), reinterpret_cast<const void*>(&PGXP::CPU_LH)}},
   {{reinterpret_cast<const void*>(&PGXP::CPU_LW)}}}};
const std::array<const void*, 3> CPU::NewRec::Compiler::s_pgxp_mem_store_functions = {
  {reinterpret_cast<const void*>(&PGXP::CPU_SB), reinterpret_cast<const void*>(&PGXP::CPU_SH),
   reinterpret_cast<const void*>(&PGXP::CPU_SW)}};

CPU::NewRec::Compiler::Compiler() = default;

CPU::NewRec::Compiler::~Compiler() = default;

void CPU::NewRec::Compiler::Reset(CodeCache::Block* block, u8* code_buffer, u32 code_buffer_space, u8* far_code_buffer,
                                  u32 far_code_space)
{
  m_block = block;
  m_compiler_pc = block->pc;
  m_cycles = 0;
  m_gte_done_cycle = 0;
  inst = nullptr;
  iinfo = nullptr;
  m_current_instruction_pc = 0;
  m_current_instruction_branch_delay_slot = false;
  m_dirty_pc = false;
  m_dirty_instruction_bits = false;
  m_dirty_gte_done_cycle = true;
  m_block_ended = false;
  m_constant_reg_values.fill(0);
  m_constant_regs_valid.reset();
  m_constant_regs_dirty.reset();

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
    ClearHostReg(i);
  m_register_alloc_counter = 0;

  m_constant_reg_values[static_cast<u32>(Reg::zero)] = 0;
  m_constant_regs_valid.set(static_cast<u32>(Reg::zero));

  m_load_delay_dirty = EMULATE_LOAD_DELAYS;
  m_load_delay_register = Reg::count;
  m_load_delay_value_register = NUM_HOST_REGS;

  InitSpeculativeRegs();
}

void CPU::NewRec::Compiler::BeginBlock()
{
#if 0
  GenerateCall(reinterpret_cast<const void*>(&CPU::CodeCache::LogCurrentState));
#endif

  if (m_block->protection == CodeCache::PageProtectionMode::ManualCheck)
  {
    Log_DebugPrintf("Generate manual protection for PC %08X", m_block->pc);
    const u8* ram_ptr = Bus::g_ram + VirtualAddressToPhysical(m_block->pc);
    const u8* shadow_ptr = reinterpret_cast<const u8*>(m_block->Instructions());
    GenerateBlockProtectCheck(ram_ptr, shadow_ptr, m_block->size * sizeof(Instruction));
  }

  if (m_block->uncached_fetch_ticks > 0 || m_block->icache_line_count > 0)
    GenerateICacheCheckAndUpdate();

  if (g_settings.bios_tty_logging)
  {
    if (m_block->pc == 0xa0)
      GenerateCall(reinterpret_cast<const void*>(&CPU::HandleA0Syscall));
    else if (m_block->pc == 0xb0)
      GenerateCall(reinterpret_cast<const void*>(&CPU::HandleB0Syscall));
  }

  inst = m_block->Instructions();
  iinfo = m_block->InstructionsInfo();
  m_current_instruction_pc = m_block->pc;
  m_current_instruction_branch_delay_slot = false;
  m_compiler_pc += sizeof(Instruction);
  m_dirty_pc = true;
  m_dirty_instruction_bits = true;
}

const void* CPU::NewRec::Compiler::CompileBlock(CodeCache::Block* block, u32* host_code_size, u32* host_far_code_size)
{
  JitCodeBuffer& buffer = CodeCache::GetCodeBuffer();
  Reset(block, buffer.GetFreeCodePointer(), buffer.GetFreeCodeSpace(), buffer.GetFreeFarCodePointer(),
        buffer.GetFreeFarCodeSpace());

  Log_DebugPrintf("Block range: %08X -> %08X", block->pc, block->pc + block->size * 4);

  BeginBlock();

  for (;;)
  {
    CompileInstruction();

    if (m_block_ended || iinfo->is_last_instruction)
    {
      if (!m_block_ended)
      {
        // Block was truncated. Link it.
        EndBlock(m_compiler_pc, false);
      }

      break;
    }

    inst++;
    iinfo++;
    m_current_instruction_pc += sizeof(Instruction);
    m_compiler_pc += sizeof(Instruction);
    m_dirty_pc = true;
    m_dirty_instruction_bits = true;
  }

  // Nothing should be valid anymore
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
    DebugAssert(!IsHostRegAllocated(i));
  for (u32 i = 1; i < static_cast<u32>(Reg::count); i++)
    DebugAssert(!m_constant_regs_dirty.test(i) && !m_constant_regs_valid.test(i));
  m_speculative_constants.memory.clear();

  u32 code_size, far_code_size;
  const void* code = EndCompile(&code_size, &far_code_size);
  *host_code_size = code_size;
  *host_far_code_size = far_code_size;
  buffer.CommitCode(code_size);
  buffer.CommitFarCode(far_code_size);

  return code;
}

void CPU::NewRec::Compiler::SetConstantReg(Reg r, u32 v)
{
  DebugAssert(r < Reg::count && r != Reg::zero);

  // There might still be an incoming load delay which we need to cancel.
  CancelLoadDelaysToReg(r);

  if (m_constant_regs_valid.test(static_cast<u32>(r)) && m_constant_reg_values[static_cast<u8>(r)] == v)
  {
    // Shouldn't be any host regs though.
    DebugAssert(!CheckHostReg(0, HR_TYPE_CPU_REG, r).has_value());
    return;
  }

  m_constant_reg_values[static_cast<u32>(r)] = v;
  m_constant_regs_valid.set(static_cast<u32>(r));
  m_constant_regs_dirty.set(static_cast<u32>(r));

  if (const std::optional<u32> hostreg = CheckHostReg(0, HR_TYPE_CPU_REG, r); hostreg.has_value())
  {
    Log_DebugPrintf("Discarding guest register %s in host register %s due to constant set", GetRegName(r),
                    GetHostRegName(hostreg.value()));
    FreeHostReg(hostreg.value());
  }
}

void CPU::NewRec::Compiler::CancelLoadDelaysToReg(Reg reg)
{
  if (m_load_delay_register != reg)
    return;

  Log_DebugPrintf("Cancelling load delay to %s", GetRegName(reg));
  m_load_delay_register = Reg::count;
  if (m_load_delay_value_register != NUM_HOST_REGS)
    ClearHostReg(m_load_delay_value_register);
}

void CPU::NewRec::Compiler::UpdateLoadDelay()
{
  if (m_load_delay_dirty)
  {
    // we shouldn't have a static load delay.
    DebugAssert(!HasLoadDelay());

    // have to invalidate registers, we might have one of them cached
    // TODO: double check the order here, will we trash a new value? we shouldn't...
    // thankfully since this only happens on the first instruction, we can get away with just killing anything which
    // isn't in write mode, because nothing could've been written before it, and the new value overwrites any
    // load-delayed value
    Log_DebugPrintf("Invalidating non-dirty registers, and flushing load delay from state");

    constexpr u32 req_flags = (HR_ALLOCATED | HR_MODE_WRITE);

    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      HostRegAlloc& ra = m_host_regs[i];
      if (ra.type != HR_TYPE_CPU_REG || !IsHostRegAllocated(i) || ((ra.flags & req_flags) == req_flags))
        continue;

      Log_DebugPrintf("Freeing non-dirty cached register %s in %s", GetRegName(ra.reg), GetHostRegName(i));
      DebugAssert(!(ra.flags & HR_MODE_WRITE));
      ClearHostReg(i);
    }

    // remove any non-dirty constants too
    for (u32 i = 1; i < static_cast<u32>(Reg::count); i++)
    {
      if (!HasConstantReg(static_cast<Reg>(i)) || HasDirtyConstantReg(static_cast<Reg>(i)))
        continue;

      Log_DebugPrintf("Clearing non-dirty constant %s", GetRegName(static_cast<Reg>(i)));
      ClearConstantReg(static_cast<Reg>(i));
    }

    Flush(FLUSH_LOAD_DELAY_FROM_STATE);
  }

  // commit the delayed register load
  FinishLoadDelay();

  // move next load delay forward
  if (m_next_load_delay_register != Reg::count)
  {
    // if it somehow got flushed, read it back in.
    if (m_next_load_delay_value_register == NUM_HOST_REGS)
    {
      AllocateHostReg(HR_MODE_READ, HR_TYPE_NEXT_LOAD_DELAY_VALUE, m_next_load_delay_register);
      DebugAssert(m_next_load_delay_value_register != NUM_HOST_REGS);
    }

    HostRegAlloc& ra = m_host_regs[m_next_load_delay_value_register];
    ra.flags |= HR_MODE_WRITE;
    ra.type = HR_TYPE_LOAD_DELAY_VALUE;

    m_load_delay_register = m_next_load_delay_register;
    m_load_delay_value_register = m_next_load_delay_value_register;
    m_next_load_delay_register = Reg::count;
    m_next_load_delay_value_register = NUM_HOST_REGS;
  }
}

void CPU::NewRec::Compiler::FinishLoadDelay()
{
  DebugAssert(!m_load_delay_dirty);
  if (!HasLoadDelay())
    return;

  // we may need to reload the value..
  if (m_load_delay_value_register == NUM_HOST_REGS)
  {
    AllocateHostReg(HR_MODE_READ, HR_TYPE_LOAD_DELAY_VALUE, m_load_delay_register);
    DebugAssert(m_load_delay_value_register != NUM_HOST_REGS);
  }

  // kill any (old) cached value for this register
  DeleteMIPSReg(m_load_delay_register, false);

  Log_DebugPrintf("Finished delayed load to %s in host register %s", GetRegName(m_load_delay_register),
                  GetHostRegName(m_load_delay_value_register));

  // and swap the mode over so it gets written back later
  HostRegAlloc& ra = m_host_regs[m_load_delay_value_register];
  DebugAssert(ra.reg == m_load_delay_register);
  ra.flags = (ra.flags & IMMUTABLE_HR_FLAGS) | HR_ALLOCATED | HR_MODE_READ | HR_MODE_WRITE;
  ra.counter = m_register_alloc_counter++;
  ra.type = HR_TYPE_CPU_REG;

  // constants are gone
  Log_DebugPrintf("Clearing constant in %s due to load delay", GetRegName(m_load_delay_register));
  ClearConstantReg(m_load_delay_register);

  m_load_delay_register = Reg::count;
  m_load_delay_value_register = NUM_HOST_REGS;
}

void CPU::NewRec::Compiler::FinishLoadDelayToReg(Reg reg)
{
  if (m_load_delay_dirty)
  {
    // inter-block :(
    UpdateLoadDelay();
    return;
  }

  if (m_load_delay_register != reg)
    return;

  FinishLoadDelay();
}

u32 CPU::NewRec::Compiler::GetFlagsForNewLoadDelayedReg() const
{
  return g_settings.gpu_pgxp_enable ? (HR_MODE_WRITE | HR_CALLEE_SAVED) : (HR_MODE_WRITE);
}

void CPU::NewRec::Compiler::ClearConstantReg(Reg r)
{
  DebugAssert(r < Reg::count && r != Reg::zero);
  m_constant_reg_values[static_cast<u32>(r)] = 0;
  m_constant_regs_valid.reset(static_cast<u32>(r));
  m_constant_regs_dirty.reset(static_cast<u32>(r));
}

void CPU::NewRec::Compiler::FlushConstantRegs(bool invalidate)
{
  for (u32 i = 1; i < static_cast<u32>(Reg::count); i++)
  {
    if (m_constant_regs_dirty.test(static_cast<u32>(i)))
      FlushConstantReg(static_cast<Reg>(i));
    if (invalidate)
      ClearConstantReg(static_cast<Reg>(i));
  }
}

CPU::Reg CPU::NewRec::Compiler::MipsD() const
{
  return inst->r.rd;
}

u32 CPU::NewRec::Compiler::GetConditionalBranchTarget(CompileFlags cf) const
{
  // compiler pc has already been advanced when swapping branch delay slots
  const u32 current_pc = m_compiler_pc - (cf.delay_slot_swapped ? sizeof(Instruction) : 0);
  return current_pc + (inst->i.imm_sext32() << 2);
}

u32 CPU::NewRec::Compiler::GetBranchReturnAddress(CompileFlags cf) const
{
  // compiler pc has already been advanced when swapping branch delay slots
  return m_compiler_pc + (cf.delay_slot_swapped ? 0 : sizeof(Instruction));
}

bool CPU::NewRec::Compiler::TrySwapDelaySlot(Reg rs, Reg rt, Reg rd)
{
  if constexpr (!SWAP_BRANCH_DELAY_SLOTS)
    return false;

  const Instruction* next_instruction = inst + 1;
  DebugAssert(next_instruction < (m_block->Instructions() + m_block->size));

  const Reg opcode_rs = next_instruction->r.rs;
  const Reg opcode_rt = next_instruction->r.rt;
  const Reg opcode_rd = next_instruction->r.rd;

#ifdef _DEBUG
  TinyString disasm;
  DisassembleInstruction(&disasm, m_current_instruction_pc + 4, next_instruction->bits);
#endif

  // Just in case we read it in the instruction.. but the block should end after this.
  const Instruction* const backup_instruction = inst;
  const u32 backup_instruction_pc = m_current_instruction_pc;
  const bool backup_instruction_delay_slot = m_current_instruction_branch_delay_slot;

  if (next_instruction->bits == 0)
  {
    // nop
    goto is_safe;
  }

  // can't swap when the branch is the first instruction because of bloody load delays
  if ((EMULATE_LOAD_DELAYS && m_block->pc == m_current_instruction_pc) || m_load_delay_dirty ||
      (HasLoadDelay() && (m_load_delay_register == rs || m_load_delay_register == rt || m_load_delay_register == rd)))
  {
    goto is_unsafe;
  }

  switch (next_instruction->op)
  {
    case InstructionOp::addi:
    case InstructionOp::addiu:
    case InstructionOp::slti:
    case InstructionOp::sltiu:
    case InstructionOp::andi:
    case InstructionOp::ori:
    case InstructionOp::xori:
    case InstructionOp::lui:
    case InstructionOp::lb:
    case InstructionOp::lh:
    case InstructionOp::lwl:
    case InstructionOp::lw:
    case InstructionOp::lbu:
    case InstructionOp::lhu:
    case InstructionOp::lwr:
    case InstructionOp::sb:
    case InstructionOp::sh:
    case InstructionOp::swl:
    case InstructionOp::sw:
    case InstructionOp::swr:
    {
      if ((rs != Reg::zero && rs == opcode_rt) || (rt != Reg::zero && rt == opcode_rt) ||
          (rd != Reg::zero && (rd == opcode_rs || rd == opcode_rt)) ||
          (HasLoadDelay() && (m_load_delay_register == opcode_rs || m_load_delay_register == opcode_rt)))
      {
        goto is_unsafe;
      }
    }
    break;

    case InstructionOp::lwc2: // LWC2
    case InstructionOp::swc2: // SWC2
      break;

    case InstructionOp::funct: // SPECIAL
    {
      switch (next_instruction->r.funct)
      {
        case InstructionFunct::sll:
        case InstructionFunct::srl:
        case InstructionFunct::sra:
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
        {
          if ((rs != Reg::zero && rs == opcode_rd) || (rt != Reg::zero && rt == opcode_rd) ||
              (rd != Reg::zero && (rd == opcode_rs || rd == opcode_rt)) ||
              (HasLoadDelay() && (m_load_delay_register == opcode_rs || m_load_delay_register == opcode_rt ||
                                  m_load_delay_register == opcode_rd)))
          {
            goto is_unsafe;
          }
        }
        break;

        case InstructionFunct::mult:
        case InstructionFunct::multu:
        case InstructionFunct::div:
        case InstructionFunct::divu:
        {
          if (HasLoadDelay() && (m_load_delay_register == opcode_rs || m_load_delay_register == opcode_rt))
            goto is_unsafe;
        }
        break;

        default:
          goto is_unsafe;
      }
    }
    break;

    case InstructionOp::cop0: // COP0
    case InstructionOp::cop1: // COP1
    case InstructionOp::cop2: // COP2
    case InstructionOp::cop3: // COP3
    {
      if (next_instruction->cop.IsCommonInstruction())
      {
        switch (next_instruction->cop.CommonOp())
        {
          case CopCommonInstruction::mfcn: // MFC0
          case CopCommonInstruction::cfcn: // CFC0
          {
            if ((rs != Reg::zero && rs == opcode_rt) || (rt != Reg::zero && rt == opcode_rt) ||
                (rd != Reg::zero && rd == opcode_rt) || (HasLoadDelay() && m_load_delay_register == opcode_rt))
            {
              goto is_unsafe;
            }
          }
          break;

          case CopCommonInstruction::mtcn: // MTC0
          case CopCommonInstruction::ctcn: // CTC0
            break;
        }
      }
      else
      {
        // swap when it's GTE
        if (next_instruction->op != InstructionOp::cop2)
          goto is_unsafe;
      }
    }
    break;

    default:
      goto is_unsafe;
  }

is_safe:
#ifdef _DEBUG
  Log_DebugFmt("Swapping delay slot {:08X} {}", m_current_instruction_pc + 4, disasm);
#endif

  CompileBranchDelaySlot();

  inst = backup_instruction;
  m_current_instruction_pc = backup_instruction_pc;
  m_current_instruction_branch_delay_slot = backup_instruction_delay_slot;
  return true;

is_unsafe:
#ifdef _DEBUG
  Log_DebugFmt("NOT swapping delay slot {:08X} {}", m_current_instruction_pc + 4, disasm);
#endif

  return false;
}

void CPU::NewRec::Compiler::SetCompilerPC(u32 newpc)
{
  m_compiler_pc = newpc;
  m_dirty_pc = true;
}

u32 CPU::NewRec::Compiler::GetFreeHostReg(u32 flags)
{
  const u32 req_flags = HR_USABLE | (flags & HR_CALLEE_SAVED);

  u32 fallback = NUM_HOST_REGS;
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    if ((m_host_regs[i].flags & (req_flags | HR_NEEDED | HR_ALLOCATED)) == req_flags)
    {
      // Prefer callee-saved registers.
      if (m_host_regs[i].flags & HR_CALLEE_SAVED)
        return i;
      else if (fallback == NUM_HOST_REGS)
        fallback = i;
    }
  }
  if (fallback != NUM_HOST_REGS)
    return fallback;

  // find register with lowest counter
  u32 lowest = NUM_HOST_REGS;
  u16 lowest_count = std::numeric_limits<u16>::max();
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    const HostRegAlloc& ra = m_host_regs[i];
    if ((ra.flags & (req_flags | HR_NEEDED)) != req_flags)
      continue;

    DebugAssert(ra.flags & HR_ALLOCATED);
    if (ra.type == HR_TYPE_TEMP)
    {
      // can't punt temps
      continue;
    }

    if (ra.counter < lowest_count)
    {
      lowest = i;
      lowest_count = ra.counter;
    }
  }

  //

  AssertMsg(lowest != NUM_HOST_REGS, "Register allocation failed.");

  const HostRegAlloc& ra = m_host_regs[lowest];
  switch (ra.type)
  {
    case HR_TYPE_CPU_REG:
    {
      // If the register is needed later, and we're allocating a callee-saved register, try moving it to a caller-saved
      // register.
      if (iinfo->UsedTest(ra.reg) && flags & HR_CALLEE_SAVED)
      {
        u32 caller_saved_lowest = NUM_HOST_REGS;
        u16 caller_saved_lowest_count = std::numeric_limits<u16>::max();
        for (u32 i = 0; i < NUM_HOST_REGS; i++)
        {
          constexpr u32 caller_req_flags = HR_USABLE;
          constexpr u32 caller_req_mask = HR_USABLE | HR_NEEDED | HR_CALLEE_SAVED;
          const HostRegAlloc& caller_ra = m_host_regs[i];
          if ((caller_ra.flags & caller_req_mask) != caller_req_flags)
            continue;

          if (!(caller_ra.flags & HR_ALLOCATED))
          {
            caller_saved_lowest = i;
            caller_saved_lowest_count = 0;
            break;
          }

          if (caller_ra.type == HR_TYPE_TEMP)
            continue;

          if (caller_ra.counter < caller_saved_lowest_count)
          {
            caller_saved_lowest = i;
            caller_saved_lowest_count = caller_ra.counter;
          }
        }

        if (caller_saved_lowest_count < lowest_count)
        {
          Log_DebugPrintf("Moving caller-saved host register %s with MIPS register %s to %s for allocation",
                          GetHostRegName(lowest), GetRegName(ra.reg), GetHostRegName(caller_saved_lowest));
          if (IsHostRegAllocated(caller_saved_lowest))
            FreeHostReg(caller_saved_lowest);
          CopyHostReg(caller_saved_lowest, lowest);
          SwapHostRegAlloc(caller_saved_lowest, lowest);
          DebugAssert(!IsHostRegAllocated(lowest));
          return lowest;
        }
      }

      Log_DebugPrintf("Freeing register %s in host register %s for allocation", GetRegName(ra.reg),
                      GetHostRegName(lowest));
    }
    break;
    case HR_TYPE_LOAD_DELAY_VALUE:
    {
      Log_DebugPrintf("Freeing load delay register %s in host register %s for allocation", GetHostRegName(lowest),
                      GetRegName(ra.reg));
    }
    break;
    case HR_TYPE_NEXT_LOAD_DELAY_VALUE:
    {
      Log_DebugPrintf("Freeing next load delay register %s in host register %s due for allocation", GetRegName(ra.reg),
                      GetHostRegName(lowest));
    }
    break;
    default:
    {
      Panic("Unknown type freed");
    }
    break;
  }

  FreeHostReg(lowest);
  return lowest;
}

const char* CPU::NewRec::Compiler::GetReadWriteModeString(u32 flags)
{
  if ((flags & (HR_MODE_READ | HR_MODE_WRITE)) == (HR_MODE_READ | HR_MODE_WRITE))
    return "read-write";
  else if (flags & HR_MODE_READ)
    return "read-only";
  else if (flags & HR_MODE_WRITE)
    return "write-only";
  else
    return "UNKNOWN";
}

u32 CPU::NewRec::Compiler::AllocateHostReg(u32 flags, HostRegAllocType type /* = HR_TYPE_TEMP */,
                                           Reg reg /* = Reg::count */)
{
  // Cancel any load delays before booting anything out
  if (flags & HR_MODE_WRITE && (type == HR_TYPE_CPU_REG || type == HR_TYPE_NEXT_LOAD_DELAY_VALUE))
    CancelLoadDelaysToReg(reg);

  // Already have a matching type?
  if (type != HR_TYPE_TEMP)
  {
    const std::optional<u32> check_reg = CheckHostReg(flags, type, reg);

    // shouldn't be allocating >1 load delay in a single instruction..
    // TODO: prefer callee saved registers for load delay
    DebugAssert((type != HR_TYPE_LOAD_DELAY_VALUE && type != HR_TYPE_NEXT_LOAD_DELAY_VALUE) || !check_reg.has_value());
    if (check_reg.has_value())
      return check_reg.value();
  }

  const u32 hreg = GetFreeHostReg(flags);
  HostRegAlloc& ra = m_host_regs[hreg];
  ra.flags = (ra.flags & IMMUTABLE_HR_FLAGS) | (flags & ALLOWED_HR_FLAGS) | HR_ALLOCATED | HR_NEEDED;
  ra.type = type;
  ra.reg = reg;
  ra.counter = m_register_alloc_counter++;

  switch (type)
  {
    case HR_TYPE_CPU_REG:
    {
      DebugAssert(reg != Reg::zero);

      Log_DebugPrintf("Allocate host reg %s to guest reg %s in %s mode", GetHostRegName(hreg), GetRegName(reg),
                      GetReadWriteModeString(flags));

      if (flags & HR_MODE_READ)
      {
        DebugAssert(ra.reg > Reg::zero && ra.reg < Reg::count);

        if (HasConstantReg(reg))
        {
          // may as well flush it now
          Log_DebugPrintf("Flush constant register in guest reg %s to host reg %s", GetRegName(reg),
                          GetHostRegName(hreg));
          LoadHostRegWithConstant(hreg, GetConstantRegU32(reg));
          m_constant_regs_dirty.reset(static_cast<u8>(reg));
          ra.flags |= HR_MODE_WRITE;
        }
        else
        {
          LoadHostRegFromCPUPointer(hreg, &g_state.regs.r[static_cast<u8>(reg)]);
        }
      }

      if (flags & HR_MODE_WRITE && HasConstantReg(reg))
      {
        DebugAssert(reg != Reg::zero);
        Log_DebugPrintf("Clearing constant register in guest reg %s due to write mode in %s", GetRegName(reg),
                        GetHostRegName(hreg));

        ClearConstantReg(reg);
      }
    }
    break;

    case HR_TYPE_LOAD_DELAY_VALUE:
    {
      DebugAssert(!m_load_delay_dirty && (!HasLoadDelay() || !(flags & HR_MODE_WRITE)));
      Log_DebugPrintf("Allocating load delayed guest register %s in host reg %s in %s mode", GetRegName(reg),
                      GetHostRegName(hreg), GetReadWriteModeString(flags));
      m_load_delay_register = reg;
      m_load_delay_value_register = hreg;
      if (flags & HR_MODE_READ)
        LoadHostRegFromCPUPointer(hreg, &g_state.load_delay_value);
    }
    break;

    case HR_TYPE_NEXT_LOAD_DELAY_VALUE:
    {
      Log_DebugPrintf("Allocating next load delayed guest register %s in host reg %s in %s mode", GetRegName(reg),
                      GetHostRegName(hreg), GetReadWriteModeString(flags));
      m_next_load_delay_register = reg;
      m_next_load_delay_value_register = hreg;
      if (flags & HR_MODE_READ)
        LoadHostRegFromCPUPointer(hreg, &g_state.next_load_delay_value);
    }
    break;

    case HR_TYPE_TEMP:
    {
      DebugAssert(!(flags & (HR_MODE_READ | HR_MODE_WRITE)));
      Log_DebugPrintf("Allocate host reg %s as temporary", GetHostRegName(hreg));
    }
    break;

    default:
      Panic("Unknown type");
      break;
  }

  return hreg;
}

std::optional<u32> CPU::NewRec::Compiler::CheckHostReg(u32 flags, HostRegAllocType type /* = HR_TYPE_TEMP */,
                                                       Reg reg /* = Reg::count */)
{
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];
    if (!(ra.flags & HR_ALLOCATED) || ra.type != type || ra.reg != reg)
      continue;

    DebugAssert(ra.flags & HR_MODE_READ);
    if (flags & HR_MODE_WRITE)
    {
      DebugAssert(type == HR_TYPE_CPU_REG);
      if (!(ra.flags & HR_MODE_WRITE))
      {
        Log_DebugPrintf("Switch guest reg %s from read to read-write in host reg %s", GetRegName(reg),
                        GetHostRegName(i));
      }

      if (HasConstantReg(reg))
      {
        DebugAssert(reg != Reg::zero);
        Log_DebugPrintf("Clearing constant register in guest reg %s due to write mode in %s", GetRegName(reg),
                        GetHostRegName(i));

        ClearConstantReg(reg);
      }
    }

    ra.flags |= (flags & ALLOWED_HR_FLAGS) | HR_NEEDED;
    ra.counter = m_register_alloc_counter++;

    // Need a callee saved reg?
    if (flags & HR_CALLEE_SAVED && !(ra.flags & HR_CALLEE_SAVED))
    {
      // Need to move it to one which is
      const u32 new_reg = GetFreeHostReg(HR_CALLEE_SAVED);
      Log_DebugPrintf("Rename host reg %s to %s for callee saved", GetHostRegName(i), GetHostRegName(new_reg));

      CopyHostReg(new_reg, i);
      SwapHostRegAlloc(i, new_reg);
      DebugAssert(!IsHostRegAllocated(i));
      return new_reg;
    }

    return i;
  }

  return std::nullopt;
}

u32 CPU::NewRec::Compiler::AllocateTempHostReg(u32 flags)
{
  return AllocateHostReg(flags, HR_TYPE_TEMP);
}

void CPU::NewRec::Compiler::SwapHostRegAlloc(u32 lhs, u32 rhs)
{
  HostRegAlloc& lra = m_host_regs[lhs];
  HostRegAlloc& rra = m_host_regs[rhs];

  const u8 lra_flags = lra.flags;
  lra.flags = (lra.flags & IMMUTABLE_HR_FLAGS) | (rra.flags & ~IMMUTABLE_HR_FLAGS);
  rra.flags = (rra.flags & IMMUTABLE_HR_FLAGS) | (lra_flags & ~IMMUTABLE_HR_FLAGS);
  std::swap(lra.type, rra.type);
  std::swap(lra.reg, rra.reg);
  std::swap(lra.counter, rra.counter);
}

void CPU::NewRec::Compiler::FlushHostReg(u32 reg)
{
  HostRegAlloc& ra = m_host_regs[reg];
  if (ra.flags & HR_MODE_WRITE)
  {
    switch (ra.type)
    {
      case HR_TYPE_CPU_REG:
      {
        DebugAssert(ra.reg > Reg::zero && ra.reg < Reg::count);
        Log_DebugPrintf("Flushing register %s in host register %s to state", GetRegName(ra.reg), GetHostRegName(reg));
        StoreHostRegToCPUPointer(reg, &g_state.regs.r[static_cast<u8>(ra.reg)]);
      }
      break;

      case HR_TYPE_LOAD_DELAY_VALUE:
      {
        DebugAssert(m_load_delay_value_register == reg);
        Log_DebugPrintf("Flushing load delayed register %s in host register %s to state", GetRegName(ra.reg),
                        GetHostRegName(reg));

        StoreHostRegToCPUPointer(reg, &g_state.load_delay_value);
        m_load_delay_value_register = NUM_HOST_REGS;
      }
      break;

      case HR_TYPE_NEXT_LOAD_DELAY_VALUE:
      {
        DebugAssert(m_next_load_delay_value_register == reg);
        Log_WarningPrintf("Flushing NEXT load delayed register %s in host register %s to state", GetRegName(ra.reg),
                          GetHostRegName(reg));

        StoreHostRegToCPUPointer(reg, &g_state.next_load_delay_value);
        m_next_load_delay_value_register = NUM_HOST_REGS;
      }
      break;

      default:
        break;
    }

    ra.flags = (ra.flags & ~HR_MODE_WRITE) | HR_MODE_READ;
  }
}

void CPU::NewRec::Compiler::FreeHostReg(u32 reg)
{
  DebugAssert(IsHostRegAllocated(reg));
  Log_DebugPrintf("Freeing host register %s", GetHostRegName(reg));
  FlushHostReg(reg);
  ClearHostReg(reg);
}

void CPU::NewRec::Compiler::ClearHostReg(u32 reg)
{
  HostRegAlloc& ra = m_host_regs[reg];
  ra.flags &= IMMUTABLE_HR_FLAGS;
  ra.type = HR_TYPE_TEMP;
  ra.counter = 0;
  ra.reg = Reg::count;
}

void CPU::NewRec::Compiler::MarkRegsNeeded(HostRegAllocType type, Reg reg)
{
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];
    if (ra.flags & HR_ALLOCATED && ra.type == type && ra.reg == reg)
      ra.flags |= HR_NEEDED;
  }
}

void CPU::NewRec::Compiler::RenameHostReg(u32 reg, u32 new_flags, HostRegAllocType new_type, Reg new_reg)
{
  // only supported for cpu regs for now
  DebugAssert(new_type == HR_TYPE_TEMP || new_type == HR_TYPE_CPU_REG || new_type == HR_TYPE_NEXT_LOAD_DELAY_VALUE);

  const std::optional<u32> old_reg = CheckHostReg(0, new_type, new_reg);
  if (old_reg.has_value())
  {
    // don't writeback
    ClearHostReg(old_reg.value());
  }

  // kill any load delay to this reg
  if (new_type == HR_TYPE_CPU_REG || new_type == HR_TYPE_NEXT_LOAD_DELAY_VALUE)
    CancelLoadDelaysToReg(new_reg);

  if (new_type == HR_TYPE_CPU_REG)
  {
    Log_DebugPrintf("Renaming host reg %s to guest reg %s", GetHostRegName(reg), GetRegName(new_reg));
  }
  else if (new_type == HR_TYPE_NEXT_LOAD_DELAY_VALUE)
  {
    Log_DebugPrintf("Renaming host reg %s to load delayed guest reg %s", GetHostRegName(reg), GetRegName(new_reg));
    DebugAssert(m_next_load_delay_register == Reg::count && m_next_load_delay_value_register == NUM_HOST_REGS);
    m_next_load_delay_register = new_reg;
    m_next_load_delay_value_register = reg;
  }
  else
  {
    Log_DebugPrintf("Renaming host reg %s to temp", GetHostRegName(reg));
  }

  HostRegAlloc& ra = m_host_regs[reg];
  ra.flags = (ra.flags & IMMUTABLE_HR_FLAGS) | HR_NEEDED | HR_ALLOCATED | (new_flags & ALLOWED_HR_FLAGS);
  ra.counter = m_register_alloc_counter++;
  ra.type = new_type;
  ra.reg = new_reg;
}

void CPU::NewRec::Compiler::ClearHostRegNeeded(u32 reg)
{
  DebugAssert(reg < NUM_HOST_REGS && IsHostRegAllocated(reg));
  HostRegAlloc& ra = m_host_regs[reg];
  if (ra.flags & HR_MODE_WRITE)
    ra.flags |= HR_MODE_READ;

  ra.flags &= ~HR_NEEDED;
}

void CPU::NewRec::Compiler::ClearHostRegsNeeded()
{
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];
    if (!(ra.flags & HR_ALLOCATED))
      continue;

    // shouldn't have any temps left
    DebugAssert(ra.type != HR_TYPE_TEMP);

    if (ra.flags & HR_MODE_WRITE)
      ra.flags |= HR_MODE_READ;

    ra.flags &= ~HR_NEEDED;
  }
}

void CPU::NewRec::Compiler::DeleteMIPSReg(Reg reg, bool flush)
{
  DebugAssert(reg != Reg::zero);

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];
    if (ra.flags & HR_ALLOCATED && ra.type == HR_TYPE_CPU_REG && ra.reg == reg)
    {
      if (flush)
        FlushHostReg(i);
      ClearHostReg(i);
      ClearConstantReg(reg);
      return;
    }
  }

  if (flush)
    FlushConstantReg(reg);
  ClearConstantReg(reg);
}

bool CPU::NewRec::Compiler::TryRenameMIPSReg(Reg to, Reg from, u32 fromhost, Reg other)
{
  // can't rename when in form Rd = Rs op Rt and Rd == Rs or Rd == Rt
  if (to == from || to == other || !iinfo->RenameTest(from))
    return false;

  Log_DebugPrintf("Renaming MIPS register %s to %s", GetRegName(from), GetRegName(to));

  if (iinfo->LiveTest(from))
    FlushHostReg(fromhost);

  // remove all references to renamed-to register
  DeleteMIPSReg(to, false);
  CancelLoadDelaysToReg(to);

  // and do the actual rename, new register has been modified.
  m_host_regs[fromhost].reg = to;
  m_host_regs[fromhost].flags |= HR_MODE_READ | HR_MODE_WRITE;
  return true;
}

void CPU::NewRec::Compiler::UpdateHostRegCounters()
{
  const CodeCache::InstructionInfo* const info_end = m_block->InstructionsInfo() + m_block->size;

  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    HostRegAlloc& ra = m_host_regs[i];
    if ((ra.flags & (HR_ALLOCATED | HR_NEEDED)) != HR_ALLOCATED)
      continue;

    // Try not to punt out load delays.
    if (ra.type != HR_TYPE_CPU_REG)
    {
      ra.counter = std::numeric_limits<u16>::max();
      continue;
    }

    DebugAssert(IsHostRegAllocated(i));
    const CodeCache::InstructionInfo* cur = iinfo;
    const Reg reg = ra.reg;
    if (!(cur->reg_flags[static_cast<u8>(reg)] & CodeCache::RI_USED))
    {
      ra.counter = 0;
      continue;
    }

    // order based on the number of instructions until this register is used
    u16 counter_val = std::numeric_limits<u16>::max();
    for (; cur != info_end; cur++, counter_val--)
    {
      if (cur->ReadsReg(reg))
        break;
    }

    ra.counter = counter_val;
  }
}

void CPU::NewRec::Compiler::Flush(u32 flags)
{
  // TODO: Flush unneeded caller-saved regs (backup/replace calle-saved needed with caller-saved)
  if (flags &
      (FLUSH_FREE_UNNEEDED_CALLER_SAVED_REGISTERS | FLUSH_FREE_CALLER_SAVED_REGISTERS | FLUSH_FREE_ALL_REGISTERS))
  {
    const u32 req_mask = (flags & FLUSH_FREE_ALL_REGISTERS) ?
                           HR_ALLOCATED :
                           ((flags & FLUSH_FREE_CALLER_SAVED_REGISTERS) ? (HR_ALLOCATED | HR_CALLEE_SAVED) :
                                                                          (HR_ALLOCATED | HR_CALLEE_SAVED | HR_NEEDED));
    constexpr u32 req_flags = HR_ALLOCATED;

    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      HostRegAlloc& ra = m_host_regs[i];
      if ((ra.flags & req_mask) == req_flags)
        FreeHostReg(i);
    }
  }

  if (flags & FLUSH_INVALIDATE_MIPS_REGISTERS)
  {
    for (u32 i = 0; i < NUM_HOST_REGS; i++)
    {
      HostRegAlloc& ra = m_host_regs[i];
      if (ra.flags & HR_ALLOCATED && ra.type == HR_TYPE_CPU_REG)
        FreeHostReg(i);
    }

    FlushConstantRegs(true);
  }
  else
  {
    if (flags & FLUSH_FLUSH_MIPS_REGISTERS)
    {
      for (u32 i = 0; i < NUM_HOST_REGS; i++)
      {
        HostRegAlloc& ra = m_host_regs[i];
        if ((ra.flags & (HR_ALLOCATED | HR_MODE_WRITE)) == (HR_ALLOCATED | HR_MODE_WRITE) && ra.type == HR_TYPE_CPU_REG)
          FlushHostReg(i);
      }

      // flush any constant registers which are dirty too
      FlushConstantRegs(false);
    }
  }

  if (flags & FLUSH_INVALIDATE_SPECULATIVE_CONSTANTS)
    InvalidateSpeculativeValues();
}

void CPU::NewRec::Compiler::FlushConstantReg(Reg r)
{
  DebugAssert(m_constant_regs_valid.test(static_cast<u32>(r)));
  Log_DebugPrintf("Writing back register %s with constant value 0x%08X", GetRegName(r),
                  m_constant_reg_values[static_cast<u32>(r)]);
  StoreConstantToCPUPointer(m_constant_reg_values[static_cast<u32>(r)], &g_state.regs.r[static_cast<u32>(r)]);
  m_constant_regs_dirty.reset(static_cast<u32>(r));
}

void CPU::NewRec::Compiler::BackupHostState()
{
  DebugAssert(m_host_state_backup_count < m_host_state_backup.size());

  // need to back up everything...
  HostStateBackup& bu = m_host_state_backup[m_host_state_backup_count];
  bu.cycles = m_cycles;
  bu.gte_done_cycle = m_gte_done_cycle;
  bu.compiler_pc = m_compiler_pc;
  bu.dirty_pc = m_dirty_pc;
  bu.dirty_instruction_bits = m_dirty_instruction_bits;
  bu.dirty_gte_done_cycle = m_dirty_gte_done_cycle;
  bu.block_ended = m_block_ended;
  bu.inst = inst;
  bu.iinfo = iinfo;
  bu.current_instruction_pc = m_current_instruction_pc;
  bu.current_instruction_delay_slot = m_current_instruction_branch_delay_slot;
  bu.const_regs_valid = m_constant_regs_valid;
  bu.const_regs_dirty = m_constant_regs_dirty;
  bu.const_regs_values = m_constant_reg_values;
  bu.host_regs = m_host_regs;
  bu.register_alloc_counter = m_register_alloc_counter;
  bu.load_delay_dirty = m_load_delay_dirty;
  bu.load_delay_register = m_load_delay_register;
  bu.load_delay_value_register = m_load_delay_value_register;
  bu.next_load_delay_register = m_next_load_delay_register;
  bu.next_load_delay_value_register = m_next_load_delay_value_register;
  m_host_state_backup_count++;
}

void CPU::NewRec::Compiler::RestoreHostState()
{
  DebugAssert(m_host_state_backup_count > 0);
  m_host_state_backup_count--;

  HostStateBackup& bu = m_host_state_backup[m_host_state_backup_count];
  m_host_regs = std::move(bu.host_regs);
  m_constant_reg_values = std::move(bu.const_regs_values);
  m_constant_regs_dirty = std::move(bu.const_regs_dirty);
  m_constant_regs_valid = std::move(bu.const_regs_valid);
  m_current_instruction_branch_delay_slot = bu.current_instruction_delay_slot;
  m_current_instruction_pc = bu.current_instruction_pc;
  inst = bu.inst;
  iinfo = bu.iinfo;
  m_block_ended = bu.block_ended;
  m_dirty_gte_done_cycle = bu.dirty_gte_done_cycle;
  m_dirty_instruction_bits = bu.dirty_instruction_bits;
  m_dirty_pc = bu.dirty_pc;
  m_compiler_pc = bu.compiler_pc;
  m_register_alloc_counter = bu.register_alloc_counter;
  m_load_delay_dirty = bu.load_delay_dirty;
  m_load_delay_register = bu.load_delay_register;
  m_load_delay_value_register = bu.load_delay_value_register;
  m_next_load_delay_register = bu.next_load_delay_register;
  m_next_load_delay_value_register = bu.next_load_delay_value_register;
  m_gte_done_cycle = bu.gte_done_cycle;
  m_cycles = bu.cycles;
}

void CPU::NewRec::Compiler::AddLoadStoreInfo(void* code_address, u32 code_size, u32 address_register, u32 data_register,
                                             MemoryAccessSize size, bool is_signed, bool is_load)
{
  DebugAssert(CodeCache::IsUsingFastmem());
  DebugAssert(address_register < NUM_HOST_REGS);
  DebugAssert(data_register < NUM_HOST_REGS);

  u32 gpr_bitmask = 0;
  for (u32 i = 0; i < NUM_HOST_REGS; i++)
  {
    if (IsHostRegAllocated(i))
      gpr_bitmask |= (1u << i);
  }

  CPU::CodeCache::AddLoadStoreInfo(code_address, code_size, m_current_instruction_pc, m_block->pc, m_cycles,
                                   gpr_bitmask, static_cast<u8>(address_register), static_cast<u8>(data_register), size,
                                   is_signed, is_load);
}

void CPU::NewRec::Compiler::CompileInstruction()
{
#ifdef _DEBUG
  TinyString str;
  DisassembleInstruction(&str, m_current_instruction_pc, inst->bits);
  Log_DebugFmt("Compiling{} {:08X}: {}", m_current_instruction_branch_delay_slot ? " branch delay slot" : "",
               m_current_instruction_pc, str);
#endif

  m_cycles++;

  if (IsNopInstruction(*inst))
  {
    UpdateLoadDelay();
    return;
  }

  switch (inst->op)
  {
#define PGXPFN(x) reinterpret_cast<const void*>(&PGXP::x)

      // clang-format off
      // TODO: PGXP for jalr

    case InstructionOp::funct:
    {
      switch (inst->r.funct)
      {
        case InstructionFunct::sll: CompileTemplate(&Compiler::Compile_sll_const, &Compiler::Compile_sll, PGXPFN(CPU_SLL), TF_WRITES_D | TF_READS_T); SpecExec_sll(); break;
        case InstructionFunct::srl: CompileTemplate(&Compiler::Compile_srl_const, &Compiler::Compile_srl, PGXPFN(CPU_SRL), TF_WRITES_D | TF_READS_T); SpecExec_srl(); break;
        case InstructionFunct::sra: CompileTemplate(&Compiler::Compile_sra_const, &Compiler::Compile_sra, PGXPFN(CPU_SRA), TF_WRITES_D | TF_READS_T); SpecExec_sra(); break;
        case InstructionFunct::sllv: CompileTemplate(&Compiler::Compile_sllv_const, &Compiler::Compile_sllv, PGXPFN(CPU_SLLV), TF_WRITES_D | TF_READS_S | TF_READS_T); SpecExec_sllv(); break;
        case InstructionFunct::srlv: CompileTemplate(&Compiler::Compile_srlv_const, &Compiler::Compile_srlv, PGXPFN(CPU_SRLV), TF_WRITES_D | TF_READS_S | TF_READS_T); SpecExec_srlv(); break;
        case InstructionFunct::srav: CompileTemplate(&Compiler::Compile_srav_const, &Compiler::Compile_srav, PGXPFN(CPU_SRAV), TF_WRITES_D | TF_READS_S | TF_READS_T); SpecExec_srav(); break;
        case InstructionFunct::jr: CompileTemplate(&Compiler::Compile_jr_const, &Compiler::Compile_jr, nullptr, TF_READS_S); break;
        case InstructionFunct::jalr: CompileTemplate(&Compiler::Compile_jalr_const, &Compiler::Compile_jalr, nullptr, /*TF_WRITES_D |*/ TF_READS_S | TF_NO_NOP); SpecExec_jalr(); break;
        case InstructionFunct::syscall: Compile_syscall(); break;
        case InstructionFunct::break_: Compile_break(); break;
        case InstructionFunct::mfhi: SpecCopyReg(inst->r.rd, Reg::hi); CompileMoveRegTemplate(inst->r.rd, Reg::hi, g_settings.gpu_pgxp_cpu); break;
        case InstructionFunct::mthi: SpecCopyReg(Reg::hi, inst->r.rs); CompileMoveRegTemplate(Reg::hi, inst->r.rs, g_settings.gpu_pgxp_cpu); break;
        case InstructionFunct::mflo: SpecCopyReg(inst->r.rd, Reg::lo); CompileMoveRegTemplate(inst->r.rd, Reg::lo, g_settings.gpu_pgxp_cpu); break;
        case InstructionFunct::mtlo: SpecCopyReg(Reg::lo, inst->r.rs); CompileMoveRegTemplate(Reg::lo, inst->r.rs, g_settings.gpu_pgxp_cpu); break;
        case InstructionFunct::mult: CompileTemplate(&Compiler::Compile_mult_const, &Compiler::Compile_mult, PGXPFN(CPU_MULT), TF_READS_S | TF_READS_T | TF_WRITES_LO | TF_WRITES_HI | TF_COMMUTATIVE); SpecExec_mult(); break;
        case InstructionFunct::multu: CompileTemplate(&Compiler::Compile_multu_const, &Compiler::Compile_multu, PGXPFN(CPU_MULTU), TF_READS_S | TF_READS_T | TF_WRITES_LO | TF_WRITES_HI | TF_COMMUTATIVE); SpecExec_multu(); break;
        case InstructionFunct::div: CompileTemplate(&Compiler::Compile_div_const, &Compiler::Compile_div, PGXPFN(CPU_DIV), TF_READS_S | TF_READS_T | TF_WRITES_LO | TF_WRITES_HI); SpecExec_div(); break;
        case InstructionFunct::divu: CompileTemplate(&Compiler::Compile_divu_const, &Compiler::Compile_divu, PGXPFN(CPU_DIVU), TF_READS_S | TF_READS_T | TF_WRITES_LO | TF_WRITES_HI); SpecExec_divu(); break;
        case InstructionFunct::add: CompileTemplate(&Compiler::Compile_add_const, &Compiler::Compile_add, PGXPFN(CPU_ADD), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_COMMUTATIVE | TF_CAN_OVERFLOW | TF_RENAME_WITH_ZERO_T); SpecExec_add(); break;
        case InstructionFunct::addu: CompileTemplate(&Compiler::Compile_addu_const, &Compiler::Compile_addu, PGXPFN(CPU_ADD), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_COMMUTATIVE | TF_RENAME_WITH_ZERO_T); SpecExec_addu(); break;
        case InstructionFunct::sub: CompileTemplate(&Compiler::Compile_sub_const, &Compiler::Compile_sub, PGXPFN(CPU_SUB), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_CAN_OVERFLOW | TF_RENAME_WITH_ZERO_T); SpecExec_sub(); break;
        case InstructionFunct::subu: CompileTemplate(&Compiler::Compile_subu_const, &Compiler::Compile_subu, PGXPFN(CPU_SUB), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_RENAME_WITH_ZERO_T); SpecExec_subu(); break;
        case InstructionFunct::and_: CompileTemplate(&Compiler::Compile_and_const, &Compiler::Compile_and, PGXPFN(CPU_AND_), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_COMMUTATIVE); SpecExec_and(); break;
        case InstructionFunct::or_: CompileTemplate(&Compiler::Compile_or_const, &Compiler::Compile_or, PGXPFN(CPU_OR_), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_COMMUTATIVE | TF_RENAME_WITH_ZERO_T); SpecExec_or(); break;
        case InstructionFunct::xor_: CompileTemplate(&Compiler::Compile_xor_const, &Compiler::Compile_xor, PGXPFN(CPU_XOR_), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_COMMUTATIVE | TF_RENAME_WITH_ZERO_T); SpecExec_xor(); break;
        case InstructionFunct::nor: CompileTemplate(&Compiler::Compile_nor_const, &Compiler::Compile_nor, PGXPFN(CPU_NOR), TF_WRITES_D | TF_READS_S | TF_READS_T | TF_COMMUTATIVE); SpecExec_nor(); break;
        case InstructionFunct::slt: CompileTemplate(&Compiler::Compile_slt_const, &Compiler::Compile_slt, PGXPFN(CPU_SLT), TF_WRITES_D | TF_READS_T | TF_READS_S); SpecExec_slt(); break;
        case InstructionFunct::sltu: CompileTemplate(&Compiler::Compile_sltu_const, &Compiler::Compile_sltu, PGXPFN(CPU_SLTU), TF_WRITES_D | TF_READS_T | TF_READS_S); SpecExec_sltu(); break;
        default: Compile_Fallback(); InvalidateSpeculativeValues(); TruncateBlock(); break;
      }
    }
    break;

    case InstructionOp::j: Compile_j(); break;
    case InstructionOp::jal: Compile_jal(); SpecExec_jal(); break;

    case InstructionOp::b: CompileTemplate(&Compiler::Compile_b_const, &Compiler::Compile_b, nullptr, TF_READS_S | TF_CAN_SWAP_DELAY_SLOT); SpecExec_b(); break;
    case InstructionOp::blez: CompileTemplate(&Compiler::Compile_blez_const, &Compiler::Compile_blez, nullptr, TF_READS_S | TF_CAN_SWAP_DELAY_SLOT); break;
    case InstructionOp::bgtz: CompileTemplate(&Compiler::Compile_bgtz_const, &Compiler::Compile_bgtz, nullptr, TF_READS_S | TF_CAN_SWAP_DELAY_SLOT); break;
    case InstructionOp::beq: CompileTemplate(&Compiler::Compile_beq_const, &Compiler::Compile_beq, nullptr, TF_READS_S | TF_READS_T | TF_COMMUTATIVE | TF_CAN_SWAP_DELAY_SLOT); break;
    case InstructionOp::bne: CompileTemplate(&Compiler::Compile_bne_const, &Compiler::Compile_bne, nullptr, TF_READS_S | TF_READS_T | TF_COMMUTATIVE | TF_CAN_SWAP_DELAY_SLOT); break;

    case InstructionOp::addi: CompileTemplate(&Compiler::Compile_addi_const, &Compiler::Compile_addi, PGXPFN(CPU_ADDI), TF_WRITES_T | TF_READS_S | TF_COMMUTATIVE | TF_CAN_OVERFLOW | TF_RENAME_WITH_ZERO_IMM); SpecExec_addi(); break;
    case InstructionOp::addiu: CompileTemplate(&Compiler::Compile_addiu_const, &Compiler::Compile_addiu, PGXPFN(CPU_ADDI), TF_WRITES_T | TF_READS_S | TF_COMMUTATIVE | TF_RENAME_WITH_ZERO_IMM); SpecExec_addiu(); break;
    case InstructionOp::slti: CompileTemplate(&Compiler::Compile_slti_const, &Compiler::Compile_slti, PGXPFN(CPU_SLTI), TF_WRITES_T | TF_READS_S); SpecExec_slti(); break;
    case InstructionOp::sltiu: CompileTemplate(&Compiler::Compile_sltiu_const, &Compiler::Compile_sltiu, PGXPFN(CPU_SLTIU), TF_WRITES_T | TF_READS_S); SpecExec_sltiu(); break;
    case InstructionOp::andi: CompileTemplate(&Compiler::Compile_andi_const, &Compiler::Compile_andi, PGXPFN(CPU_ANDI), TF_WRITES_T | TF_READS_S | TF_COMMUTATIVE); SpecExec_andi(); break;
    case InstructionOp::ori: CompileTemplate(&Compiler::Compile_ori_const, &Compiler::Compile_ori, PGXPFN(CPU_ORI), TF_WRITES_T | TF_READS_S | TF_COMMUTATIVE | TF_RENAME_WITH_ZERO_IMM); SpecExec_ori(); break;
    case InstructionOp::xori: CompileTemplate(&Compiler::Compile_xori_const, &Compiler::Compile_xori, PGXPFN(CPU_XORI), TF_WRITES_T | TF_READS_S | TF_COMMUTATIVE | TF_RENAME_WITH_ZERO_IMM); SpecExec_xori(); break;
    case InstructionOp::lui: Compile_lui(); SpecExec_lui(); break;

    case InstructionOp::lb: CompileLoadStoreTemplate(&Compiler::Compile_lxx, MemoryAccessSize::Byte, false, true, TF_READS_S | TF_WRITES_T | TF_LOAD_DELAY); SpecExec_lxx(MemoryAccessSize::Byte, true); break;
    case InstructionOp::lbu: CompileLoadStoreTemplate(&Compiler::Compile_lxx, MemoryAccessSize::Byte, false, false, TF_READS_S | TF_WRITES_T | TF_LOAD_DELAY); SpecExec_lxx(MemoryAccessSize::Byte, false); break;
    case InstructionOp::lh: CompileLoadStoreTemplate(&Compiler::Compile_lxx, MemoryAccessSize::HalfWord, false, true, TF_READS_S | TF_WRITES_T | TF_LOAD_DELAY); SpecExec_lxx(MemoryAccessSize::HalfWord, true); break;
    case InstructionOp::lhu: CompileLoadStoreTemplate(&Compiler::Compile_lxx, MemoryAccessSize::HalfWord, false, false, TF_READS_S | TF_WRITES_T | TF_LOAD_DELAY); SpecExec_lxx(MemoryAccessSize::HalfWord, false); break;
    case InstructionOp::lw: CompileLoadStoreTemplate(&Compiler::Compile_lxx, MemoryAccessSize::Word, false, false, TF_READS_S | TF_WRITES_T | TF_LOAD_DELAY); SpecExec_lxx(MemoryAccessSize::Word, false); break;
    case InstructionOp::lwl: CompileLoadStoreTemplate(&Compiler::Compile_lwx, MemoryAccessSize::Word, false, false, TF_READS_S | /*TF_READS_T | TF_WRITES_T | */TF_LOAD_DELAY); SpecExec_lwx(false); break;
    case InstructionOp::lwr: CompileLoadStoreTemplate(&Compiler::Compile_lwx, MemoryAccessSize::Word, false, false, TF_READS_S | /*TF_READS_T | TF_WRITES_T | */TF_LOAD_DELAY); SpecExec_lwx(true); break;
    case InstructionOp::sb: CompileLoadStoreTemplate(&Compiler::Compile_sxx, MemoryAccessSize::Byte, true, false, TF_READS_S | TF_READS_T); SpecExec_sxx(MemoryAccessSize::Byte); break;
    case InstructionOp::sh: CompileLoadStoreTemplate(&Compiler::Compile_sxx, MemoryAccessSize::HalfWord, true, false, TF_READS_S | TF_READS_T); SpecExec_sxx(MemoryAccessSize::HalfWord); break;
    case InstructionOp::sw: CompileLoadStoreTemplate(&Compiler::Compile_sxx, MemoryAccessSize::Word, true, false, TF_READS_S | TF_READS_T); SpecExec_sxx(MemoryAccessSize::Word); break;
    case InstructionOp::swl: CompileLoadStoreTemplate(&Compiler::Compile_swx, MemoryAccessSize::Word, false, false, TF_READS_S | /*TF_READS_T | TF_WRITES_T | */TF_LOAD_DELAY); SpecExec_swx(false); break;
    case InstructionOp::swr: CompileLoadStoreTemplate(&Compiler::Compile_swx, MemoryAccessSize::Word, false, false, TF_READS_S | /*TF_READS_T | TF_WRITES_T | */TF_LOAD_DELAY); SpecExec_swx(true); break;

    case InstructionOp::cop0:
      {
        if (inst->cop.IsCommonInstruction())
        {
          switch (inst->cop.CommonOp())
          {
            case CopCommonInstruction::mfcn: if (inst->r.rt != Reg::zero) { CompileTemplate(nullptr, &Compiler::Compile_mfc0, PGXPFN(CPU_MFC0), TF_WRITES_T | TF_LOAD_DELAY); } SpecExec_mfc0(); break;
            case CopCommonInstruction::mtcn: CompileTemplate(nullptr, &Compiler::Compile_mtc0, PGXPFN(CPU_MTC0), TF_READS_T); SpecExec_mtc0(); break;
            default: Compile_Fallback(); break;
          }
        }
        else
        {
          switch (inst->cop.Cop0Op())
          {
            case Cop0Instruction::rfe: CompileTemplate(nullptr, &Compiler::Compile_rfe, nullptr, 0); SpecExec_rfe(); break;
            default: Compile_Fallback(); break;
          }
        }
      }
      break;

    case InstructionOp::cop2:
      {
        if (inst->cop.IsCommonInstruction())
        {
          switch (inst->cop.CommonOp())
          {
            case CopCommonInstruction::mfcn: if (inst->r.rt != Reg::zero) { CompileTemplate(nullptr, &Compiler::Compile_mfc2, nullptr, TF_GTE_STALL); } break;
            case CopCommonInstruction::cfcn: if (inst->r.rt != Reg::zero) { CompileTemplate(nullptr, &Compiler::Compile_mfc2, nullptr, TF_GTE_STALL); } break;
            case CopCommonInstruction::mtcn: CompileTemplate(nullptr, &Compiler::Compile_mtc2, PGXPFN(CPU_MTC2), TF_GTE_STALL | TF_READS_T | TF_PGXP_WITHOUT_CPU); break;
            case CopCommonInstruction::ctcn: CompileTemplate(nullptr, &Compiler::Compile_mtc2, PGXPFN(CPU_MTC2), TF_GTE_STALL | TF_READS_T | TF_PGXP_WITHOUT_CPU); break;
            default: Compile_Fallback(); break;
          }
        }
        else
        {
          // GTE ops
          CompileTemplate(nullptr, &Compiler::Compile_cop2, nullptr, TF_GTE_STALL);
        }
      }
      break;

    case InstructionOp::lwc2: CompileLoadStoreTemplate(&Compiler::Compile_lwc2, MemoryAccessSize::Word, false, false, TF_GTE_STALL | TF_READS_S | TF_LOAD_DELAY); break;
    case InstructionOp::swc2: CompileLoadStoreTemplate(&Compiler::Compile_swc2, MemoryAccessSize::Word, true, false, TF_GTE_STALL | TF_READS_S); SpecExec_swc2(); break;

      // swc0/lwc0/cop1/cop3 are essentially no-ops
    case InstructionOp::cop1:
    case InstructionOp::cop3:
    case InstructionOp::lwc0:
    case InstructionOp::lwc1:
    case InstructionOp::lwc3:
    case InstructionOp::swc0:
    case InstructionOp::swc1:
    case InstructionOp::swc3:
      break;

    default: Compile_Fallback(); InvalidateSpeculativeValues(); TruncateBlock(); break;
      // clang-format on

#undef PGXPFN
  }

  ClearHostRegsNeeded();
  UpdateLoadDelay();

#if 0
  const void* end = GetCurrentCodePointer();
  if (start != end && !m_current_instruction_branch_delay_slot)
  {
    CodeCache::DisassembleAndLogHostCode(start,
                                         static_cast<u32>(static_cast<const u8*>(end) - static_cast<const u8*>(start)));
  }
#endif
}

void CPU::NewRec::Compiler::CompileBranchDelaySlot(bool dirty_pc /* = true */)
{
  // Update load delay at the end of the previous instruction.
  UpdateLoadDelay();

  // TODO: Move cycle add before this.
  inst++;
  iinfo++;
  m_current_instruction_pc += sizeof(Instruction);
  m_current_instruction_branch_delay_slot = true;
  m_compiler_pc += sizeof(Instruction);
  m_dirty_pc = dirty_pc;
  m_dirty_instruction_bits = true;

  CompileInstruction();

  m_current_instruction_branch_delay_slot = false;
}

void CPU::NewRec::Compiler::CompileTemplate(void (Compiler::*const_func)(CompileFlags),
                                            void (Compiler::*func)(CompileFlags), const void* pgxp_cpu_func, u32 tflags)
{
  // TODO: This is where we will do memory operand optimization. Remember to kill constants!
  // TODO: Swap S and T if commutative
  // TODO: For and, treat as zeroing if imm is zero
  // TODO: Optimize slt + bne to cmp + jump
  // TODO: Prefer memory operands when load delay is dirty, since we're going to invalidate immediately after the first
  // instruction..
  // TODO: andi with zero -> zero const
  // TODO: load constant so it can be flushed if it's not overwritten later
  // TODO: inline PGXP ops.
  // TODO: don't rename on sltu.

  bool allow_constant = static_cast<bool>(const_func);
  Reg rs = inst->r.rs.GetValue();
  Reg rt = inst->r.rt.GetValue();
  Reg rd = inst->r.rd.GetValue();

  if (tflags & TF_GTE_STALL)
    StallUntilGTEComplete();

  // throw away instructions writing to $zero
  if (!(tflags & TF_NO_NOP) && (!g_settings.cpu_recompiler_memory_exceptions || !(tflags & TF_CAN_OVERFLOW)) &&
      ((tflags & TF_WRITES_T && rt == Reg::zero) || (tflags & TF_WRITES_D && rd == Reg::zero)))
  {
    Log_DebugPrintf("Skipping instruction because it writes to zero");
    return;
  }

  // handle rename operations
  if ((tflags & TF_RENAME_WITH_ZERO_T && HasConstantRegValue(rt, 0)))
  {
    DebugAssert((tflags & (TF_WRITES_D | TF_READS_S | TF_READS_T)) == (TF_WRITES_D | TF_READS_S | TF_READS_T));
    CompileMoveRegTemplate(rd, rs, true);
    return;
  }
  else if ((tflags & (TF_RENAME_WITH_ZERO_T | TF_COMMUTATIVE)) == (TF_RENAME_WITH_ZERO_T | TF_COMMUTATIVE) &&
           HasConstantRegValue(rs, 0))
  {
    DebugAssert((tflags & (TF_WRITES_D | TF_READS_S | TF_READS_T)) == (TF_WRITES_D | TF_READS_S | TF_READS_T));
    CompileMoveRegTemplate(rd, rt, true);
    return;
  }
  else if (tflags & TF_RENAME_WITH_ZERO_IMM && inst->i.imm == 0)
  {
    CompileMoveRegTemplate(rt, rs, true);
    return;
  }

  if (pgxp_cpu_func && g_settings.gpu_pgxp_enable && ((tflags & TF_PGXP_WITHOUT_CPU) || g_settings.UsingPGXPCPUMode()))
  {
    std::array<Reg, 2> reg_args = {{Reg::count, Reg::count}};
    u32 num_reg_args = 0;
    if (tflags & TF_READS_S)
      reg_args[num_reg_args++] = rs;
    if (tflags & TF_READS_T)
      reg_args[num_reg_args++] = rt;
    if (tflags & TF_READS_LO)
      reg_args[num_reg_args++] = Reg::lo;
    if (tflags & TF_READS_HI)
      reg_args[num_reg_args++] = Reg::hi;

    DebugAssert(num_reg_args <= 2);
    GeneratePGXPCallWithMIPSRegs(pgxp_cpu_func, inst->bits, reg_args[0], reg_args[1]);
  }

  // if it's a commutative op, and we have one constant reg but not the other, swap them
  // TODO: make it swap when writing to T as well
  // TODO: drop the hack for rd == rt
  if (tflags & TF_COMMUTATIVE && !(tflags & TF_WRITES_T) &&
      ((HasConstantReg(rs) && !HasConstantReg(rt)) || (tflags & TF_WRITES_D && rd == rt)))
  {
    Log_DebugPrintf("Swapping S:%s and T:%s due to commutative op and constants", GetRegName(rs), GetRegName(rt));
    std::swap(rs, rt);
  }

  CompileFlags cf = {};

  if (tflags & TF_READS_S)
  {
    MarkRegsNeeded(HR_TYPE_CPU_REG, rs);
    if (HasConstantReg(rs))
      cf.const_s = true;
    else
      allow_constant = false;
  }
  if (tflags & TF_READS_T)
  {
    MarkRegsNeeded(HR_TYPE_CPU_REG, rt);
    if (HasConstantReg(rt))
      cf.const_t = true;
    else
      allow_constant = false;
  }
  if (tflags & TF_READS_LO)
  {
    MarkRegsNeeded(HR_TYPE_CPU_REG, Reg::lo);
    if (HasConstantReg(Reg::lo))
      cf.const_lo = true;
    else
      allow_constant = false;
  }
  if (tflags & TF_READS_HI)
  {
    MarkRegsNeeded(HR_TYPE_CPU_REG, Reg::hi);
    if (HasConstantReg(Reg::hi))
      cf.const_hi = true;
    else
      allow_constant = false;
  }

  // Needed because of potential swapping
  if (tflags & TF_READS_S)
    cf.mips_s = static_cast<u8>(rs);
  if (tflags & (TF_READS_T | TF_WRITES_T))
    cf.mips_t = static_cast<u8>(rt);

  if (allow_constant)
  {
    // woot, constant path
    (this->*const_func)(cf);
    return;
  }

  UpdateHostRegCounters();

  if (tflags & TF_CAN_SWAP_DELAY_SLOT && TrySwapDelaySlot(cf.MipsS(), cf.MipsT()))
    cf.delay_slot_swapped = true;

  if (tflags & TF_READS_S &&
      (tflags & TF_NEEDS_REG_S || !cf.const_s || (tflags & TF_WRITES_D && rd != Reg::zero && rd == rs)))
  {
    cf.host_s = AllocateHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rs);
    cf.const_s = false;
    cf.valid_host_s = true;
  }

  if (tflags & TF_READS_T &&
      (tflags & (TF_NEEDS_REG_T | TF_WRITES_T) || !cf.const_t || (tflags & TF_WRITES_D && rd != Reg::zero && rd == rt)))
  {
    cf.host_t = AllocateHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt);
    cf.const_t = false;
    cf.valid_host_t = true;
  }

  if (tflags & (TF_READS_LO | TF_WRITES_LO))
  {
    cf.host_lo =
      AllocateHostReg(((tflags & TF_READS_LO) ? HR_MODE_READ : 0u) | ((tflags & TF_WRITES_LO) ? HR_MODE_WRITE : 0u),
                      HR_TYPE_CPU_REG, Reg::lo);
    cf.const_lo = false;
    cf.valid_host_lo = true;
  }

  if (tflags & (TF_READS_HI | TF_WRITES_HI))
  {
    cf.host_hi =
      AllocateHostReg(((tflags & TF_READS_HI) ? HR_MODE_READ : 0u) | ((tflags & TF_WRITES_HI) ? HR_MODE_WRITE : 0u),
                      HR_TYPE_CPU_REG, Reg::hi);
    cf.const_hi = false;
    cf.valid_host_hi = true;
  }

  const HostRegAllocType write_type =
    (tflags & TF_LOAD_DELAY && EMULATE_LOAD_DELAYS) ? HR_TYPE_NEXT_LOAD_DELAY_VALUE : HR_TYPE_CPU_REG;

  if (tflags & TF_CAN_OVERFLOW && g_settings.cpu_recompiler_memory_exceptions)
  {
    // allocate a temp register for the result, then swap it back
    const u32 tempreg = AllocateHostReg(0, HR_TYPE_TEMP);
    ;
    if (tflags & TF_WRITES_D)
    {
      cf.host_d = tempreg;
      cf.valid_host_d = true;
    }
    else if (tflags & TF_WRITES_T)
    {
      cf.host_t = tempreg;
      cf.valid_host_t = true;
    }

    (this->*func)(cf);

    if (tflags & TF_WRITES_D && rd != Reg::zero)
    {
      DeleteMIPSReg(rd, false);
      RenameHostReg(tempreg, HR_MODE_WRITE, write_type, rd);
    }
    else if (tflags & TF_WRITES_T && rt != Reg::zero)
    {
      DeleteMIPSReg(rt, false);
      RenameHostReg(tempreg, HR_MODE_WRITE, write_type, rt);
    }
    else
    {
      FreeHostReg(tempreg);
    }
  }
  else
  {
    if (tflags & TF_WRITES_D && rd != Reg::zero)
    {
      if (tflags & TF_READS_S && cf.valid_host_s && TryRenameMIPSReg(rd, rs, cf.host_s, Reg::count))
        cf.host_d = cf.host_s;
      else
        cf.host_d = AllocateHostReg(HR_MODE_WRITE, write_type, rd);
      cf.valid_host_d = true;
    }

    if (tflags & TF_WRITES_T && rt != Reg::zero)
    {
      if (tflags & TF_READS_S && cf.valid_host_s && TryRenameMIPSReg(rt, rs, cf.host_s, Reg::count))
        cf.host_t = cf.host_s;
      else
        cf.host_t = AllocateHostReg(HR_MODE_WRITE, write_type, rt);
      cf.valid_host_t = true;
    }

    (this->*func)(cf);
  }
}

void CPU::NewRec::Compiler::CompileLoadStoreTemplate(void (Compiler::*func)(CompileFlags, MemoryAccessSize, bool, bool,
                                                                            const std::optional<VirtualMemoryAddress>&),
                                                     MemoryAccessSize size, bool store, bool sign, u32 tflags)
{
  const Reg rs = inst->i.rs;
  const Reg rt = inst->i.rt;

  if (tflags & TF_GTE_STALL)
    StallUntilGTEComplete();

  CompileFlags cf = {};

  if (tflags & TF_READS_S)
  {
    MarkRegsNeeded(HR_TYPE_CPU_REG, rs);
    cf.mips_s = static_cast<u8>(rs);
  }
  if (tflags & (TF_READS_T | TF_WRITES_T))
  {
    if (tflags & TF_READS_T)
      MarkRegsNeeded(HR_TYPE_CPU_REG, rt);
    cf.mips_t = static_cast<u8>(rt);
  }

  UpdateHostRegCounters();

  // constant address?
  std::optional<VirtualMemoryAddress> addr;
  std::optional<VirtualMemoryAddress> spec_addr;
  bool use_fastmem = CodeCache::IsUsingFastmem() && !g_settings.cpu_recompiler_memory_exceptions &&
                     !SpecIsCacheIsolated() && !CodeCache::HasPreviouslyFaultedOnPC(m_current_instruction_pc);
  if (HasConstantReg(rs))
  {
    addr = GetConstantRegU32(rs) + inst->i.imm_sext32();
    spec_addr = addr;
    cf.const_s = true;

    if (!Bus::CanUseFastmemForAddress(addr.value()))
    {
      Log_DebugFmt("Not using fastmem for {:08X}", addr.value());
      use_fastmem = false;
    }
  }
  else
  {
    spec_addr = SpecExec_LoadStoreAddr();
    if (use_fastmem && spec_addr.has_value() && !Bus::CanUseFastmemForAddress(spec_addr.value()))
    {
      Log_DebugFmt("Not using fastmem for speculative {:08X}", spec_addr.value());
      use_fastmem = false;
    }

    if constexpr (HAS_MEMORY_OPERANDS)
    {
      // don't bother caching it since we're going to flush anyway
      // TODO: make less rubbish, if it's caller saved we don't need to flush...
      const std::optional<u32> hreg = CheckHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rs);
      if (hreg.has_value())
      {
        cf.valid_host_s = true;
        cf.host_s = hreg.value();
      }
    }
    else
    {
      // need rs in a register
      cf.host_s = AllocateHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rs);
      cf.valid_host_s = true;
    }
  }

  // reads T -> store, writes T -> load
  // for now, we defer the allocation to afterwards, because C call
  if (tflags & TF_READS_T)
  {
    if (HasConstantReg(rt))
    {
      cf.const_t = true;
    }
    else
    {
      if constexpr (HAS_MEMORY_OPERANDS)
      {
        const std::optional<u32> hreg = CheckHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt);
        if (hreg.has_value())
        {
          cf.valid_host_t = true;
          cf.host_t = hreg.value();
        }
      }
      else
      {
        cf.host_t = AllocateHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, rt);
        cf.valid_host_t = true;
      }
    }
  }

  (this->*func)(cf, size, sign, use_fastmem, addr);

  if (store && !m_block_ended && !m_current_instruction_branch_delay_slot && spec_addr.has_value() &&
      GetSegmentForAddress(spec_addr.value()) != Segment::KSEG2)
  {
    // Get rid of physical aliases.
    const u32 phys_spec_addr = VirtualAddressToPhysical(spec_addr.value());
    if (phys_spec_addr >= VirtualAddressToPhysical(m_block->pc) &&
        phys_spec_addr < VirtualAddressToPhysical(m_block->pc + (m_block->size * sizeof(Instruction))))
    {
      Log_WarningFmt("Instruction {:08X} speculatively writes to {:08X} inside block {:08X}-{:08X}. Truncating block.",
                     m_current_instruction_pc, phys_spec_addr, m_block->pc,
                     m_block->pc + (m_block->size * sizeof(Instruction)));
      TruncateBlock();
    }
  }
}

void CPU::NewRec::Compiler::TruncateBlock()
{
  m_block->size = ((m_current_instruction_pc - m_block->pc) / sizeof(Instruction)) + 1;
  iinfo->is_last_instruction = true;
}

void CPU::NewRec::Compiler::FlushForLoadStore(const std::optional<VirtualMemoryAddress>& address, bool store,
                                              bool use_fastmem)
{
  if (use_fastmem)
    return;

  // TODO: Stores don't need to flush GTE cycles...
  Flush(FLUSH_FOR_C_CALL | FLUSH_FOR_LOADSTORE);
}

void CPU::NewRec::Compiler::CompileMoveRegTemplate(Reg dst, Reg src, bool pgxp_move)
{
  if (dst == src || dst == Reg::zero)
    return;

  if (HasConstantReg(src))
  {
    DeleteMIPSReg(dst, false);
    SetConstantReg(dst, GetConstantRegU32(src));
  }
  else
  {
    const u32 srcreg = AllocateHostReg(HR_MODE_READ, HR_TYPE_CPU_REG, src);
    if (!TryRenameMIPSReg(dst, src, srcreg, Reg::count))
    {
      const u32 dstreg = AllocateHostReg(HR_MODE_WRITE, HR_TYPE_CPU_REG, dst);
      CopyHostReg(dstreg, srcreg);
      ClearHostRegNeeded(dstreg);
    }
  }

  // TODO: This could be made better if we only did it for registers where there was a previous MFC2.
  if (g_settings.gpu_pgxp_enable && pgxp_move)
  {
    // might've been renamed, so use dst here
    GeneratePGXPCallWithMIPSRegs(reinterpret_cast<const void*>(&PGXP::CPU_MOVE_Packed), PGXP::PackMoveArgs(dst, src),
                                 dst);
  }
}

void CPU::NewRec::Compiler::Compile_j()
{
  const u32 newpc = (m_compiler_pc & UINT32_C(0xF0000000)) | (inst->j.target << 2);

  // TODO: Delay slot swap.
  // We could also move the cycle commit back.
  CompileBranchDelaySlot();
  EndBlock(newpc, true);
}

void CPU::NewRec::Compiler::Compile_jr_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  const u32 newpc = GetConstantRegU32(cf.MipsS());
  if (newpc & 3 && g_settings.cpu_recompiler_memory_exceptions)
  {
    EndBlockWithException(Exception::AdEL);
    return;
  }

  CompileBranchDelaySlot();
  EndBlock(newpc, true);
}

void CPU::NewRec::Compiler::Compile_jal()
{
  const u32 newpc = (m_compiler_pc & UINT32_C(0xF0000000)) | (inst->j.target << 2);
  SetConstantReg(Reg::ra, GetBranchReturnAddress({}));
  CompileBranchDelaySlot();
  EndBlock(newpc, true);
}

void CPU::NewRec::Compiler::Compile_jalr_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  const u32 newpc = GetConstantRegU32(cf.MipsS());
  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetBranchReturnAddress({}));

  CompileBranchDelaySlot();
  EndBlock(newpc, true);
}

void CPU::NewRec::Compiler::Compile_syscall()
{
  EndBlockWithException(Exception::Syscall);
}

void CPU::NewRec::Compiler::Compile_break()
{
  EndBlockWithException(Exception::BP);
}

void CPU::NewRec::Compiler::Compile_b_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));

  const u8 irt = static_cast<u8>(inst->i.rt.GetValue());
  const bool bgez = ConvertToBoolUnchecked(irt & u8(1));
  const bool link = (irt & u8(0x1E)) == u8(0x10);

  const s32 rs = GetConstantRegS32(cf.MipsS());
  const bool taken = bgez ? (rs >= 0) : (rs < 0);
  const u32 taken_pc = GetConditionalBranchTarget(cf);

  if (link)
    SetConstantReg(Reg::ra, GetBranchReturnAddress(cf));

  CompileBranchDelaySlot();
  EndBlock(taken ? taken_pc : m_compiler_pc, true);
}

void CPU::NewRec::Compiler::Compile_b(CompileFlags cf)
{
  const u8 irt = static_cast<u8>(inst->i.rt.GetValue());
  const bool bgez = ConvertToBoolUnchecked(irt & u8(1));
  const bool link = (irt & u8(0x1E)) == u8(0x10);

  if (link)
    SetConstantReg(Reg::ra, GetBranchReturnAddress(cf));

  Compile_bxx(cf, bgez ? BranchCondition::GreaterEqualZero : BranchCondition::LessThanZero);
}

void CPU::NewRec::Compiler::Compile_blez(CompileFlags cf)
{
  Compile_bxx(cf, BranchCondition::LessEqualZero);
}

void CPU::NewRec::Compiler::Compile_blez_const(CompileFlags cf)
{
  Compile_bxx_const(cf, BranchCondition::LessEqualZero);
}

void CPU::NewRec::Compiler::Compile_bgtz(CompileFlags cf)
{
  Compile_bxx(cf, BranchCondition::GreaterThanZero);
}

void CPU::NewRec::Compiler::Compile_bgtz_const(CompileFlags cf)
{
  Compile_bxx_const(cf, BranchCondition::GreaterThanZero);
}

void CPU::NewRec::Compiler::Compile_beq(CompileFlags cf)
{
  Compile_bxx(cf, BranchCondition::Equal);
}

void CPU::NewRec::Compiler::Compile_beq_const(CompileFlags cf)
{
  Compile_bxx_const(cf, BranchCondition::Equal);
}

void CPU::NewRec::Compiler::Compile_bne(CompileFlags cf)
{
  Compile_bxx(cf, BranchCondition::NotEqual);
}

void CPU::NewRec::Compiler::Compile_bne_const(CompileFlags cf)
{
  Compile_bxx_const(cf, BranchCondition::NotEqual);
}

void CPU::NewRec::Compiler::Compile_bxx_const(CompileFlags cf, BranchCondition cond)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));

  bool taken;
  switch (cond)
  {
    case BranchCondition::Equal:
      taken = GetConstantRegU32(cf.MipsS()) == GetConstantRegU32(cf.MipsT());
      break;

    case BranchCondition::NotEqual:
      taken = GetConstantRegU32(cf.MipsS()) != GetConstantRegU32(cf.MipsT());
      break;

    case BranchCondition::GreaterThanZero:
      taken = GetConstantRegS32(cf.MipsS()) > 0;
      break;

    case BranchCondition::GreaterEqualZero:
      taken = GetConstantRegS32(cf.MipsS()) >= 0;
      break;

    case BranchCondition::LessThanZero:
      taken = GetConstantRegS32(cf.MipsS()) < 0;
      break;

    case BranchCondition::LessEqualZero:
      taken = GetConstantRegS32(cf.MipsS()) <= 0;
      break;

    default:
      Panic("Unhandled condition");
      return;
  }

  const u32 taken_pc = GetConditionalBranchTarget(cf);
  CompileBranchDelaySlot();
  EndBlock(taken ? taken_pc : m_compiler_pc, true);
}

void CPU::NewRec::Compiler::Compile_sll_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsT()) << inst->r.shamt);
}

void CPU::NewRec::Compiler::Compile_srl_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsT()) >> inst->r.shamt);
}

void CPU::NewRec::Compiler::Compile_sra_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), static_cast<u32>(GetConstantRegS32(cf.MipsT()) >> inst->r.shamt));
}

void CPU::NewRec::Compiler::Compile_sllv_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsT()) << (GetConstantRegU32(cf.MipsS()) & 0x1Fu));
}

void CPU::NewRec::Compiler::Compile_srlv_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsT()) >> (GetConstantRegU32(cf.MipsS()) & 0x1Fu));
}

void CPU::NewRec::Compiler::Compile_srav_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), static_cast<u32>(GetConstantRegS32(cf.MipsT()) >> (GetConstantRegU32(cf.MipsS()) & 0x1Fu)));
}

void CPU::NewRec::Compiler::Compile_and_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsS()) & GetConstantRegU32(cf.MipsT()));
}

void CPU::NewRec::Compiler::Compile_or_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsS()) | GetConstantRegU32(cf.MipsT()));
}

void CPU::NewRec::Compiler::Compile_xor_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsS()) ^ GetConstantRegU32(cf.MipsT()));
}

void CPU::NewRec::Compiler::Compile_nor_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), ~(GetConstantRegU32(cf.MipsS()) | GetConstantRegU32(cf.MipsT())));
}

void CPU::NewRec::Compiler::Compile_slt_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), BoolToUInt32(GetConstantRegS32(cf.MipsS()) < GetConstantRegS32(cf.MipsT())));
}

void CPU::NewRec::Compiler::Compile_sltu_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), BoolToUInt32(GetConstantRegU32(cf.MipsS()) < GetConstantRegU32(cf.MipsT())));
}

void CPU::NewRec::Compiler::Compile_mult_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));

  const u64 res =
    static_cast<u64>(static_cast<s64>(GetConstantRegS32(cf.MipsS())) * static_cast<s64>(GetConstantRegS32(cf.MipsT())));
  SetConstantReg(Reg::hi, static_cast<u32>(res >> 32));
  SetConstantReg(Reg::lo, static_cast<u32>(res));
}

void CPU::NewRec::Compiler::Compile_multu_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));

  const u64 res = static_cast<u64>(GetConstantRegU32(cf.MipsS())) * static_cast<u64>(GetConstantRegU32(cf.MipsT()));
  SetConstantReg(Reg::hi, static_cast<u32>(res >> 32));
  SetConstantReg(Reg::lo, static_cast<u32>(res));
}

void CPU::NewRec::Compiler::MIPSSignedDivide(s32 num, s32 denom, u32* lo, u32* hi)
{
  if (denom == 0)
  {
    // divide by zero
    *lo = (num >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
    *hi = static_cast<u32>(num);
  }
  else if (static_cast<u32>(num) == UINT32_C(0x80000000) && denom == -1)
  {
    // unrepresentable
    *lo = UINT32_C(0x80000000);
    *hi = 0;
  }
  else
  {
    *lo = static_cast<u32>(num / denom);
    *hi = static_cast<u32>(num % denom);
  }
}

void CPU::NewRec::Compiler::MIPSUnsignedDivide(u32 num, u32 denom, u32* lo, u32* hi)
{
  if (denom == 0)
  {
    // divide by zero
    *lo = UINT32_C(0xFFFFFFFF);
    *hi = static_cast<u32>(num);
  }
  else
  {
    *lo = num / denom;
    *hi = num % denom;
  }
}

void CPU::NewRec::Compiler::Compile_div_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));

  const s32 num = GetConstantRegS32(cf.MipsS());
  const s32 denom = GetConstantRegS32(cf.MipsT());

  u32 lo, hi;
  MIPSSignedDivide(num, denom, &lo, &hi);

  SetConstantReg(Reg::hi, hi);
  SetConstantReg(Reg::lo, lo);
}

void CPU::NewRec::Compiler::Compile_divu_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));

  const u32 num = GetConstantRegU32(cf.MipsS());
  const u32 denom = GetConstantRegU32(cf.MipsT());

  u32 lo, hi;
  MIPSUnsignedDivide(num, denom, &lo, &hi);

  SetConstantReg(Reg::hi, hi);
  SetConstantReg(Reg::lo, lo);
}

void CPU::NewRec::Compiler::Compile_add_const(CompileFlags cf)
{
  // TODO: Overflow
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsS()) + GetConstantRegU32(cf.MipsT()));
}

void CPU::NewRec::Compiler::Compile_addu_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsS()) + GetConstantRegU32(cf.MipsT()));
}

void CPU::NewRec::Compiler::Compile_sub_const(CompileFlags cf)
{
  // TODO: Overflow
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  if (MipsD() != Reg::zero)
    SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsS()) - GetConstantRegU32(cf.MipsT()));
}

void CPU::NewRec::Compiler::Compile_subu_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()) && HasConstantReg(cf.MipsT()));
  SetConstantReg(MipsD(), GetConstantRegU32(cf.MipsS()) - GetConstantRegU32(cf.MipsT()));
}

void CPU::NewRec::Compiler::Compile_addi_const(CompileFlags cf)
{
  // TODO: Overflow
  DebugAssert(HasConstantReg(cf.MipsS()));
  if (cf.MipsT() != Reg::zero)
    SetConstantReg(cf.MipsT(), GetConstantRegU32(cf.MipsS()) + inst->i.imm_sext32());
}

void CPU::NewRec::Compiler::Compile_addiu_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  SetConstantReg(cf.MipsT(), GetConstantRegU32(cf.MipsS()) + inst->i.imm_sext32());
}

void CPU::NewRec::Compiler::Compile_slti_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  SetConstantReg(cf.MipsT(), BoolToUInt32(GetConstantRegS32(cf.MipsS()) < static_cast<s32>(inst->i.imm_sext32())));
}

void CPU::NewRec::Compiler::Compile_sltiu_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  SetConstantReg(cf.MipsT(), GetConstantRegU32(cf.MipsS()) < inst->i.imm_sext32());
}

void CPU::NewRec::Compiler::Compile_andi_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  SetConstantReg(cf.MipsT(), GetConstantRegU32(cf.MipsS()) & inst->i.imm_zext32());
}

void CPU::NewRec::Compiler::Compile_ori_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  SetConstantReg(cf.MipsT(), GetConstantRegU32(cf.MipsS()) | inst->i.imm_zext32());
}

void CPU::NewRec::Compiler::Compile_xori_const(CompileFlags cf)
{
  DebugAssert(HasConstantReg(cf.MipsS()));
  SetConstantReg(cf.MipsT(), GetConstantRegU32(cf.MipsS()) ^ inst->i.imm_zext32());
}

void CPU::NewRec::Compiler::Compile_lui()
{
  if (inst->i.rt == Reg::zero)
    return;

  SetConstantReg(inst->i.rt, inst->i.imm_zext32() << 16);

  if (g_settings.UsingPGXPCPUMode())
    GeneratePGXPCallWithMIPSRegs(reinterpret_cast<const void*>(&PGXP::CPU_LUI), inst->bits);
}

static constexpr const std::array<std::pair<u32*, u32>, 16> s_cop0_table = {
  {{nullptr, 0x00000000u},
   {nullptr, 0x00000000u},
   {nullptr, 0x00000000u},
   {&CPU::g_state.cop0_regs.BPC, 0xffffffffu},
   {nullptr, 0},
   {&CPU::g_state.cop0_regs.BDA, 0xffffffffu},
   {&CPU::g_state.cop0_regs.TAR, 0x00000000u},
   {&CPU::g_state.cop0_regs.dcic.bits, CPU::Cop0Registers::DCIC::WRITE_MASK},
   {&CPU::g_state.cop0_regs.BadVaddr, 0x00000000u},
   {&CPU::g_state.cop0_regs.BDAM, 0xffffffffu},
   {nullptr, 0x00000000u},
   {&CPU::g_state.cop0_regs.BPCM, 0xffffffffu},
   {&CPU::g_state.cop0_regs.sr.bits, CPU::Cop0Registers::SR::WRITE_MASK},
   {&CPU::g_state.cop0_regs.cause.bits, CPU::Cop0Registers::CAUSE::WRITE_MASK},
   {&CPU::g_state.cop0_regs.EPC, 0x00000000u},
   {&CPU::g_state.cop0_regs.PRID, 0x00000000u}}};

u32* CPU::NewRec::Compiler::GetCop0RegPtr(Cop0Reg reg)
{
  return (static_cast<u8>(reg) < s_cop0_table.size()) ? s_cop0_table[static_cast<u8>(reg)].first : nullptr;
}

u32 CPU::NewRec::Compiler::GetCop0RegWriteMask(Cop0Reg reg)
{
  return (static_cast<u8>(reg) < s_cop0_table.size()) ? s_cop0_table[static_cast<u8>(reg)].second : 0;
}

void CPU::NewRec::Compiler::Compile_mfc0(CompileFlags cf)
{
  const Cop0Reg r = static_cast<Cop0Reg>(MipsD());
  const u32* ptr = GetCop0RegPtr(r);
  if (!ptr)
  {
    Log_ErrorPrintf("Read from unknown cop0 reg %u", static_cast<u32>(r));
    Compile_Fallback();
    return;
  }

  DebugAssert(cf.valid_host_t);
  LoadHostRegFromCPUPointer(cf.host_t, ptr);
}

std::pair<u32*, CPU::NewRec::Compiler::GTERegisterAccessAction>
CPU::NewRec::Compiler::GetGTERegisterPointer(u32 index, bool writing)
{
  if (!writing)
  {
    // Most GTE registers can be read directly. Handle the special cases here.
    if (index == 15) // SXY3
    {
      // mirror of SXY2
      index = 14;
    }

    switch (index)
    {
      case 28: // IRGB
      case 29: // ORGB
      {
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::CallHandler);
      }
      break;

      default:
      {
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::Direct);
      }
      break;
    }
  }
  else
  {
    switch (index)
    {
      case 1:  // V0[z]
      case 3:  // V1[z]
      case 5:  // V2[z]
      case 8:  // IR0
      case 9:  // IR1
      case 10: // IR2
      case 11: // IR3
      case 36: // RT33
      case 44: // L33
      case 52: // LR33
      case 58: // H       - sign-extended on read but zext on use
      case 59: // DQA
      case 61: // ZSF3
      case 62: // ZSF4
      {
        // sign-extend z component of vector registers
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::SignExtend16);
      }
      break;

      case 7:  // OTZ
      case 16: // SZ0
      case 17: // SZ1
      case 18: // SZ2
      case 19: // SZ3
      {
        // zero-extend unsigned values
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::ZeroExtend16);
      }
      break;

      case 15: // SXY3
      {
        // writing to SXYP pushes to the FIFO
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::PushFIFO);
      }
      break;

      case 28: // IRGB
      case 30: // LZCS
      case 63: // FLAG
      {
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::CallHandler);
      }

      case 29: // ORGB
      case 31: // LZCR
      {
        // read-only registers
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::Ignore);
      }

      default:
      {
        // written as-is, 2x16 or 1x32 bits
        return std::make_pair(&g_state.gte_regs.r32[index], GTERegisterAccessAction::Direct);
      }
    }
  }
}

void CPU::NewRec::Compiler::AddGTETicks(TickCount ticks)
{
  // TODO: check, int has +1 here
  m_gte_done_cycle = m_cycles + ticks;
  Log_DebugPrintf("Adding %d GTE ticks", ticks);
}

void CPU::NewRec::Compiler::StallUntilGTEComplete()
{
  // TODO: hack to match old rec.. this may or may not be correct behavior
  // it's the difference between stalling before and after the current instruction's cycle
  DebugAssert(m_cycles > 0);
  m_cycles--;

  if (!m_dirty_gte_done_cycle)
  {
    // simple case - in block scheduling
    if (m_gte_done_cycle > m_cycles)
    {
      Log_DebugPrintf("Stalling for %d ticks from GTE", m_gte_done_cycle - m_cycles);
      m_cycles += (m_gte_done_cycle - m_cycles);
    }
  }
  else
  {
    // switch to in block scheduling
    Log_DebugPrintf("Flushing GTE stall from state");
    Flush(FLUSH_GTE_STALL_FROM_STATE);
  }

  m_cycles++;
}

void CPU::NewRec::BackpatchLoadStore(void* exception_pc, const CodeCache::LoadstoreBackpatchInfo& info)
{
  // remove the cycles we added for the memory read, then take them off again after the backpatch
  // the normal rec path will add the ram read ticks later, so we need to take them off at the end
  DebugAssert(!info.is_load || info.cycles >= Bus::RAM_READ_TICKS);
  const TickCount cycles_to_add =
    static_cast<TickCount>(static_cast<u32>(info.cycles)) - (info.is_load ? Bus::RAM_READ_TICKS : 0);
  const TickCount cycles_to_remove = static_cast<TickCount>(static_cast<u32>(info.cycles));

  JitCodeBuffer& buffer = CodeCache::GetCodeBuffer();
  void* thunk_address = buffer.GetFreeFarCodePointer();
  const u32 thunk_size = CompileLoadStoreThunk(
    thunk_address, buffer.GetFreeFarCodeSpace(), exception_pc, info.code_size, cycles_to_add, cycles_to_remove,
    info.gpr_bitmask, info.address_register, info.data_register, info.AccessSize(), info.is_signed, info.is_load);

#if 0
  Log_DebugPrintf("**Backpatch Thunk**");
  CodeCache::DisassembleAndLogHostCode(thunk_address, thunk_size);
#endif

  // backpatch to a jump to the slowmem handler
  CodeCache::EmitJump(exception_pc, thunk_address, true);

  buffer.CommitFarCode(thunk_size);
}

void CPU::NewRec::Compiler::InitSpeculativeRegs()
{
  for (u8 i = 0; i < static_cast<u8>(Reg::count); i++)
    m_speculative_constants.regs[i] = g_state.regs.r[i];

  m_speculative_constants.cop0_sr = g_state.cop0_regs.sr.bits;
  m_speculative_constants.memory.clear();
}

void CPU::NewRec::Compiler::InvalidateSpeculativeValues()
{
  m_speculative_constants.regs.fill(std::nullopt);
  m_speculative_constants.memory.clear();
  m_speculative_constants.cop0_sr.reset();
}

CPU::NewRec::Compiler::SpecValue CPU::NewRec::Compiler::SpecReadReg(Reg reg)
{
  return m_speculative_constants.regs[static_cast<u8>(reg)];
}

void CPU::NewRec::Compiler::SpecWriteReg(Reg reg, SpecValue value)
{
  if (reg == Reg::zero)
    return;

  m_speculative_constants.regs[static_cast<u8>(reg)] = value;
}

void CPU::NewRec::Compiler::SpecInvalidateReg(Reg reg)
{
  if (reg == Reg::zero)
    return;

  m_speculative_constants.regs[static_cast<u8>(reg)].reset();
}

void CPU::NewRec::Compiler::SpecCopyReg(Reg dst, Reg src)
{
  if (dst == Reg::zero)
    return;

  m_speculative_constants.regs[static_cast<u8>(dst)] = m_speculative_constants.regs[static_cast<u8>(src)];
}

CPU::NewRec::Compiler::SpecValue CPU::NewRec::Compiler::SpecReadMem(VirtualMemoryAddress address)
{
  auto it = m_speculative_constants.memory.find(address);
  if (it != m_speculative_constants.memory.end())
    return it->second;

  u32 value;
  if ((address & SCRATCHPAD_ADDR_MASK) == SCRATCHPAD_ADDR)
  {
    u32 scratchpad_offset = address & SCRATCHPAD_OFFSET_MASK;
    std::memcpy(&value, &CPU::g_state.scratchpad[scratchpad_offset], sizeof(value));
    return value;
  }

  const PhysicalMemoryAddress phys_addr = address & PHYSICAL_MEMORY_ADDRESS_MASK;
  if (Bus::IsRAMAddress(phys_addr))
  {
    u32 ram_offset = phys_addr & Bus::g_ram_mask;
    std::memcpy(&value, &Bus::g_ram[ram_offset], sizeof(value));
    return value;
  }

  return std::nullopt;
}

void CPU::NewRec::Compiler::SpecWriteMem(u32 address, SpecValue value)
{
  auto it = m_speculative_constants.memory.find(address);
  if (it != m_speculative_constants.memory.end())
  {
    it->second = value;
    return;
  }

  const PhysicalMemoryAddress phys_addr = address & PHYSICAL_MEMORY_ADDRESS_MASK;
  if ((address & SCRATCHPAD_ADDR_MASK) == SCRATCHPAD_ADDR || Bus::IsRAMAddress(phys_addr))
    m_speculative_constants.memory.emplace(address, value);
}

void CPU::NewRec::Compiler::SpecInvalidateMem(VirtualMemoryAddress address)
{
  SpecWriteMem(address, std::nullopt);
}

bool CPU::NewRec::Compiler::SpecIsCacheIsolated()
{
  if (!m_speculative_constants.cop0_sr.has_value())
    return false;

  const Cop0Registers::SR sr{m_speculative_constants.cop0_sr.value()};
  return sr.Isc;
}

void CPU::NewRec::Compiler::SpecExec_b()
{
  const bool link = (static_cast<u8>(inst->i.rt.GetValue()) & u8(0x1E)) == u8(0x10);
  if (link)
    SpecWriteReg(Reg::ra, m_compiler_pc);
}

void CPU::NewRec::Compiler::SpecExec_jal()
{
  SpecWriteReg(Reg::ra, m_compiler_pc);
}

void CPU::NewRec::Compiler::SpecExec_jalr()
{
  SpecWriteReg(inst->r.rd, m_compiler_pc);
}

void CPU::NewRec::Compiler::SpecExec_sll()
{
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rt.has_value())
    SpecWriteReg(inst->r.rd, rt.value() << inst->r.shamt);
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_srl()
{
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rt.has_value())
    SpecWriteReg(inst->r.rd, rt.value() >> inst->r.shamt);
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_sra()
{
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rt.has_value())
    SpecWriteReg(inst->r.rd, static_cast<u32>(static_cast<s32>(rt.value()) >> inst->r.shamt));
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_sllv()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, rt.value() << (rs.value() & 0x1F));
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_srlv()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, rt.value() >> (rs.value() & 0x1F));
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_srav()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, static_cast<u32>(static_cast<s32>(rt.value()) >> (rs.value() & 0x1F)));
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_mult()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
  {
    const u64 result =
      static_cast<u64>(static_cast<s64>(SignExtend64(rs.value())) * static_cast<s64>(SignExtend64(rt.value())));
    SpecWriteReg(Reg::hi, Truncate32(result >> 32));
    SpecWriteReg(Reg::lo, Truncate32(result));
  }
  else
  {
    SpecInvalidateReg(Reg::hi);
    SpecInvalidateReg(Reg::lo);
  }
}

void CPU::NewRec::Compiler::SpecExec_multu()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
  {
    const u64 result = ZeroExtend64(rs.value()) * SignExtend64(rt.value());
    SpecWriteReg(Reg::hi, Truncate32(result >> 32));
    SpecWriteReg(Reg::lo, Truncate32(result));
  }
  else
  {
    SpecInvalidateReg(Reg::hi);
    SpecInvalidateReg(Reg::lo);
  }
}

void CPU::NewRec::Compiler::SpecExec_div()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
  {
    u32 lo, hi;
    MIPSSignedDivide(static_cast<s32>(rs.value()), static_cast<s32>(rt.value()), &lo, &hi);
    SpecWriteReg(Reg::hi, hi);
    SpecWriteReg(Reg::lo, lo);
  }
  else
  {
    SpecInvalidateReg(Reg::hi);
    SpecInvalidateReg(Reg::lo);
  }
}

void CPU::NewRec::Compiler::SpecExec_divu()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
  {
    u32 lo, hi;
    MIPSUnsignedDivide(rs.value(), rt.value(), &lo, &hi);
    SpecWriteReg(Reg::hi, hi);
    SpecWriteReg(Reg::lo, lo);
  }
  else
  {
    SpecInvalidateReg(Reg::hi);
    SpecInvalidateReg(Reg::lo);
  }
}

void CPU::NewRec::Compiler::SpecExec_add()
{
  SpecExec_addu();
}

void CPU::NewRec::Compiler::SpecExec_addu()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, rs.value() + rt.value());
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_sub()
{
  SpecExec_subu();
}

void CPU::NewRec::Compiler::SpecExec_subu()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, rs.value() - rt.value());
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_and()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, rs.value() & rt.value());
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_or()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, rs.value() | rt.value());
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_xor()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, rs.value() ^ rt.value());
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_nor()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, ~(rs.value() | rt.value()));
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_slt()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, BoolToUInt32(static_cast<s32>(rs.value()) < static_cast<s32>(rt.value())));
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_sltu()
{
  const SpecValue rs = SpecReadReg(inst->r.rs);
  const SpecValue rt = SpecReadReg(inst->r.rt);
  if (rs.has_value() && rt.has_value())
    SpecWriteReg(inst->r.rd, BoolToUInt32(rs.value() < rt.value()));
  else
    SpecInvalidateReg(inst->r.rd);
}

void CPU::NewRec::Compiler::SpecExec_addi()
{
  SpecExec_addiu();
}

void CPU::NewRec::Compiler::SpecExec_addiu()
{
  const SpecValue rs = SpecReadReg(inst->i.rs);
  if (rs.has_value())
    SpecWriteReg(inst->i.rt, rs.value() + inst->i.imm_sext32());
  else
    SpecInvalidateReg(inst->i.rt);
}

void CPU::NewRec::Compiler::SpecExec_slti()
{
  const SpecValue rs = SpecReadReg(inst->i.rs);
  if (rs.has_value())
    SpecWriteReg(inst->i.rt, BoolToUInt32(static_cast<s32>(rs.value()) < static_cast<s32>(inst->i.imm_sext32())));
  else
    SpecInvalidateReg(inst->i.rt);
}

void CPU::NewRec::Compiler::SpecExec_sltiu()
{
  const SpecValue rs = SpecReadReg(inst->i.rs);
  if (rs.has_value())
    SpecWriteReg(inst->i.rt, BoolToUInt32(rs.value() < inst->i.imm_sext32()));
  else
    SpecInvalidateReg(inst->i.rt);
}

void CPU::NewRec::Compiler::SpecExec_andi()
{
  const SpecValue rs = SpecReadReg(inst->i.rs);
  if (rs.has_value())
    SpecWriteReg(inst->i.rt, rs.value() & inst->i.imm_zext32());
  else
    SpecInvalidateReg(inst->i.rt);
}

void CPU::NewRec::Compiler::SpecExec_ori()
{
  const SpecValue rs = SpecReadReg(inst->i.rs);
  if (rs.has_value())
    SpecWriteReg(inst->i.rt, rs.value() | inst->i.imm_zext32());
  else
    SpecInvalidateReg(inst->i.rt);
}

void CPU::NewRec::Compiler::SpecExec_xori()
{
  const SpecValue rs = SpecReadReg(inst->i.rs);
  if (rs.has_value())
    SpecWriteReg(inst->i.rt, rs.value() ^ inst->i.imm_zext32());
  else
    SpecInvalidateReg(inst->i.rt);
}

void CPU::NewRec::Compiler::SpecExec_lui()
{
  SpecWriteReg(inst->i.rt, inst->i.imm_zext32() << 16);
}

CPU::NewRec::Compiler::SpecValue CPU::NewRec::Compiler::SpecExec_LoadStoreAddr()
{
  const SpecValue rs = SpecReadReg(inst->i.rs);
  return rs.has_value() ? (rs.value() + inst->i.imm_sext32()) : rs;
}

void CPU::NewRec::Compiler::SpecExec_lxx(MemoryAccessSize size, bool sign)
{
  const SpecValue addr = SpecExec_LoadStoreAddr();
  SpecValue val;
  if (!addr.has_value() || !(val = SpecReadMem(addr.value())).has_value())
  {
    SpecInvalidateReg(inst->i.rt);
    return;
  }

  switch (size)
  {
    case MemoryAccessSize::Byte:
      val = sign ? SignExtend32(static_cast<u8>(val.value())) : ZeroExtend32(static_cast<u8>(val.value()));
      break;

    case MemoryAccessSize::HalfWord:
      val = sign ? SignExtend32(static_cast<u16>(val.value())) : ZeroExtend32(static_cast<u16>(val.value()));
      break;

    case MemoryAccessSize::Word:
      break;

    default:
      UnreachableCode();
  }

  SpecWriteReg(inst->r.rt, val);
}

void CPU::NewRec::Compiler::SpecExec_lwx(bool lwr)
{
  // TODO
  SpecInvalidateReg(inst->i.rt);
}

void CPU::NewRec::Compiler::SpecExec_sxx(MemoryAccessSize size)
{
  const SpecValue addr = SpecExec_LoadStoreAddr();
  if (!addr.has_value())
    return;

  SpecValue rt = SpecReadReg(inst->i.rt);
  if (rt.has_value())
  {
    switch (size)
    {
      case MemoryAccessSize::Byte:
        rt = ZeroExtend32(static_cast<u8>(rt.value()));
        break;

      case MemoryAccessSize::HalfWord:
        rt = ZeroExtend32(static_cast<u16>(rt.value()));
        break;

      case MemoryAccessSize::Word:
        break;

      default:
        UnreachableCode();
    }
  }

  SpecWriteMem(addr.value(), rt);
}

void CPU::NewRec::Compiler::SpecExec_swx(bool swr)
{
  const SpecValue addr = SpecExec_LoadStoreAddr();
  if (addr.has_value())
    SpecInvalidateMem(addr.value() & ~3u);
}

void CPU::NewRec::Compiler::SpecExec_swc2()
{
  const SpecValue addr = SpecExec_LoadStoreAddr();
  if (addr.has_value())
    SpecInvalidateMem(addr.value());
}

void CPU::NewRec::Compiler::SpecExec_mfc0()
{
  const Cop0Reg rd = static_cast<Cop0Reg>(inst->r.rd.GetValue());
  if (rd != Cop0Reg::SR)
  {
    SpecInvalidateReg(inst->r.rt);
    return;
  }

  SpecWriteReg(inst->r.rt, m_speculative_constants.cop0_sr);
}

void CPU::NewRec::Compiler::SpecExec_mtc0()
{
  const Cop0Reg rd = static_cast<Cop0Reg>(inst->r.rd.GetValue());
  if (rd != Cop0Reg::SR || !m_speculative_constants.cop0_sr.has_value())
    return;

  SpecValue val = SpecReadReg(inst->r.rt);
  if (val.has_value())
  {
    constexpr u32 mask = Cop0Registers::SR::WRITE_MASK;
    val = (m_speculative_constants.cop0_sr.value() & mask) | (val.value() & mask);
  }

  m_speculative_constants.cop0_sr = val;
}

void CPU::NewRec::Compiler::SpecExec_rfe()
{
  if (!m_speculative_constants.cop0_sr.has_value())
    return;

  const u32 val = m_speculative_constants.cop0_sr.value();
  m_speculative_constants.cop0_sr = (val & UINT32_C(0b110000)) | ((val & UINT32_C(0b111111)) >> 2);
}
