// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Helpers for running MIPS programs through each CPU execution mode.

#pragma once

#include "core/bus.h"
#include "core/mips_encoder.h"
#include "core/settings.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace CPUTestHarness {

// pcsx_debugbreak(), used to get out of the execution loop.
inline constexpr u32 ADDR_EXIT = 0x1F802081u;

inline constexpr u32 PROGRAM_BASE = 0x80010000u;
inline constexpr u32 RESULTS_BASE = 0x80100000u;
inline constexpr u32 RAM_TEST_ADDR = 0x80180000u;

/// Builds a program, results are stored to RAM because the recompiler doesn't write registers back when exiting.
class Program
{
public:
  void Emit(u32 inst) { m_code.push_back(inst); }

  void LoadImmediate(Mips::Encoder::Reg reg, u32 value);

  /// Stores the register to the next result slot, and returns the slot.
  u32 StoreResult(Mips::Encoder::Reg reg);

  /// Executes the program the specified number of times, the first run warms up the icache.
  void Run(u32 runs = 1);

  u32 Result(u32 slot) const { return ReadRAM(RESULTS_BASE + slot * sizeof(u32)); }

  static u32 ReadRAM(u32 address)
  {
    u32 value;
    std::memcpy(&value, &g_bus.ram[address & g_bus.ram_mask], sizeof(value));
    return value;
  }

private:
  std::vector<u32> m_code;
  u32 m_num_results = 0;
};

/// Brings up the CPU and the hardware it talks to once per suite, each test runs in the parameter's execution mode.
class ExecutionModeTest : public ::testing::TestWithParam<CPUExecutionMode>
{
protected:
  static void SetUpTestSuite();
  static void TearDownTestSuite();

  void SetUp() override
  {
    // The switch itself happens when the CPU is next executed.
    g_settings.cpu_execution_mode = GetParam();
  }

  static inline std::unique_ptr<TimingEvent> s_event;
};

inline constexpr CPUExecutionMode EXECUTION_MODES[] = {
  CPUExecutionMode::Interpreter, CPUExecutionMode::CachedInterpreter, CPUExecutionMode::Recompiler};

inline std::string GetExecutionModeTestName(const ::testing::TestParamInfo<CPUExecutionMode>& info)
{
  return std::string(Settings::GetCPUExecutionModeName(info.param));
}

} // namespace CPUTestHarness
