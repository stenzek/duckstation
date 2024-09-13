// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "timing_event.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "system.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/thirdparty/SmallVector.h"

Log_SetChannel(TimingEvents);

namespace TimingEvents {

static GlobalTicks GetTimestampForNewEvent();

static void SortEvent(TimingEvent* event);
static void AddActiveEvent(TimingEvent* event);
static void RemoveActiveEvent(TimingEvent* event);
static void SortEvents();
static TimingEvent* FindActiveEvent(const std::string_view name);
static void CommitGlobalTicks(const GlobalTicks new_global_ticks);

namespace {
struct TimingEventsState
{
  TimingEvent* active_events_head = nullptr;
  TimingEvent* active_events_tail = nullptr;
  TimingEvent* current_event = nullptr;
  u32 active_event_count = 0;
  GlobalTicks current_event_next_run_time = 0;
  GlobalTicks global_tick_counter = 0;
  GlobalTicks event_run_tick_counter = 0;
};
} // namespace

ALIGN_TO_CACHE_LINE static TimingEventsState s_state;

} // namespace TimingEvents

GlobalTicks TimingEvents::GetGlobalTickCounter()
{
  return s_state.global_tick_counter;
}

GlobalTicks TimingEvents::GetTimestampForNewEvent()
{
  // we want to schedule relative to the currently-being processed event, but if we haven't run events in a while, it
  // needs to include the pending time. so explicitly add the two.
  return s_state.global_tick_counter + CPU::GetPendingTicks();
}

GlobalTicks TimingEvents::GetEventRunTickCounter()
{
  return s_state.event_run_tick_counter;
}

void TimingEvents::Initialize()
{
  Reset();
}

void TimingEvents::Reset()
{
  s_state.global_tick_counter = 0;
  s_state.event_run_tick_counter = 0;
}

void TimingEvents::Shutdown()
{
  Assert(s_state.active_event_count == 0);
}

void TimingEvents::UpdateCPUDowncount()
{
  DebugAssert(s_state.active_events_head->m_next_run_time >= s_state.global_tick_counter);
  const u32 event_downcount =
    static_cast<u32>(s_state.active_events_head->m_next_run_time - s_state.global_tick_counter);
  CPU::g_state.downcount = CPU::HasPendingInterrupt() ? 0 : event_downcount;
}

TimingEvent** TimingEvents::GetHeadEventPtr()
{
  return &s_state.active_events_head;
}

void TimingEvents::SortEvent(TimingEvent* event)
{
  const GlobalTicks event_runtime = event->m_next_run_time;

  if (event->prev && event->prev->m_next_run_time > event_runtime)
  {
    // move backwards
    TimingEvent* current = event->prev;
    while (current && current->m_next_run_time > event_runtime)
      current = current->prev;

    // unlink
    if (event->prev)
      event->prev->next = event->next;
    else
      s_state.active_events_head = event->next;
    if (event->next)
      event->next->prev = event->prev;
    else
      s_state.active_events_tail = event->prev;

    // insert after current
    if (current)
    {
      event->next = current->next;
      if (current->next)
        current->next->prev = event;
      else
        s_state.active_events_tail = event;

      event->prev = current;
      current->next = event;
    }
    else
    {
      // insert at front
      DebugAssert(s_state.active_events_head);
      s_state.active_events_head->prev = event;
      event->prev = nullptr;
      event->next = s_state.active_events_head;
      s_state.active_events_head = event;
      UpdateCPUDowncount();
    }
  }
  else if (event->next && event_runtime > event->next->m_next_run_time)
  {
    // move forwards
    TimingEvent* current = event->next;
    while (current && event_runtime > current->m_next_run_time)
      current = current->next;

    // unlink
    if (event->prev)
    {
      event->prev->next = event->next;
    }
    else
    {
      s_state.active_events_head = event->next;
      if (!s_state.current_event)
        UpdateCPUDowncount();
    }
    if (event->next)
      event->next->prev = event->prev;
    else
      s_state.active_events_tail = event->prev;

    // insert before current
    if (current)
    {
      event->next = current;
      event->prev = current->prev;

      if (current->prev)
      {
        current->prev->next = event;
      }
      else
      {
        s_state.active_events_head = event;
        if (!s_state.current_event)
          UpdateCPUDowncount();
      }

      current->prev = event;
    }
    else
    {
      // insert at back
      DebugAssert(s_state.active_events_tail);
      s_state.active_events_tail->next = event;
      event->next = nullptr;
      event->prev = s_state.active_events_tail;
      s_state.active_events_tail = event;
    }
  }
}

