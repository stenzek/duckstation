// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "timers.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "system.h"
#include "timing_event.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/log.h"

#include "imgui.h"

#include <array>
#include <memory>

LOG_CHANNEL(Timers);

namespace Timers {
namespace {

static constexpr u32 NUM_TIMERS = 3;
static constexpr u32 TIMER_OVERFLOW = 0x10000;

// On the system clock, the counter reads as the target on the tick it gets there, then is held at zero for two ticks.
// Writes only hold it for one tick, so a full hold is only left after reaching the target.
static constexpr u8 TARGET_RESET_DELAY = 2;

enum class SyncMode : u8
{
  PauseWhileGateActive = 0,
  ResetOnGateEnd = 1,
  ResetAndRunOnGateStart = 2,
  FreeRunOnGateEnd = 3
};

union CounterMode
{
  u32 bits;

  BitField<u32, bool, 0, 1> sync_enable;
  BitField<u32, SyncMode, 1, 2> sync_mode;
  BitField<u32, bool, 3, 1> reset_at_target;
  BitField<u32, bool, 4, 1> irq_at_target;
  BitField<u32, bool, 5, 1> irq_on_overflow;
  BitField<u32, bool, 6, 1> irq_repeat;
  BitField<u32, bool, 7, 1> irq_pulse_n;
  BitField<u32, u8, 8, 2> clock_source;
  BitField<u32, bool, 10, 1> interrupt_request_n;
  BitField<u32, bool, 11, 1> reached_target;
  BitField<u32, bool, 12, 1> reached_overflow;
};

struct CounterState
{
  CounterMode mode;
  u32 counter;
  u32 target;
  bool gate;
  bool use_external_clock;
  bool external_counting_enabled;
  bool counting_enabled;
  bool irq_done;
  u8 counting_delay;
};

} // namespace

static void UpdateCountingEnabled(u32 index, CounterState& cs);
static void CheckForIRQ(u32 index, u32 first_counter);
static void SynchronizeGPUForRead(u32 index, const CounterState& cs);
static u32 UpdateForRead(u32 index, const CounterState& cs);
static u32 GetReadCounter(const CounterState& cs, u32 counter, u32 delay);

static void AddSysClkTicks(void*, TickCount sysclk_ticks);

static TickCount GetTicksUntilNextInterrupt();
static void UpdateSysClkEvent();

namespace {
struct TimersState
{
  TimingEvent sysclk_event{"Timer SysClk Interrupt", 1, 1, &Timers::AddSysClkTicks, nullptr};

  std::array<CounterState, NUM_TIMERS> counters{};
  TickCount sysclk_ticks_carry = 0; // 0 unless overclocking is enabled
  u32 sysclk_div_8_carry = 0;       // partial ticks for timer 3 with sysclk/8
};
} // namespace

ALIGN_TO_CACHE_LINE static TimersState s_state;
}; // namespace Timers

void Timers::Initialize()
{
  Reset();
}

void Timers::Shutdown()
{
  s_state.sysclk_event.Deactivate();
}

void Timers::Reset()
{
  for (CounterState& cs : s_state.counters)
  {
    cs.mode.bits = 0;
    cs.mode.interrupt_request_n = true;
    cs.counter = 0;
    cs.target = 0;
    cs.gate = false;
    cs.use_external_clock = false;
    cs.external_counting_enabled = false;
    cs.counting_enabled = true;
    cs.irq_done = false;
    cs.counting_delay = 0;
  }

  s_state.sysclk_event.Deactivate();
  s_state.sysclk_ticks_carry = 0;
  s_state.sysclk_div_8_carry = 0;
  UpdateSysClkEvent();
}

bool Timers::DoState(StateWrapper& sw)
{
  for (CounterState& cs : s_state.counters)
  {
    sw.Do(&cs.mode.bits);
    sw.Do(&cs.counter);
    sw.Do(&cs.target);
    sw.Do(&cs.gate);
    sw.Do(&cs.use_external_clock);
    sw.Do(&cs.external_counting_enabled);
    sw.Do(&cs.counting_enabled);
    sw.Do(&cs.irq_done);
    sw.DoEx(&cs.counting_delay, 88, static_cast<u8>(0));
  }

  sw.Do(&s_state.sysclk_ticks_carry);
  sw.Do(&s_state.sysclk_div_8_carry);

  if (sw.IsReading())
    UpdateSysClkEvent();

  return !sw.HasError();
}

