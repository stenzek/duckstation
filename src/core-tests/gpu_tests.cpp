// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/gpu.h"
#include "core/gpu_sw_rasterizer.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/timers.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <tuple>
#include <vector>

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
    Timers::Initialize();
    GPU::Initialize();
    GPU::Reset(false);
  }

  void TearDown() override
  {
    // Other suites see the GPU's static state, so put the beam back at the start of the frame.
    CPU::g_state.pending_ticks = 0;
    GPU::Reset(false);
    GPU::Shutdown();
    Timers::Shutdown();
    CPU::g_state.pending_ticks = 0;
    g_settings = m_old_settings;
  }

  static TimingEvent* FindEvent(std::string_view name)
  {
    for (TimingEvent* event = *TimingEvents::GetHeadEventPtr(); event; event = event->next)
    {
      if (name == event->GetName())
        return event;
    }
    return nullptr;
  }

  // CPU ticks until the GPU clock reaches the given number of quarter ticks, rounded up.
  static TickCount SystemTicksForGPUQuarterTicks(u64 quarter_ticks)
  {
    const u64 frequency = static_cast<u64>(GPU::GetCRTCFrequency()) * 4;
    return static_cast<TickCount>((quarter_ticks * System::MASTER_CLOCK + frequency - 1) / frequency);
  }

  Settings m_old_settings;
};

} // namespace

TEST_F(GPUFrontend, VBlankEndCheatsOnlyScheduleWhenEnabled)
{
  TimingEvent* const crtc_event = FindEvent("GPU CRTC Tick");
  ASSERT_NE(crtc_event, nullptr);
  const TickCount normal_ticks = crtc_event->GetTicksUntilNextExecution();

  // These tests initialize the GPU without a running system, so explicitly update its schedule.
  GPU::SetVBlankEndCheatsEnabled(true);
  GPU::RescheduleCRTC();
  EXPECT_LT(crtc_event->GetTicksUntilNextExecution(), normal_ticks / 8);

  GPU::SetVBlankEndCheatsEnabled(false);
  GPU::RescheduleCRTC();
  EXPECT_EQ(crtc_event->GetTicksUntilNextExecution(), normal_ticks);
}

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

// Line 0 is in vblank, so timer 1 pausing during vblank doesn't count until the display starts.
TEST_F(GPUFrontend, Timer1PausedDuringInitialVBlank)
{
  Timers::WriteRegister(0x14, 0x0001); // sync enable, pause during vblank
  Timers::WriteRegister(0x10, 0);
  CPU::AddPendingTicks(100);
  EXPECT_EQ(Timers::ReadRegister(0x10), 0u);
}

// With Y2 moved before Y1 in vblank, the CRTC still has to run when the scan reaches Y1 for timer 1's gate.
TEST_F(GPUFrontend, ReversedRangeEndsVBlankAtY1)
{
  const auto run = [](u32 ticks) {
    for (u32 i = 0; i < ticks; i++)
    {
      CPU::AddPendingTicks(1);
      if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
        TimingEvents::RunEvents();
    }
  };

  // Count lines up to line 150 while the display is at 200..256, so it stays in vblank.
  GPU::WriteRegister(4, 0x07000000 | 200 | (256 << 10));
  Timers::WriteRegister(0x14, 0x0100); // hblank clock
  u32 ticks = 0;
  while (Timers::ReadRegister(0x10) < 150)
  {
    run(100);
    ticks += 100;
  }
  const u32 ticks_per_line = ticks / 150;

  // Move Y2 to 100, then count hblanks outside vblank for 60 lines, with nothing synchronizing the CRTC.
  GPU::WriteRegister(4, 0x07000000 | 200 | (100 << 10));
  Timers::WriteRegister(0x14, 0x0101); // hblank clock, pause during vblank
  run(ticks_per_line * 60);
  EXPECT_NEAR(Timers::ReadRegister(0x10), 10, 1);
}

