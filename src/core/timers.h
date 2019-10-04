#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>

class StateWrapper;

class System;
class InterruptController;

class Timers
{
public:
  Timers();
  ~Timers();

  bool Initialize(System* system, InterruptController* interrupt_controller);
  void Reset();
  bool DoState(StateWrapper& sw);

  void SetGate(u32 timer, bool state);

  // dot clock/hblank/sysclk div 8
  bool IsUsingExternalClock(u32 timer) const { return m_states[timer].external_counting_enabled; }
  void AddTicks(u32 timer, u32 ticks);
  void AddSystemTicks(u32 ticks);

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

private:
  static constexpr u32 NUM_TIMERS = 3;

  enum class SyncMode : u8
  {
    PauseOnGate = 0,
    ResetOnGate = 1,
    ResetAndRunOnGate = 2,
    FreeRunOnGate = 3
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
    BitField<u32, bool, 7, 1> irq_pulse;
    BitField<u32, u8, 8, 2> clock_source;
    BitField<u32, bool, 10, 1> interrupt_request;
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
  };

  void UpdateCountingEnabled(CounterState& cs);

  void UpdateDowncount();
  u32 GetSystemTicksForTimerTicks(u32 timer) const;

  System* m_system = nullptr;
  InterruptController* m_interrupt_controller = nullptr;

  std::array<CounterState, NUM_TIMERS> m_states{};
};
