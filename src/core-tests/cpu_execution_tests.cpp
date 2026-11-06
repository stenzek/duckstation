// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_test_harness.h"

#include "core/cpu_call_stack.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_code_cache_private.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/cpu_pgxp.h"
#include "core/save_state_version.h"
#include "core/system.h"
#include "core/system_private.h"

#include "util/state_wrapper.h"

#include <array>
#include <initializer_list>
#include <iomanip>

namespace {

using namespace CPUTestHarness;
using namespace Mips::Encoder;

class CPUExecution : public ExecutionModeTest
{
};

struct CPUMemoryExecution : public ExecutionModeTest
{
protected:
  void SetUp() override
  {
    m_old_settings = g_settings;
    ExecutionModeTest::SetUp();
  }

  void TearDown() override
  {
    ResetMemory(m_old_settings.cpu_ram_size, m_old_settings.cpu_fastmem_mode);
    g_settings = m_old_settings;
  }

  static void SetWindow(u32 value)
  {
    CPU::GetMemoryWriteHandler(0xBF801060, MemoryAccessSize::Word)(0xBF801060, value);
  }

  static void WriteCode(u32 address, std::initializer_list<u32> instructions)
  {
    for (const u32 bits : instructions)
    {
      std::memcpy(&g_bus.unprotected_ram[address & g_bus.ram_mask], &bits, sizeof(bits));
      address += sizeof(bits);
    }
  }

  static void WriteExceptionHandler(u32 exit_pc)
  {
    // Exceptions set V0 to 99 and exit.
    CPU::g_state.cop0_regs.sr.BEV = false;
    WriteCode(0x80000080, {nop(), addiu(Reg::V0, Reg::R0, 99), j(exit_pc), nop()});
  }

  static void ExpectRecompiledBlock(u32 pc)
  {
    if (g_settings.cpu_execution_mode != CPUExecutionMode::Recompiler)
      return;

    const void* code = CPU::CodeCache::g_code_lut[pc >> CPU::CodeCache::LUT_TABLE_SHIFT][(pc & 0xFFFFu) / sizeof(u32)];
    EXPECT_NE(code, CPU::CodeCache::g_recompiler_functions.interpret_block) << std::hex << pc;
  }

  Settings m_old_settings;
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

TEST_P(CPUExecution, PGXPIgnoresWritesToZero)
{
  // The PGXP value for $zero must stay zero, in both PGXP modes, and with memory exceptions where add isn't a nop.
  g_settings.gpu_pgxp_enable = true;
  CPU::PGXP::Initialize();
  for (const bool cpu_mode : {false, true})
  {
    for (const bool memory_exceptions : {false, true})
    {
      g_settings.gpu_pgxp_cpu = cpu_mode;
      g_settings.cpu_recompiler_memory_exceptions = memory_exceptions;
      CPU::PGXP::Reset();
      const CPU::PGXPValue zero = CPU::g_state.pgxp_gpr[0];

      // Unchanged blocks are revalidated rather than recompiled, so they'd keep the previous settings.
      CPU::CodeCache::Reset();

      Program prog;
      prog.LoadImmediate(Reg::T0, 0x12345678u);
      prog.LoadImmediate(Reg::T1, RAM_TEST_ADDR);
      prog.Emit(sw(Reg::T0, 0, Reg::T1));
      prog.Emit(lw(Reg::R0, 0, Reg::T1));
      prog.Emit(nop());
      prog.Emit(addu(Reg::R0, Reg::T0, Reg::R0));
      prog.Emit(add(Reg::R0, Reg::T0, Reg::T0));
      prog.Emit(orr(Reg::R0, Reg::T0, Reg::T0));
      prog.Emit(addiu(Reg::R0, Reg::T0, 1));
      prog.Emit(ori(Reg::R0, Reg::T0, 0));
      prog.Emit(sll(Reg::R0, Reg::T0, 1));
      prog.Emit(lui(Reg::R0, 0x1234));
      prog.Run();

      EXPECT_EQ(std::memcmp(&CPU::g_state.pgxp_gpr[0], &zero, sizeof(zero)), 0)
        << (cpu_mode ? "CPU" : "memory") << (memory_exceptions ? " with memory exceptions" : "");
      if (cpu_mode)
        EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(Reg::T0)].value, 0x12345678u);
    }
  }