TEST_F(GPUFrontend, LineParityBitClearDuringVBlank)
{
  const auto run = [](u32 ticks) {
    for (u32 i = 0; i < ticks; i++)
    {
      CPU::AddPendingTicks(1);
      if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
        TimingEvents::RunEvents();
    }
  };

  // 240p, starting at line 0 with the display at lines 16..256. Count lines with timer 1.
  Timers::WriteRegister(0x14, 0x0100); // hblank clock

  // Lines 0..15 are vblank, so GPUSTAT.31 stays clear.
  u32 set_reads = 0;
  while (Timers::ReadRegister(0x10) < 15)
  {
    set_reads += GPU::ReadRegister(4) >> 31;
    run(100);
  }
  EXPECT_EQ(set_reads, 0u);

  // It toggles per line once the display starts.
  while (Timers::ReadRegister(0x10) < 40)
  {
    set_reads += GPU::ReadRegister(4) >> 31;
    run(100);
  }
  EXPECT_GT(set_reads, 0u);
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

TEST_F(GPUFrontend, GP1HighCommandBitsAreDecoded)
{
  constexpr u32 DISPLAY_DISABLE = 1u << 23;

  GPU::WriteRegister(4, 0x03000000); // display on
  for (const u32 v : {0x23000001u, 0x43000001u, 0x83000001u, 0xC3000001u})
  {
    GPU::WriteRegister(4, v);
    EXPECT_EQ(GPU::ReadRegister(4) & DISPLAY_DISABLE, 0u) << std::hex << v;
  }

  // GameShark Sampler Disc writes this before every glyph, it must not reset.
  GPU::WriteRegister(0, 0xE10006A5);
  for (const u32 v : {0x40000000u, 0x80000000u, 0xC0000000u})
  {
    GPU::WriteRegister(4, v);
    EXPECT_EQ(GPU::ReadRegister(4) & DISPLAY_DISABLE, 0u) << std::hex << v;
    EXPECT_EQ(GPU::ReadRegister(4) & 0x7FFu, 0x6A5u) << std::hex << v;
  }

  GPU::WriteRegister(4, 0x03000001);
  EXPECT_NE(GPU::ReadRegister(4) & DISPLAY_DISABLE, 0u);
}

TEST_F(GPUFrontend, GP1InfoIgnoresBits30And31)
{
  GPU::WriteRegister(0, 0xE3001234);
  GPU::WriteRegister(0, 0xE4005678);
  for (const u32 v : {0x50000004u, 0x90000004u, 0xD0000004u})
  {
    GPU::WriteRegister(4, 0x10000003);
    ASSERT_EQ(GPU::ReadRegister(0), 0x1234u);
    GPU::WriteRegister(4, v);
    EXPECT_EQ(GPU::ReadRegister(0), 0x5678u) << std::hex << v;
  }
  GPU::WriteRegister(4, 0x10000003);
  GPU::WriteRegister(4, 0x30000004); // bit 29 is decoded: no read
  EXPECT_EQ(GPU::ReadRegister(0), 0x1234u);
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

TEST_F(GPUFrontend, BeamWaitAccountsForPendingFractionalPhase)
{
  GPU::WriteRegister(4, 0x070FFC00); // Keep the display range open while synchronizing.
  const TickCount target_time = SystemTicksForGPUQuarterTicks(200 * 13650 + 123 * 4);
  for (const TickCount pending_ticks : {1, 17, 100, 12345})
  {
    SCOPED_TRACE(pending_ticks);
    CPU::g_state.pending_ticks = pending_ticks;
    EXPECT_EQ(GPU::GetSystemTicksUntilTicksAndLine(123, 200), target_time - pending_ticks);
    GPU::SynchronizeCRTC();
    EXPECT_EQ(GPU::GetSystemTicksUntilTicksAndLine(123, 200), target_time - pending_ticks);
  }
}

TEST_F(GPUFrontend, NTSCScanlinesDoNotDriftAcrossSynchronizations)
{
  GPU::WriteRegister(4, 0x070FFC00);
  for (u32 expected_line = 1; expected_line <= 262; expected_line++)
  {
    SCOPED_TRACE(expected_line);
    CPU::g_state.pending_ticks = SystemTicksForGPUQuarterTicks(expected_line * 13650);
    u32 tick, line;
    GPU::GetBeamPosition(&tick, &line);
    EXPECT_EQ(line, expected_line);
    EXPECT_LE(tick, 1u);

    GPU::SynchronizeCRTC();
    u32 synchronized_tick, synchronized_line;
    GPU::GetBeamPosition(&synchronized_tick, &synchronized_line);
    EXPECT_EQ(synchronized_tick, tick);
    EXPECT_EQ(synchronized_line, line);
  }
}

// Timer 0 only counts the dots completed in each line: 3412.5 / 10 is 341 per line in NTSC, 3405 / 10 is 340 in PAL.
// PAL 320 is the exception, 3405 / 8 counts 426 like NTSC's 3412.5 / 8.
TEST_F(GPUFrontend, TimersCountDotsAndHBlanksAcrossFractionalScanlines)
{
  for (const auto [mode, line_quarter_ticks, dots_per_line, half_line_dots] :
       {std::tuple<u32, u64, u32, u32>{0x00, 13650, 341, 170},
        {0x08, 13620, 340, 170},
        {0x01, 13650, 426, 213},
        {0x09, 13620, 426, 213}})
  {
    SCOPED_TRACE(mode);
    CPU::g_state.pending_ticks = 0;
    GPU::Reset(false);
    GPU::WriteRegister(4, 0x08000000 | mode);
    GPU::WriteRegister(4, 0x070FFC00);
    Timers::WriteRegister(0x04, 1u << 8); // Timer 0 counts dots.
    Timers::WriteRegister(0x14, 1u << 8); // Timer 1 counts HBlanks.

    // Synchronize on every line, and halfway through it, so updates carry partial dots across the line end.
    for (u32 expected_line = 1; expected_line <= 100; expected_line++)
    {
      SCOPED_TRACE(expected_line);
      CPU::g_state.pending_ticks = SystemTicksForGPUQuarterTicks((expected_line - 1) * line_quarter_ticks + 1705 * 4);
      EXPECT_EQ(Timers::ReadRegister(0x00), (expected_line - 1) * dots_per_line + half_line_dots);
      CPU::g_state.pending_ticks = SystemTicksForGPUQuarterTicks(expected_line * line_quarter_ticks);
      EXPECT_EQ(Timers::ReadRegister(0x00), expected_line * dots_per_line);
      EXPECT_EQ(Timers::ReadRegister(0x10), expected_line);
    }
  }
}

// The CRTC wakes up for a Timer 0 IRQ on a later line, skipping the partial dot at the end of each line.
TEST_F(GPUFrontend, DotTimerIRQSkipsThePartialDotAtTheEndOfTheLine)
{
  GPU::WriteRegister(4, 0x070FFC00);
  CPU::g_state.pending_ticks = SystemTicksForGPUQuarterTicks(100 * 4);
  Timers::WriteRegister(0x08, 1000);                              // target
  Timers::WriteRegister(0x04, (1u << 8) | (1u << 4) | (1u << 3)); // dot clock, IRQ and reset at target
  ASSERT_EQ(Timers::ReadRegister(0x00), 0u);

  // 1000 dots from tick 100 of line 0: 331 in line 0, 341 in line 1, and 328 into line 2.
  const TimingEvent* const crtc_event = FindEvent("GPU CRTC Tick");
  ASSERT_NE(crtc_event, nullptr);
  const TickCount target_time = SystemTicksForGPUQuarterTicks(2 * 13650 + 3280 * 4);
  EXPECT_EQ(CPU::g_state.pending_ticks + crtc_event->GetTicksUntilNextExecution(), target_time);
}

TEST_F(GPUFrontend, GatedTimerAdvancesPastFractionalHBlankBoundaries)
{
  GPU::WriteRegister(4, 0x070FFC00);
  Timers::WriteRegister(0x04, (1u << 8) | 1u); // Count dots outside HBlank.
  for (const u32 line : {9u, 11u, 13u})
  {
    SCOPED_TRACE(line);
    CPU::g_state.pending_ticks = SystemTicksForGPUQuarterTicks(line * 13650 + 488 * 4);
    Timers::ReadRegister(0x00);
    // The next event must advance time, rather than reschedule the boundary we just crossed.
    EXPECT_GT(CPU::g_state.downcount, CPU::g_state.pending_ticks);
  }
}

TEST_F(GPUFrontend, OverclockKeepsFractionalScanlines)
{
  g_settings.cpu_overclock_active = true;
  g_settings.cpu_overclock_numerator = 10;
  g_settings.cpu_overclock_denominator = 1;
  GPU::CPUClockChanged();

  const TickCount target_time = SystemTicksForGPUQuarterTicks(100 * 13650 * 10);
  EXPECT_EQ(GPU::GetSystemTicksUntilTicksAndLine(0, 100), target_time);
  CPU::g_state.pending_ticks = target_time;
  u32 tick, line;
  GPU::GetBeamPosition(&tick, &line);
  EXPECT_EQ(line, 100u);
  EXPECT_LE(tick, 1u);
}

TEST_F(GPUFrontend, ForcedVideoTimingUsesTheSelectedLineLength)
{
  GPU::WriteRegister(4, 0x08000008); // PAL output on the fixture's NTSC console clock.
  EXPECT_EQ(GPU::GetSystemTicksUntilTicksAndLine(0, 100), SystemTicksForGPUQuarterTicks(100 * 13620));

  Settings old_settings = g_settings;
  g_settings.gpu_force_video_timing = ForceVideoTimingMode::NTSC;
  GPU::UpdateSettings(old_settings);
  EXPECT_EQ(GPU::GetSystemTicksUntilTicksAndLine(0, 100), SystemTicksForGPUQuarterTicks(100 * 13650));

  old_settings = g_settings;
  g_settings.gpu_force_video_timing = ForceVideoTimingMode::PAL;
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x08000000);
  EXPECT_EQ(GPU::GetSystemTicksUntilTicksAndLine(0, 100), SystemTicksForGPUQuarterTicks(100 * 13620));
}

static void RunGPUTicks(TickCount ticks)
{
  while (ticks > 0)
  {
    const TickCount step = std::min<TickCount>(ticks, 256);
    CPU::AddPendingTicks(step);
    ticks -= step;
    if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
      TimingEvents::RunEvents();
  }
}

// In 240p, GPUSTAT.31 is the parity of the displayed VRAM row, counting from Y at Y1, and the parity of Y in vblank.
// GP1(05h) needs the video thread, so this only covers Y = 0.
TEST_F(GPUFrontend, ProgressiveLineBitIsTheDisplayedRowParity)
{
  GPU::WriteRegister(4, 0x07000000 | 0x11 | (0xC8 << 10)); // Y1 = 17, Y2 = 200
  for (u32 line = 1; line < 30; line++)
  {
    SCOPED_TRACE(line);
    u32 tick, current_line;
    do
    {
      RunGPUTicks(64);
      GPU::GetBeamPosition(&tick, &current_line);
    } while (current_line != line);
    EXPECT_EQ(GPU::ReadRegister(4) >> 31, (line < 0x11) ? 0u : ((line - 0x11) & 1u));
  }
}

// Interlaced fields differ in length, so a wait that pending ticks carried into the next field must use its length.
TEST_F(GPUFrontend, BeamWaitAcrossAPendingFieldChangeUsesTheNextField)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x08000028);
  GPU::WriteRegister(4, 0x07000000 | 0x125 | (0x125 << 10)); // Y1 == Y2 stays in vblank.
  for (u32 i = 0; i < 2; i++)
  {
    SCOPED_TRACE(i);
    RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 150));
    CPU::g_state.pending_ticks += SystemTicksForGPUQuarterTicks(200 * 13620);
    // The beam is now at line 37 or 38 of the next field, so the wait wraps.
    const TickCount pending_wait = GPU::GetSystemTicksUntilTicksAndLine(0, 20);
    GPU::SynchronizeCRTC();
    EXPECT_EQ(GPU::GetSystemTicksUntilTicksAndLine(0, 20), pending_wait);
    RunGPUTicks(1000);
  }
}

