#include "timing_event.h"
#include "common/assert.h"
#include "cpu_core.h"
#include "system.h"

TimingEvent::TimingEvent(System* system, std::string name, TickCount period, TickCount interval,
                         TimingEventCallback callback)
  : m_downcount(interval), m_time_since_last_run(0), m_period(period), m_interval(interval),
    m_callback(std::move(callback)), m_system(system), m_name(std::move(name)), m_active(false)
{
}

TimingEvent::~TimingEvent()
{
  if (m_active)
    m_system->RemoveActiveEvent(this);
}

TickCount TimingEvent::GetTicksSinceLastExecution() const
{
  return m_system->m_cpu->GetPendingTicks() + m_time_since_last_run;
}

TickCount TimingEvent::GetTicksUntilNextExecution() const
{
  return std::max(m_downcount - m_system->m_cpu->GetPendingTicks(), static_cast<TickCount>(0));
}

void TimingEvent::Schedule(TickCount ticks)
{
  const TickCount pending_ticks = m_system->m_cpu->GetPendingTicks();
  m_downcount = pending_ticks + ticks;

  if (!m_active)
  {
    // Event is going active, so we want it to only execute ticks from the current timestamp.
    m_time_since_last_run = -pending_ticks;
    m_active = true;
    m_system->AddActiveEvent(this);
  }
  else
  {
    // Event is already active, so we leave the time since last run alone, and just modify the downcount.
    // If this is a call from an IO handler for example, re-sort the event queue.
    m_system->SortEvents();
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
  m_system->SortEvents();
}

void TimingEvent::InvokeEarly(bool force /* = false */)
{
  if (!m_active)
    return;

  const TickCount pending_ticks = m_system->m_cpu->GetPendingTicks();
  const TickCount ticks_to_execute = m_time_since_last_run + pending_ticks;
  if (!force && ticks_to_execute < m_period)
    return;

  m_downcount = pending_ticks + m_interval;
  m_time_since_last_run -= ticks_to_execute;
  m_callback(ticks_to_execute, 0);

  // Since we've changed the downcount, we need to re-sort the events.
  m_system->SortEvents();
}

void TimingEvent::Activate()
{
  if (m_active)
    return;

  // leave the downcount intact
  const TickCount pending_ticks = m_system->m_cpu->GetPendingTicks();
  m_downcount += pending_ticks;
  m_time_since_last_run -= pending_ticks;

  m_active = true;
  m_system->AddActiveEvent(this);
}

void TimingEvent::Deactivate()
{
  if (!m_active)
    return;

  const TickCount pending_ticks = m_system->m_cpu->GetPendingTicks();
  m_downcount -= pending_ticks;
  m_time_since_last_run += pending_ticks;

  m_active = false;
  m_system->RemoveActiveEvent(this);
}