  CPU::PGXP::Shutdown();
  g_settings.gpu_pgxp_enable = false;
  g_settings.gpu_pgxp_cpu = false;
  g_settings.cpu_recompiler_memory_exceptions = false;
  CPU::CodeCache::Reset();
}

TEST_P(CPUExecution, PGXPMTC0KeepsSourceValid)
{
  // mtc0 validates its source register, which must not lose its flags.
  g_settings.gpu_pgxp_enable = true;
  g_settings.gpu_pgxp_cpu = true;
  CPU::PGXP::Initialize();
  CPU::CodeCache::Reset();
  const CPU::PGXPValue zero = CPU::g_state.pgxp_gpr[0];

  Program prog;
  prog.LoadImmediate(Reg::T0, 0x12345678u);
  prog.Emit(mtc0(Reg::T0, 3));
  prog.Emit(mtc0(Reg::R0, 3));
  prog.Run();

  EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(Reg::T0)].flags & CPU::PGXP::VALID_XY, CPU::PGXP::VALID_XY);
  EXPECT_EQ(std::memcmp(&CPU::g_state.pgxp_gpr[0], &zero, sizeof(zero)), 0);

  CPU::PGXP::Shutdown();
  g_settings.gpu_pgxp_enable = false;
  g_settings.gpu_pgxp_cpu = false;
  CPU::CodeCache::Reset();
}

TEST_P(CPUExecution, PGXPMemoryModeMovesWithZeroImmediate)
{
  // addiu/ori/xori with a zero immediate are moves, which memory mode PGXP keeps the value through.
  g_settings.gpu_pgxp_enable = true;
  CPU::PGXP::Initialize();
  CPU::CodeCache::Reset();

  static constexpr CPU::PGXPValue precise = {100.25f, 200.5f, 300.0f, 0x00C80064u, CPU::PGXP::VALID_ALL};
  CPU::g_state.pgxp_gpr[static_cast<u32>(Reg::T0)] = precise;

  Program prog;
  prog.LoadImmediate(Reg::T0, precise.value);
  prog.Emit(addiu(Reg::T1, Reg::T0, 0));
  prog.Emit(ori(Reg::T2, Reg::T0, 0));
  prog.Emit(xori(Reg::T3, Reg::T0, 0));
  prog.Run();

  for (const Reg reg : {Reg::T1, Reg::T2, Reg::T3})
  {
    EXPECT_EQ(std::memcmp(&CPU::g_state.pgxp_gpr[static_cast<u32>(reg)], &precise, sizeof(precise)), 0)
      << static_cast<u32>(reg);
  }

  CPU::PGXP::Shutdown();
  g_settings.gpu_pgxp_enable = false;
  CPU::CodeCache::Reset();
}

TEST_P(CPUExecution, PGXPUnalignedStoreInvalidatesWrittenHalf)
{
  // swr into the upper half of a precise word must invalidate that half, and leave the lower one alone.
  g_settings.gpu_pgxp_enable = true;
  g_settings.gpu_pgxp_cpu = true;
  CPU::PGXP::Initialize();
  CPU::CodeCache::Reset();

  // Not written through the CPU, so untracked.
  const u32 untracked = 0x0000012Cu;
  std::memcpy(&g_bus.ram[(RAM_TEST_ADDR + 8) & g_bus.ram_mask], &untracked, sizeof(untracked));

  Program prog;
  prog.LoadImmediate(Reg::T0, 0x00640064u);
  prog.LoadImmediate(Reg::T1, RAM_TEST_ADDR);
  prog.Emit(lw(Reg::T2, 8, Reg::T1));
  prog.Emit(nop());
  prog.Emit(sw(Reg::T0, 0, Reg::T1));
  prog.Emit(swr(Reg::T2, 2, Reg::T1));
  prog.Emit(lhu(Reg::T3, 0, Reg::T1));
  prog.Emit(lhu(Reg::T4, 2, Reg::T1));
  prog.Emit(sw(Reg::T0, 4, Reg::T1));
  prog.Emit(swr(Reg::T2, 7, Reg::T1));
  prog.Emit(lhu(Reg::T5, 4, Reg::T1));
  prog.Emit(lhu(Reg::T6, 6, Reg::T1));
  prog.Emit(nop());
  const u32 upper_half = prog.StoreResult(Reg::T4);
  const u32 upper_byte = prog.StoreResult(Reg::T6);
  prog.Run();

  EXPECT_EQ(prog.Result(upper_half), 0x012Cu);
  EXPECT_EQ(prog.Result(upper_byte), 0x2C64u);
  for (const Reg reg : {Reg::T3, Reg::T5})
  {
    EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(reg)].flags & CPU::PGXP::VALID_X, CPU::PGXP::VALID_X)
      << static_cast<u32>(reg);
  }
  for (const Reg reg : {Reg::T4, Reg::T6})
    EXPECT_EQ(CPU::g_state.pgxp_gpr[static_cast<u32>(reg)].flags & CPU::PGXP::VALID_X, 0u) << static_cast<u32>(reg);

  CPU::PGXP::Shutdown();
  g_settings.gpu_pgxp_enable = false;
  g_settings.gpu_pgxp_cpu = false;
  CPU::CodeCache::Reset();
}