// Returns the hblanks until GPUSTAT.13 next changes, at the start of the last line of the field, and whether it fell.
// Gives up after the longest possible field, so a stuck field fails instead of hanging.
static std::pair<u32, bool> RunToFieldBitChange()
{
  const u32 start = Timers::ReadRegister(0x10);
  const u32 field_bit = GPU::ReadRegister(4) & (1u << 13);
  while ((GPU::ReadRegister(4) & (1u << 13)) == field_bit && (Timers::ReadRegister(0x10) - start) <= 1024)
    RunGPUTicks(256);
  return {Timers::ReadRegister(0x10) - start, field_bit != 0};
}

// Interlaced fields alternate between 263 and 262 lines in NTSC, and 313 and 312 in PAL, with GPUSTAT.13 falling at
// the end of the long one. The horizontal phase carries across fields, so each pair of fields has 525 or 625 hblanks.
// Y1 == Y2 stays in vblank, the fields alternate in 480i without the display field being latched.
TEST_F(GPUFrontend, InterlacedFieldsAlternateBetweenLongAndShort)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);

  for (const auto [mode, long_lines] : {std::pair<u32, u32>{0x20, 263}, {0x28, 313}, {0x24, 263}, {0x2C, 313}})
  {
    SCOPED_TRACE(mode);
    CPU::g_state.pending_ticks = 0;
    GPU::Reset(false);
    GPU::WriteRegister(4, 0x08000000 | mode);
    GPU::WriteRegister(4, 0x07000000 | 0x100 | (0x100 << 10));
    Timers::WriteRegister(0x14, 0x0100); // hblank clock
    RunToFieldBitChange();

    u32 previous_lines = 0;
    for (u32 i = 0; i < 6; i++)
    {
      SCOPED_TRACE(i);
      const auto [lines, fell] = RunToFieldBitChange();
      EXPECT_EQ(lines, fell ? long_lines : (long_lines - 1));
      if (i > 0)
        EXPECT_EQ(lines + previous_lines, long_lines * 2 - 1);
      previous_lines = lines;
    }
  }
}

