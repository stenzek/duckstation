#pragma once
#include "common/bitfield.h"
#include "controller.h"
#include "memory_card_image.h"
#include <array>
#include <memory>
#include <string>
#include <string_view>

class TimingEvent;

class MemoryCard final
{
public:
  MemoryCard();
  ~MemoryCard();

  static std::string SanitizeGameTitleForFileName(const std::string_view& name);

  static std::unique_ptr<MemoryCard> Create();
  static std::unique_ptr<MemoryCard> Open(std::string_view filename);

  const MemoryCardImage::DataArray& GetData() const { return m_data; }
  MemoryCardImage::DataArray& GetData() { return m_data; }
  const std::string& GetFilename() const { return m_filename; }
  void SetFilename(std::string filename) { m_filename = std::move(filename); }

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

  static TickCount GetSaveDelayInTicks();

  bool LoadFromFile();
  bool SaveIfChanged(bool display_osd_message);
  void QueueFileSave();

  std::unique_ptr<TimingEvent> m_save_event;

  State m_state = State::Idle;
  FLAG m_FLAG = {};
  u16 m_address = 0;
  u8 m_sector_offset = 0;
  u8 m_checksum = 0;
  u8 m_last_byte = 0;
  bool m_changed = false;

  MemoryCardImage::DataArray m_data{};

  std::string m_filename;
};