TEST_P(CPUExecution, CallStackLeafAfterOutOfLineBlocks)
{
  // Compilers can place blocks of a function after its return, so the instructions preceding a leaf function are
  // not necessarily the previous function's return. The leaf must not inherit the out-of-line block's call.
  static constexpr u32 BASE = 0x80030000u;
  static constexpr u32 LEAF = BASE + 0x30;
  static constexpr u32 CALLER = BASE + 0x100;
  static constexpr u32 SP = 0x801FF000u;
  std::memset(&g_bus.unprotected_ram[BASE & g_bus.ram_mask], 0, 0x200);
  std::memset(&g_bus.unprotected_ram[SP & g_bus.ram_mask], 0, 0x40);

  u32 address = BASE;
  for (const u32 bits :
       {// Previous function, with an out-of-line block containing a call after the return.
        addiu(Reg::SP, Reg::SP, -0x18), sw(Reg::RA, 0x14, Reg::SP), jal(CALLER), nop(), lw(Reg::RA, 0x14, Reg::SP),
        nop(), jr(Reg::RA), addiu(Reg::SP, Reg::SP, 0x18), jal(CALLER), nop(), j(BASE + 0x10), nop(),
        // Leaf function.
        beq(Reg::A2, Reg::R0, 2), nop(), lbu(Reg::V1, 0, Reg::A1), jr(Reg::RA), nop()})
  {
    std::memcpy(&g_bus.unprotected_ram[address & g_bus.ram_mask], &bits, sizeof(bits));
    address += sizeof(bits);
  }

  address = CALLER;
  for (const u32 bits : {addiu(Reg::SP, Reg::SP, -0x20), sw(Reg::RA, 0x1C, Reg::SP), jal(LEAF), nop(),
                         lw(Reg::RA, 0x1C, Reg::SP), nop(), jr(Reg::RA), addiu(Reg::SP, Reg::SP, 0x20)})
  {
    std::memcpy(&g_bus.unprotected_ram[address & g_bus.ram_mask], &bits, sizeof(bits));
    address += sizeof(bits);
  }

  CPU::CallStack call_stack(LEAF + 8, SP, CALLER + 0x10);
  call_stack.Walk();
  const CPU::CallStack::FrameList& frames = call_stack.GetFrames();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[1].address, CALLER + 8);
  EXPECT_EQ(frames[1].stack_pointer, SP);
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, CPUExecution, ::testing::ValuesIn(EXECUTION_MODES), GetExecutionModeTestName);

