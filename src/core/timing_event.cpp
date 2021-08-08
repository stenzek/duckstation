#include "timing_event.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "cpu_core_private.h"
#include "system.h"
Log_SetChannel(TimingEvents);

namespace TimingEvents {

static TimingEvent* s_active_events_head;
static TimingEvent* s_active_events_tail;
static TimingEvent* s_current_event = nullptr;
static u32 s_active_event_count = 0;
static u32 s_global_tick_counter = 0;

u32 GetGlobalTickCounter()
{
  return s_global_tick_counter;
}

void Initialize()
{
  Reset();
}

void Reset()
{
  s_global_tick_counter = 0;
}

void Shutdown()
{
  Assert(s_active_event_count == 0);
}

std::unique_ptr<TimingEvent> CreateTimingEvent(std::string name, TickCount period, TickCount interval,
                                               TimingEventCallback callback, void* callback_param, bool activate)
{
  std::unique_ptr<TimingEvent> event =
    std::make_unique<TimingEvent>(std::move(name), period, interval, callback, callback_param);
  if (activate)
    event->Activate();

  return event;
}

void UpdateCPUDowncount()
{
  if (!CPU::g_state.frame_done && (!CPU::HasPendingInterrupt() || CPU::g_using_interpreter))
  {
    CPU::g_state.downcount = s_active_events_head->GetDowncount();
  }
}

TimingEvent** GetHeadEventPtr()
{
  return &s_active_events_head;
}

static void SortEvent(TimingEvent* event)
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
      s_active_events_head = event->next;
    if (event->next)
      event->next->prev = event->prev;
    else
      s_active_events_tail = event->prev;

    // insert after current
    if (current)
    {
      event->next = current->next;
      if (current->next)
        current->next->prev = event;
      else
        s_active_events_tail = event;

      event->prev = current;
      current->next = event;
    }
    else
    {
      // insert at front
      DebugAssert(s_active_events_head);
      s_active_events_head->prev = event;
      event->prev = nullptr;
      event->next = s_active_events_head;
      s_active_events_head = event;
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
      event->prev->next = event->next;
    else
      s_active_events_head = event->next;
    if (event->next)
      event->next->prev = event->prev;
    else
      s_active_events_tail = event->prev;

    // insert before current
    if (current)
    {
      event->next = current;
      event->prev = current->prev;

      if (current->prev)
        current->prev->next = event;
      else
        s_active_events_head = event;

      current->prev = event;
    }
    else
    {
      // insert at back
      DebugAssert(s_active_events_tail);
      s_active_events_tail->next = event;
      event->next = nullptr;
      event->prev = s_active_events_tail;
      s_active_events_tail = event;
    }
  }
}

