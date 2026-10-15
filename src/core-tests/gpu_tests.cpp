// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <array>

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

TEST_F(GPUFrontend, ProgressiveSettingUpdatesInterlacedDisplayHeight)
{
  Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive;
  g_settings.display_crop_mode = DisplayCropMode::None;
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x07040010); // Display lines 16 through 255.
  GPU::WriteRegister(4, 0x08000020); // Interlace enabled, 240-line framebuffer.
  EXPECT_EQ(GPU::GetCRTCVideoSize().y, 240);

  old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Adaptive;
  GPU::UpdateSettings(old_settings);
  EXPECT_EQ(GPU::GetCRTCVideoSize().y, 480);

  old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive;
  GPU::UpdateSettings(old_settings);
  EXPECT_EQ(GPU::GetCRTCVideoSize().y, 240);

  GPU::WriteRegister(4, 0x08000024); // Interleaved 480i still has a 480-line framebuffer.
  EXPECT_EQ(GPU::GetCRTCVideoSize().y, 480);
}

TEST(GPUDisplay, FineCropUsesOriginalSourceScale)
{
  GSVector4i source, display, draw;
  GPU::CalculateDrawRect(GSVector2i(576, 480), GSVector2i(640, 480), GSVector4i(0, 0, 640, 480),
                         GSVector4i(100, 10, 740, 490), DisplayRotation::Normal, DisplayAlignment::Center, 1.0f, false,
                         DisplayFineCropMode::VideoResolution, std::array<s16, 4>{64, 0, 0, 0}, &source, &display,
                         &draw);
  EXPECT_TRUE(source.eq(GSVector4i(164, 10, 740, 490)));
  EXPECT_TRUE(display.eq(GSVector4i(0, 0, 576, 480)));
  EXPECT_TRUE(draw.eq(display));
}

TEST(GPUDisplay, FineCropRemovesPaddingBeforeSourcePixels)
{
  GSVector4i source, display, draw;
  GPU::CalculateDrawRect(GSVector2i(300, 240), GSVector2i(320, 240), GSVector4i(16, 0, 304, 240),
                         GSVector4i(0, 0, 576, 480), DisplayRotation::Normal, DisplayAlignment::Center, 1.0f, false,
                         DisplayFineCropMode::VideoResolution, std::array<s16, 4>{20, 0, 0, 0}, &source, &display,
                         &draw);
  EXPECT_TRUE(source.eq(GSVector4i(8, 0, 576, 480)));
  EXPECT_TRUE(display.eq(GSVector4i(0, 0, 300, 240)));
  EXPECT_TRUE(draw.eq(GSVector4i(0, 0, 284, 240)));
}

TEST(GPUDisplay, ExcessiveFineCropProducesEmptyBoundedRectangles)
{
  for (const auto amount : {std::array<s16, 4>{640, 480, 0, 0}, std::array<s16, 4>{1000, 1000, 1000, 1000}})
  {
    GSVector4i source, display, draw;
    GPU::CalculateDrawRect(GSVector2i(640, 480), GSVector2i(640, 480), GSVector4i(0, 0, 640, 480),
                           GSVector4i(100, 10, 740, 490), DisplayRotation::Normal, DisplayAlignment::Center, 1.0f,
                           false, DisplayFineCropMode::VideoResolution, amount, &source, &display, &draw);
    EXPECT_TRUE(source.eq(GSVector4i(740, 490, 740, 490)));
    EXPECT_EQ(draw.width(), 0);
    EXPECT_EQ(draw.height(), 0);
    EXPECT_GT(display.width(), 0);
    EXPECT_GT(display.height(), 0);
  }
}
