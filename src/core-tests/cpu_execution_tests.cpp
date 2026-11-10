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

class CPUCop0Breakpoints : public ExecutionModeTest
{
};

class CPUInterruptTiming : public ExecutionModeTest
{
protected:
  void SetUp() override
  {
    ExecutionModeTest::SetUp();
    m_old_tty_logging = g_settings.bios_tty_logging;
    const std::array handler = {lui(Reg::AT, static_cast<u16>(ADDR_EXIT >> 16)),
                                sb(Reg::R0, static_cast<s16>(ADDR_EXIT), Reg::AT), nop()};
    std::memcpy(&g_bus.ram[0x80], handler.data(), sizeof(handler));
  }

  void TearDown() override
  {
    CPU::g_state.cop0_regs.sr.bits = 0;
    CPU::g_state.cop0_regs.cause.bits = 0;
    g_settings.bios_tty_logging = m_old_tty_logging;
    CPU::UpdateDebugDispatcherFlag();
    s_event->SetIntervalAndSchedule(1000000);
  }

  bool m_old_tty_logging;
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

TEST_P(CPUCop0Breakpoints, AddressEnablesAndTrapControl)
{
  for (const bool user_mode : {false, true})
  {
    for (const bool user_address : {false, true})
    {
      for (const u32 mode_enables : {0u, 1u << 29, 1u << 30, 3u << 29})
      {
        for (const bool debug_enable : {false, true})
        {
          for (const bool trap_enable : {false, true})
          {
            for (const u32 access : {0u, 1u, 2u})
            {
              SCOPED_TRACE(::testing::Message() << "user mode=" << user_mode << " user address=" << user_address
                                                << " modes=" << std::hex << mode_enables << " debug=" << debug_enable
                                                << " trap=" << trap_enable << " access=" << access);
              const u32 base = (access == 0 && user_address) ? (PROGRAM_BASE & CPU::KSEG_MASK) : PROGRAM_BASE;
              const u32 data_address = user_address ? (RAM_TEST_ADDR & CPU::KSEG_MASK) : RAM_TEST_ADDR;
              const u32 access_enable = (access == 0) ? (1u << 24) : ((1u << 25) | (1u << (25 + access)));
              const u32 dcic =
                mode_enables | access_enable | (debug_enable ? (1u << 23) : 0u) | (trap_enable ? (1u << 31) : 0u);
              const bool matches = debug_enable && (mode_enables & (1u << (user_address ? 30 : 29)));
              const u32 status = matches ? ((access == 0) ? 3u : ((access == 1) ? 13u : 21u)) : 0u;

              // Both paths capture DCIC and store the result through the common epilogue.
              const std::array handler = {mfc0(Reg::V0, 7),           nop(),        nop(),
                                          addiu(Reg::V1, Reg::R0, 1), j(base + 32), nop()};
              std::memcpy(&g_bus.ram[0x40], handler.data(), sizeof(handler));
              CPU::g_state.cop0_regs.sr.bits = (1u << 28) | (user_mode ? 2u : 0u);
              CPU::g_state.cop0_regs.dcic.bits = dcic;
              CPU::g_state.cop0_regs.BPC = base + 4;
              CPU::g_state.cop0_regs.BPCM = 0xFFFFFFFFu;
              CPU::g_state.cop0_regs.BDA = data_address;
              CPU::g_state.cop0_regs.BDAM = 0xFFFFFFFFu;
              CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)] = 0;
              CPU::UpdateDebugDispatcherFlag();

              Program prog;
              prog.Emit(nop());
              prog.LoadImmediate(Reg::T0, data_address);
              prog.Emit((access == 1) ? lw(Reg::T1, 0, Reg::T0) : sw(Reg::R0, 0, Reg::T0));
              prog.Emit(mfc0(Reg::V0, 7));
              prog.Emit(nop());
              prog.Emit(nop());
              const u32 captured_dcic = prog.StoreResult(Reg::V0);
              const u32 trapped = prog.StoreResult(Reg::V1);
              prog.RunAt(base);
              EXPECT_EQ(prog.Result(captured_dcic), dcic | status);
              EXPECT_EQ(prog.Result(trapped), matches && trap_enable ? 1u : 0u);
              CPU::g_state.cop0_regs.dcic.bits = 0;
              CPU::UpdateDebugDispatcherFlag();
            }
          }
        }
      }
    }
  }
}

