#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "types.h"

class StateWrapper;

// Event callback type. Second parameter is the number of cycles the event was executed "late".
using TimingEventCallback = void (*)(void* param, TickCount ticks, TickCount ticks_late);

class TimingEvent
{
public:
  TimingEvent(std::string name, TickCount period, TickCount interval, TimingEventCallback callback,
              void* callback_param);
  ~TimingEvent();

  ALWAYS_INLINE const std::string& GetName() const { return m_name; }
  ALWAYS_INLINE bool IsActive() const { return m_active; }

  // Returns the number of ticks between each event.
  ALWAYS_INLINE TickCount GetPeriod() const { return m_period; }
  ALWAYS_INLINE TickCount GetInterval() const { return m_interval; }
  ALWAYS_INLINE TickCount GetDowncount() const { return m_downcount; }

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

  // Directly alters the interval of the event.
  void SetInterval(TickCount interval) { m_interval = interval; }
  void SetPeriod(TickCount period) { m_period = period; }

  TimingEvent* prev = nullptr;
  TimingEvent* next = nullptr;

  TimingEventCallback m_callback;
  void* m_callback_param;

  TickCount m_downcount;
  TickCount m_time_since_last_run;
  TickCount m_period;
  TickCount m_interval;
  bool m_active = false;

  std::string m_name;
};

namespace TimingEvents {

u32 GetGlobalTickCounter();

void Initialize();
void Reset();
void Shutdown();

/// Creates a new event.
std::unique_ptr<TimingEvent> CreateTimingEvent(std::string name, TickCount period, TickCount interval,
                                               TimingEventCallback callback, void* callback_param, bool activate);

/// Serialization.
bool DoState(StateWrapper& sw);

void RunEvents();

void UpdateCPUDowncount();

TimingEvent** GetHeadEventPtr();

} // namespace TimingEvents