// The field only toggles at the end of an interlaced field, so it holds through progressive frames: turning interlace
// back on resumes the same field type.
TEST_F(GPUFrontend, ProgressiveFramesHoldTheInterlacedField)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x08000028);
  GPU::WriteRegister(4, 0x07000000 | 0x125 | (0x125 << 10)); // Y1 == Y2 stays in vblank.
  Timers::WriteRegister(0x14, 0x0100);                       // hblank clock

  for (const bool short_field : {true, false, true})
  {
    SCOPED_TRACE(short_field);

    // GPUSTAT.13 reads 0 in the short field, until its last line.
    RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
    while (((GPU::ReadRegister(4) & (1u << 13)) == 0) != short_field)
    {
      RunGPUTicks(1000);
      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
    }

    GPU::WriteRegister(4, 0x08000008);
    for (u32 i = 0; i < 3; i++)
    {
      RunGPUTicks(1000);
      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
    }
    GPU::WriteRegister(4, 0x08000028);

    // GPUSTAT.13 changes at the start of the field's last line.
    const auto [lines, fell] = RunToFieldBitChange();
    EXPECT_EQ(lines, short_field ? 211u : 212u);
    EXPECT_EQ(fell, !short_field);
  }
}

// The next field is chosen a quarter into scanline 311, the last line of a short field and the line before the last
// of a long one. Interlace turned off before that holds the field in progress, and from there the next field.
TEST_F(GPUFrontend, InterlaceOffInScanline311HoldsTheNextField)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x08000028);
  GPU::WriteRegister(4, 0x07000000 | 0x125 | (0x125 << 10)); // Y1 == Y2 stays in vblank.
  Timers::WriteRegister(0x14, 0x0100);                       // hblank clock

  for (const auto [short_field, off_line, off_tick, held_short] :
       {std::tuple<bool, u32, u32, bool>{false, 310, 1700, false},
        {false, 311, 100, false},
        {false, 311, 1700, true},
        {true, 310, 1700, true},
        {true, 311, 100, true},
        {true, 311, 1700, false}})
  {
    SCOPED_TRACE(short_field);
    SCOPED_TRACE(off_line);
    SCOPED_TRACE(off_tick);

    // GPUSTAT.13 reads 0 in the short field, until its last line.
    RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
    while (((GPU::ReadRegister(4) & (1u << 13)) == 0) != short_field)
    {
      RunGPUTicks(1000);
      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
    }

    RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(off_tick, off_line));
    GPU::WriteRegister(4, 0x08000008);
    for (u32 i = 0; i < 3; i++)
    {
      RunGPUTicks(1000);
      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
    }
    GPU::WriteRegister(4, 0x08000028);

    // A held short field rises at its last line, 211 lines on, a held long field falls 212 lines on.
    const auto [lines, fell] = RunToFieldBitChange();
    EXPECT_EQ(lines, held_short ? 211u : 212u);
    EXPECT_EQ(fell, !held_short);
  }
}

