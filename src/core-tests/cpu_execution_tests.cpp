// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_test_harness.h"

#include "core/cpu_code_cache.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/system_private.h"

#include <initializer_list>

namespace {

using namespace CPUTestHarness;
using namespace Mips::Encoder;

class CPUExecution : public ExecutionModeTest
{
};

} // namespace

TEST_P(CPUExecution, DoubleLoadDelayAcrossBlocks)
{
  // A second load to the same register discards the first one's value, even when the first one is in a branch delay
  // slot and the second one starts the next block.
  for (const bool taken : {false, true})
  {
    Program prog;
    prog.LoadImmediate(Reg::T0, RAM_TEST_ADDR);
    prog.LoadImmediate(Reg::T2, 0xAAAAAAAAu);
    prog.Emit(sw(Reg::T2, 0, Reg::T0));
    prog.LoadImmediate(Reg::T2, 0xBBBBBBBBu);
    prog.Emit(sw(Reg::T2, 4, Reg::T0));
    prog.LoadImmediate(Reg::T1, 0x12345678u);
    prog.LoadImmediate(Reg::A2, taken ? 0x12345678u : 0u);

    // Either way ends up at the instruction after the delay slot.
    prog.Emit(beq(Reg::A2, Reg::T1, 4));
    prog.Emit(lw(Reg::T1, 0, Reg::T0));
    prog.Emit(lw(Reg::T1, 4, Reg::T0));
    prog.Emit(addu(Reg::A0, Reg::T1, Reg::R0));
    prog.Emit(addu(Reg::A1, Reg::T1, Reg::R0));
    const u32 before = prog.StoreResult(Reg::A0);
    const u32 after = prog.StoreResult(Reg::A1);
    prog.Run();

    EXPECT_EQ(prog.Result(before), 0x12345678u) << (taken ? "taken" : "not taken");
    EXPECT_EQ(prog.Result(after), 0xBBBBBBBBu) << (taken ? "taken" : "not taken");
  }
}

TEST_P(CPUExecution, ManualProtectionDetectsChangedCode)
{
  // A block with its delay slot in the next page always uses manual protection, so changes are only picked up by the
  // check at the start of the block. Change each instruction of different sized blocks without invalidating it.
  const u32 page_end_pc = PROGRAM_BASE + HOST_PAGE_SIZE * 2;
  const u32 exit_pc = PROGRAM_BASE + HOST_PAGE_SIZE * 3;
  const auto write_code = [](u32 address, std::initializer_list<u32> code) {
    std::memcpy(&g_bus.ram[address & g_bus.ram_mask], std::data(code), code.size() * sizeof(u32));
  };

  std::vector<u32> sizes;
  for (u32 size = 2; size <= 24; size++)
    sizes.push_back(size);
  for (u32 size : {63u, 64u, 65u, 66u, 258u, 259u, 260u, 261u, 262u, 263u, 300u})
    sizes.push_back(size);

  for (const u32 size : sizes)
  {
    const u32 block_pc = page_end_pc - (size - 1) * sizeof(u32);
    for (u32 changed = 0; changed < size; changed++)
    {
      // Don't change the branch.
      if (changed == (size - 2))
        continue;

      // Pages with blocks in them are read-only. Blocks recompiled too often within 15 frames get interpreted instead.
      CPU::CodeCache::InvalidateAllRAMBlocks();
      for (u32 i = 0; i < 15; i++)
        System::IncrementFrameNumber();
      for (u32 i = 0; i < (size - 2); i++)
        write_code(block_pc + i * sizeof(u32), {addiu(Reg::V0, Reg::V0, 1)});
      write_code(page_end_pc - sizeof(u32), {j(exit_pc), addiu(Reg::V0, Reg::V0, 1)});
      write_code(exit_pc,
                 {sw(Reg::V0, 0, Reg::T9), sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT), nop(), nop()});

      // Second run executes the already compiled block.
      Program prog;
      prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
      prog.Emit(j(block_pc));
      prog.Emit(addu(Reg::V0, Reg::R0, Reg::R0));
      prog.Run(2);
      ASSERT_EQ(prog.Result(0), size - 1) << size << " instructions";

      write_code(block_pc + changed * sizeof(u32), {addiu(Reg::V0, Reg::V0, 2)});
      CPU::ClearICache();
      CPU::SetPC(PROGRAM_BASE);
      CPU::Execute();
      ASSERT_EQ(prog.Result(0), size) << size << " instructions, changed " << changed;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, CPUExecution, ::testing::ValuesIn(EXECUTION_MODES), GetExecutionModeTestName);
