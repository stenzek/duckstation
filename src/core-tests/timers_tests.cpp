// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/interrupt_controller.h"
#include "core/settings.h"
#include "core/system.h"
#include "core/timers.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

#include <cstring>
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

  void TearDown() override
  {
    Timers::Shutdown();

    // Writes to GPU-clocked timers reschedule the CRTC event, which would otherwise be left active.
    DeactivateAllEvents();
  }

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

  /// Returns the CRTC event, or null if it isn't active.
  static TimingEvent* FindCRTCEvent()
  {
    TimingEvent* event = *TimingEvents::GetHeadEventPtr();
    while (event && std::strcmp(event->GetName(), "GPU CRTC Tick") != 0)
      event = event->next;
    return event;
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

    // Held for a tick after the mode write, and for two ticks after each reset.
    ASSERT_EQ(TakeIRQ(2), tick > PERIOD && ((tick - (PERIOD + 1)) % (PERIOD + 2)) == 0) << "at tick " << tick;
  }
}

// The counter wrapping around isn't the same thing as reaching the target.
TEST_F(TimersTest, OverflowDoesNotReachTarget)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0x8000);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);

  // The counter is held for a tick after writing the mode or counter.
  Timers::AddTicks(2, 1 + 0x8000);
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
  Timers::AddTicks(2, 1 + 0x9010);
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
  Timers::AddTicks(2, 1 + (0x10000 - 200) + 5);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 5u);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_OVERFLOW);

  // Held at zero for two ticks after resetting.
  Timers::AddTicks(2, 100);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 3u);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET);

  // Same thing, but getting to the target after wrapping in a single update.
  Timers::WriteRegister(TIMER2 + REG_COUNTER, 200);
  Timers::AddTicks(2, 1 + (0x10000 - 200) + 250);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 46u);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & (MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW),
            MODE_REACHED_TARGET | MODE_REACHED_OVERFLOW);
}

TEST_F(TimersTest, ToggleModeDoesNotToggleOnOverflow)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0x100);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT | MODE_IRQ_TOGGLE);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, MODE_IRQ_N);

  Timers::AddTicks(2, 1 + 0x100);
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

// One-shot toggle mode leaves the request asserted, until it is rearmed by writing the mode.
TEST_F(TimersTest, OneShotToggleModeOnlyTogglesOnce)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0x100);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_TOGGLE);

  Timers::AddTicks(2, 1 + 0x100);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, 0u);
  EXPECT_TRUE(TakeIRQ(2));

  for (u32 i = 0; i < 2; i++)
  {
    Timers::AddTicks(2, 0x10000);
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, 0u);
    EXPECT_FALSE(TakeIRQ(2));
  }

  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_TOGGLE);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, MODE_IRQ_N);
  Timers::AddTicks(2, 1 + 0x100);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_MODE) & MODE_IRQ_N, 0u);
  EXPECT_TRUE(TakeIRQ(2));
}

// On the system clock, the counter is held for a tick after writing the mode or counter, and for two ticks after
// resetting at the target, so the period is the target plus two.
TEST_F(TimersTest, SysClkCounterHeldAfterWritesAndReset)
{
  Timers::WriteRegister(TIMER2 + REG_TARGET, 3);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_RESET_AT_TARGET);
  for (const u32 expected : {0u, 0u, 1u, 2u, 3u, 0u, 0u, 1u, 2u, 3u})
  {
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), expected);
    Run(1);
  }

  Timers::WriteRegister(TIMER2 + REG_COUNTER, 1);
  for (const u32 expected : {1u, 1u, 2u, 3u, 0u})
  {
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), expected);
    Run(1);
  }

  // The example from the documentation, with a target of 1.
  Timers::WriteRegister(TIMER2 + REG_TARGET, 1);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_RESET_AT_TARGET);
  for (const u32 expected : {0u, 0u, 1u, 0u, 0u, 1u})
  {
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), expected);
    Run(1);
  }

  // Reading after updating the counters gives the same values.
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_RESET_AT_TARGET);
  for (const u32 expected : {0u, 0u, 1u, 0u, 0u, 1u})
  {
    Timers::WriteRegister(TIMER0 + REG_TARGET, 0);
    EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), expected);
    Run(1);
  }
}