void Timers::CPUClocksChanged()
{
  s_state.sysclk_ticks_carry = 0;
}

bool Timers::IsUsingExternalClock(u32 timer)
{
  return s_state.counters[timer].external_counting_enabled;
}

bool Timers::IsSyncEnabled(u32 timer)
{
  return s_state.counters[timer].mode.sync_enable;
}

bool Timers::IsExternalIRQEnabled(u32 timer)
{
  const CounterState& cs = s_state.counters[timer];
  return (cs.external_counting_enabled && (cs.mode.bits & ((1u << 4) | (1u << 5))) != 0);
}

void Timers::SetGate(u32 timer, bool state)
{
  CounterState& cs = s_state.counters[timer];
  if (cs.gate == state)
    return;

  cs.gate = state;

  if (!cs.mode.sync_enable)
    return;

  // Because the gate prevents counting in or outside of the gate, we need a correct counter.
  // For reset, we _can_ skip it, until the gate clears.
  const bool update_sysclk = (!cs.use_external_clock && (cs.mode.sync_mode != SyncMode::ResetOnGateEnd || !state));
  if (update_sysclk)
    s_state.sysclk_event.InvokeEarlyNoReschedule();

  switch (cs.mode.sync_mode)
  {
    case SyncMode::PauseWhileGateActive:
      break;

    case SyncMode::ResetOnGateEnd:
      cs.counter = state ? cs.counter : 0;
      break;

    case SyncMode::ResetAndRunOnGateStart:
      // PS2 hardwires the counter to 0 outside of gate. Needs to be tested for PSX too.
      cs.counter = state ? 0 : cs.counter;
      break;

    case SyncMode::FreeRunOnGateEnd:
      cs.mode.sync_enable &= state;
      break;

    default:
      UnreachableCode();
  }

  UpdateCountingEnabled(timer, cs);

  // Can't reschedule without updating the counters first, the IRQ would end up being late.
  if (update_sysclk)
    UpdateSysClkEvent();
}

TickCount Timers::GetTicksUntilIRQ(u32 timer)
{
  const CounterState& cs = s_state.counters[timer];
  if (!cs.counting_enabled)
    return std::numeric_limits<TickCount>::max();

  // If the counter isn't below the target, it has to wrap around to reach it. Which is also why it can still
  // overflow when resetting at the target.
  TickCount ticks_until_irq = std::numeric_limits<TickCount>::max();
  if (cs.mode.irq_at_target)
  {
    ticks_until_irq = cs.counting_delay + ((cs.counter < cs.target) ?
                                             static_cast<TickCount>(cs.target - cs.counter) :
                                             static_cast<TickCount>((TIMER_OVERFLOW - cs.counter) + cs.target));
  }
  if (cs.mode.irq_on_overflow)
  {
    ticks_until_irq =
      std::min(ticks_until_irq, cs.counting_delay + static_cast<TickCount>(TIMER_OVERFLOW - cs.counter));
  }

  return ticks_until_irq;
}

