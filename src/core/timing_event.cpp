#include "timing_event.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "cpu_core.h"
#include "system.h"
Log_SetChannel(TimingEvents);

namespace TimingEvents {

static std::vector<TimingEvent*> s_events;
static u32 s_global_tick_counter = 0;
static u32 s_last_event_run_time = 0;
static bool s_running_events = false;
static bool s_events_need_sorting = false;

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
  s_last_event_run_time = 0;
}

void Shutdown()
{
  Assert(s_events.empty());
}

std::unique_ptr<TimingEvent> CreateTimingEvent(std::string name, TickCount period, TickCount interval,
                                               TimingEventCallback callback, bool activate)
{
  std::unique_ptr<TimingEvent> event =
    std::make_unique<TimingEvent>(std::move(name), period, interval, std::move(callback));
  if (activate)
    event->Activate();

  return event;
}

void UpdateCPUDowncount()
{
  if (!CPU::g_state.frame_done)
    CPU::g_state.downcount = s_events[0]->GetDowncount();
}

static bool CompareEvents(const TimingEvent* lhs, const TimingEvent* rhs)
{
  return lhs->GetDowncount() > rhs->GetDowncount();
}

static void AddActiveEvent(TimingEvent* event)
{
  s_events.push_back(event);
  if (!s_running_events)
  {
    std::push_heap(s_events.begin(), s_events.end(), CompareEvents);
    UpdateCPUDowncount();
  }
  else
  {
    s_events_need_sorting = true;
  }
}

static void RemoveActiveEvent(TimingEvent* event)
{
  auto iter = std::find_if(s_events.begin(), s_events.end(), [event](const auto& it) { return event == it; });
  if (iter == s_events.end())
  {
    Panic("Attempt to remove inactive event");
    return;
  }

  s_events.erase(iter);
  if (!s_running_events)
  {
    std::make_heap(s_events.begin(), s_events.end(), CompareEvents);
    if (!s_events.empty())
      UpdateCPUDowncount();
  }
  else
  {
    s_events_need_sorting = true;
  }
}

static TimingEvent* FindActiveEvent(const char* name)
{
  auto iter =
    std::find_if(s_events.begin(), s_events.end(), [&name](auto& ev) { return ev->GetName().compare(name) == 0; });

  return (iter != s_events.end()) ? *iter : nullptr;
}

static void SortEvents()
{
  if (!s_running_events)
  {
    std::make_heap(s_events.begin(), s_events.end(), CompareEvents);
    UpdateCPUDowncount();
  }
  else
  {
    s_events_need_sorting = true;
  }
}

void RunEvents()
{
  DebugAssert(!s_running_events && !s_events.empty());

  s_running_events = true;

  TickCount pending_ticks = (s_global_tick_counter + CPU::GetPendingTicks()) - s_last_event_run_time;
  CPU::ResetPendingTicks();
  while (pending_ticks > 0)
  {
    const TickCount time = std::min(pending_ticks, s_events[0]->GetDowncount());
    s_global_tick_counter += static_cast<u32>(time);
    pending_ticks -= time;

    // Apply downcount to all events.
    // This will result in a negative downcount for those events which are late.
    for (TimingEvent* evt : s_events)
    {
      evt->m_downcount -= time;
      evt->m_time_since_last_run += time;
    }

    // Now we can actually run the callbacks.
    while (s_events.front()->m_downcount <= 0)
    {
      TimingEvent* evt = s_events.front();
      std::pop_heap(s_events.begin(), s_events.end(), CompareEvents);

      // Factor late time into the time for the next invocation.
      const TickCount ticks_late = -evt->m_downcount;
      const TickCount ticks_to_execute = evt->m_time_since_last_run;
      evt->m_downcount += evt->m_interval;
      evt->m_time_since_last_run = 0;

      // The cycles_late is only an indicator, it doesn't modify the cycles to execute.
      evt->m_callback(ticks_to_execute, ticks_late);

      // Place it in the appropriate position in the queue.
      if (s_events_need_sorting)
      {
        // Another event may have been changed by this event, or the interval/downcount changed.
        std::make_heap(s_events.begin(), s_events.end(), CompareEvents);
        s_events_need_sorting = false;
      }
      else
      {
        // Keep the event list in a heap. The event we just serviced will be in the last place,
        // so we can use push_here instead of make_heap, which should be faster.
        std::push_heap(s_events.begin(), s_events.end(), CompareEvents);
      }
    }
  }

  s_last_event_run_time = s_global_tick_counter;
  s_running_events = false;
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

    sw.Do(&s_last_event_run_time);

    Log_DevPrintf("Loaded %u events from save state.", event_count);
    SortEvents();
  }
  else
  {
    u32 event_count = static_cast<u32>(s_events.size());
    sw.Do(&event_count);

    for (TimingEvent* evt : s_events)
    {
      sw.Do(&evt->m_name);
      sw.Do(&evt->m_downcount);
      sw.Do(&evt->m_time_since_last_run);
      sw.Do(&evt->m_period);
      sw.Do(&evt->m_interval);
    }

    sw.Do(&s_last_event_run_time);

    Log_DevPrintf("Wrote %u events to save state.", event_count);
  }

  return !sw.HasError();
}

} // namespace TimingEvents

TimingEvent::TimingEvent(std::string name, TickCount period, TickCount interval, TimingEventCallback callback)
  : m_downcount(interval), m_time_since_last_run(0), m_period(period), m_interval(interval),
    m_callback(std::move(callback)), m_name(std::move(name)), m_active(false)
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
    TimingEvents::SortEvents();
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
  TimingEvents::SortEvents();
}

void TimingEvent::InvokeEarly(bool force /* = false */)
{
  if (!m_active)
    return;

  const TickCount pending_ticks = CPU::GetPendingTicks();
  const TickCount ticks_to_execute = m_time_since_last_run + pending_ticks;
  if (!force && ticks_to_execute < m_period)
    return;

  m_downcount = pending_ticks + m_interval;
  m_time_since_last_run -= ticks_to_execute;
  m_callback(ticks_to_execute, 0);

  // Since we've changed the downcount, we need to re-sort the events.
  TimingEvents::SortEvents();
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