TEST_P(CPUCop0Breakpoints, PartialWordBreakpointsUseFirstAccessedByte)
{
  for (const bool write : {false, true})
  {
    for (const bool right : {false, true})
    {
      for (const s16 offset : {0, 1, 2, 3})
      {
        for (const u32 breakpoint_offset : {0u, 1u, 2u, 3u, 4u})
        {
          for (const u32 mask : {0xFFFFFFFFu, 0xFFFFFFFCu})
          {
            SCOPED_TRACE(::testing::Message()
                         << "write=" << write << " right=" << right << " offset=" << offset
                         << " breakpoint offset=" << breakpoint_offset << " mask=" << std::hex << mask);
            // Status-only matching lets the access finish, so the program can report which byte matched.
            const u32 dcic = (1u << 23) | (1u << 25) | (1u << 29) | (1u << (write ? 27 : 26));
            CPU::g_state.cop0_regs.sr.bits = 0;
            CPU::g_state.cop0_regs.dcic.bits = dcic;
            CPU::g_state.cop0_regs.BDA = RAM_TEST_ADDR + breakpoint_offset;
            CPU::g_state.cop0_regs.BDAM = mask;
            CPU::UpdateDebugDispatcherFlag();

            Program prog;
            prog.LoadImmediate(Reg::T0, RAM_TEST_ADDR);
            prog.Emit(write ? (right ? swr(Reg::R0, offset, Reg::T0) : swl(Reg::R0, offset, Reg::T0)) :
                              (right ? lwr(Reg::T1, offset, Reg::T0) : lwl(Reg::T1, offset, Reg::T0)));
            prog.Emit(mfc0(Reg::V0, 7));
            prog.Emit(nop());
            const u32 captured_dcic = prog.StoreResult(Reg::V0);
            prog.Run();

            // Left accesses start at the aligned byte; right accesses start at the effective address.
            const u32 first_byte = right ? static_cast<u32>(offset) : 0u;
            const bool matches = (mask == 0xFFFFFFFFu) ? (breakpoint_offset == first_byte) : (breakpoint_offset < 4);
            EXPECT_EQ(prog.Result(captured_dcic), dcic | (matches ? (write ? 21u : 13u) : 0u));
            CPU::g_state.cop0_regs.dcic.bits = 0;
            CPU::UpdateDebugDispatcherFlag();
          }
        }
      }
    }
  }
}

