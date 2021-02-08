#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class TimingEvent;
class GPU;

class Timers final
{
public:
  Timers();
  ~Timers();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  void SetGate(u32 timer, bool state);

  void DrawDebugStateWindow();

  void CPUClocksChanged();

  // dot clock/hblank/sysclk div 8
  ALWAYS_INLINE bool IsUsingExternalClock(u32 timer) const { return m_states[timer].external_counting_enabled; }
  ALWAYS_INLINE bool IsSyncEnabled(u32 timer) const { return m_states[timer].mode.sync_enable; }

  // queries for GPU
  ALWAYS_INLINE bool IsExternalIRQEnabled(u32 timer) const
  {
    const CounterState& cs = m_states[timer];
    return (cs.external_counting_enabled && (cs.mode.bits & ((1u << 4) | (1u << 5))) != 0);
  }

  TickCount GetTicksUntilIRQ(u32 timer) const;

  void AddTicks(u32 timer, TickCount ticks);

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

  void UpdateCountingEnabled(CounterState& cs);
  void CheckForIRQ(u32 index, u32 old_counter);
  void UpdateIRQ(u32 index);

  void AddSysClkTicks(TickCount sysclk_ticks);

  TickCount GetTicksUntilNextInterrupt() const;
  void UpdateSysClkEvent();

  std::unique_ptr<TimingEvent> m_sysclk_event;

  std::array<CounterState, NUM_TIMERS> m_states{};
  TickCount m_syclk_ticks_carry = 0; // 0 unless overclocking is enabled
  u32 m_sysclk_div_8_carry = 0;      // partial ticks for timer 3 with sysclk/8
};

extern Timers g_timers;