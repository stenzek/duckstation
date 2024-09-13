// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memory_card.h"
#include "host.h"
#include "system.h"

#include "util/imgui_manager.h"
#include "util/state_wrapper.h"

#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include "IconsFontAwesome5.h"
#include "fmt/format.h"

Log_SetChannel(MemoryCard);

MemoryCard::MemoryCard()
  : m_save_event(
      "Memory Card Host Flush", GetSaveDelayInTicks(), GetSaveDelayInTicks(),
      [](void* param, TickCount ticks, TickCount ticks_late) { static_cast<MemoryCard*>(param)->SaveIfChanged(true); },
      this)
{
  m_FLAG.no_write_yet = true;
}

MemoryCard::~MemoryCard()
{
  SaveIfChanged(false);
}

TickCount MemoryCard::GetSaveDelayInTicks()
{
  return System::GetTicksPerSecond() * SAVE_DELAY_IN_SECONDS;
}

void MemoryCard::Reset()
{
  ResetTransferState();
  SaveIfChanged(true);
  m_FLAG.no_write_yet = true;
}

bool MemoryCard::DoState(StateWrapper& sw)
{
  if (sw.IsReading())
    SaveIfChanged(true);

  sw.Do(&m_state);
  sw.Do(&m_FLAG.bits);
  sw.Do(&m_address);
  sw.Do(&m_sector_offset);
  sw.Do(&m_checksum);
  sw.Do(&m_last_byte);
  sw.Do(&m_data);
  sw.Do(&m_changed);

  return !sw.HasError();
}

void MemoryCard::CopyState(const MemoryCard* src)
{
  DebugAssert(m_data == src->m_data);

  m_state = src->m_state;
  m_FLAG.bits = src->m_FLAG.bits;
  m_address = src->m_address;
  m_sector_offset = src->m_sector_offset;
  m_checksum = src->m_checksum;
  m_last_byte = src->m_last_byte;
  m_changed = src->m_changed;
}

void MemoryCard::ResetTransferState()
{
  m_state = State::Idle;
  m_address = 0;
  m_sector_offset = 0;
  m_checksum = 0;
  m_last_byte = 0;
}

bool MemoryCard::Transfer(const u8 data_in, u8* data_out)
{
  bool ack = false;
#ifdef _DEBUG
  const State old_state = m_state;
#endif

  switch (m_state)
  {

#define FIXED_REPLY_STATE(state, reply, ack_value, next_state)                                                         \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = reply;                                                                                                 \
    ack = ack_value;                                                                                                   \
    m_state = next_state;                                                                                              \
  }                                                                                                                    \
  break;

#define ADDRESS_STATE_MSB(state, next_state)                                                                           \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = 0x00;                                                                                                  \
    ack = true;                                                                                                        \
    m_address = ((m_address & u16(0x00FF)) | (ZeroExtend16(data_in) << 8)) & 0x3FF;                                    \
    m_state = next_state;                                                                                              \
  }                                                                                                                    \
  break;

