// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

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

static void SortEvent(TimingEvent* event);
static void AddActiveEvent(TimingEvent* event);
static void RemoveActiveEvent(TimingEvent* event);
static void SortEvents();
static TimingEvent* FindActiveEvent(const std::string_view name);

namespace {
struct TimingEventsState
{
  TimingEvent* active_events_head = nullptr;
  TimingEvent* active_events_tail = nullptr;
  TimingEvent* current_event = nullptr;
  TickCount current_event_new_downcount = 0;
  u32 active_event_count = 0;
  u32 global_tick_counter = 0;
  u32 event_run_tick_counter = 0;
  bool frame_done = false;
};
} // namespace

ALIGN_TO_CACHE_LINE static TimingEventsState s_state;

} // namespace TimingEvents

u32 TimingEvents::GetGlobalTickCounter()
{
  return s_state.global_tick_counter;
}

u32 TimingEvents::GetEventRunTickCounter()
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
}

void TimingEvents::Shutdown()
{
  Assert(s_state.active_event_count == 0);
}

void TimingEvents::UpdateCPUDowncount()
{
  const u32 event_downcount = s_state.active_events_head->GetDowncount();
  CPU::g_state.downcount = CPU::HasPendingInterrupt() ? 0 : event_downcount;
}

TimingEvent** TimingEvents::GetHeadEventPtr()
{
  return &s_state.active_events_head;
}

