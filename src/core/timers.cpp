// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "timers.h"
#include "gpu.h"
#include "interrupt_controller.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitfield.h"
#include "common/log.h"

#include "imgui.h"

#include <array>
#include <memory>

Log_SetChannel(Timers);

namespace Timers {
namespace {

static constexpr u32 NUM_TIMERS = 3;

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
};

} // namespace

static void UpdateCountingEnabled(CounterState& cs);
static void CheckForIRQ(u32 index, u32 old_counter);

static void AddSysClkTicks(void*, TickCount sysclk_ticks, TickCount ticks_late);

static TickCount GetTicksUntilNextInterrupt();
static void UpdateSysClkEvent();

namespace {
struct TimersState
{
  TimingEvent sysclk_event{ "Timer SysClk Interrupt", 1, 1, &Timers::AddSysClkTicks, nullptr };

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
    cs.external_counting_enabled = false;
    cs.counting_enabled = true;
    cs.irq_done = false;
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
  if (!cs.use_external_clock && (cs.mode.sync_mode != SyncMode::ResetOnGateEnd || !state))
    s_state.sysclk_event.InvokeEarly();

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

  UpdateCountingEnabled(cs);
  UpdateSysClkEvent();
}

TickCount Timers::GetTicksUntilIRQ(u32 timer)
{
  const CounterState& cs = s_state.counters[timer];
  if (!cs.counting_enabled)
    return std::numeric_limits<TickCount>::max();

  TickCount ticks_until_irq = std::numeric_limits<TickCount>::max();
  if (cs.mode.irq_at_target && cs.counter < cs.target)
    ticks_until_irq = static_cast<TickCount>(cs.target - cs.counter);
  if (cs.mode.irq_on_overflow)
    ticks_until_irq = std::min(ticks_until_irq, static_cast<TickCount>(0xFFFFu - cs.counter));

  return ticks_until_irq;
}

void Timers::AddTicks(u32 timer, TickCount count)
{
  CounterState& cs = s_state.counters[timer];
  const u32 old_counter = cs.counter;
  cs.counter += static_cast<u32>(count);
  CheckForIRQ(timer, old_counter);
}

void Timers::CheckForIRQ(u32 timer, u32 old_counter)
{
  CounterState& cs = s_state.counters[timer];

  bool interrupt_request = false;
  if (cs.counter >= cs.target && (old_counter < cs.target || cs.target == 0))
  {
    interrupt_request |= cs.mode.irq_at_target;
    cs.mode.reached_target = true;

    if (cs.mode.reset_at_target && cs.target > 0)
      cs.counter %= cs.target;
  }
  if (cs.counter >= 0xFFFF)
  {
    interrupt_request |= cs.mode.irq_on_overflow;
    cs.mode.reached_overflow = true;
    cs.counter %= 0xFFFFu;
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
    else
    {
      // TODO: How does the non-repeat mode work here?
      cs.mode.interrupt_request_n ^= true;
      if (!cs.mode.interrupt_request_n)
        DEBUG_LOG("Raising timer {} alternate IRQ", timer);

      InterruptController::SetLineState(irqnum, !cs.mode.interrupt_request_n);
    }
  }
}

void Timers::AddSysClkTicks(void*, TickCount sysclk_ticks, TickCount ticks_late)
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
      if (timer_index < 2 && cs.external_counting_enabled)
      {
        // timers 0/1 depend on the GPU
        if (timer_index == 0 || g_gpu->IsCRTCScanlinePending())
          g_gpu->SynchronizeCRTC();
      }

      s_state.sysclk_event.InvokeEarly();

