// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_call_stack.h"

#include "cpu_core.h"
#include "cpu_core_private.h"
#include "cpu_types.h"

#include <algorithm>
#include <optional>
#include <vector>

static constexpr u32 CALL_STACK_MAX_FRAMES = 64;
static constexpr u32 CALL_STACK_MAX_SCAN_INSTRUCTIONS = 1024;

namespace CPU {
namespace {

struct CallStackInstruction
{
  VirtualMemoryAddress address;
  Instruction instruction;
};

struct CallStackFrameLayout
{
  u32 frame_size = 0;
  std::optional<s16> saved_ra_offset;
  bool frame_allocated = false;
  bool saved_ra_available = false;
  bool can_use_live_ra = false;
};

} // namespace

static bool IsStackPointerAdjustment(const Instruction instruction, s16* adjustment);
static bool IsReturnAddressStackAccess(const Instruction instruction, InstructionOp op, s16* offset);
static bool AddSignedOffset(VirtualMemoryAddress base, s32 offset, VirtualMemoryAddress* result);
static bool ScanCallStackInstructions(VirtualMemoryAddress pc, std::vector<CallStackInstruction>* instructions,
                                      size_t* current_instruction_index, bool* found_start, bool* found_end);
static bool AnalyzeCallStackFrame(VirtualMemoryAddress pc, CallStackFrameLayout* layout);
static bool UnwindCallStackFrame(VirtualMemoryAddress pc, VirtualMemoryAddress sp,
                                 std::optional<VirtualMemoryAddress> live_ra, VirtualMemoryAddress* caller_address,
                                 VirtualMemoryAddress* caller_sp);
} // namespace CPU

bool CPU::IsStackPointerAdjustment(const Instruction instruction, s16* adjustment)
{
  if ((instruction.op != InstructionOp::addi && instruction.op != InstructionOp::addiu) ||
      instruction.i.rs != Reg::sp || instruction.i.rt != Reg::sp)
  {
    return false;
  }

  *adjustment = instruction.i.imm_s16();
  return (*adjustment != 0);
}

bool CPU::IsReturnAddressStackAccess(const Instruction instruction, InstructionOp op, s16* offset)
{
  if (instruction.op != op || instruction.i.rs != Reg::sp || instruction.i.rt != Reg::ra)
    return false;

  *offset = instruction.i.imm_s16();
  return true;
}

bool CPU::AddSignedOffset(VirtualMemoryAddress base, s32 offset, VirtualMemoryAddress* result)
{
  const s64 address = static_cast<s64>(base) + static_cast<s64>(offset);
  if (address < 0 || address > static_cast<s64>(UINT32_MAX))
    return false;

  *result = static_cast<VirtualMemoryAddress>(address);
  return true;
}

bool CPU::ScanCallStackInstructions(VirtualMemoryAddress pc, std::vector<CallStackInstruction>* instructions,
                                    size_t* current_instruction_index, bool* found_start, bool* found_end)
{
  instructions->clear();
  *found_start = false;
  *found_end = false;

  std::vector<CallStackInstruction> before;
  before.reserve(CALL_STACK_MAX_SCAN_INSTRUCTIONS);
  bool in_return_delay_slot = false;
  for (u32 i = 1; i <= CALL_STACK_MAX_SCAN_INSTRUCTIONS; i++)
  {
    const u32 distance = i * INSTRUCTION_SIZE;
    if (pc < distance)
      break;

    const VirtualMemoryAddress address = pc - distance;
    Instruction instruction;
    if (!SafeReadInstruction(address, &instruction.bits))
      break;

    if (IsReturnInstruction(instruction))
    {
      if (i == 1)
      {
        // pc is the delay slot of this return, so the return still belongs to
        // the current function and the forward scan must stop after pc.
        before.push_back({address, instruction});
        in_return_delay_slot = true;
        *found_end = true;
        continue;
      }

      // The instruction immediately after the previous return is its delay slot,
      // not part of the function containing pc.
      if (!before.empty() && before.back().address == (address + INSTRUCTION_SIZE))
        before.pop_back();

      *found_start = true;
      break;
    }

    before.push_back({address, instruction});
  }

  instructions->reserve(before.size() + CALL_STACK_MAX_SCAN_INSTRUCTIONS + 1);
  for (auto it = before.rbegin(); it != before.rend(); ++it)
    instructions->push_back(*it);

  *current_instruction_index = instructions->size();

  for (u32 i = 0; i < CALL_STACK_MAX_SCAN_INSTRUCTIONS; i++)
  {
    const u64 address_64 = static_cast<u64>(pc) + (static_cast<u64>(i) * INSTRUCTION_SIZE);
    if (address_64 > UINT32_MAX)
      break;

    const VirtualMemoryAddress address = static_cast<VirtualMemoryAddress>(address_64);
    Instruction instruction;
    if (!SafeReadInstruction(address, &instruction.bits))
      break;

    instructions->push_back({address, instruction});
    if (in_return_delay_slot)
      break;

    if (IsReturnInstruction(instruction))
    {
      // Include the delay slot, since stack restoration is commonly placed there.
      const u64 delay_slot_address_64 = address_64 + INSTRUCTION_SIZE;
      if (delay_slot_address_64 <= UINT32_MAX)
      {
        Instruction delay_slot_instruction;
        if (SafeReadInstruction(static_cast<VirtualMemoryAddress>(delay_slot_address_64), &delay_slot_instruction.bits))
        {
          instructions->push_back({static_cast<VirtualMemoryAddress>(delay_slot_address_64), delay_slot_instruction});
        }
      }

      *found_end = true;
      break;
    }
  }

  return !instructions->empty();
}

