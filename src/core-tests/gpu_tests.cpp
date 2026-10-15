// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

namespace {

class GPUFrontend : public testing::Test
{
protected:
  void SetUp() override
  {
    m_old_settings = g_settings;
    g_settings = Settings{};
    g_settings.gpu_automatic_resolution_scale = false;
    CPU::g_state.pending_ticks = 0;
    TimingEvents::Reset();
    InterruptController::Reset();
    GPU::Initialize();
    GPU::Reset(false);
  }

  void TearDown() override
  {
    GPU::Shutdown();
    g_settings = m_old_settings;
  }

  Settings m_old_settings;
};

} // namespace

TEST_F(GPUFrontend, ResetAcknowledgesInterrupt)
{
  GPU::WriteRegister(0, 0x1F000000);
  ASSERT_NE(GPU::ReadRegister(4) & (1u << 24), 0u);
  ASSERT_NE(InterruptController::ReadRegister(0) & (1u << 1), 0u);

  GPU::WriteRegister(4, 0x00000000);
  EXPECT_EQ(GPU::ReadRegister(4) & (1u << 24), 0u);

  // Reset must lower the GPU's interrupt line so the next request produces another rising edge.
  InterruptController::WriteRegister(0, ~(1u << 1));
  GPU::WriteRegister(0, 0x1F000000);
  EXPECT_NE(InterruptController::ReadRegister(0) & (1u << 1), 0u);
}

TEST_F(GPUFrontend, CommandBufferResetPreservesInterrupt)
{
  GPU::WriteRegister(0, 0x1F000000);
  GPU::WriteRegister(4, 0x01000000);
  EXPECT_NE(GPU::ReadRegister(4) & (1u << 24), 0u);
}
