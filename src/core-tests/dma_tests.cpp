// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cpu_test_harness.h"

#include "core/dma.h"
#include "core/spu.h"

namespace {

using namespace CPUTestHarness;
using namespace Mips::Encoder;

class DMAExecution : public ExecutionModeTest
{
};

} // namespace

TEST_P(DMAExecution, ChoppedManualDMARunsWhileGPUTransferIsSliced)
{
  const Settings old_settings = g_settings;
  g_settings.dma_max_slice_ticks = 32;
  g_settings.dma_halt_ticks = 16;
  DMA::Reset();

  // An empty GPU linked list that loops back to itself keeps exhausting its DMA slice without sending GPU commands.
  const u32 node_address = RAM_TEST_ADDR & g_bus.ram_mask;
  std::memcpy(&g_bus.ram[node_address], &node_address, sizeof(node_address));
  DMA::WriteRegister(0x70, 0);
  DMA::WriteRegister(0x20, node_address);
  DMA::WriteRegister(0x28, 0x01000401);
  DMA::SetRequest(DMA::Channel::GPU, true);
  DMA::SetRequest(DMA::Channel::SPU, true);
  DMA::WriteRegister(0x70, 0x00080800);

  // A chopped manual transfer is delayed by halting DMA. When the halt ends, the GPU runs first and halts again, which
  // must not keep the manual transfer waiting until the linked list ends (CDROM reads in Tondemo Crisis).
  DMA::WriteRegister(0x40, node_address + 0x1000);
  DMA::WriteRegister(0x44, 16);
  DMA::WriteRegister(0x48, 0x01100101);
  EXPECT_NE(DMA::ReadRegister(0x48) & (1u << 24), 0u);

  Program prog;
  prog.LoadImmediate(Reg::T0, 128);
  prog.Emit(addiu(Reg::T0, Reg::T0, -1));
  prog.Emit(bne(Reg::T0, Reg::R0, -8));
  prog.Emit(nop());
  prog.Run();

  EXPECT_NE(DMA::ReadRegister(0x28) & (1u << 24), 0u);
  EXPECT_EQ(DMA::ReadRegister(0x48) & (1u << 24), 0u);

  DMA::Reset();
  SPU::Reset();
  g_settings = old_settings;
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, DMAExecution, ::testing::ValuesIn(EXECUTION_MODES), GetExecutionModeTestName);