bool CPU::AnalyzeCallStackFrame(VirtualMemoryAddress pc, CallStackFrameLayout* layout)
{
  std::vector<CallStackInstruction> instructions;
  size_t current_instruction_index;
  bool found_start;
  bool found_end;
  if (!ScanCallStackInstructions(pc, &instructions, &current_instruction_index, &found_start, &found_end))
    return false;

  std::optional<s16> allocation_adjustment;
  std::optional<s16> deallocation_adjustment;
  std::optional<s16> saved_ra_offset;
  std::optional<s16> restored_ra_offset;
  std::optional<s16> last_executed_sp_adjustment;
  bool saved_ra_executed = false;
  bool restored_ra_executed = false;
  bool has_call = false;

  for (size_t i = 0; i < instructions.size(); i++)
  {
    const Instruction instruction = instructions[i].instruction;
    const bool executed = (i < current_instruction_index);

    s16 adjustment;
    if (IsStackPointerAdjustment(instruction, &adjustment))
    {
      std::optional<s16>& expected_adjustment = (adjustment < 0) ? allocation_adjustment : deallocation_adjustment;
      if (expected_adjustment.has_value() && expected_adjustment.value() != adjustment)
        return false;

      expected_adjustment = adjustment;
      if (executed)
        last_executed_sp_adjustment = adjustment;
      continue;
    }

    s16 offset;
    if (IsReturnAddressStackAccess(instruction, InstructionOp::sw, &offset))
    {
      if (saved_ra_offset.has_value() && saved_ra_offset.value() != offset)
        return false;

      saved_ra_offset = offset;
      saved_ra_executed |= executed;
      continue;
    }

    if (IsReturnAddressStackAccess(instruction, InstructionOp::lw, &offset))
    {
      if (restored_ra_offset.has_value() && restored_ra_offset.value() != offset)
        return false;

      restored_ra_offset = offset;
      restored_ra_executed |= executed;
      continue;
    }

    has_call |= IsCallInstruction(instruction);
  }

  if (allocation_adjustment.has_value())
  {
    layout->frame_size = static_cast<u32>(-static_cast<s32>(allocation_adjustment.value()));
    if (deallocation_adjustment.has_value() &&
        static_cast<s32>(deallocation_adjustment.value()) != static_cast<s32>(layout->frame_size))
    {
      return false;
    }
  }
  else if (deallocation_adjustment.has_value())
  {
    layout->frame_size = static_cast<u32>(deallocation_adjustment.value());
  }

  if (saved_ra_offset.has_value() && restored_ra_offset.has_value() &&
      saved_ra_offset.value() != restored_ra_offset.value())
  {
    return false;
  }

  layout->saved_ra_offset = saved_ra_offset.has_value() ? saved_ra_offset : restored_ra_offset;
  layout->frame_allocated = last_executed_sp_adjustment.has_value() && last_executed_sp_adjustment.value() < 0;
  layout->saved_ra_available = layout->frame_allocated && saved_ra_executed;

  if (layout->saved_ra_available)
    return layout->saved_ra_offset.has_value();

  // The live return address is trustworthy only for the innermost frame when the
  // save has not happened yet, the restore has happened, or the whole function is
  // bounded and contains no calls or saved return address.
  const bool before_save = allocation_adjustment.has_value() && saved_ra_offset.has_value() && !saved_ra_executed;
  const bool after_restore = restored_ra_executed && !layout->frame_allocated;
  const bool proven_leaf = found_start && found_end && !saved_ra_offset.has_value() && !has_call;
  layout->can_use_live_ra = before_save || after_restore || proven_leaf;
  return layout->can_use_live_ra;
}