#define ADDRESS_STATE_LSB(state, next_state)                                                                           \
  case state:                                                                                                          \
  {                                                                                                                    \
    *data_out = m_last_byte;                                                                                           \
    ack = true;                                                                                                        \
    m_address = ((m_address & u16(0xFF00)) | ZeroExtend16(data_in)) & 0x3FF;                                           \
    m_sector_offset = 0;                                                                                               \
    m_state = next_state;                                                                                              \
  }                                                                                                                    \
  break;

    // read state

    FIXED_REPLY_STATE(State::ReadCardID1, 0x5A, true, State::ReadCardID2);
    FIXED_REPLY_STATE(State::ReadCardID2, 0x5D, true, State::ReadAddressMSB);
    ADDRESS_STATE_MSB(State::ReadAddressMSB, State::ReadAddressLSB);
    ADDRESS_STATE_LSB(State::ReadAddressLSB, State::ReadACK1);
    FIXED_REPLY_STATE(State::ReadACK1, 0x5C, true, State::ReadACK2);
    FIXED_REPLY_STATE(State::ReadACK2, 0x5D, true, State::ReadConfirmAddressMSB);
    FIXED_REPLY_STATE(State::ReadConfirmAddressMSB, Truncate8(m_address >> 8), true, State::ReadConfirmAddressLSB);
    FIXED_REPLY_STATE(State::ReadConfirmAddressLSB, Truncate8(m_address), true, State::ReadData);

    case State::ReadData:
    {
      const u8 bits = m_data[ZeroExtend32(m_address) * MemoryCardImage::FRAME_SIZE + m_sector_offset];
      if (m_sector_offset == 0)
      {
        DEV_LOG("Reading memory card sector {}", m_address);
        m_checksum = Truncate8(m_address >> 8) ^ Truncate8(m_address) ^ bits;
      }
      else
      {
        m_checksum ^= bits;
      }

      *data_out = bits;
      ack = true;

      m_sector_offset++;
      if (m_sector_offset == MemoryCardImage::FRAME_SIZE)
      {
        m_state = State::ReadChecksum;
        m_sector_offset = 0;
      }
    }
    break;

      FIXED_REPLY_STATE(State::ReadChecksum, m_checksum, true, State::ReadEnd);
      FIXED_REPLY_STATE(State::ReadEnd, 0x47, true, State::Idle);

      // write state

      FIXED_REPLY_STATE(State::WriteCardID1, 0x5A, true, State::WriteCardID2);
      FIXED_REPLY_STATE(State::WriteCardID2, 0x5D, true, State::WriteAddressMSB);
      ADDRESS_STATE_MSB(State::WriteAddressMSB, State::WriteAddressLSB);
      ADDRESS_STATE_LSB(State::WriteAddressLSB, State::WriteData);

    case State::WriteData:
    {
      if (m_sector_offset == 0)
      {
        INFO_LOG("Writing memory card sector {}", m_address);
        m_checksum = Truncate8(m_address >> 8) ^ Truncate8(m_address) ^ data_in;
        m_FLAG.no_write_yet = false;
      }
      else
      {
        m_checksum ^= data_in;
      }

      const u32 offset = ZeroExtend32(m_address) * MemoryCardImage::FRAME_SIZE + m_sector_offset;
      m_changed |= (m_data[offset] != data_in);
      m_data[offset] = data_in;

      *data_out = m_last_byte;
      ack = true;

      m_sector_offset++;
      if (m_sector_offset == MemoryCardImage::FRAME_SIZE)
      {
        m_state = State::WriteChecksum;
        m_sector_offset = 0;
        if (m_changed)
          QueueFileSave();
      }
    }
    break;

      FIXED_REPLY_STATE(State::WriteChecksum, m_checksum, true, State::WriteACK1);
      FIXED_REPLY_STATE(State::WriteACK1, 0x5C, true, State::WriteACK2);
      FIXED_REPLY_STATE(State::WriteACK2, 0x5D, true, State::WriteEnd);
      FIXED_REPLY_STATE(State::WriteEnd, 0x47, false, State::Idle);

      // TODO: This really needs a proper buffer system...
      FIXED_REPLY_STATE(State::GetIDCardID1, 0x5A, true, State::GetIDCardID2);
      FIXED_REPLY_STATE(State::GetIDCardID2, 0x5D, true, State::GetIDACK1);
      FIXED_REPLY_STATE(State::GetIDACK1, 0x5C, true, State::GetIDACK2);
      FIXED_REPLY_STATE(State::GetIDACK2, 0x5D, true, State::GetID1);
      FIXED_REPLY_STATE(State::GetID1, 0x04, true, State::GetID2);
      FIXED_REPLY_STATE(State::GetID2, 0x00, true, State::GetID3);
      FIXED_REPLY_STATE(State::GetID3, 0x00, true, State::GetID4);
      FIXED_REPLY_STATE(State::GetID4, 0x80, true, State::Command);

      // new command
    case State::Idle:
    {
      // select device
      if (data_in == 0x81)
      {
        *data_out = 0xFF;
        ack = true;
        m_state = State::Command;
      }
    }
    break;

    case State::Command:
    {
      switch (data_in)
      {
        case 0x52: // read data
        {
          *data_out = m_FLAG.bits;
          ack = true;
          m_state = State::ReadCardID1;
        }
        break;

        case 0x57: // write data
        {
          *data_out = m_FLAG.bits;
          ack = true;
          m_state = State::WriteCardID1;
        }
        break;

        case 0x53: // get id
        {
          *data_out = m_FLAG.bits;
          ack = true;
          m_state = State::GetIDCardID1;
        }
        break;

        default:
          [[unlikely]]
          {
            ERROR_LOG("Invalid command 0x{:02X}", data_in);
            *data_out = m_FLAG.bits;
            ack = false;
            m_state = State::Idle;
          }
      }
    }
    break;

    default:
      UnreachableCode();
      break;
  }

  DEBUG_LOG("Transfer, old_state={}, new_state={}, data_in=0x{:02X}, data_out=0x{:02X}, ack={}",
            static_cast<u32>(old_state), static_cast<u32>(m_state), data_in, *data_out, ack ? "true" : "false");
  m_last_byte = data_in;
  return ack;
}

