// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <algorithm>
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

TEST_F(GPUFrontend, BeamCoordinatesScaleTheDisplayOriginWithOverclock)
{
  for (const GSVector2 position : {GSVector2(0.0f, 0.0f), GSVector2(64.0f, 100.0f)})
  {
    g_settings.cpu_overclock_active = false;
    GPU::CPUClockChanged();
    u32 native_tick, native_line;
    ASSERT_TRUE(GPU::ConvertDisplayCoordinatesToBeamTicksAndLines(position, 1.0f, &native_tick, &native_line));

    g_settings.cpu_overclock_active = true;
    g_settings.cpu_overclock_numerator = 2;
    g_settings.cpu_overclock_denominator = 1;
    GPU::CPUClockChanged();
    u32 overclocked_tick, overclocked_line;
    ASSERT_TRUE(
      GPU::ConvertDisplayCoordinatesToBeamTicksAndLines(position, 1.0f, &overclocked_tick, &overclocked_line));
    EXPECT_EQ(overclocked_tick, native_tick * 2);
    EXPECT_EQ(overclocked_line, native_line);
  }
}

TEST_F(GPUFrontend, ProgressiveInterlaceUsesOneBeamLinePerDisplayLine)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive;
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x08000020);

  u32 tick, line, first_line;
  ASSERT_TRUE(GPU::ConvertDisplayCoordinatesToBeamTicksAndLines(GSVector2(0.0f, 0.0f), 1.0f, &tick, &first_line));
  ASSERT_TRUE(GPU::ConvertDisplayCoordinatesToBeamTicksAndLines(GSVector2(0.0f, 100.0f), 1.0f, &tick, &line));
  EXPECT_EQ(line, first_line + 100);
}

TEST_F(GPUFrontend, V2InternalRegisterQueries)
{
  GPU::WriteRegister(4, 0x10000007);
  EXPECT_EQ(GPU::ReadRegister(0), 2u);
  GPU::WriteRegister(4, 0x10000017);
  EXPECT_EQ(GPU::ReadRegister(0), 2u);

  GPU::WriteRegister(0, 0xE2001234);
  GPU::WriteRegister(4, 0x10000002);
  ASSERT_EQ(GPU::ReadRegister(0), 0x1234u);
  GPU::WriteRegister(4, 0x1000000A); // Unsupported v2 indices do not mirror 0..7.
  EXPECT_EQ(GPU::ReadRegister(0), 0x1234u);
  GPU::WriteRegister(4, 0x10000008);
  EXPECT_EQ(GPU::ReadRegister(0), 0u);
}

TEST_F(GPUFrontend, V2StatusReportsFieldAndUpperTextureAddress)
{
  EXPECT_NE(GPU::ReadRegister(4) & (1u << 13), 0u);
  GPU::WriteRegister(0, 0xE1000800);
  EXPECT_NE(GPU::ReadRegister(4) & (1u << 15), 0u);
  GPU::WriteRegister(4, 0x09000001);
  EXPECT_NE(GPU::ReadRegister(4) & (1u << 15), 0u);
  GPU::WriteRegister(4, 0x09000000);
  EXPECT_NE(GPU::ReadRegister(4) & (1u << 15), 0u);
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

TEST(GPURasterizer, V2RectangleFlipsWrapBeforeApplyingTextureWindow)
{
  GPU_SW_Rasterizer::SelectImplementation();
  const GPUDrawingArea old_area = GPU_SW_Rasterizer::g_drawing_area;
  GPU_SW_Rasterizer::g_drawing_area = {515, 301, 808, 303};

  for (u32 i = 0; i < 8; i++)
  {
    const u16 flip_bits = static_cast<u16>((i & 3u) << 12);
    std::fill_n(g_vram, VRAM_WIDTH * VRAM_HEIGHT, 0);
    for (u32 y = 0; y < 256; y++)
    {
      for (u32 x = 0; x < 256; x++)
        g_vram[y * VRAM_WIDTH + x] = static_cast<u16>(1 + ((y * 256 + x) % 32767));
    }

    GPUBackendDrawRectangleCommand cmd = {};
    cmd.texture_enable = true;
    cmd.raw_texture_enable = true;
    cmd.draw_mode.bits = flip_bits;
    cmd.draw_mode.texture_mode = GPUTextureMode::Direct16Bit;
    if (i & 4u)
      cmd.window = {.and_x = 0xFF, .and_y = 0xFF, .or_x = 0x00, .or_y = 0x00};
    else
      cmd.window = {.and_x = 0xEF, .and_y = 0xF7, .or_x = 0x10, .or_y = 0x08};
    cmd.x = 512;
    cmd.y = 300;
    cmd.width = 300;
    cmd.height = 5;
    cmd.texcoord = 0x0101;

    GPU_SW_Rasterizer::GetDrawRectangleFunction(GPU_SW_Rasterizer::TextureModulationMode::NoModulation, false)(&cmd);
    for (u32 y = 0; y < cmd.height; y++)
    {
      for (u32 x = 0; x < cmd.width; x++)
      {
        u16 expected = 0;
        if ((cmd.x + x) >= 515 && (cmd.x + x) <= 808 && (cmd.y + y) >= 301 && (cmd.y + y) <= 303)
        {
          const u32 u = ((((flip_bits & (1u << 12)) ? (1 - x) : (1 + x)) & 0xFF) & cmd.window.and_x) | cmd.window.or_x;
          const u32 v = ((((flip_bits & (1u << 13)) ? (1 - y) : (1 + y)) & 0xFF) & cmd.window.and_y) | cmd.window.or_y;
          expected = g_vram[v * VRAM_WIDTH + u];
        }
        EXPECT_EQ(g_vram[(cmd.y + y) * VRAM_WIDTH + cmd.x + x], expected) << "i=" << i << ", x=" << x << ", y=" << y;
      }
    }
  }
  GPU_SW_Rasterizer::g_drawing_area = old_area;
}
