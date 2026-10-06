// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/bus.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/mips_encoder.h"
#include "core/settings.h"
#include "core/timing_event.h"

#include "common/error.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace Mips::Encoder;

// pcsx_debugbreak(), used to get out of the execution loop.
static constexpr u32 ADDR_EXIT = 0x1F802081u;

static constexpr u32 PROGRAM_BASE = 0x80010000u;
static constexpr u32 PADDING_NOPS = 8;

static constexpr u32 CFC2_FLAG = 0x4849F800u; // cfc2 $t1, $31
static constexpr u32 MFC2_MAC1 = 0x4809C800u; // mfc2 $t1, $25

struct GTECommand
{
  const char* name;
  u32 bits;
  u32 cycles;
};
static constexpr GTECommand s_commands[] = {
  {"SQR", 0x4AA00428u, 5},
  {"AVSZ4", 0x4B68002Eu, 6},
  {"NCLIP", 0x4B400006u, 8},
  {"RTPS", 0x4A180001u, 15},
};

class GTETiming : public ::testing::TestWithParam<CPUExecutionMode>
{
protected:
  static void SetUpTestSuite()
  {
    g_settings.cpu_execution_mode = CPUExecutionMode::Interpreter;
    g_settings.pcsx_expansion_region_enable = true;

    Error error;
    ASSERT_TRUE(CPU::CodeCache::ProcessStartup(&error)) << error.GetDescription();
    ASSERT_TRUE(Bus::AllocateMemory(false, &error)) << error.GetDescription();
    Bus::Initialize();
    CPU::Initialize();
    TimingEvents::Initialize();
    Bus::Reset();
    CPU::Reset();

    // The CPU needs something to count down to.
    s_event = std::make_unique<TimingEvent>("Test Event", 1000000, 1000000, [](void*, TickCount) {}, nullptr);
    s_event->Activate();
  }

  static void TearDownTestSuite()
  {
    s_event->Deactivate();
    s_event.reset();
    CPU::CodeCache::Shutdown();
    CPU::Shutdown();
    Bus::Shutdown();
    Bus::ReleaseMemory();
    CPU::CodeCache::ProcessShutdown();
  }

  void SetUp() override
  {
    g_settings.cpu_execution_mode = GetParam();
    CPU::g_state.cop0_regs.sr.CE2 = true;
  }

  static u32 GetTicks() { return static_cast<u32>(TimingEvents::GetGlobalTickCounter()) + CPU::GetPendingTicks(); }

  /// Runs body repeated the specified number of times, returns the number of ticks taken.
  static u32 Run(const std::vector<u32>& body, u32 reps)
  {
    std::vector<u32> code;
    code.push_back(lui(Reg::AT, static_cast<u16>(ADDR_EXIT >> 16)));

    // Padding ensures nothing is in flight coming in to or out of the body.
    code.insert(code.end(), PADDING_NOPS, nop());
    for (u32 i = 0; i < reps; i++)
      code.insert(code.end(), body.begin(), body.end());
    code.insert(code.end(), PADDING_NOPS, nop());

    code.push_back(sb(Reg::R0, static_cast<s16>(ADDR_EXIT & 0xFFFFu), Reg::AT));
    code.push_back(nop());
    code.push_back(nop());

    // Has to fit in the icache, otherwise we're measuring refills.
    EXPECT_LE(code.size() * sizeof(u32), static_cast<size_t>(CPU::ICACHE_SIZE));
    std::memcpy(&g_bus.unprotected_ram[PROGRAM_BASE & g_bus.ram_mask], code.data(), code.size() * sizeof(u32));
    CPU::ClearICache();
    if (!CPU::g_state.using_interpreter)
      CPU::CodeCache::Reset();

    // First run warms up the icache.
    u32 ticks = 0;
    for (u32 i = 0; i < 2; i++)
    {
      CPU::SetPC(PROGRAM_BASE);
      ticks = GetTicks();
      CPU::Execute();
      ticks = GetTicks() - ticks;
    }

    return ticks;
  }

  /// Returns the number of ticks that each repetition of body takes.
  static u32 TicksPerIteration(const std::vector<u32>& body)
  {
    static constexpr u32 REPS = 16;
    const u32 ticks = Run(body, REPS * 2) - Run(body, REPS);
    EXPECT_EQ(ticks % REPS, 0u);
    return ticks / REPS;
  }

  /// From psxtest_gte: reading a GTE register or issuing another command stalls for what is left of the command,
  /// except when only a single cycle is left, in which case there is no stall at all.
  static u32 ExpectedStall(u32 cycles, u32 instructions_between)
  {
    const u32 remaining = (cycles > instructions_between) ? (cycles - instructions_between) : 0;
    return (remaining > 1) ? remaining : 0;
  }

  static inline std::unique_ptr<TimingEvent> s_event;
};

} // namespace

TEST_P(GTETiming, StallOnRead)
{
  for (const GTECommand& cmd : s_commands)
  {
    for (const u32 nops : {0u, 1u, 4u, 7u})
    {
      for (const u32 read : {CFC2_FLAG, MFC2_MAC1})
      {
        std::vector<u32> body = {cmd.bits};
        body.insert(body.end(), nops, nop());
        body.push_back(read);
        body.insert(body.end(), 2, nop());
        EXPECT_EQ(TicksPerIteration(body), static_cast<u32>(body.size()) + ExpectedStall(cmd.cycles, nops))
          << cmd.name << " read after " << nops << " nops";
      }
    }
  }
}

TEST_P(GTETiming, StallOnNextCommand)
{
  for (const GTECommand& cmd : s_commands)
  {
    for (const u32 nops : {0u, 1u, 4u, 7u})
    {
      std::vector<u32> body = {cmd.bits};
      body.insert(body.end(), nops, nop());
      EXPECT_EQ(TicksPerIteration(body), static_cast<u32>(body.size()) + ExpectedStall(cmd.cycles, nops))
        << cmd.name << " repeated after " << nops << " nops";
    }
  }
}

// Same thing, but with the command and the read in different blocks.
TEST_P(GTETiming, StallAcrossBranch)
{
  for (const GTECommand& cmd : s_commands)
  {
    for (const u32 nops : {0u, 2u, 5u})
    {
      std::vector<u32> body = {cmd.bits};
      body.insert(body.end(), nops, nop());
      body.push_back(beq(Reg::R0, Reg::R0, 4));
      body.push_back(nop());
      body.push_back(MFC2_MAC1);
      body.insert(body.end(), 2, nop());
      EXPECT_EQ(TicksPerIteration(body), static_cast<u32>(body.size()) + ExpectedStall(cmd.cycles, nops + 2))
        << cmd.name << " read across branch after " << nops << " nops";
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, GTETiming,
                         ::testing::Values(CPUExecutionMode::Interpreter, CPUExecutionMode::CachedInterpreter,
                                           CPUExecutionMode::Recompiler),
                         [](const ::testing::TestParamInfo<CPUExecutionMode>& info) {
                           return std::string(Settings::GetCPUExecutionModeName(info.param));
                         });