// Interlace turned off at scanline 309 (259 in NTSC) and back on later. On after the held field's end was decided, at
// the start of a short field's last line or a quarter into the line before a long field's, misses it, and the 10-bit
// line counter runs on to 1024: (1024-309)+293 = 1008 lines to the IRQ in PAL, (1024-259)+256 = 1021 in NTSC. On in
// the 240p frame's last line lets it finish at 314 or 263 lines instead.
TEST_F(GPUFrontend, InterlaceSwitchNearTheEndOfTheFieldRunsTheLineCounterOn)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);

  struct Case
  {
    bool short_field;
    u32 on_line;
    u32 on_tick;
    u32 expected_lines;
  };
  for (const auto& [pal, off_line, y2, cases] :
       {std::tuple<bool, u32, u32, std::vector<Case>>{true,
                                                      309,
                                                      0x125,
                                                      {{true, 310, 0, 296},
                                                       {true, 311, 0, 1008},
                                                       {true, 312, 0, 1008},
                                                       {true, 313, 0, 298},
                                                       {false, 311, 0, 297},
                                                       {false, 311, 1700, 1008},
                                                       {false, 312, 0, 1008},
                                                       {false, 313, 0, 298}}},
        {false,
         259,
         0x100,
         {{true, 260, 0, 259},
          {true, 261, 0, 1021},
          {false, 261, 0, 260},
          {false, 261, 1700, 1021},
          {false, 262, 0, 260}}}})
  {
    const u32 mode = pal ? 0x08 : 0x00;
    const TimingEvent* const crtc_event = FindEvent("GPU CRTC Tick");
    ASSERT_NE(crtc_event, nullptr);
    CPU::g_state.pending_ticks = 0;
    GPU::Reset(false);
    GPU::WriteRegister(4, 0x08000020 | mode);
    GPU::WriteRegister(4, 0x07000000 | y2 | (y2 << 10)); // Y1 == Y2 stays in vblank.
    Timers::WriteRegister(0x14, 0x0100);                 // hblank clock

    for (const Case& c : cases)
    {
      SCOPED_TRACE(pal);
      SCOPED_TRACE(c.short_field);
      SCOPED_TRACE(c.on_line);
      SCOPED_TRACE(c.on_tick);

      // GPUSTAT.13 reads 0 in the short field, until its last line.
      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
      while (((GPU::ReadRegister(4) & (1u << 13)) == 0) != c.short_field)
      {
        RunGPUTicks(1000);
        RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
      }

      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, off_line));
      GPU::WriteRegister(4, 0x08000000 | mode);
      const u32 start = Timers::ReadRegister(0x10);
      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(c.on_tick, c.on_line));
      GPU::WriteRegister(4, 0x08000020 | mode);

      // The CRTC must wake up for the vblank IRQ without being synchronized.
      const TickCount crtc_deadline = crtc_event->GetTicksUntilNextExecution();
      EXPECT_LE(crtc_deadline, GPU::GetSystemTicksUntilTicksAndLine(0, y2));

      u32 tick, line;
      if (c.expected_lines > 1000)
      {
        RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 1000));
        GPU::GetBeamPosition(&tick, &line);
        EXPECT_EQ(line, 1000u);
      }

      RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, y2));
      GPU::GetBeamPosition(&tick, &line);
      EXPECT_EQ(line, y2);
      EXPECT_EQ(Timers::ReadRegister(0x10) - start, c.expected_lines);

      // The field after a run-on is the held one, GPUSTAT.13 reads 1 in a long field until its last line.
      if (c.expected_lines > 1000)
        EXPECT_EQ((GPU::ReadRegister(4) >> 13) & 1u, BoolToUInt32(!c.short_field));
    }
  }
}

