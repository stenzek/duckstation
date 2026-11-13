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
  g_settings.dma_max_slice_cycles = 32;
  g_settings.dma_halt_cycles = 16;
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

TEST_P(DMAExecution, SPURequestDMARunsWhileGPUTransferIsSliced)
{
  const Settings old_settings = g_settings;
  g_settings.dma_max_slice_cycles = 32;
  g_settings.dma_halt_cycles = 16;
  DMA::Reset();
  SPU::Reset();

  // Keep the GPU channel busy with an empty linked list which loops back to itself.
  const u32 node_address = RAM_TEST_ADDR & g_bus.ram_mask;
  std::memcpy(&g_bus.ram[node_address], &node_address, sizeof(node_address));
  DMA::WriteRegister(0x70, 0);
  DMA::WriteRegister(0x20, node_address);
  DMA::WriteRegister(0x28, 0x01000401);
  DMA::SetRequest(DMA::Channel::GPU, true);
  DMA::WriteRegister(0x70, 0x00080800);
  DMA::WriteRegister(0x74, 0x00900000);

  // The first block fills the SPU FIFO immediately. Subsequent blocks must be allowed to run when the FIFO drains,
  // even though the GPU keeps exhausting its slice. Otherwise the game's sound transfer wait never completes.
  SPU::WriteRegister(0x1F801DAA - 0x1F801C00, 0xC020);
  DMA::WriteRegister(0x40, node_address + 0x1000);
  DMA::WriteRegister(0x44, 0x00040010);
  DMA::WriteRegister(0x48, 0x01000201);
  EXPECT_NE(DMA::ReadRegister(0x48) & (1u << 24), 0u);
  EXPECT_EQ(DMA::ReadRegister(0x44) >> 16, 3u);

  Program prog;
  prog.LoadImmediate(Reg::T0, 4096);
  prog.Emit(addiu(Reg::T0, Reg::T0, -1));
  prog.Emit(bne(Reg::T0, Reg::R0, -8));
  prog.Emit(nop());
  prog.Run();

  EXPECT_NE(DMA::ReadRegister(0x28) & (1u << 24), 0u);
  EXPECT_EQ(DMA::ReadRegister(0x48) & (1u << 24), 0u);
  EXPECT_EQ(DMA::ReadRegister(0x44) >> 16, 0u);
  EXPECT_NE(DMA::ReadRegister(0x74) & (1u << 28), 0u);

  DMA::Reset();
  SPU::Reset();
  g_settings = old_settings;
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes, DMAExecution, ::testing::ValuesIn(EXECUTION_MODES), GetExecutionModeTestName);