TEST_P(CPUCop0Breakpoints, DataTrapsStopAccessesAndPreserveAlignmentExceptions)
{
  struct Access
  {
    u32 opcode;
    u32 alignment;
    bool write;
    u32 ce;
  };
  const Access accesses[] = {
    {0x20, 1, false, 0}, {0x24, 1, false, 0}, {0x21, 2, false, 1}, {0x25, 2, false, 1}, {0x23, 4, false, 3},
    {0x22, 1, false, 2}, {0x26, 1, false, 2}, {0x28, 1, true, 0},  {0x29, 2, true, 1},  {0x2B, 4, true, 3},
    {0x2A, 1, true, 2},  {0x2E, 1, true, 2},  {0x30, 4, false, 0}, {0x31, 4, false, 1}, {0x32, 4, false, 2},
    {0x33, 4, false, 3}, {0x38, 4, true, 0},  {0x39, 4, true, 1},  {0x3A, 4, true, 2},  {0x3B, 4, true, 3},
  };
  for (const Access& access : accesses)
  {
    for (const bool mapped : {false, true})
    {
      for (const u32 offset : {0u, 1u, 2u, 3u})
      {
        for (const bool delay_slot : {false, true})
        {
          SCOPED_TRACE(::testing::Message() << "opcode=" << std::hex << access.opcode << " mapped=" << mapped
                                            << " offset=" << offset << " delay slot=" << delay_slot);
          for (const u32 vector : {0x40u, 0x80u})
          {
            const std::array handler = {addiu(Reg::V1, Reg::R0, static_cast<s16>(vector)),
                                        lui(Reg::AT, static_cast<u16>(ADDR_EXIT >> 16)),
                                        sb(Reg::R0, static_cast<s16>(ADDR_EXIT), Reg::AT), nop()};
            std::memcpy(&g_bus.ram[vector], handler.data(), sizeof(handler));
          }
          const u32 address = ((mapped) ? RAM_TEST_ADDR : 0x40180000u) + offset;
          const u32 dcic = (1u << 23) | (1u << 25) | (3u << 29) | (1u << 31) | (1u << ((access.write) ? 27 : 26));
          CPU::g_state.cop0_regs.sr.bits = 0xF0000000u;
          CPU::g_state.cop0_regs.cause.bits = 0;
          CPU::g_state.cop0_regs.BadVaddr = 0xDEADBEEFu;
          CPU::g_state.cop0_regs.dcic.bits = dcic;
          CPU::g_state.cop0_regs.BDA = (access.opcode == 0x22 || access.opcode == 0x2A) ? (address & ~3u) : address;
          CPU::g_state.cop0_regs.BDAM = 0xFFFFFFFFu;
          CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)] = 0;
          CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::t1)] = 0xAABBCCDDu;
          const u32 original_memory = 0x12345678u;
          std::memcpy(&g_bus.ram[RAM_TEST_ADDR & g_bus.ram_mask], &original_memory, sizeof(original_memory));
          CPU::UpdateDebugDispatcherFlag();

          Program prog;
          prog.LoadImmediate(Reg::T0, address);
          if (delay_slot)
            prog.Emit(beq(Reg::R0, Reg::R0, 1));
          prog.Emit((access.opcode << 26) | srcVal(Reg::T0) | tgtVal(Reg::T1));
          prog.Emit(nop());
          prog.Run();

          const bool misaligned = (offset % access.alignment) != 0;
          const u32 exception = (misaligned) ? ((access.write) ? 5u : 4u) : 9u;
          const u32 cause =
            (exception << 2) | ((misaligned) ? (access.ce << 28) : 0u) | ((delay_slot) ? 0xC0000000u : 0u);
          EXPECT_EQ(CPU::g_state.cop0_regs.cause.bits, cause);
          EXPECT_EQ(CPU::g_state.cop0_regs.EPC, PROGRAM_BASE + 12);
          EXPECT_EQ(CPU::g_state.cop0_regs.BadVaddr, (misaligned) ? address : 0xDEADBEEFu);
          EXPECT_EQ(CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)], (misaligned) ? 0x80u : 0x40u);
          EXPECT_EQ(CPU::g_state.cop0_regs.dcic.bits, dcic | ((access.write) ? 21u : 13u));
          EXPECT_EQ(CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::t1)], 0xAABBCCDDu);
          EXPECT_EQ(Program::ReadRAM(RAM_TEST_ADDR), original_memory);
          CPU::g_state.cop0_regs.dcic.bits = 0;
          CPU::UpdateDebugDispatcherFlag();
        }
      }
    }
  }
}

