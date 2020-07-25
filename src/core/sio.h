#pragma once
#include "common/bitfield.h"
#include "common/fifo_queue.h"
#include "types.h"
#include <array>
#include <memory>

class StateWrapper;

class Controller;
class MemoryCard;

class SIO
{
public:
  SIO();
  ~SIO();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

private:
  union SIO_CTRL
  {
    u16 bits;

    BitField<u16, bool, 0, 1> TXEN;
    BitField<u16, bool, 1, 1> DTROUTPUT;
    BitField<u16, bool, 2, 1> RXEN;
    BitField<u16, bool, 3, 1> TXOUTPUT;
    BitField<u16, bool, 4, 1> ACK;
    BitField<u16, bool, 5, 1> RTSOUTPUT;
    BitField<u16, bool, 6, 1> RESET;
    BitField<u16, u8, 8, 2> RXIMODE;
    BitField<u16, bool, 10, 1> TXINTEN;
    BitField<u16, bool, 11, 1> RXINTEN;
    BitField<u16, bool, 12, 1> ACKINTEN;
  };

  union SIO_STAT
  {
    u32 bits;

    BitField<u32, bool, 0, 1> TXRDY;
    BitField<u32, bool, 1, 1> RXFIFONEMPTY;
    BitField<u32, bool, 2, 1> TXDONE;
    BitField<u32, bool, 3, 1> RXPARITY;
    BitField<u32, bool, 4, 1> RXFIFOOVERRUN;
    BitField<u32, bool, 5, 1> RXBADSTOPBIT;
    BitField<u32, bool, 6, 1> RXINPUTLEVEL;
    BitField<u32, bool, 7, 1> DSRINPUTLEVEL;
    BitField<u32, bool, 8, 1> CTSINPUTLEVEL;
    BitField<u32, bool, 9, 1> INTR;
    BitField<u32, u32, 11, 15> TMR;
  };

  union SIO_MODE
  {
    u16 bits;

    BitField<u16, u8, 0, 2> reload_factor;
    BitField<u16, u8, 2, 2> character_length;
    BitField<u16, bool, 4, 1> parity_enable;
    BitField<u16, u8, 5, 1> parity_type;
    BitField<u16, u8, 6, 2> stop_bit_length;
  };

  void SoftReset();

  SIO_CTRL m_SIO_CTRL = {};
  SIO_STAT m_SIO_STAT = {};
  SIO_MODE m_SIO_MODE = {};
  u16 m_SIO_BAUD = 0;
};

extern SIO g_sio;