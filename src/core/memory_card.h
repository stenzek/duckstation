#pragma once
#include "common/bitfield.h"
#include "controller.h"
#include <array>
#include <memory>
#include <string>
#include <string_view>

class System;
class TimingEvent;

class MemoryCard final
{
public:
  enum : u32
  {
    DATA_SIZE = 128 * 1024, // 1mbit
    SECTOR_SIZE = 128,
    NUM_SECTORS = DATA_SIZE / SECTOR_SIZE
  };

  MemoryCard(System* system);
  ~MemoryCard();

  static std::unique_ptr<MemoryCard> Create(System* system);
  static std::unique_ptr<MemoryCard> Open(System* system, std::string_view filename);

  void Reset();
  bool DoState(StateWrapper& sw);

  void ResetTransferState();
  bool Transfer(const u8 data_in, u8* data_out);

  void Format();

private:
  enum : u32
  {
    // save in three seconds, that should be long enough for everything to finish writing
    SAVE_DELAY_IN_SECONDS = 5,
    SAVE_DELAY_IN_SYSCLK_TICKS = MASTER_CLOCK * SAVE_DELAY_IN_SECONDS,
  };

  union FLAG
  {
    u8 bits;

    BitField<u8, bool, 3, 1> no_write_yet;
    BitField<u8, bool, 2, 1> write_error;
  };

  enum class State : u8
  {
    Idle,
    Command,

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

  static u8 ChecksumFrame(const u8* fptr);

  u8* GetSectorPtr(u32 sector);

  bool LoadFromFile();
  bool SaveIfChanged(bool display_osd_message);
  void QueueFileSave();

  System* m_system;
  std::unique_ptr<TimingEvent> m_save_event;

  State m_state = State::Idle;
  FLAG m_FLAG = {};
  u16 m_address = 0;
  u8 m_sector_offset = 0;
  u8 m_checksum = 0;
  u8 m_last_byte = 0;
  bool m_changed = false;

  std::array<u8, DATA_SIZE> m_data{};

  std::string m_filename;
};
