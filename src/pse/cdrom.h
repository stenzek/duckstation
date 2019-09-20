#pragma once
#include "types.h"
#include "common/bitfield.h"
#include "common/fifo_queue.h"

class CDImage;
class StateWrapper;

class DMA;
class InterruptController;

class CDROM
{
public:
  CDROM();
  ~CDROM();

  bool Initialize(DMA* dma, InterruptController* interrupt_controller);
  void Reset();
  bool DoState(StateWrapper& sw);

  bool HasMedia() const { return static_cast<bool>(m_media); }
  bool InsertMedia(const char* filename);
  void RemoveMedia();

  // I/O
  u8 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u8 value);

  void Execute();

private:
  static constexpr u32 PARAM_FIFO_SIZE = 16;
  static constexpr u32 RESPONSE_FIFO_SIZE = 16;
  static constexpr u32 DATA_FIFO_SIZE = 4096;
  static constexpr u32 NUM_INTERRUPTS = 32;
  static constexpr u8 INTERRUPT_REGISTER_MASK = 0x1F;

  enum class Interrupt : u8
  {
    INT1 = 0x01,
    INT2 = 0x02,
    INT3 = 0x03,
    INT4 = 0x04,
    INT5 = 0x05
  };

  enum class Command : u8
  {
    Sync = 0x00,
    Getstat = 0x01,
    Setloc = 0x02,
    Play = 0x03,
    Forward = 0x04,
    Backward = 0x05,
    ReadN = 0x06,
    MotorOn = 0x07,
    Stop = 0x08,
    Pause = 0x09,
    Init = 0x0A,
    Mute = 0x0B,
    Demute = 0x0C,
    Setfilter = 0x0D,
    Setmode = 0x0E,
    Getparam = 0x0F,
    GetlocL = 0x10,
    GetlocP = 0x11,
    SetSession = 0x12,
    GetTN = 0x13,
    GetTD = 0x14,
    SeekL = 0x15,
    SeekP = 0x16,
    SetClock = 0x17,
    GetClock = 0x18,
    Test = 0x19,
    GetID = 0x1A,
    ReadS = 0x1B,
    Reset = 0x1C,
    GetQ = 0x1D,
    ReadTOC = 0x1E,
    VideoCD = 0x1F,
  };

  bool HasPendingInterrupt() const { return m_interrupt_flag_register != 0; }
  void SetInterrupt(Interrupt interrupt);
  void UpdateStatusRegister();
  void ExecuteCommand(Command command);
  void ExecuteTestCommand(u8 subcommand);

  DMA* m_dma;
  InterruptController* m_interrupt_controller;
  std::unique_ptr<CDImage> m_media;

  enum class State : u32
  {
    Idle
  };

  State m_state = State::Idle;

  union
  {
    u8 bits;
    BitField<u8, u8, 0, 2> index;
    BitField<u8, bool, 2, 1> ADPBUSY;
    BitField<u8, bool, 3, 1> PRMEMPTY;
    BitField<u8, bool, 4, 1> PRMWRDY;
    BitField<u8, bool, 5, 1> RSLRRDY;
    BitField<u8, bool, 6, 1> DRQSTS;
    BitField<u8, bool, 7, 1> BUSYSTS;
  } m_status = {};

  union
  {
    u8 bits;
    BitField<u8, bool, 0, 1> error;
    BitField<u8, bool, 1, 1> motor_on;
    BitField<u8, bool, 2, 1> seek_error;
    BitField<u8, bool, 3, 1> id_error;
    BitField<u8, bool, 4, 1> shell_open;
    BitField<u8, bool, 5, 1> reading;
    BitField<u8, bool, 6, 1> seeking;
    BitField<u8, bool, 7, 1> playing_cdda;
  } m_secondary_status = {};

  u8 m_interrupt_enable_register = INTERRUPT_REGISTER_MASK;
  u8 m_interrupt_flag_register = 0;

  InlineFIFOQueue<u8, PARAM_FIFO_SIZE> m_param_fifo;
  InlineFIFOQueue<u8, RESPONSE_FIFO_SIZE> m_response_fifo;
  HeapFIFOQueue<u8, DATA_FIFO_SIZE> m_data_fifo;
};