TEST_P(CPUMemoryExecution, FastmemAccessesFollowDRAMWindowsAndInstalledRAM)
{
  // The normal result buffer starts at 1 MB, outside the smallest DRAM window.
  static constexpr u32 result_address = 0x80070000;
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    for (const CPURAMSize size :
         {CPURAMSize::Default, CPURAMSize::Expanded4MB, CPURAMSize::Expanded8MB, CPURAMSize::Expanded16MB})
    {
      SCOPED_TRACE(Settings::GetCPURAMSizeName(size));
      ResetMemory(size, mode);
      for (u32 window = 0; window < 16; window++)
      {
        SCOPED_TRACE(window);
        SetWindow((window << 8) | 0x88);
        for (const u32 segment : {0u, 0x80000000u, 0xA0000000u})
        {
          for (u32 offset = 0x20000; offset < g_bus.ram_mapped_size; offset += 1048576)
          {
            const u32 address = segment | offset;
            SCOPED_TRACE(address);
            const u32 value = 0x12340000 | (offset / 1048576);
            ASSERT_TRUE(CPU::SafeWriteMemoryWord(address, value));

            // Space out recompiles of the same PC so the SMC fallback limit cannot turn this into an interpreter
            // test after the first few addresses. The dispatch check below verifies that it stays compiled.
            for (u32 i = 0; i < 15; i++)
              System::IncrementFrameNumber();
            Program prog;
            prog.LoadImmediate(Reg::T0, address);
            prog.Emit(lw(Reg::T1, 0, Reg::T0));
            prog.Emit(nop());
            prog.LoadImmediate(Reg::T9, result_address);
            prog.Emit(sw(Reg::T1, 0, Reg::T9));
            prog.LoadImmediate(Reg::T1, value ^ 0x55555555);
            prog.Emit(sw(Reg::T1, 0, Reg::T0));
            prog.Run();

            EXPECT_EQ(Program::ReadRAM(result_address), value);
            u32 written_value;
            ASSERT_TRUE(CPU::SafeReadMemoryWord(address, &written_value));
            EXPECT_EQ(written_value, value ^ 0x55555555);
            ExpectRecompiledBlock(PROGRAM_BASE);
          }
        }
      }
    }
  }
}

TEST_P(CPUMemoryExecution, MirrorBranchAcrossPageUsesManualProtectionWithoutInterpreterFallback)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Default, mode);
    const u32 page_end_pc = (PROGRAM_BASE | Bus::RAM_DEFAULT_SIZE) + HOST_PAGE_SIZE * 2;
    const u32 block_pc = page_end_pc - 4 * sizeof(u32);
    const u32 exit_pc = PROGRAM_BASE + HOST_PAGE_SIZE * 4;
    WriteCode(block_pc, {addiu(Reg::V0, Reg::R0, 42), nop(), nop(), j(exit_pc), sw(Reg::V0, 0, Reg::T9)});
    WriteCode(exit_pc, {sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT), j(exit_pc), nop()});

    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.Emit(j(block_pc));
    prog.Emit(nop());
    prog.Run(2);
    EXPECT_EQ(prog.Result(0), 42u);
    ExpectRecompiledBlock(block_pc);

    // Change RAM without invalidating the block. The manual check must inspect the mirrored backing bytes.
    WriteCode(block_pc, {addiu(Reg::V0, Reg::R0, 43)});
    CPU::ClearICache();
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
    EXPECT_EQ(prog.Result(0), 43u);
    ExpectRecompiledBlock(block_pc);
  }
}

TEST_P(CPUMemoryExecution, BlockWrappingPhysicalRAMSeesChangedDelaySlot)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Default, mode);

    // The delay slot is in the next mirror, backed by the start of physical RAM.
    const u32 block_pc = 0x80000000u + (Bus::RAM_DEFAULT_SIZE * 2) - sizeof(u32);
    const u32 exit_pc = PROGRAM_BASE + HOST_PAGE_SIZE * 4;
    WriteCode(block_pc, {j(exit_pc), nop()});
    WriteCode(exit_pc, {sw(Reg::V0, 0, Reg::T9), sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT),
                        j(exit_pc), nop()});

    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.Emit(addiu(Reg::V0, Reg::R0, 42));
    prog.Emit(j(block_pc));
    prog.Emit(nop());
    prog.Run(2);
    ASSERT_EQ(prog.Result(0), 42u);

    // Change the delay slot without invalidating the block.
    WriteCode(block_pc + sizeof(u32), {addiu(Reg::V0, Reg::R0, 43)});
    CPU::ClearICache();
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
    EXPECT_EQ(prog.Result(0), 43u);
  }
}