void TimingEvents::AddActiveEvent(TimingEvent* event)
{
  DebugAssert(!event->prev && !event->next);
  s_state.active_event_count++;

  const GlobalTicks event_runtime = event->m_next_run_time;
  TimingEvent* current = nullptr;
  TimingEvent* next = s_state.active_events_head;
  while (next && event_runtime > next->m_next_run_time)
  {
    current = next;
    next = next->next;
  }

  if (!next)
  {
    // new tail
    event->prev = s_state.active_events_tail;
    if (s_state.active_events_tail)
    {
      s_state.active_events_tail->next = event;
      s_state.active_events_tail = event;
    }
    else
    {
      // first event
      s_state.active_events_tail = event;
      s_state.active_events_head = event;
      UpdateCPUDowncount();
    }
  }
  else if (!current)
  {
    // new head
    event->next = s_state.active_events_head;
    s_state.active_events_head->prev = event;
    s_state.active_events_head = event;
    UpdateCPUDowncount();
  }
  else
  {
    // inbetween current < event > next
    event->prev = current;
    event->next = next;
    current->next = event;
    next->prev = event;
  }
}

void TimingEvents::RemoveActiveEvent(TimingEvent* event)
{
  DebugAssert(s_state.active_event_count > 0);

  if (event->next)
  {
    event->next->prev = event->prev;
  }
  else
  {
    s_state.active_events_tail = event->prev;
  }

  if (event->prev)
  {
    event->prev->next = event->next;
  }
  else
  {
    s_state.active_events_head = event->next;
    if (s_state.active_events_head && !s_state.current_event)
      UpdateCPUDowncount();
  }

  event->prev = nullptr;
  event->next = nullptr;

  s_state.active_event_count--;
}

void TimingEvents::SortEvents()
{
  llvm::SmallVector<TimingEvent*, 16> events;
  events.reserve(s_state.active_event_count);

  TimingEvent* next = s_state.active_events_head;
  while (next)
  {
    TimingEvent* current = next;
    events.push_back(current);
    next = current->next;
    current->prev = nullptr;
    current->next = nullptr;
  }

  s_state.active_events_head = nullptr;
  s_state.active_events_tail = nullptr;
  s_state.active_event_count = 0;

  for (TimingEvent* event : events)
    AddActiveEvent(event);
}

static TimingEvent* TimingEvents::FindActiveEvent(const std::string_view name)
{
  for (TimingEvent* event = s_state.active_events_head; event; event = event->next)
  {
    if (event->GetName() == name)
      return event;
  }

  return nullptr;
}

bool TimingEvents::IsRunningEvents()
{
  return (s_state.current_event != nullptr);
}

void TimingEvents::CancelRunningEvent()
{
  TimingEvent* const event = s_state.current_event;
  if (!event)
    return;

  // Might need to sort it, since we're bailing out.
  if (event->IsActive())
  {
    event->m_next_run_time = s_state.current_event_next_run_time;
    SortEvent(event);
  }

  s_state.current_event = nullptr;
}

ALWAYS_INLINE_RELEASE void TimingEvents::CommitGlobalTicks(const GlobalTicks new_global_ticks)
{
  s_state.event_run_tick_counter = new_global_ticks;

  do
  {
    TimingEvent* event = s_state.active_events_head;
    s_state.global_tick_counter = std::min(new_global_ticks, event->m_next_run_time);

    // Now we can actually run the callbacks.
    while (s_state.global_tick_counter >= event->m_next_run_time)
    {
      s_state.current_event = event;

      // Factor late time into the time for the next invocation.
      const TickCount ticks_late = static_cast<TickCount>(s_state.global_tick_counter - event->m_next_run_time);
      const TickCount ticks_to_execute = static_cast<TickCount>(s_state.global_tick_counter - event->m_last_run_time);

      // Why don't we modify event->m_downcount directly? Because otherwise the event list won't be sorted.
      // Adding the interval may cause this event to have a greater downcount than the next, and a new event
      // may be inserted at the front, despite having a higher downcount than the next.
      s_state.current_event_next_run_time = event->m_next_run_time + static_cast<u32>(event->m_interval);
      event->m_last_run_time = s_state.global_tick_counter;

      // The cycles_late is only an indicator, it doesn't modify the cycles to execute.
      event->m_callback(event->m_callback_param, ticks_to_execute, ticks_late);
      if (event->m_active)
      {
        event->m_next_run_time = s_state.current_event_next_run_time;
        SortEvent(event);
      }

      event = s_state.active_events_head;
    }
  } while (new_global_ticks > s_state.global_tick_counter);
  s_state.current_event = nullptr;
}

