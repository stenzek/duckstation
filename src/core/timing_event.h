#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "types.h"

class System;
class TimingEvent;

// Event callback type. Second parameter is the number of cycles the event was executed "late".
using TimingEventCallback = std::function<void(TickCount ticks, TickCount ticks_late)>;

class TimingEvent
{
  friend System;

public:
  TimingEvent(System* system, std::string name, TickCount period, TickCount interval, TimingEventCallback callback);
  ~TimingEvent();

  System* GetSystem() const { return m_system; }
  const std::string& GetName() const { return m_name; }
  bool IsActive() const { return m_active; }

  // Returns the number of ticks between each event.
  TickCount GetPeriod() const { return m_period; }
  TickCount GetInterval() const { return m_interval; }

  TickCount GetDowncount() const { return m_downcount; }

  // Includes pending time.
  TickCount GetTicksSinceLastExecution() const;
  TickCount GetTicksUntilNextExecution() const;

  void Schedule(TickCount ticks);
  void SetIntervalAndSchedule(TickCount ticks);
  void SetPeriodAndSchedule(TickCount ticks);

  void Reset();

  // Services the event with the current accmulated time. If force is set, when not enough time is pending to
  // simulate a single cycle, the callback will still be invoked, otherwise it won't be.
  void InvokeEarly(bool force = false);

  // Deactivates the event, preventing it from firing again.
  // Do not call within a callback, return Deactivate instead.
  void Activate();
  void Deactivate();

  ALWAYS_INLINE void SetState(bool active)
  {
    if (active)
      Activate();
    else
      Deactivate();
  }

  // Directly alters the downcount of the event.
  void SetDowncount(TickCount downcount);

  // Directly alters the interval of the event.
  void SetInterval(TickCount interval) { m_interval = interval; }
  void SetPeriod(TickCount period) { m_period = period; }

private:
  TickCount m_downcount;
  TickCount m_time_since_last_run;
  TickCount m_period;
  TickCount m_interval;

  TimingEventCallback m_callback;
  System* m_system;
  std::string m_name;
  bool m_active;
};
