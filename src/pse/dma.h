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

private:
  Bus* m_bus = nullptr;
  GPU* m_gpu = nullptr;

  enum class SyncMode : u32
  {
    Word = 0,
    Block = 1,
    LinkedList = 2,
    Reserved = 3
  };

  struct ChannelState
  {
    u32 base_address;

    union BlockControl
    {
      u32 bits;
      struct
      {
        BitField<u32, u32, 0, 16> word_count;
      } word_mode;
      struct
      {
        BitField<u32, u32, 0, 16> block_size;
        BitField<u32, u32, 16, 16> block_count;
      } block_mode;
    } block_control;

    union ChannelControl
    {
      u32 bits;
      BitField<u32, bool, 0, 1> direction_to_ram;
      BitField<u32, bool, 1, 1> address_step_forward;
      BitField<u32, bool, 8, 1> chopping_enable;
      BitField<u32, SyncMode, 9, 2> sync_mode;
      BitField<u32, u32, 16, 3> chopping_dma_window_size;
      BitField<u32, u32, 20, 3> chopping_cpu_window_size;
      BitField<u32, bool, 28, 1> start_trigger;
    } channel_control;
  };

  std::array<ChannelState, NUM_CHANNELS> m_state = {};

  struct DPCR
  {
    u32 bits;

    u8 GetPriority(Channel channel) { return ((bits >> (static_cast<u8>(channel) * 4)) & u32(3)); }
    bool GetMasterEnable(Channel channel)
    {
      return ConvertToBoolUnchecked((bits >> (static_cast<u8>(channel) * 4 + 3)) & u32(1));
    }
  } m_DPCR;

  static constexpr u32 DCIR_WRITE_MASK = 0b11111111'11111111'10000000'00111111;
  u32 m_DCIR = 0;
};