// The sysclk event should run when the IRQ happens, and not before then.
TEST_F(TimersTest, SysClkEventScheduledForIRQ)
{
  // Overflow is one tick after reaching 0xFFFF, and the counter is held for a tick after writing the mode.
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_ON_OVERFLOW | MODE_IRQ_REPEAT);
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10001);
  Run(0x10000);
  EXPECT_FALSE(TakeIRQ(2));
  Run(1);
  EXPECT_TRUE(TakeIRQ(2));
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10000);

  // When free-running, the counter has to wrap around to reach the target again.
  Timers::WriteRegister(TIMER2 + REG_TARGET, 0x100);
  Timers::WriteRegister(TIMER2 + REG_MODE, MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x101);
  Run(0x101);
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

  // 1003 timer ticks with the hold after resetting is 3510.5 CPU ticks at 350%, plus one for the mode write.
  EXPECT_EQ(irq_ticks[0], (std::vector<u32>{3507, 7018, 10528, 14039, 17549}));
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

    // sysclk/8 adds zero ticks most of the time. The counter is held for a tick after writing the mode.
    Timers::AddTicks(2, 0);
    Timers::AddTicks(2, 1 + 100);
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
  EXPECT_EQ(GetTicksUntilSysClkEvent(), 0x10001);

  for (u32 tick = 1; tick <= 0x20001; tick++)
  {
    Run(1);

    if ((tick % 37) == 0)
      Timers::WriteRegister(TIMER0 + REG_TARGET, 0);

    ASSERT_EQ(TakeIRQ(2), tick > 1 && ((tick - 1) % 0x10000) == 0) << "at tick " << tick;
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

// The CRTC event is only scheduled for the next IRQ of the timers it clocks, so it needs redoing when they change.
TEST_F(TimersTest, GPUClockedTimerWriteReschedulesCRTC)
{
  // Brings back the CRTC event, at the end of the frame. It's fine as long as it doesn't get to run.
  System::UpdateOverclock();

  const TimingEvent* crtc_event = FindCRTCEvent();
  ASSERT_NE(crtc_event, nullptr);
  const TickCount ticks_per_frame = crtc_event->GetTicksUntilNextExecution();

  // Timer 1 counts scanlines.
  Timers::WriteRegister(TIMER1 + REG_TARGET, 4);
  Timers::WriteRegister(TIMER1 + REG_MODE,
                        MODE_CLOCK_SOURCE_1 | MODE_RESET_AT_TARGET | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);
  const TickCount ticks_for_4_lines = crtc_event->GetTicksUntilNextExecution();
  EXPECT_LT(ticks_for_4_lines, ticks_per_frame / 50);

  Timers::WriteRegister(TIMER1 + REG_TARGET, 2);
  EXPECT_NEAR(crtc_event->GetTicksUntilNextExecution(), ticks_for_4_lines / 2, 1);

  Timers::WriteRegister(TIMER1 + REG_COUNTER, 1);
  EXPECT_NEAR(crtc_event->GetTicksUntilNextExecution(), ticks_for_4_lines / 4, 1);

  // Back to sysclk, the CRTC doesn't have to do anything for it.
  Timers::WriteRegister(TIMER1 + REG_MODE, 0);
  EXPECT_EQ(crtc_event->GetTicksUntilNextExecution(), ticks_per_frame);
}

// When the callback reschedules the event, it should end up the same as it does with InvokeEarly().
TEST_F(TimersTest, InvokeEarlyNoReschedule)
{
  struct State
  {
    TimingEvent event;
    TickCount ticks = 0;
  };
  static constexpr TimingEventCallback callback = [](void* param, TickCount ticks) {
    State* state = static_cast<State*>(param);
    state->ticks += ticks;
    state->event.Schedule(500);
  };
  State a = {.event = TimingEvent("Test Event A", 1, 1, callback, &a)};
  State b = {.event = TimingEvent("Test Event B", 1, 1, callback, &b)};
  a.event.Schedule(1000);
  b.event.Schedule(1000);

  Run(300);
  a.event.InvokeEarly();
  b.event.InvokeEarlyNoReschedule();
  EXPECT_EQ(a.ticks, 300);
  EXPECT_EQ(b.ticks, 300);
  EXPECT_EQ(b.event.GetTicksSinceLastExecution(), a.event.GetTicksSinceLastExecution());
  EXPECT_EQ(b.event.GetTicksUntilNextExecution(), a.event.GetTicksUntilNextExecution());
  EXPECT_EQ(b.event.GetTicksUntilNextExecution(), 500);
  EXPECT_EQ(CPU::g_state.downcount, 800u);

  // Nothing has happened since, so there's nothing to do.
  b.event.InvokeEarlyNoReschedule();
  EXPECT_EQ(b.ticks, 300);

  // Both should run again from the event loop.
  Run(500);
  EXPECT_EQ(a.ticks, 800);
  EXPECT_EQ(b.ticks, 800);

  // The callback can deactivate the event instead of scheduling it, and it's still left alone until the period has
  // passed.
  static constexpr TimingEventCallback deactivate_callback = [](void* param, TickCount ticks) {
    State* state = static_cast<State*>(param);
    state->ticks += ticks;
    state->event.Deactivate();
  };
  State c = {.event = TimingEvent("Test Event C", 200, 200, deactivate_callback, &c)};
  c.event.Schedule(400);

  Run(100);
  c.event.InvokeEarlyNoReschedule();
  EXPECT_EQ(c.ticks, 0);
  EXPECT_EQ(c.event.GetTicksUntilNextExecution(), 300);

  Run(150);
  c.event.InvokeEarlyNoReschedule();
  EXPECT_EQ(c.ticks, 250);
  EXPECT_FALSE(c.event.IsActive());
  EXPECT_EQ(CPU::g_state.downcount, 500u);

  a.event.Deactivate();
  b.event.Deactivate();
}

// Reading the counter or mode leaves the counters alone when updating them wouldn't change anything else. It has to
// give the same result as when they are updated.
TEST_F(TimersTest, DeferredReadsMatchUpdatedReads)
{
  Run(100);
  EXPECT_EQ(Timers::ReadRegister(TIMER2 + REG_COUNTER), 100u);
  EXPECT_EQ((*TimingEvents::GetHeadEventPtr())->GetTicksSinceLastExecution(), 100);

  struct Access
  {
    u32 tick;
    u32 value;
  };
  std::vector<Access> accesses[2];
  for (u32 pass = 0; pass < 2; pass++)
  {
    Timers::Reset();
    Timers::WriteRegister(TIMER0 + REG_TARGET, 300);
    Timers::WriteRegister(TIMER0 + REG_MODE, MODE_RESET_AT_TARGET | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);
    Timers::WriteRegister(TIMER1 + REG_TARGET, 0x7000);
    Timers::WriteRegister(TIMER1 + REG_MODE, MODE_IRQ_ON_OVERFLOW | MODE_IRQ_REPEAT);
    Timers::WriteRegister(TIMER2 + REG_TARGET, 1000);
    Timers::WriteRegister(TIMER2 + REG_MODE,
                          MODE_CLOCK_SOURCE_2 | MODE_RESET_AT_TARGET | MODE_IRQ_AT_TARGET | MODE_IRQ_REPEAT);

    // The mode writes to timers 0/1 reschedule the CRTC event. It isn't needed with them on sysclk, and it can't be
    // allowed to run, because the GPU starts a frame when it reaches vblank and there's no video thread to take it.
    FindCRTCEvent()->Deactivate();

    u32 rng = 0x12345678u;
    for (u32 tick = 1; tick <= 200000; tick++)
    {
      Run(1);

      for (u32 timer = 0; timer < 3; timer++)
      {
        if (TakeIRQ(timer))
          accesses[pass].push_back({tick, 0x80000000u | timer});
      }

      rng = (rng * 1664525u) + 1013904223u;
      if ((rng >> 28) != 0)
        continue;

      // Writing a register always updates the counters, here without changing anything.
      if (pass == 1)
        Timers::WriteRegister(TIMER1 + REG_TARGET, 0x7000);

      const u32 timer = ((rng >> 24) & 0xF) % 3;
      const u32 reg = ((rng >> 20) & 1) ? REG_MODE : REG_COUNTER;
      accesses[pass].push_back({tick, Timers::ReadRegister((timer << 4) + reg)});
    }
  }

  ASSERT_GT(accesses[0].size(), 10000u);
  ASSERT_EQ(accesses[0].size(), accesses[1].size());
  for (size_t i = 0; i < accesses[0].size(); i++)
  {
    ASSERT_EQ(accesses[0][i].tick, accesses[1][i].tick) << "access " << i;
    ASSERT_EQ(accesses[0][i].value, accesses[1][i].value) << "access " << i << " at tick " << accesses[0][i].tick;
  }
}

// Ticks get added in one go when nothing is looking at the timer, and a few at a time when something is. Both have to
// end up in the same place.
TEST_F(TimersTest, AddTicksMatchesSingleTicks)
{
  static constexpr u32 targets[] = {0, 1, 100, 0xFFFF};
  static constexpr u32 counters[] = {0, 100, 101, 0xFFFF};
  static constexpr u32 counts[] = {1, 2, 3, 150, 0x10000 + 150};
  static constexpr u32 irq_modes[] = {0, MODE_IRQ_AT_TARGET, MODE_IRQ_ON_OVERFLOW,
                                      MODE_IRQ_AT_TARGET | MODE_IRQ_ON_OVERFLOW};

  for (const u32 reset_mode : {0u, static_cast<u32>(MODE_RESET_AT_TARGET)})
  {
    for (const u32 toggle_mode : {0u, static_cast<u32>(MODE_IRQ_TOGGLE)})
    {
      for (const u32 irq_mode : irq_modes)
      {
        const u32 mode = reset_mode | toggle_mode | irq_mode | MODE_IRQ_REPEAT;
        for (const u32 target : targets)
        {
          for (const u32 counter : counters)
          {
            for (const u32 count : counts)
            {
              u32 result_counter[2];
              u32 result_mode[2];
              bool result_irq[2];
              for (u32 pass = 0; pass < 2; pass++)
              {
                Timers::WriteRegister(TIMER2 + REG_TARGET, target);
                Timers::WriteRegister(TIMER2 + REG_MODE, mode);
                Timers::WriteRegister(TIMER2 + REG_COUNTER, counter);
                Timers::ReadRegister(TIMER2 + REG_MODE);
                TakeIRQ(2);

                if (pass == 0)
                {
                  Timers::AddTicks(2, static_cast<TickCount>(count));
                }
                else
                {
                  for (u32 i = 0; i < count; i++)
                    Timers::AddTicks(2, 1);
                }

                result_counter[pass] = Timers::ReadRegister(TIMER2 + REG_COUNTER);
                result_mode[pass] = Timers::ReadRegister(TIMER2 + REG_MODE);
                result_irq[pass] = TakeIRQ(2);
              }

              ASSERT_TRUE(result_counter[0] == result_counter[1] && result_mode[0] == result_mode[1] &&
                          result_irq[0] == result_irq[1])
                << std::hex << "mode 0x" << mode << " target 0x" << target << " counter 0x" << counter << " count 0x"
                << count << ": counter 0x" << result_counter[0] << "/0x" << result_counter[1] << " mode 0x"
                << result_mode[0] << "/0x" << result_mode[1] << " irq " << result_irq[0] << "/" << result_irq[1];
            }
          }
        }
      }
    }
  }
}

// The CPU can read a timer after it has gone past an event which hasn't run yet. A gate change from that event can't
// be allowed to change how the counter counted up to the read, otherwise the counter goes backwards when paused.
TEST_F(TimersTest, ReadAfterOverdueGateChange)
{
  struct State
  {
    TimingEvent event;
    bool gate;
  };
  static constexpr TimingEventCallback callback = [](void* param, TickCount) {
    State* state = static_cast<State*>(param);
    Timers::SetGate(1, state->gate);
    state->event.Deactivate();
  };

  for (u32 sync_mode = 0; sync_mode < 4; sync_mode++)
  {
    for (const bool gate : {true, false})
    {
      u32 values[2][2];
      for (u32 pass = 0; pass < 2; pass++)
      {
        Timers::Reset();
        Timers::SetGate(1, !gate);
        Timers::WriteRegister(TIMER1 + REG_MODE, MODE_SYNC_ENABLE | (sync_mode << 1));

        // The mode write reschedules the CRTC event, and the overdue read would then synchronize it. The gate comes
        // from the test event instead, and the CRTC can't be allowed to run, because the GPU starts a frame when it
        // reaches vblank and there's no video thread to take it.
        FindCRTCEvent()->Deactivate();

        State state = {.event = TimingEvent("Test Gate Event", 1, 1, callback, &state), .gate = gate};
        state.event.Schedule(1000);

        // The read happens 500 ticks after the gate change was due.
        CPU::AddPendingTicks(1500);

        // Writing a register always updates the counters, here without changing anything.
        if (pass == 1)
          Timers::WriteRegister(TIMER2 + REG_TARGET, 0);

        values[pass][0] = Timers::ReadRegister(TIMER1 + REG_COUNTER);
        TimingEvents::RunEvents();
        Run(100);
        values[pass][1] = Timers::ReadRegister(TIMER1 + REG_COUNTER);
        ASSERT_FALSE(state.event.IsActive());
      }

      EXPECT_EQ(values[0][0], values[1][0]) << "sync mode " << sync_mode << " gate " << gate;
      EXPECT_EQ(values[0][1], values[1][1]) << "sync mode " << sync_mode << " gate " << gate;
      // Paused at the time of the read, less the tick it's held for after writing the mode.
      if (sync_mode == 0 && gate)
        EXPECT_EQ(values[0][1], 1499u);
    }
  }
}