TEST_P(CPUCop0Breakpoints, ExecutionMatchesBeforeFetchFaults)
{
  const bool old_tty_logging = g_settings.bios_tty_logging;
  std::vector<u32> addresses = {0x20010000u, 0x40010000u, 0x60010000u, 0xC0010000u,
                                0xE0010000u, 0x00030001u, 0x80030001u};
  for (const u32 segment : {0u, 0x80000000u, 0xA0000000u})
  {
    // Physical holes, scratchpad, and I/O aren't instruction-fetchable in any of these aliases.
    for (const u32 physical : {0x01000000u, 0x1F800000u, 0x1F801000u})
    {
      for (const u32 offset : {0u, 4u, 8u, 12u})
        addresses.push_back(segment | physical | offset);
    }
  }
  for (const u32 address : addresses)
  {
    const u32 physical_line = address & CPU::KSEG_MASK & ~0xFu;
    const bool physical_hole =
      (physical_line == 0x01000000u || physical_line == 0x1F800000u || physical_line == 0x1F801000u);
    // Physical holes don't force the debug interpreter; exercise their faults with TTY forcing it on.
    g_settings.bios_tty_logging = physical_hole;
    for (const u32 address_enables : {0u, 1u << 29, 1u << 30, 3u << 29})
    {
      for (const bool trap : {false, true})
      {
        for (const bool match : {false, true})
        {
          for (const u32 initial_status : {0u, 0x3Fu})
          {
            for (const u32 mask : {0xFFFFFFFFu, 0xFFFFFF00u})
            {
              SCOPED_TRACE(::testing::Message()
                           << "address=" << std::hex << address << " enables=" << address_enables << " trap=" << trap
                           << " match=" << match << " status=" << initial_status << " mask=" << mask);
              for (const u32 vector : {0x40u, 0x80u})
              {
                const std::array handler = {addiu(Reg::V1, Reg::R0, static_cast<s16>(vector)),
                                            lui(Reg::AT, static_cast<u16>(ADDR_EXIT >> 16)),
                                            sb(Reg::R0, static_cast<s16>(ADDR_EXIT), Reg::AT), nop()};
                std::memcpy(&g_bus.ram[vector], handler.data(), sizeof(handler));
              }
              const u32 dcic = (1u << 23) | (1u << 24) | address_enables | ((trap) ? (1u << 31) : 0u) | initial_status;
              CPU::g_state.cop0_regs.sr.bits = 0;
              CPU::g_state.cop0_regs.cause.bits = 0;
              CPU::g_state.cop0_regs.BadVaddr = 0xDEADBEEFu;
              CPU::g_state.cop0_regs.dcic.bits = dcic;
              CPU::g_state.cop0_regs.BPC = address + ((match) ? 0u : 0x100u);
              CPU::g_state.cop0_regs.BPCM = mask;
              CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)] = 0;
              CPU::UpdateDebugDispatcherFlag();
              EXPECT_EQ(CPU::g_state.using_debug_dispatcher, physical_hole || address_enables != 0u);

              Program prog;
              prog.LoadImmediate(Reg::T0, address);
              prog.Emit(jr(Reg::T0));
              prog.Emit(nop());
              prog.Run();

              const bool matched = match && (address_enables & (1u << ((address & 0x80000000u) ? 29 : 30)));
              const bool misaligned = (address & 3u) != 0;
              const bool trapped = matched && trap && !misaligned;
              EXPECT_EQ(CPU::g_state.cop0_regs.EPC, address);
              EXPECT_EQ(CPU::g_state.cop0_regs.cause.bits, (misaligned) ? 0x10u : ((trapped) ? 0x24u : 0x18u));
              EXPECT_EQ(CPU::g_state.cop0_regs.BadVaddr, (misaligned) ? address : 0xDEADBEEFu);
              EXPECT_EQ(CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)], (trapped) ? 0x40u : 0x80u);
              EXPECT_EQ(CPU::g_state.cop0_regs.dcic.bits, dcic | ((matched) ? 3u : 0u));
              CPU::g_state.cop0_regs.dcic.bits = 0;
              CPU::UpdateDebugDispatcherFlag();
            }
          }
        }
      }
    }
  }
  g_settings.bios_tty_logging = old_tty_logging;
  CPU::UpdateDebugDispatcherFlag();
}

TEST_P(CPUCop0Breakpoints, ExecutionBreakpointsMatchExpansionCode)
{
  for (const u32 address : {0x1F000000u, 0x9F000000u, 0xBF000000u})
  {
    for (const bool trap : {false, true})
    {
      SCOPED_TRACE(::testing::Message() << "address=" << std::hex << address << " trap=" << trap);
      for (const u32 vector : {0x40u, 0x80u})
      {
        const std::array handler = {addiu(Reg::V1, Reg::R0, static_cast<s16>(vector)),
                                    lui(Reg::AT, static_cast<u16>(ADDR_EXIT >> 16)),
                                    sb(Reg::R0, static_cast<s16>(ADDR_EXIT), Reg::AT), nop()};
        std::memcpy(&g_bus.ram[vector], handler.data(), sizeof(handler));
      }
      const u32 dcic = (1u << 23) | (1u << 24) | (3u << 29) | ((trap) ? (1u << 31) : 0u);
      CPU::g_state.cop0_regs.sr.bits = 0;
      CPU::g_state.cop0_regs.cause.bits = 0;
      CPU::g_state.cop0_regs.dcic.bits = dcic;
      CPU::g_state.cop0_regs.BPC = address;
      CPU::g_state.cop0_regs.BPCM = 0xFFFFFFFFu;
      CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)] = 0;
      CPU::UpdateDebugDispatcherFlag();
      EXPECT_TRUE(CPU::g_state.using_debug_dispatcher);

      Program prog;
      prog.LoadImmediate(Reg::T0, address);
      prog.Emit(jr(Reg::T0));
      prog.Emit(nop());
      prog.Run();

      // The null PIO device returns FFFFFFFF: fetching succeeds, then the instruction is reserved.
      EXPECT_EQ(CPU::g_state.cop0_regs.cause.Excode, (trap) ? CPU::Exception::BP : CPU::Exception::RI);
      EXPECT_EQ(CPU::g_state.cop0_regs.EPC, address);
      EXPECT_EQ(CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)], (trap) ? 0x40u : 0x80u);
      EXPECT_EQ(CPU::g_state.cop0_regs.dcic.bits, dcic | 3u);
      CPU::g_state.cop0_regs.dcic.bits = 0;
      CPU::UpdateDebugDispatcherFlag();
    }
  }
}

