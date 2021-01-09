#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>
#include <memory>
#include <vector>

class StateWrapper;

class TimingEvent;

class DMA
{
public:
  enum : u32
  {
    NUM_CHANNELS = 7
  };

  enum class Channel : u32
  {
    MDECin = 0,
    MDECout = 1,
    GPU = 2,
    CDROM = 3,
    SPU = 4,
    PIO = 5,
    OTC = 6
  };

  DMA();
  ~DMA();

  void Initialize();
  void Shutdown();
  void Reset();
  bool DoState(StateWrapper& sw);

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  void SetRequest(Channel channel, bool request);

  // changing interfaces
  void SetMaxSliceTicks(TickCount ticks) { m_max_slice_ticks = ticks; }
  void SetHaltTicks(TickCount ticks) { m_halt_ticks = ticks; }

  void DrawDebugStateWindow();

private:
  static constexpr PhysicalMemoryAddress BASE_ADDRESS_MASK = UINT32_C(0x00FFFFFF);
  static constexpr PhysicalMemoryAddress ADDRESS_MASK = UINT32_C(0x001FFFFC);

  enum class SyncMode : u32
  {
    Manual = 0,
    Request = 1,
    LinkedList = 2,
    Reserved = 3
  };

  void ClearState();

  // is everything enabled for a channel to operate?
  bool CanTransferChannel(Channel channel, bool ignore_halt) const;
  bool IsTransferHalted() const;
  void UpdateIRQ();

  // returns false if the DMA should now be halted
  TickCount GetTransferSliceTicks() const;
  TickCount GetTransferHaltTicks() const;
  bool TransferChannel(Channel channel);
  void HaltTransfer(TickCount duration);
  void UnhaltTransfer(TickCount ticks);

  // from device -> memory
  TickCount TransferDeviceToMemory(Channel channel, u32 address, u32 increment, u32 word_count);

  // from memory -> device
  TickCount TransferMemoryToDevice(Channel channel, u32 address, u32 increment, u32 word_count);

  // configuration
  TickCount m_max_slice_ticks = 1000;
  TickCount m_halt_ticks = 100;

  std::vector<u32> m_transfer_buffer;
  std::unique_ptr<TimingEvent> m_unhalt_event;
  TickCount m_halt_ticks_remaining = 0;

  struct ChannelState
  {
    u32 base_address = 0;

    union BlockControl
    {
      u32 bits;
      union
      {
        BitField<u32, u32, 0, 16> word_count;

        u32 GetWordCount() const { return (word_count == 0) ? 0x10000 : word_count; }
      } manual;
      union
      {
        BitField<u32, u32, 0, 16> block_size;
        BitField<u32, u32, 16, 16> block_count;

        u32 GetBlockSize() const { return (block_size == 0) ? 0x10000 : block_size; }
        u32 GetBlockCount() const { return (block_count == 0) ? 0x10000 : block_count; }
      } request;
    } block_control = {};

    union ChannelControl
    {
      u32 bits;
      BitField<u32, bool, 0, 1> copy_to_device;
      BitField<u32, bool, 1, 1> address_step_reverse;
      BitField<u32, bool, 8, 1> chopping_enable;
      BitField<u32, SyncMode, 9, 2> sync_mode;
      BitField<u32, u32, 16, 3> chopping_dma_window_size;
      BitField<u32, u32, 20, 3> chopping_cpu_window_size;
      BitField<u32, bool, 24, 1> enable_busy;
      BitField<u32, bool, 28, 1> start_trigger;

      static constexpr u32 WRITE_MASK = 0b01110001'01110111'00000111'00000011;
    } channel_control = {};

    bool request = false;
  };

  std::array<ChannelState, NUM_CHANNELS> m_state;

  union DPCR
  {
    u32 bits;

    BitField<u32, u8, 0, 3> MDECin_priority;
    BitField<u32, bool, 3, 1> MDECin_master_enable;
    BitField<u32, u8, 4, 3> MDECout_priority;
    BitField<u32, bool, 7, 1> MDECout_master_enable;
    BitField<u32, u8, 8, 3> GPU_priority;
    BitField<u32, bool, 10, 1> GPU_master_enable;
    BitField<u32, u8, 12, 3> CDROM_priority;
    BitField<u32, bool, 15, 1> CDROM_master_enable;
    BitField<u32, u8, 16, 3> SPU_priority;
    BitField<u32, bool, 19, 1> SPU_master_enable;
    BitField<u32, u8, 20, 3> PIO_priority;
    BitField<u32, bool, 23, 1> PIO_master_enable;
    BitField<u32, u8, 24, 3> OTC_priority;
    BitField<u32, bool, 27, 1> OTC_master_enable;
    BitField<u32, u8, 28, 3> priority_offset;
    BitField<u32, bool, 31, 1> unused;

    u8 GetPriority(Channel channel) const { return ((bits >> (static_cast<u8>(channel) * 4)) & u32(3)); }
    bool GetMasterEnable(Channel channel) const
    {
      return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) * 4 + 3)) & u32(1));
    }
  } m_DPCR = {};

  static constexpr u32 DICR_WRITE_MASK = 0b00000000'11111111'10000000'00111111;
  static constexpr u32 DICR_RESET_MASK = 0b01111111'00000000'00000000'00000000;
  union DICR
  {
    u32 bits;

    BitField<u32, bool, 15, 1> force_irq;
    BitField<u32, bool, 16, 1> MDECin_irq_enable;
    BitField<u32, bool, 17, 1> MDECout_irq_enable;
    BitField<u32, bool, 18, 1> GPU_irq_enable;
    BitField<u32, bool, 19, 1> CDROM_irq_enable;
    BitField<u32, bool, 20, 1> SPU_irq_enable;
    BitField<u32, bool, 21, 1> PIO_irq_enable;
    BitField<u32, bool, 22, 1> OTC_irq_enable;
    BitField<u32, bool, 23, 1> master_enable;
    BitField<u32, bool, 24, 1> MDECin_irq_flag;
    BitField<u32, bool, 25, 1> MDECout_irq_flag;
    BitField<u32, bool, 26, 1> GPU_irq_flag;
    BitField<u32, bool, 27, 1> CDROM_irq_flag;
    BitField<u32, bool, 28, 1> SPU_irq_flag;
    BitField<u32, bool, 29, 1> PIO_irq_flag;
    BitField<u32, bool, 30, 1> OTC_irq_flag;
    BitField<u32, bool, 31, 1> master_flag;

    bool IsIRQEnabled(Channel channel) const
    {
      return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) + 16)) & u32(1));
    }

    bool GetIRQFlag(Channel channel) const
    {
      return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) + 24)) & u32(1));
    }

    void SetIRQFlag(Channel channel) { bits |= (u32(1) << (static_cast<u8>(channel) + 24)); }
    void ClearIRQFlag(Channel channel) { bits &= ~(u32(1) << (static_cast<u8>(channel) + 24)); }

    void UpdateMasterFlag()
    {
      master_flag = master_enable && ((((bits >> 16) & u32(0b1111111)) & ((bits >> 24) & u32(0b1111111))) != 0);
    }
  } m_DICR = {};
};

extern DMA g_dma;