void Timers::AddTicks(u32 timer, TickCount count)
{
  CounterState& cs = s_state.counters[timer];

  // The counter doesn't increment for a few ticks after writes, or resetting at the target.
  const TickCount delay = std::min<TickCount>(cs.counting_delay, count);
  cs.counting_delay -= static_cast<u8>(delay);
  count -= delay;

  // Nothing to check when the counter isn't reaching the target or overflowing, which is most of the time.
  const u32 new_counter = cs.counter + static_cast<u32>(count);
  if (new_counter < TIMER_OVERFLOW && (new_counter < cs.target || cs.counter >= cs.target))
  {
    cs.counter = new_counter;
    return;
  }

  // Needs slow path if using toggle mode, because reaching the target and overflowing each toggle the request, and
  // unless it's toggling an even number of times the IRQ might not actually trigger.
  if (cs.mode.irq_pulse_n)
  {
    do
    {
      const u32 reset_at = (cs.counter < cs.target) ? cs.target : TIMER_OVERFLOW;
      const u32 add = std::min(reset_at - cs.counter, static_cast<u32>(count));
      const u32 old_counter = cs.counter;
      cs.counter += add;
      count -= static_cast<TickCount>(add);
      CheckForIRQ(timer, old_counter + 1);

      const TickCount reset_delay = std::min<TickCount>(cs.counting_delay, count);
      cs.counting_delay -= static_cast<u8>(reset_delay);
      count -= reset_delay;
    } while (count > 0);
  }
  else
  {
    const u32 old_counter = cs.counter;
    cs.counter += static_cast<u32>(count);
    CheckForIRQ(timer, old_counter + 1);
  }
}

void Timers::CheckForIRQ(u32 timer, u32 first_counter)
{
  CounterState& cs = s_state.counters[timer];

  // first_counter is the first counter value which hasn't been compared to the target yet. When adding ticks, that's
  // one past the old counter. When the counter is reset by a mode write, or wraps around, it's zero, which is the
  // only time a zero target is reached.
  // The counter can be set above the target, in which case it _will_ wrap around and trigger an IRQ.
  bool wrapped_overflow = false;
  if (cs.counter >= TIMER_OVERFLOW)
  {
    wrapped_overflow = (!cs.mode.reset_at_target || first_counter > cs.target);

    // If the target wasn't passed before wrapping, it has to be reached again afterwards.
    if (first_counter > cs.target)
      cs.counter -= TIMER_OVERFLOW;

    first_counter = 0;
  }

  bool interrupt_request = false;
  if (cs.counter >= cs.target && first_counter <= cs.target)
  {
    interrupt_request |= cs.mode.irq_at_target;
    cs.mode.reached_target = true;

    // This is pretty janky. If you set the target to 0xFFFF, the overflow IRQ still needs to fire.
    // Presumably, the reset happens after the next increment. Also consider the case where the
    // target is resetting below 0xFFFF, and the counter is incremented by a large amount. In this
    // second case, the overflow IRQ shouldn't trigger.
    if (cs.mode.reset_at_target && cs.target > 0 && cs.target != 0xFFFFu)
    {
      if (cs.use_external_clock)
      {
        cs.counter %= cs.target;
      }
      else
      {
        // The system clock holds the counter at zero for two more ticks after resetting.
        const u32 ticks_since_reset = (cs.counter - cs.target) % (cs.target + TARGET_RESET_DELAY);
        cs.counter = (ticks_since_reset > TARGET_RESET_DELAY) ? (ticks_since_reset - TARGET_RESET_DELAY) : 0;
        cs.counting_delay =
          static_cast<u8>((ticks_since_reset < TARGET_RESET_DELAY) ? (TARGET_RESET_DELAY - ticks_since_reset) : 0);
      }
    }
  }
  if (cs.counter >= TIMER_OVERFLOW || wrapped_overflow)
  {
    interrupt_request |= cs.mode.irq_on_overflow;
    cs.mode.reached_overflow = true;
    cs.counter &= 0xFFFFu;
  }

  if (interrupt_request)
  {
    const InterruptController::IRQ irqnum =
      static_cast<InterruptController::IRQ>(static_cast<u32>(InterruptController::IRQ::TMR0) + timer);
    if (!cs.mode.irq_pulse_n)
    {
      if (!cs.irq_done || cs.mode.irq_repeat)
      {
        // this is actually low for a few cycles
        DEBUG_LOG("Raising timer {} pulse IRQ", timer);
        InterruptController::SetLineState(irqnum, false);
        InterruptController::SetLineState(irqnum, true);
      }

      cs.irq_done = true;
      cs.mode.interrupt_request_n = true;
    }
    else if (!cs.irq_done || cs.mode.irq_repeat)
    {
      // In one-shot mode, the request stays asserted until the next mode write.
      cs.irq_done = true;
      cs.mode.interrupt_request_n ^= true;
      if (!cs.mode.interrupt_request_n)
        DEBUG_LOG("Raising timer {} alternate IRQ", timer);

      InterruptController::SetLineState(irqnum, !cs.mode.interrupt_request_n);
    }
  }
}

