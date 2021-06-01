#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "multitap.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class TimingEvent;
class Controller;
class MemoryCard;

class Pad final
{
public:
  Pad();
  ~Pad();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  Controller* GetController(u32 slot) const { return m_controllers[slot].get(); }
  void SetController(u32 slot, std::unique_ptr<Controller> dev);

  MemoryCard* GetMemoryCard(u32 slot) { return m_memory_cards[slot].get(); }
  void SetMemoryCard(u32 slot, std::unique_ptr<MemoryCard> dev);
  std::unique_ptr<MemoryCard> RemoveMemoryCard(u32 slot);

  Multitap* GetMultitap(u32 slot) { return &m_multitaps[slot]; };

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  ALWAYS_INLINE bool IsTransmitting() const { return m_state != State::Idle; }

private:
  static constexpr u32 NUM_SLOTS = 2;

  enum class State : u32
  {
    Idle,
    Transmitting,
    WaitingForACK
  };

  enum class ActiveDevice : u8
  {
    None,
    Controller,
    MemoryCard,
    Multitap
  };

  union JOY_CTRL
  {
    u16 bits;

    BitField<u16, bool, 0, 1> TXEN;
    BitField<u16, bool, 1, 1> SELECT;
    BitField<u16, bool, 2, 1> RXEN;
    BitField<u16, bool, 4, 1> ACK;
    BitField<u16, bool, 6, 1> RESET;
    BitField<u16, u8, 8, 2> RXIMODE;
    BitField<u16, bool, 10, 1> TXINTEN;
    BitField<u16, bool, 11, 1> RXINTEN;
    BitField<u16, bool, 12, 1> ACKINTEN;
    BitField<u16, u8, 13, 1> SLOT;
  };

  union JOY_STAT
  {
    u32 bits;

    BitField<u32, bool, 0, 1> TXRDY;
    BitField<u32, bool, 1, 1> RXFIFONEMPTY;
    BitField<u32, bool, 2, 1> TXDONE;
    BitField<u32, bool, 7, 1> ACKINPUT;
    BitField<u32, bool, 9, 1> INTR;
    BitField<u32, u32, 11, 21> TMR;
  };

  union JOY_MODE
  {
    u16 bits;

    BitField<u16, u8, 0, 2> reload_factor;
    BitField<u16, u8, 2, 2> character_length;
    BitField<u16, bool, 4, 1> parity_enable;
    BitField<u16, u8, 5, 1> parity_type;
    BitField<u16, u8, 8, 1> clk_polarity;
  };

  ALWAYS_INLINE bool CanTransfer() const { return m_transmit_buffer_full && m_JOY_CTRL.SELECT && m_JOY_CTRL.TXEN; }

  ALWAYS_INLINE TickCount GetTransferTicks() const { return static_cast<TickCount>(ZeroExtend32(m_JOY_BAUD) * 8); }

  // From @JaCzekanski
  // ACK lasts ~96 ticks or approximately 2.84us at master clock (not implemented).
  // ACK delay is between 6.8us-13.7us, or ~338 ticks at master clock for approximately 9.98us.
  // Memory card responds faster, approximately 5us or ~170 ticks.
  static constexpr TickCount GetACKTicks(bool memory_card) { return memory_card ? 170 : 450; }

  void SoftReset();
  void UpdateJoyStat();
  void TransferEvent(TickCount ticks_late);
  void BeginTransfer();
  void DoTransfer(TickCount ticks_late);
  void DoACK();
  void EndTransfer();
  void ResetDeviceTransferState();

  bool DoStateController(StateWrapper& sw, u32 i);
  bool DoStateMemcard(StateWrapper& sw, u32 i);

  std::array<std::unique_ptr<Controller>, NUM_CONTROLLER_AND_CARD_PORTS> m_controllers;
  std::array<std::unique_ptr<MemoryCard>, NUM_CONTROLLER_AND_CARD_PORTS> m_memory_cards;

  std::array<Multitap, NUM_MULTITAPS> m_multitaps;

  std::unique_ptr<TimingEvent> m_transfer_event;
  State m_state = State::Idle;

  JOY_CTRL m_JOY_CTRL = {};
  JOY_STAT m_JOY_STAT = {};
  JOY_MODE m_JOY_MODE = {};
  u16 m_JOY_BAUD = 0;

  ActiveDevice m_active_device = ActiveDevice::None;
  u8 m_receive_buffer = 0;
  u8 m_transmit_buffer = 0;
  u8 m_transmit_value = 0;
  bool m_receive_buffer_full = false;
  bool m_transmit_buffer_full = false;
};

extern Pad g_pad;
