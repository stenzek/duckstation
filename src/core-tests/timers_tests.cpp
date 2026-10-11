// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/timers.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

enum : u32
{
  TIMER0 = 0x00,
  TIMER1 = 0x10,
  TIMER2 = 0x20,

  REG_COUNTER = 0x00,
  REG_MODE = 0x04,
  REG_TARGET = 0x08,

  MODE_SYNC_ENABLE = (1u << 0),
  MODE_RESET_AT_TARGET = (1u << 3),
  MODE_IRQ_AT_TARGET = (1u << 4),
  MODE_IRQ_ON_OVERFLOW = (1u << 5),
  MODE_IRQ_REPEAT = (1u << 6),
  MODE_IRQ_TOGGLE = (1u << 7),
  MODE_CLOCK_SOURCE_1 = (1u << 8), // dot clock for timer 0, hblank for timer 1
  MODE_CLOCK_SOURCE_2 = (1u << 9), // sysclk/8 for timer 2
  MODE_IRQ_N = (1u << 10),
  MODE_REACHED_TARGET = (1u << 11),
  MODE_REACHED_OVERFLOW = (1u << 12),
};

class TimersTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    // The max slice ticks come from System. Updating it schedules the SPU/GPU events too, which we can't run.
    TimingEvents::Initialize();
    System::UpdateOverclock();
    DeactivateAllEvents();
  }

  void SetUp() override
  {
    // Interrupts stay disabled in the CPU, the tests look at the interrupt controller instead.
    CPU::g_state.cop0_regs.sr.bits = 0;
    CPU::g_state.pending_ticks = 0;
    InterruptController::Reset();
    TimingEvents::Reset();
    Timers::Initialize();
  }

  void TearDown() override { Timers::Shutdown(); }

  static void DeactivateAllEvents()
  {
    while (TimingEvent* event = *TimingEvents::GetHeadEventPtr())
      event->Deactivate();
  }

  static void SetOverclock(bool enabled)
  {
    g_settings.cpu_overclock_enable = enabled;
    g_settings.SetCPUOverclockPercent(enabled ? 350 : 100);
    g_settings.UpdateOverclockActive();

    // This puts the other events back, and takes ours out with them.
    System::UpdateOverclock();
    DeactivateAllEvents();
    Timers::Reset();
  }

  /// Runs the CPU for the specified number of ticks, without any register accesses.
  static void Run(u32 ticks)
  {
    for (u32 i = 0; i < ticks; i++)
    {
      CPU::AddPendingTicks(1);
      if (CPU::GetPendingTicks() >= CPU::g_state.downcount)
        TimingEvents::RunEvents();
    }
  }

  /// Returns the number of ticks until the sysclk event runs next, it is the only active event.
  static TickCount GetTicksUntilSysClkEvent()
  {
    return (*TimingEvents::GetHeadEventPtr())->GetTicksUntilNextExecution();
  }

  /// Returns true if the timer has raised an IRQ since the last call.
  static bool TakeIRQ(u32 timer)
  {
    const u32 bit = 1u << (static_cast<u32>(InterruptController::IRQ::TMR0) + timer);
    const bool raised = (InterruptController::ReadRegister(0x00) & bit) != 0;
    InterruptController::WriteRegister(0x00, ~bit);
    return raised;
  }
};

} // namespace

// Gate changes on a GPU-clocked timer used to reschedule the sysclk event from counters which weren't up to date,
// pushing the IRQ back each time.
TEST_F(TimersTest, GateChangeDoesNotDelaySysClkIRQ)
{
  static constexpr u32 PERIOD = 1000;
  Timers::WriteRegister(TIMER2 + REG_TARGET, PERIOD);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_RESET_AT_TARGET | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);
  Timers::WriteRegister(TIMER1 + REG_MODE, MODE_SYNC_ENABLE | MODE_CLOCK_SOURCE_1);

  bool gate = false;
  for (u32 tick = 1; tick <= PERIOD * 10; tick++)
  {
    Run(1);

    if ((tick % 300) == 0)
    {
      gate = !gate;
      Timers::SetGate(1, gate);
    }

    ASSERT_EQ(TakeIRQ(2), (tick % PERIOD) == 0) << "at tick " << tick;
  }
}