TEST_P(CPUCop0Breakpoints, FailedCacheFillPreservesPrefetchedExceptionHandler)
{
  for (const u32 segment : {0u, 0x80000000u})
  {
    for (const bool trap : {false, true})
    {
      const u32 vector = (trap) ? 0x80000040u : 0x80000080u;
      const u32 address = segment | 0x01000000u | (vector & 0xFFu);
      SCOPED_TRACE(::testing::Message() << "address=" << std::hex << address << " trap=" << trap);
      const u32 handler_instruction = addiu(Reg::V1, Reg::R0, 1);
      std::memcpy(&g_bus.ram[vector & g_bus.ram_mask], &handler_instruction, sizeof(handler_instruction));
      CPU::g_state.cop0_regs.sr.bits = 0;
      CPU::g_state.cop0_regs.dcic.bits = (1u << 23) | (1u << 24) | (3u << 29) | ((trap) ? (1u << 31) : 0u);
      CPU::g_state.cop0_regs.BPC = address;
      CPU::g_state.cop0_regs.BPCM = 0xFFFFFFFFu;
      CPU::UpdateDebugDispatcherFlag();
      CPU::ClearICache();

      u32 instruction;
      ASSERT_TRUE(CPU::FillICache<true>(vector, &instruction));
      ASSERT_EQ(instruction, handler_instruction);
      EXPECT_FALSE(CPU::FillICache<true>(address, &instruction));
      EXPECT_EQ(CPU::g_state.pc, vector);
      EXPECT_EQ(CPU::g_state.next_instruction.bits, handler_instruction);
      EXPECT_EQ(CPU::g_state.cop0_regs.cause.Excode, (trap) ? CPU::Exception::BP : CPU::Exception::IBE);
      CPU::g_state.cop0_regs.dcic.bits = 0;
      CPU::UpdateDebugDispatcherFlag();
    }
  }
}

TEST_P(CPUCop0Breakpoints, BPCAndBPCMWriteOrderDoesNotAffectDispatch)
{
  for (const bool mask_first : {false, true})
  {
    SCOPED_TRACE(::testing::Message() << "mask first=" << mask_first);
    const std::array handler = {addiu(Reg::V1, Reg::R0, 0x40), lui(Reg::AT, static_cast<u16>(ADDR_EXIT >> 16)),
                                sb(Reg::R0, static_cast<s16>(ADDR_EXIT), Reg::AT), nop()};
    std::memcpy(&g_bus.ram[0x40], handler.data(), sizeof(handler));
    CPU::g_state.cop0_regs.sr.bits = 0;
    CPU::g_state.cop0_regs.cause.bits = 0;
    CPU::g_state.cop0_regs.dcic.bits = (1u << 23) | (1u << 24) | (1u << 29) | (1u << 31);
    CPU::g_state.cop0_regs.BPC = 0x81000000u;
    CPU::g_state.cop0_regs.BPCM = 0xFFFFFFFFu;
    CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)] = 0;
    CPU::UpdateDebugDispatcherFlag();
    EXPECT_FALSE(CPU::g_state.using_debug_dispatcher);

    const u32 target = PROGRAM_BASE + 28;
    Program prog;
    prog.LoadImmediate(Reg::T0, 0xFFFFFFFFu);
    prog.LoadImmediate(Reg::T1, target);
    prog.Emit((mask_first) ? mtc0(Reg::T0, 11) : mtc0(Reg::T1, 3));
    prog.Emit((mask_first) ? mtc0(Reg::T1, 3) : mtc0(Reg::T0, 11));
    prog.Emit(addiu(Reg::V1, Reg::R0, 0x11));
    prog.Run();
    // Enabling the debug dispatcher exits execution; resume until the program or handler exits.
    for (u32 attempt = 0; attempt < 2 && CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)] == 0; attempt++)
      CPU::Execute();

    EXPECT_EQ(CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v1)], 0x40u);
    EXPECT_EQ(CPU::g_state.cop0_regs.EPC, target);
    EXPECT_EQ(CPU::g_state.cop0_regs.cause.Excode, CPU::Exception::BP);
    CPU::g_state.cop0_regs.dcic.bits = 0;
    CPU::UpdateDebugDispatcherFlag();
  }
}