static void AddActiveEvent(TimingEvent* event)
{
  DebugAssert(!event->prev && !event->next);
  s_active_event_count++;

  TimingEvent* current = nullptr;
  TimingEvent* next = s_active_events_head;
  while (next && event->m_downcount > next->m_downcount)
  {
    current = next;
    next = next->next;
  }

  if (!next)
  {
    // new tail
    event->prev = s_active_events_tail;
    if (s_active_events_tail)
    {
      s_active_events_tail->next = event;
      s_active_events_tail = event;
    }
    else
    {
      // first event
      s_active_events_tail = event;
      s_active_events_head = event;
      UpdateCPUDowncount();
    }
  }
  else if (!current)
  {
    // new head
    event->next = s_active_events_head;
    s_active_events_head->prev = event;
    s_active_events_head = event;
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

static void RemoveActiveEvent(TimingEvent* event)
{
  DebugAssert(s_active_event_count > 0);

  if (event->next)
  {
    event->next->prev = event->prev;
  }
  else
  {
    s_active_events_tail = event->prev;
  }

  if (event->prev)
  {
    event->prev->next = event->next;
  }
  else
  {
    s_active_events_head = event->next;
    if (s_active_events_head)
      UpdateCPUDowncount();
  }

  event->prev = nullptr;
  event->next = nullptr;

  s_active_event_count--;
}

static void SortEvents()
{
  std::vector<TimingEvent*> events;
  events.reserve(s_active_event_count);

  TimingEvent* next = s_active_events_head;
  while (next)
  {
    TimingEvent* current = next;
    events.push_back(current);
    next = current->next;
    current->prev = nullptr;
    current->next = nullptr;
  }

  s_active_events_head = nullptr;
  s_active_events_tail = nullptr;
  s_active_event_count = 0;

  for (TimingEvent* event : events)
    AddActiveEvent(event);
}

static TimingEvent* FindActiveEvent(const char* name)
{
  for (TimingEvent* event = s_active_events_head; event; event = event->next)
  {
    if (event->GetName().compare(name) == 0)
      return event;
  }

  return nullptr;
}

void RunEvents()
{
  DebugAssert(!s_current_event);

  TickCount pending_ticks = CPU::GetPendingTicks();
  CPU::ResetPendingTicks();
  while (pending_ticks > 0)
  {
    const TickCount time = std::min(pending_ticks, s_active_events_head->GetDowncount());
    s_global_tick_counter += static_cast<u32>(time);
    pending_ticks -= time;

    // Apply downcount to all events.
    // This will result in a negative downcount for those events which are late.
    for (TimingEvent* event = s_active_events_head; event; event = event->next)
    {
      event->m_downcount -= time;
      event->m_time_since_last_run += time;
    }

    // Now we can actually run the callbacks.
    while (s_active_events_head->m_downcount <= 0)
    {
      // move it to the end, since that'll likely be its new position
      TimingEvent* event = s_active_events_head;
      s_current_event = event;

      // Factor late time into the time for the next invocation.
      const TickCount ticks_late = -event->m_downcount;
      const TickCount ticks_to_execute = event->m_time_since_last_run;
      event->m_downcount += event->m_interval;
      event->m_time_since_last_run = 0;

      // The cycles_late is only an indicator, it doesn't modify the cycles to execute.
      event->m_callback(event->m_callback_param, ticks_to_execute, ticks_late);
      if (event->m_active)
        SortEvent(event);
    }
  }

  s_current_event = nullptr;
  UpdateCPUDowncount();
}

bool DoState(StateWrapper& sw)
{
  sw.Do(&s_global_tick_counter);

  if (sw.IsReading())
  {
    // Load timestamps for the clock events.
    // Any oneshot events should be recreated by the load state method, so we can fix up their times here.
    u32 event_count = 0;
    sw.Do(&event_count);

    for (u32 i = 0; i < event_count; i++)
    {
      std::string event_name;
      TickCount downcount, time_since_last_run, period, interval;
      sw.Do(&event_name);
      sw.Do(&downcount);
      sw.Do(&time_since_last_run);
      sw.Do(&period);
      sw.Do(&interval);
      if (sw.HasError())
        return false;

      TimingEvent* event = FindActiveEvent(event_name.c_str());
      if (!event)
      {
        Log_WarningPrintf("Save state has event '%s', but couldn't find this event when loading.", event_name.c_str());
        continue;
      }

      // Using reschedule is safe here since we call sort afterwards.
      event->m_downcount = downcount;
      event->m_time_since_last_run = time_since_last_run;
      event->m_period = period;
      event->m_interval = interval;
    }

    if (sw.GetVersion() < 43)
    {
      u32 last_event_run_time = 0;
      sw.Do(&last_event_run_time);
    }

    Log_DevPrintf("Loaded %u events from save state.", event_count);
    SortEvents();
  }
  else
  {

    sw.Do(&s_active_event_count);

    for (TimingEvent* event = s_active_events_head; event; event = event->next)
    {
      sw.Do(&event->m_name);
      sw.Do(&event->m_downcount);
      sw.Do(&event->m_time_since_last_run);
      sw.Do(&event->m_period);
      sw.Do(&event->m_interval);
    }

    Log_DevPrintf("Wrote %u events to save state.", s_active_event_count);
  }

  return !sw.HasError();
}

} // namespace TimingEvents

TimingEvent::TimingEvent(std::string name, TickCount period, TickCount interval, TimingEventCallback callback,
                         void* callback_param)
  : m_callback(callback), m_callback_param(callback_param), m_downcount(interval), m_time_since_last_run(0),
    m_period(period), m_interval(interval), m_name(std::move(name))
{
}

TimingEvent::~TimingEvent()
{
  if (m_active)
    TimingEvents::RemoveActiveEvent(this);
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

  DebugAssert(TimingEvents::s_current_event != this);
  TimingEvents::SortEvent(this);
}

void TimingEvent::Schedule(TickCount ticks)
{
  const TickCount pending_ticks = CPU::GetPendingTicks();
  m_downcount = pending_ticks + ticks;

  if (!m_active)
  {
    // Event is going active, so we want it to only execute ticks from the current timestamp.
    m_time_since_last_run = -pending_ticks;
    m_active = true;
    TimingEvents::AddActiveEvent(this);
  }
  else
  {
    // Event is already active, so we leave the time since last run alone, and just modify the downcount.
    // If this is a call from an IO handler for example, re-sort the event queue.
    if (TimingEvents::s_current_event != this)
      TimingEvents::SortEvent(this);
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

void TimingEvent::Reset()
{
  if (!m_active)
    return;

  m_downcount = m_interval;
  m_time_since_last_run = 0;
  if (TimingEvents::s_current_event != this)
    TimingEvents::SortEvent(this);
}

void TimingEvent::InvokeEarly(bool force /* = false */)
{
  if (!m_active)
    return;

  const TickCount pending_ticks = CPU::GetPendingTicks();
  const TickCount ticks_to_execute = m_time_since_last_run + pending_ticks;
  if ((!force && ticks_to_execute < m_period) || ticks_to_execute <= 0)
    return;

  m_downcount = pending_ticks + m_interval;
  m_time_since_last_run -= ticks_to_execute;
  m_callback(m_callback_param, ticks_to_execute, 0);

  // Since we've changed the downcount, we need to re-sort the events.
  DebugAssert(TimingEvents::s_current_event != this);
  TimingEvents::SortEvent(this);
}

void TimingEvent::Activate()
{
  if (m_active)
    return;

  // leave the downcount intact
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
