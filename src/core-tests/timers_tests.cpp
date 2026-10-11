// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "core/cpu_core.h"
#include "core/interrupt_controller.h"
#include "core/system.h"
#include "core/timers.h"
#include "core/timing_event.h"

#include <gtest/gtest.h>

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