INSTANTIATE_TEST_SUITE_P(Interpreter, CPUCop0Breakpoints, ::testing::Values(CPUExecutionMode::Interpreter),
                         GetExecutionModeTestName);

TEST_P(CPUInterruptTiming, StatusWriteAllowsFollowingInstructionBeforeInterrupt)
{
  for (const bool debug : {false, true})
  {
    for (const u32 base : {PROGRAM_BASE, PROGRAM_BASE | 0x20000000u})
    {
      for (const u32 irq : {0x100u, 0x200u, 0x400u})
      {
        for (const bool enable_ie : {false, true})
        {
          for (const u32 following : {0u, 1u, 2u})
          {
            SCOPED_TRACE(::testing::Message() << "debug=" << debug << " base=" << std::hex << base << " irq=" << irq
                                              << " enable IE=" << enable_ie << " following=" << following);
            g_settings.bios_tty_logging = debug;
            CPU::g_state.cop0_regs.sr.bits = (enable_ie) ? irq : 1u;
            CPU::g_state.cop0_regs.cause.bits = irq;
            CPU::g_state.cop0_regs.EPC = 0;
            CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v0)] = 0;
            CPU::g_state.regs.lo = 0x12345678u;
            CPU::g_state.muldiv_completion_tick = CPU::g_state.pending_ticks + 128;
            CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::t3)] = RAM_TEST_ADDR;
            const u32 loaded_value = 0x55667788u;
            std::memcpy(&g_bus.ram[RAM_TEST_ADDR & g_bus.ram_mask], &loaded_value, sizeof(loaded_value));
            CPU::UpdateDebugDispatcherFlag();
            EXPECT_EQ(CPU::g_state.using_debug_dispatcher, debug);
            // Exercise the boundary even when another event is already due at the SR write.
            s_event->SetIntervalAndSchedule(1);

            Program prog;
            prog.LoadImmediate(Reg::T0, irq | 1u);
            prog.Emit(mtc0(Reg::T0, 12));
            const std::array instructions = {addiu(Reg::V0, Reg::R0, 1), mflo(Reg::V0), lw(Reg::V0, 0, Reg::T3)};
            prog.Emit(instructions[following]);
            prog.Emit(divu(Reg::R0, Reg::R0));
            prog.Emit(nop());
            prog.RunAt(base);

            const std::array expected = {1u, 0x12345678u, loaded_value};
            EXPECT_EQ(CPU::g_state.cop0_regs.EPC, base + 20);
            EXPECT_EQ(CPU::g_state.cop0_regs.cause.bits, irq);
            EXPECT_EQ(CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v0)], expected[following]);
            EXPECT_EQ(CPU::g_state.regs.lo, 0x12345678u);
            CPU::g_state.cop0_regs.sr.bits = 0;
            CPU::g_state.cop0_regs.cause.bits = 0;
          }
        }
      }
    }
  }
}

TEST_P(CPUInterruptTiming, CauseWriteInterruptRemainsImmediate)
{
  for (const bool debug : {false, true})
  {
    for (const u32 base : {PROGRAM_BASE, PROGRAM_BASE | 0x20000000u})
    {
      for (const u32 irq : {0x100u, 0x200u})
      {
        g_settings.bios_tty_logging = debug;
        CPU::g_state.cop0_regs.sr.bits = irq | 1u;
        CPU::g_state.cop0_regs.cause.bits = 0;
        CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v0)] = 0;
        CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::t2)] = 1;
        CPU::UpdateDebugDispatcherFlag();

        Program prog;
        prog.LoadImmediate(Reg::T0, irq);
        prog.Emit(mtc0(Reg::T0, 13));
        prog.Emit(addu(Reg::V0, Reg::T2, Reg::R0));
        prog.RunAt(base);

        EXPECT_EQ(CPU::g_state.cop0_regs.EPC, base + 16);
        EXPECT_EQ(CPU::g_state.cop0_regs.cause.bits, irq);
        EXPECT_EQ(CPU::g_state.regs.r[static_cast<u32>(CPU::Reg::v0)], 0u);
        CPU::g_state.cop0_regs.sr.bits = 0;
        CPU::g_state.cop0_regs.cause.bits = 0;
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Interpreter, CPUInterruptTiming, ::testing::Values(CPUExecutionMode::Interpreter),
                         GetExecutionModeTestName);

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
