#pragma once
#include "common/bitfield.h"
#include "pad_device.h"
#include <memory>
#include <array>

class MemoryCard final : public PadDevice
{
public:
  enum : u32
  {
    DATA_SIZE = 128 * 1024, // 1mbit
    SECTOR_SIZE = 128,
    NUM_SECTORS = DATA_SIZE / SECTOR_SIZE
  };

  MemoryCard();
  ~MemoryCard() override;

  static std::shared_ptr<MemoryCard> Create();

  void ResetTransferState() override;
  bool Transfer(const u8 data_in, u8* data_out) override;

private:
  union FLAG
  {
    u8 bits;

    BitField<u8, bool, 3, 1> no_write_yet;
    BitField<u8, bool, 2, 1> write_error;
  };

  FLAG m_FLAG = {};

  enum class State : u8
  {
    Idle,

    ReadCardID1,
    ReadCardID2,
    ReadAddressMSB,
    ReadAddressLSB,
    ReadACK1,
    ReadACK2,
    ReadConfirmAddressMSB,
    ReadConfirmAddressLSB,
    ReadData,
    ReadChecksum,
    ReadEnd,

    WriteCardID1,
    WriteCardID2,
    WriteAddressMSB,
    WriteAddressLSB,
    WriteData,
    WriteChecksum,
    WriteACK1,
    WriteACK2,
    WriteEnd,
  };

  State m_state = State::Idle;
  u16 m_address = 0;
  u8 m_sector_offset = 0;
  u8 m_checksum = 0;
  u8 m_last_byte = 0;

  std::array<u8, DATA_SIZE> m_data{};
};
