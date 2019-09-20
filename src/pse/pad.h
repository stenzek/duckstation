#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class InterruptController;
class PadDevice;

class Pad
{
public:
  Pad();
  ~Pad();

  bool Initialize(InterruptController* interrupt_controller);
  void Reset();
  bool DoState(StateWrapper& sw);

  PadDevice* GetDevice(u32 slot) { return m_devices[slot].get(); }
  void SetDevice(u32 slot, std::shared_ptr<PadDevice> dev) { m_devices[slot] = dev; }

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

private:
  static constexpr u32 NUM_SLOTS = 2;

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
    BitField<u32, bool, 3, 1> ACKINPUTLEVEL;
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

  void SoftReset();
  void UpdateJoyStat();
  void DoTransfer();

  InterruptController* m_interrupt_controller = nullptr;

  JOY_CTRL m_JOY_CTRL = {};
  JOY_STAT m_JOY_STAT = {};
  JOY_MODE m_JOY_MODE = {};

  InlineFIFOQueue<u8, 8> m_RX_FIFO;
  InlineFIFOQueue<u8, 2> m_TX_FIFO;

  std::array<std::shared_ptr<PadDevice>, NUM_SLOTS> m_devices;
};