bool MemoryCard::IsOrWasRecentlyWriting() const
{
  return (m_state == State::WriteData || m_save_event.IsActive());
}

std::unique_ptr<MemoryCard> MemoryCard::Create()
{
  std::unique_ptr<MemoryCard> mc = std::make_unique<MemoryCard>();
  mc->Format();
  return mc;
}

std::unique_ptr<MemoryCard> MemoryCard::Open(std::string_view filename)
{
  std::unique_ptr<MemoryCard> mc = std::make_unique<MemoryCard>();
  mc->m_filename = filename;

  Error error;
  if (!FileSystem::FileExists(mc->m_filename.c_str())) [[unlikely]]
  {
    mc->Format();
    mc->m_changed = false;
  }
  else if (!MemoryCardImage::LoadFromFile(&mc->m_data, mc->m_filename.c_str(), &error)) [[unlikely]]
  {
    Host::AddIconOSDMessage(
      fmt::format("memory_card_{}", filename), ICON_FA_SD_CARD,
      fmt::format(TRANSLATE_FS("MemoryCard", "{} could not be read:\n{}\nThe memory card will NOT be saved.\nYou must "
                                             "delete the memory card manually if you want to save."),
                  Path::GetFileName(filename), error.GetDescription()),
      Host::OSD_CRITICAL_ERROR_DURATION);
    mc->Format();
    mc->m_filename = {};
    mc->m_changed = false;
  }

  return mc;
}

void MemoryCard::Format()
{
  MemoryCardImage::Format(&m_data);
  m_changed = true;
}

bool MemoryCard::SaveIfChanged(bool display_osd_message)
{
  m_save_event.Deactivate();

  if (!m_changed)
    return true;

  m_changed = false;

  if (m_filename.empty())
    return false;

  std::string osd_key;
  std::string display_name;
  if (display_osd_message)
  {
    osd_key = fmt::format("memory_card_save_{}", m_filename);
    display_name = FileSystem::GetDisplayNameFromPath(m_filename);
  }

  INFO_LOG("Saving memory card to {}...", Path::GetFileTitle(m_filename));

  Error error;
  if (!MemoryCardImage::SaveToFile(m_data, m_filename.c_str(), &error))
  {
    if (display_osd_message)
    {
      Host::AddIconOSDMessage(std::move(osd_key), ICON_FA_SD_CARD,
                              fmt::format(TRANSLATE_FS("MemoryCard", "Failed to save memory card to '{}': {}"),
                                          Path::GetFileName(display_name), error.GetDescription()),
                              Host::OSD_ERROR_DURATION);
    }

    return false;
  }

  if (display_osd_message)
  {
    Host::AddIconOSDMessage(
      std::move(osd_key), ICON_FA_SD_CARD,
      fmt::format(TRANSLATE_FS("MemoryCard", "Saved memory card to '{}'."), Path::GetFileName(display_name)),
      Host::OSD_QUICK_DURATION);
  }

  return true;
}

void MemoryCard::QueueFileSave()
{
  // skip if the event is already pending, or we don't have a backing file
  if (m_save_event.IsActive() || m_filename.empty())
    return;

  // save in one second, that should be long enough for everything to finish writing
  m_save_event.Schedule(GetSaveDelayInTicks());
}