// The counter wrapping around isn't the same thing as reaching the target.
TEST_F(TimersTest, OverflowDoesNotReachTarget)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0x8000);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);

  Timers::AddTicks(2, 0x8000);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET);

  // Wraps, but is nowhere near the target.
  Timers::AddTicks(2, 0x8040);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 0x40u);
  EXPECT_FALSE(TakeIRQ(2));
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_OVERFLOW);

  Timers::AddTicks(2, 0x7FC0);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 0x8000u);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET);

  // Wraps, and gets to the target again.
  Timers::AddTicks(2, 0x10000);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 0x8000u);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW);

  // Passes the target, then wraps.
  Timers::WriteRegister(TIMER2 + REG_COUNTER, 0x7000);
  Timers::AddTicks(2, 0x9010);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 0x10u);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW);
}

// When the counter is set past a resetting target, it has to wrap around before the target applies again.
TEST_F(TimersTest, OverflowWithCounterAboveResetTarget)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 100);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_RESET_AT_TARGET);

  Timers::WriteRegister(TIMER2 + REG_COUNTER, 200);
  Timers::AddTicks(2, (0x10000 - 200) + 5);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 5u);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_OVERFLOW);

  Timers::AddTicks(2, 100);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 5u);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET);

  // Same thing, but getting to the target after wrapping in a single update.
  Timers::WriteRegister(TIMER2 + REG_COUNTER, 200);
  Timers::AddTicks(2, (0x10000 - 200) + 250);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 50u);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW);
}

TEST_F(TimersTest, ToggleModeDoesNotToggleOnOverflow)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0x100);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT | MODE_IRQ_TOGGLE);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, MODE_IRQ_N);

  Timers::AddTicks(2, 0x100);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, 0u);
  EXPECT_TRUE(TakeIRQ(2));

  // One trip around the counter is one toggle.
  Timers::AddTicks(2, 0x10000);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 0x100u);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, MODE_IRQ_N);
  EXPECT_FALSE(TakeIRQ(2));

  Timers::AddTicks(2, 0x10000);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, 0u);
  EXPECT_TRUE(TakeIRQ(2));
}

// The sysclk event should run when the IRQ happens, and not before then.
TEST_F(TimersTest, SysClkEventScheduledForIRQ)
{
  // Overflow is one tick after reaching 0xFFFF.
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_ON_OVERFLOW | MODE_IRQ_REPEAT);
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10000);
  Run(0xFFFF);
  EXPECT_FALSE(TakeIRQ(2));
  Run(1);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10000);

  // When free-running, the counter has to wrap around to reach the target again.
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0x100);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x100);
  Run(0x100);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10000);
  Run(0xFFFF);
  EXPECT_FALSE(TakeIRQ(2));
  Run(1);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10000);
}

// The sysclk/8 prescaler keeps running regardless of when the counters get updated, so the IRQ shouldn't move either.
TEST_F(TimersTest, SysClkDiv8IRQNotAffectedByUpdates)
{
  static constexpr u32 PERIOD = 1000 * 8;
  static constexpr u32 MODE = MODE_CLOCK_SOURCE_2 | MODE_RESET_AT_TARGET | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT;
  Timers::WriteRegister(TIMER2 + REG_TARGET, 1000);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE);

  // Leave the prescaler part way through a tick, it doesn't get reset with the counter.
  Run(3);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE);
  EXPECT_EQ(GetTicksUntilSysClkEvent(), static_cast<TickCount>(PERIOD - 3));

  for (u32 tick = 1; tick <= PERIOD * 5; tick++)
  {
    Run(1);

    // Writing to any of the timers updates all of them.
    if ((tick % 61) == 0)
      Timers::WriteRegister(TIMER0 + REG_TARGET, 0);

    ASSERT_EQ(TakeIRQ(2), ((tick + 3) % PERIOD) == 0) << "at tick " << tick;
  }
}