TEST_P(CPUMemoryExecution, MirrorPageRemainsCompiledAfterRepeatedInvalidation)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Default, mode);
    const u32 block_pc = (PROGRAM_BASE | 0x00600000u) + HOST_PAGE_SIZE * 2;
    const u32 exit_pc = PROGRAM_BASE + HOST_PAGE_SIZE * 4;
    WriteCode(block_pc, {addiu(Reg::V0, Reg::R0, 42), sw(Reg::V0, 0, Reg::T9), j(exit_pc), nop()});
    WriteCode(exit_pc, {sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT), j(exit_pc), nop()});

    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.Emit(j(block_pc));
    prog.Emit(nop());
    prog.Run();
    for (u32 i = 0; i < 8; i++)
    {
      // Leave the instructions unchanged so ordinary invalidation can revalidate them. Frequent invalidations
      // eventually switch the page to manual checking, which must use the same backing bytes.
      CPU::CodeCache::InvalidateBlocksWithPageIndex(Bus::GetRAMCodePageIndex(block_pc));
      CPU::SetPC(PROGRAM_BASE);
      CPU::Execute();
      EXPECT_EQ(prog.Result(0), 42u);
      ExpectRecompiledBlock(block_pc);
    }
  }
}

TEST_P(CPUMemoryExecution, BlockDelaySlotBecomesUnmappedWhenWindowShrinks)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Expanded8MB, mode);
    static constexpr u32 block_pc = 0xA01FFFFC;
    static constexpr u32 exit_pc = 0xA0020000;
    WriteExceptionHandler(exit_pc);
    WriteCode(block_pc, {j(exit_pc), addiu(Reg::V0, Reg::R0, 43)});
    WriteCode(exit_pc, {sw(Reg::V0, 0, Reg::T9), sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT),
                        j(exit_pc), nop()});

    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.Emit(addiu(Reg::V0, Reg::R0, 42));
    prog.LoadImmediate(Reg::T8, block_pc);
    prog.Emit(jr(Reg::T8));
    prog.Emit(nop());
    prog.Run(2);
    ASSERT_EQ(prog.Result(0), 43u);

    // Shrinking to 2MB leaves the delay slot unmapped, so the cached block must not be reused. Memory state loads
    // invalidate all blocks again, which must not allow it to be revalidated either.
    SetWindow(0x488);
    ASSERT_EQ(g_bus.ram_mapped_size, Bus::RAM_DEFAULT_SIZE);
    CPU::CodeCache::InvalidateAllRAMBlocks();
    CPU::ClearICache();
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
    EXPECT_EQ(prog.Result(0), 99u);
  }
}

TEST_P(CPUMemoryExecution, UnmappedCodeIsCompiledWhenWindowExpands)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Expanded8MB, mode);
    static constexpr u32 block_pc = 0xA0610000;
    static constexpr u32 exit_pc = 0xA0020000;
    WriteExceptionHandler(exit_pc);
    WriteCode(block_pc, {addiu(Reg::V0, Reg::R0, 43), j(exit_pc), nop()});
    WriteCode(exit_pc, {sw(Reg::V0, 0, Reg::T9), sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT),
                        j(exit_pc), nop()});

    // Dispatch while the code is unmapped.
    SetWindow(0x488);
    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.LoadImmediate(Reg::T8, block_pc);
    prog.Emit(jr(Reg::T8));
    prog.Emit(nop());
    prog.Run(1);
    ASSERT_EQ(prog.Result(0), 99u);

    // Once mapped, it should be cached rather than left on the interpreter fallback.
    SetWindow(0xB88);
    CPU::ClearICache();
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
    EXPECT_EQ(prog.Result(0), 43u);
    if (GetParam() != CPUExecutionMode::Interpreter)
      EXPECT_TRUE(Bus::IsRAMCodePage(Bus::GetRAMCodePageIndex(block_pc)));
    ExpectRecompiledBlock(block_pc);
  }
}

