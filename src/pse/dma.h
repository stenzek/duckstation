#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <array>

class Bus;
class GPU;

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

  bool Initialize(Bus* bus, GPU* gpu);
  void Reset();

  u32 ReadRegister(u32 offset);
  void WriteRegister(u32 offset, u32 value);

  void SetRequest(Channel channel, bool request);

private:
  static constexpr PhysicalMemoryAddress ADDRESS_MASK = UINT32_C(0x00FFFFFF);

  enum class SyncMode : u32
  {
    Manual = 0,
    Request = 1,
    LinkedList = 2,
    Reserved = 3
  };

  // is everything enabled for a channel to operate?
  bool CanRunChannel(Channel channel) const;

  void RunDMA(Channel channel);

  // from device -> memory
  u32 DMARead(Channel channel, PhysicalMemoryAddress dst_address, u32 remaining_words);

  // from memory -> device
  void DMAWrite(Channel channel, u32 value, PhysicalMemoryAddress src_address, u32 remaining_words);

  Bus* m_bus = nullptr;
  GPU* m_gpu = nullptr;

  struct ChannelState
  {
    u32 base_address;

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
    } block_control;

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
    } channel_control;

    bool request = false;
  };

  std::array<ChannelState, NUM_CHANNELS> m_state = {};

  struct DPCR
  {
    u32 bits;

    u8 GetPriority(Channel channel) const { return ((bits >> (static_cast<u8>(channel) * 4)) & u32(3)); }
    bool GetMasterEnable(Channel channel) const
    {
      return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) * 4 + 3)) & u32(1));
    }
  } m_DPCR;

  static constexpr u32 DCIR_WRITE_MASK = 0b11111111'11111111'10000000'00111111;
  u32 m_DCIR = 0;
};