// Same again, but for the partial tick which is left over when overclocking.
TEST_F(TimersTest, OverclockedIRQNotAffectedByUpdates)
{
  SetOverclock(true);

  std::vector<u32> irq_ticks[2];
  for (u32 pass = 0; pass < 2; pass++)
  {
    Timers::Reset();
    Timers::WriteRegister(TIMER2 + REG_TARGET, 1001);
    Timers::WriteRegister(TIMER2 + REG_MODE, MODE_RESET_AT_TARGET | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);

    for (u32 tick = 1; tick <= 20000; tick++)
    {
      Run(1);

      if (pass == 1 && (tick % 61) == 0)
        Timers::WriteRegister(TIMER0 + REG_TARGET, 0);
      if (TakeIRQ(2))
        irq_ticks[pass].push_back(tick);
    }
  }

  SetOverclock(false);

  // 1001 timer ticks is 3503.5 CPU ticks at 350%.
  EXPECT_EQ(irq_ticks[0], (std::vector<u32>{3504, 7007, 10511, 14014, 17518}));
  EXPECT_EQ(irq_ticks[1], irq_ticks[0]);
}

// A zero target is only reached when the counter is zero, that is when it's reset by the mode write, or wraps around.
TEST_F(TimersTest, ZeroTarget)
{
  for (const u32 mode :
       {MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT, MODE_RESET_AT_TARGET | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT})
  {
    Timers::WriteRegister(TIMER2 + REG_TARGET, 0);
    Timers::WriteRegister(TIMER2 + REG_MODE, mode);
    EXPECT_TRUE(TakeIRQ(2));
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
              MODE_REACHED_TARGET);

    // sysclk/8 adds zero ticks most of the time.
    Timers::AddTicks(2, 0);
    Timers::AddTicks(2, 100);
    EXPECT_FALSE(TakeIRQ(2));
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW), 0u);

    Timers::AddTicks(2, 0x10000 - 100);
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 0u);
    EXPECT_TRUE(TakeIRQ(2));
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
              MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW);
  }
}

// It used to be reached every time the counters were updated, so accessing any timer register raised the IRQ.
TEST_F(TimersTest, ZeroTargetIRQNotAffectedByUpdates)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10000);

  for (u32 tick = 1; tick <= 0x20000; tick++)
  {
    Run(1);

    if ((tick % 37) == 0)
      Timers::WriteRegister(TIMER0 + REG_TARGET, 0);

    ASSERT_EQ(TakeIRQ(2), (tick % 0x10000) == 0) << "at tick " << tick;
  }
}

// This is what the GPU schedules the CRTC event with, for the timers it clocks.
TEST_F(TimersTest, GetTicksUntilIRQ)
{
  Timers::WriteRegister(TIMER1 + REG_TARGET, 100);
  Timers::WriteRegister(TIMER1 + REG_MODE, MODE_CLOCK_SOURCE_1 | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);
  EXPECT_EQ(Timers::GetTicksUntilIRQ(1), 100);
  Timers::AddTicks(1, 100);
  EXPECT_TRUE(TakeIRQ(1));

  // When free-running, the counter has to wrap around to reach the target again.
  EXPECT_EQ(Timers::GetTicksUntilIRQ(1), 0x10000);
  Timers::AddTicks(1, 0xFFFF);
  EXPECT_FALSE(TakeIRQ(1));
  EXPECT_EQ(Timers::GetTicksUntilIRQ(1), 1);
  Timers::AddTicks(1, 1);
  EXPECT_TRUE(TakeIRQ(1));

  // Resetting at the target doesn't stop the counter from overflowing if it's already past it.
  Timers::WriteRegister(TIMER1 + REG_MODE,
                        MODE_CLOCK_SOURCE_1 | MODE_RESET_AT_TARGET | MODE_IRQ_ON_OVERFLOW | MODE_IRQ_REPEAT);
  Timers::WriteRegister(TIMER1 + REG_COUNTER, 200);
  EXPECT_EQ(Timers::GetTicksUntilIRQ(1), 0x10000 - 200);
  Timers::AddTicks(1, 0x10000 - 200);
  EXPECT_TRUE(TakeIRQ(1));
}