// A GPU reset during a line counter run-on starts a normal frame.
TEST_F(GPUFrontend, ResetDuringALineCounterRunOnEndsIt)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x08000028);
  GPU::WriteRegister(4, 0x07000000 | 0x125 | (0x125 << 10)); // Y1 == Y2 stays in vblank.

  // GPUSTAT.13 reads 0 in the short field, until its last line.
  RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
  while ((GPU::ReadRegister(4) & (1u << 13)) != 0)
  {
    RunGPUTicks(1000);
    RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 100));
  }
  RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 309));
  GPU::WriteRegister(4, 0x08000008);
  RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 311));
  GPU::WriteRegister(4, 0x08000028);
  RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 500));
  u32 tick, line;
  GPU::GetBeamPosition(&tick, &line);
  ASSERT_EQ(line, 500u);

  CPU::g_state.pending_ticks = 0;
  GPU::Reset(false);
  GPU::WriteRegister(4, 0x07000000 | 0x100 | (0x100 << 10));
  RunGPUTicks(SystemTicksForGPUQuarterTicks(263 * 13650 + 1000 * 4));
  GPU::GetBeamPosition(&tick, &line);
  EXPECT_EQ(line, 0u);
}

// Turning interlace off on the last line of a short field leaves the line counter alone, so the field ends at the
// progressive length instead of skipping lines.
TEST_F(GPUFrontend, DisablingInterlaceOnTheLastLineKeepsTheLineCounter)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);

  for (const auto [mode, progressive_lines] : {std::pair<u32, u32>{0x20, 263}, {0x28, 314}})
  {
    SCOPED_TRACE(mode);
    CPU::g_state.pending_ticks = 0;
    GPU::Reset(false);
    GPU::WriteRegister(4, 0x08000000 | mode);
    GPU::WriteRegister(4, 0x07000000 | 0x100 | (0x100 << 10)); // Y1 == Y2 stays in vblank.
    Timers::WriteRegister(0x14, 0x0100);                       // hblank clock

    // GPUSTAT.13 rises at the start of the last line of the short field.
    while (RunToFieldBitChange().second)
      ;
    u32 tick, line;
    GPU::GetBeamPosition(&tick, &line);
    ASSERT_EQ(line, (mode == 0x20) ? 261u : 311u);

    GPU::WriteRegister(4, 0x08000000 | (mode & 0x08));
    u32 new_line;
    GPU::GetBeamPosition(&tick, &new_line);
    EXPECT_EQ(new_line, line);

    const u32 start = Timers::ReadRegister(0x10);
    RunGPUTicks(GPU::GetSystemTicksUntilTicksAndLine(0, 0));
    EXPECT_EQ(Timers::ReadRegister(0x10) - start, progressive_lines - line);
  }
}

