#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class System;
class InterruptController;
class PadDevice;
class MemoryCard;

class Pad
{
public:
  Pad();
  ~Pad();

  void Initialize(System* system, InterruptController* interrupt_controller);
  void Reset();
  bool DoState(StateWrapper& sw);

  PadDevice* GetController(u32 slot) { return m_controllers[slot].get(); }
  void SetController(u32 slot, std::shared_ptr<PadDevice> dev) { m_controllers[slot] = dev; }

  MemoryCard* GetMemoryCard(u32 slot) { return m_memory_cards[slot].get(); }
  void SetMemoryCard(u32 slot, std::shared_ptr<MemoryCard> dev) { m_memory_cards[slot] = dev; }

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  void Execute(TickCount ticks);

private:
  static constexpr u32 NUM_SLOTS = 2;

  enum class State : u32
  {
    Idle,
    Transmitting
  };

  enum class ActiveDevice : u8
  {
    None,
    Controller,
    MemoryCard
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

  bool IsTransmitting() const { return m_state == State::Transmitting; }
  bool CanTransfer() const
  {
    return !m_TX_FIFO.IsEmpty() && !m_RX_FIFO.IsFull() && m_JOY_CTRL.SELECT && m_JOY_CTRL.TXEN;
  }

  TickCount GetTransferTicks() const { return static_cast<TickCount>(ZeroExtend32(m_JOY_BAUD) * 8); }

  void SoftReset();
  void UpdateJoyStat();
  void BeginTransfer();
  void DoTransfer();
  void EndTransfer();
  void ResetDeviceTransferState();

  System* m_system = nullptr;
  InterruptController* m_interrupt_controller = nullptr;

  State m_state = State::Idle;
  TickCount m_ticks_remaining = 0;

  JOY_CTRL m_JOY_CTRL = {};
  JOY_STAT m_JOY_STAT = {};
  JOY_MODE m_JOY_MODE = {};
  u16 m_JOY_BAUD = 0;

  ActiveDevice m_active_device = ActiveDevice::None;
  InlineFIFOQueue<u8, 8> m_RX_FIFO;
  InlineFIFOQueue<u8, 2> m_TX_FIFO;

  std::array<std::shared_ptr<PadDevice>, NUM_SLOTS> m_controllers;
  std::array<std::shared_ptr<MemoryCard>, NUM_SLOTS> m_memory_cards;
};