void TimingEvents::SortEvent(TimingEvent* event)
{
  const TickCount event_downcount = event->m_downcount;

  if (event->prev && event->prev->m_downcount > event_downcount)
  {
    // move backwards
    TimingEvent* current = event->prev;
    while (current && current->m_downcount > event_downcount)
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
  else if (event->next && event_downcount > event->next->m_downcount)
  {
    // move forwards
    TimingEvent* current = event->next;
    while (current && event_downcount > current->m_downcount)
      current = current->next;

    // unlink
    if (event->prev)
    {
      event->prev->next = event->next;
    }
    else
    {
      s_state.active_events_head = event->next;
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

  TimingEvent* current = nullptr;
  TimingEvent* next = s_state.active_events_head;
  while (next && event->m_downcount > next->m_downcount)
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
    if (s_state.active_events_head)
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

void TimingEvents::SetFrameDone()
{
  s_state.frame_done = true;
  CPU::g_state.downcount = 0;
}

void TimingEvents::RunEvents()
{
  DebugAssert(!s_state.current_event);

  do
  {
    TickCount pending_ticks = CPU::GetPendingTicks();
    if (pending_ticks >= s_state.active_events_head->GetDowncount())
    {
      CPU::ResetPendingTicks();
      s_state.event_run_tick_counter = s_state.global_tick_counter + static_cast<u32>(pending_ticks);

      do
      {
        const TickCount time = std::min(pending_ticks, s_state.active_events_head->GetDowncount());
        s_state.global_tick_counter += static_cast<u32>(time);
        pending_ticks -= time;

        // Apply downcount to all events.
        // This will result in a negative downcount for those events which are late.
        for (TimingEvent* event = s_state.active_events_head; event; event = event->next)
        {
          event->m_downcount -= time;
          event->m_time_since_last_run += time;
        }

        // Now we can actually run the callbacks.
        while (s_state.active_events_head->m_downcount <= 0)
        {
          // move it to the end, since that'll likely be its new position
          TimingEvent* event = s_state.active_events_head;
          s_state.current_event = event;

          // Factor late time into the time for the next invocation.
          const TickCount ticks_late = -event->m_downcount;
          const TickCount ticks_to_execute = event->m_time_since_last_run;

          // Why don't we modify event->m_downcount directly? Because otherwise the event list won't be sorted.
          // Adding the interval may cause this event to have a greater downcount than the next, and a new event
          // may be inserted at the front, despite having a higher downcount than the next.
          s_state.current_event_new_downcount = event->m_downcount + event->m_interval;
          event->m_time_since_last_run = 0;

          // The cycles_late is only an indicator, it doesn't modify the cycles to execute.
          event->m_callback(event->m_callback_param, ticks_to_execute, ticks_late);
          if (event->m_active)
          {
            event->m_downcount = s_state.current_event_new_downcount;
            SortEvent(event);
          }
        }
      } while (pending_ticks > 0);

      s_state.current_event = nullptr;
    }

    if (s_state.frame_done)
    {
      s_state.frame_done = false;
      System::FrameDone();
    }

    if (CPU::HasPendingInterrupt())
      CPU::DispatchInterrupt();

    UpdateCPUDowncount();
  } while (CPU::GetPendingTicks() >= CPU::g_state.downcount);
}

bool TimingEvents::DoState(StateWrapper& sw)
{
  sw.Do(&s_state.global_tick_counter);

  if (sw.IsReading())
  {
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

      // Using reschedule is safe here since we call sort afterwards.
      event->m_downcount = downcount;
      event->m_time_since_last_run = time_since_last_run;
      event->m_period = period;
      event->m_interval = interval;
    }

    if (sw.GetVersion() < 43) [[unlikely]]
    {
      u32 last_event_run_time = 0;
      sw.Do(&last_event_run_time);
    }

    DEBUG_LOG("Loaded {} events from save state.", event_count);
    SortEvents();
  }
  else
  {

    sw.Do(&s_state.active_event_count);

    for (TimingEvent* event = s_state.active_events_head; event; event = event->next)
    {
      sw.Do(&event->m_name);
      sw.Do(&event->m_downcount);
      sw.Do(&event->m_time_since_last_run);
      sw.Do(&event->m_period);
      sw.Do(&event->m_interval);
    }

    DEBUG_LOG("Wrote {} events to save state.", s_state.active_event_count);
  }

  return !sw.HasError();
}

TimingEvent::TimingEvent(const std::string_view name, TickCount period, TickCount interval,
                         TimingEventCallback callback, void* callback_param)
  : m_callback(callback), m_callback_param(callback_param), m_downcount(interval), m_time_since_last_run(0),
    m_period(period), m_interval(interval), m_name(name)
{
}

TimingEvent::~TimingEvent()
{
  DebugAssert(!m_active);
}

TickCount TimingEvent::GetTicksSinceLastExecution() const
{
  return CPU::GetPendingTicks() + m_time_since_last_run;
}

TickCount TimingEvent::GetTicksUntilNextExecution() const
{
  return std::max(m_downcount - CPU::GetPendingTicks(), static_cast<TickCount>(0));
}

void TimingEvent::Delay(TickCount ticks)
{
  if (!m_active)
  {
    Panic("Trying to delay an inactive event");
    return;
  }

  m_downcount += ticks;

  DebugAssert(TimingEvents::s_state.current_event != this);
  TimingEvents::SortEvent(this);
  if (TimingEvents::s_state.active_events_head == this)
    TimingEvents::UpdateCPUDowncount();
}

void TimingEvent::Schedule(TickCount ticks)
{
  using namespace TimingEvents;

  const TickCount pending_ticks = CPU::GetPendingTicks();
  const TickCount new_downcount = pending_ticks + ticks;

  // See note in RunEvents().
  s_state.current_event_new_downcount =
    (s_state.current_event == this) ? new_downcount : s_state.current_event_new_downcount;

  if (!m_active)
  {
    // Event is going active, so we want it to only execute ticks from the current timestamp.
    m_downcount = new_downcount;
    m_time_since_last_run = -pending_ticks;
    m_active = true;
    AddActiveEvent(this);
  }
  else
  {
    // Event is already active, so we leave the time since last run alone, and just modify the downcount.
    // If this is a call from an IO handler for example, re-sort the event queue.
    if (s_state.current_event != this)
    {
      m_downcount = new_downcount;
      SortEvent(this);
      if (s_state.active_events_head == this)
        UpdateCPUDowncount();
    }
  }
}

void TimingEvent::SetIntervalAndSchedule(TickCount ticks)
{
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
  if (!m_active)
    return;

  const TickCount pending_ticks = CPU::GetPendingTicks();
  const TickCount ticks_to_execute = m_time_since_last_run + pending_ticks;
  if ((!force && ticks_to_execute < m_period) || ticks_to_execute <= 0)
    return;

  // Shouldn't be invoking early when we're the current event running.
  DebugAssert(TimingEvents::s_state.current_event != this);

  m_downcount = pending_ticks + m_interval;
  m_time_since_last_run -= ticks_to_execute;
  m_callback(m_callback_param, ticks_to_execute, 0);

  // Since we've changed the downcount, we need to re-sort the events.
  TimingEvents::SortEvent(this);
  if (TimingEvents::s_state.active_events_head == this)
    TimingEvents::UpdateCPUDowncount();
}

void TimingEvent::Activate()
{
  if (m_active)
    return;

  // leave the downcount intact
  // if we're running events, this is going to be zero, so no effect
  const TickCount pending_ticks = CPU::GetPendingTicks();
  m_downcount += pending_ticks;
  m_time_since_last_run -= pending_ticks;

  m_active = true;
  TimingEvents::AddActiveEvent(this);
}

void TimingEvent::Deactivate()
{
  if (!m_active)
    return;

  const TickCount pending_ticks = CPU::GetPendingTicks();
  m_downcount -= pending_ticks;
  m_time_since_last_run += pending_ticks;

  m_active = false;
  TimingEvents::RemoveActiveEvent(this);
}