// GP1(00h) leaves the line counter on its last line, the line before line 0 in every mode, so a mode change before
// that line ends still starts a long field on the next line.
TEST_F(GPUFrontend, ResetLineIsTheLineBeforeLineZeroInEveryMode)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);

  for (const auto [mode, long_lines] : {std::pair<u32, u32>{0x20, 263}, {0x28, 313}})
  {
    SCOPED_TRACE(mode);
    GPU::WriteRegister(4, 0x00000000);
    GPU::WriteRegister(4, 0x08000000 | mode);
    GPU::WriteRegister(4, 0x07000000 | 0x100 | (0x100 << 10)); // Y1 == Y2 stays in vblank.
    Timers::WriteRegister(0x14, 0x0100);                       // hblank clock

    // The reset line, then the long field up to its last line.
    const auto [lines, fell] = RunToFieldBitChange();
    EXPECT_TRUE(fell);
    EXPECT_EQ(lines, long_lines);
  }
}

// Enabling interlace on the last line after its hblank must not take the hblank back from Timer 1.
TEST_F(GPUFrontend, EnablingInterlaceAfterTheLastHBlankKeepsCountingHBlanks)
{
  const Settings old_settings = g_settings;
  g_settings.display_deinterlacing_mode = DisplayDeinterlacingMode::Progressive; // No display clear on interlace.
  GPU::UpdateSettings(old_settings);
  GPU::WriteRegister(4, 0x070FFC00);   // Keep the display range open.
  Timers::WriteRegister(0x14, 0x0100); // hblank clock
  CPU::g_state.pending_ticks = SystemTicksForGPUQuarterTicks(262 * 13650 + 3300 * 4);
  ASSERT_EQ(Timers::ReadRegister(0x10), 263u);

  GPU::WriteRegister(4, 0x08000020);
  EXPECT_EQ(Timers::ReadRegister(0x10), 263u);
  CPU::g_state.pending_ticks += SystemTicksForGPUQuarterTicks(13650);
  EXPECT_EQ(Timers::ReadRegister(0x10), 264u);
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