void TimingEvents::RunEvents()
{
  DebugAssert(!s_state.current_event);
  DebugAssert(CPU::GetPendingTicks() >= CPU::g_state.downcount);

  do
  {
    const GlobalTicks new_global_ticks =
      s_state.event_run_tick_counter + static_cast<GlobalTicks>(CPU::GetPendingTicks());
    if (new_global_ticks >= s_state.active_events_head->m_next_run_time)
    {
      CPU::ResetPendingTicks();
      CommitGlobalTicks(new_global_ticks);
    }

    if (CPU::HasPendingInterrupt())
      CPU::DispatchInterrupt();

    UpdateCPUDowncount();
  } while (CPU::GetPendingTicks() >= CPU::g_state.downcount);
}

void TimingEvents::CommitLeftoverTicks()
{
#ifdef _DEBUG
  if (s_state.event_run_tick_counter > s_state.global_tick_counter)
    DEV_LOG("Late-running {} ticks before execution", s_state.event_run_tick_counter - s_state.global_tick_counter);
#endif

  CommitGlobalTicks(s_state.event_run_tick_counter);

  if (CPU::HasPendingInterrupt())
    CPU::DispatchInterrupt();

  UpdateCPUDowncount();
}

bool TimingEvents::DoState(StateWrapper& sw)
{
  if (sw.GetVersion() < 71) [[unlikely]]
  {
    u32 old_global_tick_counter = 0;
    sw.Do(&old_global_tick_counter);
    s_state.global_tick_counter = static_cast<GlobalTicks>(old_global_tick_counter);
    s_state.event_run_tick_counter = s_state.global_tick_counter;

    // Load timestamps for the clock events.
    // Any oneshot events should be recreated by the load state method, so we can fix up their times here.
    u32 event_count = 0;
    sw.Do(&event_count);

    for (u32 i = 0; i < event_count; i++)
    {
      TinyString event_name;
      TickCount downcount, time_since_last_run, period, interval;
      sw.Do(&event_name);
      sw.Do(&downcount);
      sw.Do(&time_since_last_run);
      sw.Do(&period);
      sw.Do(&interval);
      if (sw.HasError())
        return false;

      TimingEvent* event = FindActiveEvent(event_name);
      if (!event)
      {
        WARNING_LOG("Save state has event '{}', but couldn't find this event when loading.", event_name);
        continue;
      }

      event->m_next_run_time = s_state.global_tick_counter + static_cast<u32>(downcount);
      event->m_last_run_time = s_state.global_tick_counter - static_cast<u32>(time_since_last_run);
      event->m_period = period;
      event->m_interval = interval;
    }

    if (sw.GetVersion() < 43) [[unlikely]]
    {
      u32 last_event_run_time = 0;
      sw.Do(&last_event_run_time);
    }

    DEBUG_LOG("Loaded {} events from save state.", event_count);
    s_state.current_event = nullptr;

    // Add pending ticks to the CPU, this'll happen if we saved state when we weren't paused.
    const TickCount pending_ticks =
      static_cast<TickCount>(s_state.event_run_tick_counter - s_state.global_tick_counter);
    DebugAssert(pending_ticks >= 0);
    CPU::AddPendingTicks(pending_ticks);
    SortEvents();
    UpdateCPUDowncount();
  }
  else
  {
    sw.Do(&s_state.global_tick_counter);
    sw.Do(&s_state.event_run_tick_counter);

    if (sw.IsReading())
    {
      // Load timestamps for the clock events.
      // Any oneshot events should be recreated by the load state method, so we can fix up their times here.
      u32 event_count = 0;
      sw.Do(&event_count);

      for (u32 i = 0; i < event_count; i++)
      {
        TinyString event_name;
        GlobalTicks next_run_time, last_run_time;
        TickCount period, interval;
        sw.Do(&event_name);
        sw.Do(&next_run_time);
        sw.Do(&last_run_time);
        sw.Do(&period);
        sw.Do(&interval);
        if (sw.HasError())
          return false;

        TimingEvent* event = FindActiveEvent(event_name);
        if (!event)
        {
          WARNING_LOG("Save state has event '{}', but couldn't find this event when loading.", event_name);
          continue;
        }

        event->m_next_run_time = next_run_time;
        event->m_last_run_time = last_run_time;
        event->m_period = period;
        event->m_interval = interval;
      }

      DEBUG_LOG("Loaded {} events from save state.", event_count);

      // Even if we're actually running an event, we don't want to set it to a new counter.
      s_state.current_event = nullptr;

      SortEvents();
      UpdateCPUDowncount();
    }
    else
    {
      sw.Do(&s_state.active_event_count);

      for (TimingEvent* event = s_state.active_events_head; event; event = event->next)
      {
        sw.Do(&event->m_name);
        GlobalTicks next_run_time =
          (s_state.current_event == event) ? s_state.current_event_next_run_time : event->m_next_run_time;
        sw.Do(&next_run_time);
        sw.Do(&event->m_last_run_time);
        sw.Do(&event->m_period);
        sw.Do(&event->m_interval);
      }

      DEBUG_LOG("Wrote {} events to save state.", s_state.active_event_count);
    }
  }

  return !sw.HasError();
}