      return cs.counter;
    }

    case 0x04:
    {
      if (timer_index < 2 && cs.external_counting_enabled)
      {
        // timers 0/1 depend on the GPU
        if (timer_index == 0 || g_gpu->IsCRTCScanlinePending())
          g_gpu->SynchronizeCRTC();
      }

      s_state.sysclk_event.InvokeEarly();

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

  if (timer_index < 2 && cs.external_counting_enabled)
  {
    // timers 0/1 depend on the GPU
    if (timer_index == 0 || g_gpu->IsCRTCScanlinePending())
      g_gpu->SynchronizeCRTC();
  }

  s_state.sysclk_event.InvokeEarly();

  // Strictly speaking these IRQ checks should probably happen on the next tick.
  switch (port_offset)
  {
    case 0x00:
    {
      const u32 old_counter = cs.counter;
      DEBUG_LOG("Timer {} write counter {}", timer_index, value);
      cs.counter = value & u32(0xFFFF);
      CheckForIRQ(timer_index, old_counter);
      if (timer_index == 2 || !cs.external_counting_enabled)
        UpdateSysClkEvent();
    }
    break;

    case 0x04:
    {
      static constexpr u32 WRITE_MASK = 0b1110001111111111;

      DEBUG_LOG("Timer {} write mode register 0x{:04X}", timer_index, value);
      cs.mode.bits = (value & WRITE_MASK) | (cs.mode.bits & ~WRITE_MASK);
      cs.use_external_clock = (cs.mode.clock_source & (timer_index == 2 ? 2 : 1)) != 0;
      cs.counter = 0;
      cs.irq_done = false;
      InterruptController::SetLineState(
        static_cast<InterruptController::IRQ>(static_cast<u32>(InterruptController::IRQ::TMR0) + timer_index), false);

      UpdateCountingEnabled(cs);
      CheckForIRQ(timer_index, cs.counter);
      UpdateSysClkEvent();
    }
    break;

    case 0x08:
    {
      DEBUG_LOG("Timer {} write target 0x{:04X}", timer_index, ZeroExtend32(Truncate16(value)));
      cs.target = value & u32(0xFFFF);
      CheckForIRQ(timer_index, cs.counter);
      if (timer_index == 2 || !cs.external_counting_enabled)
        UpdateSysClkEvent();
    }
    break;

    default:
      ERROR_LOG("Write unknown register in timer {} (offset 0x{:02X}, value 0x{:X})", timer_index, offset, value);
      break;
  }
}

void Timers::UpdateCountingEnabled(CounterState& cs)
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

  cs.external_counting_enabled = cs.use_external_clock && cs.counting_enabled;
}

TickCount Timers::GetTicksUntilNextInterrupt()
{
  TickCount min_ticks = System::GetMaxSliceTicks();
  for (u32 i = 0; i < NUM_TIMERS; i++)
  {
    const CounterState& cs = s_state.counters[i];
    if (!cs.counting_enabled || (i < 2 && cs.external_counting_enabled) ||
        (!cs.mode.irq_at_target && !cs.mode.irq_on_overflow && (cs.mode.irq_repeat || !cs.irq_done)))
    {
      continue;
    }

    if (cs.mode.irq_at_target)
    {
      TickCount ticks = (cs.counter <= cs.target) ? static_cast<TickCount>(cs.target - cs.counter) :
                                                    static_cast<TickCount>((0xFFFFu - cs.counter) + cs.target);
      if (cs.external_counting_enabled) // sysclk/8 for timer 2
        ticks *= 8;

      min_ticks = std::min(min_ticks, ticks);
    }
    if (cs.mode.irq_on_overflow)
    {
      TickCount ticks = static_cast<TickCount>(0xFFFFu - cs.counter);
      if (cs.external_counting_enabled) // sysclk/8 for timer 2
        ticks *= 8;

      min_ticks = std::min(min_ticks, ticks);
    }
  }

  return System::ScaleTicksToOverclock(std::max<TickCount>(1, min_ticks));
}

void Timers::UpdateSysClkEvent()
{
  s_state.sysclk_event.Schedule(GetTicksUntilNextInterrupt());
}

void Timers::DrawDebugStateWindow()
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

  const float framebuffer_scale = ImGuiManager::GetGlobalScale();

  ImGui::SetNextWindowSize(ImVec2(800.0f * framebuffer_scale, 115.0f * framebuffer_scale), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Timer State", nullptr))
  {
    ImGui::End();
    return;
  }

  ImGui::Columns(NUM_COLUMNS);
  ImGui::SetColumnWidth(0, 20.0f * framebuffer_scale);
  ImGui::SetColumnWidth(1, 50.0f * framebuffer_scale);
  ImGui::SetColumnWidth(2, 50.0f * framebuffer_scale);
  ImGui::SetColumnWidth(3, 100.0f * framebuffer_scale);
  ImGui::SetColumnWidth(4, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(5, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(6, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(7, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(8, 80.0f * framebuffer_scale);
  ImGui::SetColumnWidth(9, 80.0f * framebuffer_scale);

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
  ImGui::End();
}
