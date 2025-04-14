// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "controller.h"
#include "memory_card_image.h"
#include "timing_event.h"

#include "common/bitfield.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

class MemoryCard final
{
public:
  MemoryCard();
  ~MemoryCard();

  static constexpr u32 STATE_SIZE = 1 + 1 + 2 + 1 + 1 + 1 + MemoryCardImage::DATA_SIZE + 1;

  static std::unique_ptr<MemoryCard> Create();
  static std::unique_ptr<MemoryCard> Open(std::string_view path);

  const MemoryCardImage::DataArray& GetData() const { return m_data; }
  MemoryCardImage::DataArray& GetData() { return m_data; }
  const std::string& GetPath() const { return m_path; }

  void Reset();
  bool DoState(StateWrapper& sw);
  void CopyState(const MemoryCard* src);

  void ResetTransferState();
  bool Transfer(const u8 data_in, u8* data_out);

  bool IsOrWasRecentlyWriting() const;

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

    GetIDCardID1,
    GetIDCardID2,
    GetIDACK1,
    GetIDACK2,
    GetID1,
    GetID2,
    GetID3,
    GetID4,
  };

  static TickCount GetSaveDelayInTicks();

  bool SaveIfChanged(bool display_osd_message);
  void QueueFileSave();

  State m_state = State::Idle;
  FLAG m_FLAG = {};
  u16 m_address = 0;
  u8 m_sector_offset = 0;
  u8 m_checksum = 0;
  u8 m_last_byte = 0;
  bool m_changed = false;

  TimingEvent m_save_event;
  std::string m_path;

  MemoryCardImage::DataArray m_data{};
};