TEST_P(CPUMemoryExecution, BranchWithUnmappedDelaySlotIsCompiledWhenWindowExpands)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Expanded8MB, mode);
    static constexpr u32 block_pc = 0xA01FFFFC;
    static constexpr u32 exit_pc = 0xA0020000;
    WriteExceptionHandler(exit_pc);
    WriteCode(block_pc, {j(exit_pc), addiu(Reg::V0, Reg::R0, 43)});
    WriteCode(exit_pc, {sw(Reg::V0, 0, Reg::T9), sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT),
                        j(exit_pc), nop()});

    // Dispatch while the delay slot is unmapped.
    SetWindow(0x488);
    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.LoadImmediate(Reg::T8, block_pc);
    prog.Emit(jr(Reg::T8));
    prog.Emit(nop());
    prog.Run(1);
    ASSERT_EQ(prog.Result(0), 99u);

    // The branch is below the old window end, but should still be retried.
    SetWindow(0xB88);
    CPU::ClearICache();
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
    EXPECT_EQ(prog.Result(0), 43u);
    ExpectRecompiledBlock(block_pc);
  }
}

TEST_P(CPUMemoryExecution, BIOSLinkToUnmappedCodeIsRetriedWhenWindowExpands)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Expanded8MB, mode);
    static constexpr u32 block_pc = 0xA0610000;
    static constexpr u32 bios_pc = 0xBFC10000;
    static constexpr u32 exit_pc = 0xA0020000;
    WriteExceptionHandler(exit_pc);
    WriteCode(block_pc, {addiu(Reg::V0, Reg::R0, 43), j(exit_pc), nop()});
    WriteCode(exit_pc, {sw(Reg::V0, 0, Reg::T9), sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT),
                        j(exit_pc), nop()});

    // BIOS blocks aren't invalidated with RAM, so their direct link to the RAM code must be updated.
    static constexpr std::array<u32, 4> bios_code = {lui(Reg::T8, static_cast<u16>(block_pc >> 16)),
                                                     ori(Reg::T8, Reg::T8, static_cast<u16>(block_pc & 0xFFFFu)),
                                                     jr(Reg::T8), nop()};
    std::memcpy(g_bus.bios + (bios_pc & Bus::BIOS_MASK), bios_code.data(), sizeof(bios_code));

    // Dispatch while the code is unmapped.
    SetWindow(0x488);
    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.LoadImmediate(Reg::T8, bios_pc);
    prog.Emit(jr(Reg::T8));
    prog.Emit(nop());
    prog.Run(1);
    ASSERT_EQ(prog.Result(0), 99u);

    SetWindow(0xB88);
    CPU::ClearICache();
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
    EXPECT_EQ(prog.Result(0), 43u);
    if (GetParam() != CPUExecutionMode::Interpreter)
      EXPECT_TRUE(Bus::IsRAMCodePage(Bus::GetRAMCodePageIndex(block_pc)));
    ExpectRecompiledBlock(block_pc);
  }
}

TEST_P(CPUMemoryExecution, Expanded16MBCodeUsesCodeCache)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Expanded16MB, mode);
    SetWindow(0xF88);
    for (const u32 address : {0x00C10000u, 0x80C10000u, 0xA0C10000u})
    {
      SCOPED_TRACE(address);
      CPU::CodeCache::Reset();
      Program prog;
      prog.LoadImmediate(Reg::V0, 42);
      const u32 slot = prog.StoreResult(Reg::V0);
      prog.RunAt(address, 2);
      EXPECT_EQ(prog.Result(slot), 42u);
      if (GetParam() != CPUExecutionMode::Interpreter)
        EXPECT_TRUE(Bus::IsRAMCodePage(Bus::GetRAMCodePageIndex(address)));
      ExpectRecompiledBlock(address);
    }
  }
}

