#include "memory_card.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "host_interface.h"
#include "system.h"
#include <cstdio>
Log_SetChannel(MemoryCard);

MemoryCard::MemoryCard(System* system) : m_system(system)
{
  m_FLAG.no_write_yet = true;
}

MemoryCard::~MemoryCard() = default;

void MemoryCard::Reset()
{
  ResetTransferState();
}

bool MemoryCard::DoState(StateWrapper& sw)
{
  sw.Do(&m_state);
  sw.Do(&m_address);
  sw.Do(&m_sector_offset);
  sw.Do(&m_checksum);
  sw.Do(&m_last_byte);
  sw.Do(&m_data);
  sw.Do(&m_changed);

  return !sw.HasError();
}

void MemoryCard::ResetTransferState()
{
  m_state = State::Idle;
  m_address = 0;
  m_sector_offset = 0;
  m_checksum = 0;
  m_last_byte = 0;
  m_changed = false;
}

bool MemoryCard::Transfer(const u8 data_in, u8* data_out)
{
  bool ack = false;
  const State old_state = m_state;

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
      const u8 bits = m_data[ZeroExtend32(m_address) * SECTOR_SIZE + m_sector_offset];
      if (m_sector_offset == 0)
      {
        Log_DevPrintf("Reading memory card sector %u", ZeroExtend32(m_address));
        m_checksum = Truncate8(m_address >> 8) ^ Truncate8(m_address) ^ bits;
      }
      else
      {
        m_checksum ^= bits;
      }

      *data_out = bits;
      ack = true;

      m_sector_offset++;
      if (m_sector_offset == SECTOR_SIZE)
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
        Log_DevPrintf("Writing memory card sector %u", ZeroExtend32(m_address));
        m_checksum = Truncate8(m_address >> 8) ^ Truncate8(m_address) ^ data_in;
        m_FLAG.no_write_yet = false;
      }
      else
      {
        m_checksum ^= data_in;
      }

      const u32 offset = ZeroExtend32(m_address) * SECTOR_SIZE + m_sector_offset;
      if (m_data[offset] != data_in)
        m_changed = true;

      m_data[offset] = data_in;

      *data_out = m_last_byte;
      ack = true;

      m_sector_offset++;
      if (m_sector_offset == SECTOR_SIZE)
      {
        m_state = State::WriteChecksum;
        m_sector_offset = 0;
        if (m_changed)
        {
          m_changed = false;
          SaveToFile();
        }
      }
    }
    break;

      FIXED_REPLY_STATE(State::WriteChecksum, m_checksum, true, State::WriteACK1);
      FIXED_REPLY_STATE(State::WriteACK1, 0x5C, true, State::WriteACK2);
      FIXED_REPLY_STATE(State::WriteACK2, 0x5D, true, State::WriteEnd);
      FIXED_REPLY_STATE(State::WriteEnd, 0x47, true, State::Idle);

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
          Panic("implement me");
        }
        break;

        default:
        {
          Log_ErrorPrintf("Invalid command 0x%02X", ZeroExtend32(data_in));
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

  Log_DebugPrintf("Transfer, old_state=%u, new_state=%u, data_in=0x%02X, data_out=0x%02X, ack=%s",
                  static_cast<u32>(old_state), static_cast<u32>(m_state), data_in, *data_out, ack ? "true" : "false");
  m_last_byte = data_in;
  return ack;
}

std::unique_ptr<MemoryCard> MemoryCard::Create(System* system)
{
  std::unique_ptr<MemoryCard> mc = std::make_unique<MemoryCard>(system);
  mc->Format();
  return mc;
}

std::unique_ptr<MemoryCard> MemoryCard::Open(System* system, std::string_view filename)
{
  std::unique_ptr<MemoryCard> mc = std::make_unique<MemoryCard>(system);
  mc->m_filename = filename;
  if (!mc->LoadFromFile())
  {
    SmallString message;
    message.AppendString("Memory card at '");
    message.AppendString(filename.data(), static_cast<u32>(filename.length()));
    message.AppendString("' could not be read, formatting.");
    Log_ErrorPrint(message);
    mc->Format();
  }

  return mc;
}

u8 MemoryCard::ChecksumFrame(const u8* fptr)
{
  u8 value = 0;
  for (u32 i = 0; i < SECTOR_SIZE - 1; i++)
    value ^= fptr[i];

  return value;
}

void MemoryCard::Format()
{
  // fill everything with FF
  m_data.fill(u8(0xFF));

  // header
  {
    u8* fptr = GetSectorPtr(0);
    std::fill_n(fptr, SECTOR_SIZE, u8(0));
    fptr[0] = 'M';
    fptr[1] = 'C';
    fptr[0x7F] = ChecksumFrame(fptr);
  }

  // directory
  for (u32 frame = 1; frame < 16; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0));
    fptr[0] = 0xA0;                   // free
    fptr[8] = 0xFF;                   // pointer to next file
    fptr[9] = 0xFF;                   // pointer to next file
    fptr[0x7F] = ChecksumFrame(fptr); // checksum
  }

  // broken sector list
  for (u32 frame = 16; frame < 36; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0));
    fptr[0] = 0xFF;
    fptr[1] = 0xFF;
    fptr[2] = 0xFF;
    fptr[3] = 0xFF;
    fptr[8] = 0xFF;                   // pointer to next file
    fptr[9] = 0xFF;                   // pointer to next file
    fptr[0x7F] = ChecksumFrame(fptr); // checksum
  }

  // broken sector replacement data
  for (u32 frame = 36; frame < 56; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0x00));
  }

  // unused frames
  for (u32 frame = 56; frame < 63; frame++)
  {
    u8* fptr = GetSectorPtr(frame);
    std::fill_n(fptr, SECTOR_SIZE, u8(0x00));
  }

  // write test frame
  std::memcpy(GetSectorPtr(63), GetSectorPtr(0), SECTOR_SIZE);
}

u8* MemoryCard::GetSectorPtr(u32 sector)
{
  Assert(sector < NUM_SECTORS);
  return &m_data[sector * SECTOR_SIZE];
}

bool MemoryCard::LoadFromFile()
{
  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(m_filename.c_str(), BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  const size_t num_read = stream->Read(m_data.data(), SECTOR_SIZE * NUM_SECTORS);
  if (num_read != (SECTOR_SIZE * NUM_SECTORS))
  {
    Log_ErrorPrintf("Only read %zu of %u sectors from '%s'", num_read / SECTOR_SIZE, NUM_SECTORS, m_filename.c_str());
    return false;
  }

  return true;
}

bool MemoryCard::SaveToFile()
{
  if (m_filename.empty())
    return false;

  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(m_filename.c_str(), BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_TRUNCATE | BYTESTREAM_OPEN_WRITE |
                                               BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
  {
    Log_ErrorPrintf("Failed to open '%s' for writing.", m_filename.c_str());
    return false;
  }

  if (!stream->Write2(m_data.data(), SECTOR_SIZE * NUM_SECTORS) || !stream->Commit())
  {
    Log_ErrorPrintf("Failed to write sectors to '%s'", m_filename.c_str());
    stream->Discard();
    return false;
  }

  Log_InfoPrintf("Saved memory card to '%s'", m_filename.c_str());
  m_system->GetHostInterface()->AddOSDMessage(SmallString::FromFormat("Saved memory card to '%s'", m_filename.c_str()));
  return true;
}
