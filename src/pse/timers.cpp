#include "timers.h"
#include "YBaseLib/Log.h"
#include "common/state_wrapper.h"
#include "interrupt_controller.h"
#include "system.h"
Log_SetChannel(Timers);

Timers::Timers() = default;

Timers::~Timers() = default;

bool Timers::Initialize(System* system, InterruptController* interrupt_controller)
{
  m_system = system;
  m_interrupt_controller = interrupt_controller;
  return true;
}

void Timers::Reset()
{
  for (CounterState& cs : m_states)
  {
    cs.mode.bits = 0;
    cs.counter = 0;
    cs.target = 0;
    cs.gate = false;
    cs.external_counting_enabled = false;
    cs.counting_enabled = true;
  }
}

bool Timers::DoState(StateWrapper& sw)
{
  for (CounterState& cs : m_states)
  {
    sw.Do(&cs.mode.bits);
    sw.Do(&cs.counter);
    sw.Do(&cs.target);
    sw.Do(&cs.gate);
    sw.Do(&cs.external_counting_enabled);
    sw.Do(&cs.counting_enabled);
  }

  return !sw.HasError();
}

void Timers::SetGate(u32 timer, bool state)
{
  CounterState& cs = m_states[timer];
  if (cs.gate == state)
    return;

  cs.gate = state;

  if (cs.mode.sync_enable)
  {
    if (state)
    {
      switch (cs.mode.sync_mode)
      {
        case SyncMode::ResetOnGate:
        case SyncMode::ResetAndRunOnGate:
          cs.counter = 0;
          break;

        case SyncMode::FreeRunOnGate:
          cs.mode.sync_enable = false;
          break;
      }
    }

    UpdateCountingEnabled(cs);
  }
}

void Timers::AddTicks(u32 timer, u32 count)
{
  CounterState& cs = m_states[timer];
  cs.counter += count;

  const u32 reset_value = cs.mode.reset_at_target ? cs.target : u32(0xFFFF);
  if (cs.counter < reset_value)
    return;

  const bool old_intr = cs.mode.interrupt_request;

  if (cs.counter >= cs.target)
    cs.mode.reached_target = true;
  if (cs.counter >= u32(0xFFFF))
    cs.mode.reached_overflow = true;

  // TODO: Non-repeat mode.
  const bool target_intr = cs.mode.reached_target & cs.mode.irq_at_target;
  const bool overflow_intr = cs.mode.reached_overflow & cs.mode.irq_on_overflow;
  const bool new_intr = target_intr | overflow_intr;
  if (!old_intr && new_intr)
  {
    m_interrupt_controller->InterruptRequest(
      static_cast<InterruptController::IRQ>(static_cast<u32>(InterruptController::IRQ::TMR0) + timer));
  }

  if (reset_value > 0)
    cs.counter = cs.counter % reset_value;
  else
    cs.counter = 0;
}

void Timers::AddSystemTicks(u32 ticks)
{
  if (!m_states[0].external_counting_enabled && m_states[0].counting_enabled)
    AddTicks(0, ticks);
  if (!m_states[1].external_counting_enabled && m_states[1].counting_enabled)
    AddTicks(1, ticks);
  if (m_states[2].counting_enabled)
    AddTicks(2, m_states[2].external_counting_enabled ? (ticks / 8) : (ticks));
}

u32 Timers::ReadRegister(u32 offset)
{
  const u32 timer_index = (offset >> 4) & u32(0x03);
  const u32 port_offset = offset & u32(0x0F);

  CounterState& cs = m_states[timer_index];

  switch (port_offset)
  {
    case 0x00:
    {
      m_system->Synchronize();
      return cs.counter;
    }

    case 0x04:
    {
      m_system->Synchronize();

      const u32 bits = cs.mode.bits;
      cs.mode.reached_overflow = false;
      cs.mode.reached_target = false;
      return bits;
    }

    case 0x08:
      return cs.target;

    default:
      Log_ErrorPrintf("Read unknown register in timer %u (offset 0x%02X)", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void Timers::WriteRegister(u32 offset, u32 value)
{
  const u32 timer_index = (offset >> 4) & u32(0x03);
  const u32 port_offset = offset & u32(0x0F);

  CounterState& cs = m_states[timer_index];

  switch (port_offset)
  {
    case 0x00:
    {
      Log_DebugPrintf("Timer %u write counter %u", timer_index, value);
      m_system->Synchronize();
      cs.counter = value & u32(0xFFFF);
    }
    break;

    case 0x04:
    {
      Log_DebugPrintf("Timer %u write mode register 0x%04X", timer_index, value);
      m_system->Synchronize();
      cs.mode.bits = value & u32(0x1FFF);
      cs.use_external_clock = (cs.mode.clock_source & (timer_index == 2 ? 2 : 1)) != 0;
      cs.counter = 0;
      UpdateCountingEnabled(cs);
    }
    break;

    case 0x08:
    {
      Log_DebugPrintf("Timer %u write target 0x%04X", timer_index, ZeroExtend32(Truncate16(value)));
      m_system->Synchronize();
      cs.target = value & u32(0xFFFF);
    }
    break;

    default:
      Log_ErrorPrintf("Write unknown register in timer %u (offset 0x%02X, value 0x%X)", offset, value);
      break;
  }
}

void Timers::UpdateCountingEnabled(CounterState& cs)
{
  if (cs.mode.sync_enable)
  {
    switch (cs.mode.sync_mode)
    {
      case SyncMode::PauseOnGate:
      case SyncMode::FreeRunOnGate:
        cs.counting_enabled = !cs.gate;
        break;

      case SyncMode::ResetOnGate:
        cs.counting_enabled = true;
        break;

      case SyncMode::ResetAndRunOnGate:
        cs.counting_enabled = cs.gate;
        break;
    }
  }
  else
  {
    cs.counting_enabled = true;
  }

  cs.external_counting_enabled = cs.use_external_clock && cs.counting_enabled;
}

void Timers::UpdateDowncount() {}

u32 Timers::GetSystemTicksForTimerTicks(u32 timer) const
{
  return 1;
}