void Timers::AddSysClkTicks(void*, TickCount sysclk_ticks)
{
  sysclk_ticks = System::UnscaleTicksToOverclock(sysclk_ticks, &s_state.sysclk_ticks_carry);

  if (!s_state.counters[0].external_counting_enabled && s_state.counters[0].counting_enabled)
    AddTicks(0, sysclk_ticks);
  if (!s_state.counters[1].external_counting_enabled && s_state.counters[1].counting_enabled)
    AddTicks(1, sysclk_ticks);
  if (s_state.counters[2].external_counting_enabled)
  {
    TickCount sysclk_div_8_ticks = (sysclk_ticks + s_state.sysclk_div_8_carry) / 8;
    s_state.sysclk_div_8_carry = (sysclk_ticks + s_state.sysclk_div_8_carry) % 8;
    AddTicks(2, sysclk_div_8_ticks);
  }
  else if (s_state.counters[2].counting_enabled)
  {
    AddTicks(2, sysclk_ticks);
  }

  UpdateSysClkEvent();
}

void Timers::SynchronizeGPUForRead(u32 index, const CounterState& cs)
{
  if (index >= 2)
    return;

  // Timers 0/1 depend on the GPU when it's clocking them.
  if (cs.external_counting_enabled)
  {
    if (index == 0 || GPU::IsCRTCScanlinePending())
      GPU::SynchronizeCRTC();

    return;
  }

  // With sync enabled, the GPU also changes the gate. The CRTC event runs at each blank start and end, but the CPU
  // only runs events between instructions/blocks, so it can read the counter a few ticks after the gate changed,
  // before the event has run. In reset modes, the read would then see the counter from before the reset, a whole
  // line/frame off instead of a few ticks. Running the CRTC event first applies the gate change at the time of the
  // read, so the counter is only off by those few ticks. This only happens when an event is overdue, so it's rare.
  if (cs.mode.sync_enable && TimingEvents::IsAnyEventDue())
    GPU::SynchronizeCRTC();
}

u32 Timers::UpdateForRead(u32 index, const CounterState& cs)
{
  // Updating the counters early only changes the counters themselves, unless one of them reaches its target or
  // overflows. The event isn't due until the first IRQ, so before then we only need to look at the timer being read,
  // and can work out its counter without updating anything. The remainder makes this harder when overclocking.
  // The CPU can also be past events which haven't run yet, such as a gate change from the GPU. Updating stops them
  // changing the counters before the time of this read, so they have to run first, including the sysclk event.
  if (!g_settings.cpu_overclock_active && !TimingEvents::IsAnyEventDue())
  {
    u32 counter = cs.counter;
    u32 delay = cs.counting_delay;
    if (cs.counting_enabled && (index == 2 || !cs.external_counting_enabled))
    {
      const u32 ticks = static_cast<u32>(s_state.sysclk_event.GetTicksSinceLastExecution());
      if (cs.external_counting_enabled)
      {
        counter += (ticks + s_state.sysclk_div_8_carry) / 8; // sysclk/8
      }
      else
      {
        const u32 held_ticks = std::min(ticks, delay);
        counter += ticks - held_ticks;
        delay -= held_ticks;
      }
    }

    if (counter < TIMER_OVERFLOW && (counter < cs.target || cs.counter >= cs.target))
      return GetReadCounter(cs, counter, delay);
  }

  s_state.sysclk_event.InvokeEarlyNoReschedule();
  return GetReadCounter(cs, cs.counter, cs.counting_delay);
}

u32 Timers::GetReadCounter(const CounterState& cs, u32 counter, u32 delay)
{
  return (delay == TARGET_RESET_DELAY) ? cs.target : counter;
}