TEST_P(CPUMemoryExecution, CachedFetchFromUnmappedRAMRaisesBusError)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Default, mode);
    static constexpr u32 block_pc = 0x80650000;
    static constexpr u32 exit_pc = 0xA0020000;
    WriteExceptionHandler(exit_pc);
    WriteCode(block_pc, {addiu(Reg::V0, Reg::R0, 43), j(exit_pc), nop()});
    WriteCode(exit_pc, {sw(Reg::V0, 0, Reg::T9), sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT),
                        j(exit_pc), nop()});
    Program prog;
    prog.Emit(lui(Reg::T9, static_cast<u16>(RESULTS_BASE >> 16)));
    prog.LoadImmediate(Reg::T8, block_pc);
    prog.Emit(jr(Reg::T8));
    prog.Emit(nop());
    prog.Run(1);
    ASSERT_EQ(prog.Result(0), 43u);

    // The mirror is unmapped once the window shrinks. Fetching through the cached segment must raise a bus error,
    // even though the code was previously cached.
    SetWindow(0x888);
    CPU::ClearICache();
    CPU::g_state.cop0_regs.BadVaddr = 0x12345678;
    CPU::SetPC(PROGRAM_BASE);
    CPU::Execute();
    EXPECT_EQ(prog.Result(0), 99u);
    EXPECT_EQ(static_cast<u8>(CPU::g_state.cop0_regs.cause.Excode.GetValue()), static_cast<u8>(CPU::Exception::IBE));
    EXPECT_EQ(CPU::g_state.cop0_regs.EPC, block_pc);
    EXPECT_EQ(CPU::g_state.cop0_regs.BadVaddr, 0x12345678u);
  }
}

TEST_P(CPUMemoryExecution, Expanded16MBCodeUsesCodeCacheAfterStateLoad)
{
  for (const CPUFastmemMode mode : FASTMEM_MODES)
  {
    SCOPED_TRACE(Settings::GetCPUFastmemModeName(mode));
    ResetMemory(CPURAMSize::Expanded16MB, mode);
    SetWindow(0xF88);
    std::vector<u8> data(System::GetMaxSaveStateSize(CPURAMSize::Expanded16MB, false));
    StateWrapper save_sw(std::span<u8>(data), StateWrapper::Mode::Write, SAVE_STATE_VERSION);
    ASSERT_TRUE(Bus::DoState(save_sw));
    data.resize(save_sw.GetPosition());

    // The 2MB LUTs only cover 8MB, so loading the state must grow them.
    ResetMemory(CPURAMSize::Default, mode);
    StateWrapper load_sw(std::span<const u8>(data), StateWrapper::Mode::Read, SAVE_STATE_VERSION);
    ASSERT_TRUE(Bus::DoState(load_sw));
    ASSERT_EQ(g_bus.ram_size, Bus::RAM_MAX_SIZE);

    static constexpr u32 address = 0x80C10000;
    Program prog;
    prog.LoadImmediate(Reg::V0, 42);
    const u32 slot = prog.StoreResult(Reg::V0);
    prog.RunAt(address, 2);
    EXPECT_EQ(prog.Result(slot), 42u);
    if (GetParam() != CPUExecutionMode::Interpreter)
      EXPECT_TRUE(Bus::IsRAMCodePage(Bus::GetRAMCodePageIndex(address)));
    ExpectRecompiledBlock(address);
  }
}

TEST_P(CPUMemoryExecution, EXP1LUTFollowsPIODevice)
{
  // 0x1F7F0000 is never covered by a LUT, so it always shares the unreachable table.
  const auto has_exp1_lut = []() {
    return (CPU::CodeCache::g_code_lut[0x1F000000u >> CPU::CodeCache::LUT_TABLE_SHIFT] !=
            CPU::CodeCache::g_code_lut[0x1F7F0000u >> CPU::CodeCache::LUT_TABLE_SHIFT]);
  };

  g_settings.pio_device_type = PIODeviceType::None;
  ResetMemory(CPURAMSize::Default, CPUFastmemMode::Disabled);
  EXPECT_FALSE(has_exp1_lut());

  g_settings.pio_device_type = PIODeviceType::XplorerCart;
  CPU::CodeCache::ReallocateLUTs();
  EXPECT_TRUE(has_exp1_lut());

  // RAM code should still be cached after reallocating.
  Program prog;
  prog.LoadImmediate(Reg::V0, 42);
  const u32 slot = prog.StoreResult(Reg::V0);
  prog.Run(2);
  EXPECT_EQ(prog.Result(slot), 42u);
  ExpectRecompiledBlock(PROGRAM_BASE);

  g_settings.pio_device_type = PIODeviceType::None;
  CPU::CodeCache::ReallocateLUTs();
  EXPECT_FALSE(has_exp1_lut());
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, CPUMemoryExecution, ::testing::ValuesIn(EXECUTION_MODES),
                         GetExecutionModeTestName);