bool CPU::UnwindCallStackFrame(VirtualMemoryAddress pc, VirtualMemoryAddress sp,
                               std::optional<VirtualMemoryAddress> live_ra, VirtualMemoryAddress* caller_address,
                               VirtualMemoryAddress* caller_sp)
{
  CallStackFrameLayout layout;
  if (!AnalyzeCallStackFrame(pc, &layout))
    return false;

  VirtualMemoryAddress return_address;
  if (layout.frame_allocated)
  {
    if (layout.saved_ra_available)
    {
      VirtualMemoryAddress saved_ra_address;
      if (!AddSignedOffset(sp, layout.saved_ra_offset.value(), &saved_ra_address) ||
          !SafeReadMemoryWord(saved_ra_address, &return_address))
      {
        return false;
      }
    }
    else
    {
      if (!layout.can_use_live_ra || !live_ra.has_value())
        return false;

      return_address = live_ra.value();
    }

    const u64 caller_sp_64 = static_cast<u64>(sp) + layout.frame_size;
    if (caller_sp_64 > UINT32_MAX)
      return false;
    *caller_sp = static_cast<VirtualMemoryAddress>(caller_sp_64);
  }
  else
  {
    if (!layout.can_use_live_ra || !live_ra.has_value())
      return false;

    return_address = live_ra.value();
    *caller_sp = sp;
  }

  if (return_address < (2 * INSTRUCTION_SIZE) || (return_address & (INSTRUCTION_SIZE - 1)) != 0)
    return false;

  const VirtualMemoryAddress call_address = return_address - (2 * INSTRUCTION_SIZE);
  Instruction call_instruction;
  if (!SafeReadInstruction(call_address, &call_instruction.bits) || !IsCallInstruction(call_instruction))
    return false;

  *caller_address = call_address;
  // *caller_address = return_address; // Show the post-delay-slot return address instead.
  return true;
}

CPU::CallStack::CallStack(VirtualMemoryAddress pc, VirtualMemoryAddress sp, VirtualMemoryAddress ra)
  : m_initial_pc(pc), m_initial_sp(sp), m_initial_ra(ra)
{
}

void CPU::CallStack::Walk()
{
  m_frames.clear();
  m_frames.reserve(CALL_STACK_MAX_FRAMES);

  VirtualMemoryAddress address = m_initial_pc;
  VirtualMemoryAddress sp = m_initial_sp;
  std::optional<VirtualMemoryAddress> live_ra = m_initial_ra;
  m_frames.push_back({address, sp});

  while (m_frames.size() < CALL_STACK_MAX_FRAMES)
  {
    VirtualMemoryAddress caller_address;
    VirtualMemoryAddress caller_sp;
    if (!UnwindCallStackFrame(address, sp, live_ra, &caller_address, &caller_sp) || caller_sp < sp)
      break;

    const auto duplicate =
      std::find_if(m_frames.begin(), m_frames.end(), [caller_address, caller_sp](const Frame& frame) {
        return (frame.address == caller_address && frame.stack_pointer == caller_sp);
      });
    if (duplicate != m_frames.end())
      break;

    m_frames.push_back({caller_address, caller_sp});
    address = caller_address;
    sp = caller_sp;
    live_ra.reset();
  }
}

std::vector<CPU::CallStack::Frame> CPU::CallStack::TakeFrames()
{
  return std::move(m_frames);
}