u32 Timers::ReadRegister(u32 offset)
{
  const u32 timer_index = (offset >> 4) & u32(0x03);
  const u32 port_offset = offset & u32(0x0F);
  if (timer_index >= 3) [[unlikely]]
  {
    ERROR_LOG("Timer read out of range: offset 0x{:02X}", offset);
    return UINT32_C(0xFFFFFFFF);
  }

  CounterState& cs = s_state.counters[timer_index];

  switch (port_offset)
  {
    case 0x00:
    {
      SynchronizeGPUForRead(timer_index, cs);

      return UpdateForRead(timer_index, cs);
    }

    case 0x04:
    {
      SynchronizeGPUForRead(timer_index, cs);

      UpdateForRead(timer_index, cs);

      const u32 bits = cs.mode.bits;
      cs.mode.reached_overflow = false;
      cs.mode.reached_target = false;
      return bits;
    }

    case 0x08:
      return cs.target;

    default:
      [[unlikely]] ERROR_LOG("Read unknown register in timer {} (offset 0x{:02X})", timer_index, offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void Timers::WriteRegister(u32 offset, u32 value)
{
  const u32 timer_index = (offset >> 4) & u32(0x03);
  const u32 port_offset = offset & u32(0x0F);
  if (timer_index >= 3) [[unlikely]]
  {
    ERROR_LOG("Timer write out of range: offset 0{:02X} value 0x{:08X}", offset, value);
    return;
  }

  CounterState& cs = s_state.counters[timer_index];

  // timers 0/1 depend on the GPU, which has to be up to date so that it can be rescheduled for the new IRQ/gate
  const bool update_gpu = (timer_index < 2 && (cs.external_counting_enabled || port_offset == 0x04));
  if (update_gpu)
    GPU::SynchronizeCRTC();

  s_state.sysclk_event.InvokeEarlyNoReschedule();

  // Strictly speaking these IRQ checks should probably happen on the next tick.
  switch (port_offset)
  {
    case 0x00:
    {
      // Writing the new counter does not compare against the target value.
      DEBUG_LOG("Timer {} write counter {}", timer_index, value);
      cs.counter = value & u32(0xFFFF);
      cs.counting_delay = cs.use_external_clock ? 0 : 1;
      if (timer_index == 2 || !cs.external_counting_enabled)
        UpdateSysClkEvent();
    }
    break;

    case 0x04:
    {
      static constexpr u32 WRITE_MASK = 0b0000001111111111;

      DEBUG_LOG("Timer {} write mode register 0x{:04X}", timer_index, value);
      cs.mode.bits = (value & WRITE_MASK) | (cs.mode.bits & ~WRITE_MASK);

      // Interrupt is cleared after any mode write.
      cs.mode.interrupt_request_n = true;

      // Why is this extra assignment here? MSVC compiler bugs it seems.
      // Without the copy in the local variable, when compiling with LTCG, it generates:
      //
      // 00007FF7BAF721E1 41 C1 EA 08          shr         r10d,8
      // 00007FF7BAF721E5 41 83 E2 01          and         r10d,1
      // 00007FF7BAF721E9 0F 95 C0             setne       al
      //
      // and without LTCG, or with the copy:
      //
      // 00007FF7C1859732 C1 EA 08             shr         edx,8
      // 00007FF7C1859735 83 FB 02             cmp         ebx,2
      // 00007FF7C1859738 0F 94 C0             sete        al
      // 00007FF7C185973B FF C0                inc         eax
      // 00007FF7C185973D 84 D0                test        al,dl
      // 00007FF7C185973F 0F 95 C0             setne       al
      //
      // In other words, we get:
      //   use_external_clock = (clock_source & 1) != 0
      // and
      //   use_external_clock = (clock_source & ((timer_index == 2) + 1)) != 0
      //
      // The entire "timer_index == 2" sub-expression is ignored. Furthermore, the
      // and r10d, 1; setne al sequence doesn't make sense to me either, since r10d & 1 is going
      // to always be equal to !ZF...
      //
      // I can't seem to make a minimal repro case for it, and if you do _anything_ inbetween
      // the statements (e.g. printf), it doesn't generate the bad code. But at least this works
      // around it for now.
      //
      const u8 clock_source = cs.mode.clock_source;
      cs.use_external_clock = (clock_source & (timer_index == 2 ? 2 : 1)) != 0;

      cs.counter = 0;
      cs.counting_delay = cs.use_external_clock ? 0 : 1;
      cs.irq_done = false;
      InterruptController::SetLineState(
        static_cast<InterruptController::IRQ>(static_cast<u32>(InterruptController::IRQ::TMR0) + timer_index), false);

      UpdateCountingEnabled(timer_index, cs);
      CheckForIRQ(timer_index, cs.counter);
      UpdateSysClkEvent();
    }
    break;

    case 0x08:
    {
      DEBUG_LOG("Timer {} write target 0x{:04X}", timer_index, ZeroExtend32(Truncate16(value)));
      cs.target = value & u32(0xFFFF);
      if (timer_index == 2 || !cs.external_counting_enabled)
        UpdateSysClkEvent();
    }
    break;

    default:
      ERROR_LOG("Write unknown register in timer {} (offset 0x{:02X}, value 0x{:X})", timer_index, offset, value);
      break;
  }

  if (update_gpu)
    GPU::RescheduleCRTC();
}

void Timers::UpdateCountingEnabled(u32 index, CounterState& cs)
{
  if (index != 2)
  {
    if (cs.mode.sync_enable)
    {
      switch (cs.mode.sync_mode)
      {
        case SyncMode::PauseWhileGateActive:
          cs.counting_enabled = !cs.gate;
          break;

        case SyncMode::ResetOnGateEnd:
          cs.counting_enabled = true;
          break;

        case SyncMode::ResetAndRunOnGateStart:
        case SyncMode::FreeRunOnGateEnd:
          cs.counting_enabled = cs.gate;
          break;

        default:
          UnreachableCode();
      }
    }
    else
    {
      cs.counting_enabled = true;
    }
  }
  else
  {
    // Timer 2 doesn't have a gate source. With sync enabled, modes 0/3 stop counting and 1/2 free-run.
    cs.counting_enabled = (!cs.mode.sync_enable || cs.mode.sync_mode == SyncMode::ResetOnGateEnd ||
                           cs.mode.sync_mode == SyncMode::ResetAndRunOnGateStart);
  }

  cs.external_counting_enabled = cs.use_external_clock && cs.counting_enabled;
}

TickCount Timers::GetTicksUntilNextInterrupt()
{
  TickCount min_ticks = System::GetMaxSliceTicks();
  for (u32 i = 0; i < NUM_TIMERS; i++)
  {
    const CounterState& cs = s_state.counters[i];
    if (!cs.counting_enabled || (i < 2 && cs.external_counting_enabled) ||
        (!cs.mode.irq_at_target && !cs.mode.irq_on_overflow) || (!cs.mode.irq_repeat && cs.irq_done))
    {
      continue;
    }

    if (cs.mode.irq_at_target)
    {
      TickCount ticks = cs.counting_delay + ((cs.counter < cs.target) ?
                                               static_cast<TickCount>(cs.target - cs.counter) :
                                               static_cast<TickCount>((TIMER_OVERFLOW - cs.counter) + cs.target));
      if (cs.external_counting_enabled) // sysclk/8 for timer 2
        ticks = (ticks * 8) - static_cast<TickCount>(s_state.sysclk_div_8_carry);

      min_ticks = std::min(min_ticks, ticks);
    }
    if (cs.mode.irq_on_overflow)
    {
      TickCount ticks = cs.counting_delay + static_cast<TickCount>(TIMER_OVERFLOW - cs.counter);
      if (cs.external_counting_enabled) // sysclk/8 for timer 2
        ticks = (ticks * 8) - static_cast<TickCount>(s_state.sysclk_div_8_carry);

      min_ticks = std::min(min_ticks, ticks);
    }
  }

  min_ticks = std::max<TickCount>(1, min_ticks);
  if (!g_settings.cpu_overclock_active)
    return min_ticks;

  // Same as ScaleTicksToOverclock(), except part of the next tick has already been counted.
  return static_cast<TickCount>(((static_cast<u64>(static_cast<u32>(min_ticks)) * g_settings.cpu_overclock_numerator) -
                                 static_cast<u32>(s_state.sysclk_ticks_carry) +
                                 (g_settings.cpu_overclock_denominator - 1)) /
                                g_settings.cpu_overclock_denominator);
}

void Timers::UpdateSysClkEvent()
{
  s_state.sysclk_event.Schedule(GetTicksUntilNextInterrupt());
}

void Timers::DrawDebugStateWindow(float scale)
{
  static constexpr u32 NUM_COLUMNS = 10;
  static constexpr std::array<const char*, NUM_COLUMNS> column_names = {
    {"#", "Value", "Target", "Sync", "Reset", "IRQ", "IRQRepeat", "IRQToggle", "Clock Source", "Reached"}};
  static constexpr std::array<const char*, 4> sync_mode_names = {
    {"PauseOnGate", "ResetOnGate", "ResetAndRunOnGate", "FreeRunOnGate"}};
  static constexpr std::array<std::array<const char*, 4>, 3> clock_source_names = {
    {{{"SysClk", "DotClk", "SysClk", "DotClk"}},
     {{"SysClk", "HBlank", "SysClk", "HBlank"}},
     {{"SysClk", "DotClk", "SysClk/8", "SysClk/8"}}}};

  ImGui::Columns(NUM_COLUMNS);
  ImGui::SetColumnWidth(0, 20.0f * scale);
  ImGui::SetColumnWidth(1, 50.0f * scale);
  ImGui::SetColumnWidth(2, 50.0f * scale);
  ImGui::SetColumnWidth(3, 100.0f * scale);
  ImGui::SetColumnWidth(4, 80.0f * scale);
  ImGui::SetColumnWidth(5, 80.0f * scale);
  ImGui::SetColumnWidth(6, 80.0f * scale);
  ImGui::SetColumnWidth(7, 80.0f * scale);
  ImGui::SetColumnWidth(8, 80.0f * scale);
  ImGui::SetColumnWidth(9, 80.0f * scale);

  for (const char* title : column_names)
  {
    ImGui::TextUnformatted(title);
    ImGui::NextColumn();
  }

  for (u32 i = 0; i < NUM_TIMERS; i++)
  {
    const CounterState& cs = s_state.counters[i];
    ImGui::PushStyleColor(ImGuiCol_Text,
                          cs.counting_enabled ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    ImGui::Text("%u", i);
    ImGui::NextColumn();
    ImGui::Text("%u", cs.counter);
    ImGui::NextColumn();
    ImGui::Text("%u", cs.target);
    ImGui::NextColumn();
    ImGui::Text("%s",
                cs.mode.sync_enable ? sync_mode_names[static_cast<u8>(cs.mode.sync_mode.GetValue())] : "Disabled");
    ImGui::NextColumn();
    ImGui::Text("%s", cs.mode.reset_at_target ? "@Target" : "@Overflow");
    ImGui::NextColumn();
    ImGui::Text("%s%s", cs.mode.irq_at_target ? "Target " : "", cs.mode.irq_on_overflow ? "Overflow" : "");
    ImGui::NextColumn();
    ImGui::Text("%s", cs.mode.irq_repeat ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::Text("%s", cs.mode.irq_pulse_n ? "Yes" : "No");
    ImGui::NextColumn();
    ImGui::Text("%s%s", clock_source_names[i][cs.mode.clock_source], cs.external_counting_enabled ? " (External)" : "");
    ImGui::NextColumn();
    ImGui::Text("%s%s", cs.mode.reached_target ? "Target " : "", cs.mode.reached_overflow ? "Overflow" : "");
    ImGui::NextColumn();
    ImGui::PopStyleColor();
  }

  ImGui::Columns(1);
}