TimingEvent::TimingEvent(const std::string_view name, TickCount period, TickCount interval,
                         TimingEventCallback callback, void* callback_param)
  : m_callback(callback), m_callback_param(callback_param), m_period(period), m_interval(interval), m_name(name)
{
  const GlobalTicks ts = TimingEvents::GetTimestampForNewEvent();
  m_last_run_time = ts;
  m_next_run_time = ts + static_cast<u32>(interval);
}

TimingEvent::~TimingEvent()
{
  DebugAssert(!m_active);
}

TickCount TimingEvent::GetTicksSinceLastExecution() const
{
  // Can be negative if event A->B invoked B early while in the event loop.
  const GlobalTicks ts = TimingEvents::GetTimestampForNewEvent();
  return (ts >= m_last_run_time) ? static_cast<TickCount>(ts - m_last_run_time) : 0;
}

TickCount TimingEvent::GetTicksUntilNextExecution() const
{
  const GlobalTicks ts = TimingEvents::GetTimestampForNewEvent();
  return (ts >= m_next_run_time) ? 0 : static_cast<TickCount>(m_next_run_time - ts);
}

void TimingEvent::Delay(TickCount ticks)
{
  using namespace TimingEvents;

  if (!m_active)
  {
    Panic("Trying to delay an inactive event");
    return;
  }

  DebugAssert(TimingEvents::s_state.current_event != this);

  m_next_run_time += static_cast<u32>(ticks);
  SortEvent(this);
  if (s_state.active_events_head == this)
    UpdateCPUDowncount();
}

void TimingEvent::Schedule(TickCount ticks)
{
  using namespace TimingEvents;

  const GlobalTicks ts = GetTimestampForNewEvent();
  const GlobalTicks next_run_time = ts + static_cast<u32>(ticks);

  // See note in RunEvents().
  s_state.current_event_next_run_time =
    (s_state.current_event == this) ? next_run_time : s_state.current_event_next_run_time;

  if (!m_active)
  {
    // Event is going active, so we want it to only execute ticks from the current timestamp.
    m_next_run_time = next_run_time;
    m_last_run_time = ts;
    m_active = true;
    AddActiveEvent(this);
  }
  else
  {
    // Event is already active, so we leave the time since last run alone, and just modify the downcount.
    // If this is a call from an IO handler for example, re-sort the event queue.
    if (s_state.current_event != this)
    {
      m_next_run_time = next_run_time;
      SortEvent(this);
      if (s_state.active_events_head == this)
        UpdateCPUDowncount();
    }
  }
}

void TimingEvent::SetIntervalAndSchedule(TickCount ticks)
{
  DebugAssert(ticks > 0);
  SetInterval(ticks);
  Schedule(ticks);
}

void TimingEvent::SetPeriodAndSchedule(TickCount ticks)
{
  SetPeriod(ticks);
  SetInterval(ticks);
  Schedule(ticks);
}

void TimingEvent::InvokeEarly(bool force /* = false */)
{
  using namespace TimingEvents;

  if (!m_active)
    return;

  // Might happen due to other InvokeEarly()'s mid event loop.
  const GlobalTicks ts = GetTimestampForNewEvent();
  if (ts <= m_last_run_time)
    return;

  // Shouldn't be invoking early when we're the current event running.
  // TODO: Make DebugAssert instead.
  Assert(s_state.current_event != this);

  const TickCount ticks_to_execute = static_cast<TickCount>(ts - m_last_run_time);
  if (!force && ticks_to_execute < m_period)
    return;

  m_next_run_time = ts + static_cast<u32>(m_interval);
  m_last_run_time = ts;

  // Since we've changed the downcount, we need to re-sort the events.
  SortEvent(this);
  if (s_state.active_events_head == this)
    UpdateCPUDowncount();

  m_callback(m_callback_param, ticks_to_execute, 0);
}

void TimingEvent::Activate()
{
  using namespace TimingEvents;

  if (m_active)
    return;

  const GlobalTicks ts = GetTimestampForNewEvent();
  const GlobalTicks next_run_time = ts + static_cast<u32>(m_interval);
  m_next_run_time = next_run_time;
  m_last_run_time = ts;

  s_state.current_event_next_run_time =
    (s_state.current_event == this) ? next_run_time : s_state.current_event_next_run_time;

  m_active = true;
  AddActiveEvent(this);
}

void TimingEvent::Deactivate()
{
  using namespace TimingEvents;

  if (!m_active)
    return;

  m_active = false;
  RemoveActiveEvent(this);
}
